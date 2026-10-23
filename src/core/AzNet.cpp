#include "AzNet.h"
#include <algorithm>
#include <cstring>

// Поля кадров прошивка читает и пишет как 16/32-разрядные слова ARM (младший байт
// первым) — перенос побайтово точен только на машине с тем же порядком.
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "AzNet: нужен little-endian хост");

namespace bk {

namespace {

inline uint16_t rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline void wr16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
inline uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline void wr32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
constexpr uint16_t HTONS(uint16_t x) { return static_cast<uint16_t>((x << 8) | (x >> 8)); }
inline uint32_t bswap32(uint32_t x) { return __builtin_bswap32(x); }

constexpr int IP_OFS = 14, UDP_OFS = 34, BOOTP_OFS = 42, DHCP_OPT = BOOTP_OFS + 240;

// Время SPI-обмена с ENC28J60 (SPI2 21 МГц, вокруг каждой операции delay_us(5) дважды).
constexpr uint64_t kSpiOpUs = 11;
uint64_t spiBytesUs(uint32_t n) { return (n * 38 + 99) / 100; }   // 0,38 мкс на байт

// Сумма для IP/UDP/TCP: результат уже в порядке байтов сети (eth.c inet_checksum).
uint16_t inetChecksum(uint32_t sum, const uint8_t* p, uint32_t len) {
    while (len >= 2) { sum += static_cast<uint32_t>((p[0] << 8) | p[1]); p += 2; len -= 2; }
    if (len) sum += static_cast<uint32_t>(p[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return static_cast<uint16_t>(~HTONS(static_cast<uint16_t>(sum)));
}

const uint8_t kDefaultMac[6] = {0x00, 0x0E, 0x35, 0x22, 0x0B, 0x01};   // 0x080163BC
const uint8_t kParamList[] = {1, 3, 42, 54, 6, 73};                     // запрос опций DHCP
constexpr uint32_t LEASE_CLAMP = 21600;                                  // 6 ч
constexpr uint32_t NTP_EPOCH = 2208988800u;

enum { DH_BOUND = 1, DH_SELECTING = 2, DH_REQUESTING = 3 };
enum { ST_FREE = 0, ST_SYN_SENT = 1, ST_SYN_RCVD = 2, ST_ESTABLISHED = 3, ST_FIN_WAIT = 4 };
enum { DIR_NEW = 0, DIR_REPLY = 1, DIR_RAW = 2 };
enum { CLOSE_REMOTE = 0, CLOSE_RESET = 1, CLOSE_LOCAL = 2, CLOSE_TIMEOUT = 3 };
enum : uint8_t { FIN = 1, SYN = 2, RST = 4, PSH = 8, ACK = 0x10 };

// parse_int прошивки (0x0801462E): веса разрядов по длине, не больше пяти цифр.
int parseInt(const char* s, int n) {
    int i = 0, sign = 1, len = n;
    if (*s == '-') { i = 1; sign = -1; --len; }
    else if (*s == '+') { i = 1; --len; }
    int w = len == 5 ? 10000 : len == 4 ? 1000 : len == 3 ? 100 : len == 2 ? 10 : 1;
    int16_t v = 0;
    for (; i < n; ++i) { v = static_cast<int16_t>(v + (static_cast<uint8_t>(s[i]) - '0') * w); w /= 10; }
    return static_cast<int16_t>(v * sign);
}

// «a.b.c.d» -> адрес в порядке байтов сети (parse_octet ×4, 0x080098D8).
uint32_t parseIp(const char* s) {
    const int len = static_cast<int>(std::strlen(s));
    int dot[3] = {0, 0, 0}, k = 0;
    for (int i = 0; i < len && k < 3; ++i) if (s[i] == '.') dot[k++] = i;
    const int o1 = parseInt(s, dot[0]);
    const int o2 = parseInt(s + dot[0] + 1, (dot[1] - dot[0] - 1) & 0xFF);
    const int o3 = parseInt(s + dot[1] + 1, (dot[2] - dot[1] - 1) & 0xFF);
    const int o4 = parseInt(s + dot[2] + 1, ((len & 0xFF) - dot[2] - 1) & 0xFF);
    return static_cast<uint32_t>(o1 & 0xFF) | (static_cast<uint32_t>(o2 & 0xFF) << 8) |
           (static_cast<uint32_t>(o3 & 0xFF) << 16) | (static_cast<uint32_t>(o4 & 0xFF) << 24);
}

uint8_t hexPair(const char* p) {
    uint8_t v = 0;
    if (p[0] >= '0' && p[0] <= '9') v = static_cast<uint8_t>((p[0] - '0') << 4);
    if (p[0] >= 'A' && p[0] <= 'F') v = static_cast<uint8_t>((p[0] - 'A' + 10) << 4);
    if (p[1] >= '0' && p[1] <= '9') v = static_cast<uint8_t>(v + p[1] - '0');
    if (p[1] >= 'A' && p[1] <= 'F') v = static_cast<uint8_t>(v + p[1] - 'A' + 10);
    return v;
}

bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
int monthDays(bool leap, int m) {
    if (m == 1) return leap ? 29 : 28;
    if (m > 6) --m;
    return (m % 2 == 1) ? 30 : 31;
}

} // namespace

AzNet::AzNet() : netBuf_(4096, 0), rxBuf_(2048, 0) {}

AzNet::~AzNet() {
    fiber_.abort();
    for (HttpSess& h : http_) h.close();
}

void AzNet::setPort(std::unique_ptr<EthPort> p) {
    port_ = std::move(p);
    ring_.clear();
    ringBytes_ = 0;
    llstat_ = linkUp();
}

// ============================================================ время МК

void AzNet::spend(uint64_t us) {
    mcuUs_ += us;
    if (tim6On_) timUs_ += us;
    // В задаче — подождать, пока плата догонит: ход переходит к эмуляции.
    while (fiber_.inFiber() && !unbounded_ && mcuUs_ > boardUs_) fiber_.yield();
}

void AzNet::advance(uint64_t us) {
    boardUs_ += us;
    // ENC28J60 принимает всегда, независимо от того, опрашивает ли его МК: раз в
    // миллисекунду кадры с кабеля переходят в кольцо приёма (не больше шести за раз —
    // столько за миллисекунду и дошло бы по 10 Мбит/с; остальные ждут в очереди хоста).
    if (boardUs_ >= nextPump_) {
        encPump();
        nextPump_ = boardUs_ + 1000;
    }
    if (fiber_.running()) {
        if (boardUs_ < mcuUs_ + 1000) return;        // задачу будим раз в миллисекунду
        fiber_.resume();
        if (!fiber_.running()) {
            if (mcuUs_ < boardUs_) { if (tim6On_) timUs_ += boardUs_ - mcuUs_; mcuUs_ = boardUs_; }
            if (jobDone) jobDone();
        }
        return;
    }
    // МК ждёт в главном цикле: время идёт (и TIM6, если включён).
    if (mcuUs_ < boardUs_) {
        if (tim6On_) timUs_ += boardUs_ - mcuUs_;
        mcuUs_ = boardUs_;
    }
}

void AzNet::startJob(std::function<void()> job) {
    fiber_.start(std::move(job));
    // Первый ход — сразу: задача, которой не нужна сеть, кончится тут же.
    fiber_.resume();
    if (!fiber_.running() && jobDone) jobDone();
}

void AzNet::abortJob() {
    fiber_.abort();
}

void AzNet::finishJob() {
    if (!fiber_.running()) return;
    unbounded_ = true;
    while (fiber_.running()) fiber_.resume();
    unbounded_ = false;
    if (jobDone) jobDone();
}

// ============================================================ ENC28J60

void AzNet::encInit() {
    // enc_hw_reset: 100 + 100 мс, программный сброс 1 мс, затем около 25 записей регистров.
    spend(201000 + 25 * kSpiOpUs);
    ring_.clear();
    ringBytes_ = 0;
    std::memcpy(maadr_, cfg_.mac, 6);
    rxEnabled_ = true;
    rxOverflow_ = 0;
    txBusyUntil_ = 0;
}

// ERXFCON после сброса = 0xA1: свой адрес (UCEN), широковещательные (BCEN), годный CRC.
// Кадр длиннее MAMXFL (0x44C с CRC) MAC отбрасывает.
void AzNet::encPump() {
    if (!port_) return;
    if (!port_->link()) { llstat_ = false; return; }
    for (int n = 0; n < 6; ++n) {
        const size_t len = port_->recv(rxBuf_.data(), rxBuf_.size());
        if (!len) break;
        const uint8_t* d = rxBuf_.data();
        const bool bcast = len >= 6 && std::all_of(d, d + 6, [](uint8_t b) { return b == 0xFF; });
        if (len < 14 || (!bcast && std::memcmp(d, maadr_, 6) != 0)) { ++stats.rxFiltered; continue; }
        const uint32_t need = 6 + ((static_cast<uint32_t>(len) + 4 + 1) & ~1u);
        if (!rxEnabled_ || len + 4 > static_cast<size_t>(FRAME) || ringBytes_ + need > RX_RING) { ++stats.rxDrop; continue; }
        ring_.emplace_back(d, d + len);
        ringBytes_ += need;
        ++stats.rx;
    }
}

// enc_packet_receive: длина кадра без CRC, 0 — нет кадров. Семь и больше кадров в
// кольце — приём останавливается, больше семи — все выбрасываются (так в прошивке).
uint16_t AzNet::encReceive(uint8_t* buf, uint16_t maxlen) {
    spend(kSpiOpUs);                                  // EPKTCNT
    size_t cnt = ring_.size();
    if (cnt == 0) {
        if (rxOverflow_) { spend(6 * kSpiOpUs); rxEnabled_ = true; rxOverflow_ = 0; }
        return 0;
    }
    if (cnt >= 7) {
        rxEnabled_ = false;
        spend(3 * kSpiOpUs);
        cnt = ring_.size();
        if (cnt > 7) {
            rxOverflow_ = 2;
            spend(cnt * (kSpiOpUs + 100));
            ring_.clear();
            ringBytes_ = 0;
            stats.rxDrop += cnt;
            return 0;
        }
        rxOverflow_ = 1;
    }
    std::vector<uint8_t> f = std::move(ring_.front());
    ring_.pop_front();
    ringBytes_ -= 6 + ((static_cast<uint32_t>(f.size()) + 4 + 1) & ~1u);
    uint16_t len = static_cast<uint16_t>(f.size());
    if (len > maxlen) len = maxlen;
    std::memcpy(buf, f.data(), len);
    spend(9 * kSpiOpUs + spiBytesUs(len));
    return len;
}

void AzNet::encSend(const uint8_t* frame, uint16_t len) {
    // Ждём, пока предыдущий кадр уйдёт с линии (ECON1.TXRTS), затем буфер передачи.
    if (txBusyUntil_ > mcuUs_) spend(txBusyUntil_ - mcuUs_);
    spend(10 * kSpiOpUs + spiBytesUs(len + 1));
    uint8_t pad[60];
    const uint8_t* out = frame;
    uint16_t n = len;
    if (n < 60) {                                     // MACON3.PADCFG: дополнение до 60 байт
        std::memset(pad, 0, sizeof pad);
        std::memcpy(pad, frame, n);
        out = pad;
        n = 60;
    }
    txBusyUntil_ = mcuUs_ + (static_cast<uint64_t>(n) + 4 + 8 + 12) * 8 / 10;   // 10 Мбит/с
    ++stats.tx;
    if (port_ && port_->link()) port_->send(out, n);
}

// PHSTAT1.LLSTAT. Защёлка падает, когда пропадает несущая, и встаёт при чтении, если линк
// есть. Сброс микросхемы её не роняет — иначе прошивка, проверяющая линк раз в 5 с,
// переинициализировала бы ENC28J60 бесконечно, а живые платы так не делают.
bool AzNet::encLinkLatched() {
    spend(5 * kSpiOpUs + 11);
    const bool up = linkUp();
    const bool v = llstat_ && up;
    llstat_ = up;
    return v;
}

// ============================================================ Ethernet / ARP

const uint8_t* AzNet::arpLookup(uint32_t ip) const {
    for (const ArpEntry& e : arp_) if (e.ip == ip) return e.mac;
    return nullptr;
}

void AzNet::arpAdd(const uint8_t* mac, uint32_t ip) {
    if (arpLookup(ip) || arpNext_ >= 16) return;
    arp_[arpNext_].ip = ip;
    std::memcpy(arp_[arpNext_].mac, mac, 6);
    if (++arpNext_ == 16) arpNext_ = 0;
}

// MAC из кэша или nullptr; запрос ARP — не чаще раза в 200 мс. Запрос собирается в общем
// буфере кадра — кадр, который хотели отправить, при этом теряется (как в прошивке).
const uint8_t* AzNet::arpResolveMac(uint32_t ip) {
    const uint8_t* mac = arpLookup(ip);
    if (mac) return mac;
    if (static_cast<int64_t>(arpReqTime_ - uptimeMs()) >= 0) return nullptr;
    uint8_t* f = nb();
    uint8_t* a = f + 14;
    std::memset(f, 0xFF, 6);
    wr16(f + 12, HTONS(0x0806));
    wr16(a + 0, HTONS(1));
    wr16(a + 2, HTONS(0x0800));
    a[4] = 6; a[5] = 4;
    wr16(a + 6, HTONS(1));
    std::memcpy(a + 8, cfg_.mac, 6);
    wr32(a + 14, cfg_.ip);
    std::memset(a + 18, 0, 6);
    wr32(a + 24, ip);
    ethSend(f, 28);
    arpReqTime_ = uptimeMs() + 200;
    return nullptr;
}

void AzNet::arpInput(uint8_t* f, uint16_t len) {
    uint8_t* a = f + 14;
    if (len < 28 || rd16(a) != HTONS(1) || rd16(a + 2) != HTONS(0x0800)) return;
    if (rd32(a + 24) != cfg_.ip) return;
    if (rd16(a + 6) == HTONS(1)) {
        wr16(a + 6, HTONS(2));
        std::memmove(a + 18, a + 8, 10);
        std::memcpy(a + 8, cfg_.mac, 6);
        wr32(a + 14, cfg_.ip);
        ethReply(f, 28);
    } else if (rd16(a + 6) == HTONS(2)) {
        const uint32_t sip = rd32(a + 14);
        if (arpLookup(sip)) return;
        arp_[arpNext_].ip = sip;
        std::memcpy(arp_[arpNext_].mac, a + 8, 6);
        if (++arpNext_ == 16) arpNext_ = 0;
    }
}

void AzNet::ethReply(uint8_t* f, uint16_t len) {
    std::memcpy(f, f + 6, 6);
    std::memcpy(f + 6, cfg_.mac, 6);
    encSend(f, static_cast<uint16_t>(len + 14));
}

void AzNet::ethSendRaw(uint8_t* f, uint16_t len) { encSend(f, static_cast<uint16_t>(len + 14)); }

void AzNet::ethSend(uint8_t* f, uint16_t len) {
    std::memcpy(f + 6, cfg_.mac, 6);
    encSend(f, static_cast<uint16_t>(len + 14));
}

void AzNet::ethInput(uint8_t* f, uint16_t len) {
    if (len < 14) return;
    if (rd16(f + 12) == HTONS(0x0800)) ipInput(f, static_cast<uint16_t>(len - 14));
    else if (rd16(f + 12) == HTONS(0x0806)) arpInput(f, static_cast<uint16_t>(len - 14));
}

// ============================================================ IP / ICMP / UDP

bool AzNet::ipSend(uint8_t* f, uint16_t len) {
    uint8_t* ip = f + 14;
    const uint32_t dst = rd32(ip + 16);
    if (dst == (cfg_.ip | ~cfg_.mask)) {
        std::memset(f, 0xFF, 6);
    } else {
        const uint32_t hop = ((dst ^ cfg_.ip) & cfg_.mask) == 0 ? dst : cfg_.gw;
        const uint8_t* mac = arpResolveMac(hop);
        if (!mac) return false;
        std::memmove(f, mac, 6);
    }
    wr16(f + 12, HTONS(0x0800));
    const uint16_t tot = static_cast<uint16_t>(len + 20);
    ip[0] = 0x45; ip[1] = 0;
    wr16(ip + 2, HTONS(tot));
    wr16(ip + 4, 0); wr16(ip + 6, 0);
    ip[8] = 64;
    wr16(ip + 10, 0);
    wr32(ip + 12, cfg_.ip);
    wr16(ip + 10, inetChecksum(0, ip, 20));
    ethSend(f, tot);
    return true;
}

void AzNet::ipReply(uint8_t* f, uint16_t len) {
    uint8_t* ip = f + 14;
    const uint16_t tot = static_cast<uint16_t>(len + 20);
    wr16(ip + 2, HTONS(tot));
    wr16(ip + 4, 0); wr16(ip + 6, 0);
    ip[8] = 64;
    wr16(ip + 10, 0);
    std::memmove(ip + 16, ip + 12, 4);
    wr32(ip + 12, cfg_.ip);
    wr16(ip + 10, inetChecksum(0, ip, 20));
    ethReply(f, tot);
}

void AzNet::ipSendRaw(uint8_t* f, uint16_t len) {
    uint8_t* ip = f + 14;
    const uint16_t tot = static_cast<uint16_t>(len + 20);
    wr16(ip + 2, HTONS(tot));
    wr16(ip + 10, 0);
    wr16(ip + 10, inetChecksum(0, ip, 20));
    ethSendRaw(f, tot);
}

void AzNet::icmpInput(uint8_t* f, uint16_t len) {
    uint8_t* ip = f + 14;
    uint8_t* ic = ip + 20;
    if (len < 8) return;
    if (ic[0] == 8) {
        ic[0] = 0;
        wr16(ic + 2, static_cast<uint16_t>(rd16(ic + 2) + 8));   // поправка суммы без переноса
        ipReply(f, len);
    }
    if (ic[0] == 0 && rd16(ic + 4) == 0xA1B1 && rd16(ic + 6) == pingSeq_ && rd32(ip + 12) == pingIp_)
        pingOk_ = true;
}

void AzNet::icmpPing(uint32_t ipAddr, uint16_t seq) {
    uint8_t* f = nb();
    uint8_t* ip = f + 14;
    uint8_t* ic = ip + 20;
    std::memset(f, 0xAA, FRAME);
    pingIp_ = ipAddr;
    pingSeq_ = HTONS(seq);
    ip[9] = 1;
    wr32(ip + 12, cfg_.ip);
    wr32(ip + 16, pingIp_);
    ic[0] = 8; ic[1] = 0;
    wr16(ic + 2, 0);
    wr16(ic + 4, 0xA1B1);
    wr16(ic + 6, pingSeq_);
    uint8_t* p = ic + 8;
    for (char c = '0'; c < 'P'; ++c) *p++ = static_cast<uint8_t>(c);
    wr16(ic + 2, inetChecksum(0, ic, 40));
    pingOk_ = false;
    ipSend(f, 40);
}

bool AzNet::udpSend(uint8_t* f, uint16_t len) {
    uint8_t* ip = f + 14;
    uint8_t* udp = ip + 20;
    const uint16_t ulen = static_cast<uint16_t>(len + 8);
    ip[9] = 17;
    wr32(ip + 12, cfg_.ip);
    wr16(udp + 4, HTONS(ulen));
    wr16(udp + 6, 0);
    wr16(udp + 6, inetChecksum(ulen + 17u, udp - 8, ulen + 8u));
    return ipSend(f, ulen);
}

void AzNet::udpInput(uint8_t* f, uint16_t len) {
    uint8_t* udp = f + UDP_OFS;
    if (len < 8) return;
    len = static_cast<uint16_t>(HTONS(rd16(udp + 4)) - 8);
    switch (rd16(udp + 2)) {
    case HTONS(68): dhcpInput(f, len); break;
    case HTONS(53): dnsInput(f, len); break;
    case HTONS(14444):
        if (len >= 0x30) {                       // ntp_parse_reply: метка передачи
            const uint32_t t = bswap32(rd32(udp + 8 + 40)) - NTP_EPOCH;
            if (t) ntpOffset_ = static_cast<int32_t>(t - uptimeS());
        }
        break;
    default: break;                              // SNMP (161/162) не эмулируется
    }
}

void AzNet::ipInput(uint8_t* f, uint16_t len) {
    uint8_t* ip = f + 14;
    const uint16_t cks = rd16(ip + 10);
    wr16(ip + 10, 0);
    if (ip[0] != 0x45 || inetChecksum(0, ip, 20) != cks) return;
    const uint32_t dst = rd32(ip + 16);
    if (dst != cfg_.ip && dst != (cfg_.ip | ~cfg_.mask)) return;
    const uint16_t tot = HTONS(rd16(ip + 2));
    if (tot < 20 || tot > len) return;           // у прошивки проверки нет; кадр короче — мусор
    len = static_cast<uint16_t>(tot - 20);
    switch (ip[9]) {
    case 1:  icmpInput(f, len); break;
    case 17: udpInput(f, len); break;
    case 6:  tcpInput(f, len); break;
    default: break;
    }
}

// ============================================================ DHCP

void AzNet::dhcpReset() {
    dhcpState_ = 0;
    leaseDeadline_ = uptimeS() + 1;
}

uint8_t* AzNet::dhcpBegin(uint8_t* f, uint32_t dst, uint32_t xid) {
    uint8_t* b = f + BOOTP_OFS;
    wr32(f + IP_OFS + 16, dst);
    wr16(f + UDP_OFS + 2, HTONS(67));
    wr16(f + UDP_OFS + 0, HTONS(68));
    std::memset(b, 0, 0xF0);
    b[0] = 1; b[1] = 1; b[2] = 6;
    wr32(b + 4, xid);
    wr16(b + 10, HTONS(0x8000));
    std::memcpy(b + 28, cfg_.mac, 6);
    wr32(b + 236, 0x63538263);
    return b + 240;
}

void AzNet::dhcpClientSm() {
    uint8_t* f = nb();
    if (leaseDeadline_ <= uptimeS()) {
        dhcpState_ = DH_SELECTING;
        leaseDeadline_ = uptimeS() + 15;
        // xid — миллисекунды и хвост ЗАВОДСКОГО MAC прошивки, а не MAC из настроек.
        dhcpXid_ = static_cast<uint32_t>(uptimeMs() << 16) + kDefaultMac[5] + kDefaultMac[4] * 256u;
        cfg_.ip = cfg_.mask = cfg_.gw = 0;
        uint8_t* o = dhcpBegin(f, 0xFFFFFFFF, dhcpXid_);
        *o++ = 53; *o++ = 1; *o++ = 1; *o++ = 0;
        *o++ = 55; *o++ = 6;
        std::memcpy(o, kParamList, 6); o += 6;
        *o++ = 0;
        *o++ = 0xFF;
        udpSend(f, static_cast<uint16_t>(o - (f + BOOTP_OFS)));
    }
    if (t1Deadline_ <= uptimeS() && dhcpState_ == DH_BOUND) {
        dhcpXid_ = static_cast<uint32_t>(uptimeMs() << 16) + static_cast<uint32_t>(uptimeMs());
        uint8_t* o = dhcpBegin(f, dhcpServer_, dhcpXid_);
        uint8_t* b = f + BOOTP_OFS;
        wr32(b + 12, cfg_.ip);
        *o++ = 53; *o++ = 1; *o++ = 3; *o++ = 0;
        *o++ = 50; *o++ = 4; wr32(o, cfg_.ip); o += 4;
        *o++ = 54; *o++ = 4; wr32(o, dhcpServer_); o += 4;
        *o++ = 55; *o++ = 6;
        std::memcpy(o, kParamList, 6); o += 6;
        *o++ = 0;
        *o++ = 0xFF;
        if (!udpSend(f, static_cast<uint16_t>(o - b))) t1Deadline_ = uptimeS() + 5;
        else dhcpState_ = DH_REQUESTING;
    }
}

void AzNet::dhcpInput(uint8_t* f, uint16_t len) {
    uint8_t* b = f + BOOTP_OFS;
    uint8_t* o = f + DHCP_OPT;
    uint32_t mask = 0, gw = 0, ntp = 0, dns1 = 0, dns2 = 0, lease = 0, t1 = 0, server = 0;
    uint8_t type = 0;
    if (len <= 0xEF) return;
    if (b[0] != 2 || rd32(b + 4) != dhcpXid_ || rd32(b + 236) != 0x63538263) return;
    int left = len - 0xF0;
    uint8_t* p = o;
    uint8_t* const end = netBuf_.data() + netBuf_.size() - 8;
    while (left > 1 && p < end) {
        if (*p == 0) { ++p; --left; continue; }
        if (*p == 0xFF) break;
        switch (*p) {
        case 1:  mask = rd32(p + 2); break;
        case 3:  gw = rd32(p + 2); break;
        case 6:
            if (p[1] == 4) dns1 = rd32(p + 2);
            if (p[1] > 7) { dns1 = rd32(p + 2); dns2 = rd32(p + 6); }
            break;
        case 42: ntp = rd32(p + 2); break;
        case 51: lease = std::min(bswap32(rd32(p + 2)), LEASE_CLAMP); break;
        case 53: type = p[2]; break;
        case 54: server = rd32(p + 2); break;
        case 58: t1 = std::min(bswap32(rd32(p + 2)), LEASE_CLAMP); break;
        default: break;
        }
        const uint8_t l = static_cast<uint8_t>(p[1] + 2);
        p += l;
        left -= l;
    }
    if (server == 0) server = rd32(f + IP_OFS + 12);
    if (type == 2) {                                   // OFFER -> REQUEST прямо в принятом кадре
        if (dhcpState_ != DH_SELECTING || rd32(b + 16) == 0) return;
        dhcpState_ = DH_REQUESTING;
        wr32(f + IP_OFS + 16, 0xFFFFFFFF);
        wr16(f + UDP_OFS + 2, HTONS(67));
        wr16(f + UDP_OFS + 0, HTONS(68));
        o[0] = 53; o[1] = 1; o[2] = 3; o[3] = 0;
        o[4] = 50; o[5] = 4; wr32(o + 6, rd32(b + 16));
        o[10] = 54; o[11] = 4; wr32(o + 12, server);
        const size_t nl = strnlen(cfg_.dhcpname, sizeof cfg_.dhcpname - 1);
        o[16] = 12;
        o[17] = static_cast<uint8_t>(nl);
        std::memcpy(o + 18, cfg_.dhcpname, nl);
        p = o + 18 + nl;
        if (nl & 1) *p++ = 0;
        *p++ = 55; *p++ = 6;
        std::memcpy(p, kParamList, 6); p += 6;
        *p++ = 0;
        *p++ = 0xFF;
        b[0] = 1;
        wr32(b + 16, 0);
        wr32(b + 20, 0);
        wr16(b + 10, HTONS(0x8000));
        udpSend(f, static_cast<uint16_t>(p - b));
    } else if (type == 5 && dhcpState_ == DH_REQUESTING) {
        if (t1 == 0) t1 = lease / 2;
        dhcpState_ = DH_BOUND;
        dhcpServer_ = server;
        t1Deadline_ = uptimeS() + t1;
        leaseDeadline_ = uptimeS() + lease;
        cfg_.ip = rd32(b + 16);
        cfg_.mask = mask; cfg_.gw = gw; cfg_.ntp = ntp; cfg_.dns1 = dns1; cfg_.dns2 = dns2;
    }
}

// ============================================================ DNS

void AzNet::dnsInit() { std::memset(dns_, 0, sizeof dns_); dnsCount_ = 0; }

void AzNet::dnsAddHost(const char* name) {
    for (int i = 0; i < dnsCount_; ++i) if (!std::strcmp(name, dns_[i].name)) return;
    if (dnsCount_ >= 4) return;
    DnsEntry& e = dns_[dnsCount_++];
    std::memset(&e, 0, sizeof e);
    std::strncpy(e.name, name, sizeof e.name - 1);
    e.retryAt = uptimeS() + 1;
}

bool AzNet::dnsSendQuery(DnsEntry& e) {
    uint8_t* f = nb();
    uint8_t* q = f + UDP_OFS + 8;
    wr32(f + IP_OFS + 16, cfg_.dns1);
    wr16(f + UDP_OFS + 2, HTONS(53));
    wr16(f + UDP_OFS + 0, HTONS(53));
    const uint16_t a = dnsSeq_++, b2 = dnsSeq_++;   // счётчик читается дважды, половины смешаны
    wr16(q, static_cast<uint16_t>((a << 8) | (b2 >> 8)));
    e.id = rd16(q);
    wr16(q + 2, 0x8001);
    wr16(q + 4, 0x0100);
    wr16(q + 6, 0); wr16(q + 8, 0); wr16(q + 10, 0);
    uint8_t* n = q + 12;
    uint16_t i = 1, lenpos = 0;
    int8_t cnt = 0;
    for (const char* s = e.name; *s; ++s) {
        if (*s == '.') { n[lenpos] = static_cast<uint8_t>(cnt); cnt = -1; lenpos = i; }
        n[i++] = static_cast<uint8_t>(*s);
        ++cnt;
    }
    n[lenpos] = static_cast<uint8_t>(cnt);
    n[i] = 0;
    n[i + 1] = 0; n[i + 2] = 1;
    n[i + 3] = 0; n[i + 4] = 1;
    e.qlen = static_cast<uint16_t>(i + 4);
    return !udpSend(f, static_cast<uint16_t>(i + 0x11));
}

uint32_t AzNet::dnsResolve(const char* name) {
    for (int i = 0; i < dnsCount_; ++i) {
        DnsEntry& e = dns_[i];
        if (!std::strcmp(name, e.name)) {
            if (e.ip == 0) { e.pending = 0; return 0; }
            return e.ip;
        }
    }
    dnsAddHost(name);
    return 0;
}

void AzNet::dnsInput(uint8_t* f, uint16_t) {
    uint8_t* q = f + UDP_OFS + 8;
    const uint16_t id = rd16(q);
    int i = 0;
    while (i < dnsCount_ && dns_[i].id != id) ++i;
    if (i >= dnsCount_) return;
    if (!(q[2] & 0x80)) return;
    if (rd16(q + 6) == 0 && rd16(q + 8) == 0 && rd16(q + 10) == 0) return;
    uint8_t* rr = q + 12;
    uint32_t cur = dns_[i].qlen + 1u;
    const uint32_t end = static_cast<uint32_t>(HTONS(rd16(f + UDP_OFS + 4))) - 12;
    const uint8_t* lim = netBuf_.data() + netBuf_.size() - 16;
    while (cur < end && rr + cur < lim) {
        const uint16_t type = static_cast<uint16_t>((rr[cur + 2] << 8) | rr[cur + 3]);
        const uint16_t rdl = static_cast<uint16_t>((rr[cur + 10] << 8) | rr[cur + 11]);
        if (type == 1 && rdl == 4) {
            char name[0x40];                          // dns_decode_name: имя вопроса
            const uint8_t* p = rr;
            char* out = name;
            while (*p && out < name + sizeof name - 2) {
                uint8_t k = *p++;
                while (k-- && out < name + sizeof name - 2) *out++ = static_cast<char>(*p++);
                *out++ = '.';
            }
            out[out > name ? -1 : 0] = 0;
            for (int j = 0; j < dnsCount_; ++j)
                if (!std::strcmp(name, dns_[j].name)) { dns_[j].ip = rd32(rr + cur + 12); return; }
            return;
        }
        cur += 12u + rdl;
    }
}

void AzNet::dnsCacheTick() {
    for (int i = 0; i < dnsCount_; ++i)
        if (dns_[i].ip == 0 && dns_[i].retryAt <= uptimeS()) {
            dnsSendQuery(dns_[i]);
            dns_[i].retryAt = uptimeS() + 1;
        }
}

// ============================================================ NTP и часы

void AzNet::ntpInit() {
    ntpTimer_ = 0;
    if (cfg_.timezone > 720 || cfg_.timezone < -720) cfg_.timezone = 0;
}

void AzNet::ntpPoll() {
    if (cfg_.ntp == 0 || ntpTimer_ > uptimeS()) return;
    if (cfg_.ip == 0) { ntpTimer_ = uptimeS() + 10; return; }
    uint8_t* f = nb();
    wr32(f + IP_OFS + 16, cfg_.ntp);
    wr16(f + UDP_OFS + 2, HTONS(123));
    wr16(f + UDP_OFS + 0, HTONS(14444));
    std::memset(f + UDP_OFS + 8, 0, 0x30);
    f[UDP_OFS + 8] = 8;                               // LI=0 VN=1 Mode=0 — так у прошивки
    ntpTimer_ = uptimeS() + (udpSend(f, 0x30) ? 600 : 10);
}

bool AzNet::ntpClock(int& yy, int& mon, int& day, int& wday, int& h, int& m, int& s) const {
    const uint32_t t = unixLocal();
    uint16_t days = static_cast<uint16_t>(t / 86400);
    int year = 1970, mm = 0;
    wday = (days + 4) % 7;
    while (days >= (isLeap(year) ? 366 : 365)) { days = static_cast<uint16_t>(days - (isLeap(year) ? 366 : 365)); ++year; }
    while (days >= monthDays(isLeap(year), mm)) { days = static_cast<uint16_t>(days - monthDays(isLeap(year), mm)); ++mm; }
    day = days + 1;
    mon = mm + 1;
    yy = year - 2000;
    h = static_cast<int>(t / 3600 % 24);
    m = static_cast<int>(t / 60 % 60);
    s = static_cast<int>(t % 60);
    return static_cast<uint8_t>(yy) > 21;
}

uint16_t AzNet::rt11DateWord() const {
    int yy, mon, day, wd, h, m, s;
    ntpClock(yy, mon, day, wd, h, m, s);
    const uint8_t y = static_cast<uint8_t>(yy + 28);
    return static_cast<uint16_t>(((y / 32) << 14) | (mon << 10) | (day << 5) | (y % 32));
}

void AzNet::timeRecordNtp(uint16_t r[14]) const {
    int yy, mon, day, wd, h, m, s;
    ntpClock(yy, mon, day, wd, h, m, s);
    const uint32_t t50 = static_cast<uint32_t>(h * 3600 * 50 + m * 60 * 50 + s * 50);
    const uint32_t t60 = static_cast<uint32_t>(h * 3600 * 60 + m * 60 * 60 + s * 60);
    r[0] = rt11DateWord();
    r[1] = static_cast<uint16_t>(t50 >> 16); r[2] = static_cast<uint16_t>(t50);
    r[3] = static_cast<uint16_t>(t60 >> 16); r[4] = static_cast<uint16_t>(t60);
    r[5] = static_cast<uint16_t>(day | (mon << 5) | ((static_cast<uint8_t>(yy) + 20) << 9));
    r[6] = static_cast<uint16_t>((s / 2) | (m << 5) | (h << 11));
    r[7] = static_cast<uint16_t>(static_cast<uint8_t>(yy) + 2000);
    r[8] = static_cast<uint16_t>(mon); r[9] = static_cast<uint16_t>(day); r[10] = static_cast<uint16_t>(wd);
    r[11] = static_cast<uint16_t>(h); r[12] = static_cast<uint16_t>(m); r[13] = static_cast<uint16_t>(s);
}

void AzNet::netinfo(uint8_t out[24]) const {
    const uint32_t v[6] = {cfg_.ip, cfg_.mask, cfg_.gw, cfg_.ntp, cfg_.dns1, cfg_.dns2};
    std::memcpy(out, v, 24);
}

// ============================================================ net.c

void AzNet::lanInit() {
    // Сброс МК: его ОЗУ пусто. Остаются кабель, связи с контроллером и часы платы.
    abortJob();
    std::memset(&cfg_, 0, sizeof cfg_);
    std::memcpy(cfg_.mac, kDefaultMac, 6);           // BKPSRAM без записи — значения по умолчанию
    std::strncpy(cfg_.dhcpname, "AZNET-BK.home.maxiol.com", sizeof cfg_.dhcpname - 1);
    std::memset(arp_, 0, sizeof arp_); arpNext_ = 0; arpReqTime_ = 0;
    pingIp_ = 0; pingSeq_ = 0; pingOk_ = false;
    dhcpState_ = 0; dhcpServer_ = t1Deadline_ = leaseDeadline_ = dhcpXid_ = 0;
    dnsInit(); dnsSeq_ = 0;
    ntpTimer_ = 0; ntpOffset_ = 0;
    linkPrev_ = ntpTries_ = 0; hofNext_ = 0x78; linkNext_ = 0; rebootAt_ = 0;
    for (Conn& c : conns_) c = Conn{};
    iss_ = 0;
    ldServerIp_ = 0; ldServiceOk_ = false; ldConn_ = 0; ldState_ = 0; ldBuf_ = nullptr; ldName_ = nullptr;
    ldBlock_ = 0; ldCmd_ = 0; ldResult_ = 0;
    hofIp_ = 0; hofWorkServ_ = false; hofConn_ = 0; hofState_ = 0; hofJson_ = nullptr;
    std::memset(hofKey_, 0, sizeof hofKey_); std::memset(hofSid_, 0, sizeof hofSid_);
    stats = Stats{};
    mcuUs_ = boardUs_;
    timUs_ = 0;
    nextPump_ = boardUs_;
    makeUid();
    tim6On_ = true;                                   // tim6_config
    encInit();
    if (cfg_.staticIp == 0) {
        dhcpReset();
        cfg_.ip = cfg_.mask = cfg_.gw = cfg_.ntp = 0;
        ntpTimer_ = 0;
    }
    tcpInit();
    ntpInit();
    pingOk_ = false;
    linkPrev_ = 0;
    httpServerInit();
    dnsInit();
    hofNext_ = 7200;
}

// Строка [NETWORK] (ini_parse_network): ключ ищется в строке как подстрока.
void AzNet::iniLine(const std::string& line) {
    auto val = [&](const char* key) -> const char* {
        const size_t p = line.find(key);
        return p == std::string::npos ? nullptr : line.c_str() + p + std::strlen(key);
    };
    const char* v;
    if ((v = val("DHCPNAME="))) { std::memset(cfg_.dhcpname, 0, sizeof cfg_.dhcpname); std::strncpy(cfg_.dhcpname, v, sizeof cfg_.dhcpname - 1); }
    if ((v = val("IP="))) cfg_.ip = parseIp(v);
    if ((v = val("MASK="))) cfg_.mask = parseIp(v);
    if ((v = val("GW="))) cfg_.gw = parseIp(v);
    if ((v = val("NTP="))) cfg_.ntp = parseIp(v);
    if ((v = val("DNS1="))) cfg_.dns1 = parseIp(v);
    if ((v = val("DNS2="))) cfg_.dns2 = parseIp(v);
    if ((v = val("DHCP="))) cfg_.staticIp = std::strcmp(v, "OFF") == 0 ? 1 : 0;
    if ((v = val("MAC=")) && std::strlen(v) == 12)
        for (int i = 0; i < 6; ++i) cfg_.mac[i] = hexPair(v + 2 * i);
    if ((v = val("TIMEZONE="))) cfg_.timezone = static_cast<int16_t>(parseInt(v, static_cast<int>(std::strlen(v) & 0xFF)));
}

// UID кристалла STM32 (ключ HOF, описание платы для landisk): у эмулятора его нет, берём
// из MAC — свой у каждой платы, постоянный.
void AzNet::makeUid() {
    uid_[0] = 0x455A4B42;                             // «BKZE»
    uid_[1] = static_cast<uint32_t>(cfg_.mac[0]) << 24 | static_cast<uint32_t>(cfg_.mac[1]) << 16 |
              static_cast<uint32_t>(cfg_.mac[2]) << 8 | cfg_.mac[3];
    uid_[2] = static_cast<uint32_t>(cfg_.mac[4]) << 24 | static_cast<uint32_t>(cfg_.mac[5]) << 16 | 0x0019;
}

void AzNet::netReinit() {
    makeUid();
    encInit();
    if (cfg_.staticIp == 0) {
        dhcpState_ = 0;
        leaseDeadline_ = uptimeS() + 1;
        cfg_.ip = cfg_.mask = cfg_.gw = cfg_.ntp = 0;
        ntpTimer_ = 0;
    }
    ntpInit();
    linkPrev_ = 0;
    ntpTries_ = 0;
    tcpInit();
    httpServerInit();
}

void AzNet::bootFinish() {
    dnsAddHost("back.hof.maxiol.com");
    dnsAddHost("back2.hof.maxiol.com");
    dnsAddHost("landisk.maxiol.com");
    dnsAddHost("landisk2.maxiol.com");
    for (int i = 0; i < 1000; ++i) netPoll();
}

bool AzNet::waitLink(uint32_t seconds) {
    const uint32_t until = uptimeS() + seconds;
    while (cfg_.ip == 0 && until > uptimeS()) netPoll();
    return cfg_.ip != 0;
}

void AzNet::linkMonitor() {
    if (!encLinkLatched()) {
        // Ghidra 0x08005304: адреса обнуляются и при постоянном IP — до перезапуска МК.
        encInit();
        dhcpState_ = 0;
        leaseDeadline_ = uptimeS() + 2;
        cfg_.ip = cfg_.mask = cfg_.gw = cfg_.ntp = 0;
        linkNext_ = uptimeS() + 5;
    }
}

void AzNet::deferredRebootCheck() {
    if (rebootAt_ && rebootAt_ < uptimeS() && reboot) { rebootAt_ = 0; reboot(); }
}

void AzNet::netPoll() {
    spend(kPassUs - kSpiOpUs);                        // сам опрос EPKTCNT посчитан в encReceive
    uint16_t len;
    while ((len = encReceive(nb(), FRAME)) != 0) ethInput(nb(), len);
    if (cfg_.staticIp == 0) dhcpClientSm();
    if (hofNext_ < uptimeS()) {
        hofNext_ = uptimeS() + 108000;
        char* x = xbuf ? xbuf() : nullptr;
        if (!x) { hofScratch_.assign(0x220, 0); x = hofScratch_.data(); }
        hofKeepaliveNop(x);
    }
    if (cfg_.ip) {
        if (linkPrev_ != 1) {
            if (linkPrev_ == 3 && ntpTries_++ > 20) { linkPrev_ = 1; ntpTries_ = 0; }
            if (linkPrev_ == 2 && arpResolve(cfg_.ntp)) { linkPrev_ = 3; ntpTries_ = 0; }
            if (linkPrev_ == 0) linkPrev_ = 2;
        }
        tcpTimers();
        deferredRebootCheck();
        dnsCacheTick();
        ntpPoll();
    } else {
        linkPrev_ = 0;
    }
    if (linkNext_ < uptimeS()) {
        linkMonitor();
        linkNext_ = uptimeS() + 5;
    }
}

void AzNet::netPollWait(const std::function<bool()>& more) {
    tim6On_ = true;
    do netPoll(); while (more());
    tim6On_ = false;
}

// ============================================================ TCP

void AzNet::tcpInit() {
    nServs_ = 0;
    for (Serv& s : servs_) s = Serv{};
}

bool AzNet::tcpAddService(uint16_t port, std::function<int(int)> acc, std::function<void(int, uint8_t*, uint16_t)> recv,
                          std::function<void(int, uint8_t*, uint16_t)> send, std::function<void(int, int)> close, uint16_t mss) {
    Serv* s = nullptr;
    int i = 0;
    for (; i < N_SERV; ++i)
        if (servs_[i].port == port || servs_[i].port == 0) { s = &servs_[i]; break; }
    if (nServs_ >= 9 && i >= nServs_ && !s) return false;
    if (!s) s = &servs_[nServs_++];
    s->port = port;
    s->onAccept = std::move(acc);
    s->onRecv = std::move(recv);
    s->onSend = std::move(send);
    s->onClose = std::move(close);
    s->mss = HTONS(mss ? mss : 0x408);
    return true;
}

void AzNet::tcpDelService(uint16_t port) {
    for (Serv& s : servs_) if (s.port == port) s.port = 0;
}

bool AzNet::tcpXmit(Conn& c, uint8_t* f, uint16_t len) {
    uint8_t* ip = f + 14;
    uint8_t* t = ip + 20;
    const Serv& s = servs_[c.serv];
    uint16_t tlen = len;
    bool ok = true;
    if (c.dir == DIR_NEW) {
        wr32(ip + 16, c.ip);
        wr32(ip + 12, cfg_.ip);
        ip[9] = 6;
        wr16(t + 2, c.lport);
        wr16(t + 0, c.rport);
        wr16(t + 14, s.mss);
    }
    if (c.dir == DIR_REPLY) {
        const uint16_t tmp = rd16(t + 0);
        wr16(t + 0, rd16(t + 2));
        wr16(t + 2, tmp);
    }
    if (c.dir != DIR_RAW) {
        wr16(t + 14, s.mss);
        wr16(t + 18, 0);
    }
    if (t[13] & SYN) {
        t[12] = 0x60;
        t[20] = 2; t[21] = 4; t[22] = 4; t[23] = 0;    // MSS 1024
        tlen = 4;
    } else {
        t[12] = 0x50;
    }
    wr32(t + 4, bswap32(c.sndNxt));
    wr32(t + 8, bswap32(c.rcvNxt));
    tlen = static_cast<uint16_t>(tlen + 20);
    wr16(t + 16, 0);
    wr16(t + 16, inetChecksum(tlen + 6u, t - 8, tlen + 8u));
    switch (c.dir) {
    case DIR_NEW:
        wr16(t + 2, c.lport);
        wr16(t + 0, c.rport);
        ok = ipSend(f, tlen);
        c.dir = DIR_RAW;
        break;
    case DIR_REPLY:
        ipReply(f, tlen);
        c.dir = DIR_RAW;
        break;
    default:
        ipSendRaw(f, tlen);
        break;
    }
    c.sndNxt += len;
    if (t[13] & (SYN | FIN)) ++c.sndNxt;
    if ((t[13] & ACK) && ok) c.acked = 1;
    return ok;
}

void AzNet::tcpSend(int conn, uint8_t* f, uint16_t len, uint8_t flags) {
    Conn& c = conns_[conn];
    uint8_t* t = f + 34;
    uint8_t fl = ACK;
    if (c.state != ST_ESTABLISHED) return;
    if (flags & 1) fl |= PSH;
    if (flags & 2) { fl |= FIN; c.state = ST_FIN_WAIT; }
    t[13] = fl;
    tcpXmit(c, f, len);
}

int AzNet::tcpAccept(int conn, uint8_t* f) {
    uint8_t* ip = f + 14;
    uint8_t* t = ip + 20;
    const uint16_t port = HTONS(rd16(t + 2));
    for (int i = 0; i < N_SERV; ++i)
        if (servs_[i].port == port && servs_[i].onAccept) {
            servs_[i].onAccept(conn);
            arpAdd(f + 6, rd32(ip + 12));
            return i;
        }
    return -1;
}

int AzNet::tcpOpen(uint32_t ipAddr, uint16_t rport, uint16_t lport) {
    int s = -1;
    for (int i = 0; i < N_SERV; ++i) if (servs_[i].port == lport) { s = i; break; }
    if (s < 0) return 0xFF;
    for (int i = 0; i < N_CONN; ++i)
        if (conns_[i].state && conns_[i].rport == HTONS(lport) && conns_[i].lport == HTONS(rport) && conns_[i].ip == ipAddr)
            return i;
    int slot = -1;
    for (int i = 0; i < N_CONN; ++i) if (conns_[i].state == ST_FREE) { slot = i; break; }
    if (slot < 0) return 0xFF;
    Conn& c = conns_[slot];
    c.sndNxt = iss_++;
    c.state = ST_SYN_SENT;
    c.tLast = uptimeMs();
    c.rcvNxt = 0;
    c.ip = ipAddr;
    c.lport = HTONS(rport);
    c.rport = HTONS(lport);
    c.serv = s;
    c.finSent = 0;
    c.retries = 0;
    c.sndUna = c.sndNxt;
    c.dir = DIR_NEW;
    nb()[34 + 13] = SYN;
    if (tcpXmit(c, nb(), 0)) return slot;
    c.state = ST_FREE;
    return 0xFF;
}

void AzNet::tcpInput(uint8_t* f, uint16_t len) {
    uint8_t* ip = f + 14;
    uint8_t* t = ip + 20;
    if (rd32(ip + 16) != cfg_.ip) return;
    const uint16_t dlen = static_cast<uint16_t>(len - ((t[12] & 0xF0) >> 2));
    const uint8_t flags = t[13] & 0x17;
    int conn = -1;
    const uint32_t sip = rd32(ip + 12);
    for (int i = 0; i < N_CONN; ++i)
        if (conns_[i].state && conns_[i].ip == sip && conns_[i].lport == rd16(t + 0) && conns_[i].rport == rd16(t + 2)) {
            conn = i;
            break;
        }
    if (conn < 0) {                                    // новое соединение — только SYN
        if (flags != SYN) return;
        for (int i = 0; i < N_CONN; ++i) if (conns_[i].state == ST_FREE) { conn = i; break; }
        if (conn < 0) return;
        const int s = tcpAccept(conn, f);
        if (s < 0) return;
        Conn& c = conns_[conn];
        c.state = ST_SYN_RCVD;
        c.tLast = uptimeMs();
        c.sndNxt = static_cast<uint32_t>(uptimeMs() << 16) + static_cast<uint32_t>(uptimeMs());
        c.rcvNxt = bswap32(rd32(t + 4)) + 1;
        c.ip = sip;
        c.lport = rd16(t + 0);
        c.rport = rd16(t + 2);
        c.serv = s;
        c.dir = DIR_REPLY;
        c.acked = 0;
        c.finSent = 0;
        c.retries = 0;
        c.sndUna = c.sndNxt;
        t[13] = SYN | ACK;
        tcpXmit(c, f, 0);
        return;
    }
    Conn& c = conns_[conn];
    Serv& s = servs_[c.serv];
    c.dir = DIR_REPLY;
    c.acked = 0;
    if (flags & RST) {
        if ((c.state == ST_ESTABLISHED || c.state == ST_FIN_WAIT) && s.onClose) s.onClose(conn, CLOSE_RESET);
        c.state = ST_FREE;
        return;
    }
    const uint32_t seq = bswap32(rd32(t + 4));
    if (seq != c.rcvNxt && !(flags & SYN) && c.state == ST_SYN_SENT) {
        c.rcvNxt = seq;
        c.sndNxt = bswap32(rd32(t + 8));
        t[13] = ACK | FIN;
        tcpXmit(c, f, 0);
        c.state = ST_FREE;
        if (s.onClose) s.onClose(conn, CLOSE_RESET);
        return;
    }
    if (seq == c.rcvNxt && bswap32(rd32(t + 8)) == c.sndNxt && (flags & ACK)) {
        // по порядку
    } else if (flags & SYN) {
        c.rcvNxt = seq;
        c.sndNxt = bswap32(rd32(t + 8));
    } else {
        return;
    }
    c.sndUna = c.sndNxt;
    c.retries = 0;
    c.rcvNxt += dlen;
    if (flags & (FIN | SYN)) ++c.rcvNxt;
    c.tLast = uptimeMs();
    switch (c.state) {
    case ST_SYN_SENT:
        if (flags != (SYN | ACK)) { c.state = ST_FREE; break; }
        t[13] = ACK;
        tcpXmit(c, f, 0);
        c.state = ST_ESTABLISHED;
        if (s.onSend) s.onSend(conn, f, 0);
        break;
    case ST_SYN_RCVD:
        if (flags != ACK) { c.state = ST_FREE; break; }
        c.state = ST_ESTABLISHED;
        if (s.onSend) s.onSend(conn, f, 0);
        break;
    case ST_ESTABLISHED:
        if (flags == (FIN | ACK)) {
            if (dlen && s.onRecv) s.onRecv(conn, f, dlen);
            t[13] = FIN | ACK;
            tcpXmit(c, f, 0);
            c.state = ST_FREE;
            if (s.onClose) s.onClose(conn, CLOSE_REMOTE);
        } else if (flags == ACK) {
            if (dlen && s.onRecv) s.onRecv(conn, f, dlen);
            if (s.onSend) s.onSend(conn, f, 0);
            if (dlen && !c.acked) { t[13] = ACK; tcpXmit(c, f, 0); }
        }
        break;
    case ST_FIN_WAIT:
        if (flags == (FIN | ACK)) {
            if (dlen && s.onRecv) s.onRecv(conn, f, dlen);
            t[13] = ACK;
            tcpXmit(c, f, 0);
            c.state = ST_FREE;
            if (s.onClose) s.onClose(conn, CLOSE_LOCAL);
        } else if (flags == ACK && dlen) {
            if (s.onRecv) s.onRecv(conn, f, dlen);
            t[13] = ACK;
            tcpXmit(c, f, 0);
            c.finSent = 1;
        }
        break;
    default:
        break;
    }
}

void AzNet::tcpTimers() {
    uint8_t* t = nb() + 34;
    for (int i = 0; i < N_CONN; ++i) {
        Conn& c = conns_[i];
        if (c.state == ST_FREE) continue;
        Serv& s = servs_[c.serv];
        if (static_cast<int64_t>(1000 - (uptimeMs() - c.tLast)) >= 0) continue;
        if (c.retries > 5) {
            c.state = ST_FREE;
            if (s.onClose) s.onClose(i, CLOSE_TIMEOUT);
            continue;
        }
        c.tLast = uptimeMs();
        ++c.retries;
        c.sndNxt = c.sndUna;
        c.dir = DIR_NEW;
        c.acked = 0;
        switch (c.state) {
        case ST_SYN_SENT: t[13] = SYN; tcpXmit(c, nb(), 0); break;
        case ST_SYN_RCVD: t[13] = SYN | ACK; tcpXmit(c, nb(), 0); break;
        case ST_FIN_WAIT:
            if (c.finSent) { t[13] = ACK; tcpXmit(c, nb(), 0); c.state = ST_FREE; }
            else c.state = ST_ESTABLISHED;
            break;
        case ST_ESTABLISHED:
            if (s.onSend) s.onSend(i, nb(), 1);
            if (!c.acked) { t[13] = ACK; tcpXmit(c, nb(), 0); }
            break;
        default: break;
        }
    }
}

// ============================================================ сводка и снимок

std::string AzNet::summary() const {
    auto ipS = [](uint32_t a) {
        char b[20];
        std::snprintf(b, sizeof b, "%u.%u.%u.%u", a & 0xFF, (a >> 8) & 0xFF, (a >> 16) & 0xFF, a >> 24);
        return std::string(b);
    };
    static const char* kDh[] = {"нет", "адрес получен", "поиск", "запрос"};
    char b[512];
    std::snprintf(b, sizeof b,
                  "Сеть: %s, линк %s; MAC %02X:%02X:%02X:%02X:%02X:%02X, %s, имя %s\n"
                  "  IP %s маска %s шлюз %s DNS %s/%s NTP %s; DHCP: %s\n"
                  "  кадров принято %llu, отброшено %llu (чужих %llu), отправлено %llu; TIM6 %s, uptime %u с%s\n",
                  port_ ? port_->describe().c_str() : "без кабеля", linkUp() ? "есть" : "нет",
                  cfg_.mac[0], cfg_.mac[1], cfg_.mac[2], cfg_.mac[3], cfg_.mac[4], cfg_.mac[5],
                  cfg_.staticIp ? "постоянный адрес" : "DHCP", cfg_.dhcpname,
                  ipS(cfg_.ip).c_str(), ipS(cfg_.mask).c_str(), ipS(cfg_.gw).c_str(), ipS(cfg_.dns1).c_str(),
                  ipS(cfg_.dns2).c_str(), ipS(cfg_.ntp).c_str(), cfg_.staticIp ? "выкл" : kDh[dhcpState_ & 3],
                  static_cast<unsigned long long>(stats.rx), static_cast<unsigned long long>(stats.rxDrop),
                  static_cast<unsigned long long>(stats.rxFiltered), static_cast<unsigned long long>(stats.tx),
                  tim6On_ ? "идёт" : "стоит", uptimeS(), fiber_.running() ? "; МК занят сетью" : "");
    std::string s = b;
    for (int i = 0; i < N_CONN; ++i) {
        const Conn& c = conns_[i];
        if (!c.state) continue;
        static const char* kSt[] = {"", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "FIN_WAIT"};
        std::snprintf(b, sizeof b, "  TCP %d: %u -> %s:%u %s\n", i, HTONS(c.lport), ipS(c.ip).c_str(), HTONS(c.rport),
                      kSt[c.state % 5]);
        s += b;
    }
    return s;
}

// В снимок — то, что переживает живую сеть: настройки, часы, аренда DHCP, ключ HOF.
// Соединения TCP и задача МК не сохраняются: после загрузки их просто нет.
void AzNet::saveState(std::vector<uint8_t>& out) const {
    auto put = [&](const void* v, size_t n) { const uint8_t* p = static_cast<const uint8_t*>(v); out.insert(out.end(), p, p + n); };
    const uint32_t ver = 1;
    put(&ver, 4);
    put(&cfg_, sizeof cfg_);
    put(&timUs_, 8);
    const uint8_t fl = static_cast<uint8_t>(tim6On_ ? 1 : 0);
    put(&fl, 1);
    put(&ntpOffset_, 4); put(&ntpTimer_, 4);
    put(&dhcpState_, 1); put(&dhcpServer_, 4); put(&t1Deadline_, 4); put(&leaseDeadline_, 4); put(&dhcpXid_, 4);
    put(&hofNext_, 4); put(&linkNext_, 4);
    put(hofKey_, sizeof hofKey_); put(hofSid_, sizeof hofSid_);
    put(&ldServerIp_, 4); put(&hofIp_, 4);
}

bool AzNet::loadState(const uint8_t*& p, const uint8_t* e) {
    auto get = [&](void* v, size_t n) { if (static_cast<size_t>(e - p) < n) return false; std::memcpy(v, p, n); p += n; return true; };
    uint32_t ver;
    if (!get(&ver, 4) || ver != 1) return false;
    abortJob();
    uint8_t fl;
    if (!get(&cfg_, sizeof cfg_) || !get(&timUs_, 8) || !get(&fl, 1) || !get(&ntpOffset_, 4) || !get(&ntpTimer_, 4)
        || !get(&dhcpState_, 1) || !get(&dhcpServer_, 4) || !get(&t1Deadline_, 4) || !get(&leaseDeadline_, 4)
        || !get(&dhcpXid_, 4) || !get(&hofNext_, 4) || !get(&linkNext_, 4) || !get(hofKey_, sizeof hofKey_)
        || !get(hofSid_, sizeof hofSid_) || !get(&ldServerIp_, 4) || !get(&hofIp_, 4)) return false;
    cfg_.dhcpname[sizeof cfg_.dhcpname - 1] = 0;
    tim6On_ = fl & 1;
    for (Conn& c : conns_) c = Conn{};
    ldConn_ = 0xFF; hofConn_ = 0xFF;
    std::memcpy(maadr_, cfg_.mac, 6);
    ring_.clear(); ringBytes_ = 0;
    mcuUs_ = boardUs_;
    return true;
}

} // namespace bk
