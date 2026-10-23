// Службы поверх стека AzNet: сетевой диск landisk, облачный клиент HOF и HTTP-сервер —
// по landisk.c, hof.c и http.c разбора прошивки 00019.
#include "AzNet.h"
#include "AzCrypto.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace bk {

namespace {

inline uint16_t rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline void wr16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
inline void wr32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

// CRC-16/MODBUS табличной реализацией прошивки — это побитовая CRC с переставленными байтами.
uint16_t crc16Sw(const uint8_t* p, size_t n) {
    uint16_t c = 0xFFFF;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; ++i) c = (c & 1) ? static_cast<uint16_t>((c >> 1) ^ 0xA001) : static_cast<uint16_t>(c >> 1);
    }
    return static_cast<uint16_t>((c >> 8) | (c << 8));
}

constexpr uint16_t LD_RPORT = 34162, LD_LPORT = 10000;
constexpr uint16_t HOF_PORT = 80, HOF_INIT_LPORT = 20088, HOF_WORK_LPORT = 21111;
const char kConnErr[] = "{\"RESULT\":\"ERROR\",\"DESCRIPTION\":\"CONNECTION_ERROR\"}";
const char kBuildDate[] = "May 24 2025", kBuildTime[] = "01:38:31";

// get_value_from_JSON: «"KEY":"значение"»; 0 — найдено.
int jsonValue(const char* json, const char* key, char* out, size_t cap) {
    const char* p = std::strstr(json, key);
    if (!p) return 1;
    p += std::strlen(key);
    if (!std::strstr(p, "\":\"")) return 2;
    p += 3;
    const char* end = std::strstr(p, "\"");
    if (!end) return 3;
    const size_t n = std::min<size_t>(static_cast<size_t>(end - p), cap - 1);
    std::memcpy(out, p, n);
    if (n + 1 < cap) out[n + 1] = 0;                  // прошивка ставит нуль на байт дальше конца
    return 0;
}

} // namespace

// ============================================================ landisk

bool AzNet::ldAddService() {
    return tcpAddService(LD_LPORT, [](int) { return 0; },
                         [this](int, uint8_t* f, uint16_t len) { ldOnRecv(f, len); },
                         [this](int c, uint8_t* f, uint16_t) { ldOnSend(c, f); },
                         [this](int, int) { ldConn_ = 0xFF; }, 0x408);
}

// Соединение готово: запрос уходит один раз и с FIN — сервер отвечает и закрывает.
// Запрос 938 байт: u32 блок | u16 команда | sid[32] | data[512] | имя[386] | u16 crc.
void AzNet::ldOnSend(int conn, uint8_t* f) {
    uint8_t* req = f + 54;
    if (ldState_ != 0) { tcpSend(conn, f, 0, 2); return; }
    wr16(req + 4, ldCmd_);
    wr32(req + 0, ldBlock_);
    std::memcpy(req + 0x26, ldBuf_, 512);
    std::memset(req + 0x226, 0, 0x182);
    std::strncpy(reinterpret_cast<char*>(req + 0x226), ldName_, 0x181);
    std::memcpy(req + 6, hofSid_, 32);
    wr16(req + 0x3A8, crc16Sw(req, 0x3A8));
    ldState_ = 1;
    ldResult_ = 100;
    tcpSend(conn, f, 0x3AA, 2);
}

// Ответ 516 байт: u16 результат | data[512] | u16 crc.
void AzNet::ldOnRecv(uint8_t* f, uint16_t len) {
    uint8_t* tcp = f + 34;
    uint8_t* rsp = tcp + ((tcp[12] & 0xF0) >> 2);
    if (len == 0x204 && rd16(rsp + 0x202) == crc16Sw(rsp, 0x202)) {
        std::memcpy(ldBuf_, rsp + 2, 512);
        ldResult_ = rd16(rsp);
        ldState_ = 2;
    }
}

// 0 — готово; 0x14 — нет соединения; 0x0A — нет ответа; иначе код сервера.
uint16_t AzNet::landiskCmd(const char* name, uint32_t block, uint8_t* buf, uint16_t cmd) {
    tim6On_ = true;
    if (!ldServiceOk_ && ldAddService()) ldServiceOk_ = true;
    uint8_t* th = nb() + 34;
    wr16(th, LD_LPORT);
    wr16(th + 2, LD_RPORT);
    ldState_ = 0;
    ldResult_ = 200;
    ldBuf_ = buf; ldName_ = name; ldBlock_ = block; ldCmd_ = cmd;
    int retryCmd = 10;
    while (ldState_ != 2 && retryCmd-- > 0) {
        ldState_ = 0;
        int retryOpen = 10;
        while (ldConn_ == 0xFF && retryOpen-- > 0) ldConn_ = tcpOpen(ldServerIp_, LD_RPORT, LD_LPORT);
        int32_t wait = 300000;
        if (ldConn_ == 0xFF) { ldResult_ = 0x14; continue; }
        while (ldState_ != 2 && wait > 0 && ldConn_ != 0xFF) { netPoll(); --wait; }
    }
    if (ldState_ != 2) ldResult_ = 0x0A;
    tim6On_ = false;
    return ldResult_;
}

// Шаги по 3 с: адрес -> DNS -> ARP сервера -> ping (1 с) -> служба на порту 10000.
int AzNet::landiskInit() {
    tim6On_ = true;
    uint32_t until = uptimeS() + 3;
    while (uptimeS() < until && cfg_.ip == 0) netPoll();
    if (ldServerIp_ == 0) {
        until = uptimeS() + 3;
        while (uptimeS() < until) {
            ldServerIp_ = dnsResolve("landisk.maxiol.com");
            if (ldServerIp_) break;
            netPoll();
        }
    }
    if (ldServerIp_ == 0) return -1;                  // TIM6 так и остаётся включённым
    until = uptimeS() + 3;
    while (uptimeS() < until && !arpResolve(ldServerIp_)) netPoll();
    icmpPing(ldServerIp_, 0);
    until = uptimeS() + 1;
    while (uptimeS() < until && !pingOk_) netPoll();
    if (!ldServiceOk_ && ldAddService()) ldServiceOk_ = true;
    ldConn_ = 0xFF;
    tim6On_ = false;
    return 0;
}

// ============================================================ HOF (back.hof.maxiol.com)

// Тело ответа: base64 -> AES-256 ECB -> JSON. В ответе /work2 первые 32 байта — новый ключ.
void AzNet::hofDecodeBody(uint8_t* payload, bool rotateKey) {
    char* text = reinterpret_cast<char*>(payload);
    char* body = std::strstr(text, "\r\n\r\n");
    char* cl = std::strstr(text, "Content-Length:");
    char* eol = cl ? std::strstr(cl, "\r\n") : nullptr;
    if (!body || !eol || !hofJson_) return;
    body += 4;
    *eol = 0;
    const int n = std::max(0, std::min(std::atoi(cl + 16), 0x300));
    body[n] = 0;
    std::memset(hofJson_, 0, 0x1FF);
    size_t blen = 0;
    azcrypto::base64Decode(reinterpret_cast<uint8_t*>(hofJson_), 0x200, &blen, reinterpret_cast<uint8_t*>(body), static_cast<size_t>(n));
    std::memset(payload, 0, 0x200);
    size_t olen = 0;
    azcrypto::ecbDecrypt(reinterpret_cast<uint8_t*>(hofJson_), std::min<size_t>(blen, 0x200), payload, &olen, hofKey_);
    if (rotateKey) {
        std::memcpy(hofKey_, payload, 0x20);
        std::memcpy(hofJson_, payload + 0x20, 0x1E0);
    } else {
        std::memcpy(hofJson_, payload, 0x200);
    }
    hofState_ = 2;
}

void AzNet::hofTransaction(char* json) {
    tim6On_ = true;
    jsonValue(hofJson_ ? hofJson_ : json, "SID", hofSid_, sizeof hofSid_);   // (!) из прежнего буфера
    if (!hofWorkServ_) {
        hofWorkServ_ = tcpAddService(HOF_WORK_LPORT, [](int) { return 0; },
            [this](int, uint8_t* f, uint16_t) { uint8_t* t = f + 34; hofDecodeBody(t + ((t[12] & 0xF0) >> 2), true); },
            [this](int c, uint8_t* f, uint16_t) {
                uint8_t* req = f + 54;
                uint16_t len = 0;
                if (hofState_ == 0) {
                    const size_t enc = azcrypto::ecbEncrypt(reinterpret_cast<uint8_t*>(hofJson_), strnlen(hofJson_, 0x1F0), req, hofKey_);
                    size_t b64 = 0;
                    azcrypto::base64Encode(hofJson_, 0x200, &b64, req, enc);
                    std::memset(req, 0, 0x1FF);
                    std::snprintf(reinterpret_cast<char*>(req), 0x400,
                                  "GET /work2/%s/%s HTTP/1.1\r\nHost: back.hof.maxiol.com\r\nConnection: close\r\n\r\n",
                                  hofSid_, hofJson_);
                    len = static_cast<uint16_t>(std::strlen(reinterpret_cast<char*>(req)));
                    hofState_ = 1;
                } else {
                    std::memset(req, 0, 0x1FF);
                }
                tcpSend(c, f, len, 2);
            },
            [](int, int) {}, 0x800);
    }
    uint8_t* ports = nb() + 34;
    wr16(ports, HOF_WORK_LPORT);
    wr16(ports + 2, HOF_PORT);
    hofState_ = 0;
    hofJson_ = json;
    int retry = 3;
    int32_t wait = 20000;
    while (retry-- && hofState_ != 2) {
        wait = 20000;
        int retryOpen = 3;
        hofConn_ = 0xFF;
        while (retryOpen-- && hofConn_ == 0xFF) hofConn_ = tcpOpen(hofIp_, HOF_PORT, HOF_WORK_LPORT);
        if (hofConn_ == 0xFF) continue;
        hofState_ = 0;
        while (hofState_ != 2 && wait--) netPoll();
    }
    if (hofConn_ == 0xFF || wait <= 0) std::snprintf(json, 0x200, "%s", kConnErr);
    tim6On_ = false;
}

void AzNet::hofSession(char* json) {
    tim6On_ = true;
    if (strnlen(hofKey_, 33) == 0x20 && strnlen(hofSid_, 33) == 0x20) {
        std::snprintf(json, 0x200, "{\"SID\":\"%s\",\"CMD\":\"NOP\"}", hofSid_);
        hofTransaction(json);
        if (std::strstr(json, "\"RESULT\":\"OK\"")) { tim6On_ = false; return; }
        std::memset(hofKey_, 0, sizeof hofKey_);
        std::memset(hofSid_, 0, sizeof hofSid_);
    }
    tcpDelService(HOF_INIT_LPORT);
    int32_t dnsWait = 10000;
    while (hofIp_ == 0 && dnsWait--) {
        hofIp_ = dnsResolve("back.hof.maxiol.com");
        if (!hofIp_) netPoll();
    }
    tcpAddService(HOF_INIT_LPORT, [](int) { return 0; },
        [this](int, uint8_t* f, uint16_t) { uint8_t* t = f + 34; hofDecodeBody(t + ((t[12] & 0xF0) >> 2), false); },
        [this](int c, uint8_t* f, uint16_t) {
            uint8_t* req = f + 54;
            uint16_t len = 0;
            if (hofState_ == 0) {
                const uint8_t* m = cfg_.mac;
                std::snprintf(reinterpret_cast<char*>(req), 0x400,
                              "GET /init/%02x%02x%02x%02x%02x%02x/%08X%08X%08X HTTP/1.1\r\n"
                              "Host: back.hof.maxiol.com\r\nConnection: close\r\n\r\n",
                              m[0], m[1], m[2], m[3], m[4], m[5], uid_[0], uid_[1], uid_[2]);
                len = static_cast<uint16_t>(std::strlen(reinterpret_cast<char*>(req)));
                hofState_ = 1;
                std::snprintf(hofKey_, sizeof hofKey_, "%08X%08X%08X%s", uid_[0], uid_[1], uid_[2], "_HOF2025");
            } else {
                std::memset(req, 0, 0x1FF);
            }
            tcpSend(c, f, len, 2);
        },
        [this](int, int) { tcpDelService(HOF_INIT_LPORT); }, 0x800);
    uint8_t* ports = nb() + 34;
    wr16(ports, HOF_INIT_LPORT);
    wr16(ports + 2, HOF_PORT);
    hofConn_ = 0xFF;
    int retry = 3;
    while (retry && hofConn_ == 0xFF) {
        hofConn_ = tcpOpen(hofIp_, HOF_PORT, HOF_INIT_LPORT);
        for (int w = 0; w < 500; ++w) netPoll();
        --retry;
    }
    std::memset(json, 0, 0x1FF);
    if (hofConn_ == 0xFF) std::snprintf(json, 0x200, "%s", kConnErr);
    hofState_ = 0;
    hofJson_ = json;
    int32_t wait = 20000;
    while (hofState_ != 2 && wait--) netPoll();
    if (wait <= 0) std::snprintf(json, 0x200, "%s", kConnErr);
    tim6On_ = false;
}

void AzNet::hofKeepaliveNop(char* json) {
    if (strnlen(hofKey_, 33) != 0x20 || strnlen(hofSid_, 33) != 0x20) return;
    std::snprintf(json, 0x200, "{\"SID\":\"%s\",\"CMD\":\"NOP\"}", hofSid_);
    hofTransaction(json);
    if (!std::strstr(json, "\"RESULT\":\"OK\"")) {
        std::memset(hofKey_, 0, sizeof hofKey_);
        std::memset(hofSid_, 0, sizeof hofSid_);
    }
}

// ============================================================ HTTP («MAXIOL HTTP/WEBDAV Server v1.00»)
//
// Ответ на запрос уходит по два сегмента за вызов on_send; тело — из файла карты или из
// постоянной строки. Методы WebDAV (PUT, PROPFIND, MOVE…) и файлы внутри образов RT-11
// (ИМЯ.DSK.F11/…) не эмулируются: на них — страница 404.

enum { SRC_INLINE = 0, SRC_CALLBACK = 1, SRC_CONST = 2, SRC_FILE = 3 };
enum { S_IDLE = 0, S_HEADER = 1, S_PARTIAL = 2, S_READY = 3, S_KEEP = 4 };
enum { M_GET = 0, M_HEAD = 1, M_POST = 2, M_PUT = 3, M_DELETE = 4, M_CONNECT = 5, M_OPTIONS = 6, M_TRACE = 7,
       M_PROPFIND = 8, M_PROPPATCH = 9, M_MKCOL = 10, M_COPY = 11, M_MOVE = 12, M_LOCK = 13, M_UNLOCK = 14, M_BAD = 15 };
constexpr uint32_t SEG = 0x400;
const char k404[] = "<h1>404 - Not Found</h1>";



void AzNet::httpServerInit() {
    // Сессии прошивки индексируются номером соединения (их у неё 3 при 10 соединениях — запись
    // мимо массива); здесь — по сессии на соединение.
    if (http_.size() != N_CONN) http_.resize(N_CONN);
    for (HttpSess& s : http_) { s.close(); s = HttpSess{}; }
    tcpAddService(80, [](int) { return 0; },
                  [this](int c, uint8_t* f, uint16_t len) { httpRequest(c, f, len); },
                  [this](int c, uint8_t* f, uint16_t r) { httpOnSend(c, f, r); },
                  [this](int c, int) { httpOnClose(c); }, static_cast<uint16_t>(SEG));
}

void AzNet::httpOnClose(int conn) {
    HttpSess& s = http_[conn];
    s.close();
    s.state = S_IDLE;
}

void AzNet::httpBuildHeader(HttpSess& s, char*& p) {
    auto app = [&](const char* t) { const size_t n = std::strlen(t); std::memcpy(p, t, n); p += n; };
    switch (s.status) {
    case 201: app("HTTP/1.1 201 Created\r\n"); break;
    case 2: case 200: app("HTTP/1.1 200 OK\r\n"); break;
    case 207: app("HTTP/1.1 207 Multi-Status\r\n"); break;
    default: app("HTTP/1.1 404 Not Found\r\n"); break;
    }
    if (s.ctype) { app("Content-Type: "); app(s.ctype); app("\r\n"); }
    if (s.method == M_OPTIONS) {
        app("DAV: 1,2"); app("\r\n");
        app("MS-Author-Via: DAV"); app("\r\n");
        app("Allow: OPTIONS,GET,HEAD,PUT,POST,DELETE,TRACE,PROPFIND,PROPPATCH,COPY,MOVE,LOCK,UNLOCK"); app("\r\n");
    }
    if (s.left) {
        char num[16];
        std::snprintf(num, sizeof num, "%d", static_cast<int>(s.left));
        app("Content-Length: "); app(num); app("\r\n");
    }
    app("Server: MAXIOL HTTP/WEBDAV Server v1.00 \r\n");
    app("Connection: close\r\n");
    app("\r\n");
}

// Оглавление каталога пишется НА КАРТУ файлом ИМЯ/INDEX.HTM; сам GET каталога при этом
// отвечает 404 (так в прошивке) — оглавление потом берут запросом /…/INDEX.HTM.
void AzNet::httpGenIndex(const std::string& dir, const std::string& out) {
    std::vector<DirEnt> ents;
    if (!listDir || !listDir(dir, &ents)) return;
    const std::string host = writePath ? writePath(out) : std::string();
    FILE* f = host.empty() ? nullptr : std::fopen(host.c_str(), "wb");
    if (!f) return;
    int yy, mon, day, wd, h, m, sec;
    ntpClock(yy, mon, day, wd, h, m, sec);
    std::fprintf(f, "<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 3.2 Final//EN\">\n\r<html><head><title>AZNET: Index of %s "
                    "</title></head><body><h1>Index of %s </h1><pre>Name                    Last modified          Size<hr>"
                    "<a href=\"/\">Parent Directory</a>\n", dir.c_str(), dir.c_str());
    for (const DirEnt& e : ents) {
        if (e.name.find("INDEX.HTM") != std::string::npos || e.name.find(".XML") != std::string::npos) continue;
        char yr[8], date[16], tm[16], size[24];
        std::snprintf(yr, sizeof yr, "%4i", 1980 + (e.fdate >> 9));
        std::snprintf(date, sizeof date, "20%s.%02i.%02i", yr + 2, (e.fdate >> 5) & 0xF, e.fdate & 0x1F);
        std::snprintf(tm, sizeof tm, "%02i:%02i:%02i", e.ftime >> 11, (e.ftime >> 5) & 0x3F, (e.ftime & 0x1F) * 2);
        const double sz = static_cast<float>(e.size);
        if (sz >= 1099511627776.0) std::snprintf(size, sizeof size, "%.2f TB", sz / 1099511627776.0);
        else if (sz >= 1073741824.0) std::snprintf(size, sizeof size, "%.2f GB", sz / 1073741824.0);
        else if (sz >= 1048576.0) std::snprintf(size, sizeof size, "%.2f MB", sz / 1048576.0);
        else if (sz >= 1024.0) std::snprintf(size, sizeof size, "%.2f KB", sz / 1024.0);
        else std::snprintf(size, sizeof size, "%.2f B ", sz);
        if (dir == "/")
            std::fprintf(f, "<a href=\"%s\">%-20s</a>    %s %s %10s  \n", e.name.c_str(), e.name.c_str(), date, tm, size);
        else
            std::fprintf(f, "<a href=\"%s/%s\">%-20s</a>    %s %s %10s  \n", dir.c_str(), e.name.c_str(), e.name.c_str(), date, tm, size);
    }
    std::fprintf(f, "<hr></pre><p>%04d-%02d-%02d %02d:%02d:%02d</p><h4>MAXIOL AZNET WEB Server V1.00 build %s %s </h4></body></html>\n\r",
                 static_cast<uint8_t>(yy) + 2000, mon, day, h, m, sec, kBuildDate, kBuildTime);
    std::fclose(f);
}

void AzNet::httpGet(HttpSess& s, const char* uri) {
    s.status = 404;
    s.ctype = "text/plain";
    s.body = SRC_INLINE;
    s.ofs = 0;
    s.left = 0;
    auto openRead = [&](const std::string& name) -> bool {
        const std::string host = readPath ? readPath(name) : std::string();
        if (host.empty()) return false;
        FILE* f = std::fopen(host.c_str(), "rb");
        if (!f) return false;
        std::fseek(f, 0, SEEK_END);
        const long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        s.close();
        s.fil = f;
        s.left = sz > 0 ? static_cast<uint32_t>(sz) : 0;
        return true;
    };
    const std::string u = uri;
    if (listDir && listDir(u, nullptr)) {
        httpGenIndex(u, u + "/INDEX.HTM");
    } else if (u.find(".DSK.F11/") == std::string::npos) {
        if (openRead(u)) {
            s.status = 200;
            s.ctype = nullptr;                        // поиск MIME в прошивке не находит ничего
            s.body = SRC_FILE;
        } else if (u == "/CMD.SCREENSHOT") {
            if (screenshot) screenshot("0:/SCREENS/TMP.SCR");
            if (openRead("0:/SCREENS/TMP.SCR")) {
                s.status = 200;
                s.ctype = "application/octet-stream";
                s.body = SRC_FILE;
            }
        } else if (u == "/CMD.REBOOT") {
            rebootAt_ = uptimeS() + 3;
            s.status = 200;
            s.ctype = "text/html";
            s.body = SRC_INLINE;
            s.ofs = 0;
            s.left = 0;
        }
    }
    if (s.status != 200) {
        s.body = SRC_CONST;
        s.ctype = "text/html";
        s.ptr = k404;
        s.left = 0x18;
    }
    s.statusSaved = static_cast<uint8_t>(s.status);    // (!) 404 -> 0x94 при повторе
    s.leftSaved = s.left;
    s.ofsSaved = s.ofs;
}

void AzNet::httpRequest(int conn, uint8_t* f, uint16_t len) {
    HttpSess& s = http_[conn];
    uint8_t* tcp = f + 34;
    char* req = reinterpret_cast<char*>(tcp + ((tcp[12] & 0xF0) >> 2));
    req[len] = 0;
    if (s.state != S_HEADER && s.state != S_KEEP) {
        if (s.state == S_PARTIAL && std::strstr(req, "\r\n\r\n")) s.state = S_READY;
        return;
    }
    char uriBuf[0x104] = {0};
    if (s.state == S_HEADER) {
        static const struct { const char* name; uint8_t id; } tab[] = {
            {"GET", M_GET}, {"HEAD", M_HEAD}, {"POST", M_POST}, {"PUT", M_PUT}, {"DELETE", M_DELETE},
            {"CONNECT", M_CONNECT}, {"OPTIONS", M_OPTIONS}, {"TRACE", M_TRACE}, {"PROPFIND", M_PROPFIND},
            {"PROPPATCH", M_PROPPATCH}, {"MKCOL", M_MKCOL}, {"COPY", M_COPY}, {"MOVE", M_MOVE},
            {"UNLOCK", M_UNLOCK}, {"LOCK", M_LOCK},
        };
        const char* uri = req;
        s.method = M_BAD;
        for (const auto& t : tab) {
            const char* p = std::strstr(req, t.name);
            if (p) { s.method = t.id; uri = p + std::strlen(t.name) + 1; break; }
        }
        size_t i = 0;
        while (i < sizeof uriBuf - 1 && uri[i] && uri[i] != '\n' && uri[i] != '\r' && uri[i] != ' ') { uriBuf[i] = uri[i]; ++i; }
        const char* rest = std::strchr(uri, ' ');
        s.state = rest && std::strstr(rest, "\r\n\r\n") ? S_READY : S_PARTIAL;
    }
    switch (s.method) {
    case M_GET: httpGet(s, uriBuf); break;
    case M_HEAD: {
        s.status = 200;
        const size_t n = std::strlen(uriBuf);
        if (n && uriBuf[n - 1] == '/') uriBuf[n - 1] = 0;
        break;
    }
    case M_OPTIONS:
        s.status = 200;
        s.ctype = "httpd/unix-directory";
        s.body = SRC_INLINE;
        s.ofs = 0;
        s.left = 0;
        break;
    case M_POST: case M_CONNECT: case M_TRACE:
        break;
    default:                                          // WebDAV и неизвестное
        s.status = 404;
        s.ofs = 0;
        s.body = SRC_CONST;
        s.ctype = "text/html";
        s.ptr = k404;
        s.left = 24;
        break;
    }
}

void AzNet::httpOnSend(int conn, uint8_t* f, uint16_t retransmit) {
    HttpSess& s = http_[conn];
    uint8_t* tcp = f + 34;
    char* base = reinterpret_cast<char*>(tcp + 20);
    if (s.state == S_IDLE) { s.state = S_HEADER; return; }
    if (s.state != S_READY) return;
    if (retransmit) {
        s.status = s.statusSaved;
        s.ofs = s.ofsSaved;
        s.left = s.leftSaved;
        if (s.body == SRC_FILE && s.fil) std::fseek(s.fil, static_cast<long>(s.ofs), SEEK_SET);
    } else {
        s.statusSaved = static_cast<uint8_t>(s.status);
        s.ofsSaved = s.ofs;
        s.leftSaved = s.left;
    }
    for (int seg = 2; seg; --seg) {
        uint32_t n = SEG;
        char* p = base;
        if (s.status) {
            httpBuildHeader(s, p);
            n -= static_cast<uint32_t>(p - base);
            s.status = 0;
        }
        if (s.left < n) n = s.left;
        switch (s.body) {
        case SRC_CONST:
            std::memcpy(p, s.ptr + s.ofs, n);
            break;
        case SRC_FILE:
            if (n > SEG - (s.ofs & 0x1FF)) n = SEG - (s.ofs & 0x1FF);
            n = s.fil ? static_cast<uint32_t>(std::fread(p, 1, n, s.fil)) : 0;
            break;
        default:                                      // встроенные данные сессии: их нет
            std::memset(p, 0, n);
            break;
        }
        p += n;
        s.ofs += n;
        s.left -= n;
        const int flags = (s.left == 0) ? 2 : 1;
        tcpSend(conn, f, static_cast<uint16_t>(p - base), static_cast<uint8_t>(flags));
        if (flags == 2) break;
    }
}

} // namespace bk
