#pragma once
#include <cstdint>
#include <deque>
#include <vector>
#include <mutex>
#include "Ay8910.h"

namespace bk {

// Converts the BK-0010 1-bit piezo speaker output (bit 6 of port 0177716) into
// a stream of 16-bit mono PCM samples. The Board feeds it (speakerBit, cpuTicks)
// on every instruction; an audio backend drains samples via read().
class Speaker {
public:
    explicit Speaker(int sampleRate = 44100, int cpuFreq = 3000000)
        : sampleRate_(sampleRate), cpuFreq_(cpuFreq) {}

    void setEnabled(bool e) { enabled_ = e; }
    void setCpuFreq(int hz) { cpuFreq_ = hz; }   // 3 МГц у БК-0010, 4 МГц у БК-0011М
    // Звуковой сопроцессор, подмешиваемый в тот же моно-поток (nullptr — нет). Его
    // выход идёт мимо RC-цепи пищалки, со своим DC-блокером.
    void setAy(Ay8910* ay) { ay_ = ay; ayDc_ = 0.0; }
    // Второй чип платы на два AY (nullptr — нет); подключается на ходу, без щелчка.
    void setAy2(Ay8910* ay) { ay2_ = ay; }
    // Covox: уровень ЦАП (-1..1), который держится до следующей записи; в отсчёт идёт
    // среднее за его длительность. Со своим DC-блокером, как у AY.
    void setCovox(bool on) { covox_ = on; covoxLevel_ = 0.0; covoxAcc_ = 0.0; covoxDc_ = 0.0; }
    void setCovoxLevel(double v) { covoxLevel_ = v; }

    // Advance by `ticks` CPU ticks with the speaker driven to `level` (0..7).
    // Пьезодинамик БК слышит сумму ТРЁХ бит регистра 0177716 (маска 0144): бит 6 —
    // основной, биты 5 и 2 подмешиваются в тот же аналоговый узел через другие
    // сопротивления. Отсюда 8 уровней, а не 2, — на них построен ШИМ-звук ряда игр.
    // Относительные амплитуды взяты из GID BKemu (devemu/Speaker.cpp:116-126).
    void feed(int level, int ticks);

    // Drain up to `maxSamples` into out; returns the number produced.
    // Plain FIFO drain — missing samples (underrun) are filled with the last
    // level by the caller, and latency management (prime/trim) also lives there.
    size_t read(int16_t* out, size_t maxSamples);

    // Cap backlog: drop the oldest samples so at most `maxSamples` remain. A
    // safety valve against runaway latency (producer far ahead of the sink);
    // with rate-matched production it should almost never fire.
    void trimTo(size_t maxSamples);

    size_t available();
    void clear();

    // Отвод для записи видео: каждый отсчёт копируется ещё и сюда — независимо от
    // вывода на звуковую карту (выключенный вывод, обрезка задержки в read/trimTo),
    // поэтому в записи нет ни пропусков, ни лишнего. Включение очищает отвод.
    void setCapture(bool on);
    void takeCaptured(std::vector<int16_t>& out);   // забрать накопленное (out заменяется)
    int sampleRate() const { return sampleRate_; }

private:
    int sampleRate_, cpuFreq_;
    bool enabled_ = true;
    double acc_ = 0.0;          // накоплено от текущего выходного отсчёта (0..1)
    double filtered_ = 0.0;     // RC low-pass state (smooths square edges)
    double dc_ = 0.0;           // slow DC tracker for the high-pass (idle -> silence)
    int level_ = 0;             // текущий уровень динамика 0..7
    Ay8910* ay_ = nullptr;
    Ay8910* ay2_ = nullptr;
    double ayDc_ = 0.0;         // DC-трекер выхода AY (выход однополярный)
    bool covox_ = false;
    double covoxLevel_ = 0.0, covoxAcc_ = 0.0, covoxDc_ = 0.0;
    std::deque<int16_t> buf_;   // FIFO of generated samples (O(1) push/pop at both ends)
    bool capture_ = false;
    std::vector<int16_t> cap_;  // отвод для записи видео
    std::mutex mtx_;
};

} // namespace bk
