#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bk {

// Параметры записи видео, общие для всех кодеков.
struct VideoParams {
    int width = 0, height = 0;           // размер кадра видео
    uint32_t fpsNum = 0, fpsDen = 1;     // частота кадров дробью (у БК 3125/64)
    int sarNum = 1, sarDen = 1;          // форма точки: 512x256 БК — 2:3 (кадр 4:3 на экране)
    int audioRate = 0;                   // звук: частота отсчётов, 16 бит; 0 — без звука
    int audioChannels = 2;               // каналов звука (стерео: L, R чередуются)
};

// Контейнер вместе с кодеками: принимает кадры ARGB (0xAARRGGBB) размера видео и звук,
// набранный за этот кадр.
class VideoSink {
public:
    virtual ~VideoSink() = default;
    virtual bool open(const std::string& path, const VideoParams& p, std::string* err) = 0;
    // audio — frames кадров звука (отсчёты каналов чередуются); forceKey — начать с
    // опорного кадра. false — ошибка (текст — error()).
    virtual bool write(const uint32_t* argb, const int16_t* audio, size_t frames, bool forceKey) = 0;
    virtual bool close() = 0;
    virtual uint64_t bytes() const = 0;
    virtual std::string error() const = 0;
    // У файла предел размера, и запись продолжается в следующем (AVI 1.0 — ~1 Гбайт).
    virtual bool splittable() const { return false; }
    // Кадр только с квадратной точкой (проигрыватели могут не учесть её форму).
    virtual bool squarePixelsOnly() const { return false; }
};

struct VideoCodecInfo {
    std::string name;                    // для настроек и --video-codec
    std::string title;                   // для меню
    std::string ext;                     // расширение файла, с точкой
    std::string format;                  // коротко — для строки состояния
};

// Кодеки, доступные в этой сборке; первый — по умолчанию. «h264» (MP4) есть, только
// если эмулятор собран с FFmpeg и в нём нашлись кодировщики libx264 и aac.
const std::vector<VideoCodecInfo>& videoCodecList();
const VideoCodecInfo* findVideoCodec(const std::string& name);
// Кодек по расширению файла (.avi -> zmbv, .mp4 -> h264) — даже если в этой сборке его
// нет (проверка — findVideoCodec); nullptr — расширение не опознано.
const VideoCodecInfo* videoCodecForPath(const std::string& path);
// Почему кодека нет в этой сборке (для сообщения об ошибке); пусто — он есть.
std::string videoCodecUnavailableReason(const std::string& name);
std::unique_ptr<VideoSink> makeVideoSink(const std::string& codec);

} // namespace bk
