#pragma once
#include <cstdint>
#include <vector>
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
    // Missing samples (underrun) are filled with the last level by the caller.
    size_t read(int16_t* out, size_t maxSamples);

    size_t available();
    void clear();

private:
    int sampleRate_, cpuFreq_;
    bool enabled_ = true;
    double acc_ = 0.0;          // fractional CPU-ticks-per-sample accumulator
    double filtered_ = 0.0;     // RC low-pass state (smooths square edges)
    double dc_ = 0.0;           // slow DC tracker for the high-pass (idle -> silence)
    int level_ = 0;             // current speaker bit
    std::vector<int16_t> buf_;  // FIFO of generated samples
    std::mutex mtx_;
};

} // namespace bk
