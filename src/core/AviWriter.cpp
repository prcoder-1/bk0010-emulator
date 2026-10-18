#include "AviWriter.h"
#include <sys/types.h>
#include <algorithm>

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
                     uint32_t rate, uint32_t scale, int aspectX, int aspectY, std::string* err,
                     int audioRate, int audioChannels) {
    close();
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) { if (err) *err = "файл не открывается на запись"; return false; }
    pos_ = 0; index_.clear(); maxChunk_ = 0; ok_ = true;
    audioCh_ = audioRate > 0 ? std::max(1, audioChannels) : 0;
    audioBlocks_ = 0; frames_ = 0; maxAudio_ = 0;

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
    le32(h, audioCh_ ? 0x110 : 0x10); // AVIF_HASINDEX (| AVIF_ISINTERLEAVED со звуком)
    avihFrames_ = h.size(); le32(h, 0);
    le32(h, 0);                       // dwInitialFrames
    le32(h, audioCh_ ? 2 : 1);        // dwStreams
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
    endList();                                        // strl видео
    if (audioCh_) {
        const uint16_t align = uint16_t(2 * audioCh_);
        list("strl");
        tag("strh"); le32(h, 56);
        tag("auds"); le32(h, 0);
        le32(h, 0); le16(h, 0); le16(h, 0); le32(h, 0);
        le32(h, align); le32(h, uint32_t(audioRate) * align);   // блоков в секунду = частота
        le32(h, 0);
        audLength_ = h.size(); le32(h, 0);
        audBuffer_ = h.size(); le32(h, 0);
        le32(h, 0xFFFFFFFFu);
        le32(h, align);                                   // dwSampleSize — блок
        le16(h, 0); le16(h, 0); le16(h, 0); le16(h, 0);
        tag("strf"); le32(h, 16);                         // PCMWAVEFORMAT
        le16(h, 1);                                       // WAVE_FORMAT_PCM
        le16(h, uint16_t(audioCh_));
        le32(h, uint32_t(audioRate));
        le32(h, uint32_t(audioRate) * align);
        le16(h, align);
        le16(h, 16);
        endList();                                    // strl звука
    }
    endList();                                        // hdrl
    tag("LIST"); moviList_ = h.size(); le32(h, 0); tag("movi");
    if (!put(h.data(), h.size())) { if (err) *err = "ошибка записи"; close(); return false; }
    return true;
}

bool AviWriter::chunk(uint32_t id, const void* data, size_t size, bool key) {
    if (!f_ || !ok_) return false;
    const uint32_t n = uint32_t(size);
    std::vector<uint8_t> hdr;
    le32(hdr, id);
    le32(hdr, n);
    // Смещение в idx1 — от слова «movi».
    index_.push_back({id, uint32_t(pos_ - (moviList_ + 4)), n, key});
    static const uint8_t pad = 0;
    return put(hdr.data(), hdr.size()) && put(data, size) && ((n & 1) == 0 || put(&pad, 1));
}

bool AviWriter::writeFrame(const uint8_t* data, size_t size, bool keyframe) {
    if (size > maxChunk_) maxChunk_ = uint32_t(size);
    ++frames_;
    return chunk(fourcc('0', '0', 'd', 'c'), data, size, keyframe);
}

bool AviWriter::writeAudio(const int16_t* samples, size_t count) {
    if (!audioCh_ || !count) return true;
    const size_t bytes = count * 2 * size_t(audioCh_);
    if (bytes > maxAudio_) maxAudio_ = uint32_t(bytes);
    audioBlocks_ += count;
    // PCM в AVI — little-endian, как и int16 в памяти x86/ARM.
    return chunk(fourcc('0', '1', 'w', 'b'), samples, bytes, true);
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
            le32(idx, e.id);
            le32(idx, e.key ? 0x10 : 0);              // AVIIF_KEYFRAME
            le32(idx, e.offset); le32(idx, e.size);
        }
        good = put(idx.data(), idx.size());
        const uint32_t frames = frames_;
        good = good && patch32(4, uint32_t(pos_ - 8))
                    && patch32(moviList_, uint32_t(moviEnd - moviList_ - 4))
                    && patch32(avihFrames_, frames) && patch32(strhLength_, frames)
                    && patch32(avihBuffer_, std::max(maxChunk_, maxAudio_) + 8)
                    && patch32(strhBuffer_, maxChunk_ + 8);
        if (audioCh_)
            good = good && patch32(audLength_, uint32_t(audioBlocks_)) && patch32(audBuffer_, maxAudio_);
    }
    good = (std::fclose(f_) == 0) && good;
    f_ = nullptr;
    return good;
}

} // namespace bk
