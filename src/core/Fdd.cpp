#include "Fdd.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cctype>

namespace bk {

namespace {

// Разряды регистра управления, которые контроллер ЗАПОМИНАЕТ. Шаг (ST) и поиск
// маркера (GDR) — импульсные: они действуют в момент записи и не хранятся.
constexpr uint16_t kStoredFlags = Fdd::CMD_MOTOR | Fdd::CMD_SIDE | Fdd::CMD_DIR | Fdd::CMD_WM;

// Разложить полезную дорожку (10 секторов по 512 байт) в сырой поток IBM-формата:
// гэпы, зоны нулей, маркеры A1A1A1 + признак поля, поля идентификатора и данных.
// Именно этот поток и «крутится» под головкой; драйвер в ПЗУ разбирает его сам.
//
// CRC не считаем: микросхема отдаёт результат сверки отдельным разрядом состояния
// (ST_CRCOK), и драйвер смотрит на разряд, а не на сами байты. Поэтому на месте
// CRC стоят заглушки — так же сделано в BKBTL.
void encodeTrack(const uint8_t* src, uint8_t* raw, uint8_t* marker, int track, int side) {
    std::memset(raw, 0, Fdd::RAW_TRACK);
    std::memset(marker, 0, Fdd::RAW_TRACK / 2);
    int ptr = 0;
    int gap = 42;                        // GAP4a + GAP1 перед первым сектором
    for (int sect = 0; sect < Fdd::SECTORS; ++sect) {
        for (int i = 0; i < gap; ++i) raw[ptr++] = 0x4e;
        for (int i = 0; i < 12; ++i) raw[ptr++] = 0x00;   // зона нулей: без неё ФАПЧ не поймает маркер
        marker[ptr / 2] = 1;                              // маркер поля идентификатора
        raw[ptr++] = 0xa1; raw[ptr++] = 0xa1; raw[ptr++] = 0xa1; raw[ptr++] = 0xfe;
        raw[ptr++] = static_cast<uint8_t>(track);
        raw[ptr++] = static_cast<uint8_t>(side != 0);
        raw[ptr++] = static_cast<uint8_t>(sect + 1);      // сектора нумеруются с единицы
        raw[ptr++] = 2;                                   // код длины: 2 = 512 байт
        raw[ptr++] = 0x12; raw[ptr++] = 0x34;             // CRC — заглушка
        for (int i = 0; i < 22; ++i) raw[ptr++] = 0x4e;   // GAP2
        for (int i = 0; i < 12; ++i) raw[ptr++] = 0x00;
        marker[ptr / 2] = 1;                              // маркер поля данных
        raw[ptr++] = 0xa1; raw[ptr++] = 0xa1; raw[ptr++] = 0xa1; raw[ptr++] = 0xfb;
        for (int i = 0; i < Fdd::SECTOR_SIZE; ++i) raw[ptr++] = src[sect * Fdd::SECTOR_SIZE + i];
        raw[ptr++] = 0x43; raw[ptr++] = 0x21;             // CRC — заглушка
        gap = 36;                                         // GAP3 между секторами
    }
    while (ptr < Fdd::RAW_TRACK) raw[ptr++] = 0x4e;       // GAP4b до конца дорожки
}

// Обратный разбор: вытащить из сырой дорожки 10 секторов. Нужен, когда программа
// писала на диск — в образ кладём уже полезные данные, а не поток.
bool decodeTrack(const uint8_t* raw, uint8_t* dst) {
    int p = 0, out = 0;
    for (;;) {
        while (p < Fdd::RAW_TRACK && raw[p] == 0x4e) ++p;      // GAP1/GAP3
        if (p >= Fdd::RAW_TRACK) break;                        // дорожка кончилась
        while (p < Fdd::RAW_TRACK && raw[p] == 0x00) ++p;      // зона нулей
        if (p >= Fdd::RAW_TRACK) return false;
        for (int i = 0; i < 3 && p < Fdd::RAW_TRACK && raw[p] == 0xa1; ++i) ++p;
        if (p >= Fdd::RAW_TRACK || raw[p++] != 0xfe) return false;
        if (p + 4 > Fdd::RAW_TRACK) return false;
        p += 3;                                                 // дорожка, сторона, сектор
        const uint8_t sizeCode = raw[p++];
        const int sectorSize = (sizeCode == 1) ? 256 : (sizeCode == 2) ? 512
                             : (sizeCode == 3) ? 1024 : 0;
        if (!sectorSize) return false;
        p += 2;                                                 // CRC
        while (p < Fdd::RAW_TRACK && raw[p] == 0x4e) ++p;       // GAP2
        while (p < Fdd::RAW_TRACK && raw[p] == 0x00) ++p;
        if (p >= Fdd::RAW_TRACK) return false;
        for (int i = 0; i < 3 && p < Fdd::RAW_TRACK && raw[p] == 0xa1; ++i) ++p;
        if (p >= Fdd::RAW_TRACK || raw[p++] != 0xfb) return false;
        for (int i = 0; i < sectorSize; ++i) {
            if (out >= Fdd::TRACK_BYTES || p >= Fdd::RAW_TRACK) break;
            dst[out++] = raw[p++];
        }
        p += 2;                                                 // CRC
    }
    return true;
}

} // namespace

bool Fdd::looksLikeImage(const std::string& path) {
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == "bkd" || ext == "img";
}

void Fdd::note(LogEntry::Kind k, uint16_t v) {
    if (!logOn_) return;
    const Drive* d = cur();
    const LogEntry e{k, v, static_cast<uint16_t>(d ? d->dataPtr : 0), ctxPc_,
                     static_cast<uint8_t>(track_), static_cast<uint8_t>(side_)};
    if (log_.size() < LOG_CAP) { log_.push_back(e); return; }
    log_[logPos_] = e;                      // кольцо: старое затирается по кругу
    logPos_ = (logPos_ + 1) % LOG_CAP;
}

Fdd::Fdd() { reset(); }

bool Fdd::attach(int drive, const std::string& path, bool readOnly, int sides) {
    if (drive < 0 || drive >= DRIVES) return false;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) { std::fclose(f); return false; }

    Drive& d = drives_[drive];
    d.image.assign(static_cast<size_t>(size), 0);
    const size_t got = std::fread(d.image.data(), 1, d.image.size(), f);
    std::fclose(f);
    if (got != d.image.size()) { d.image.clear(); return false; }

    d.path = path;
    d.readOnly = readOnly;
    d.dirty = false;
    // Число дорожек — из размера образа. 800 Кбайт дают ровно 80; образ короче
    // (односторонний или 40-дорожечный) тоже примем, недостающее прочтётся нулями.
    d.sides = (sides == 1 || sides == 2) ? sides : SIDES;
    d.tracks = static_cast<int>(d.image.size() / (d.sides * TRACK_BYTES));
    if (d.tracks < 1) d.tracks = 1;
    d.raw.assign(RAW_TRACK, 0x4e);
    d.marker.assign(RAW_TRACK / 2, 0);
    d.trackLoaded = false;
    d.trackChanged = false;
    d.dataPtr = 0;
    if (drive_ == drive) prepareTrack();
    return true;
}

void Fdd::detach(int drive) {
    if (drive < 0 || drive >= DRIVES) return;
    flushTrack();
    Drive& d = drives_[drive];
    d = Drive{};
}

bool Fdd::attached(int drive) const {
    return drive >= 0 && drive < DRIVES && drives_[drive].attached();
}

const std::string& Fdd::path(int drive) const {
    static const std::string kEmpty;
    return (drive >= 0 && drive < DRIVES) ? drives_[drive].path : kEmpty;
}

bool Fdd::readOnly(int drive) const {
    return drive >= 0 && drive < DRIVES ? drives_[drive].readOnly : true;
}

int Fdd::sides(int drive) const {
    return (drive >= 0 && drive < DRIVES) ? drives_[drive].sides : SIDES;
}

int Fdd::tracks(int drive) const {
    return (drive >= 0 && drive < DRIVES) ? drives_[drive].tracks : 0;
}

bool Fdd::dirty(int drive) const {
    return drive >= 0 && drive < DRIVES ? drives_[drive].dirty : false;
}

bool Fdd::save(int drive) {
    if (drive < 0 || drive >= DRIVES) return false;
    flushTrack();
    Drive& d = drives_[drive];
    if (!d.attached() || d.path.empty()) return false;
    std::FILE* f = std::fopen(d.path.c_str(), "r+b");
    if (!f) return false;
    const size_t wrote = std::fwrite(d.image.data(), 1, d.image.size(), f);
    std::fclose(f);
    if (wrote != d.image.size()) return false;
    d.dirty = false;
    return true;
}

void Fdd::reset() {
    flushTrack();
    drive_ = -1;
    track_ = side_ = 0;
    dataReg_ = writeReg_ = shiftReg_ = 0;
    writing_ = searchSync_ = writeMarker_ = crcCalc_ = false;
    gdrTrigger_ = false;
    writeFlag_ = shiftFlag_ = shiftMarker_ = false;
    status_ = ST_TRACK0;
    flags_ = 0;
    for (Drive& d : drives_) { d.dataPtr = 0; d.trackLoaded = false; }
}

// Раскладка дорожки из encodeTrack: первый сектор занимает 42+12+4+4+2+22+12+4+512+2
// = 616 байт, дальше по 610 (гэп 36 вместо 42). Считаем номер сектора под головкой —
// в отладке сразу видно, куда именно смотрит головка.
int Fdd::sectorUnderHead() const {
    const Drive* d = cur();
    if (!d || !d->attached()) return 0;
    const int first = 42 + 12 + 4 + 4 + 2 + 22 + 12 + 4 + SECTOR_SIZE + 2;   // 616
    if (d->dataPtr >= first + (SECTORS - 1) * (first - 6)) return 0;         // хвост дорожки
    const int n = d->dataPtr / (first - 6) + 1;
    return (n >= 1 && n <= SECTORS) ? n : 0;
}

int Fdd::headPos() const {
    const Drive* d = cur();
    return d ? d->dataPtr : 0;
}

// ---- Регистр 0177130 -------------------------------------------------------

void Fdd::writeCtrl(uint16_t cmd) {
    bool needTrack = false;

    // Выбор привода: разряды DS0..DS3 — это линии выбора, а не двоичный номер.
    // Разбор — как в BKBTL: смотрим, какая линия взведена (несколько сразу
    // реального смысла не имеют, побеждает младшая).
    int newDrive = -1;
    switch (cmd & CMD_DS) {
    case 0:                          newDrive = -1; break;
    case 2: case 6: case 10: case 14: newDrive = 1; break;
    case 4: case 12:                 newDrive = 2;  break;
    case 8:                          newDrive = 3;  break;
    default:                         newDrive = 0;  break;
    }
    if (newDrive != drive_) {
        flushTrack();
        drive_ = newDrive;
        needTrack = true;
    }
    if (drive_ < 0) return;

    flags_ = static_cast<uint16_t>((flags_ & ~kStoredFlags) | (cmd & kStoredFlags));

    const int newSide = (flags_ & CMD_SIDE) ? 1 : 0;
    if (newSide != side_) { side_ = newSide; needTrack = true; }

    stats_.lastCmd = cmd;
    note(LogEntry::Kind::Cmd, cmd);
    if (cmd & CMD_STEP) {           // импульс шага: одна дорожка в сторону DIR
        ++stats_.steps;
        if (flags_ & CMD_DIR) {
            if (track_ < MAX_TRACK) { ++track_; needTrack = true; }
        } else {
            if (track_ > 0) { --track_; needTrack = true; }
        }
    }
    if (needTrack) prepareTrack();

    // «Начало чтения» — двухтактное. Документация прямо предписывает приём:
    // записать в GDR единицу, затем ноль в зоне нулей перед маркером; единица
    // приводит схему в исходное состояние, а СНЯТИЕ разрешает поиск маркера и
    // синхронизирует ФАПЧ с потоком. Поэтому поиск запускается по спаду разряда,
    // а не по единице, и вместе с ним гасится готовность данных: иначе драйвер
    // успевает забрать «протухшее» слово, оставшееся от предыдущего чтения.
    if (cmd & CMD_GDR) {
        gdrTrigger_ = true;
    } else if (gdrTrigger_) {
        gdrTrigger_ = false;
        ++stats_.searches;
        searchSync_ = true;
        crcCalc_ = true;
        status_ &= ~(ST_CRCOK | ST_TR);
    }
    if (writing_ && (cmd & CMD_WM)) {   // маркер записывается со следующим словом
        writeMarker_ = true;
        status_ &= ~ST_CRCOK;
    }
}

uint16_t Fdd::readStatus() {
    const Drive* d = cur();
    if (!d) return 0;                                   // привод не выбран — шина молчит
    if (!d->attached())                                 // выбран пустой привод
        return static_cast<uint16_t>(ST_INDEX | (track_ == 0 ? ST_TRACK0 : 0));

    if (track_ == 0) status_ |= ST_TRACK0; else status_ &= ~ST_TRACK0;
    if (d->readOnly) status_ |= ST_WPROT; else status_ &= ~ST_WPROT;
    // Готовность накопителя следует за двигателем: раскрученный привод сообщает
    // RDY, остановленный — нет, и заодно теряет готовность данных. Так делает
    // BKemu, и без этого драйвер прошивки 326 не уходит дальше раскрутки:
    // он ждёт RDY и до чтения секторов не добирается вовсе.
    if (motor()) status_ |= ST_RDY;
    else         status_ &= ~(ST_RDY | ST_TR | ST_CRCOK);
    return status_;
}

// ---- Регистр 0177132 -------------------------------------------------------

uint16_t Fdd::readData() {
    // Любое обращение к регистру данных снимает TR — так устроена микросхема.
    status_ &= ~ST_TR;
    writing_ = searchSync_ = false;
    writeFlag_ = shiftFlag_ = false;
    const Drive* d = cur();
    if (!d || !d->attached()) return 0;
    // Второе слово поля несёт его признак: 0xA1FE — идентификатор, 0xA1FB — данные.
    ++fieldWords_;
    if (fieldWords_ == 2 && dataReg_ == 0xA1FB) fieldIsData_ = true;
    note(LogEntry::Kind::Data, dataReg_);
    return dataReg_;
}

void Fdd::writeData(uint16_t v) {
    writing_ = true;
    searchSync_ = false;
    if (!writeFlag_ && !shiftFlag_) {            // оба регистра пусты
        shiftReg_ = v;
        shiftFlag_ = true;
        status_ |= ST_TR;
    } else if (!writeFlag_) {                    // свободен регистр записи
        writeReg_ = v;
        writeFlag_ = true;
        status_ &= ~ST_TR;
    } else if (!shiftFlag_) {                    // свободен сдвиговый
        shiftReg_ = writeReg_;
        shiftFlag_ = true;
        writeReg_ = v;
        status_ &= ~ST_TR;
    } else {
        writeReg_ = v;                           // оба заняты — теряем предыдущее
    }
}

// ---- Вращение диска --------------------------------------------------------

void Fdd::periodic() {
    if (!motor()) return;                        // двигатель выключен — диск стоит

    for (Drive& d : drives_) {                   // крутятся все вставленные диски
        if (!d.attached()) continue;
        d.dataPtr += 2;
        if (d.dataPtr >= RAW_TRACK) d.dataPtr = 0;
    }

    Drive* d = cur();
    if (!d || !d->attached()) return;
    if (!d->trackLoaded) prepareTrack();

    // Индексное отверстие — короткое окно в НАЧАЛЕ дорожки (как в BKemu):
    // драйвер считает по нему обороты, дожидаясь раскрутки.
    if (d->dataPtr < INDEX_LENGTH) status_ |= ST_INDEX;
    else                           status_ &= ~ST_INDEX;

    if (!writing_) {                             // чтение
        dataReg_ = static_cast<uint16_t>((d->raw[d->dataPtr] << 8) | d->raw[d->dataPtr + 1]);
        ++stats_.words;
        if (status_ & ST_TR) {
            if (crcCalc_) {          // поле кончилось: запомним, сколько из него взяли
                ++stats_.fields;
                stats_.lastLen[stats_.lastIdx] = fieldWords_;
                stats_.lastIdx = (stats_.lastIdx + 1) & 7;
                // Поле данных опознаём по длине маркерной пары: у него 258 слов.
                if (fieldIsData_ && fieldWords_ < 250) ++stats_.shortData;
                fieldWords_ = 0;
            }
            // Процессор не забрал предыдущее слово. Если шёл подсчёт CRC, это его
            // конец: сверку мы не считаем, а сразу сообщаем «сошлось» — драйвер
            // смотрит именно на разряд состояния.
            if (crcCalc_) { crcCalc_ = false; status_ |= ST_CRCOK; }
        } else if (searchSync_) {
            if (d->marker[d->dataPtr / 2]) {     // маркер найден — обмен пошёл
                status_ |= ST_TR;
                searchSync_ = false;
                ++stats_.markers;
                note(LogEntry::Kind::Marker, dataReg_);
                fieldWords_ = 0;
                fieldIsData_ = false;
            }
        } else {
            status_ |= ST_TR;
        }
    } else {                                     // запись
        if (!shiftFlag_) return;
        d->raw[d->dataPtr]     = static_cast<uint8_t>(shiftReg_ >> 8);
        d->raw[d->dataPtr + 1] = static_cast<uint8_t>(shiftReg_ & 0xff);
        shiftFlag_ = false;
        d->trackChanged = true;
        d->marker[d->dataPtr / 2] = shiftMarker_ ? 1 : 0;
        if (shiftMarker_) { shiftMarker_ = false; crcCalc_ = true; }
        if (writeFlag_) {
            shiftReg_ = writeReg_;
            shiftFlag_ = true;  writeFlag_ = false;
            shiftMarker_ = writeMarker_; writeMarker_ = false;
            status_ |= ST_TR;
        } else if (crcCalc_) {                   // слов больше нет — дописываем CRC
            shiftReg_ = 0x4444;
            shiftFlag_ = true;
            shiftMarker_ = false;
            crcCalc_ = false;
            status_ |= ST_CRCOK;
        }
    }
}

// ---- Дорожка ---------------------------------------------------------------

void Fdd::prepareTrack() {
    flushTrack();
    Drive* d = cur();
    if (!d || !d->attached()) return;

    d->trackChanged = false;
    d->dataTrack = track_;
    d->dataSide = side_;
    d->trackLoaded = true;
    status_ |= ST_TR;

    uint8_t buf[TRACK_BYTES];
    std::memset(buf, 0, sizeof buf);
    // Смещение дорожки в образе. У одностороннего диска второй стороны нет вовсе:
    // обращение к ней должно читаться нулями, а не чужой дорожкой.
    const size_t off = (side_ >= d->sides)
        ? d->image.size()
        : (static_cast<size_t>(track_) * d->sides + side_) * TRACK_BYTES;
    if (off < d->image.size()) {
        const size_t n = std::min(static_cast<size_t>(TRACK_BYTES), d->image.size() - off);
        std::memcpy(buf, d->image.data() + off, n);
    }
    if (d->raw.size() != RAW_TRACK) d->raw.assign(RAW_TRACK, 0);
    if (d->marker.size() != RAW_TRACK / 2) d->marker.assign(RAW_TRACK / 2, 0);
    encodeTrack(buf, d->raw.data(), d->marker.data(), track_, side_);
}

void Fdd::flushTrack() {
    Drive* d = cur();
    if (!d || !d->attached() || !d->trackChanged) return;
    d->trackChanged = false;

    uint8_t buf[TRACK_BYTES];
    std::memset(buf, 0, sizeof buf);
    if (!decodeTrack(d->raw.data(), buf)) return;   // дорожка разобралась не полностью — не портим образ
    if (d->dataSide >= d->sides) return;    // односторонний диск: писать некуда
    const size_t off = (static_cast<size_t>(d->dataTrack) * d->sides + d->dataSide) * TRACK_BYTES;
    if (off + TRACK_BYTES > d->image.size()) d->image.resize(off + TRACK_BYTES, 0);
    std::memcpy(d->image.data() + off, buf, TRACK_BYTES);
    d->dirty = true;
}

} // namespace bk
