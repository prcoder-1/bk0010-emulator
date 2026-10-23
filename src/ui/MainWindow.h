#pragma once
#include <QMainWindow>
#include <memory>
#include <deque>
#include <set>
#include <vector>
#include <utility>
#include <map>
#include "Board.h"
#include "BkKeymap.h"
#include "Gamepad.h"

class GlScreen;
class DebuggerOverlay;
class MemVisWidget;
class SmkRamWidget;
class BkRamWidget;
class HotPathWidget;
class CallGraphWidget;
class FlameWidget;
class FlameChartWidget;
class HotChartWidget;
class ProfilerView;
class AudioOut;
namespace bk { class VideoCapture; }
#include <QElapsedTimer>
#include <csignal>
class QAction;

class QTimer;
class QLabel;

// Main application window: hosts the GL screen, drives the emulation at 50 Hz
// and provides the File/Emulation menus (load .BIN, reset, screen mode).
// Контроллер AZБК из командной строки: on (-1 — настройка машины), карта SD (пусто —
// настройка, иначе <ПЗУ>/azbk), диски AZn, запись в образы, доработка 037 (-1 — настройка).
struct AzCliOptions {
    int on = -1;
    QString sd;
    std::map<int, QString> units;
    bool rw = false;
    int mod037 = -1;
    QString net;                           // кабель сетевой карты (пусто — настройка, «-» — нет)
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    using AzOptions = AzCliOptions;
    // smkOverride: 1 — включить блок СМК-512, 0 — выключить, -1 — взять
    // сохранённую настройку (ключ командной строки перекрывает её, но не
    // перезаписывает: настройка меняется только переключателем в меню).
    // aspectOverride — то же для неквадратных пикселей БК (--aspect34/--no-aspect34).
    // choice — выбор машины (ключ --machine или настройка, см. savedMachineChoice),
    // machine — машина, с которой стартуем (для auto её уже выбрала программа из
    // командной строки). ayOverride — звуковой сопроцессор (-1 — настройка этой машины);
    // ay2Override и covoxOverride — то же для второго AY (плата «2AY») и Covox.
    explicit MainWindow(const QString& romDir, bk::MachineChoice choice, bk::Machine machine,
                        int smkOverride = -1, int aspectOverride = -1,
                        int ayOverride = -1, int ay2Override = -1, int covoxOverride = -1,
                        const AzOptions& az = AzOptions(), QWidget* parent = nullptr);
    // Выбор машины из настроек (меню «Машина»), по умолчанию — auto.
    static bk::MachineChoice savedMachineChoice();
    // AZБК: стоит ли плата по настройке (своя у каждой машины) и куда пишет БК (EEPROM,
    // файлы): с --az-rw — на саму карту, иначе в каталог данных эмулятора.
    static bool savedAzbk(bool bk11);
    static QString azWriteDir(const QString& sdDir, bool rw);
    ~MainWindow() override;

    bool loadBinFromPath(const QString& path);
    // Вставить образ диска и, для привода A, отдать управление автозагрузчику
    // ПЗУ контроллера. Тем же путём идут и меню, и образ из командной строки.
    bool insertDiskAndBoot(const QString& path, int drive = 0);
    // --disk-rw: образы вставляются без защиты записи; изменённые пишутся обратно в
    // файл при выходе и перед заменой дискеты
    void setDiskWritable(bool on) { diskRw_ = on; }
    // --disk-rom: прошивка контроллера (smk512.rom, disk326.rom или свой файл).
    void setDiskRom(const QString& nameOrPath) { board_->setDiskRom(nameOrPath.toStdString()); }
    // Подключить образ винчестера (HDI) к плате СМК-512: unit 0 — master, 1 — slave.
    // boot — включить питание, чтобы прошивка платы загрузилась с винчестера. Плату
    // ставит сама на этот запуск, если её нет (настройку не меняет).
    bool attachHdd(const QString& path, int unit = 0, bool boot = true);
    void setHddWritable(bool on) { hddRw_ = on; }   // --hdd-rw: изменения — в файл
    void setHddController(bk::Board::HddController c);   // --hdd-ctl
    // Запись экрана в видео (меню «Видео», --video). Пустой path — спросить имя;
    // пустой codec, aspect < 0 и audio < 0 — из настроек.
    bool startVideo(const QString& path, const QString& codec = QString(), int aspect = -1, int audio = -1);
    void stopVideo();
    // Из обработчика сигнала: закрыть окно на следующем тике таймера.
    static void requestQuit() { quitRequested_ = 1; }

    // Потактовая эмуляция арбитража КР1801ВП1-037 (ожидания доступа к ДОЗУ).
    void setArbitration(bool on) { if (board_) board_->setArbitration(on); }
    void setCpuPhase(int p) { if (board_) board_->setCpuPhase(p); }
    void setSmk512(bool on);            // подключить/снять плату и перезапустить машину
    void setAzbk(bool on);              // контроллер AZБК: установить/снять, перезапуск
    void setAzVga(bool on);             // показывать VGA платы (иначе — экран БК)
    void setPixelAspect34(bool on);     // пиксель БК блоком 4x3 точек хоста
    // Выбор машины в меню (сохраняется между запусками): явная машина включается сразу,
    // auto — машину выбирает загруженная программа.
    void setMachineChoice(bk::MachineChoice c);
    void setAy(bool on);                // звуковой сопроцессор AY-3-8910 на 0177714
    void setAy2(bool on);               // второй AY (плата «2AY»: TurboSound / «Грифон»)
    void setCovox(bool on);             // Covox — ЦАП на 0177714
    // Построчная отрисовка: -1 — по машине (БК-0011М — вкл), 0/1 — принудительно.
    void setScanlineOverride(int v) { scanlineOverride_ = v; applyScanline(); }
    void setMstd(bool on) { if (board_ && on != board_->memory().mstd()) { board_->setMstd(on); resetMachine(); } }

protected:
    bool event(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void closeEvent(QCloseEvent* e) override;   // auto-save annotations

private slots:
    void onTick();
    void runOneSlice(int kSlices);   // один слайс кадра + раз в кадр — отрисовка
    void openBin();
    void resetMachine();
    void toggleColorMode();
    void openDisk(int drive = 0);
    void openMemVis();
    void openSmkRam();
    void openBkRam();
    void openAzRam();
    void openHotPath();
    void openCallGraph();
    void openFlame();
    void openFlameChart();
    void openHotChart();
    void saveState();
    void loadState();

private:
    void updateTitle();
    void renderScreen();
    // Linked highlighting: relay a hovered address to every profiler window (and
    // the debugger's disassembler) except the one it came from.
    void broadcastHighlight(int addr, ProfilerView* src);
    // Окно профилировщика создаётся при первом открытии и попадает в profilers_; трасса
    // включается (flame/spans — ещё дерево вызовов и хронология).
    template <class W> void openProfiler(W*& w, QSize size, bool flame = false, bool spans = false);
    void setPaused(bool paused);
    void setSuspended(bool suspended);   // simple pause (Pause key), no debugger
    void stepInto();
    void stepOver();
    void runToCursor();                  // F4: прогнать до инструкции под курсором
    // Interactive-disassembler annotations (symbols + comments).
    void nameCursorSymbol();             // N: name/rename the symbol at the cursor
    void commentCursor();                // ;: edit the comment at the cursor
    void markData(bk::DataType t);       // B/W/S/P: mark the cursor as data of a type
    void gotoDialog();                   // G: jump to a symbol (from a list) or a typed address
    void xrefsDialog();                  // X: list who references the cursor address, jump to one
    void saveAnnotations(const QString& path);
    void loadAnnotations(const QString& path);
    QString annotationsPath() const;     // "<loaded .bin>.bkdb", or empty

    std::unique_ptr<bk::Board> board_;
    GlScreen* screen_ = nullptr;
    DebuggerOverlay* overlay_ = nullptr;
    MemVisWidget* memvis_ = nullptr;
    SmkRamWidget* smkram_ = nullptr;
    BkRamWidget* bkram_ = nullptr;
    class AzRamWidget* azram_ = nullptr;
    HotPathWidget* hotpath_ = nullptr;
    CallGraphWidget* callgraph_ = nullptr;
    FlameWidget* flame_ = nullptr;
    FlameChartWidget* flamechart_ = nullptr;
    HotChartWidget* hotchart_ = nullptr;
    std::vector<ProfilerView*> profilers_;   // все открывавшиеся окна профилировщика
    AudioOut* audio_ = nullptr;
    QTimer* timer_ = nullptr;
    QLabel* status_ = nullptr;
    QAction* smkAction_ = nullptr;   // галка «СМК-512» — синхронизируется при загрузке снимка
    QAction* machineAutoAction_ = nullptr; // радиопункты меню «Машина»
    QAction* machine10Action_ = nullptr;
    QAction* machine11Action_ = nullptr;
    bk::MachineChoice machineChoice_ = bk::MachineChoice::Auto;
    // Включить другую машину — как выключить одну и включить другую: ПЗУ, частота, карта
    // памяти, звук и плата по её настройкам; загруженная программа забывается.
    void switchMachine(bk::Machine m);
    QAction* ayAction_ = nullptr;          // галка «AY-3-8910»
    QAction* ay2Action_ = nullptr;         // галка «второй AY»
    QAction* covoxAction_ = nullptr;       // галка «Covox»
    // Ключи настроек звука — свои у каждой машины; по умолчанию всё включено на 11М.
    QString soundKey(const char* base) const { return QString(base) + ((board_ && board_->bk11()) ? "11m" : "10"); }
    void applySoundSettings(int ayOverride = -1, int ay2Override = -1, int covoxOverride = -1);
    int  scanlineOverride_ = -1;
    // Ключ настройки «плата СМК-512 стоит» — свой у каждой машины.
    QString smkKey() const { return (board_ && board_->bk11()) ? "smk512_11m" : "smk512"; }
    void applyScanline();
    // AZБК: настройка «плата стоит» своя у каждой машины; карта SD, диск AZ0 и доработка
    // 037 — общие. Ключи командной строки перекрывают их на запуск.
    QString azKey() const { return (board_ && board_->bk11()) ? "azbk_11m" : "azbk"; }
    bk::Azbk::Config azConfig() const;
    bool applyAz(bool on);              // поставить/снять плату (без сброса); false — нет карты SD
    void chooseAzSd();
    void chooseAzUnit0();
    void chooseAzNet();
    AzOptions azCli_;
    QString romDir_;
    QAction* azAction_ = nullptr;          // галка «AZБК»
    QAction* az037Action_ = nullptr;       // галка «доработка 037» (БК-0010)
    QAction* azVgaAction_ = nullptr;       // галка «экран VGA AZБК»
    void syncMachineMenu();                // галки меню — по состоянию платы (без сигналов)
    QAction* aspectAction_ = nullptr;   // галка «Пиксели 3:4»
    QString lastBin_;
    // С чего машина запущена — сброс (Ctrl+R, смена платы или машины) повторяет этот пуск.
    enum class Boot { None, Bin, Disk, Hdd };
    Boot boot_ = Boot::None;
    void forgetBin();       // программа больше не та: сохранить её разметку и убрать
    QString lastDisk_;      // последний вставленный образ диска
    bool diskRw_ = false;
    QString lastHdd_;       // последний подключённый образ винчестера
    bool hddRw_ = false;
    std::vector<std::pair<QAction*, bk::Board::HddController>> hddCtlActions_;
    std::unique_ptr<bk::VideoCapture> video_;
    QAction* videoAction_ = nullptr;
    QLabel* videoLabel_ = nullptr;
    bool frameDone_ = false;          // за этот круг слайсов досчитан кадр БК
    bool frameRec_ = false;           // кадр начат при идущей записи — он попадёт в видео
    std::vector<int16_t> videoAudio_; // звук кадра для записи
    std::vector<uint32_t> vgaFrame_;  // экран БК, растянутый до кадра VGA (запись с AZБК)
    static inline volatile std::sig_atomic_t quitRequested_ = 0;   // запрос закрытия из обработчика сигнала
    void updateVideoLabel();
    void openHdd(int unit);
    void detachHdds();
    void saveDisks();
    BkKeymap keymap_;
    Gamepad gamepad_;           // джойстик на порту 0177714 через SDL2-геймпад
    // Host-side typing buffer: a keypress may translate to more than one BK code
    // (e.g. a РУС/ЛАТ switch + the character). Because the BK register holds only
    // one code at a time, we feed codes one per frame as the register frees up.
    std::deque<uint16_t> keyFeed_;
    // Клавиши матрицы (дающие код БК), физически нажатые — без автоповтора. Как у
    // 1801ВП1-014 (gate netlist, OneChipBook sim/ref014): разряд 6 0177716 «нажата»,
    // пока замкнута ЛЮБАЯ из них; новая клавиша при уже нажатой кода не даёт, и когда
    // первую отпускают, вторую контроллер заново не узнаёт. Модификаторы (АР2, СУ…)
    // и СТОП — не клавиши матрицы.
    std::set<int> heldKeys_;
    void releaseAllKeys();
    bool colorMode_ = true;
    bool sizeFitted_ = false;   // окно уже подогнано под целый масштаб экрана
    bool paused_ = false;       // Soft-ICE debugger overlay active
    bool suspended_ = false;    // emulation frozen via the Pause key
    int  phase_ = 0;            // emulation sub-slice within the current frame
    // Темп эмуляции привязан к РЕАЛЬНОМУ времени, а не к числу срабатываний таймера:
    // кадр БК длится 61440 тактов = 20,48 мс, что целым числом миллисекунд в QTimer
    // не выражается. Считаем, сколько слайсов должно было пройти к текущему моменту.
    QElapsedTimer clock_;
    qint64 slicesDone_ = 0;
};
