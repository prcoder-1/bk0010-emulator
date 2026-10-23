#include "McuFiber.h"

namespace bk {

McuFiber::~McuFiber() {
    if (!thread_.joinable()) return;
    abort();
    {
        std::lock_guard<std::mutex> l(m_);
        quit_ = true;
        fiberTurn_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void McuFiber::start(std::function<void()> job) {
    if (running()) abort();
    if (!thread_.joinable()) thread_ = std::thread([this] { loop(); });
    std::lock_guard<std::mutex> l(m_);
    job_ = std::move(job);
    abort_ = false;
    busy_ = true;
}

void McuFiber::resume() {
    if (!busy_) return;
    std::unique_lock<std::mutex> l(m_);
    fiberTurn_ = true;
    cv_.notify_all();
    cv_.wait(l, [this] { return !fiberTurn_; });
}

void McuFiber::abort() {
    if (!busy_) return;
    abort_ = true;
    resume();                                // задача увидит abort_ в yield() и размотается
}

void McuFiber::yield() {
    std::unique_lock<std::mutex> l(m_);
    fiberTurn_ = false;
    cv_.notify_all();
    cv_.wait(l, [this] { return fiberTurn_; });
    if (abort_ || quit_) throw Abort{};
}

void McuFiber::loop() {
    std::unique_lock<std::mutex> l(m_);
    for (;;) {
        cv_.wait(l, [this] { return fiberTurn_; });
        if (quit_) { fiberTurn_ = false; cv_.notify_all(); return; }
        std::function<void()> job = std::move(job_);
        job_ = nullptr;
        if (job && !abort_) {
            l.unlock();
            try { job(); } catch (const Abort&) {}
            l.lock();
        }
        busy_ = false;
        fiberTurn_ = false;
        cv_.notify_all();
    }
}

} // namespace bk
