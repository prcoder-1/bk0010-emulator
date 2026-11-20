// Регрессия по 19 защищённым кассетам БК-0010 (страница Manwe «Кассеты с защитой», из
// kromych/BK-0011M): свои загрузчики, файлы поверх настроек ПЗУ, мотор, остановленный
// посреди загрузки, ключ расшифровки из таймера ВМ1 (игры BIL) — проверка таймингов и
// магнитофона, какой не дают чистые записи.
//
// Каждая кассета грузится через монитор, как у человека (MONIT, M, имя), лента доигрывает
// до конца («Воспроизведение» после остановки мотора), затем видео-ОЗУ сравнивается с
// эталоном tests/data/protected/<кассета>.vram.gz (допуск — на анимацию заставок).
//
//   tape_tests                 — прогон (кассеты: $BK_PROTECTED_TAPES или
//                                ~/emulators/БК-0010-01/tapes/protected; нет — пропуск)
//   tape_tests --update        — записать эталоны заново (после проверки снимков глазами)
//   tape_tests Planet Ferrari  — только эти кассеты
#include "Board.h"
#include "Typist.h"
#include <zlib.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

using namespace bk;

namespace {

struct Case {
    const char* file;     // запись без .wav
    const char* name;     // имя для монитора
    int extraFrames;      // после конца ленты
    uint16_t key;         // нажать после (0 — нет): игра ждёт клавишу
    int afterKey;
};

// Что видно в конце (как в kromych/BK-0011M, software/tapes/protected/README.md).
const Case kCases[] = {
    {"Bally",          "BALLY",          1500, 0, 0},     // картинка игры
    {"Destroyer",      "DESTROYER",      1500, 0, 0},     // заставка
    {"Dva-Negra",      "ДВА НЕГРА",      1500, 0, 0},     // заставка
    {"Exolon-1",       "EXOLON ONE",     1500, 0, 0},     // текст вступления
    {"Exolon-Run",     "EXOLON.RUN",     1500, 0, 0},     // информация
    {"Expert",         "EXPERT",         1500, 0, 0},     // заставка
    {"Ferrari",        "FERRARI",        1500, 0, 0},     // заставка
    {"Hobbit",         "HOBBIT",         1500, 0, 0},     // заставка
    {"Indiana-Jones",  "INDIANA JONES",  1500, 0, 0},     // заставка и история
    {"New-Revolution", "NEW REVOLUTION", 1500, 0, 0},     // заставка
    {"Pereval-1",      "ПЕРЕВАЛ",        1500, 0, 0},     // загрузчик ждёт следующую запись
    {"Planet",         "PLANET",         4000, 0, 0},     // меню (ключ BIL из таймера)
    {"Return-Jedi-1",  "RETURNJ",        1500, 0, 0},     // заставка
    {"Robocop",        "ROBOCOP",        1500, 0, 0},     // заставка
    {"Runing",         "RUNING",         4000, 0, 0},     // BIL
    {"Second",         "SECOND",         4000, 0, 0},     // BIL
    {"Soldier-1",      "SOLDIER",         300, 040, 1500}, // © ждёт клавишу, затем заставка
    {"SuperMan",       "SUPERMAN",       4000, 0, 0},     // BIL
    {"Terminator-1",   "TERMINATOR1",     300, 040, 500},  // выбор управления, клавиша — игра
};

constexpr size_t VRAM = 040000;
constexpr double kMaxDiff = 0.10;   // доля разных байтов видео-ОЗУ

struct Result { std::string msg; bool ok = false; bool skipped = false; };

bool readGz(const std::string& path, std::vector<uint8_t>& out) {
    gzFile f = gzopen(path.c_str(), "rb");
    if (!f) return false;
    out.assign(VRAM, 0);
    const int n = gzread(f, out.data(), static_cast<unsigned>(VRAM));
    gzclose(f);
    return n == static_cast<int>(VRAM);
}

bool writeGz(const std::string& path, const std::vector<uint8_t>& data) {
    gzFile f = gzopen(path.c_str(), "wb9");
    if (!f) return false;
    const int n = gzwrite(f, data.data(), static_cast<unsigned>(data.size()));
    return gzclose(f) == Z_OK && n == static_cast<int>(data.size());
}

Result runCase(const Case& c, const std::string& tapes, const std::string& golden, bool update) {
    Result r;
    Board b;
    b.setSmk512(false);
    if (!b.loadRoms(BK_DEFAULT_ROM_DIR)) { r.msg = "нет ПЗУ"; return r; }
    b.reset();
    for (int i = 0; i < 100; ++i) b.runFrame();
    std::string err;
    if (!b.insertTape(tapes + "/" + c.file + ".wav", err)) { r.skipped = true; r.msg = "нет записи: " + err; return r; }
    bool cyr = false;
    typeText(b, std::string("MONIT\nM\n") + c.name + "\n", cyr, 2, 2, 2000);
    Tape& tp = b.tape();
    // До остановки ленты (конец или мотор выключен загрузчиком), затем — до конца записи.
    bool moved = false;
    for (int f = 0; f < 20000; ++f) {
        b.runFrame();
        const bool m = tp.moving(b.totalTicks());
        moved |= m;
        if (moved && !m) break;
    }
    if (!moved) { r.msg = "лента не тронулась — монитор не начал читать"; return r; }
    if (tp.position(b.totalTicks()) < tp.length()) {
        tp.play(b.totalTicks());
        for (int f = 0; f < 40000 && tp.moving(b.totalTicks()); ++f) b.runFrame();
    }
    for (int f = 0; f < c.extraFrames; ++f) b.runFrame();
    if (c.key) {
        b.pressKey(c.key); b.setKeyHeld(true);
        for (int f = 0; f < 4; ++f) b.runFrame();
        b.setKeyHeld(false);
        for (int f = 0; f < c.afterKey; ++f) b.runFrame();
    }
    std::vector<uint8_t> vram(VRAM);
    for (size_t a = 0; a < VRAM; ++a) vram[a] = b.memory().peekByte(static_cast<uint16_t>(040000 + a));
    const std::string gpath = golden + "/" + c.file + ".vram.gz";
    if (update) {
        r.ok = writeGz(gpath, vram);
        r.msg = r.ok ? "эталон записан" : "не записался " + gpath;
        return r;
    }
    std::vector<uint8_t> ref;
    if (!readGz(gpath, ref)) { r.msg = "нет эталона " + gpath + " (--update)"; return r; }
    size_t diff = 0;
    for (size_t a = 0; a < VRAM; ++a) diff += vram[a] != ref[a];
    const double share = double(diff) / VRAM;
    char buf[96];
    std::snprintf(buf, sizeof buf, "экран отличается от эталона на %.1f%%", share * 100);
    r.ok = share <= kMaxDiff;
    r.msg = buf;
    return r;
}

} // namespace

int main(int argc, char** argv) {
    bool update = false;
    std::vector<std::string> only;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--update")) update = true;
        else only.push_back(argv[i]);
    }
    std::string tapes;
    if (const char* e = std::getenv("BK_PROTECTED_TAPES")) tapes = e;
    else if (const char* h = std::getenv("HOME")) tapes = std::string(h) + "/emulators/БК-0010-01/tapes/protected";
    struct stat st{};
    if (tapes.empty() || stat(tapes.c_str(), &st) != 0) {
        std::printf("SKIP: нет каталога защищённых кассет (%s; BK_PROTECTED_TAPES)\n", tapes.c_str());
        return 77;
    }
    const std::string golden = std::string(BK_TEST_DATA_DIR) + "/protected";

    std::vector<const Case*> cases;
    for (const Case& c : kCases)
        if (only.empty() || std::find(only.begin(), only.end(), c.file) != only.end()) cases.push_back(&c);
    std::vector<Result> res(cases.size());
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    const unsigned n = std::max(1u, std::min<unsigned>(std::thread::hardware_concurrency(), cases.size()));
    for (unsigned t = 0; t < n; ++t)
        pool.emplace_back([&] {
            for (size_t i; (i = next++) < cases.size();) res[i] = runCase(*cases[i], tapes, golden, update);
        });
    for (auto& t : pool) t.join();

    int fails = 0, skips = 0;
    for (size_t i = 0; i < cases.size(); ++i) {
        const char* tag = res[i].skipped ? "SKIP" : res[i].ok ? "ok  " : "FAIL";
        std::printf("%s %-15s %s\n", tag, cases[i]->file, res[i].msg.c_str());
        fails += !res[i].ok && !res[i].skipped;
        skips += res[i].skipped;
    }
    std::printf("%zu кассет: %d не прошли, %d пропущены\n", cases.size(), fails, skips);
    return fails ? 1 : 0;
}
