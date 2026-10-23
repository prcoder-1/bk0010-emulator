#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace bk {

// «Кабель» сетевой карты: кадры Ethernet без CRC туда и обратно. Неблокирующий.
class EthPort {
public:
    virtual ~EthPort() = default;
    // Следующий принятый кадр в buf; 0 — кадров нет.
    virtual size_t recv(uint8_t* buf, size_t cap) = 0;
    virtual void send(const uint8_t* frame, size_t len) = 0;
    virtual bool link() const = 0;                 // есть ли несущая
    virtual std::string describe() const = 0;      // для сводки
    virtual bool live() const { return true; }     // настоящая сеть хоста (не тест)
};

// Порт хоста по описанию:
//   tap:ИМЯ      — постоянный tap-интерфейс (ip tuntap add ИМЯ mode tap user $USER);
//   macvtap:ИМЯ  — macvtap на физической карте (свой MAC в локальной сети),
//                  открывается /dev/tapN по ifindex интерфейса;
//   /dev/tapN    — то же, прямо устройством.
// mac — MAC платы: macvtap пропускает только кадры на свой MAC, при расхождении в warn
// попадает подсказка. nullptr и текст ошибки в err — не открылся.
std::unique_ptr<EthPort> openEthPort(const std::string& spec, const uint8_t mac[6],
                                     std::string* err, std::string* warn = nullptr);
// Для macvtap: совпадает ли MAC интерфейса с MAC платы; пусто — да (или это не macvtap).
std::string ethPortMacWarning(const std::string& spec, const uint8_t mac[6]);

} // namespace bk
