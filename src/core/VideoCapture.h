#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bk {

class AviWriter;
class VideoEncoder;

// Запись экрана эмулятора в видеофайл AVI. Кадр видео — один кадр БК, поэтому
// частота точно та же, что у машины: rate/scale = такт ЦП / такты кадра
// (3 000 000 / 61 440 = 4 000 000 / 81 920 = 48,828125 Гц = 3125/64). Время в файле —
// эмулируемое: пауза и отладчик в видео не попадают, торможение хоста — тоже.
//
// Звук (если включён) — PCM 16 бит моно с частотой отсчётов Speaker: отсчёты, набранные
// за кадр БК, пишутся сразу после картинки этого кадра, так что дорожки идут вместе.
//
// Кадры копируются в очередь и кодируются в отдельном потоке, чтобы опорный кадр
// (deflate всего экрана) не дёргал эмуляцию. Файл больше ~1 Гбайт (предел AVI 1.0 без
// OpenDML) продолжается в следующем: имя_002.avi, имя_003.avi…
class VideoCapture {
public:
    struct Options {
        std::string codec = "zmbv";
        // true — кадр 1024x768: точка БК (512x256) — блоком 2x3, как пиксель 3:4 в окне
        // GUI; false — как буфер, 512x256, с пометкой в AVI «показывать 4:3».
        bool aspect43 = true;
        uint64_t splitBytes = 1000000000ULL;
        int audioRate = 0;          // частота отсчётов звука; 0 — без звука
    };
    VideoCapture();
    ~VideoCapture();

    // srcW x srcH — размер буфера экрана (512x256), fpsNum/fpsDen — частота кадров.
    bool start(const std::string& path, const Options& opt, int srcW, int srcH,
               uint32_t fpsNum, uint32_t fpsDen, std::string* err = nullptr);
    // Один кадр БК в ARGB и звук, набранный за этот кадр (count отсчётов, моно).
    void addFrame(const uint32_t* argb, const int16_t* audio = nullptr, size_t count = 0);
    bool hasAudio() const { return active_ && opt_.audioRate > 0; }
    // Остановить: дописать очередь, закрыть файл. false — была ошибка записи.
    bool stop();
    bool active() const { return active_; }
    uint64_t frames() const { return queued_; }
    double seconds() const { return fpsNum_ ? double(queued_) * fpsDen_ / fpsNum_ : 0; }
    std::string error() const;                 // ошибка из потока записи (пусто — нет)
    std::string currentFile() const;
    int outWidth() const { return outW_; }
    int outHeight() const { return outH_; }

private:
    void worker();
    bool openPart(std::string* err);
    std::string partName(int part) const;

    Options opt_;
    std::string path_;
    int srcW_ = 0, srcH_ = 0, outW_ = 0, outH_ = 0;
    uint32_t fpsNum_ = 0, fpsDen_ = 1;
    bool active_ = false;
    uint64_t queued_ = 0;
    int part_ = 1;

    std::unique_ptr<AviWriter> avi_;
    std::unique_ptr<VideoEncoder> enc_;
    std::thread thread_;
    mutable std::mutex m_;
    std::condition_variable cv_, space_;
    struct Item { std::vector<uint32_t> frame; std::vector<int16_t> audio; };
    std::deque<Item> queue_;
    std::vector<Item> spare_;
    bool stopping_ = false;
    std::string error_, file_;
};

} // namespace bk
