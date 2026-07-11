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
#include "Board.h"

// Headless verification: boot the monitor, optionally load a .BIN, run N frames
// and save the screen (rendered from the CPU-side pixel buffer, no GL needed).
static int runHeadless(const QString& romDir, const QString& bin,
                       int frames, bool color, const QString& shot,
                       int keyCode, int keyFrame, const QString& dbgShot,
                       const QString& memvisShot, const QString& cgShot) {
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
    for (int i = 0; i < frames; ++i) {
        if (keyCode >= 0 && i == keyFrame) board.pressKey(static_cast<uint16_t>(keyCode));
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
        pm.save(memvisShot);
        std::printf("headless: wrote memvis %s\n", qPrintable(memvisShot));
    }
    if (!cgShot.isEmpty()) {
        CodeGraphWidget w(&board);
        w.resize(760, 560);
        QPixmap pm = w.grab();
        pm.save(cgShot);
        std::printf("headless: wrote codegraph %s\n", qPrintable(cgShot));
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

    QString binToLoad, shot, dbgShot, memvisShot, cgShot;
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
        else if (!args[i].startsWith("--")) binToLoad = args[i];
    }

    if (headless)
        return runHeadless(romDir, binToLoad, frames, color, shot, keyCode, keyFrame,
                           dbgShot, memvisShot, cgShot);

    MainWindow w(romDir);
    w.show();
    if (!binToLoad.isEmpty()) w.loadBinFromPath(binToLoad);

    return app.exec();
}
