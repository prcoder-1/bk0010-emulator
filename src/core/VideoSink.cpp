#include "VideoSink.h"
#include "AviWriter.h"
#include "Zmbv.h"
#include <algorithm>
#include <cctype>

namespace bk {

#ifdef BK_HAVE_FFMPEG
// FfmpegVideo.cpp
std::unique_ptr<VideoSink> makeMp4Sink();
std::string mp4Unavailable();
#endif

namespace {
// AVI 1.0: видео ZMBV и звук PCM (стерео).
class AviSink : public VideoSink {
public:
    bool open(const std::string& path, const VideoParams& p, std::string* err) override {
        enc_ = std::make_unique<ZmbvEncoder>(p.width, p.height);
        // Точка не квадратная — пометка OpenDML «vprp» с формой кадра на экране.
        int ax = 0, ay = 0;
        if (p.sarNum != p.sarDen) {
            ax = p.width * p.sarNum; ay = p.height * p.sarDen;
            int x = ax, y = ay;
            while (y) { const int t = x % y; x = y; y = t; }
            if (x > 1) { ax /= x; ay /= x; }
        }
        error_.clear();
        return avi_.open(path, p.width, p.height, enc_->fourcc(), enc_->bitCount(), p.fpsNum, p.fpsDen,
                         ax, ay, err, p.audioRate, p.audioChannels);
    }
    bool write(const uint32_t* argb, const int16_t* audio, size_t frames, bool forceKey) override {
        bool key = false;
        if (!enc_->encode(argb, forceKey, packet_, key)) return fail("ошибка кодирования ZMBV");
        if (!avi_.writeFrame(packet_.data(), packet_.size(), key) || !avi_.writeAudio(audio, frames))
            return fail("ошибка записи");
        return true;
    }
    bool close() override {
        if (!avi_.close()) fail("ошибка записи");
        return error_.empty();
    }
    uint64_t bytes() const override { return avi_.bytes(); }
    std::string error() const override { return error_; }
    bool splittable() const override { return true; }

private:
    bool fail(const char* e) { if (error_.empty()) error_ = e; return false; }
    AviWriter avi_;
    std::unique_ptr<ZmbvEncoder> enc_;
    std::vector<uint8_t> packet_;
    std::string error_;
};
}

namespace {
// Все кодеки, которые эмулятор знает; доступны ли они в этой сборке — videoCodecList().
const std::vector<VideoCodecInfo>& knownCodecs() {
    static const std::vector<VideoCodecInfo> all = {
        {"zmbv", "ZMBV + PCM в AVI — без потерь (кодек DOSBox)", ".avi", "ZMBV + PCM"},
        {"h264", "H.264 + AAC в MP4 — для Telegram и телефонов", ".mp4", "H.264 + AAC"},
    };
    return all;
}
}

const std::vector<VideoCodecInfo>& videoCodecList() {
    static const std::vector<VideoCodecInfo> list = [] {
        std::vector<VideoCodecInfo> l;
        for (const auto& c : knownCodecs())
            if (videoCodecUnavailableReason(c.name).empty()) l.push_back(c);
        return l;
    }();
    return list;
}

const VideoCodecInfo* findVideoCodec(const std::string& name) {
    if (!videoCodecUnavailableReason(name).empty()) return nullptr;
    for (const auto& c : knownCodecs())
        if (c.name == name) return &c;
    return nullptr;
}

const VideoCodecInfo* videoCodecForPath(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return nullptr;
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const auto& c : knownCodecs())
        if (c.ext == ext) return &c;
    return nullptr;
}

std::string videoCodecUnavailableReason(const std::string& name) {
    if (name == "zmbv") return std::string();
    if (name == "h264") {
#ifdef BK_HAVE_FFMPEG
        return mp4Unavailable();
#else
        return "эмулятор собран без FFmpeg (libavcodec/libavformat/libswscale)";
#endif
    }
    return "неизвестный кодек " + name;
}

std::unique_ptr<VideoSink> makeVideoSink(const std::string& codec) {
    if (codec == "zmbv") return std::make_unique<AviSink>();
#ifdef BK_HAVE_FFMPEG
    if (codec == "h264" && mp4Unavailable().empty()) return makeMp4Sink();
#endif
    return nullptr;
}

} // namespace bk
