#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace bk {

// Накопитель IDE (ATA) с образом в формате HDI — формате эмулятора GID (утилиты
// HDDImgMaker / BKHDDTool). HDI — побайтовый образ винчестера, перед которым дописан
// один сектор (512 байт) с паспортом накопителя, который тот отдаёт по команде
// IDENTIFY: слово 1 — число цилиндров, 3 — головок, 6 — секторов на дорожке; байт
// 510 = 0xA5, байт 511 — дополнение суммы байтов 0..510 до нуля. Данные секторов
// лежат так, как их хранит сам накопитель, то есть В ИНВЕРСНОМ КОДЕ относительно
// процессора: шину БК и шину IDE разводят инвертирующие буферы платы СМК (БК-docs
// §15.12), и инверсию делает плата, а не накопитель (см. Board::MpiMux).
//
// Модель — порт CHDD из GID (devemu/HDD.cpp), только команды исполняются сразу, без
// потока: BSY процессору не виден. Регистры здесь в обычной (не инверсной) логике ATA.
class HddDrive {
public:
    static constexpr int SECTOR_BYTES = 512;
    static constexpr int SECTOR_WORDS = 256;

    // Регистры канала: блок CS0 (0x1F0..0x1F7) и два регистра блока CS1.
    // ERR_FEAT: чтение — ошибки, запись — features; STATUS_CMD: чтение — статус, запись —
    // команда; CTRL (3F6): чтение — тот же статус, запись — управление (SRST); DRIVE_ADDR (3F7).
    enum Reg { DATA = 0, ERR_FEAT = 1, COUNT = 2, SECTOR = 3, CYL_LO = 4, CYL_HI = 5,
               HEAD = 6, STATUS_CMD = 7, CTRL = 8, DRIVE_ADDR = 9 };
    enum : uint8_t { ST_BSY = 0x80, ST_DRDY = 0x40, ST_DWF = 0x20, ST_DSC = 0x10,
                     ST_DRQ = 0x08, ST_ERR = 0x01 };

    HddDrive() = default;
    ~HddDrive() { detach(); }
    HddDrive(const HddDrive&) = delete;
    HddDrive& operator=(const HddDrive&) = delete;

    // slave — накопитель 1 (разряд DEV регистра головки). readOnly: запись БК живёт
    // в памяти поверх файла и в образ не попадает. Ошибка — в err.
    bool attach(const std::string& path, bool readOnly, bool slave, std::string* err = nullptr);
    void detach();
    bool attached() const { return f_ != nullptr; }
    const std::string& path() const { return path_; }
    bool readOnly() const { return readOnly_; }
    int cylinders() const { return cyl_; }
    int heads() const { return heads_; }
    int sectors() const { return secs_; }
    uint32_t totalSectors() const { return total_; }
    size_t dirtySectors() const { return overlay_.size(); }   // записано БК, но не в файл
    std::string model() const;                                // модель из паспорта
    // Сектор так, как он лежит на накопителе (в инверсном коде), без побочных эффектов.
    bool peekSector(uint32_t lba, uint8_t* dst) const { return attached() && lba < total_ && readSector(lba, dst); }

    void reset(bool hard);                     // hard — включение питания, иначе SRST
    uint16_t read(Reg r);                      // с побочными эффектами (регистр данных)
    uint16_t peek(Reg r) const;                // для отладчика
    void write(Reg r, uint16_t v);
    // Выбран ли этот накопитель разрядом DEV последнего записанного регистра головки.
    bool selected() const { return attached() && !((head_ ^ devId_) & 0x10); }

    // Отладочная сводка: последняя команда, позиция, фаза.
    uint8_t lastCommand() const { return command_; }
    uint32_t lastLba() const { return lastLba_; }

private:
    enum class Phase { Failure, Ready, PioOut, PioIn };
    bool readSector(uint32_t lba, uint8_t* dst) const;
    bool writeSector(uint32_t lba, const uint8_t* src);
    bool seek();                               // регистры -> lba_, сдвиг на следующий
    void readNext(bool verify);
    void writeNext();
    void commandOk();
    void fail(uint8_t err);
    void execute(uint8_t cmd);
    void identify();
    void resetSignature(bool hard);
    void readData(uint16_t& v);
    void writeData(uint16_t v);

    std::FILE* f_ = nullptr;
    std::string path_;
    bool readOnly_ = true;
    uint8_t devId_ = 0;                        // 0 — master, 0x10 — slave
    int cyl_ = 0, heads_ = 0, secs_ = 0;
    uint32_t total_ = 0;
    std::array<uint8_t, SECTOR_BYTES> ident_{};
    std::unordered_map<uint32_t, std::array<uint8_t, SECTOR_BYTES>> overlay_;

    std::array<uint16_t, SECTOR_WORDS> buf_{};
    int word_ = 0;                             // позиция в буфере сектора, слова
    uint32_t lba_ = 0;                         // сектор, который сейчас в буфере
    uint32_t lastLba_ = 0;
    int remain_ = 0;                           // секторов осталось (0 в регистре = 256)
    Phase phase_ = Phase::Failure;
    bool bufferOnly_ = false;                  // обмен только с буфером (READ/WRITE BUFFER, формат)

    uint16_t data_ = 0;
    uint8_t error_ = 0, features_ = 0, count_ = 0, sector_ = 0, cylLo_ = 0, cylHi_ = 0;
    uint8_t head_ = 0, status_ = 0, command_ = 0, control_ = 0, addr_ = 0;
};

// Канал IDE платы: два накопителя на одних проводах. Запись видят оба, читает тот,
// что выбран разрядом DEV (невыбранный отдаёт единицы — монтажное И, как у GID).
class Ide {
public:
    static constexpr int DRIVES = 2;
    HddDrive& drive(int i) { return d_[i & 1]; }
    const HddDrive& drive(int i) const { return d_[i & 1]; }
    bool any() const { return d_[0].attached() || d_[1].attached(); }
    void reset(bool hard) { d_[0].reset(hard); d_[1].reset(hard); }
    void write(HddDrive::Reg r, uint16_t v) { d_[0].write(r, v); d_[1].write(r, v); }
    uint16_t read(HddDrive::Reg r) { return d_[0].read(r) & d_[1].read(r); }
    uint16_t peek(HddDrive::Reg r) const { return d_[0].peek(r) & d_[1].peek(r); }
    // Адрес на шине БК -> регистр канала (СМК: 0177740..0177757, БК-docs §15.12).
    // Нечётные 0177741 и 0177743 — самостоятельные регистры блока CS1.
    static bool decodeSmk(uint16_t addr, HddDrive::Reg& r);
    // «Самара» (БК-docs §13.12): тот же блок на 0177640..0177657, а регистры CS1 —
    // по чётным адресам 0177620 (адрес накопителя) и 0177622 (второй статус/сброс).
    static bool decodeSamara(uint16_t addr, HddDrive::Reg& r);
private:
    HddDrive d_[DRIVES];
};

// Похож ли файл на образ винчестера по расширению (.hdi в любом регистре).
bool looksLikeHdi(const std::string& path);

// Разметка винчестера на логические диски — её пишет программа обслуживания
// контроллера, и у двух контроллеров она своя (docs/hdd.md):
//  * «АльтПро»/СМК — таблица в конце блока 7: слова 255/254/253 — цилиндры, головки,
//    секторы; 252 — число дисков и (старший байт) диск автозагрузки; дальше пары
//    «(цилиндр << 4) | головка начала, длина» и контрольная сумма от 012701;
//  * «Самара» — блок 1: номер диска автозагрузки, H·S, S | (H−1) << 8 и с третьего
//    слова — номера первых блоков дисков двойными словами. Первый блок диска —
//    заголовок (номер диска, ёмкость, флаги защиты), данные — со следующего.
// Логические диски нумеруются с 2 (C:). Разбор — по данным после инверсии.
struct HddLogicalDisk {
    int unit = 0;              // 2 = C:
    uint32_t first = 0;        // первый блок данных (LBA)
    uint32_t blocks = 0;
    uint16_t flags = 0;        // «Самара»: 010 — защита от записи, 020 — от чтения
};
struct HddMap {
    enum class Kind { None, AltPro, Samara };
    Kind kind = Kind::None;
    int bootUnit = 0;          // диск автозагрузки (2 = C:), 0 — не задан
    std::vector<HddLogicalDisk> disks;
};
HddMap readHddMap(const HddDrive& d);
// Разметка образа HDI по файлу (Kind::None — не опознана или файл не открылся).
HddMap::Kind hdiLayout(const std::string& path);
const char* hddMapName(HddMap::Kind k);

} // namespace bk
