#pragma once
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "McuFiber.h"
#include "NetPort.h"

namespace bk {

// Сетевая часть контроллера AZБК: микросхема ENC28J60 на SPI микроконтроллера и стек
// TCP/IP прошивки 00019 («GADO-5 TCP/IP v2.00.0» MAXIOL), перенесённый с разбора
// RE_ARM/decompiled/src (net.c, eth.c, tcp.c, netsvc.c, landisk.c, hof.c, http.c, time.c)
// и машинного декомпилята Ghidra — с теми же форматами, таймаутами и ошибками.
// docs/azbk.md, раздел «Сеть».
//
// Время. Счётчики прошивки (uptime) идут от таймера TIM6, а он тикает, только пока его
// включила прошивка: от пуска МК до команды 000, на время сетевых операций и ожидания
// по команде 010. Это время МК — эмулированное, от тактов БК. Код, который в прошивке
// крутит net_poll в цикле, исполняется сопрограммой (McuFiber) в такт с эмуляцией:
// проход net_poll стоит kPassUs, задержки (сброс ENC28J60 — 200 мс) — свои.
//
// Кабель — EthPort (tap/macvtap хоста); без него — как плата без кабеля: линка нет,
// кадры уходят в никуда.
class AzNet {
public:
    static constexpr int FRAME = 0x44C;          // буфер кадра прошивки (net_buf)
    static constexpr uint32_t kPassUs = 12;      // проход net_poll без кадров: один опрос EPKTCNT по SPI

    // Конфигурация netcfg_t (BKPSRAM, слот 0): адреса — в порядке байтов сети.
    struct Cfg {
        int16_t  timezone = 0;                   // минуты
        uint8_t  mac[6] = {0};
        uint32_t staticIp = 0;                   // не 0 — DHCP выключен
        uint32_t ip = 0, mask = 0, gw = 0, ntp = 0, dns1 = 0, dns2 = 0;
        char     dhcpname[0x41] = {0};
    };

    // Связь с остальным контроллером (карта SD, снимок экрана, перезагрузка МК).
    std::function<std::string(const std::string&)> readPath;    // «/A/B» -> файл хоста (чтение)
    std::function<std::string(const std::string&)> writePath;   // то же для записи
    struct DirEnt { std::string name; uint32_t size = 0; uint16_t fdate = 0, ftime = 0; bool dir = false; };
    // Каталог карты «/A/B»: false — нет такого каталога; имена 8.3 в верхнем регистре.
    std::function<bool(const std::string&, std::vector<DirEnt>*)> listDir;
    std::function<void(const std::string&)> screenshot;        // снимок .SCR в файл карты
    std::function<void()> reboot;                               // NVIC_SystemReset (CMD.REBOOT)
    std::function<char*()> xbuf;                                // общий буфер БК (keepalive HOF)

    AzNet();
    ~AzNet();

    // Кабель. nullptr — без кабеля.
    void setPort(std::unique_ptr<EthPort> p);
    EthPort* port() const { return port_.get(); }
    bool linkUp() const { return port_ && port_->link(); }

    // ---- Жизнь МК (вызывает AzCtrl) -------------------------------------------------
    void lanInit();                                  // lan_init: netcfg по умолчанию, ENC, DHCP, службы
    void iniLine(const std::string& line);           // строка [NETWORK] (ini_parse_network)
    void netReinit();                                // net_reinit — применить [NETWORK]
    void bootFinish();                               // main: имена DNS, 1000 проходов net_poll
    void tim6(bool on) { tim6On_ = on; }

    // ---- Время ----------------------------------------------------------------------
    // Время платы идёт; задача МК (если есть) получает ход, пока не догонит его.
    void advance(uint64_t us);
    uint64_t uptimeMs() const { return timUs_ / 1000; }
    uint32_t uptimeS() const { return static_cast<uint32_t>(timUs_ / 1000000); }
    void spend(uint64_t us);                          // время МК (в задаче — с уступкой хода)

    // ---- Задачи МК (длинные команды с сетью) ----------------------------------------
    void startJob(std::function<void()> job);        // CSR «занят», пока задача не кончится
    bool jobRunning() const { return fiber_.running(); }
    void abortJob();
    // Довести задачу до конца сейчас (её условие ожидания уже ложно): время МК может уйти
    // вперёд платы.
    void finishJob();
    std::function<void()> jobDone;                   // задача закончилась (в потоке эмуляции)

    // ---- Команды БК ------------------------------------------------------------------
    void netinfo(uint8_t out[24]) const;             // 040: IP, маска, шлюз, NTP, DNS1, DNS2
    void macAddr(uint8_t out[6]) const { for (int i = 0; i < 6; ++i) out[i] = cfg_.mac[i]; }
    void timeRecordNtp(uint16_t rec[14]) const;      // 042: запись времени по часам NTP
    void ntpResync() { ntpTimer_ = uptimeS(); }      // 035
    // 036: часы NTP (местное время) — для RTC; false, если год не позже 2021-го.
    bool ntpClock(int& yy, int& mon, int& day, int& wday, int& h, int& m, int& s) const;
    uint16_t rt11DateWord() const;                   // слово даты RT-11 по часам NTP
    uint32_t unixLocal() const { return static_cast<uint32_t>(ntpOffset_ + static_cast<int32_t>(uptimeS()) + cfg_.timezone * 60); }

    // Блокирующие операции прошивки: только из задачи (startJob).
    void netPoll();                                  // net_poll
    void netPollWait(const std::function<bool()>& more);   // net_poll_wait: проход, пока more()
    uint16_t landiskCmd(const char* name, uint32_t block, uint8_t* buf512, uint16_t cmd);
    int  landiskInit();
    void hofSession(char* json512);
    void hofTransaction(char* json512);
    enum { LD_OPEN = 1, LD_READ = 2, LD_WRITE = 3, LD_CLOSE = 4 };
    bool waitLink(uint32_t seconds);                 // пуск с сетевыми дисками: линк и IP до 10 с

    const Cfg& cfg() const { return cfg_; }
    const uint32_t* uid() const { return uid_; }
    // Для отладчика и MCP.
    std::string summary() const;
    struct Stats { uint64_t rx = 0, tx = 0, rxDrop = 0, rxFiltered = 0; } stats;

    void saveState(std::vector<uint8_t>& out) const;
    bool loadState(const uint8_t*& p, const uint8_t* end);

private:
    // ---- ENC28J60 -------------------------------------------------------------------
    static constexpr uint32_t RX_RING = 0x1A00;      // ERXST..ERXND = 0x0000..0x19FF
    void encInit();
    void encPump();                                  // кадры с кабеля — в кольцо приёма
    uint16_t encReceive(uint8_t* buf, uint16_t maxlen);
    void encSend(const uint8_t* frame, uint16_t len);
    bool encLinkLatched();                           // PHSTAT1.LLSTAT (защёлка падения линка)
    std::deque<std::vector<uint8_t>> ring_;
    uint32_t ringBytes_ = 0;
    bool rxEnabled_ = false;
    uint8_t rxOverflow_ = 0;
    uint8_t maadr_[6] = {0};
    bool llstat_ = false;
    uint64_t txBusyUntil_ = 0;                       // 10 Мбит/с: кадр на линии
    uint64_t nextPump_ = 0;

    // ---- Ethernet / ARP / IP / ICMP / UDP (eth.c) ------------------------------------
    uint8_t* nb() { return netBuf_.data(); }
    void ethInput(uint8_t* f, uint16_t len);
    void arpInput(uint8_t* f, uint16_t len);
    void ipInput(uint8_t* f, uint16_t len);
    void icmpInput(uint8_t* f, uint16_t len);
    void udpInput(uint8_t* f, uint16_t len);
    const uint8_t* arpLookup(uint32_t ip) const;
    void arpAdd(const uint8_t* mac, uint32_t ip);
    const uint8_t* arpResolveMac(uint32_t ip);
    bool arpResolve(uint32_t ip) { return arpResolveMac(ip) != nullptr; }
    bool ipSend(uint8_t* f, uint16_t len);
    void ipReply(uint8_t* f, uint16_t len);
    void ipSendRaw(uint8_t* f, uint16_t len);
    bool udpSend(uint8_t* f, uint16_t len);
    void ethReply(uint8_t* f, uint16_t len);
    void ethSendRaw(uint8_t* f, uint16_t len);
    void ethSend(uint8_t* f, uint16_t len);
    void icmpPing(uint32_t ip, uint16_t seq);
    struct ArpEntry { uint32_t ip; uint8_t mac[6]; };
    ArpEntry arp_[16] = {};
    uint8_t arpNext_ = 0;
    uint64_t arpReqTime_ = 0;
    uint32_t pingIp_ = 0;
    uint16_t pingSeq_ = 0;
    bool pingOk_ = false;

    // ---- DHCP / DNS / NTP (netsvc.c) -------------------------------------------------
    void dhcpReset();
    void dhcpClientSm();
    uint8_t* dhcpBegin(uint8_t* f, uint32_t dst, uint32_t xid);
    void dhcpInput(uint8_t* f, uint16_t len);
    uint8_t dhcpState_ = 0;
    uint32_t dhcpServer_ = 0, t1Deadline_ = 0, leaseDeadline_ = 0, dhcpXid_ = 0;
    struct DnsEntry { uint32_t retryAt; uint8_t pending; uint32_t ip; uint16_t id, qlen; char name[0x20]; };
    void dnsInit();
    void dnsAddHost(const char* name);
    uint32_t dnsResolve(const char* name);
    bool dnsSendQuery(DnsEntry& e);
    void dnsInput(uint8_t* f, uint16_t len);
    void dnsCacheTick();
    DnsEntry dns_[4] = {};
    uint8_t dnsCount_ = 0;
    uint16_t dnsSeq_ = 0;
    void ntpInit();
    void ntpPoll();
    uint32_t ntpTimer_ = 0;
    int32_t ntpOffset_ = 0;                          // unix − uptime_s

    // ---- net.c ------------------------------------------------------------------------
    void linkMonitor();
    void deferredRebootCheck();
    uint8_t linkPrev_ = 0, ntpTries_ = 0;
    uint32_t hofNext_ = 0x78, linkNext_ = 0, rebootAt_ = 0;

    // ---- TCP (tcp.c) ------------------------------------------------------------------
    struct Serv {
        uint16_t port = 0;
        std::function<int(int)> onAccept;
        std::function<void(int, uint8_t*, uint16_t)> onRecv, onSend;
        std::function<void(int, int)> onClose;
        uint16_t mss = 0;                            // в порядке байтов сети
    };
    struct Conn {
        uint8_t state = 0;
        uint64_t tLast = 0;
        uint32_t sndNxt = 0, rcvNxt = 0, ip = 0;
        uint16_t lport = 0, rport = 0;               // в порядке байтов сети
        uint8_t dir = 0, acked = 0;
        int serv = -1;
        uint8_t finSent = 0, retries = 0;
        uint32_t sndUna = 0;
    };
    enum { N_CONN = 10, N_SERV = 10 };
    void tcpInit();
    bool tcpAddService(uint16_t port, std::function<int(int)> acc, std::function<void(int, uint8_t*, uint16_t)> recv,
                       std::function<void(int, uint8_t*, uint16_t)> send, std::function<void(int, int)> close, uint16_t mss);
    void tcpDelService(uint16_t port);
    bool tcpXmit(Conn& c, uint8_t* f, uint16_t len);
    void tcpSend(int conn, uint8_t* f, uint16_t len, uint8_t flags);
    int  tcpAccept(int conn, uint8_t* f);
    int  tcpOpen(uint32_t ip, uint16_t rport, uint16_t lport);
    void tcpInput(uint8_t* f, uint16_t len);
    void tcpTimers();
    Conn conns_[N_CONN];
    Serv servs_[N_SERV];
    uint8_t nServs_ = 0;
    uint32_t iss_ = 0;

    // ---- landisk / HOF / HTTP (AzNetApps.cpp) ------------------------------------------
    void ldOnSend(int conn, uint8_t* f);
    void ldOnRecv(uint8_t* f, uint16_t len);
    uint32_t ldServerIp_ = 0;
    bool ldServiceOk_ = false;
    int ldConn_ = 0, ldState_ = 0;
    uint8_t* ldBuf_ = nullptr;
    const char* ldName_ = nullptr;
    uint32_t ldBlock_ = 0;
    uint16_t ldCmd_ = 0, ldResult_ = 0;
    bool ldAddService();
    void hofDecodeBody(uint8_t* payload, bool rotateKey);
    void hofKeepaliveNop(char* json);
    uint32_t hofIp_ = 0;
    bool hofWorkServ_ = false;
    int hofConn_ = 0, hofState_ = 0;
    char* hofJson_ = nullptr;
    char hofKey_[33] = {0}, hofSid_[33] = {0};
    std::vector<char> hofScratch_;                   // вместо xbuf, если контроллер его не дал
    struct HttpSess {
        uint8_t state = 0;
        uint16_t status = 0;
        const char* ctype = nullptr;
        uint8_t body = 0;
        uint32_t ofs = 0, left = 0;
        FILE* fil = nullptr;
        const char* ptr = nullptr;
        uint8_t method = 0;
        uint8_t statusSaved = 0;
        uint32_t leftSaved = 0, ofsSaved = 0;
        void close() { if (fil) { std::fclose(fil); fil = nullptr; } }
    };
    void httpServerInit();
    void httpRequest(int conn, uint8_t* f, uint16_t len);
    void httpOnSend(int conn, uint8_t* f, uint16_t retransmit);
    void httpOnClose(int conn);
    void httpGet(HttpSess& s, const char* uri);
    void httpBuildHeader(HttpSess& s, char*& p);
    void httpGenIndex(const std::string& dir, const std::string& out);
    void makeUid();
    std::vector<HttpSess> http_;

    // ---- Общее ------------------------------------------------------------------------
    Cfg cfg_;
    uint32_t uid_[3] = {0};
    std::unique_ptr<EthPort> port_;
    std::vector<uint8_t> netBuf_;                    // net_buf + запас (прошивка пишет за 0x44C)
    std::vector<uint8_t> rxBuf_;
    McuFiber fiber_;
    uint64_t mcuUs_ = 0, boardUs_ = 0;               // часы МК и платы
    uint64_t timUs_ = 0;                             // накопленное время TIM6
    bool tim6On_ = false;
    bool unbounded_ = false;                         // finishJob: задача не ждёт плату
};

} // namespace bk
