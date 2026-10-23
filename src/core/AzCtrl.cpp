#include "AzCtrl.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <sys/stat.h>

namespace bk {

namespace fs = std::filesystem;

// Встроенный загрузчик прошивки (bootB, 0x08016C86 в AZBK_00019.hex; разбор —
// RE_ARM/bootB_pdp11.lst): {адрес, число слов, слова}. Его выдают потоки BOOT1/BOOT2,
// если в AZ.INI нет [ALTBOOT]. Читает блок 0 диска AZn в 01000 и передаёт туда управление.
static const uint16_t kBootB[] = {
    010000, 144,
    000240, 000240, 000240, 000005, 012706, 010000, 012704, 0177220, 005024, 010405, 016700, 000410,
    032700, 0177770, 001402, 004767, 000116, 010015, 012744, 000001, 005015, 012714, 000002, 012714,
    000005, 012702, 001000, 012701, 000400, 0105714, 0100376, 012714, 000015, 011522, 077102, 000137,
    001000, 012700, 000007, 0105737, 0177564, 0100375, 010037, 0177566, 000207, 004767, 0177760, 0112100,
    001374, 000207, 0105737, 0177560, 0100375, 013700, 0177562, 000207, 000000, 000207, 000240, 012701,
    010314, 004767, 0177740, 000240, 004767, 0177740, 010001, 042701, 000007, 022701, 000060, 001403,
    004767, 0177666, 000765, 004767, 0177664, 010003, 004767, 0177704, 022700, 000015, 001406, 022700,
    000177, 001407, 004767, 0177632, 000765, 010300, 042700, 0177770, 000207, 012701, 010304, 004767,
    0177634, 000736, 025052, 005015, 037040, 000040, 005015, 055101, 042114, 026522, 026527, 047516,
    041040, 047517, 020124, 044504, 045523, 047040, 046525, 042502, 020122, 050123, 041505, 043111,
    042511, 006504, 004412, 047105, 042524, 020122, 044124, 020105, 047502, 052117, 042040, 051511,
    020113, 052516, 041115, 051105, 055440, 027060, 033456, 006535, 020012, 020076, 000000, 044504,
};

static std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Компоненты пути «0:/A/B» (без префикса диска); пусто, если в пути есть «..».
static bool splitPath(const std::string& name, std::vector<std::string>& parts) {
    std::string s = name;
    if (s.size() >= 2 && s[1] == ':') s = s.substr(2);
    std::replace(s.begin(), s.end(), '\\', '/');
    parts.clear();
    size_t i = 0;
    while (i <= s.size()) {
        const size_t j = s.find('/', i);
        const std::string p = s.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (p == "..") return false;
        if (!p.empty() && p != ".") parts.push_back(p);
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return true;
}

// FAT регистр букв не различает: компонент ищем без учёта регистра.
std::string AzCtrl::resolve(const std::string& root, const std::string& name) {
    std::vector<std::string> parts;
    if (root.empty() || !splitPath(name, parts)) return {};
    fs::path p = root;
    std::error_code ec;
    for (const std::string& c : parts) {
        fs::path exact = p / c;
        if (fs::exists(exact, ec)) { p = exact; continue; }
        bool found = false;
        for (const auto& e : fs::directory_iterator(p, ec)) {
            if (upper(e.path().filename().string()) == upper(c)) { p = e.path(); found = true; break; }
        }
        if (!found) return {};
    }
    return p.string();
}

std::string AzCtrl::writePath(const std::string& name) const {
    std::vector<std::string> parts;
    const std::string root = cfg_.writeDir.empty() ? cfg_.sdDir : cfg_.writeDir;
    if (root.empty() || !splitPath(name, parts) || parts.empty()) return {};
    // Уже есть такой файл (в любом регистре) — пишем в него.
    const std::string got = resolve(root, name);
    if (!got.empty() && !fs::is_directory(got)) return got;
    fs::path p = root;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        const std::string sub = resolve(p.string(), parts[i]);
        p = sub.empty() ? p / upper(parts[i]) : fs::path(sub);
    }
    std::error_code ec;
    fs::create_directories(p, ec);
    return (p / parts.back()).string();
}

std::string AzCtrl::readPath(const std::string& name) const {
    if (!cfg_.writeDir.empty()) {
        const std::string w = resolve(cfg_.writeDir, name);
        if (!w.empty()) return w;
    }
    return resolve(cfg_.sdDir, name);
}

static void fatStamp(const std::string& path, uint16_t& date, uint16_t& tme, uint32_t& size, bool& dir) {
    struct stat st {};
    date = tme = 0; size = 0; dir = false;
    if (::stat(path.c_str(), &st) != 0) return;
    std::tm tm {};
    const time_t t = st.st_mtime;
    localtime_r(&t, &tm);
    const int y = std::max(0, tm.tm_year + 1900 - 1980);
    date = static_cast<uint16_t>(tm.tm_mday | ((tm.tm_mon + 1) << 5) | (y << 9));
    tme = static_cast<uint16_t>((tm.tm_sec / 2) | (tm.tm_min << 5) | (tm.tm_hour << 11));
    size = static_cast<uint32_t>(st.st_size);
    dir = S_ISDIR(st.st_mode);
}

std::string AzCtrl::diskNameStr(int u) const {
    const char* p = reinterpret_cast<const char*>(&ram_[R_DISKS + u * DISK_WORDS + 5]);
    return std::string(p, strnlen(p, NAME_BYTES));
}

std::string AzCtrl::cstr(uint32_t at, size_t maxBytes) const {
    std::string s;
    for (size_t i = 0; i < maxBytes; ++i) {
        const uint16_t v = w(at + static_cast<uint32_t>(i / 2));
        const char c = static_cast<char>((i & 1) ? v >> 8 : v & 0377);
        if (!c) break;
        s += c;
    }
    return s;
}

void AzCtrl::fillDiskEntry(int u, const std::string& host) {
    uint16_t d, t; uint32_t size; bool dir;
    fatStamp(host, d, t, size, dir);
    const uint32_t blocks = size >> 9;
    uint16_t* e = &ram_[R_DISKS + u * DISK_WORDS];
    e[0] = static_cast<uint16_t>(blocks); e[1] = static_cast<uint16_t>(blocks >> 16);
    e[2] = d; e[3] = t;
    bytes(R_DISKS + u * DISK_WORDS + 4)[1] = 0x20;   // attr: архивный
}

bool AzCtrl::openDisk(int u, const std::string& host, const std::string& name) {
    Unit& un = units_[u];
    if (un.f) { std::fclose(un.f); un.f = nullptr; }
    un.overlay.clear();
    un.host = host;
    char* nm = diskName(u);
    std::memset(nm, 0, NAME_BYTES);
    std::strncpy(nm, name.c_str(), NAME_BYTES - 1);
    diskFlags(u) |= DISK_MOUNTED;
    if (host.empty() || fs::is_directory(host)) {
        diskFlags(u) = static_cast<uint8_t>((diskFlags(u) | DISK_ERROR) & ~DISK_MOUNTED);
        bootErrors_ |= 0x4000;
        return false;
    }
    un.rw = cfg_.rw;
    un.f = std::fopen(host.c_str(), un.rw ? "r+b" : "rb");
    if (!un.f && un.rw) { un.rw = false; un.f = std::fopen(host.c_str(), "rb"); }
    if (!un.f) {
        diskFlags(u) |= DISK_ERROR;
        bootErrors_ |= 0x8000;
        return false;
    }
    fillDiskEntry(u, host);
    diskFlags(u) |= DISK_OPENED;
    auto k = keep_.find(host);
    if (k != keep_.end()) { un.overlay = std::move(k->second); keep_.erase(k); }
    return true;
}

void AzCtrl::closeDisks() {
    for (auto& u : units_) {
        if (u.f) std::fclose(u.f);
        u.f = nullptr;
        u.overlay.clear();
        u.host.clear();
    }
}

void AzCtrl::close() {
    closeDisks();
    if (any_) { std::fclose(any_); any_ = nullptr; }
}

// Разбор AZ.INI — как azbk_boot_from_ini прошивки: [ROM] Rnn — слот ПЗУ (байт
// 0x40000 + nn·4 Кбайт), [LOGO] L — заставка (байт 0x20000), [DISKS] Dnn — образ,
// [ALTBOOT] B4/B6 — своя программа потоков загрузки. [BOOT] этой сборкой не читается:
// загрузочный диск выбирает AZBOOT по EEPROM. Строки без пробелов, регистр не важен.
void AzCtrl::loadIni() {
    // Блоки, записанные на образы «только чтение», переживают перезапуск контроллера:
    // это изменения диска, а не состояние МК.
    keep_.clear();
    for (auto& u : units_) if (!u.overlay.empty()) keep_[u.host] = std::move(u.overlay);
    std::fill(ram_.begin(), ram_.end(), 0);
    closeDisks();
    bootErrors_ = 0;
    bootUnit_ = 0x20;
    bootB4_.assign(std::begin(kBootB), std::end(kBootB));
    bootB6_ = bootB4_;
    logo_ = false;
    // Своя копия AZ.INI (её пишет Setup) — главнее карты, как и EEPROM.
    const std::string ini = readPath("AZ.INI");
    FILE* f = ini.empty() ? nullptr : std::fopen(ini.c_str(), "rb");
    std::map<int, std::string> disks;
    std::string altboot, altKey;
    if (f) {
        std::string section, line;
        int c;
        auto handle = [&](std::string s) {
            s.erase(std::remove_if(s.begin(), s.end(), [](char ch) { return ch == '\r' || ch == ' ' || ch == '\t'; }), s.end());
            if (s.empty() || s[0] == ';') return;
            if (s[0] == '[') { section = upper(s); return; }
            const size_t eq = s.find('=');
            const std::string key = upper(s.substr(0, eq));
            const std::string val = eq == std::string::npos ? std::string() : s.substr(eq + 1);
            if (section == "[ROM]" && key.size() >= 2 && key[0] == 'R' && eq != std::string::npos) {
                const int slot = std::atoi(key.c_str() + 1);
                const std::string p = resolve(cfg_.sdDir, val);
                FILE* r = p.empty() ? nullptr : std::fopen(p.c_str(), "rb");
                if (!r) { std::fprintf(stderr, "AZБК: нет ПЗУ %s (R%02d)\n", val.c_str(), slot); bootErrors_ |= 0x8000; return; }
                std::vector<uint8_t> data(0400000);
                const size_t n = std::fread(data.data(), 1, data.size(), r);
                std::fclose(r);
                if (memLoad && slot >= 0 && slot < 64) memLoad(0x40000u + uint32_t(slot) * 4096u, data.data(), n);
            } else if (section == "[LOGO]" && key == "L") {
                const std::string p = resolve(cfg_.sdDir, val);
                FILE* r = p.empty() ? nullptr : std::fopen(p.c_str(), "rb");
                if (!r) return;
                std::vector<uint8_t> data(0x40000);
                const size_t n = std::fread(data.data(), 1, data.size(), r);
                std::fclose(r);
                if (memLoad) memLoad(0x20000u, data.data(), n);
                logo_ = n > 0;
            } else if (section == "[DISKS]" && key.size() >= 2 && key[0] == 'D') {
                const int u = std::atoi(key.c_str() + 1);
                if (u >= 0 && u < MAX_DISKS) disks[u] = val;
            } else if (section == "[ALTBOOT]" && (key == "B4" || key == "B6") && altKey.empty()) {
                altKey = key;                      // прошивка берёт только первую запись
                altboot = val;
            }
        };
        while ((c = std::fgetc(f)) != EOF) {
            if (c == '\n') { handle(line); line.clear(); }
            else line += static_cast<char>(c);
        }
        handle(line);
        std::fclose(f);
    } else {
        std::fprintf(stderr, "AZБК: нет %s/AZ.INI\n", cfg_.sdDir.c_str());
        bootErrors_ |= 0x100;
    }
    for (const auto& kv : disks) {
        // Имена — как их нормализует прошивка: заглавные, «\» -> «/».
        std::string name = upper(kv.second);
        std::replace(name.begin(), name.end(), '\\', '/');
        if (name.empty() || name[0] != '0') continue;      // сетевые (landisk) — без сети нет
        openDisk(kv.first, readPath(kv.second), name);
    }
    for (const auto& kv : cfg_.units) {
        if (kv.first < 0 || kv.first >= MAX_DISKS) continue;
        openDisk(kv.first, kv.second, "0:/" + upper(fs::path(kv.second).filename().string()));
    }
    // [ALTBOOT]: файл {адрес, n, слова}; подменяет только свой поток. Последнее слово
    // программы прошивка заменяет номером загрузочного диска (на этот момент 0x20).
    // Нулевые слова прошивка выбрасывает и отключает ALTBOOT — тогда остаётся bootB.
    if (!altboot.empty()) {
        const std::string p = readPath(altboot);
        if (FILE* a = p.empty() ? nullptr : std::fopen(p.c_str(), "rb")) {
            std::vector<uint16_t> prog;
            uint8_t b[2];
            while (std::fread(b, 1, 2, a) == 2 && prog.size() < 0x406) prog.push_back(static_cast<uint16_t>(b[0] | (b[1] << 8)));
            std::fclose(a);
            bool ok = prog.size() >= 2 && prog[0] && prog[1] && 6u + 2u * prog[1] <= 0x80C && prog.size() >= 2u + prog[1];
            if (ok) {
                prog.resize(2u + prog[1]);
                ok = std::find(prog.begin() + 2, prog.end(), uint16_t(0)) == prog.end();
            }
            if (ok) {
                prog.back() = static_cast<uint16_t>(bootUnit_);
                (altKey == "B4" ? bootB4_ : bootB6_) = prog;
            }
        }
    }
    // Ошибки разбора AZ.INI — загрузчик AZLDR должен спросить диск (номер > 7).
    if (bootErrors_) bootUnit_ = 8;
}

bool AzCtrl::usable(const Config& c, std::string* err) {
    std::error_code ec;
    if (c.sdDir.empty() || !fs::is_directory(c.sdDir, ec)) {
        if (err) *err = "нет каталога карты SD «" + c.sdDir + "»";
        return false;
    }
    if (resolve(c.sdDir, "AZ.INI").empty() && (c.writeDir.empty() || resolve(c.writeDir, "AZ.INI").empty())) {
        if (err) *err = "в каталоге карты SD «" + c.sdDir + "» нет AZ.INI";
        return false;
    }
    return true;
}

bool AzCtrl::init(const Config& c, std::string* err) {
    cfg_ = c;
    const bool ok = usable(c, err);
    if (ok) reboot();
    return ok;
}

void AzCtrl::setUnit(int u, const std::string& host) {
    if (u < 0 || u >= MAX_DISKS) return;
    Unit& un = units_[u];
    if (un.f) std::fclose(un.f);
    un = Unit{};
    std::memset(&ram_[R_DISKS + u * DISK_WORDS], 0, DISK_WORDS * 2);
    if (host.empty()) { cfg_.units.erase(u); return; }
    cfg_.units[u] = host;
    openDisk(u, host, "0:/" + upper(fs::path(host).filename().string()));
}

void AzCtrl::reboot() {
    if (any_) { std::fclose(any_); any_ = nullptr; }
    loadIni();
    resetRegs();
    csr_ = 0200; status_ = 0; dataWr_ = 0;
    unit_ = 0; block_ = 0; blkHiPending_ = false;
    xferBuf_ = R_BLK; xferLen_ = 0x100;
    bootAddr_ = 0; bootPos_ = 0;
    dirOpen_ = false; dirList_.clear(); dirHost_.clear(); dirPath_.clear(); dirPos_ = 0;
    anyPos_ = 0; anyHost_.clear(); anyWrite_ = false;
    lastCmd_ = -1;
}

bool AzCtrl::unitMounted() {
    if (unit_ < 0 || unit_ >= MAX_DISKS || !(diskFlags(unit_) & DISK_MOUNTED)) {
        status_ = 0x8C00;
        return false;
    }
    return true;
}

// ---- Регистры -----------------------------------------------------------------

uint16_t AzCtrl::read(int reg, bool peek) {
    // Отладчику — без побочных эффектов: автоматы прогоняются на копии.
    if (peek) {
        const Reg saved[4] = {regs_[0], regs_[1], regs_[2], regs_[3]};
        const uint32_t c = csr_, a = bootAddr_, bp = bootPos_, sz = sizeTmp_;
        const uint16_t v = read(reg, false);
        for (int i = 0; i < 4; ++i) regs_[i] = saved[i];
        csr_ = c; bootAddr_ = a; bootPos_ = bp; sizeTmp_ = sz;
        return v;
    }
    int64_t v = 0;
    bool streamEnd = false;
    Reg& r = regs_[reg & 3];
    auto b = [&](uint32_t i) -> uint16_t { return i < bootB4_.size() ? bootB4_[i] : 0; };
    auto b6 = [&](uint32_t i) -> uint16_t { return i < bootB6_.size() ? bootB6_[i] : 0; };
    switch (reg & 3) {
    case 0:                                   // CSR; в цепочке BOOT1 — команды
        switch (r.rd) {
        case 0:    v = csr_; break;
        case 1:    r.rd = 2933; v = 012700; break;            // MOV #адрес,R0
        case 2933:
        case 2935: r.rd = 2935; v = 012720; break;            // MOV #слово,(R0)+
        case 2:    r.rd = 2938; v = 012720; break;            // последнее слово — номер диска
        case 2938: r.rd = 2939; v = 012700; break;            // R0 = номер диска
        case 2939: r.rd = 0; regs_[2].rd = 0; v = 0137; break; // JMP @#адрес
        default:   v = 0; break;
        }
        break;
    case 1: {                                 // DR
        Reg& s = regs_[2];                    // поток B4 держит указатель в регистре 2
        switch (r.rd) {
        case 0: v = status_; break;
        case 1:
            s.buf = 0;
            bootAddr_ = b(s.buf++);
            s.count = b(s.buf++);
            r.rd = 2961;
            v = bootAddr_;
            break;
        case 2961:
        streamWord:
            if (s.count == 1) regs_[0].rd = 2;
            r.rd = 2967;
            v = b(s.buf++);
            break;
        case 2967:
            if (--s.count > 0) goto streamWord;
            r.rd = 2970; v = bootUnit_ & 0377; break;
        case 2970: r.rd = 2971; v = bootUnit_ & 0377; break;
        case 2971: r.rd = 0; v = bootAddr_; break;
        case 2: {                             // поток из буфера, заданного командой
            const uint16_t x = w(r.buf++);
            v = x;
            if (--r.count < 0) { r.rd = 0; streamEnd = true; }   // за концом — разряд ошибки
            break;
        }
        case 3:                               // размер диска, 16 разрядов
            r.rd = 0;
            sizeTmp_ = (unit_ >= 0 && unit_ < MAX_DISKS) ? diskBlocks(unit_) : 0;
            if (sizeTmp_ >= 0x10000) { sizeTmp_ = 0xFFFF; csr_ |= 0x4000; }
            v = sizeTmp_;
            break;
        case 4:                               // 32 разряда
            sizeTmp_ = (unit_ >= 0 && unit_ < MAX_DISKS) ? diskBlocks(unit_) : 0;
            r.rd = 2992; v = sizeTmp_ & 0xFFFF; break;
        case 2992: r.rd = 0; v = sizeTmp_ >> 16; break;
        case 5:    r.rd = 2996; v = bootErrors_ & 0xFFFF; break;
        case 2996: r.rd = 0; v = bootErrors_ >> 16; break;
        case 6:    v = w(R_BLK); break;
        case 7:    r.rd = 3003; v = 0x1904; break;            // прошивка 19, плата AZ БК (тип 4)
        case 3003: r.rd = 3004; v = 0x1F; break;              // последний номер диска
        default:   v = 0; break;
        }
        break;
    }
    case 2:                                   // BOOT1: BR . три раза, MTPS #340, BR .-4
        switch (r.rd) {
        case 0:    r.rd = 2914; v = 0777; break;
        case 2914: r.rd = 2915; v = 0777; break;
        case 2915: r.rd = 2916; v = 0777; break;
        case 2916: r.rd = 2917; v = 0106447; break;
        case 2917: r.rd = 2918; v = 0340; break;
        case 2918: regs_[0].rd = 1; regs_[1].rd = 1; [[fallthrough]];
        case 2922: r.rd = 2922; v = 0775; break;
        default:   v = 0; break;
        }
        break;
    case 3:                                   // BOOT2: самостоятельный поток через -(PC)
        switch (r.rd) {
        case 0:
            bootPos_ = 0;
            bootAddr_ = b6(bootPos_++);
            r.count = b6(bootPos_++);
            r.rd = 2886; v = 0106447; break;                  // MTPS -(PC)
        case 2886: r.rd = 2887; v = 0340; break;
        case 2887: r.rd = 2888; v = 014700; break;            // MOV -(PC),R0
        case 2888: r.rd = 2889; v = bootAddr_; break;
        case 2889:
        copyNext:
            r.rd = 2891; v = 014720; break;                   // MOV -(PC),(R0)+
        case 2891: r.rd = 2892; v = b6(bootPos_++); break;
        case 2892:
            if (--r.count > 0) goto copyNext;
            r.rd = 2895; v = 014720; break;
        case 2895: r.rd = 2896; v = bootUnit_ & 0377; break;
        case 2896: r.rd = 2897; v = 014700; break;
        case 2897: r.rd = 2898; v = bootUnit_ & 0377; break;
        case 2898: r.rd = 2899; v = 014707; break;            // MOV -(PC),PC
        case 2899: r.rd = 0; v = bootAddr_; break;
        default:   v = 0; break;
        }
        break;
    }
    if (streamEnd) csr_ |= 0x8000;
    return static_cast<uint16_t>(v);
}

void AzCtrl::write(int reg, uint16_t v) {
    int32_t res = 0;
    switch (reg & 3) {
    case 0: res = command(v); break;
    case 1: {
        Reg& r = regs_[1];
        if (r.wr == 0) { dataWr_ = v; break; }
        if (r.wr != 2) { res = -1; break; }
        w(r.buf++) = v;
        if (--r.count < 0) res = -1;
        break;
    }
    case 2: res = -1; break;
    default: break;                            // у регистра 3 обработчика записи нет
    }
    if (res < 0) csr_ |= 0x8000;
}

// Запись в CSR: код команды — младшие 6 разрядов, номера команд прошивки десятичные
// (в документации — восьмеричные, в скобках).
int32_t AzCtrl::command(uint16_t v) {
    const int cmd = v & 077;
    const bool ie = (v & 0100) != 0;
    lastCmd_ = cmd;
    ++ncmd_;
    csr_ = 0200;
    int j = 0;
    int32_t res = 0;
    switch (cmd) {
    case 0: resetRegs(); return 0;
    case 1:                                    // (001) выбор диска
        unit_ = static_cast<int>(dataWr_ & 0xFFFF);
        if (unit_ < 0 || unit_ >= MAX_DISKS) { status_ = 0x8600; return -1; }
        return unitMounted() ? 0 : -1;
    case 2:                                    // (002) номер блока, младшие 16
        if (!unitMounted()) return -1;
        block_ = dataWr_ & 0xFFFF;
        if (diskBlocks(unit_) < block_) { status_ = 0x8D00; return -1; }
        blkHiPending_ = true;
        return 0;
    case 10:                                   // (012) старшие 16
        if (!unitMounted()) return -1;
        if (!blkHiPending_) return -1;
        block_ |= (dataWr_ & 0xFFFF) << 16;
        if (diskBlocks(unit_) < block_) { status_ = 0x8D00; return -1; }
        blkHiPending_ = false;
        return 0;
    case 3:  j = 3; blkHiPending_ = false; break;                    // (003) открыть каталог
    case 11:                                                         // (013) запись каталога
        if (!dirOpen_) { status_ = 0x8B00; return -1; }
        j = 11; blkHiPending_ = false;
        xferBuf_ = R_FNO; xferLen_ = 12;
        break;
    case 4:  j = 4; break;                                           // (004) смонтировать
    case 12: j = 12; break;                                          // (014) размонтировать
    case 5:                                                          // (005) чтение блока
        if (!unitMounted()) return -1;
        j = 5; xferBuf_ = R_BLK; xferLen_ = 0x100; blkHiPending_ = false;
        break;
    case 6:                                                          // (006) запись блока
        if (!unitMounted()) return -1;
        j = 6; blkHiPending_ = false;
        break;
    case 7:  regs_[1].rd = 3; return 0;                              // (007) размер, 16
    case 15: regs_[1].rd = 4; return 0;                              // (017) размер, 32
    case 16: regs_[1].rd = 5; return 0;                              // (020) ошибки загрузки
    case 23: regs_[1].rd = 7; return 0;                              // (027) версия
    case 8:  resetRegs(); return 0;                                  // (010) сеть: без кабеля — ничего
    case 9:  xferBuf_ = R_DISKS; xferLen_ = 0x18C0; return 0;        // (011) таблица дисков
    case 13: streamOut(xferBuf_, static_cast<int32_t>(xferLen_)); return 0;   // (015)
    case 14: streamIn(R_BLK, 0x100); return 0;                       // (016)
    case 17: resetRegs(); j = 17; break;                             // (021) EEPROM -> буфер
    case 18: streamOut(R_XBUF, 0x100); return 0;                     // (022)
    case 19: streamIn(R_XBUF, 0x100); return 0;                      // (023)
    case 20: resetRegs(); j = 20; break;                             // (024) буфер -> EEPROM
    case 21: case 22: resetRegs(); j = cmd; break;                   // (025/026) Hall of Fame
    case 24: return 0;                                               // (030) NOP: важен разряд IE
    case 25: case 34: resetRegs(); j = 25; break;                    // (031/042) время в буфер
    case 26: streamOut(R_TIME, 14); return 0;                        // (032)
    case 27: streamIn(R_SETTIME, 7); return 0;                       // (033)
    case 28: resetRegs(); j = 28; break;                             // (034) установить часы
    case 29: case 30: resetRegs(); j = cmd; break;                   // (035/036) NTP — нет сети, но долгая

    case 31:                                                         // (037) перезапуск AZ и БК
        resetRegs();
        if (resetMachine) resetMachine();
        return 0;
    case 32: case 35:                                                // (040/043) IP, MAC — нули
        resetRegs();
        for (uint32_t i = 0; i < 12; ++i) w(R_NET + i) = 0;
        j = cmd;
        break;
    case 33: streamOut(R_NET, 12); return 0;                         // (041)
    case 36: resetRegs(); j = 36; break;                             // (044) снимок экрана
    case 39: resetRegs(); j = 39; break;                             // (047) файл в память AZ
    case 40: resetRegs(); j = 40; break;                             // (050) открыть на чтение
    case 41: streamOut(R_ANYSIZE, 2); return 0;                      // (051)
    case 42: resetRegs(); j = 42; break;                             // (052) блок файла
    case 43: resetRegs(); j = 43; break;                             // (053) открыть на запись
    case 44: streamIn(R_ANYSIZE, 2); return 0;                       // (054)
    case 45: resetRegs(); j = 45; break;                             // (055) записать блок
    case 46: resetRegs(); j = 46; break;                             // (056) размер карты
    case 47: streamOut(R_CARD, 2); return 0;                         // (057)
    default: return -1;                                              // у прошивки — фатальная ошибка 5
    }
    res = job(j);
    if (ie && irq) irq();
    return res;
}

int32_t AzCtrl::job(int j) {
    switch (j) {
    case 5:  hdRead(); break;
    case 6:  hdWrite(); break;
    case 3:  status_ = static_cast<uint32_t>(dirOpen()); break;
    case 11: status_ = static_cast<uint32_t>(dirRead()); break;
    case 4:  status_ = static_cast<uint32_t>(mount()); break;
    case 12: status_ = static_cast<uint32_t>(dismount()); break;
    case 17: eepromRead(); break;
    case 20: eepromWrite(); break;
    case 21: case 22: {
        static const char kJson[] = "{\"RESULT\":\"ERROR\",\"DESCRIPTION\":\"CONNECTION_ERROR\"}";
        for (size_t i = 0; i < sizeof kJson; ++i) {
            uint16_t& x = w(R_XBUF + static_cast<uint32_t>(i / 2));
            x = (i & 1) ? static_cast<uint16_t>((x & 0377) | (uint8_t(kJson[i]) << 8))
                        : static_cast<uint16_t>((x & 0177400) | uint8_t(kJson[i]));
        }
        break;
    }
    case 25: timeRecord(); break;
    case 28: setTime(); break;
    case 36: screenshot(); break;
    case 39: fileToMem(); break;
    case 40: fileOpenRead(); break;
    case 42: fileRead(); break;
    case 43: fileOpenWrite(); break;
    case 45: fileWrite(); break;
    case 46: cardSize(); break;
    default: break;
    }
    return 0;
}

// ---- Диски --------------------------------------------------------------------

void AzCtrl::hdRead() {
    Unit& u = units_[unit_];
    uint8_t* dst = bytes(R_BLK);
    auto ov = u.overlay.find(block_);
    if (ov != u.overlay.end()) { std::memcpy(dst, ov->second.data(), 512); return; }
    if (!u.f) { status_ = 0x8C00; csr_ |= 0x8000; return; }
    if (std::fseek(u.f, static_cast<long>(block_) * 512, SEEK_SET) == 0) {
        const size_t n = std::fread(dst, 1, 512, u.f);
        (void)n;                               // короче блока — остаётся прежнее, как в прошивке
    }
}

void AzCtrl::hdWrite() {
    Reg& r = regs_[1];
    if (r.wr != 2) { csr_ |= 0x8000; return; }   // у прошивки — фатальная ошибка 8
    for (int32_t n = r.count; n > 0; --n) w(r.buf++) = 0;   // БК прислала меньше 256 слов
    Unit& u = units_[unit_];
    const uint8_t* src = bytes(R_BLK);
    if (u.f && u.rw) {
        if (std::fseek(u.f, static_cast<long>(block_) * 512, SEEK_SET) == 0) {
            std::fwrite(src, 1, 512, u.f);
            std::fflush(u.f);
        }
    } else {
        u.overlay[block_].assign(src, src + 512);
    }
    resetRegs();
}

int32_t AzCtrl::dirOpen() {
    Reg& r = regs_[1];
    if (r.wr != 2) return 0x8100;
    for (int32_t n = r.count; n > 0; --n) w(r.buf++) = 0;
    const std::string name = cstr(R_BLK, 512);
    std::string host = name.empty() || name == "0:" || name == "0:/" ? cfg_.sdDir : resolve(cfg_.sdDir, name);
    std::error_code ec;
    if (host.empty() || !fs::is_directory(host, ec)) return 0x8200 | 5;   // FR_NO_PATH
    listDir(host);
    return 0;
}

void AzCtrl::listDir(const std::string& host) {
    dirList_.clear(); dirHost_.clear(); dirPos_ = 0;
    std::error_code ec;
    std::vector<std::pair<std::string, std::string>> ents;
    for (const auto& e : fs::directory_iterator(host, ec)) ents.push_back({upper(e.path().filename().string()), e.path().string()});
    std::sort(ents.begin(), ents.end());
    for (auto& e : ents) { dirList_.push_back(e.first); dirHost_.push_back(e.second); }
    dirPath_ = host;
    dirOpen_ = true;
}

int32_t AzCtrl::dirRead() {
    for (uint32_t i = 0; i < 12; ++i) w(R_FNO + i) = 0;
    if (dirPos_ >= dirList_.size()) return 0;     // конец каталога: пустое имя
    uint16_t d, t; uint32_t size; bool dir;
    fatStamp(dirHost_[dirPos_], d, t, size, dir);
    w(R_FNO + 0) = static_cast<uint16_t>(size); w(R_FNO + 1) = static_cast<uint16_t>(size >> 16);
    w(R_FNO + 2) = d; w(R_FNO + 3) = t;
    uint8_t* p = bytes(R_FNO);
    p[8] = dir ? 0x10 : 0x20;
    const std::string& n = dirList_[dirPos_];
    for (size_t i = 0; i < 12 && i < n.size(); ++i) p[9 + i] = static_cast<uint8_t>(n[i]);
    ++dirPos_;
    return 0;
}

// «Dnn=путь» в буфере блока — тот же синтаксис, что в [DISKS].
int32_t AzCtrl::mount() {
    const std::string s = upper(cstr(R_BLK, 512));
    if (s.size() < 1 || s[0] != 'D') return 0x80008500;
    if (s.size() < 3 || s[1] < '0' || s[1] > '3' || s[2] < '0' || s[2] > '9') return 0x80008600;
    const int u = (s[1] - '0') * 10 + (s[2] - '0');
    if (u >= MAX_DISKS) return 0x80008600;
    if (diskFlags(u) & DISK_OPENED) return 0x80008900;
    if (s.size() < 4 || s[3] != '=') return 0x80008700;
    std::string name = s.substr(4);
    if (name.size() >= 0x180) return 0x80008800;
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name[0] != '0') return 0x80000000;   // сетевой диск — нет сети
    const std::string host = readPath(name);
    if (host.empty()) {
        std::strncpy(diskName(u), name.c_str(), NAME_BYTES - 1);
        diskFlags(u) = DISK_ERROR;
        return 0x80008A00;
    }
    diskFlags(u) = 0;
    return openDisk(u, host, name) ? 0 : int32_t(0x80008B00);
}

int32_t AzCtrl::dismount() {
    const std::string s = upper(cstr(R_BLK, 512));
    if (s.size() < 1 || s[0] != 'D') return 0x8500;
    if (s.size() < 3 || s[1] < '0' || s[1] > '3' || s[2] < '0' || s[2] > '9') return 0x8600;
    const int u = (s[1] - '0') * 10 + (s[2] - '0');
    if (u >= MAX_DISKS) return 0x8600;
    if (!(diskFlags(u) & DISK_OPENED)) return 0xC00;
    Unit& un = units_[u];
    if (un.f) std::fclose(un.f);
    un = Unit{};
    std::memset(&ram_[R_DISKS + u * DISK_WORDS], 0, DISK_WORDS * 2);
    return 0;
}

// ---- EEPROM, часы, файлы --------------------------------------------------------

// EEPROM — 510 байт (запись BKPSRAM прошивки, eeprom.dat у GID). Чтение кладёт их за
// словом результата (0 — прочитано), запись берёт буфер с самого начала — так у прошивки.
void AzCtrl::eepromRead() {
    for (uint32_t i = 0; i < 0x100; ++i) w(R_XBUF + i) = 0;
    const std::string p = readPath("EEPROM.DAT");
    FILE* f = p.empty() ? nullptr : std::fopen(p.c_str(), "rb");
    if (!f) { w(R_XBUF) = 1; return; }
    const size_t n = std::fread(bytes(R_XBUF + 1), 1, 0x1FE, f);
    std::fclose(f);
    w(R_XBUF) = n == 0x1FE ? 0 : 1;
}

void AzCtrl::eepromWrite() {
    const std::string p = writePath("EEPROM.DAT");
    FILE* f = p.empty() ? nullptr : std::fopen(p.c_str(), "wb");
    if (!f) return;
    std::fwrite(bytes(R_XBUF), 1, 0x1FE, f);
    std::fclose(f);
}

// Запись времени (14 слов): дата RT-11, тики 50 и 60 Гц от полуночи, дата и время FAT,
// год, месяц, день, день недели, часы, минуты, секунды.
void AzCtrl::timeRecord() {
    const time_t t = std::time(nullptr) + static_cast<time_t>(timeOffset_);
    std::tm tm {};
    localtime_r(&t, &tm);
    const int yy = (tm.tm_year + 1900) % 100;
    const int y = yy + 28;
    const uint32_t sec = static_cast<uint32_t>(tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
    const uint32_t t50 = sec * 50, t60 = sec * 60;
    uint16_t* r = &ram_[R_TIME];
    r[0] = static_cast<uint16_t>(((y / 32) << 14) | ((tm.tm_mon + 1) << 10) | (tm.tm_mday << 5) | (y % 32));
    r[1] = static_cast<uint16_t>(t50 >> 16); r[2] = static_cast<uint16_t>(t50);
    r[3] = static_cast<uint16_t>(t60 >> 16); r[4] = static_cast<uint16_t>(t60);
    r[5] = static_cast<uint16_t>(tm.tm_mday | ((tm.tm_mon + 1) << 5) | ((yy + 20) << 9));
    r[6] = static_cast<uint16_t>((tm.tm_sec / 2) | (tm.tm_min << 5) | (tm.tm_hour << 11));
    r[7] = static_cast<uint16_t>(yy + 2000);
    r[8] = static_cast<uint16_t>(tm.tm_mon + 1);
    r[9] = static_cast<uint16_t>(tm.tm_mday);
    r[10] = static_cast<uint16_t>(tm.tm_wday);
    r[11] = static_cast<uint16_t>(tm.tm_hour);
    r[12] = static_cast<uint16_t>(tm.tm_min);
    r[13] = static_cast<uint16_t>(tm.tm_sec);
}

// {год (две цифры), месяц, день, день недели, часы, минуты, секунды} — часы AZ от хоста
// не отделить, поэтому запоминаем смещение.
void AzCtrl::setTime() {
    const uint16_t* s = &ram_[R_SETTIME];
    std::tm tm {};
    const int year = s[0] > 1900 ? s[0] : 2000 + (s[0] % 100);
    tm.tm_year = year - 1900; tm.tm_mon = (s[1] & 15) - 1; tm.tm_mday = s[2] & 31;
    tm.tm_hour = s[4] % 24; tm.tm_min = s[5] % 60; tm.tm_sec = s[6] % 60;
    tm.tm_isdst = -1;
    const time_t want = std::mktime(&tm);
    if (want != static_cast<time_t>(-1)) timeOffset_ = static_cast<int64_t>(want - std::time(nullptr));
}

void AzCtrl::fileOpenRead() {
    if (any_) { std::fclose(any_); any_ = nullptr; }
    const std::string name = cstr(R_XBUF, 256);
    anyPos_ = 0;
    const std::string p = readPath(name);
    any_ = p.empty() || fs::is_directory(p) ? nullptr : std::fopen(p.c_str(), "rb");
    anyHost_ = any_ ? p : std::string();
    anyWrite_ = false;
    uint32_t size = 0;
    if (any_) { std::fseek(any_, 0, SEEK_END); size = static_cast<uint32_t>(std::ftell(any_)); }
    w(R_ANYSIZE) = static_cast<uint16_t>(size); w(R_ANYSIZE + 1) = static_cast<uint16_t>(size >> 16);
}

void AzCtrl::fileRead() {
    const uint32_t size = w(R_ANYSIZE) | (uint32_t(w(R_ANYSIZE + 1)) << 16);
    uint32_t n = size - anyPos_;
    if (size >= anyPos_ && n >= 0x200) n = 0x200;
    else { n = size > anyPos_ ? n : 0; for (uint32_t i = 0; i < 0x100; ++i) w(R_XBUF + i) = 0; }
    size_t br = 0;
    if (any_ && n && std::fseek(any_, static_cast<long>(anyPos_), SEEK_SET) == 0) br = std::fread(bytes(R_XBUF), 1, n, any_);
    anyPos_ += static_cast<uint32_t>(br);
    if (anyPos_ >= size && any_) { std::fclose(any_); any_ = nullptr; }
}

void AzCtrl::fileOpenWrite() {
    if (any_) { std::fclose(any_); any_ = nullptr; }
    const std::string p = writePath(cstr(R_XBUF, 256));
    anyPos_ = 0;
    any_ = p.empty() ? nullptr : std::fopen(p.c_str(), "wb");
    anyHost_ = any_ ? p : std::string();
    anyWrite_ = true;
    w(R_ANYSIZE) = 0; w(R_ANYSIZE + 1) = 0;
}

void AzCtrl::fileWrite() {
    const uint32_t size = w(R_ANYSIZE) | (uint32_t(w(R_ANYSIZE + 1)) << 16);
    const uint32_t n = size > anyPos_ ? std::min<uint32_t>(0x200, size - anyPos_) : 0;
    size_t bw = 0;
    if (any_ && n) bw = std::fwrite(bytes(R_XBUF), 1, n, any_);
    anyPos_ += static_cast<uint32_t>(bw);
    if (anyPos_ >= size && any_) { std::fclose(any_); any_ = nullptr; }
}

// Файл, открытый командой 050, — целиком в память AZ. Адрес — как у прошивки:
// (слово0 + слово1) << 16 (слов), не ниже 1 Мбайт и не выше 8 Мслов; код — в слово 0.
void AzCtrl::fileToMem() {
    uint32_t waddr = (uint32_t(w(R_XBUF)) + w(R_XBUF + 1)) << 16;
    const uint32_t size = w(R_ANYSIZE) | (uint32_t(w(R_ANYSIZE + 1)) << 16);
    if (waddr < 0x100000 || waddr > 0x800000) { w(R_XBUF) = 1; return; }
    if (size == 0 || size + waddr > 0x800000 || !any_) { w(R_XBUF) = 2; return; }
    std::vector<uint8_t> data(size);
    std::fseek(any_, static_cast<long>(anyPos_), SEEK_SET);
    const size_t n = std::fread(data.data(), 1, size, any_);
    std::fclose(any_); any_ = nullptr;
    if (n < (size & ~0777u)) { w(R_XBUF) = 3; return; }
    for (uint32_t i = 0; i < n; i += 2) {
        const uint16_t x = static_cast<uint16_t>(data[i] | ((i + 1 < n ? data[i + 1] : 0) << 8));
        if (memWrite) memWrite(waddr++, x);
    }
    w(R_XBUF) = 0;
}

void AzCtrl::cardSize() {
    std::error_code ec;
    const auto sp = fs::space(cfg_.sdDir, ec);
    w(R_CARD) = ec ? 0 : static_cast<uint16_t>(sp.capacity >> 20);
    w(R_CARD + 1) = ec ? 0 : static_cast<uint16_t>(sp.available >> 20);
}

// Снимок экрана .SCR: блок теневых регистров видео (слово 0x1F000 AZ) и палитры
// (0x1F100) — их кладёт screenshotPrepare, — затем видеопамять (screenshot.c).
void AzCtrl::screenshot() {
    if (!memRead) return;
    if (screenshotPrepare) screenshotPrepare();
    std::string name = cstr(R_XBUF, 256);
    if (name.size() < 10) {
        const time_t t = std::time(nullptr) + static_cast<time_t>(timeOffset_);
        std::tm tm {};
        localtime_r(&t, &tm);
        char b[40];
        std::snprintf(b, sizeof b, "0:/SCREENS/%02d%02d%02d%02d.SCR", tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        name = b;
    }
    const std::string p = writePath(name);
    FILE* f = p.empty() ? nullptr : std::fopen(p.c_str(), "wb");
    if (!f) return;
    auto dump = [&](uint32_t wa, uint32_t n) {
        std::vector<uint8_t> b(n * 2);
        for (uint32_t i = 0; i < n; ++i) { const uint16_t x = memRead(wa + i); b[2 * i] = uint8_t(x); b[2 * i + 1] = uint8_t(x >> 8); }
        std::fwrite(b.data(), 1, b.size(), f);
    };
    std::vector<uint16_t> h(256);
    for (uint32_t i = 0; i < 256; ++i) h[i] = memRead(0x1F000 + i);
    dump(0x1F000, 256);
    static const uint32_t roll[12] = {0x1800, 0x2000, 0x3000, 0x4000, 0x6000, 0x8000, 0xC000, 0x10000, 0x18000, 0x20000, 0x30000, 0x40000};
    const uint16_t mode = h[29];
    const uint32_t size = (mode >> 12) < 12 ? roll[mode >> 12] : 0x2000;
    dump(0x1F100, 256);
    dump(uint32_t(h[30]) << 11, size);
    if (mode & 4) { dump(uint32_t(h[31]) << 11, size); dump(uint32_t(h[32]) << 11, size); }
    std::fclose(f);
}

// ---- Отладка и снимок ------------------------------------------------------------

std::vector<AzCtrl::UnitInfo> AzCtrl::units() const {
    std::vector<UnitInfo> v;
    for (int u = 0; u < MAX_DISKS; ++u) {
        if (!(diskFlags(u) & (DISK_MOUNTED | DISK_ERROR))) continue;
        v.push_back({u, diskNameStr(u), units_[u].host, diskBlocks(u), units_[u].f != nullptr,
                     units_[u].rw, !units_[u].overlay.empty()});
    }
    return v;
}

std::string AzCtrl::summary() const {
    char b[160];
    std::snprintf(b, sizeof b, "CSR=%06o DR-сост=%d диск=%d блок=%u посл.команда=%03o (%llu команд)",
                  csr_, regs_[1].rd, unit_, block_, lastCmd_ < 0 ? 0 : lastCmd_,
                  static_cast<unsigned long long>(ncmd_));
    return b;
}

namespace {
template <class T> void putv(std::vector<uint8_t>& o, const T& v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    o.insert(o.end(), p, p + sizeof v);
}
template <class T> bool getv(const uint8_t*& p, const uint8_t* e, T& v) {
    if (static_cast<size_t>(e - p) < sizeof v) return false;
    std::memcpy(&v, p, sizeof v); p += sizeof v; return true;
}
}

void AzCtrl::saveState(std::vector<uint8_t>& out) const {
    auto putStr = [&](const std::string& t) { putv(out, uint32_t(t.size())); out.insert(out.end(), t.begin(), t.end()); };
    out.insert(out.end(), reinterpret_cast<const uint8_t*>(ram_.data()),
               reinterpret_cast<const uint8_t*>(ram_.data() + ram_.size()));
    for (const Reg& r : regs_) { putv(out, int32_t(r.rd)); putv(out, int32_t(r.wr)); putv(out, r.buf); putv(out, r.count); }
    putv(out, csr_); putv(out, status_); putv(out, dataWr_); putv(out, int32_t(unit_)); putv(out, block_);
    putv(out, uint8_t(blkHiPending_)); putv(out, xferBuf_); putv(out, xferLen_); putv(out, bootErrors_);
    putv(out, bootUnit_); putv(out, bootAddr_); putv(out, bootPos_); putv(out, sizeTmp_); putv(out, timeOffset_);
    // Открытые образы (в том числе смонтированные командой 004 — их нет в конфигурации).
    uint8_t nu = 0;
    for (const Unit& un : units_) nu += un.f ? 1 : 0;
    putv(out, nu);
    for (int i = 0; i < MAX_DISKS; ++i)
        if (units_[i].f) { putv(out, uint8_t(i)); putStr(units_[i].host); putv(out, uint8_t(units_[i].rw)); }
    // Блоки, записанные на образы «только чтение», — часть состояния машины.
    uint32_t n = 0;
    for (const Unit& u : units_) n += static_cast<uint32_t>(u.overlay.size());
    putv(out, n);
    for (int i = 0; i < MAX_DISKS; ++i)
        for (const auto& kv : units_[i].overlay) {
            putv(out, uint8_t(i)); putv(out, kv.first);
            out.insert(out.end(), kv.second.begin(), kv.second.end());
        }
    // Открытый каталог (003/013) и файл (050–055).
    putv(out, uint8_t(dirOpen_)); putStr(dirPath_); putv(out, uint32_t(dirPos_));
    putv(out, uint8_t(any_ != nullptr)); putStr(anyHost_); putv(out, uint8_t(anyWrite_)); putv(out, anyPos_);
}

bool AzCtrl::reopenFile(int u, const std::string& host, bool rw) {
    Unit& un = units_[u];
    if (un.f) std::fclose(un.f);
    un.f = nullptr;
    un.host = host;
    un.rw = rw && cfg_.rw;
    un.f = std::fopen(host.c_str(), un.rw ? "r+b" : "rb");
    if (!un.f && un.rw) { un.rw = false; un.f = std::fopen(host.c_str(), "rb"); }
    return un.f != nullptr;
}

bool AzCtrl::loadState(const uint8_t*& p, const uint8_t* end) {
    auto getStr = [&](std::string& t) {
        uint32_t len;
        if (!getv(p, end, len) || static_cast<uint32_t>(end - p) < len) return false;
        t.assign(reinterpret_cast<const char*>(p), len); p += len;
        return true;
    };
    const size_t rb = ram_.size() * 2;
    if (static_cast<size_t>(end - p) < rb) return false;
    std::memcpy(ram_.data(), p, rb); p += rb;
    for (Reg& r : regs_) {
        int32_t rd, wr;
        if (!getv(p, end, rd) || !getv(p, end, wr) || !getv(p, end, r.buf) || !getv(p, end, r.count)) return false;
        r.rd = rd; r.wr = wr;
    }
    int32_t u; uint8_t hi;
    if (!getv(p, end, csr_) || !getv(p, end, status_) || !getv(p, end, dataWr_) || !getv(p, end, u)
        || !getv(p, end, block_) || !getv(p, end, hi) || !getv(p, end, xferBuf_) || !getv(p, end, xferLen_)
        || !getv(p, end, bootErrors_) || !getv(p, end, bootUnit_) || !getv(p, end, bootAddr_)
        || !getv(p, end, bootPos_) || !getv(p, end, sizeTmp_) || !getv(p, end, timeOffset_)) return false;
    unit_ = u; blkHiPending_ = hi != 0;
    // Образы — как в снимке: закрыть все, открыть сохранённые (файл мог пропасть —
    // тогда диск числится, но чтение даст ошибку, как на плате с вынутым образом).
    for (Unit& un : units_) { if (un.f) std::fclose(un.f); un = Unit{}; }
    uint8_t nu;
    if (!getv(p, end, nu)) return false;
    for (uint8_t i = 0; i < nu; ++i) {
        uint8_t d, rw; std::string host;
        if (!getv(p, end, d) || !getStr(host) || !getv(p, end, rw)) return false;
        if (d < MAX_DISKS && !reopenFile(d, host, rw != 0))
            std::fprintf(stderr, "AZБК: снимок — образ AZ%d (%s) не открывается\n", int(d), host.c_str());
    }
    uint32_t n;
    if (!getv(p, end, n)) return false;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t d; uint32_t blk;
        if (!getv(p, end, d) || !getv(p, end, blk) || end - p < 512) return false;
        units_[d % MAX_DISKS].overlay[blk].assign(p, p + 512);
        p += 512;
    }
    uint8_t dirOn, anyOn, anyW; uint32_t dpos; std::string dpath, apath;
    if (!getv(p, end, dirOn) || !getStr(dpath) || !getv(p, end, dpos) || !getv(p, end, anyOn) || !getStr(apath)
        || !getv(p, end, anyW) || !getv(p, end, anyPos_)) return false;
    dirOpen_ = false; dirList_.clear(); dirHost_.clear(); dirPath_.clear(); dirPos_ = 0;
    if (dirOn && !dpath.empty()) { listDir(dpath); dirPos_ = std::min<size_t>(dpos, dirList_.size()); }
    if (any_) { std::fclose(any_); any_ = nullptr; }
    anyHost_.clear(); anyWrite_ = anyW != 0;
    if (anyOn && !apath.empty()) {
        // Записываемый файл уже начат — дописываем, а не создаём заново.
        any_ = std::fopen(apath.c_str(), anyWrite_ ? "r+b" : "rb");
        if (any_) anyHost_ = apath;
    }
    return true;
}

} // namespace bk
