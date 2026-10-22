#pragma once
#include <cstdint>
#include <deque>
#include <vector>
#include <mutex>
#include "Ay8910.h"

namespace bk {

// Converts the BK-0010 1-bit piezo speaker output (bit 6 of port 0177716) into
// a stream of 16-bit STEREO PCM: кадр — пара отсчётов (левый, правый), в буферах они
// чередуются. Пищалка звучит в оба канала одинаково, AY — по схеме ABC (Ay8910),
// Covox — левый/правый ЦАП «Мультизвука». The Board feeds it (speakerBit, cpuTicks)
// on every instruction; an audio backend drains frames via read().
class Speaker {
public:
    explicit Speaker(int sampleRate = 44100, int cpuFreq = 3000000)
        : sampleRate_(sampleRate), cpuFreq_(cpuFreq) {}

    void setEnabled(bool e) { enabled_ = e; }
    void setCpuFreq(int hz) { cpuFreq_ = hz; }   // 3 МГц у БК-0010, 4 МГц у БК-0011М
    // Звуковой сопроцессор, подмешиваемый в тот же поток (nullptr — нет). Его
    // выход идёт мимо RC-цепи пищалки, со своим DC-блокером на канал.
    void setAy(Ay8910* ay) { ay_ = ay; ayDcL_ = ayDcR_ = 0.0; }
    // Второй чип платы на два AY (nullptr — нет); подключается на ходу, без щелчка.
    void setAy2(Ay8910* ay) { ay2_ = ay; }
    // Covox: уровни ЦАП левого и правого канала (-1..1), которые держатся до следующей
    // записи; в отсчёт идёт среднее за их длительность. Со своими DC-блокерами.
    void setCovox(bool on) {
        covox_ = on;
        covoxL_ = covoxR_ = covoxAccL_ = covoxAccR_ = covoxDcL_ = covoxDcR_ = 0.0;
    }
    void setCovoxLevel(double left, double right) { covoxL_ = left; covoxR_ = right; }
    void setCovoxLevel(double v) { setCovoxLevel(v, v); }
    // ЦАП платы AZБК (Covox 16 бит и DMA-звук): уровни -1..1 держатся до следующей смены,
    // как у Covox, со своими DC-блокерами.
    void setAzbk(bool on) { az_ = on; azL_ = azR_ = azAccL_ = azAccR_ = azDcL_ = azDcR_ = 0.0; }
    void setAzLevel(double left, double right) { azL_ = left; azR_ = right; }

    // Advance by `ticks` CPU ticks with the speaker driven to `level` (0..7).
    // Пьезодинамик БК слышит сумму ТРЁХ бит регистра 0177716 (маска 0144): бит 6 —
    // основной, биты 5 и 2 подмешиваются в тот же аналоговый узел через другие
    // сопротивления. Отсюда 8 уровней, а не 2, — на них построен ШИМ-звук ряда игр.
    // Относительные амплитуды взяты из GID BKemu (devemu/Speaker.cpp:116-126).
    void feed(int level, int ticks);

    // Забрать до maxFrames кадров (в out — 2·кадров отсчётов: L, R, L, R…); вернёт число
    // кадров. Plain FIFO drain — missing samples (underrun) are filled with the last
    // level by the caller, and latency management (prime/trim) also lives there.
    size_t read(int16_t* outLR, size_t maxFrames);

    // Cap backlog: drop the oldest frames so at most `maxFrames` remain. A
    // safety valve against runaway latency (producer far ahead of the sink);
    // with rate-matched production it should almost never fire.
    void trimTo(size_t maxFrames);

    size_t available();                 // кадров в FIFO
    void clear();

    // Отвод для записи видео: каждый отсчёт копируется ещё и сюда — независимо от
    // вывода на звуковую карту (выключенный вывод, обрезка задержки в read/trimTo),
    // поэтому в записи нет ни пропусков, ни лишнего. Включение очищает отвод.
    void setCapture(bool on);
    void takeCaptured(std::vector<int16_t>& out);   // забрать накопленное, L/R чередуются (out заменяется)
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
    double ayDcL_ = 0.0, ayDcR_ = 0.0;   // DC-трекеры выхода AY (выход однополярный)
    bool covox_ = false;
    double covoxL_ = 0.0, covoxR_ = 0.0, covoxAccL_ = 0.0, covoxAccR_ = 0.0, covoxDcL_ = 0.0, covoxDcR_ = 0.0;
    bool az_ = false;
    double azL_ = 0.0, azR_ = 0.0, azAccL_ = 0.0, azAccR_ = 0.0, azDcL_ = 0.0, azDcR_ = 0.0;
    std::deque<int16_t> buf_;   // FIFO кадров, L/R чередуются (O(1) push/pop at both ends)
    bool capture_ = false;
    std::vector<int16_t> cap_;  // отвод для записи видео
    std::mutex mtx_;
};

} // namespace bk
