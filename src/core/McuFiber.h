#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace bk {

// Сопрограмма для кода микроконтроллера AZБК, написанного блокирующими циклами (как в
// прошивке: «крутить net_poll, пока не придёт ответ»). Работает в своём потоке, но строго
// по очереди с эмуляцией: resume() отдаёт ход и ждёт, пока задача не уступит (yield) или
// не закончится. Одновременно они никогда не работают, так что результат детерминирован.
class McuFiber {
public:
    struct Abort {};                         // бросается из yield(), когда задачу снимают

    McuFiber() = default;
    ~McuFiber();
    McuFiber(const McuFiber&) = delete;
    McuFiber& operator=(const McuFiber&) = delete;

    void start(std::function<void()> job);   // задача начнёт работать с первого resume()
    void resume();                           // дать поработать до yield() или до конца
    void abort();                            // снять задачу (yield() бросит Abort)
    bool running() const { return job_ != nullptr || busy_; }
    bool inFiber() const { return std::this_thread::get_id() == thread_.get_id(); }
    void yield();                            // из задачи: вернуть ход эмуляции

private:
    void loop();
    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_;
    std::function<void()> job_;
    bool fiberTurn_ = false;                 // чей ход
    bool busy_ = false;                      // задача начата и не закончена
    bool abort_ = false, quit_ = false;
};

} // namespace bk
