#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Магнитофон БК: кассета — звуковой файл .WAV.
//
// Сигнал проходит «компаратор входа» (tapeBinarize: огибающие + гистерезис, как
// разделительный конденсатор и компаратор на входе БК) и дальше живёт как 1 бит на
// сэмпл. Магнитофон (Tape) отдаёт этот бит в разряд 5 регистра 0177716 по
// эмулируемому времени — запись читает сам драйвер ПЗУ или любой свой загрузчик.
// Декодер (tapeDecode) разбирает тот же поток так, как это делает драйвер EMT 36
// монитора (ПЗУ 116640–117460): пилот-тон, маркер, «1», заглавие, маркер, массив и КС;
// плюс турбо-поток загрузчика Maxim (maxim-loader, конвертер thesands.ru). Им
// .wav грузится сразу, как .BIN, и из него извлекаются файлы.

namespace bk {

// Файл на ленте, найденный декодером.
struct TapeFile {
    enum class Kind : uint8_t { Standard, MaximTurbo };
    Kind kind = Kind::Standard;
    uint8_t rawName[16]{};        // имя как на ленте (экранные коды БК)
    std::string name;             // оно же в UTF-8, без хвостовых пробелов
    uint16_t addr = 0, len = 0;   // из заглавия (у турбо — из кода загрузчика)
    std::vector<uint8_t> data;    // прочитанное тело (короче len, если запись оборвалась)
    uint16_t crcTape = 0, crcCalc = 0;
    bool complete = false;        // тело и КС прочитаны целиком
    bool inverted = false;        // фаза сигнала (ячейка 300 драйвера: 377 — инверсный)
    bool maximLoader = false;     // загрузчик Maxim: за ним сразу турбо-поток
    uint16_t entry = 0;           // турбо: адрес пуска из загрузчика
    uint32_t start = 0, end = 0;  // сэмплы: начало пилот-тона и конец записи
    double periodUs = 0;          // период пилот-тона, мкс
    bool crcOk() const { return complete && crcTape == crcCalc; }
};

// Кассета: сигнал после компаратора (1 байт на сэмпл) и найденные на ней файлы.
struct TapeImage {
    std::string path;
    int rate = 0;
    std::vector<uint8_t> bits;
    std::vector<TapeFile> files;
    double seconds() const { return rate ? double(bits.size()) / rate : 0.0; }
    // Что грузить, когда .wav открыт как программа: тело турбо-записи, иначе первый
    // файл с верной КС, иначе первый прочитанный. -1 — файлов нет.
    int mainProgram() const;
};

// WAV (PCM 8/16/24/32, float 32/64, WAVE_FORMAT_EXTENSIBLE) — в моно [-1, 1].
bool readWavMono(const std::string& path, int& rate, std::vector<float>& out, std::string& err);
void tapeBinarize(const std::vector<float>& x, int rate, std::vector<uint8_t>& bits);
std::vector<TapeFile> tapeDecode(const std::vector<uint8_t>& bits, int rate);
// Кассета из сэмплов (для тестов и источников не из файла).
std::shared_ptr<const TapeImage> makeTapeImage(const std::vector<float>& x, int rate,
                                               const std::string& path = {});
// Кассета из файла; последняя прочитанная кэшируется (путь + время изменения).
std::shared_ptr<const TapeImage> loadTapeImage(const std::string& path, std::string& err);

// «x.wav» или «x.wav#N» (N-й файл ленты, с 1). index = 0 — без номера. false — не WAV.
bool parseTapeSpec(const std::string& spec, std::string& path, int& index);
// КС ленты: сумма байтов с переносом из 16-го разряда (ADD + ADC драйвера).
uint16_t tapeChecksum(const uint8_t* d, size_t n);
// Имя файла хоста для извлечённого файла: имя с ленты или FILE<N>.
std::string tapeHostName(const TapeFile& f, int index);
// Оглавление кассеты текстом (номера с 1): имя, адрес, длина, КС, время, вид записи.
std::string tapeCatalog(const TapeImage& img);
// Файл кассеты как .BIN (адрес, длина, тело). false — не записался.
bool saveTapeFileBin(const TapeFile& f, const std::string& path);

// Магнитофон. Время — такты ЦП платы; позиция — в сэмплах кассеты.
class Tape {
public:
    void insert(std::shared_ptr<const TapeImage> img, uint64_t now);
    void eject();
    bool loaded() const { return img_ != nullptr; }
    const TapeImage* image() const { return img_.get(); }
    std::shared_ptr<const TapeImage> share() const { return img_; }

    void setClock(int cpuHz, uint64_t now);
    void rebase(uint64_t now);               // счёт тактов платы начат заново (сброс)

    // Разряд 5 регистра 0177716.
    bool level(uint64_t now) const;
    // Запись в регистр: разряд 7 = 0 — двигатель включён (ПЗУ: 020 — пуск, 0220 — стоп).
    void setMotor(bool on, uint64_t now);
    // Чтение регистра. tapeDriver — читает драйвер магнитофона ПЗУ: с автопуском
    // лента пускается сама, будто кнопку «Воспроизведение» нажали, когда БК начала слушать.
    void noteRead(uint64_t now, bool tapeDriver);

    void poll(uint64_t now);                 // конец ленты — «Стоп» (раз в кадр)
    void play(uint64_t now);                 // кнопка «Воспроизведение»
    void stop(uint64_t now);                 // «Стоп» (позиция остаётся)
    void seek(uint64_t sample, uint64_t now);
    uint64_t position(uint64_t now) const;   // сэмпл
    double seconds(uint64_t now) const;
    uint64_t length() const { return img_ ? img_->bits.size() : 0; }

    bool playing() const { return playing_; }
    bool motor() const { return motor_; }
    bool moving(uint64_t now) const;         // лента идёт: пуск (и двигатель при ДУ), не конец
    bool autoStarted() const { return auto_; }

    // ДУ — двигатель управляется разрядом 7 (лента идёт только при включённом).
    bool remote() const { return remote_; }
    void setRemote(bool on, uint64_t now);
    bool autoStart() const { return autoStart_; }
    void setAutoStart(bool on) { autoStart_ = on; }
    bool inverted() const { return invert_; }
    void setInverted(bool on) { invert_ = on; }

    // Номер файла под головкой или последнего пройденного; -1 — до первого.
    int fileAt(uint64_t sample) const;

    // Снимок: позиция, кнопки, двигатель, ДУ.
    void saveState(std::vector<uint8_t>& out, uint64_t now) const;
    void loadState(const uint8_t* p, size_t n, uint64_t now);
    static constexpr size_t STATE_SIZE = 8 + 2;

private:
    double posAt(uint64_t now) const;
    void anchor(uint64_t now);               // зафиксировать позицию перед сменой хода

    std::shared_ptr<const TapeImage> img_;
    int cpuHz_ = 3000000;
    double pos_ = 0;                         // позиция в момент anchorTick_
    uint64_t anchorTick_ = 0;
    bool playing_ = false, motor_ = false, auto_ = false;
    bool remote_ = false, autoStart_ = true, invert_ = false;
};

} // namespace bk
