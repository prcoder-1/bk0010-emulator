#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace bk {

// Файл AVI 1.0 с одним видеопотоком: RIFF «AVI », заголовки hdrl, кадры «00dc» в
// LIST «movi» и индекс idx1. Частота — точной дробью rate/scale (у БК 3125/64 =
// 48,828125 Гц). Размеры кадров и их число дописываются в заголовки при закрытии.
// aspectX:aspectY — если заданы, добавляется блок OpenDML «vprp» с соотношением
// сторон кадра на экране (так ffmpeg/mpv узнают, что 512x256 надо показывать 4:3).
class AviWriter {
public:
    ~AviWriter() { close(); }

    bool open(const std::string& path, int width, int height, uint32_t fourcc, int bitCount,
              uint32_t rate, uint32_t scale, int aspectX = 0, int aspectY = 0, std::string* err = nullptr);
    bool writeFrame(const uint8_t* data, size_t size, bool keyframe);
    bool close();
    bool isOpen() const { return f_ != nullptr; }
    uint64_t bytes() const { return pos_; }
    uint32_t frames() const { return static_cast<uint32_t>(index_.size()); }

    static constexpr uint32_t fourcc(char a, char b, char c, char d) {
        return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
    }

private:
    struct Entry { uint32_t offset, size; bool key; };
    bool put(const void* p, size_t n);
    bool patch32(uint64_t at, uint32_t v);

    std::FILE* f_ = nullptr;
    uint64_t pos_ = 0;
    std::vector<Entry> index_;
    uint32_t maxChunk_ = 0;
    uint64_t moviList_ = 0;     // смещение поля размера LIST «movi»
    uint64_t avihFrames_ = 0, avihBuffer_ = 0, strhLength_ = 0, strhBuffer_ = 0;
    bool ok_ = true;
};

} // namespace bk
