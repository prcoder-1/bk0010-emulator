#include <QApplication>
#include <QSurfaceFormat>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <cstdio>
#include <vector>
#include <cstdint>
#include "ui/MainWindow.h"
#include "ui/DebuggerOverlay.h"
#include "ui/MemVisWidget.h"
#include "ui/CodeGraphWidget.h"
#include "ui/BkKeymap.h"
#include "mcp/McpServer.h"
#include "Board.h"
#include "Disasm.h"
#include <QKeyEvent>
#include <QGuiApplication>

// Self-test of the Qt-key -> BK-code translation (Cyrillic, РУС/ЛАТ, Ctrl, specials).
static int runKeyTest() {
    BkKeymap km;
    struct Case { int key; Qt::KeyboardModifiers mods; const char* text; const char* label; };
    const Case cases[] = {
        {Qt::Key_A, Qt::NoModifier, "A", "Latin A"},
        {Qt::Key_1, Qt::NoModifier, "1", "digit 1"},
        {0,         Qt::NoModifier, "б", "Cyrillic б (switch to РУС)"},
        {0,         Qt::NoModifier, "а", "Cyrillic а (stay РУС)"},
        {Qt::Key_A, Qt::NoModifier, "A", "Latin A (switch to ЛАТ)"},
        {Qt::Key_Left,  Qt::NoModifier, "",  "Left arrow"},
        {Qt::Key_Return,Qt::NoModifier, "\r","Enter"},
        {Qt::Key_C, Qt::ControlModifier, "\x03", "Ctrl+C"},
    };
    for (const auto& c : cases) {
        QKeyEvent e(QEvent::KeyPress, c.key, c.mods, QString::fromUtf8(c.text));
        auto codes = km.translate(&e);
        std::printf("%-32s ->", c.label);
        for (uint16_t x : codes) std::printf(" %04o", x);
        std::printf("\n");
    }
    // РУС / ЛАТ mapped to the two Shift keys (told apart by the X keysym).
    struct { quint32 vk; const char* label; } shifts[] = {
        {0xffe1, "Left Shift  -> РУС (016)"},
        {0xffe2, "Right Shift -> ЛАТ (017)"},
    };
    for (const auto& c : shifts) {
        QKeyEvent e(QEvent::KeyPress, Qt::Key_Shift, Qt::ShiftModifier, 0, c.vk, 0);
        auto codes = km.translate(&e);
        std::printf("%-32s ->", c.label);
        for (uint16_t x : codes) std::printf(" %04o", x);
        std::printf("\n");
    }
    return 0;
}

// Headless verification: boot the monitor, optionally load a .BIN, run N frames
// and save the screen (rendered from the CPU-side pixel buffer, no GL needed).
static int runHeadless(const QString& romDir, const QString& bin,
                       int frames, bool color, const QString& shot,
                       int keyCode, int keyFrame, const QString& dbgShot,
                       const QString& memvisShot, const QString& cgShot,
                       const QString& typeStr, const QString& keysList) {
    // Parse "frame:code,frame:code,..." into precisely-timed key injections.
    std::vector<std::pair<int,int>> keys;
    for (const QString& part : keysList.split(',', Qt::SkipEmptyParts)) {
        const auto fc = part.split(':');
        if (fc.size() == 2) keys.push_back({fc[0].toInt(), fc[1].toInt(nullptr, 0)});
    }
    bk::Board board;
    if (!board.loadRoms(romDir.toStdString())) {
        std::fprintf(stderr, "headless: failed to load ROMs from %s\n", qPrintable(romDir));
        return 2;
    }
    board.reset();
    if (!memvisShot.isEmpty() || !cgShot.isEmpty()) board.trace().setEnabled(true);
    // Let the monitor ROM initialise (vectors, stack, display driver) before
    // jumping into a game.
    for (int i = 0; i < 25; ++i) board.runFrame();
    if (!bin.isEmpty() && !board.loadBin(bin.toStdString(), true)) {
        std::fprintf(stderr, "headless: failed to load bin %s\n", qPrintable(bin));
        return 3;
    }
    board.screen().setColorMode(color);
    // Optional: type a string, one character every few frames, through the same
    // keyboard queue the GUI uses (verifies the key -> register -> ISR -> echo path).
    int typeIdx = 0;
    for (int i = 0; i < frames; ++i) {
        if (keyCode >= 0 && i == keyFrame) board.pressKey(static_cast<uint16_t>(keyCode));
        for (const auto& kv : keys) if (kv.first == i) board.pressKey(static_cast<uint16_t>(kv.second));
        if (!typeStr.isEmpty() && i >= 60 && typeIdx < typeStr.size() && !board.keyReady()) {
            QChar c = typeStr[typeIdx++];
            uint16_t code = (c == '\n') ? 012 : static_cast<uint16_t>(c.unicode() & 0x7f);
            board.pressKey(code);
        }
        board.runFrame();
    }
    board.screen().render(board.memory());

    QImage img(reinterpret_cast<const uchar*>(board.screen().pixels()),
               bk::Screen::TEX_W, bk::Screen::TEX_H, QImage::Format_ARGB32);
    QImage out = img.copy(); // detach from the board's buffer
    if (!shot.isEmpty()) {
        if (!out.save(shot)) { std::fprintf(stderr, "headless: cannot save %s\n", qPrintable(shot)); return 4; }
        std::printf("headless: ran %d frames, wrote %s\n", frames, qPrintable(shot));
    }
    if (!dbgShot.isEmpty()) {
        // Composite the Soft-ICE overlay over the (scaled) BK screen.
        QImage big = out.scaled(1024, 768).convertToFormat(QImage::Format_ARGB32);
        DebuggerOverlay ov(&board);
        ov.resize(1024, 768);
        ov.followPc();
        ov.render(&big, QPoint(), QRegion(), QWidget::DrawChildren);
        if (!big.save(dbgShot)) { std::fprintf(stderr, "headless: cannot save %s\n", qPrintable(dbgShot)); return 5; }
        std::printf("headless: wrote debugger overlay %s (PC=%06o)\n", qPrintable(dbgShot), board.cpu().pc());
    }
    if (!memvisShot.isEmpty()) {
        MemVisWidget w(&board);
        w.resize(560, 640);
        w.refresh();
        QPixmap pm = w.grab();
        pm.save(memvisShot); // default: ROM hidden, RAM fills the window
        std::printf("headless: wrote memvis (RAM only) %s\n", qPrintable(memvisShot));
        // Verification: the same view with ROM shown (full 64 KB, smaller scale).
        MemCanvas cv(&board);
        cv.resize(560, 560);
        cv.hideRom = false;
        QString withRomPath = memvisShot; withRomPath.replace(".png", "_withrom.png");
        cv.grab().save(withRomPath);
        std::printf("headless: wrote memvis (RAM+ROM) %s\n", qPrintable(withRomPath));
        // Verification: video RAM hidden and ROM shown — the ROM compacts up to
        // fill the space the screen would occupy, and the image fills the window
        // (no blank gap for the hidden region).
        MemCanvas cs(&board);
        cs.resize(560, 640);
        cs.hideScreen = true;
        cs.hideRom = false;
        QString noScreenPath = memvisShot; noScreenPath.replace(".png", "_noscreen.png");
        cs.grab().save(noScreenPath);
        std::printf("headless: wrote memvis (screen hidden, ROM shown) %s\n", qPrintable(noScreenPath));
    }
    if (!cgShot.isEmpty()) {
        CodeGraphWidget w(&board);
        w.resize(760, 560);
        // Let the auto-follow glide converge onto the hottest instructions.
        for (int k = 0; k < 120; ++k) w.grab();
        w.grab().save(cgShot);
        std::printf("headless: wrote codegraph (auto-follow) %s\n", qPrintable(cgShot));
        // Exercise manual zoom + scroll and capture a second image.
        for (int k = 0; k < 5; ++k) {
            QKeyEvent ke(QEvent::KeyPress, Qt::Key_Plus, Qt::NoModifier);
            QApplication::sendEvent(&w, &ke);
        }
        QKeyEvent pd(QEvent::KeyPress, Qt::Key_PageDown, Qt::NoModifier);
        QApplication::sendEvent(&w, &pd);
        QString zoomPath = cgShot; zoomPath.replace(".png", "_zoom.png");
        w.grab().save(zoomPath);
        std::printf("headless: wrote manual codegraph %s\n", qPrintable(zoomPath));
    }

    // Report speaker sample activity (verifies the audio sample path).
    {
        std::vector<int16_t> s(board.sound().available());
        size_t n = s.empty() ? 0 : board.sound().read(s.data(), s.size());
        int16_t lo = 0, hi = 0;
        for (size_t i = 0; i < n; ++i) { if (s[i] < lo) lo = s[i]; if (s[i] > hi) hi = s[i]; }
        std::printf("headless: audio samples=%zu range=[%d..%d]%s\n", n, lo, hi,
                    (hi - lo > 1000) ? " (speaker active)" : " (silent)");
    }

    // Report whether the screen has any non-black pixels (sanity signal).
    int nonblack = 0;
    const uint32_t* px = board.screen().pixels();
    for (int i = 0; i < bk::Screen::TEX_W * bk::Screen::TEX_H; ++i)
        if ((px[i] & 0x00FFFFFF) != 0) ++nonblack;
    std::printf("headless: PC=%06o non-black pixels=%d\n", board.cpu().pc(), nonblack);
    return 0;
}

int main(int argc, char** argv) {
    // MCP server mode: a headless JSON-RPC-over-stdio server for debugging BK
    // programs from an MCP client (e.g. Claude Code). Handled before the GUI so
    // no display/OpenGL is needed; offscreen QGuiApplication enables PNG output.
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--server") {
            qputenv("QT_QPA_PLATFORM", "offscreen");
            QGuiApplication app(argc, argv);
            QString romDir;
#ifdef BK_DEFAULT_ROM_DIR
            romDir = BK_DEFAULT_ROM_DIR;
#endif
            if (qEnvironmentVariableIsSet("BK_ROM_DIR")) romDir = qEnvironmentVariable("BK_ROM_DIR");
            for (int j = 1; j < argc; ++j)
                if (std::string(argv[j]) == "--roms" && j + 1 < argc) romDir = argv[j + 1];
            McpServer server(romDir.toStdString());
            return server.run();
        }
    }

    // NVIDIA under a Wayland session: Qt6's default EGL/GLES path fails to create
    // GL contexts (both the QRhi backing store and our QOpenGLWidget) with
    // EGL_BAD_MATCH (3009). Route through XWayland/GLX, which is reliable, whenever
    // an X display is available and the user hasn't chosen a platform explicitly.
    // Also prefer desktop GL over GLES. Both must be set before QApplication.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && !qEnvironmentVariableIsEmpty("DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "xcb");
    qputenv("QT_OPENGL", "desktop");
    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);

    QApplication app(argc, argv);

    // Request an OpenGL 3.3 core context for the screen widget.
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(0);
    QSurfaceFormat::setDefaultFormat(fmt);

    // ROM directory: --roms <dir>, else BK_ROM_DIR env, else compiled default.
    QString romDir;
#ifdef BK_DEFAULT_ROM_DIR
    romDir = BK_DEFAULT_ROM_DIR;
#endif
    if (qEnvironmentVariableIsSet("BK_ROM_DIR"))
        romDir = qEnvironmentVariable("BK_ROM_DIR");

    QString binToLoad, shot, dbgShot, memvisShot, cgShot, typeStr, keysList;
    int frames = 0, keyCode = -1, keyFrame = 0;
    bool color = true, headless = false;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == "--roms" && i + 1 < args.size()) romDir = args[++i];
        else if (args[i] == "--frames" && i + 1 < args.size()) { frames = args[++i].toInt(); headless = true; }
        else if (args[i] == "--shot" && i + 1 < args.size()) shot = args[++i];
        else if (args[i] == "--dbgshot" && i + 1 < args.size()) { dbgShot = args[++i]; headless = true; }
        else if (args[i] == "--memvis" && i + 1 < args.size()) { memvisShot = args[++i]; headless = true; }
        else if (args[i] == "--codegraph" && i + 1 < args.size()) { cgShot = args[++i]; headless = true; }
        else if (args[i] == "--mono") color = false;
        else if (args[i] == "--key" && i + 1 < args.size()) keyCode = args[++i].toInt(nullptr, 0);
        else if (args[i] == "--keyframe" && i + 1 < args.size()) keyFrame = args[++i].toInt();
        else if (args[i] == "--type" && i + 1 < args.size()) { typeStr = args[++i]; headless = true; }
        else if (args[i] == "--keys" && i + 1 < args.size()) { keysList = args[++i]; headless = true; }
        else if (!args[i].startsWith("--")) binToLoad = args[i];
    }

    if (args.contains("--keytest"))
        return runKeyTest();

    if (headless)
        return runHeadless(romDir, binToLoad, frames, color, shot, keyCode, keyFrame,
                           dbgShot, memvisShot, cgShot, typeStr, keysList);

    MainWindow w(romDir);
    w.show();
    if (!binToLoad.isEmpty()) w.loadBinFromPath(binToLoad);

    return app.exec();
}

