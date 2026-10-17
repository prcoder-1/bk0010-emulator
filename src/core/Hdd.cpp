#include "Hdd.h"
#include <cctype>
#include <cstring>
#include <sys/types.h>

namespace bk {

namespace {
enum : uint8_t { ER_UNC = 0x40, ER_IDNF = 0x10, ER_ABRT = 0x04, ER_AMNF = 0x01 };
enum : uint8_t { HD_HEAD = 0x0f, HD_DEV = 0x10, HD_LBA = 0x40 };
enum : uint8_t { DA_DS0 = 0x01, DA_DS1 = 0x02, DA_HS = 0x3c, DA_WG = 0x40 };
enum : uint8_t { CTL_SRST = 0x04 };

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
}

bool looksLikeHdi(const std::string& path) {
    if (path.size() < 4) return false;
    std::string ext = path.substr(path.size() - 4);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".hdi";
}

bool Ide::decodeSmk(uint16_t addr, HddDrive::Reg& r) {
    if (addr < 0177740 || addr > 0177757) return false;
    if (addr == 0177741) { r = HddDrive::DRIVE_ADDR; return true; }
    if (addr == 0177743) { r = HddDrive::CTRL; return true; }
    // 0177740 — статус/команда (1F7), ... 0177756 — данные (1F0).
    r = static_cast<HddDrive::Reg>((0177756 - (addr & ~1)) / 2);
    return true;
}

bool HddDrive::attach(const std::string& path, bool readOnly, bool slave, std::string* err) {
    detach();
    auto bad = [&](const char* why) { if (err) *err = why; if (f_) { std::fclose(f_); f_ = nullptr; } return false; };
    f_ = std::fopen(path.c_str(), readOnly ? "rb" : "r+b");
    if (!f_) return bad(readOnly ? "файл не открывается" : "файл не открывается на запись");
    if (std::fread(ident_.data(), 1, SECTOR_BYTES, f_) != SECTOR_BYTES) return bad("файл короче паспорта (512 байт)");
    uint8_t cs = 0;
    for (int i = 0; i < SECTOR_BYTES - 1; ++i) cs = static_cast<uint8_t>(cs + ident_[i]);
    if (ident_[510] != 0xA5 || static_cast<uint8_t>(cs + ident_[511]) != 0)
        return bad("нет паспорта HDI: байт 510 не 0xA5 или не сошлась контрольная сумма");
    cyl_ = le16(&ident_[2]);
    heads_ = le16(&ident_[6]);
    secs_ = le16(&ident_[12]);
    if (cyl_ <= 0 || heads_ <= 0 || heads_ > 16 || secs_ <= 0 || secs_ > 255)
        return bad("в паспорте HDI недопустимая геометрия");
    total_ = static_cast<uint32_t>(cyl_) * heads_ * secs_;
    path_ = path;
    readOnly_ = readOnly;
    devId_ = slave ? HD_DEV : 0;
    overlay_.clear();
    phase_ = Phase::Ready;
    reset(true);
    return true;
}

void HddDrive::detach() {
    if (f_) std::fclose(f_);
    f_ = nullptr;
    path_.clear();
    overlay_.clear();
    cyl_ = heads_ = secs_ = 0;
    total_ = 0;
    phase_ = Phase::Failure;
}

std::string HddDrive::model() const {
    std::string s;
    for (int i = 54; i < 94; i += 2) { s += static_cast<char>(ident_[i + 1]); s += static_cast<char>(ident_[i]); }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back();
    return s;
}

bool HddDrive::readSector(uint32_t lba, uint8_t* dst) {
    auto it = overlay_.find(lba);
    if (it != overlay_.end()) { std::memcpy(dst, it->second.data(), SECTOR_BYTES); return true; }
    const off_t pos = static_cast<off_t>(lba + 1) * SECTOR_BYTES;   // +1 — паспорт
    return fseeko(f_, pos, SEEK_SET) == 0 && std::fread(dst, 1, SECTOR_BYTES, f_) == SECTOR_BYTES;
}

bool HddDrive::writeSector(uint32_t lba, const uint8_t* src) {
    if (readOnly_) {
        std::memcpy(overlay_[lba].data(), src, SECTOR_BYTES);
        return true;
    }
    const off_t pos = static_cast<off_t>(lba + 1) * SECTOR_BYTES;
    if (fseeko(f_, pos, SEEK_SET) != 0 || std::fwrite(src, 1, SECTOR_BYTES, f_) != SECTOR_BYTES) return false;
    std::fflush(f_);
    return true;
}

void HddDrive::commandOk() {
    phase_ = Phase::Ready;
    word_ = 0;
    error_ = 0;
    status_ = ST_DRDY | ST_DSC;
}

void HddDrive::fail(uint8_t err) {
    status_ |= ST_ERR;
    status_ &= ~ST_DRQ;
    error_ = err;
    phase_ = Phase::Failure;
}

// Сброс. В отличие от GID, накопитель, на котором прошлая команда кончилась ошибкой,
// после сброса снова работает: иначе драйвер, сбросивший его после ошибки (так делает
// базовый драйвер СМК), терял диск до выключения питания.
void HddDrive::reset(bool hard) {
    control_ = 0;
    resetSignature(hard);
}

void HddDrive::resetSignature(bool hard) {
    (void)hard;
    if (!attached()) {
        remain_ = 0;
        phase_ = Phase::Failure;
        data_ = 0xffff;
        error_ = features_ = count_ = sector_ = cylLo_ = cylHi_ = head_ = status_ = command_ = addr_ = 0xff;
        return;
    }
    // Сигнатура ATA: счётчик и сектор = 1, цилиндр = 0 (не ATAPI), ошибка 1 — «нет ошибок».
    remain_ = 1;
    count_ = sector_ = error_ = 1;
    cylLo_ = cylHi_ = 0;
    head_ = 0;
    addr_ = 0;
    features_ = command_ = control_ = 0;
    status_ = ST_DRDY | ST_DSC;
    phase_ = Phase::Ready;
    word_ = 0;
    bufferOnly_ = false;
}

void HddDrive::write(Reg r, uint16_t v) {
    if (!attached()) return;
    const uint8_t b = static_cast<uint8_t>(v);
    const bool idle = !(status_ & (ST_BSY | ST_DRQ));
    switch (r) {
    case DATA:
        if ((head_ ^ devId_) & HD_DEV) return;
        if (status_ & ST_DRQ) writeData(v);
        break;
    case ERR_FEAT: if (idle) features_ = b; break;
    case COUNT:    if (idle) { count_ = b; remain_ = b ? b : 256; } break;
    case SECTOR:   if (idle) sector_ = b; break;
    case CYL_LO:   if (idle) cylLo_ = b; break;
    case CYL_HI:   if (idle) cylHi_ = b; break;
    case HEAD:
        if (!idle) break;
        head_ = b;
        addr_ = static_cast<uint8_t>((addr_ & ~DA_HS) | ((head_ & HD_HEAD) << 2));
        addr_ = static_cast<uint8_t>((addr_ & ~(DA_DS0 | DA_DS1)) | ((head_ & HD_DEV) ? DA_DS1 : DA_DS0));
        break;
    case STATUS_CMD:
        // Команду исполняет только выбранный накопитель; диагностику (0x90) — оба.
        if (((head_ ^ devId_) & HD_DEV) && b != 0x90) return;
        if (idle) { command_ = b; execute(b); }
        break;
    case CTRL:
        control_ = b;
        if (control_ & CTL_SRST) reset(false);
        break;
    case DRIVE_ADDR:
        break;
    }
}

uint16_t HddDrive::read(Reg r) {
    if (!attached() || ((head_ ^ devId_) & HD_DEV)) return 0xffff;
    if (r == DATA) {
        if (!(status_ & ST_DRQ)) return 0xffff;
        uint16_t v;
        readData(v);
        return v;
    }
    return peek(r);
}

uint16_t HddDrive::peek(Reg r) const {
    if (!attached() || ((head_ ^ devId_) & HD_DEV)) return 0xffff;
    switch (r) {
    case DATA:       return data_;
    case ERR_FEAT:   return error_;
    case COUNT:      return count_;
    case SECTOR:     return sector_;
    case CYL_LO:     return cylLo_;
    case CYL_HI:     return cylHi_;
    case HEAD:       return head_;
    case STATUS_CMD: return status_;
    case CTRL:       return status_;
    case DRIVE_ADDR: return addr_;
    }
    return 0xffff;
}

void HddDrive::readData(uint16_t& v) {
    if (phase_ != Phase::PioIn) { fail(ER_ABRT | ER_IDNF); v = 0xffff; return; }
    data_ = v = buf_[word_];
    if (++word_ >= SECTOR_WORDS) {
        if (remain_ && !bufferOnly_) readNext(false);
        else commandOk();
    }
}

void HddDrive::writeData(uint16_t v) {
    if (phase_ != Phase::PioOut) { fail(ER_ABRT | ER_IDNF); return; }
    data_ = buf_[word_] = v;
    if (++word_ < SECTOR_WORDS) return;
    if (bufferOnly_) { commandOk(); return; }       // запись буфера / формат: на диск не идёт
    uint8_t bytes[SECTOR_BYTES];
    for (int i = 0; i < SECTOR_WORDS; ++i) {
        bytes[2 * i] = static_cast<uint8_t>(buf_[i]);
        bytes[2 * i + 1] = static_cast<uint8_t>(buf_[i] >> 8);
    }
    addr_ |= DA_WG;
    const bool ok = writeSector(lba_, bytes);
    addr_ &= ~DA_WG;
    if (!ok) { fail(ER_ABRT | ER_AMNF); status_ |= ST_DWF; return; }
    if (remain_) writeNext();
    else commandOk();
}

// Номер сектора из регистров (LBA или CHS) и сдвиг регистров на следующий — для
// многосекторных обменов. Порт CHDD::seek из GID.
bool HddDrive::seek() {
    int64_t n = -1;
    if (head_ & HD_LBA) {
        n = (int64_t(head_ & HD_HEAD) << 24) | (int64_t(cylHi_) << 16) | (int64_t(cylLo_) << 8) | sector_;
    } else {
        const int c = (cylHi_ << 8) | cylLo_, h = head_ & HD_HEAD, s = sector_ - 1;
        if (c < cyl_ && h < heads_ && s >= 0 && s < secs_) n = (int64_t(c) * heads_ + h) * secs_ + s;
    }
    if (n < 0 || n >= total_) { fail(ER_ABRT | ER_IDNF); return false; }
    lba_ = lastLba_ = static_cast<uint32_t>(n);
    count_ = static_cast<uint8_t>(count_ - 1);
    if (--remain_ > 0) {
        if (head_ & HD_LBA) {
            if (!++sector_ && !++cylLo_ && !++cylHi_)
                head_ = static_cast<uint8_t>((head_ & ~HD_HEAD) | ((head_ + 1) & HD_HEAD));
        } else {
            sector_ = static_cast<uint8_t>(sector_ % secs_ + 1);       // сектора — с единицы
            if (sector_ == 1) {
                const int nh = ((head_ & HD_HEAD) + 1) % heads_;
                head_ = static_cast<uint8_t>((head_ & ~HD_HEAD) | nh);
                if (!nh && !++cylLo_) ++cylHi_;
            }
        }
    } else {
        status_ &= ~ST_DRQ;
    }
    status_ |= ST_DSC;
    return true;
}

void HddDrive::readNext(bool verify) {
    if (!seek()) return;
    uint8_t bytes[SECTOR_BYTES];
    if (!readSector(lba_, bytes)) { fail(ER_ABRT | ER_AMNF | ER_UNC); return; }
    for (int i = 0; i < SECTOR_WORDS; ++i)
        buf_[i] = static_cast<uint16_t>(bytes[2 * i] | (bytes[2 * i + 1] << 8));
    phase_ = Phase::PioIn;
    if (!verify) status_ |= ST_DRQ;
    error_ = 0;
    word_ = 0;
}

void HddDrive::writeNext() {
    if (!seek()) return;
    phase_ = Phase::PioOut;
    status_ |= ST_DRQ;
    error_ = 0;
    word_ = 0;
}

void HddDrive::identify() {
    for (int i = 0; i < SECTOR_WORDS; ++i) buf_[i] = le16(&ident_[2 * i]);
    count_ = 0;
    remain_ = 0;
    sector_ = 1;
    cylLo_ = cylHi_ = 0;
    head_ &= ~HD_HEAD;
    phase_ = Phase::PioIn;
    status_ |= ST_DRQ;
    word_ = 0;
}

void HddDrive::execute(uint8_t cmd) {
    error_ = 0;
    status_ = 0;
    bufferOnly_ = false;
    bool ok = true;
    if ((cmd & 0xf0) == 0x10) cmd = 0x10;          // рекалибровка: 1x
    if ((cmd & 0xf0) == 0x70) cmd = 0x70;          // позиционирование: 7x
    switch (cmd) {
    case 0x00: case 0x08: case 0x10:               // NOP, DEVICE RESET, RECALIBRATE
    case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xe6:   // STANDBY/IDLE/SLEEP
        commandOk();
        break;
    case 0x20: case 0x21:                          // READ SECTOR(S)
        readNext(false);
        break;
    case 0x30: case 0x31: case 0x3c:               // WRITE SECTOR(S), WRITE VERIFY
        writeNext();
        break;
    case 0x40: case 0x41:                          // READ VERIFY SECTOR(S)
        while (remain_ > 0 && !(status_ & ST_ERR)) readNext(true);
        if (!(status_ & ST_ERR)) { phase_ = Phase::Ready; word_ = 0; }
        break;
    case 0x50:                                     // FORMAT TRACK: таблица чередования — в никуда
        phase_ = Phase::PioOut; status_ |= ST_DRQ; word_ = 0; remain_ = 0; bufferOnly_ = true;
        break;
    case 0x70:                                     // SEEK
        if (seek()) commandOk(); else ok = false;
        break;
    case 0x90:                                     // EXECUTE DEVICE DIAGNOSTIC
        resetSignature(false);
        return;
    case 0x91:                                     // INITIALIZE DEVICE PARAMETERS — геометрия своя
        phase_ = Phase::Ready;
        break;
    case 0xe4:                                     // READ BUFFER
        phase_ = Phase::PioIn; status_ |= ST_DRQ; word_ = 0; remain_ = 0; bufferOnly_ = true;
        break;
    case 0xe8:                                     // WRITE BUFFER
        phase_ = Phase::PioOut; status_ |= ST_DRQ; word_ = 0; remain_ = 0; bufferOnly_ = true;
        break;
    case 0xec:                                     // IDENTIFY DEVICE
        identify();
        break;
    default:                                       // прочее, как у GID, — «выполнено»
        phase_ = Phase::Ready;
        break;
    }
    status_ |= ST_DSC | ST_DRDY;
    if (!ok) { status_ |= ST_ERR; error_ |= ER_ABRT; phase_ = Phase::Failure; }
}

} // namespace bk
