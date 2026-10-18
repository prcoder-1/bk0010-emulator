#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bk {

// Кодировщик видео для записи экрана в AVI. Кадр приходит в ARGB (0xAARRGGBB), как
// его рисует Screen, уже в размере видео.
class VideoEncoder {
public:
    virtual ~VideoEncoder() = default;
    virtual uint32_t fourcc() const = 0;       // biCompression / fccHandler
    virtual int bitCount() const = 0;          // biBitCount в заголовке AVI
    // keyframe — потребовать опорный кадр (начало файла); out — данные кадра AVI,
    // isKey — получился ли опорный.
    virtual bool encode(const uint32_t* argb, bool keyframe, std::vector<uint8_t>& out, bool& isKey) = 0;
};

// ZMBV — Zip Motion Blocks Video, кодек записи экрана DOSBox (FourCC «ZMBV»): кадр
// режется на блоки 16x16, для каждого ищется вектор сдвига относительно прошлого
// кадра, и записывается XOR с ним; всё вместе сжимается одним потоком deflate,
// который сбрасывается только на опорных кадрах. Без потерь. Читают ffmpeg/mpv/VLC и
// кодек VfW из DOSBox.
//
// Цвета БК — несколько штук на кадр, поэтому формат 8 бит на точку с палитрой: палитра
// набирается по мере появления цветов, опорный кадр несёт её целиком, промежуточный —
// изменение (XOR) с флагом DeltaPalette. Опорный кадр — раз в keyInterval кадров.
class ZmbvEncoder : public VideoEncoder {
public:
    ZmbvEncoder(int width, int height, int level = 4, int keyInterval = 300);
    ~ZmbvEncoder() override;
    uint32_t fourcc() const override;
    int bitCount() const override { return 24; }   // так пишет DOSBox; декодер смотрит на заголовок кадра
    bool encode(const uint32_t* argb, bool keyframe, std::vector<uint8_t>& out, bool& isKey) override;

    // Формат кадра ZMBV (для тестов и декодера в cpu_tests).
    enum : uint8_t { KEYFRAME = 0x01, DELTA_PALETTE = 0x02, FMT_8BPP = 4, BLOCK = 16 };
    static constexpr int MAX_VECTOR = 16;

private:
    uint8_t colorIndex(uint32_t argb);
    int compareBlock(int bx, int by, int bw, int bh, int vx, int vy) const;
    int sampleBlock(int bx, int by, int bw, int bh, int vx, int vy) const;
    uint8_t* at(std::vector<uint8_t>& f, int x, int y) { return &f[(y + MAX_VECTOR) * pitch_ + x + MAX_VECTOR]; }
    const uint8_t* at(const std::vector<uint8_t>& f, int x, int y) const { return &f[(y + MAX_VECTOR) * pitch_ + x + MAX_VECTOR]; }

    struct Z;                                   // поток zlib
    std::unique_ptr<Z> z_;
    int w_, h_, pitch_, keyInterval_;
    uint64_t frame_ = 0;
    std::vector<uint8_t> cur_, prev_;           // индексы цветов с полем MAX_VECTOR вокруг
    std::vector<uint8_t> work_;
    uint8_t pal_[768] = {}, sentPal_[768] = {};
    std::vector<uint32_t> colors_;              // ARGB по индексу
    uint32_t lastColor_ = 0; uint8_t lastIndex_ = 0;
    struct Vec { int8_t x, y; };
    std::vector<Vec> vectors_;
};

} // namespace bk
