// Запись в MP4 через библиотеки FFmpeg: видео H.264 (libx264) и звук AAC — формат,
// который Telegram и телефоны показывают сразу, без скачивания и перекодирования.
// Собирается, только если найдены libavcodec/libavformat/libavutil/libswscale
// (BK_HAVE_FFMPEG).
#include "VideoSink.h"
#include <algorithm>
#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace bk {

namespace {
std::string averr(int e) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof(buf));
    return buf;
}

struct Encoders { const AVCodec* video = nullptr; const AVCodec* audio = nullptr; };

const Encoders& encoders() {
    static std::once_flag once;
    static Encoders e;
    std::call_once(once, [] {
        av_log_set_level(AV_LOG_ERROR);          // без сводок x264/aac в stderr
        e.video = avcodec_find_encoder_by_name("libx264");
        e.audio = avcodec_find_encoder_by_name("aac");
    });
    return e;
}

class Mp4Sink : public VideoSink {
public:
    ~Mp4Sink() override { release(); }

    bool open(const std::string& path, const VideoParams& p, std::string* err) override {
        auto fail = [&](const std::string& why) { if (err) *err = why; release(); return false; };
        const Encoders& enc = encoders();
        w_ = p.width; h_ = p.height;
        int e = avformat_alloc_output_context2(&fmt_, nullptr, "mp4", path.c_str());
        if (e < 0 || !fmt_) return fail("FFmpeg не создаёт MP4: " + averr(e));
        const bool global = (fmt_->oformat->flags & AVFMT_GLOBALHEADER) != 0;

        // --- видео: H.264 High, 4:2:0, один отсчёт времени — кадр БК ---
        vst_ = avformat_new_stream(fmt_, nullptr);
        vc_ = avcodec_alloc_context3(enc.video);
        if (!vst_ || !vc_) return fail("FFmpeg: нет памяти");
        vc_->width = w_;
        vc_->height = h_;
        vc_->pix_fmt = AV_PIX_FMT_YUV420P;
        vc_->time_base = AVRational{int(p.fpsDen), int(p.fpsNum)};
        vc_->framerate = AVRational{int(p.fpsNum), int(p.fpsDen)};
        vc_->sample_aspect_ratio = AVRational{p.sarNum, p.sarDen};
        vc_->gop_size = int(5 * p.fpsNum / p.fpsDen);   // опорный кадр ~ раз в 5 с — перемотка в Telegram
        // BT.709, «телевизионный» диапазон: так кадр 1024x768 понимают и проигрыватели,
        // не читающие пометку; матрицу ставим и в swscale ниже.
        vc_->colorspace = AVCOL_SPC_BT709;
        vc_->color_primaries = AVCOL_PRI_BT709;
        vc_->color_trc = AVCOL_TRC_BT709;
        vc_->color_range = AVCOL_RANGE_MPEG;
        if (global) vc_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        av_opt_set(vc_->priv_data, "preset", "medium", 0);
        av_opt_set(vc_->priv_data, "tune", "animation", 0);   // плоские цвета и чёткие края
        av_opt_set(vc_->priv_data, "profile", "high", 0);
        av_opt_set_double(vc_->priv_data, "crf", 20, 0);
        if ((e = avcodec_open2(vc_, enc.video, nullptr)) < 0) return fail("libx264 не открылся: " + averr(e));
        if ((e = avcodec_parameters_from_context(vst_->codecpar, vc_)) < 0) return fail(averr(e));
        vst_->time_base = vc_->time_base;
        vst_->avg_frame_rate = vc_->framerate;
        vst_->sample_aspect_ratio = vc_->sample_aspect_ratio;

        // --- звук: AAC-LC стерео ---
        if (p.audioRate > 0) {
            ast_ = avformat_new_stream(fmt_, nullptr);
            ac_ = avcodec_alloc_context3(enc.audio);
            if (!ast_ || !ac_) return fail("FFmpeg: нет памяти");
            ac_->sample_fmt = AV_SAMPLE_FMT_FLTP;
            ac_->sample_rate = p.audioRate;
            av_channel_layout_default(&ac_->ch_layout, 2);
            ac_->bit_rate = 160000;
            ac_->time_base = AVRational{1, p.audioRate};
            if (global) ac_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if ((e = avcodec_open2(ac_, enc.audio, nullptr)) < 0) return fail("AAC не открылся: " + averr(e));
            if ((e = avcodec_parameters_from_context(ast_->codecpar, ac_)) < 0) return fail(averr(e));
            ast_->time_base = ac_->time_base;
            aframe_ = av_frame_alloc();
            if (!aframe_) return fail("FFmpeg: нет памяти");
            aframe_->format = ac_->sample_fmt;
            aframe_->sample_rate = ac_->sample_rate;
            aframe_->nb_samples = ac_->frame_size > 0 ? ac_->frame_size : 1024;
            av_channel_layout_copy(&aframe_->ch_layout, &ac_->ch_layout);
            if ((e = av_frame_get_buffer(aframe_, 0)) < 0) return fail(averr(e));
            aframeSize_ = aframe_->nb_samples;
        }

        if ((e = avio_open(&fmt_->pb, path.c_str(), AVIO_FLAG_WRITE)) < 0)
            return fail("файл не открывается на запись: " + averr(e));
        // faststart: индекс (moov) — в начало файла, чтобы видео играло, не скачиваясь целиком.
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "movflags", "+faststart", 0);
        e = avformat_write_header(fmt_, &opts);
        av_dict_free(&opts);
        if (e < 0) return fail("MP4: " + averr(e));
        headerDone_ = true;

        sws_ = sws_getContext(w_, h_, AV_PIX_FMT_BGRA, w_, h_, AV_PIX_FMT_YUV420P,
                              SWS_AREA | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INP, nullptr, nullptr, nullptr);
        vframe_ = av_frame_alloc();
        pkt_ = av_packet_alloc();
        if (!sws_ || !vframe_ || !pkt_) return fail("FFmpeg: нет памяти");
        sws_setColorspaceDetails(sws_, sws_getCoefficients(SWS_CS_DEFAULT), 1,
                                 sws_getCoefficients(SWS_CS_ITU709), 0, 0, 1 << 16, 1 << 16);
        vframe_->format = AV_PIX_FMT_YUV420P;
        vframe_->width = w_;
        vframe_->height = h_;
        if ((e = av_frame_get_buffer(vframe_, 0)) < 0) return fail(averr(e));
        vpts_ = 0; apts_ = 0; bytes_ = 0; fifoL_.clear(); fifoR_.clear(); error_.clear();
        return true;
    }

    bool write(const uint32_t* argb, const int16_t* audio, size_t frames, bool forceKey) override {
        if (!vc_) return false;
        int e = av_frame_make_writable(vframe_);
        if (e < 0) return setError(averr(e));
        // 0xAARRGGBB в памяти (little-endian) — байты B, G, R, A.
        const uint8_t* src[1] = {reinterpret_cast<const uint8_t*>(argb)};
        const int stride[1] = {w_ * 4};
        sws_scale(sws_, src, stride, 0, h_, vframe_->data, vframe_->linesize);
        vframe_->pts = vpts_++;
        vframe_->pict_type = forceKey ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
        if ((e = avcodec_send_frame(vc_, vframe_)) < 0) return setError("libx264: " + averr(e));
        if (!drain(vc_, vst_)) return false;
        if (ac_ && frames) {
            for (size_t i = 0; i < frames; ++i) {
                fifoL_.push_back(audio[2 * i] / 32768.0f);
                fifoR_.push_back(audio[2 * i + 1] / 32768.0f);
            }
            while (fifoL_.size() >= size_t(aframeSize_))
                if (!sendAudio(aframeSize_)) return false;
        }
        return true;
    }

    bool close() override {
        bool ok = error_.empty();
        if (headerDone_) {
            if (ac_) {
                if (!fifoL_.empty()) ok = sendAudio(int(fifoL_.size())) && ok;   // последний, короткий кадр AAC
                if (avcodec_send_frame(ac_, nullptr) >= 0) ok = drain(ac_, ast_) && ok;
            }
            if (avcodec_send_frame(vc_, nullptr) >= 0) ok = drain(vc_, vst_) && ok;   // задержанные кадры x264
            const int e = av_write_trailer(fmt_);      // здесь же moov переносится в начало
            if (e < 0) ok = setError("MP4: " + averr(e));
        }
        release();
        return ok;
    }

    uint64_t bytes() const override { return bytes_; }
    std::string error() const override { return error_; }
    bool squarePixelsOnly() const override { return true; }

private:
    bool setError(const std::string& e) { if (error_.empty()) error_ = e; return false; }

    bool sendAudio(int n) {
        int e = av_frame_make_writable(aframe_);
        if (e < 0) return setError(averr(e));
        aframe_->nb_samples = n;
        // fltp — каналы раздельными плоскостями.
        std::copy(fifoL_.begin(), fifoL_.begin() + n, reinterpret_cast<float*>(aframe_->data[0]));
        std::copy(fifoR_.begin(), fifoR_.begin() + n, reinterpret_cast<float*>(aframe_->data[1]));
        fifoL_.erase(fifoL_.begin(), fifoL_.begin() + n);
        fifoR_.erase(fifoR_.begin(), fifoR_.begin() + n);
        aframe_->pts = apts_;
        apts_ += n;
        if ((e = avcodec_send_frame(ac_, aframe_)) < 0) return setError("AAC: " + averr(e));
        return drain(ac_, ast_);
    }

    bool drain(AVCodecContext* c, AVStream* st) {
        for (;;) {
            int e = avcodec_receive_packet(c, pkt_);
            if (e == AVERROR(EAGAIN) || e == AVERROR_EOF) return true;
            if (e < 0) return setError(averr(e));
            pkt_->stream_index = st->index;
            av_packet_rescale_ts(pkt_, c->time_base, st->time_base);
            bytes_ += static_cast<uint64_t>(pkt_->size);
            e = av_interleaved_write_frame(fmt_, pkt_);   // забирает пакет себе
            if (e < 0) return setError("ошибка записи: " + averr(e));
        }
    }

    void release() {
        if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
        if (vframe_) av_frame_free(&vframe_);
        if (aframe_) av_frame_free(&aframe_);
        if (pkt_) av_packet_free(&pkt_);
        if (vc_) avcodec_free_context(&vc_);
        if (ac_) avcodec_free_context(&ac_);
        if (fmt_) {
            if (fmt_->pb) avio_closep(&fmt_->pb);
            avformat_free_context(fmt_);
            fmt_ = nullptr;
        }
        vst_ = ast_ = nullptr;
        headerDone_ = false;
    }

    AVFormatContext* fmt_ = nullptr;
    AVCodecContext* vc_ = nullptr;
    AVCodecContext* ac_ = nullptr;
    AVStream* vst_ = nullptr;
    AVStream* ast_ = nullptr;
    SwsContext* sws_ = nullptr;
    AVFrame* vframe_ = nullptr;
    AVFrame* aframe_ = nullptr;
    AVPacket* pkt_ = nullptr;
    int w_ = 0, h_ = 0, aframeSize_ = 1024;
    int64_t vpts_ = 0, apts_ = 0;
    std::vector<float> fifoL_, fifoR_; // отсчёты звука, ещё не набравшие кадр AAC
    uint64_t bytes_ = 0;
    bool headerDone_ = false;
    std::string error_;
};
}

std::string mp4Unavailable() {
    const Encoders& e = encoders();
    if (!e.video) return "в FFmpeg нет кодировщика libx264";
    if (!e.audio) return "в FFmpeg нет кодировщика aac";
    return std::string();
}

std::unique_ptr<VideoSink> makeMp4Sink() { return std::make_unique<Mp4Sink>(); }

} // namespace bk
