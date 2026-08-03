#pragma once
#include <cstdint>
#include <deque>
#include <mutex>

namespace bk {

// Converts the BK-0010 1-bit piezo speaker output (bit 6 of port 0177716) into
// a stream of 16-bit mono PCM samples. The Board feeds it (speakerBit, cpuTicks)
// on every instruction; an audio backend drains samples via read().
class Speaker {
public:
    explicit Speaker(int sampleRate = 44100, int cpuFreq = 3000000)
        : sampleRate_(sampleRate), cpuFreq_(cpuFreq) {}

    void setEnabled(bool e) { enabled_ = e; }

    // Advance by `ticks` CPU ticks with the speaker driven to `bit` (0/1).
    void feed(int bit, int ticks);

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

private:
    int sampleRate_, cpuFreq_;
    bool enabled_ = true;
    double acc_ = 0.0;          // fractional CPU-ticks-per-sample accumulator
    double filtered_ = 0.0;     // RC low-pass state (smooths square edges)
    double dc_ = 0.0;           // slow DC tracker for the high-pass (idle -> silence)
    int level_ = 0;             // current speaker bit
    std::deque<int16_t> buf_;   // FIFO of generated samples (O(1) push/pop at both ends)
    std::mutex mtx_;
};

} // namespace bk
