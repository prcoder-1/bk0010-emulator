#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "AzNet.h"

namespace bk {

// Контроллер AZБК со стороны STM32: регистры 0177220 (CSR), 0177222 (DR), 0177224 (BOOT1),
// 0177226 (BOOT2) и «карта SD» — каталог хоста с AZ.INI. Семантика команд — по разбору
// прошивки 00019 (RE_ARM/decompiled/src/io11.c и соседние файлы): у каждого регистра
// автоматы чтения и записи, данные ходят потоками по явным командам (015/016, 022/023…),
// по чтению DR вне потока — результат последней «длинной» команды, ошибка — разряд 15 CSR.
// Долгие команды (диск, каталоги, EEPROM, часы, файлы) исполняются мгновенно, как у GID;
// по их окончании при разряде IE (6) в записанном CSR — прерывание по вектору 0174.
// Команды, которые в прошивке ждут сеть (010 с IE, landisk, HOF), идут в эмулированном
// времени задачей МК (AzNet): CSR всё это время «занят». См. docs/azbk.md.
class AzCtrl {
public:
    struct Config {
        std::string sdDir;                   // корень «карты SD»: AZ.INI, ROM/, DISKS/
        std::string writeDir;                // куда идёт то, что пишет БК (EEPROM, файлы)
        bool rw = false;                     // образы дисков на запись (иначе — в памяти)
        std::set<int> rwUnits;               // эти AZn на запись и без rw (запись карты — как без rw)
        std::map<int, std::string> units;    // AZn поверх [DISKS] (путь хоста)
        std::string net;                     // кабель: tap:ИМЯ | macvtap:ИМЯ | /dev/tapN, пусто — нет
    };
    // Что контроллер делает с памятью AZ при загрузке (ПЗУ в слоты, логотип) и снимке.
    std::function<void(uint32_t byteAddr, const uint8_t* p, size_t n)> memLoad;
    std::function<uint16_t(uint32_t wordAddr)> memRead;
    std::function<void(uint32_t wordAddr, uint16_t w)> memWrite;
    std::function<void()> screenshotPrepare;   // ПЛИС: теневые регистры видео и палитра
    std::function<void()> irq;                 // вектор 0174
    std::function<void()> resetMachine;        // команда 037

    AzCtrl();
    bool init(const Config& c, std::string* err);   // AZ.INI: ПЗУ, логотип, диски
    // Годится ли карта (каталог и AZ.INI на нём или в каталоге записи) — до того, как
    // что-то менять в машине.
    static bool usable(const Config& c, std::string* err);
    // Диск AZn на ходу (пустой путь — снять, вернётся по AZ.INI при перезапуске МК).
    // Остальное состояние платы не трогается.
    void setUnit(int unit, const std::string& host);
    // Запись в образ AZn на ходу: включение сначала сбрасывает в файл то, что БК уже записала
    // в память поверх него.
    void setUnitRw(int unit, bool on);
    void reboot();                                   // 037: МК перезапускается, AZ.INI заново
    void close();
    const Config& config() const { return cfg_; }
    bool logoLoaded() const { return logo_; }

    // Шина. false на чтении/записи — регистр не ответил (зависание).
    uint16_t read(int reg, bool peek = false);
    void     write(int reg, uint16_t v);

    // Время платы (такты ЦП): сеть и задачи МК.
    void advance(int ticks, int cpuHz);
    // МК грузится с сетевыми дисками — БК держится в сбросе (bk_reset_hold прошивки).
    bool holdBk() const { return holdBk_; }
    bool busy() const { return busy_; }
    AzNet&       net()       { return net_; }
    const AzNet& net() const { return net_; }
    // Кабель: открыть порт хоста по описанию (пусто — вынуть). Текст ошибки или
    // предупреждения — в netStatus().
    bool setNet(const std::string& spec);
    const std::string& netStatus() const { return netStatus_; }

    // Для отладчика и MCP.
    struct UnitInfo { int unit; std::string name, host; uint32_t blocks; bool open, rw, dirty; };
    std::vector<UnitInfo> units() const;
    std::string summary() const;
    uint16_t csr() const { return static_cast<uint16_t>(csr_); }
    int lastCmd() const { return lastCmd_; }
    int selUnit() const { return unit_; }
    uint32_t block() const { return block_; }
    uint64_t commands() const { return ncmd_; }

    void saveState(std::vector<uint8_t>& out) const;
    bool loadState(const uint8_t*& p, const uint8_t* end);

    static std::string resolve(const std::string& root, const std::string& name);  // «0:/x» -> путь хоста

private:
    // Память МК, в которой лежат все буферы обмена: потоки DR ходят по ней словами,
    // и выход за конец буфера читает соседний — как в прошивке.
    enum : uint32_t {
        R_DISKS = 0,                         // 32 записи disk_t по 198 слов (0x18C байт)
        DISK_WORDS = 198,
        R_BLK = 6400,                        // blk_buf, 256 слов
        R_XBUF = 6700,                       // xbuf_len + xbuf[0x218]
        R_TIME = 7000, R_SETTIME = 7020, R_NET = 7040, R_ANYSIZE = 7060, R_CARD = 7070,
        R_FNO = 7080,                        // FILINFO: fsize, fdate, ftime, fattrib, fname[13]
        RAM_WORDS = 7200,
    };
    static constexpr int MAX_DISKS = 32;
    enum : uint8_t { DISK_MOUNTED = 1, DISK_BOOT = 2, DISK_OPENED = 4, DISK_ERROR = 0x80 };

    struct Reg { int rd = 0, wr = 0; uint32_t buf = 0; int32_t count = 0; };
    struct Unit {
        std::string host;                    // файл на хосте
        FILE* f = nullptr;
        bool rw = false;
        std::map<uint32_t, std::vector<uint8_t>> overlay;   // записанные блоки (только чтение)
    };

    uint16_t& w(uint32_t i) { return ram_[i % RAM_WORDS]; }
    uint16_t  w(uint32_t i) const { return ram_[i % RAM_WORDS]; }
    uint8_t*  bytes(uint32_t i) { return reinterpret_cast<uint8_t*>(&ram_[i]); }
    // Запись disk_t в таблице.
    uint32_t diskBlocks(int u) const { return w(R_DISKS + u * DISK_WORDS) | (uint32_t(w(R_DISKS + u * DISK_WORDS + 1)) << 16); }
    uint8_t& diskFlags(int u) { return bytes(R_DISKS + u * DISK_WORDS + 4)[0]; }
    uint8_t  diskFlags(int u) const { return reinterpret_cast<const uint8_t*>(&ram_[R_DISKS + u * DISK_WORDS + 4])[0]; }
    char*    diskName(int u) { return reinterpret_cast<char*>(bytes(R_DISKS + u * DISK_WORDS + 5)); }
    std::string diskNameStr(int u) const;
    static constexpr int NAME_BYTES = 0x182;

    void resetRegs() { for (auto& r : regs_) r = Reg{}; }
    void streamOut(uint32_t buf, int32_t n) { regs_[1].buf = buf; regs_[1].count = n; regs_[1].rd = 2; }
    void streamIn(uint32_t buf, int32_t n) { regs_[1].buf = buf; xferBuf_ = buf; regs_[1].count = n; regs_[1].wr = 2; }
    int32_t command(uint16_t v);             // запись CSR; <0 — ошибка (разряд 15)
    bool unitMounted();
    int32_t job(int j);                      // долгая команда

    void loadIni();
    bool openDisk(int u, const std::string& host, const std::string& name);
    void closeDisks();
    void fillDiskEntry(int u, const std::string& host);
    void hdRead();
    void hdWrite();
    int32_t dirOpen();
    int32_t dirRead();
    int32_t mount();
    int32_t dismount();
    std::string cstr(uint32_t at, size_t maxBytes) const;   // строка из буфера МК
    std::string writePath(const std::string& name) const;   // файл, который пишет БК
    std::string readPath(const std::string& name) const;    // файл для чтения (сначала запись)
    void eepromRead();
    void eepromWrite();
    void timeRecord();
    void setTime();
    void fileOpenRead();
    void fileRead();
    void fileOpenWrite();
    void fileWrite();
    void fileToMem();
    void cardSize();
    void screenshot(std::string name = {});
    // Сеть.
    static bool isLandisk(char c) { return c == 'N' || c == 'P' || c == 'R' || c == 'S' || c == 'A'; }
    void startAsync(std::function<void()> job);   // долгая сетевая команда (CSR «занят»)
    void asyncDone();
    bool endNetWait();                             // обращение БК кончает ожидание по 010
    void netBoot(std::vector<int> netUnits);      // пуск МК с сетевыми дисками
    int32_t netMount(int u, const std::string& name);
    void landiskOpen(int u);                       // OPEN с описанием платы (landisk_open_at_boot)
    void rtcFromNtp();

    Config cfg_;
    bool logo_ = false;
    std::vector<uint16_t> ram_ = std::vector<uint16_t>(RAM_WORDS + 1, 0);
    Reg regs_[4];
    uint32_t csr_ = 0200, status_ = 0, dataWr_ = 0;
    int unit_ = 0;
    uint32_t block_ = 0;
    bool blkHiPending_ = false;
    uint32_t xferBuf_ = R_BLK, xferLen_ = 0x100;
    uint32_t bootErrors_ = 0, bootUnit_ = 0x20;
    uint32_t bootAddr_ = 0;
    uint32_t sizeTmp_ = 0;                  // размер диска между двумя чтениями DR
    // Программы потоков загрузки {адрес, n, слова}: BOOT1 (B4) и BOOT2 (B6), по умолчанию
    // обе — встроенный bootB; [ALTBOOT] подменяет одну из них.
    std::vector<uint16_t> bootB4_, bootB6_;
    uint32_t bootPos_ = 0;
    int lastCmd_ = -1;
    uint64_t ncmd_ = 0;
    Unit units_[MAX_DISKS];
    std::map<std::string, std::map<uint32_t, std::vector<uint8_t>>> keep_;   // записи на время перезапуска
    // Каталог (команды 003/013) и файлы (050–055).
    std::vector<std::string> dirList_;
    std::vector<std::string> dirHost_;
    std::string dirPath_;                   // открытый каталог на хосте (для снимка)
    size_t dirPos_ = 0;
    bool dirOpen_ = false;
    void listDir(const std::string& host);
    FILE* any_ = nullptr;
    std::string anyHost_;                   // открытый файл (для снимка)
    bool anyWrite_ = false;
    bool reopenFile(int u, const std::string& host, bool rw);
    bool unitRw(int u) const { return cfg_.rw || cfg_.rwUnits.count(u) != 0; }
    uint32_t anyPos_ = 0, anySize_ = 0;
    int64_t timeOffset_ = 0;                // часы AZ относительно хоста, с

    AzNet net_;
    std::string netStatus_;
    bool busy_ = false, holdBk_ = false;
    bool ieLatch_ = false;                  // D6 последней записи в CSR (его защёлкивает ПЛИС)
    bool netWait_ = false, stopWait_ = false;   // задача команды 010 с IE
    int pendingCsr_ = -1;                   // запись в CSR, пока МК занят
    uint64_t tickAcc_ = 0;
    std::vector<int> netUnits_;             // сетевые диски из [DISKS] (открываются при пуске)
};

} // namespace bk
