#include "MainWindow.h"
#include "GlScreen.h"
#include "DebuggerOverlay.h"
#include "MemVisWidget.h"
#include "SmkRamWidget.h"
#include "BkRamWidget.h"
#include "HotPathWidget.h"
#include "CallGraphWidget.h"
#include "FlameWidget.h"
#include "FlameChartWidget.h"
#include "HotChartWidget.h"
#include "AudioOut.h"
#include "Disasm.h"
#include "VideoCapture.h"
#include "VideoSink.h"
#include <QDateTime>
#include <QStandardPaths>
#include <vector>
#include <QMenuBar>
#include <QMenu>
#include <QActionGroup>
#include <QAction>
#include <QFileDialog>
#include <QMessageBox>
#include <QTimer>
#include <QStatusBar>
#include <QLabel>
#include <QFileInfo>
#include <QKeySequence>
#include <QKeyEvent>
#include <QInputDialog>
#include <QLineEdit>
#include <QStringList>
#include <QCloseEvent>
#include <QShowEvent>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSettings>
#include <QSignalBlocker>
#include <QProxyStyle>
#include <cstdio>

MainWindow::MainWindow(const QString& romDir, int smkOverride, int aspectOverride,
                       int machineOverride, int ayOverride, int ay2Override, int covoxOverride,
                       QWidget* parent)
    : QMainWindow(parent) {
    board_ = std::make_unique<bk::Board>();
    // Машина: настройка запоминается между запусками, --machine перекрывает её.
    const int machine = machineOverride >= 0 ? machineOverride
                                             : QSettings().value("machine", 1).toInt();
    board_->setMachine(machine == 1 ? bk::Machine::Bk0011m : bk::Machine::Bk0010);
    applySoundSettings(ayOverride, ay2Override, covoxOverride);
    applyScanline();
    // Блок расширения памяти СМК-512: настройка запоминается между запусками,
    // ключ --smk/--no-smk перекрывает её на один запуск.
    const bool smkOn = smkOverride >= 0 ? smkOverride > 0
                                        : QSettings().value(smkKey(), true).toBool();
    board_->setSmk512(smkOn);
    if (board_->smk512()) {
        // Стоит сказать вслух: с платой карта верхней половины адресов не штатная
        // (в режиме SYS ОЗУ контроллера перекрывает ПЗУ Бейсика), и это объясняет
        // «странности», если пользователь про плату забыл.
        std::fprintf(stderr, "%s: блок расширения памяти СМК-512 подключён (512 Кбайт)\n",
                     bk::Board::machineName(board_->machine()));
    }
    if (!board_->loadRoms(romDir.toStdString())) {
        QMessageBox::warning(this, "БК",
            QString("Не удалось загрузить ПЗУ из:\n%1\nПоложите в эту папку %2.")
                .arg(romDir).arg(board_->bk11()
                    ? "bk11m_324_bos.rom (и bk11m_325_ext, bk11m_327..329_basic*.rom)"
                    : "monit10.rom (и basic10.rom)"));
    }
    board_->reset();

    screen_ = new GlScreen(this);
    // Пиксель БК не квадратный: настройка запоминается между запусками,
    // ключ --aspect34/--no-aspect34 перекрывает её на один запуск.
    const bool aspect34 = aspectOverride >= 0 ? aspectOverride > 0
                                              : QSettings().value("aspect34", true).toBool();
    screen_->setPixelAspect34(aspect34);
    setCentralWidget(screen_);
    // Keyboard goes to the main window; the GL widget must not steal focus.
    screen_->setFocusPolicy(Qt::NoFocus);
    setFocusPolicy(Qt::StrongFocus);

    overlay_ = new DebuggerOverlay(board_.get(), screen_);
    overlay_->setGeometry(screen_->rect());
    overlay_->hide();

    // --- Menus ---
    // Alt — это АР2 БК. Одиночное нажатие Alt не должно уводить фокус в меню.
    struct NoAltNavStyle : QProxyStyle {
        int styleHint(StyleHint h, const QStyleOption* o, const QWidget* w,
                      QStyleHintReturn* r) const override {
            return h == SH_MenuBar_AltKeyNavigation ? 0 : QProxyStyle::styleHint(h, o, w, r);
        }
    };
    auto* menuStyle = new NoAltNavStyle;
    menuStyle->setParent(menuBar());
    menuBar()->setStyle(menuStyle);
    QMenu* file = menuBar()->addMenu("&Файл");
    file->addAction("&Загрузить .BIN…", this, &MainWindow::openBin, QKeySequence::Open);
    file->addAction("Вставить образ &диска… (привод A)", this,
                    [this] { openDisk(0); }, QKeySequence("Ctrl+B"));
    file->addAction("Вставить образ диска в привод &B…", this, [this] { openDisk(1); });
    file->addSeparator();
    file->addAction("Подключить &винчестер (HDI)…", this, [this] { openHdd(0); });
    file->addAction("Подключить второй винчестер (slave)…", this, [this] { openHdd(1); });
    file->addAction("Отключить винчестеры", this, &MainWindow::detachHdds);
    // Контроллер винчестера: по разметке образа или явно. Действует со следующего
    // подключения образа.
    QMenu* hddCtl = file->addMenu("Контроллер винчестера");
    auto* hddGroup = new QActionGroup(this);
    const std::pair<const char*, bk::Board::HddController> ctls[] = {
        {"По разметке образа (авто)", bk::Board::HddController::Auto},
        {"СМК-512 (0177740–0177757)", bk::Board::HddController::Smk},
        {"«Самара» (0177620–0177656)", bk::Board::HddController::Samara}};
    for (const auto& c : ctls) {
        QAction* a = hddCtl->addAction(c.first);
        a->setCheckable(true);
        a->setChecked(c.second == board_->hddController());
        hddGroup->addAction(a);
        const bk::Board::HddController v = c.second;
        connect(a, &QAction::triggered, this, [this, v] { board_->setHddController(v); });
        hddCtlActions_.push_back({a, v});
    }
    file->addSeparator();
    file->addAction("В&ыход", this, &QWidget::close, QKeySequence::Quit);

    QMenu* emu = menuBar()->addMenu("&Эмуляция");
    emu->addAction("&Сброс", this, &MainWindow::resetMachine, QKeySequence("Ctrl+R"));
    emu->addAction("Режим &цвет/ч-б", this, &MainWindow::toggleColorMode, QKeySequence("F10"));
    // Пиксель БК шире, чем выше: кадр ложится на телевизионное поле 4:3. Включённый
    // режим рисует пиксель цветного режима блоком 4x3 точек, выключённый — квадратом.
    aspectAction_ = emu->addAction("Пиксели &3:4 (как на телевизоре)");
    aspectAction_->setCheckable(true);
    aspectAction_->setChecked(aspect34);
    aspectAction_->setShortcut(QKeySequence("F11"));
    connect(aspectAction_, &QAction::toggled, this, &MainWindow::setPixelAspect34);
    emu->addAction("&Пауза", this, [this]{ setSuspended(!suspended_); }, QKeySequence(Qt::Key_Pause));
    emu->addAction("&Отладчик (Soft-ICE)", this, [this]{ setPaused(!paused_); }, QKeySequence("F12"));

    // --- Машина: БК-0010-01 или БК-0011М и её оборудование: СМК-512, AY, джойстик ---
    QMenu* mach = menuBar()->addMenu("&Машина");
    auto* machGroup = new QActionGroup(this);
    machGroup->setExclusive(true);
    machine10Action_ = mach->addAction("БК-00&10-01 (3 МГц, 32 Кбайт ОЗУ)");
    machine11Action_ = mach->addAction("БК-00&11М (4 МГц, 128 Кбайт ОЗУ, 16 палитр)");
    for (auto* a : {machine10Action_, machine11Action_}) { a->setCheckable(true); machGroup->addAction(a); }
    connect(machine10Action_, &QAction::triggered, this, [this] { setMachine(0); });
    connect(machine11Action_, &QAction::triggered, this, [this] { setMachine(1); });
    mach->addSeparator();
    // Блок расширения памяти СМК-512 («АльтПро») в разъёме МПИ. На живой машине
    // плату ставят только при выключенном питании, поэтому переключатель
    // перезапускает машину — раскладка верхней половины адресов меняется целиком.
    smkAction_ = mach->addAction("Блок расширения памяти &СМК-512 (512 Кбайт)");
    smkAction_->setCheckable(true);
    smkAction_->setChecked(board_->smk512());
    connect(smkAction_, &QAction::toggled, this, [this](bool on) { setSmk512(on); });
    ayAction_ = mach->addAction("Звуковой сопроцессор &AY-3-8910 / YM2149 (порт 0177714)");
    ayAction_->setCheckable(true);
    connect(ayAction_, &QAction::toggled, this, [this](bool on) { setAy(on); });
    ay2Action_ = mach->addAction("Второй AY — плата «&2AY» (TurboSound / «Грифон»)");
    ay2Action_->setCheckable(true);
    connect(ay2Action_, &QAction::toggled, this, [this](bool on) { setAy2(on); });
    covoxAction_ = mach->addAction("&Covox — ЦАП на порту 0177714");
    covoxAction_->setCheckable(true);
    connect(covoxAction_, &QAction::toggled, this, [this](bool on) { setCovox(on); });
    mach->addSeparator();
    // Джойстик: геймпад через SDL2, порт 0177714. Раскладка выбирается — у БК их две
    // (несовместимые). Если геймпад подключён — покажем его имя в подменю.
    QMenu* joy = mach->addMenu("&Джойстик (геймпад)");
    auto* joyGroup = new QActionGroup(this);
    joyGroup->setExclusive(true);
    auto* aStd = joy->addAction("Раскладка «Стандарт» (Джойвокс/gid, по умолч.)");
    auto* aBH  = joy->addAction("Раскладка «Break House»");
    auto* aSW  = joy->addAction("Раскладка «SWCorp»");
    auto* aKl  = joy->addAction("Раскладка «Клад-2»");
    for (auto* a : {aStd, aBH, aSW, aKl}) { a->setCheckable(true); joyGroup->addAction(a); }
    aStd->setChecked(true);
    connect(aStd, &QAction::triggered, this, [this]{ gamepad_.setStandard(Gamepad::Standard::Standard); });
    connect(aBH,  &QAction::triggered, this, [this]{ gamepad_.setStandard(Gamepad::Standard::BreakHouse); });
    connect(aSW,  &QAction::triggered, this, [this]{ gamepad_.setStandard(Gamepad::Standard::SWCorp); });
    connect(aKl,  &QAction::triggered, this, [this]{ gamepad_.setStandard(Gamepad::Standard::Klad2); });
    joy->addSeparator();
    QAction* joyStatus = joy->addAction(gamepad_.connected()
        ? QString::fromStdString("Подключён: " + gamepad_.name())
        : QString::fromUtf8("Геймпад не найден"));
    joyStatus->setEnabled(false);
    syncMachineMenu();

    // Запись экрана в видео. Кодек и размер кадра — настройки, действуют со следующей
    // записи; ключи --video-codec / --video-size перекрывают их на один запуск.
    QMenu* vid = menuBar()->addMenu("&Видео");
    videoAction_ = vid->addAction("&Записывать видео…");
    videoAction_->setCheckable(true);
    videoAction_->setShortcut(QKeySequence("Ctrl+F5"));
    connect(videoAction_, &QAction::triggered, this, [this](bool on) {
        if (on) startVideo(QString(), QString(), -1); else stopVideo();
    });
    QMenu* codecMenu = vid->addMenu("&Кодек");
    auto* codecGroup = new QActionGroup(this);
    const QString curCodec = QSettings().value("video/codec", QString::fromStdString(bk::videoCodecList().front().name)).toString();
    for (const auto& c : bk::videoCodecList()) {
        const QString name = QString::fromStdString(c.name);
        QAction* a = codecMenu->addAction(QString::fromStdString(c.title));
        a->setCheckable(true);
        a->setChecked(name == curCodec);
        codecGroup->addAction(a);
        connect(a, &QAction::triggered, this, [name] { QSettings().setValue("video/codec", name); });
    }
    QMenu* sizeMenu = vid->addMenu("Кадр &видео");
    auto* sizeGroup = new QActionGroup(this);
    const bool cur43 = QSettings().value("video/aspect43", true).toBool();
    for (int v = 1; v >= 0; --v) {
        QAction* a = sizeMenu->addAction(v ? "1024×768 — точка БК блоком 2×3 (4:3, как в окне)"
                                           : "512×256 — как буфер экрана, с пометкой «показывать 4:3» (только AVI)");
        a->setCheckable(true);
        a->setChecked(cur43 == (v == 1));
        sizeGroup->addAction(a);
        connect(a, &QAction::triggered, this, [v] { QSettings().setValue("video/aspect43", v == 1); });
    }
    QAction* vAudio = vid->addAction("Записывать &звук");
    vAudio->setCheckable(true);
    vAudio->setChecked(QSettings().value("video/audio", true).toBool());
    connect(vAudio, &QAction::toggled, this, [](bool on) { QSettings().setValue("video/audio", on); });

    QMenu* dbg = menuBar()->addMenu("&Отладка");
    dbg->addAction("&Визуализация памяти…", this, &MainWindow::openMemVis, QKeySequence("Ctrl+I"));
    dbg->addAction("&ДОЗУ СМК-512 по страницам…", this, &MainWindow::openSmkRam, QKeySequence("Ctrl+D"));
    dbg->addAction("Память &БК по страницам…", this, &MainWindow::openBkRam, QKeySequence("Ctrl+P"));
    dbg->addAction("Горячий &путь…", this, &MainWindow::openHotPath, QKeySequence("Ctrl+G"));
    dbg->addAction("Граф &вызовов…", this, &MainWindow::openCallGraph, QKeySequence("Ctrl+K"));
    dbg->addAction("&Пламенный граф…", this, &MainWindow::openFlame, QKeySequence("Ctrl+F"));
    dbg->addAction("&Хронология вызовов…", this, &MainWindow::openFlameChart, QKeySequence("Ctrl+T"));
    dbg->addAction("Горячие инструкции во &времени…", this, &MainWindow::openHotChart, QKeySequence("Ctrl+H"));
    dbg->addSeparator();
    dbg->addAction("&Сохранить состояние…", this, &MainWindow::saveState, QKeySequence("Ctrl+S"));
    dbg->addAction("В&осстановить состояние…", this, &MainWindow::loadState, QKeySequence("Ctrl+L"));
    dbg->addAction("Загрузить си&мволы (.map)…", this, [this] {
        QString path = QFileDialog::getOpenFileName(this, "Загрузить символы (linker .map)",
            lastBin_.isEmpty() ? QString() : lastBin_,
            "Карты/символы (*.map *.sym *.lst);;Все файлы (*)");
        if (path.isEmpty()) return;
        int n = board_->loadSymbols(path.toStdString());
        status_->setText(n < 0 ? QString("Не удалось открыть файл символов")
                               : QString("Загружено символов: %1").arg(n));
        overlay_->update();
    });
    dbg->addAction("Сохранить &аннотации (.bkdb)", this, [this] {
        QString p = annotationsPath();
        if (p.isEmpty()) p = QFileDialog::getSaveFileName(this, "Сохранить аннотации",
            QString(), "Аннотации БК (*.bkdb)");
        if (p.isEmpty()) return;
        saveAnnotations(p);
        status_->setText("Аннотации сохранены: " + p);
    });
    dbg->addAction("Загрузить а&ннотации (.bkdb)", this, [this] {
        QString p = QFileDialog::getOpenFileName(this, "Загрузить аннотации",
            annotationsPath(), "Аннотации БК (*.bkdb);;Все файлы (*)");
        if (p.isEmpty()) return;
        loadAnnotations(p);
        status_->setText("Аннотации загружены: " + p);
    });
    dbg->addAction("&Автоанализ процедур (JSR + трасса)", this, [this] {
        int n = overlay_->analyzeProcedures();
        status_->setText(QString("Автоанализ: добавлено процедур sub_… — %1").arg(n));
        if (paused_) overlay_->update();
    });

    status_ = new QLabel("Готов", this);
    statusBar()->addWidget(status_);
    videoLabel_ = new QLabel(this);
    videoLabel_->setStyleSheet("color: #d22; font-weight: bold;");
    videoLabel_->hide();
    statusBar()->addPermanentWidget(videoLabel_);

    // --- Таймер эмуляции. Кадр разбит на 4 слайса, чтобы звук подавался непрерывно,
    // а не одним всплеском на кадр (это давало слышимое «вобблирование»); экран и
    // профилировщик обновляются раз в кадр. Сам темп задаётся НЕ частотой таймера:
    // кадр БК = 61440 тактов = 20,48 мс, слайс = 5,12 мс, целым числом миллисекунд
    // это не выражается. Поэтому таймер только «будит» нас почаще, а сколько слайсов
    // исполнить — считается по реальному времени в onTick(). ---
    timer_ = new QTimer(this);
    timer_->setTimerType(Qt::PreciseTimer);
    connect(timer_, &QTimer::timeout, this, &MainWindow::onTick);
    clock_.start();
    timer_->start(4);

    // --- Audio (only if built with Qt6 Multimedia) ---
#ifdef HAVE_QT_MULTIMEDIA
    audio_ = new AudioOut(&board_->sound(), this);
    audio_->start();
    emu->addAction("Звук вкл/&выкл", this, [this]{
        static bool muted = false; muted = !muted;
        if (audio_) audio_->setVolume(muted ? 0.f : 0.6f);
        status_->setText(muted ? "Звук выключен" : "Звук включён");
    }, QKeySequence("Ctrl+M"));
#else
    board_->sound().setEnabled(false); // no sink; don't accumulate samples
#endif

    updateTitle();
    resize(1024, 768);
}

MainWindow::~MainWindow() { stopVideo(); }

// Запись видео. path пусто — спросить имя (по умолчанию bk_ГГГГММДД_ЧЧММСС.avi в папке
// прошлой записи); codec пусто и aspect < 0 — из настроек.
bool MainWindow::startVideo(const QString& path, const QString& codec, int aspect, int audio) {
    stopVideo();
    QString file = path;
    // Кодек: явный (--video-codec), иначе по расширению файла, иначе из настроек.
    std::string cname = codec.toStdString();
    if (cname.empty() && !file.isEmpty())
        if (const bk::VideoCodecInfo* c = bk::videoCodecForPath(file.toStdString())) cname = c->name;
    if (cname.empty()) cname = QSettings().value("video/codec", "zmbv").toString().toStdString();
    if (!bk::findVideoCodec(cname)) cname = bk::videoCodecList().front().name;   // настройка от другой сборки
    if (file.isEmpty()) {
        QSettings st;
        QString dir = st.value("video/dir").toString();
        if (dir.isEmpty()) dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
        const QString ext = QString::fromStdString(bk::findVideoCodec(cname)->ext);
        const QString def = dir + "/bk_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss") + ext;
        QStringList filters;
        QString sel;
        for (const auto& c : bk::videoCodecList()) {
            filters << QString("%1 (*%2)").arg(QString::fromStdString(c.title), QString::fromStdString(c.ext));
            if (c.name == cname) sel = filters.back();
        }
        file = QFileDialog::getSaveFileName(this, "Записывать видео в файл", def, filters.join(";;"), &sel);
        if (file.isEmpty()) { videoAction_->setChecked(false); return false; }
        // Расширение известного кодека решает, в чём писать; без него — дописать своё.
        if (const bk::VideoCodecInfo* c = bk::videoCodecForPath(file.toStdString())) cname = c->name;
        else file += ext;
        st.setValue("video/dir", QFileInfo(file).absolutePath());
    }
    if (const std::string why = bk::videoCodecUnavailableReason(cname); !why.empty()) {
        status_->setText(QString("Видео не записывается: %1").arg(QString::fromStdString(why)));
        videoAction_->setChecked(false);
        return false;
    }
    bk::VideoCapture::Options o;
    o.codec = cname;
    o.aspect43 = aspect < 0 ? QSettings().value("video/aspect43", true).toBool() : aspect != 0;
    // Звук — тот же поток, что идёт в колонки (пищалка, AY, Covox), но с отдельного
    // отвода: выключенный звук (Ctrl+M) запись не глушит. Отвод включается в начале
    // следующего кадра (runOneSlice), чтобы звук и картинка начались вместе.
    if (audio < 0 ? QSettings().value("video/audio", true).toBool() : audio != 0)
        o.audioRate = board_->sound().sampleRate();
    frameRec_ = false;
    if (!video_) video_ = std::make_unique<bk::VideoCapture>();
    std::string err;
    if (!video_->start(file.toStdString(), o, bk::Screen::TEX_W, bk::Screen::TEX_H,
                       static_cast<uint32_t>(board_->cpuFreqHz()), static_cast<uint32_t>(board_->ticksPerFrame()), &err)) {
        status_->setText(QString("Видео не записывается: %1 — %2").arg(file, QString::fromStdString(err)));
        videoAction_->setChecked(false);
        return false;
    }
    videoAction_->setChecked(true);
    status_->setText(QString("Запись видео: %1 (%2, %3×%4, %5 Гц%6)").arg(file, cname == "h264" ? "H.264 + AAC" : "ZMBV")
                         .arg(video_->outWidth()).arg(video_->outHeight())
                         .arg(double(board_->cpuFreqHz()) / board_->ticksPerFrame(), 0, 'f', 6)
                         .arg(o.audioRate ? QString(", звук %1 Гц").arg(o.audioRate) : QString(", без звука")));
    updateVideoLabel();
    return true;
}

void MainWindow::stopVideo() {
    if (!video_ || !video_->active()) return;
    const QString file = QString::fromStdString(video_->currentFile());
    const uint64_t n = video_->frames();
    const double sec = video_->seconds();
    const bool ok = video_->stop();
    board_->sound().setCapture(false);
    frameRec_ = false;
    if (videoAction_) videoAction_->setChecked(false);
    if (videoLabel_) videoLabel_->hide();
    if (status_)
        status_->setText(ok ? QString("Видео записано: %1 — %2 кадров, %3 с").arg(file).arg(n).arg(sec, 0, 'f', 1)
                            : QString("Ошибка записи видео: %1").arg(QString::fromStdString(video_->error())));
}

void MainWindow::updateVideoLabel() {
    if (!video_ || !video_->active()) return;
    const int s = static_cast<int>(video_->seconds());
    videoLabel_->setText(QString("● ЗАПИСЬ %1:%2").arg(s / 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0')));
    videoLabel_->show();
}

void MainWindow::onTick() {
    if (quitRequested_) { quitRequested_ = 0; close(); return; }
    static constexpr int kSlices = 4;   // слайсов на кадр
    // Длительность слайса в наносекундах: кадр 61440 тактов при 3 МГц (81920 при
    // 4 МГц у БК-0011М) = 20,48 мс.
    const qint64 kSliceNs = qint64(board_->ticksPerFrame()) * 1000000000LL
                          / (qint64(board_->cpuFreqHz()) * kSlices);
    // Сколько слайсов должно было пройти к этому моменту реального времени. Догоняем,
    // но не больше kCatchUp за раз — иначе после паузы (или тормоза системы) эмулятор
    // ушёл бы в долгую «отработку долга» и подвис бы интерфейс.
    static constexpr qint64 kCatchUp = kSlices * 2;
    qint64 due = clock_.nsecsElapsed() / kSliceNs;
    if (due - slicesDone_ > kCatchUp) slicesDone_ = due - kCatchUp;  // долг списываем
    int steps = static_cast<int>(due - slicesDone_);
    if (steps <= 0) return;
    slicesDone_ = due;
    while (steps-- > 0) runOneSlice(kSlices);
}

void MainWindow::runOneSlice(int kSlices) {
    if (!paused_ && !suspended_) {
        // Опросить геймпад раз в кадр и обновить байт джойстика (порт 0177714) до
        // прогона кадра, чтобы игра увидела актуальное состояние в своём ISR.
        if (phase_ == 0) board_->setJoystick(gamepad_.poll());
        // Подать один код из очереди раз в кадр. НЕ ждём освобождения регистра:
        // на реальной БК новое нажатие перезаписывает непрочитанный код (см.
        // Board::pressKey) — иначе игра, читающая 0177662 только при физически
        // нажатой клавише (PITON) или только по событию нажатия (LAND), получала
        // бы застрявший код ПРЕДЫДУЩЕЙ клавиши.
        if (phase_ == 0 && !keyFeed_.empty()) {
            board_->pressKey(keyFeed_.front());
            keyFeed_.pop_front();
        }
        // Запись видео: кадр попадает в файл, только если начался при идущей записи, —
        // тогда и звук для него набран с его первого такта.
        if (phase_ == 0 && video_ && video_->active()) {
            if (!frameRec_ && video_->hasAudio()) board_->sound().setCapture(true);
            frameRec_ = true;
        }
        board_->runFrameSlice(phase_, kSlices);
        if (phase_ == kSlices - 1) frameDone_ = frameRec_;   // кадр БК досчитан — в видео
        if (board_->breakHit()) {          // stopped at a breakpoint
            board_->clearBreakHit();
            setPaused(true);
        }
    }
    // The heavy per-frame work (screen render + profiler refresh) stays at 50 Hz:
    // run it once every kSlices ticks. While paused/suspended nothing is emulated,
    // but this still ticks so the overlay stays responsive.
    if (++phase_ < kSlices) return;
    phase_ = 0;
    renderScreen();
    if (frameDone_ && video_ && video_->active()) {
        if (video_->hasAudio()) board_->sound().takeCaptured(videoAudio_);
        video_->addFrame(board_->screen().pixels(), videoAudio_.data(), video_->hasAudio() ? videoAudio_.size() / 2 : 0);
        if (!video_->error().empty()) stopVideo();
        else if (video_->frames() % 25 == 0) updateVideoLabel();
    }
    frameDone_ = false;
    if (paused_) overlay_->update();
    if (memvis_ && memvis_->isVisible()) memvis_->refresh();
    if (smkram_ && smkram_->isVisible()) smkram_->refresh();
    if (bkram_ && bkram_->isVisible()) bkram_->refresh();
    if (hotpath_ && hotpath_->isVisible()) hotpath_->refresh();
    if (callgraph_ && callgraph_->isVisible()) callgraph_->refresh();
    if (flame_ && flame_->isVisible()) flame_->refresh();
    if (flamechart_ && flamechart_->isVisible()) flamechart_->refresh();
    if (hotchart_ && hotchart_->isVisible()) hotchart_->refresh();
}

void MainWindow::renderScreen() {
    board_->screen().setColorMode(colorMode_);
    if (!board_->scanlineRender()) board_->screen().render(board_->memory());
    screen_->setFrame(board_->screen().pixels());
}

void MainWindow::setPaused(bool paused) {
    paused_ = paused;
    phase_ = 0;                     // start the next run at a fresh frame boundary
    if (paused_) {
        overlay_->snapshotRegs();   // baseline for the changed-register highlight
        overlay_->setCursor(board_->cpu().pc());   // selection starts at PC
        overlay_->setGeometry(screen_->rect());
        overlay_->followPc();
        overlay_->show();
        overlay_->raise();
        status_->setText("ОТЛАДЧИК: F7 шаг, F8 через, F9 точка, Esc продолжить, F12 выход");
    } else {
        overlay_->hide();
        status_->setText("Выполнение");
    }
    // Entering/leaving the debugger drops any held keys so a game doesn't see a
    // key stuck down across the pause.
    heldKeys_.clear();
    board_->setKeyHeld(false);
    renderScreen();
}

// Simple pause via the Pause key: freezes the 50 Hz emulation loop without the
// Soft-ICE overlay. The last frame stays on screen; the status bar shows PAUSED.
void MainWindow::setSuspended(bool suspended) {
    suspended_ = suspended;
    phase_ = 0;                     // resume at a fresh frame boundary
    // Drop any held/buffered keys so nothing is stuck down or replayed on resume.
    heldKeys_.clear();
    keyFeed_.clear();
    board_->setKeyHeld(false);
    if (!paused_)  // don't fight the debugger for the status line
        status_->setText(suspended_ ? "ПАУЗА — нажмите Pause для продолжения"
                                     : "Выполнение");
}

void MainWindow::stepInto() {
    overlay_->snapshotRegs();   // so registers changed by this step are highlighted
    board_->stepInstruction();
    overlay_->setCursor(board_->cpu().pc());  // курсор — на исполняемую инструкцию
    overlay_->followPc();
    renderScreen();
    overlay_->update();
}

// Прогнать эмуляцию до инструкции под курсором и остановиться на ней (клавиша F4).
// Позволяет во время отладки поставить курсор ниже/выше и «дойти» туда автоматически.
// По пути честно срабатывают обычные точки останова; если за отведённый бюджет тактов
// курсор не достигнут, останавливаемся там, где оказались.
void MainWindow::runToCursor() {
    uint16_t target = overlay_->cursorAddr();
    if (target == board_->cpu().pc()) return;  // уже на месте
    overlay_->snapshotRegs();
    board_->runUntil(target, 60'000'000);
    board_->clearBreakHit();
    overlay_->setCursor(board_->cpu().pc());   // курсор — куда реально дошли
    overlay_->followPc();
    renderScreen();
    overlay_->update();
}

void MainWindow::stepOver() {
    overlay_->snapshotRegs();   // so registers changed by this step are highlighted
    uint16_t pc = board_->cpu().pc();
    bk::DisasmLine d = bk::disasm(board_->memory(), pc);
    uint16_t ir = board_->memory().peekWord(pc);
    int idx = ir >> 6;
    bool isCall = (idx >= 040 && idx <= 047) ||           // JSR
                  (idx >= 01040 && idx <= 01047);         // EMT/TRAP
    if (isCall) {
        uint16_t ret = pc + d.words * 2;
        board_->runUntil(ret, 20'000'000);                // cap to avoid hangs
        board_->clearBreakHit();
    } else {
        board_->stepInstruction();
    }
    overlay_->setCursor(board_->cpu().pc());  // курсор — на исполняемую инструкцию
    overlay_->followPc();
    renderScreen();
    overlay_->update();
}

void MainWindow::resizeEvent(QResizeEvent* e) {
    QMainWindow::resizeEvent(e);
    if (overlay_) overlay_->setGeometry(screen_->rect());
}

// Разница между размером окна и его «начинкой» (меню, строка состояния) заранее
// неизвестна, поэтому окно подгоняем один раз уже после показа: только так экран
// получает ровно целый масштаб (1024×768 при пикселях 3:4).
void MainWindow::showEvent(QShowEvent* e) {
    QMainWindow::showEvent(e);
    if (sizeFitted_) return;
    sizeFitted_ = true;
    const QSize want = screen_->preferredSize();
    resize(size() + (want - screen_->size()));
}

// ---- Interactive-disassembler annotations (symbols + comments) --------------
static QString octAddr(uint16_t a) { return QString("%1").arg(a, 6, 8, QChar('0')); }

void MainWindow::nameCursorSymbol() {
    if (!paused_) return;
    uint16_t a = overlay_->cursorAddr();
    const std::string* cur = board_->symbols().nameAt(a);
    bool ok = false;
    QString name = QInputDialog::getText(this, "Символ",
        QString("Имя для адреса %1 (пусто — удалить):").arg(octAddr(a)),
        QLineEdit::Normal, cur ? QString::fromStdString(*cur) : QString(), &ok);
    if (!ok) return;
    name = name.trimmed();
    if (name.isEmpty()) board_->symbols().remove(a);
    else board_->symbols().set(a, name.toStdString());
    overlay_->update();
}

void MainWindow::commentCursor() {
    if (!paused_) return;
    uint16_t a = overlay_->cursorAddr();
    const std::string* cur = board_->comment(a);
    bool ok = false;
    QString text = QInputDialog::getText(this, "Комментарий",
        QString("Комментарий к адресу %1 (пусто — удалить):").arg(octAddr(a)),
        QLineEdit::Normal, cur ? QString::fromStdString(*cur) : QString(), &ok);
    if (!ok) return;
    board_->setComment(a, text.trimmed().toStdString());   // empty clears
    overlay_->update();
}

void MainWindow::markData(bk::DataType t) {
    if (!paused_) return;
    uint16_t a = overlay_->cursorAddr();
    uint16_t len = (t == bk::DataType::Word || t == bk::DataType::Ptr) ? 2 : 1;
    if (t == bk::DataType::String) {
        // Auto-length: printable run up to (and including) a terminating NUL, capped.
        len = 0;
        for (int i = 0; i < 80; ++i) {
            uint8_t c = board_->memory().peekByte(a + i);
            if (c == 0) { ++len; break; }
            if (c < 32 || c >= 127) break;
            ++len;
        }
        if (len == 0) len = 1;
    }
    board_->setData(a, t, len);
    overlay_->update();
}

void MainWindow::gotoDialog() {
    if (!paused_) return;
    // Editable picker: the sorted symbol names act as a function list; the field
    // also accepts a typed address (octal by default, or 0x hex).
    QStringList items;
    for (const auto& kv : board_->symbols().all()) items << QString::fromStdString(kv.second);
    items.sort();
    bool ok = false;
    QString sel = QInputDialog::getItem(this, "Перейти",
        "Символ или адрес (восьмер., или 0x…):", items, 0, /*editable*/ true, &ok);
    if (!ok) return;
    sel = sel.trimmed();
    uint16_t addr;
    if (board_->symbols().addrOf(sel.toStdString(), addr)) { overlay_->navigateTo(addr); return; }
    bool num = false; uint parsed = sel.startsWith("0x", Qt::CaseInsensitive)
        ? sel.mid(2).toUInt(&num, 16) : sel.toUInt(&num, 8);
    if (num) overlay_->navigateTo(static_cast<uint16_t>(parsed));
    else status_->setText("Не найдено: " + sel);
}

void MainWindow::xrefsDialog() {
    if (!paused_) return;
    uint16_t t = overlay_->cursorAddr();
    std::vector<uint16_t> refs = overlay_->xrefsTo(t);
    QString what = QString("%1").arg(t, 6, 8, QChar('0'));
    if (const std::string* nm = board_->symbols().nameAt(t)) what += " (" + QString::fromStdString(*nm) + ")";
    if (refs.empty()) { status_->setText("Нет ссылок на " + what); return; }
    QStringList items;
    for (uint16_t a : refs)
        items << QString("%1  %2").arg(a, 6, 8, QChar('0'))
                     .arg(QString::fromStdString(bk::disasm(board_->memory(), a, &board_->symbols()).text));
    bool ok = false;
    QString sel = QInputDialog::getItem(this, "Ссылки на " + what,
        QString("Кто ссылается (%1):").arg(refs.size()), items, 0, /*editable*/ false, &ok);
    if (!ok) return;
    bool num = false; uint a = sel.section(' ', 0, 0).toUInt(&num, 8);
    if (num) overlay_->navigateTo(static_cast<uint16_t>(a));
}

QString MainWindow::annotationsPath() const {
    return lastBin_.isEmpty() ? QString() : lastBin_ + ".bkdb";
}

void MainWindow::saveAnnotations(const QString& path) {
    if (path.isEmpty()) return;
    QJsonArray syms, coms;
    for (const auto& kv : board_->symbols().all()) {
        QJsonObject o; o["a"] = int(kv.first); o["n"] = QString::fromStdString(kv.second);
        syms.append(o);
    }
    for (const auto& kv : board_->comments()) {
        QJsonObject o; o["a"] = int(kv.first); o["c"] = QString::fromStdString(kv.second);
        coms.append(o);
    }
    QJsonArray dat;
    for (const auto& kv : board_->dataItems()) {
        QJsonObject o; o["a"] = int(kv.first); o["t"] = int(kv.second.type); o["l"] = int(kv.second.len);
        dat.append(o);
    }
    if (syms.isEmpty() && coms.isEmpty() && dat.isEmpty()) return;   // nothing worth writing
    QJsonObject root; root["symbols"] = syms; root["comments"] = coms; root["data"] = dat;
    QFile f(path);
    if (f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

void MainWindow::loadAnnotations(const QString& path) {
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    for (const QJsonValue& v : root["symbols"].toArray()) {
        QJsonObject o = v.toObject();
        board_->symbols().set(uint16_t(o["a"].toInt()), o["n"].toString().toStdString());
    }
    for (const QJsonValue& v : root["comments"].toArray()) {
        QJsonObject o = v.toObject();
        board_->setComment(uint16_t(o["a"].toInt()), o["c"].toString().toStdString());
    }
    for (const QJsonValue& v : root["data"].toArray()) {
        QJsonObject o = v.toObject();
        board_->setData(uint16_t(o["a"].toInt()), bk::DataType(o["t"].toInt()), uint16_t(o["l"].toInt()));
    }
    if (overlay_) overlay_->update();
}

void MainWindow::closeEvent(QCloseEvent* e) {
    stopVideo();                          // дописать индекс AVI
    saveAnnotations(annotationsPath());   // persist symbols/comments next to the .bin
    saveDisks();
    QMainWindow::closeEvent(e);
}

void MainWindow::openBin() {
    QString path = QFileDialog::getOpenFileName(this, "Загрузить .BIN",
        lastBin_.isEmpty() ? QString() : lastBin_, "BK images (*.bin *.BIN);;Все файлы (*)");
    if (!path.isEmpty()) loadBinFromPath(path);
}

bool MainWindow::loadBinFromPath(const QString& path) {
    uint16_t addr = 0, len = 0;
    // Машину выбирает программа (Board::binTargetsBk11): палитра 0177662 или вызовы БОС —
    // БК-0011М, иначе БК-0010-01. Сохранённый выбор машины это не меняет.
    const bool want11 = bk::Board::binTargetsBk11(path.toStdString());
    if (want11 != board_->bk11()) setMachine(want11 ? 1 : 0, false);
    // The monitor ROM must have initialised before a game is started; when a bin
    // is passed on the command line this happens before the 50 Hz timer has run
    // a single frame, so boot it here first. (На БК-0011М монитор БОС поднимает сам
    // loadBin.)
    board_->ensureMonitorBooted();
    if (!board_->loadBin(path.toStdString(), true, &addr, &len)) {
        QMessageBox::warning(this, "БК", QString("Не удалось загрузить %1").arg(path));
        return false;
    }
    lastBin_ = path;
    loadAnnotations(annotationsPath());   // pick up "<bin>.bkdb" symbols/comments if present
    status_->setText(QString("Загружено %1: адрес %2, длина %3 (восьм.)")
        .arg(QFileInfo(path).fileName())
        .arg(QString::number(addr, 8))
        .arg(QString::number(len, 8)));
    updateTitle();
    return true;
}

void MainWindow::resetMachine() {
    board_->reset();
    keymap_.reset();
    keyFeed_.clear();
    heldKeys_.clear();
    board_->setKeyHeld(false);
    if (!lastBin_.isEmpty()) loadBinFromPath(lastBin_);
    status_->setText("Сброс");
}

// Подключение и снятие платы — операция «при выключенном питании»: меняется вся
// карта верхней половины адресов, поэтому машину перезапускаем. Настройка
// сохраняется между запусками; ключ --smk её не перезаписывает.
void MainWindow::setSmk512(bool on) {
    board_->setSmk512(on);
    QSettings().setValue(smkKey(), on);
    resetMachine();
    status_->setText(on ? "СМК-512 подключён, машина перезапущена"
                        : "СМК-512 снят, машина перезапущена");
}

void MainWindow::applyScanline() {
    board_->setScanlineRender(scanlineOverride_ >= 0 ? scanlineOverride_ > 0 : board_->bk11());
}

void MainWindow::syncMachineMenu() {
    for (QAction* a : {machine10Action_, machine11Action_, ayAction_, ay2Action_, covoxAction_, smkAction_})
        if (!a) return;
    QSignalBlocker b1(machine10Action_), b2(machine11Action_), b3(ayAction_), b4(smkAction_),
                   b5(ay2Action_), b6(covoxAction_);
    machine10Action_->setChecked(!board_->bk11());
    machine11Action_->setChecked(board_->bk11());
    ayAction_->setChecked(board_->ay());
    ay2Action_->setChecked(board_->ay2());
    covoxAction_->setChecked(board_->covox());
    smkAction_->setChecked(board_->smk512());
}

void MainWindow::setMachine(int m, bool persist) {
    const bool want11 = m == 1;
    if (persist) QSettings().setValue("machine", want11 ? 1 : 0);
    if (want11 == board_->bk11()) { syncMachineMenu(); return; }
    saveDisks();
    board_->setMachine(want11 ? bk::Machine::Bk0011m : bk::Machine::Bk0010);
    applySoundSettings();
    // Плата — своя настройка у каждой машины (на БК-0011М её ставят реже).
    board_->setSmk512(QSettings().value(smkKey(), true).toBool());
    applyScanline();
    lastBin_.clear();                  // .BIN другой машины перезапускать нельзя
    syncMachineMenu();
    resetMachine();
    updateTitle();
    status_->setText(QString("Машина: %1 — перезапущена").arg(QString::fromUtf8(bk::Board::machineName(board_->machine()))));
}

// Звук — по настройкам этой машины (по умолчанию на БК-0011М всё включено: AY, второй
// AY и Covox, на БК-0010-01 — ничего); ключи командной строки перекрывают их на запуск.
void MainWindow::applySoundSettings(int ayOverride, int ay2Override, int covoxOverride) {
    const bool b11 = board_->bk11();
    auto pick = [&](int ovr, const char* base) {
        return ovr >= 0 ? ovr > 0 : QSettings().value(soundKey(base), b11).toBool();
    };
    board_->setAy(pick(ayOverride, "ay"));
    board_->setAy2(pick(ay2Override, "ay2_"));
    board_->setCovox(pick(covoxOverride, "covox"));
}

void MainWindow::setAy(bool on) {
    board_->setAy(on);
    QSettings().setValue(soundKey("ay"), on);
    status_->setText(on ? "Звуковой сопроцессор AY-3-8910 подключён" : "Звуковой сопроцессор AY-3-8910 снят");
}

void MainWindow::setAy2(bool on) {
    board_->setAy2(on);
    QSettings().setValue(soundKey("ay2_"), on);
    status_->setText(on ? "Второй AY подключён (плата «2AY»: TurboSound и «Грифон»)" : "Второй AY снят");
}

void MainWindow::setCovox(bool on) {
    board_->setCovox(on);
    QSettings().setValue(soundKey("covox"), on);
    status_->setText(on ? "Covox подключён к порту 0177714" : "Covox снят");
}

void MainWindow::setPixelAspect34(bool on) {
    screen_->setPixelAspect34(on);
    QSettings().setValue("aspect34", on);
    if (aspectAction_ && aspectAction_->isChecked() != on) {
        QSignalBlocker b(aspectAction_);
        aspectAction_->setChecked(on);
    }
    status_->setText(on ? "Пиксели: 3:4, блок 4×3 точки (кадр 1024×768)"
                        : "Пиксели: квадратные (кадр 512×256)");
    // Кадру 3:4 нужно больше высоты, чем квадратному: если окно мельче целого кадра,
    // масштаб перестал бы быть кратным — подрастим окно (сужать при обратном
    // переключении не будем, место уже отдано пользователю).
    const QSize want = screen_->preferredSize(), have = screen_->size();
    if (want.width() > have.width() || want.height() > have.height())
        resize(size() + QSize(qMax(0, want.width() - have.width()),
                              qMax(0, want.height() - have.height())));
}

void MainWindow::toggleColorMode() {
    colorMode_ = !colorMode_;
    status_->setText(colorMode_ ? "Режим: 256×256 цвет" : "Режим: 512×256 ч/б");
}

// Alt+клавиша при работающей БК — это АР2, а не мнемоника меню («&Файл»):
// приняв ShortcutOverride, отдаём нажатие в keyPressEvent.
bool MainWindow::event(QEvent* e) {
    if (e->type() == QEvent::ShortcutOverride && !paused_ && !suspended_) {
        auto* k = static_cast<QKeyEvent*>(e);
        if (k->modifiers() & Qt::AltModifier) { k->accept(); return true; }
    }
    return QMainWindow::event(e);
}

void MainWindow::keyPressEvent(QKeyEvent* e) {
    // F12 toggles the debugger regardless of state.
    if (e->key() == Qt::Key_F12) { setPaused(!paused_); e->accept(); return; }
    // Pause key toggles the simple emulation freeze.
    if (e->key() == Qt::Key_Pause) { setSuspended(!suspended_); e->accept(); return; }

    // While suspended, swallow all other keys so nothing reaches the game or the
    // key buffer (they'd otherwise be replayed on resume).
    if (suspended_ && !paused_) { e->accept(); return; }

    // Track the physical key-down state (ignoring auto-repeat) so 0177716 bit 6
    // stays low while a key is held — games poll it to detect input.
    // АР2 (Alt) вне матрицы, сама по себе нажатия не даёт.
    if (!paused_ && !e->isAutoRepeat() && e->key() != Qt::Key_Alt) {
        heldKeys_.insert(e->key());
        board_->setKeyHeld(true);
    }

    if (paused_) {
        // Идёт правка значения на месте — все клавиши уходят в поле ввода,
        // иначе цифры и Enter разошлись бы по командам отладчика.
        if (overlay_->handleEditKey(e)) { e->accept(); return; }
        // --- Debugger controls (SoftICE overlay is visible) ---
        switch (e->key()) {
        case Qt::Key_F4:       runToCursor(); return;   // дойти до инструкции под курсором
        case Qt::Key_F7:       stepInto(); return;
        case Qt::Key_F8:       stepOver(); return;
        case Qt::Key_F9:       board_->toggleBreakpoint(board_->cpu().pc()); overlay_->update(); return;
        case Qt::Key_Escape:   setPaused(false); return;
        case Qt::Key_PageUp:   overlay_->scrollDisasm(-8); return;
        case Qt::Key_PageDown: overlay_->scrollDisasm(8); return;
        case Qt::Key_Up:       overlay_->moveCursor(-1); return;
        case Qt::Key_Down:     overlay_->moveCursor(1); return;
        case Qt::Key_N:        nameCursorSymbol(); return;   // name/rename the selected line
        case Qt::Key_Semicolon: commentCursor(); return;     // comment the selected line
        case Qt::Key_B:        markData(bk::DataType::Byte);   return;  // mark data types
        case Qt::Key_W:        markData(bk::DataType::Word);   return;
        case Qt::Key_S:        markData(bk::DataType::String); return;
        case Qt::Key_P:        markData(bk::DataType::Ptr);    return;
        case Qt::Key_U:        board_->clearData(overlay_->cursorAddr()); overlay_->update(); return; // back to code
        case Qt::Key_G:        gotoDialog(); return;                       // goto symbol/address
        case Qt::Key_X:        xrefsDialog(); return;                      // who references the cursor
        case Qt::Key_Return:
        case Qt::Key_Enter:    overlay_->followTarget(); return;           // follow branch/call target
        case Qt::Key_Left:     overlay_->navBack(); return;                // history back / forward
        case Qt::Key_Right:    overlay_->navForward(); return;
        case Qt::Key_BracketLeft:  overlay_->scrollMem(-8); return;
        case Qt::Key_BracketRight: overlay_->scrollMem(8); return;
        default: e->accept(); return; // swallow other keys while debugging
        }
    }

    // Esc при выключенном отладчике — клавиша «СТОП» БК. Она вынесена из матрицы
    // и кода не даёт, поэтому идёт мимо BkKeymap: внеприоритетное прерывание по
    // вектору 4. (Внутри отладчика Esc по-прежнему «продолжить»/«отменить ввод».)
    if (e->key() == Qt::Key_Escape) {
        board_->pressStop();
        status_->setText(QString::fromUtf8("Клавиша СТОП (прерывание по вектору 4)"));
        e->accept();
        return;
    }

    // --- SoftICE off: feed the BK-0010 keyboard ---
    // Автоповтор Qt отбрасываем целиком: у реальной БК типematic-повтора нет
    // (для повтора была клавиша ПОВТ). Иначе дубликаты удерживаемой клавиши
    // встают в очередь ВПЕРЕДИ только что нажатой — и игра, читающая код по биту
    // «клавиша нажата» (PITON), срабатывает по коду предыдущей клавиши.
    if (e->isAutoRepeat()) { e->accept(); return; }
    std::vector<uint16_t> codes = keymap_.translate(e);
    if (!codes.empty()) {
        // Одиночный код при пустой очереди защёлкиваем НЕМЕДЛЕННО — атомарно с
        // битом «клавиша нажата» (setKeyHeld выше): эмуляция однопоточная, между
        // ними не исполняется ни одной инструкции БК. Если отложить код до подачи
        // из очереди в onTick, игра, читающая 0177662 по биту 0177716 (PITON),
        // целый кадр видела бы «клавиша нажата» с кодом ПРЕДЫДУЩЕЙ клавиши.
        // Многокодовые последовательности (РУС/ЛАТ + символ) и накопившийся хвост
        // по-прежнему идут через очередь — по одному коду на кадр, чтобы монитор
        // успевал прочитать каждый.
        if (keyFeed_.empty() && codes.size() == 1) {
            board_->pressKey(codes[0]);
        } else {
            for (uint16_t c : codes) {
                // Cap the buffer, mirroring the fact that the real controller
                // keeps only one pending code.
                if (keyFeed_.size() >= 16) break;
                keyFeed_.push_back(c);
            }
        }
        e->accept();
        return;
    }
    QMainWindow::keyPressEvent(e);
}

void MainWindow::keyReleaseEvent(QKeyEvent* e) {
    // Release the physical key-held state (0177716 bit 6) when the last game key
    // is lifted. Auto-repeat generates spurious release events — ignore those.
    if (!e->isAutoRepeat()) {
        heldKeys_.erase(e->key());
        if (heldKeys_.empty()) board_->setKeyHeld(false);
    }
    QMainWindow::keyReleaseEvent(e);
}

// Вставить дискету и загрузиться с неё. Контроллер появляется на шине вместе с
// образом, поэтому машину перезапускаем: на живой БК плату в разъём МПИ ставят
// при выключенном питании. Дальше поднимаем монитор и отдаём управление
// автозагрузчику ПЗУ контроллера — то же, что набрать в мониторе «160000G».
void MainWindow::openDisk(int drive) {
    const QString path = QFileDialog::getOpenFileName(
        this, drive ? "Вставить образ диска в привод B" : "Вставить образ диска",
        lastDisk_.isEmpty() ? lastBin_ : lastDisk_,
        "Образы дисков БК (*.bkd *.BKD *.bkD *.img *.IMG *.Img);;Все файлы (*)");
    if (path.isEmpty()) return;
    insertDiskAndBoot(path, drive);
}

void MainWindow::saveDisks() {
    bk::Fdd& f = board_->kngmd().fdd();
    for (int d = 0; d < bk::Fdd::DRIVES; ++d)
        if (f.attached(d) && !f.readOnly(d)) f.save(d);   // save() дописывает и текущую дорожку
}

bool MainWindow::insertDiskAndBoot(const QString& path, int drive) {
    saveDisks();
    // Второй привод не перезапускает машину: дискету в него меняют на ходу.
    if (drive == 0) { resetMachine(); board_->ensureMonitorBooted(); }
    if (!board_->attachDisk(drive, path.toStdString(), !diskRw_)) {
        status_->setText("Не удалось вставить образ — нет roms/disk326.rom или файл не читается");
        return false;
    }
    if (drive == 0) board_->bootFromDisk();
    lastDisk_ = path;
    // СМК и дисковод уживаются на шине вместе (это и есть историческая машина с
    // диском), поэтому плату не снимаем. Наоборот, без неё большинству систем
    // некуда грузиться: у голой БК-0010-01 свободно около 16 Кбайт ОЗУ.
    status_->setText(drive
        ? QString("Дискета в приводе B: %1").arg(QFileInfo(path).fileName())
        : QString("Загрузка с диска: %1%2").arg(QFileInfo(path).fileName())
              .arg(board_->smk512() ? ", ОЗУ платы СМК-512 подключено" : board_->bk11() ? ""
                                    : " — ВНИМАНИЕ: плата СМК-512 снята, памяти может не хватить"));
    updateTitle();
    return true;
}

// Винчестер — на плате СМК-512 (регистры 0177740–0177757, грузит прошивка платы,
// перехватывающая пуск) или на «Самаре» (0177620–0177656, грузит автозагрузчик КНГМД).
// Master подключается с включением питания, slave — на ходу, как вторая дискета.
void MainWindow::openHdd(int unit) {
    const QString path = QFileDialog::getOpenFileName(
        this, unit ? "Подключить второй винчестер (slave)" : "Подключить винчестер",
        lastHdd_.isEmpty() ? lastDisk_ : lastHdd_,
        "Образы винчестеров HDI (*.hdi *.HDI *.Hdi);;Все файлы (*)");
    if (path.isEmpty()) return;
    attachHdd(path, unit, unit == 0);
}

void MainWindow::setHddController(bk::Board::HddController c) {
    board_->setHddController(c);
    for (const auto& a : hddCtlActions_) a.first->setChecked(a.second == c);
}

bool MainWindow::attachHdd(const QString& path, int unit, bool boot) {
    // Винчестер СМК — узел платы СМК-512; «Самаре» плата не нужна.
    if (board_->resolveHddController(path.toStdString()) == bk::Board::HddController::Smk
        && !board_->smk512()) {
        board_->setSmk512(true);
        syncMachineMenu();
    }
    std::string err;
    if (!board_->attachHdd(unit, path.toStdString(), !hddRw_, &err)) {
        status_->setText(QString("Винчестер не подключён: %1").arg(QString::fromStdString(err)));
        return false;
    }
    lastHdd_ = path;
    if (boot) { lastBin_.clear(); resetMachine(); }
    const bk::HddDrive& d = board_->ide().drive(unit);
    status_->setText(QString("Винчестер %1 (%9): %2 — %3/%4/%5, %6 Мбайт%7%8")
        .arg(unit ? "slave" : "master").arg(QFileInfo(path).fileName())
        .arg(d.cylinders()).arg(d.heads()).arg(d.sectors())
        .arg(d.totalSectors() / 2048)
        .arg(hddRw_ ? "" : ", только чтение (запись — в памяти)")
        .arg(boot ? ", загрузка" : "")
        .arg(board_->hddSamara() ? "«Самара»" : "СМК-512")
        + (board_->hddSamara() && !board_->bk11() && !board_->smk512()
               ? " — ВНИМАНИЕ: плата СМК-512 снята, загрузчику может не хватить памяти" : ""));
    updateTitle();
    return true;
}

void MainWindow::detachHdds() {
    if (!board_->hddAttached(0) && !board_->hddAttached(1)) return;
    board_->detachHdd(0);
    board_->detachHdd(1);
    resetMachine();
    status_->setText("Винчестеры отключены, машина перезапущена");
}

void MainWindow::openMemVis() {
    if (!memvis_) memvis_ = new MemVisWidget(board_.get());
    board_->trace().setEnabled(true);
    memvis_->show();
    memvis_->raise();
}

// Отдельное окно на ДОЗУ нужно потому, что обычный дамп ходит по адресам БК и
// показывает только подключённую страницу, да и то не целиком: верхние 512 байт
// сегмента не читаются ни в одном режиме (docs/smk512.md).
void MainWindow::openBkRam() {
    if (!bkram_) bkram_ = new BkRamWidget(board_.get());
    bkram_->show();
    bkram_->raise();
}

void MainWindow::openSmkRam() {
    if (!smkram_) smkram_ = new SmkRamWidget(board_.get());
    smkram_->show();
    smkram_->raise();
    if (!board_->smk512())
        status_->setText("СМК-512 не установлен: окно ДОЗУ пустое");
}

void MainWindow::broadcastHighlight(int addr, QWidget* src) {
    if (hotpath_    && hotpath_    != src) hotpath_->setHighlight(addr);
    if (callgraph_  && callgraph_  != src) callgraph_->setHighlight(addr);
    if (flame_      && flame_      != src) flame_->setHighlight(addr);
    if (flamechart_ && flamechart_ != src) flamechart_->setHighlight(addr);
    if (overlay_) overlay_->setHighlight(addr);   // debugger disasm is always a receiver
}

void MainWindow::openHotPath() {
    if (!hotpath_) {
        hotpath_ = new HotPathWidget(board_.get());
        hotpath_->resize(720, 620);
        // Clicking a row jumps the debugger's disassembler to that address,
        // opening the debugger overlay if it isn't already showing.
        connect(hotpath_, &HotPathWidget::addressPicked, this, [this](uint16_t addr) {
            if (!paused_) setPaused(true);   // setPaused re-anchors disasm to PC…
            overlay_->setDisasmAddr(addr);   // …so set the picked address after it
            overlay_->update();
        });
        connect(hotpath_, &HotPathWidget::hoverAddress, this,
                [this](int a) { broadcastHighlight(a, hotpath_); });
    }
    board_->trace().setEnabled(true);
    hotpath_->show();
    hotpath_->raise();
}

void MainWindow::openCallGraph() {
    if (!callgraph_) {
        callgraph_ = new CallGraphWidget(board_.get());
        callgraph_->resize(900, 680);
        // Clicking a function box jumps the debugger's disassembler to its entry.
        connect(callgraph_, &CallGraphWidget::addressPicked, this, [this](uint16_t addr) {
            if (!paused_) setPaused(true);
            overlay_->setDisasmAddr(addr);
            overlay_->update();
        });
        connect(callgraph_, &CallGraphWidget::hoverAddress, this,
                [this](int a) { broadcastHighlight(a, callgraph_); });
    }
    board_->trace().setEnabled(true);
    callgraph_->show();
    callgraph_->raise();
}

void MainWindow::openFlame() {
    if (!flame_) {
        flame_ = new FlameWidget(board_.get());
        flame_->resize(820, 560);
        connect(flame_, &FlameWidget::addressPicked, this, [this](uint16_t addr) {
            if (!paused_) setPaused(true);
            overlay_->setDisasmAddr(addr);
            overlay_->update();
        });
        connect(flame_, &FlameWidget::hoverAddress, this,
                [this](int a) { broadcastHighlight(a, flame_); });
    }
    board_->trace().setEnabled(true);
    board_->trace().setFlameEnabled(true);
    flame_->show();
    flame_->raise();
}

void MainWindow::openFlameChart() {
    if (!flamechart_) {
        flamechart_ = new FlameChartWidget(board_.get());
        flamechart_->resize(900, 480);
        connect(flamechart_, &FlameChartWidget::addressPicked, this, [this](uint16_t addr) {
            if (!paused_) setPaused(true);
            overlay_->setDisasmAddr(addr);
            overlay_->update();
        });
        connect(flamechart_, &FlameChartWidget::hoverAddress, this,
                [this](int a) { broadcastHighlight(a, flamechart_); });
    }
    board_->trace().setEnabled(true);
    board_->trace().setFlameEnabled(true);
    board_->trace().setSpansEnabled(true);
    flamechart_->show();
    flamechart_->raise();
}

void MainWindow::openHotChart() {
    if (!hotchart_) {
        hotchart_ = new HotChartWidget(board_.get());
        // Clicking a legend row jumps the debugger's disassembler to that address.
        connect(hotchart_, &HotChartWidget::addressPicked, this, [this](uint16_t addr) {
            if (!paused_) setPaused(true);
            overlay_->setDisasmAddr(addr);
            overlay_->update();
        });
    }
    board_->trace().setEnabled(true);
    hotchart_->show();
    hotchart_->raise();
}

void MainWindow::saveState() {
    QString path = QFileDialog::getSaveFileName(this, "Сохранить состояние", QString(),
        "BK state (*.bkst);;Все файлы (*)");
    if (path.isEmpty()) return;
    if (!path.endsWith(".bkst")) path += ".bkst";
    if (board_->saveState(path.toStdString()))
        status_->setText("Состояние сохранено: " + QFileInfo(path).fileName());
    else
        QMessageBox::warning(this, "БК", "Не удалось сохранить состояние");
}

void MainWindow::loadState() {
    QString path = QFileDialog::getOpenFileName(this, "Восстановить состояние", QString(),
        "BK state (*.bkst);;Все файлы (*)");
    if (path.isEmpty()) return;
    if (board_->loadState(path.toStdString())) {
        status_->setText("Состояние восстановлено: " + QFileInfo(path).fileName());
        // В снимке лежит и конфигурация машины, поэтому плата СМК могла появиться
        // или исчезнуть. Галку в меню синхронизируем без сигнала: иначе она тут же
        // перезапустила бы машину и стёрла только что восстановленное состояние.
        syncMachineMenu();
        applyScanline();
        updateTitle();
        renderScreen();
        if (paused_) overlay_->followPc();
    } else {
        QMessageBox::warning(this, "БК", "Не удалось восстановить состояние");
    }
}

void MainWindow::updateTitle() {
    QString t = QString("%1 — эмулятор-отладчик").arg(QString::fromUtf8(bk::Board::machineName(board_->machine())));
    if (!lastBin_.isEmpty()) t += " — " + QFileInfo(lastBin_).fileName();
    setWindowTitle(t);
}
