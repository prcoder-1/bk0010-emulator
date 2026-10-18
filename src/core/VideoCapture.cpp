#include "VideoCapture.h"
#include "AviWriter.h"
#include "Zmbv.h"
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
    enc_ = makeVideoEncoder(opt_.codec, outW_, outH_);
    if (!enc_) { if (err) *err = "неизвестный кодек " + opt_.codec; return false; }
    avi_ = std::make_unique<AviWriter>();
    const std::string name = partName(part_);
    // 512x256 показывается 4:3 (пиксель БК не квадратный); 1024x768 уже 4:3 сам.
    if (!avi_->open(name, outW_, outH_, enc_->fourcc(), enc_->bitCount(), fpsNum_, fpsDen_,
                    opt_.aspect43 ? 0 : 4, opt_.aspect43 ? 0 : 3, err, opt_.audioRate, 1))
        return false;
    std::lock_guard<std::mutex> l(m_);
    file_ = name;
    return true;
}

bool VideoCapture::start(const std::string& path, const Options& opt, int srcW, int srcH,
                         uint32_t fpsNum, uint32_t fpsDen, std::string* err) {
    stop();
    opt_ = opt;
    path_ = path;
    srcW_ = srcW; srcH_ = srcH;
    outW_ = opt.aspect43 ? srcW * 2 : srcW;
    outH_ = opt.aspect43 ? srcH * 3 : srcH;
    const uint32_t g = std::gcd(fpsNum, fpsDen);
    fpsNum_ = fpsNum / (g ? g : 1); fpsDen_ = fpsDen / (g ? g : 1);
    part_ = 1;
    queued_ = 0;
    error_.clear();
    if (!openPart(err)) { avi_.reset(); enc_.reset(); return false; }
    stopping_ = false;
    active_ = true;
    thread_ = std::thread(&VideoCapture::worker, this);
    return true;
}

void VideoCapture::addFrame(const uint32_t* argb, const int16_t* audio, size_t count) {
    if (!active_) return;
    std::unique_lock<std::mutex> l(m_);
    // Очередь ограничена: если кодировщик не успевает, эмуляция подождёт — кадры
    // не теряются, иначе сбилась бы частота.
    space_.wait(l, [&] { return queue_.size() < 16; });
    Item it;
    if (!spare_.empty()) { it = std::move(spare_.back()); spare_.pop_back(); }
    it.frame.assign(argb, argb + static_cast<size_t>(srcW_) * srcH_);
    if (opt_.audioRate > 0 && audio) it.audio.assign(audio, audio + count);
    else it.audio.clear();
    queue_.push_back(std::move(it));
    ++queued_;
    cv_.notify_one();
}

void VideoCapture::worker() {
    std::vector<uint32_t> scaled;
    std::vector<uint8_t> packet;
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
        if (opt_.aspect43) {
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
        bool ok = avi_ && avi_->isOpen();
        if (ok && avi_->bytes() >= opt_.splitBytes) {    // следующий файл — с опорного кадра
            ok = avi_->close();
            ++part_;
            std::string err;
            ok = ok && openPart(&err);
            forceKey = true;
            if (!ok) { std::lock_guard<std::mutex> l(m_); error_ = err.empty() ? "ошибка записи" : err; }
        }
        bool key = false;
        if (ok) ok = enc_->encode(frame, forceKey, packet, key) && avi_->writeFrame(packet.data(), packet.size(), key)
                     && avi_->writeAudio(item.audio.data(), item.audio.size());
        forceKey = false;
        {
            std::lock_guard<std::mutex> l(m_);
            if (!ok && error_.empty()) error_ = "ошибка записи " + file_;
            if (spare_.size() < 4) spare_.push_back(std::move(item));
        }
        if (!ok) {                                       // дальше писать некуда — сливаем очередь
            std::lock_guard<std::mutex> l(m_);
            queue_.clear();
            space_.notify_all();
            if (avi_) avi_->close();
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
    if (avi_ && avi_->isOpen()) ok = avi_->close() && ok;
    if (!ok && error_.empty()) error_ = "ошибка записи " + file_;
    avi_.reset();
    enc_.reset();
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
