#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace bk {

// Контроллер НГМД на однокристальной микросхеме 1801ВП1-128 и до четырёх
// приводов. Это тот же узел, что стоит в КНГМД и в контроллерах «АльтПро»/СМК,
// поэтому адреса 0177130 и 0177132 — его штатные регистры, а не выдумка
// конкретной платы (БК-docs, 14-подключение-устройств.md, §14.2г).
//
// Главное, что нужно понимать про эту микросхему: она НЕ ищет сектора. Она умеет
// ровно три вещи — крутить диск, распознавать маркер A1 с пропущенным
// синхроимпульсом и отдавать процессору поток слов раз в 64 мкс, выставляя флаг
// TR. Разбор формата дорожки (маркеры, поля идентификатора, CRC) целиком лежит
// на драйвере в ПЗУ. Поэтому эмулировать нужно именно ПОТОК: дорожка держится в
// виде «сырого» образа IBM-формата (гэпы 0x4E, зоны нулей, A1A1A1+FE/FB,
// данные), а не в виде массива секторов.
//
// Модель портирована из BKBTL (emubase/Floppy.cpp) — она проверена на реальном
// софте БК и совпадает с описанием микросхемы до разряда. Отличия у нас два:
// образ держится целиком в памяти (проще и позволяет не трогать файл, пока не
// попросят), и такт вращения привязан к тактам процессора.
class Fdd {
public:
    // Регистр 0177130 по ЗАПИСИ — управление.
    enum : uint16_t {
        CMD_DS    = 017,    // DS0..DS3 — выбор накопителя (маска)
        CMD_MOTOR = 020,    // MSW — включение двигателя
        CMD_SIDE  = 040,    // HS  — выбор поверхности (1 — верхняя)
        CMD_DIR   = 0100,   // DIR — направление шага (1 — к центру)
        CMD_STEP  = 0200,   // ST  — импульс шага (триггера нет, снимать не нужно)
        CMD_GDR   = 0400,   // GDR — «начало чтения»: синхронизация и поиск маркера
        CMD_WM    = 01000,  // WM  — «запись маркера» (пропуск синхроимпульсов)
    };
    // Регистр 0177130 по ЧТЕНИЮ — состояние.
    enum : uint16_t {
        ST_TRACK0 = 01,      // TR0 — головка на нулевой дорожке
        ST_RDY    = 02,      // RDY — готовность накопителя
        ST_WPROT  = 04,      // WPR — защита от записи
        ST_TR     = 0200,    // TR  — готовность регистра данных (основной флаг обмена)
        ST_CRCOK  = 040000,  // CRC — контрольная сумма сошлась
        ST_INDEX  = 0100000, // IND — индексное отверстие
    };

    static constexpr int SECTORS      = 10;                      // секторов на дорожке
    static constexpr int SECTOR_SIZE  = 512;
    static constexpr int TRACK_BYTES  = SECTORS * SECTOR_SIZE;   // 5120 — «полезная» дорожка
    static constexpr int RAW_TRACK    = 6250;                    // сырая дорожка (250 кбит/с, 300 об/мин)
    static constexpr int INDEX_LENGTH = 30;                      // длина индексного окна в байтах дорожки
    static constexpr int SIDES        = 2;
    static constexpr int MAX_TRACK    = 82;
    static constexpr int DRIVES       = 4;
    // Слово формируется за 64 мкс; при 3 МГц это 192 такта процессора.
    static constexpr int PERIOD_TICKS = 192;

    // Привод: образ целиком в памяти плюс распакованная текущая дорожка.
    struct Drive {
        std::vector<uint8_t> image;      // сырые сектора, как в файле
        std::string path;
        bool  readOnly = true;
        bool  dirty    = false;          // образ изменён записью
        int   tracks   = 0;              // сколько дорожек в образе (по его размеру)
        int   sides    = SIDES;          // сторон в образе: 2 обычно, 1 у односторонних
        int   dataTrack = 0, dataSide = 0;
        int   dataPtr  = 0;              // положение головки в сырой дорожке, байт
        std::vector<uint8_t> raw;        // сырая дорожка, RAW_TRACK байт
        std::vector<uint8_t> marker;     // маркеры, по признаку на слово
        bool  trackLoaded = false;
        bool  trackChanged = false;
        bool attached() const { return !image.empty(); }
    };

    Fdd();

    // Образ: 800 Кбайт (80 дорожек x 2 стороны x 10 секторов x 512) — обычный
    // сырой дамп секторов, в котором лежат и .bkd, и .img. Меньшие образы
    // (односторонние или 40-дорожечные) тоже принимаются: число дорожек
    // вычисляется по размеру, недостающее читается нулями.
    // Похоже ли имя на образ диска: расширения .bkd и .img в ЛЮБОМ регистре.
    // Нужно фильтру в диалоге открытия и проверке в командной строке.
    static bool looksLikeImage(const std::string& path);

    // sides: 0 — определить по размеру (800 Кбайт → двусторонний), 1 или 2 — задать
    // явно. Для 400-килобайтных образов однозначного признака нет: это может быть
    // и односторонний 80-дорожечный диск, и двусторонний 40-дорожечный, — поэтому
    // выбор оставлен за пользователем (ключ --disk-sides).
    bool attach(int drive, const std::string& path, bool readOnly = true, int sides = 0);
    void detach(int drive);
    bool attached(int drive) const;
    const std::string& path(int drive) const;
    bool readOnly(int drive) const;
    int  sides(int drive) const;
    int  tracks(int drive) const;
    bool dirty(int drive) const;
    bool save(int drive);                 // записать изменённый образ обратно в файл

    void reset();

    // Регистры со стороны процессора.
    void     writeCtrl(uint16_t v);       // 0177130 запись
    uint16_t readStatus();                // 0177130 чтение
    uint16_t readData();                  // 0177132 чтение (сбрасывает TR)
    void     writeData(uint16_t v);       // 0177132 запись
    void     periodic();                  // поворот диска на одно слово (64 мкс)

    // Диагностика. «Потерянное слово» — процессор не успел забрать предыдущее до
    // прихода следующего (флаг TR ещё стоял): для драйвера это порча данных, и
    // счётчик сразу отвечает на вопрос «мы не успеваем или дело в другом».
    // Ключевая величина — СКОЛЬКО СЛОВ процессор успел забрать из поля, пока оно
    // шло под головкой. У поля идентификатора это 5 слов, у поля данных 258
    // (маркер + 256 данных + CRC). Всё, что короче, — оборванная передача: значит
    // процессор не поспел за темпом 64 мкс на слово. Отличить обрыв от нормального
    // конца поля иначе нельзя: и то и другое выглядит как «слово не забрали».
    struct Stats {
        long words = 0;        // отдано слов при чтении
        long fields = 0;       // прочитано полей (от маркера до конца)
        long shortData = 0;    // полей данных, оборванных на середине
        long markers = 0;      // найдено маркеров
        long searches = 0;     // запусков поиска маркера
        long steps = 0;        // шагов головки
        uint16_t lastCmd = 0;  // последняя команда в 0177130
        int  lastLen[8] = {0}; // длины последних восьми полей, в словах
        int  lastIdx = 0;
    };
    const Stats& stats() const { return stats_; }
    void resetStats() { stats_ = Stats{}; }

    // Кольцевой журнал обращений: по нему видно точную последовательность
    // «команда — поиск — маркер — слова», без которой обрыв передачи не поймать.
    struct LogEntry {
        enum class Kind : uint8_t { Cmd, Status, Data, Marker } kind;
        uint16_t value;    // записанная команда / прочитанное слово
        uint16_t head;     // положение головки в сырой дорожке
        uint16_t pc;       // адрес команды процессора, сделавшей обращение
        uint8_t  track, side;
    };
    // Адрес текущей команды процессора — его подставляет Board из хука обращений,
    // иначе по журналу не понять, какая подпрограмма драйвера что делает.
    void setContextPc(uint16_t pc) { ctxPc_ = pc; }
    void setLog(bool on) { logOn_ = on; if (!on) log_.clear(); }
    bool logOn() const { return logOn_; }
    const std::vector<LogEntry>& log() const { return log_; }
    size_t logPos() const { return logPos_; }   // начало кольца, когда оно заполнено
    void clearLog() { log_.clear(); }
    // Номер сектора, который сейчас под головкой (1..10), 0 — межсекторный промежуток.
    int sectorUnderHead() const;

    // Для отладчика: то же, но без побочных эффектов.
    uint16_t statusView() const { return status_; }
    uint16_t dataView() const { return dataReg_; }
    int  drive() const { return drive_; }
    int  track() const { return track_; }
    int  side() const { return side_; }
    bool motor() const { return (flags_ & CMD_MOTOR) != 0; }
    // Смещение головки в сырой дорожке — видно, как вращается диск.
    int  headPos() const;

private:
    void prepareTrack();                  // распаковать текущую дорожку в сырой вид
    void flushTrack();                    // собрать сектора обратно в образ
    Drive* cur() { return (drive_ >= 0) ? &drives_[drive_] : nullptr; }
    const Drive* cur() const { return (drive_ >= 0) ? &drives_[drive_] : nullptr; }

    Drive drives_[DRIVES];
    int      drive_ = -1;        // выбранный привод, -1 — ни одного
    int      track_ = 0, side_ = 0;
    uint16_t status_ = 0;
    uint16_t flags_  = 0;        // сохраняемые разряды регистра управления
    uint16_t dataReg_ = 0;       // регистр данных (чтение)
    uint16_t writeReg_ = 0, shiftReg_ = 0;
    bool writeFlag_ = false, shiftFlag_ = false;
    bool writeMarker_ = false, shiftMarker_ = false;
    bool writing_ = false;       // true — режим записи
    bool gdrTrigger_ = false;    // разряд GDR взведён; поиск начнётся по его снятию
    bool searchSync_ = false;    // ищем маркер
    bool crcCalc_ = false;       // идёт подсчёт CRC
    Stats stats_;
    static constexpr size_t LOG_CAP = 4096;
    void note(LogEntry::Kind k, uint16_t v);
    std::vector<LogEntry> log_;
    bool logOn_ = false;
    uint16_t ctxPc_ = 0;
    size_t logPos_ = 0;          // куда писать, когда кольцо заполнено
    int  fieldWords_ = 0;        // сколько слов взято из текущего поля
    bool fieldIsData_ = false;   // текущее поле — поле данных
};

} // namespace bk
