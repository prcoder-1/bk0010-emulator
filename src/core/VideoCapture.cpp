#include "VideoCapture.h"
#include "VideoSink.h"
#include <cstdio>
#include <numeric>

namespace bk {

VideoCapture::VideoCapture() = default;
VideoCapture::~VideoCapture() { stop(); }

std::string VideoCapture::partName(int part) const {
    if (part <= 1) return path_;
    const size_t dot = path_.find_last_of('.');
    const size_t slash = path_.find_last_of('/');
    const bool ext = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    char suf[16];
    std::snprintf(suf, sizeof(suf), "_%03d", part);
    return ext ? path_.substr(0, dot) + suf + path_.substr(dot) : path_ + suf;
}

bool VideoCapture::openPart(std::string* err) {
    if (!sink_) sink_ = makeVideoSink(opt_.codec);
    if (!sink_) { if (err) *err = videoCodecUnavailableReason(opt_.codec); return false; }
    const std::string name = partName(part_);
    VideoParams p;
    p.width = outW_; p.height = outH_;
    p.fpsNum = fpsNum_; p.fpsDen = fpsDen_;
    // 512x256 показывается 4:3: точка БК 2:3; 1024x768 уже 4:3 сам.
    if (!scale_ && srcW_ * 2 <= 1024) { p.sarNum = 2; p.sarDen = 3; }
    p.audioRate = opt_.audioRate;
    if (!sink_->open(name, p, err)) { sink_.reset(); return false; }
    std::lock_guard<std::mutex> l(m_);
    file_ = name;
    return true;
}

bool VideoCapture::start(const std::string& path, const Options& opt, int srcW, int srcH,
                         uint32_t fpsNum, uint32_t fpsDen, std::string* err) {
    stop();
    opt_ = opt;
    path_ = path;
    sink_ = makeVideoSink(opt_.codec);
    if (!sink_) { if (err) *err = videoCodecUnavailableReason(opt_.codec); return false; }
    // MP4 для телефонов и Telegram — только квадратная точка: форму точки там могут
    // не учесть, и 512x256 показалось бы сплюснутым.
    if (sink_->squarePixelsOnly()) opt_.aspect43 = true;
    srcW_ = srcW; srcH_ = srcH;
    // Растягивается только буфер БК (512x256); кадр VGA AZБК (1024x768) уже 4:3.
    scale_ = opt_.aspect43 && srcW * 2 <= 1024;
    outW_ = scale_ ? srcW * 2 : srcW;
    outH_ = scale_ ? srcH * 3 : srcH;
    const uint32_t g = std::gcd(fpsNum, fpsDen);
    fpsNum_ = fpsNum / (g ? g : 1); fpsDen_ = fpsDen / (g ? g : 1);
    part_ = 1;
    queued_ = 0;
    error_.clear();
    if (!openPart(err)) return false;
    stopping_ = false;
    active_ = true;
    thread_ = std::thread(&VideoCapture::worker, this);
    return true;
}

void VideoCapture::toVga(const uint32_t* src, uint32_t* dst) {
    for (int y = 0; y < 256; ++y) {
        uint32_t* d = dst + static_cast<size_t>(y) * 3 * 1024;
        for (int x = 0; x < 512; ++x) d[2 * x] = d[2 * x + 1] = src[y * 512 + x];
        std::copy(d, d + 1024, d + 1024);
        std::copy(d, d + 1024, d + 2048);
    }
}

void VideoCapture::addFrame(const uint32_t* argb, const int16_t* audio, size_t frames) {
    if (!active_) return;
    std::unique_lock<std::mutex> l(m_);
    // Очередь ограничена: если кодировщик не успевает, эмуляция подождёт — кадры
    // не теряются, иначе сбилась бы частота.
    space_.wait(l, [&] { return queue_.size() < 16; });
    Item it;
    if (!spare_.empty()) { it = std::move(spare_.back()); spare_.pop_back(); }
    it.frame.assign(argb, argb + static_cast<size_t>(srcW_) * srcH_);
    if (opt_.audioRate > 0 && audio) it.audio.assign(audio, audio + 2 * frames);
    else it.audio.clear();
    queue_.push_back(std::move(it));
    ++queued_;
    cv_.notify_one();
}

void VideoCapture::worker() {
    std::vector<uint32_t> scaled;
    bool forceKey = true;
    for (;;) {
        Item item;
        {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait(l, [&] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) break;               // stopping_ и всё записано
            item = std::move(queue_.front());
            queue_.pop_front();
            space_.notify_one();
        }
        const std::vector<uint32_t>& src = item.frame;
        const uint32_t* frame = src.data();
        if (scale_) {
            scaled.resize(static_cast<size_t>(outW_) * outH_);
            for (int y = 0; y < srcH_; ++y) {
                const uint32_t* s = src.data() + static_cast<size_t>(y) * srcW_;
                uint32_t* d = scaled.data() + static_cast<size_t>(y) * 3 * outW_;
                for (int x = 0; x < srcW_; ++x) d[2 * x] = d[2 * x + 1] = s[x];
                std::copy(d, d + outW_, d + outW_);
                std::copy(d, d + outW_, d + 2 * outW_);
            }
            frame = scaled.data();
        }
        bool ok = sink_ != nullptr;
        if (ok && sink_->splittable() && sink_->bytes() >= opt_.splitBytes) {   // следующий файл — с опорного кадра
            ok = sink_->close();
            sink_.reset();
            ++part_;
            std::string err;
            ok = ok && openPart(&err);
            forceKey = true;
            if (!ok) { std::lock_guard<std::mutex> l(m_); error_ = err.empty() ? "ошибка записи" : err; }
        }
        if (ok) ok = sink_->write(frame, item.audio.data(), item.audio.size() / 2, forceKey);
        forceKey = false;
        {
            std::lock_guard<std::mutex> l(m_);
            if (!ok && error_.empty()) error_ = (sink_ && !sink_->error().empty() ? sink_->error() : "ошибка записи") + " — " + file_;
            if (spare_.size() < 4) spare_.push_back(std::move(item));
        }
        if (!ok) {                                       // дальше писать некуда — сливаем очередь
            std::lock_guard<std::mutex> l(m_);
            queue_.clear();
            space_.notify_all();
            if (sink_) { sink_->close(); sink_.reset(); }
        }
    }
}

bool VideoCapture::stop() {
    if (!active_) return error_.empty();
    {
        std::lock_guard<std::mutex> l(m_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    active_ = false;
    bool ok = error_.empty();
    if (sink_) {
        if (!sink_->close()) { ok = false; if (error_.empty()) error_ = sink_->error() + " — " + file_; }
        sink_.reset();
    }
    if (!ok && error_.empty()) error_ = "ошибка записи " + file_;
    return ok;
}

std::string VideoCapture::error() const {
    std::lock_guard<std::mutex> l(m_);
    return error_;
}

std::string VideoCapture::currentFile() const {
    std::lock_guard<std::mutex> l(m_);
    return file_;
}

} // namespace bk
