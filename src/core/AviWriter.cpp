#include "AviWriter.h"
#include <sys/types.h>

namespace bk {

namespace {
void le32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
void le16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }
}

bool AviWriter::put(const void* p, size_t n) {
    if (!ok_ || std::fwrite(p, 1, n, f_) != n) { ok_ = false; return false; }
    pos_ += n;
    return true;
}

bool AviWriter::patch32(uint64_t at, uint32_t v) {
    const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
    return fseeko(f_, static_cast<off_t>(at), SEEK_SET) == 0 && std::fwrite(b, 1, 4, f_) == 4;
}

bool AviWriter::open(const std::string& path, int width, int height, uint32_t codec, int bitCount,
                     uint32_t rate, uint32_t scale, int aspectX, int aspectY, std::string* err) {
    close();
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) { if (err) *err = "файл не открывается на запись"; return false; }
    pos_ = 0; index_.clear(); maxChunk_ = 0; ok_ = true;

    const bool vprp = aspectX > 0 && aspectY > 0;
    std::vector<uint8_t> h;
    auto tag = [&](const char* s) { for (int i = 0; i < 4; ++i) h.push_back(uint8_t(s[i])); };
    std::vector<size_t> fixups;                     // поля размеров LIST — дописать ниже
    auto list = [&](const char* type) { tag("LIST"); fixups.push_back(h.size()); le32(h, 0); tag(type); };
    auto endList = [&]() { const size_t at = fixups.back(); fixups.pop_back();
                           const uint32_t n = uint32_t(h.size() - at - 4);
                           for (int i = 0; i < 4; ++i) h[at + i] = uint8_t(n >> (8 * i)); };

    tag("RIFF"); le32(h, 0); tag("AVI ");
    list("hdrl");
    tag("avih"); le32(h, 56);
    le32(h, uint32_t((1000000ULL * scale + rate / 2) / rate));   // мкс на кадр
    le32(h, 0);                       // dwMaxBytesPerSec
    le32(h, 0);                       // dwPaddingGranularity
    le32(h, 0x10);                    // AVIF_HASINDEX
    avihFrames_ = h.size(); le32(h, 0);
    le32(h, 0);                       // dwInitialFrames
    le32(h, 1);                       // dwStreams
    avihBuffer_ = h.size(); le32(h, 0);
    le32(h, uint32_t(width)); le32(h, uint32_t(height));
    for (int i = 0; i < 4; ++i) le32(h, 0);
    list("strl");
    tag("strh"); le32(h, 56);
    tag("vids"); le32(h, codec);
    le32(h, 0); le16(h, 0); le16(h, 0); le32(h, 0);   // flags, priority, language, initial frames
    le32(h, scale); le32(h, rate);
    le32(h, 0);                                       // dwStart
    strhLength_ = h.size(); le32(h, 0);
    strhBuffer_ = h.size(); le32(h, 0);
    le32(h, 0xFFFFFFFFu);                             // dwQuality — по умолчанию
    le32(h, 0);                                       // dwSampleSize
    le16(h, 0); le16(h, 0); le16(h, uint16_t(width)); le16(h, uint16_t(height));
    tag("strf"); le32(h, 40);                         // BITMAPINFOHEADER
    le32(h, 40); le32(h, uint32_t(width)); le32(h, uint32_t(height));
    le16(h, 1); le16(h, uint16_t(bitCount));
    le32(h, codec); le32(h, uint32_t(width * height * 4));
    for (int i = 0; i < 4; ++i) le32(h, 0);
    if (vprp) {
        // VideoPropHeader OpenDML: соотношение сторон кадра — старшее слово X, младшее Y.
        tag("vprp"); le32(h, 68);
        le32(h, 0); le32(h, 0);                       // формат, стандарт — не заданы
        le32(h, uint32_t((rate + scale / 2) / scale));
        le32(h, uint32_t(width)); le32(h, uint32_t(height));
        le32(h, uint32_t(aspectX) << 16 | uint32_t(aspectY));
        le32(h, uint32_t(width)); le32(h, uint32_t(height));
        le32(h, 1);                                   // полей в кадре
        le32(h, uint32_t(height)); le32(h, uint32_t(width));
        le32(h, uint32_t(height)); le32(h, uint32_t(width));
        for (int i = 0; i < 4; ++i) le32(h, 0);
    }
    endList();                                        // strl
    endList();                                        // hdrl
    tag("LIST"); moviList_ = h.size(); le32(h, 0); tag("movi");
    if (!put(h.data(), h.size())) { if (err) *err = "ошибка записи"; close(); return false; }
    return true;
}

bool AviWriter::writeFrame(const uint8_t* data, size_t size, bool keyframe) {
    if (!f_ || !ok_) return false;
    const uint32_t n = uint32_t(size);
    std::vector<uint8_t> hdr;
    hdr.push_back('0'); hdr.push_back('0'); hdr.push_back('d'); hdr.push_back('c');
    le32(hdr, n);
    // Смещение в idx1 — от слова «movi».
    index_.push_back({uint32_t(pos_ - (moviList_ + 4)), n, keyframe});
    if (n > maxChunk_) maxChunk_ = n;
    static const uint8_t pad = 0;
    return put(hdr.data(), hdr.size()) && put(data, size) && ((n & 1) == 0 || put(&pad, 1));
}

bool AviWriter::close() {
    if (!f_) return true;
    bool good = ok_;
    if (good) {
        const uint64_t moviEnd = pos_;
        std::vector<uint8_t> idx;
        idx.reserve(8 + index_.size() * 16);
        idx.push_back('i'); idx.push_back('d'); idx.push_back('x'); idx.push_back('1');
        le32(idx, uint32_t(index_.size() * 16));
        for (const Entry& e : index_) {
            idx.push_back('0'); idx.push_back('0'); idx.push_back('d'); idx.push_back('c');
            le32(idx, e.key ? 0x10 : 0);              // AVIIF_KEYFRAME
            le32(idx, e.offset); le32(idx, e.size);
        }
        good = put(idx.data(), idx.size());
        const uint32_t frames = uint32_t(index_.size());
        good = good && patch32(4, uint32_t(pos_ - 8))
                    && patch32(moviList_, uint32_t(moviEnd - moviList_ - 4))
                    && patch32(avihFrames_, frames) && patch32(strhLength_, frames)
                    && patch32(avihBuffer_, maxChunk_ + 8) && patch32(strhBuffer_, maxChunk_ + 8);
    }
    good = (std::fclose(f_) == 0) && good;
    f_ = nullptr;
    return good;
}

} // namespace bk
