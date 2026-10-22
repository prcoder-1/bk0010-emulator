#pragma once
#include <string>
#include <map>
#include <vector>
#include <functional>
#include <cstdint>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QString>
#include "Board.h"
#include "Joystick.h"

// Инструменты MCP: одно место со списком имён — по нему строятся объявления
// обработчиков (tool_<имя>) и таблица диспетчера (callTool).
#define BK_MCP_TOOLS(X) \
    X(bk_load) X(bk_fdd_wlog) X(bk_disk_save) X(bk_machine) X(bk_reset) X(bk_run) \
    X(bk_run_until) X(bk_step) X(bk_step_over) X(bk_regs) X(bk_set_reg) X(bk_read_mem) \
    X(bk_write_mem) X(bk_disasm) X(bk_break) X(bk_unbreak) X(bk_breakpoints) X(bk_key) \
    X(bk_joystick) X(bk_screenshot) X(bk_step_out) X(bk_audio) X(bk_state_save) \
    X(bk_state_load) X(bk_symbols) X(bk_hotspots) X(bk_watch) X(bk_unwatch) X(bk_watchpoints) \
    X(bk_backtrace) X(bk_search_mem) X(bk_diff_mem) X(bk_type) X(bk_callers) X(bk_callees) \
    X(bk_frames) X(bk_coverage) X(bk_profile_reset) X(bk_calltree) X(bk_profile) X(bk_vram) \
    X(bk_ocr) X(bk_emt_log) X(bk_xrefs) X(bk_io_state) X(bk_io_log) X(bk_joy_probe)

// Minimal Model Context Protocol server (JSON-RPC 2.0 over newline-delimited
// stdio) exposing the BK-0010 emulator as debugging tools for an MCP client
// such as Claude Code. Reuses the Qt-free core (bk::Board) plus QtCore JSON.
class McpServer {
public:
    QString fddState() const;
    QString hddState() const;             // сводка по винчестерам для bk_io_state
    QString fddLog(int limit) const;      // сводка по контроллеру НГМД для bk_io_state

    // Машина и носители — из командной строки (тот же разбор, что у GUI и безголового режима).
    // choice — выбор машины (--machine): auto — машину выбирает программа в bk_load.
    explicit McpServer(std::string romDir, const bk::Board::Setup& setup = {},
                       const bk::Board::Media& media = {},
                       bk::MachineChoice choice = bk::MachineChoice::Auto);

    // Run the stdio message loop until EOF. Returns a process exit code.
    int run();

private:
    bk::Board   board_;
    std::string romDir_;
    bool        romsOk_ = false;
    bk::MachineChoice choice_ = bk::MachineChoice::Auto;   // выбор машины: auto — по программе
    bk::Azbk::Config azCfg_;            // AZБК: карта SD, диски, запись — для bk_reset
    QString     lastBin_;     // загруженная программа (пусто — после включения питания её нет)
    // Свои имена (bk_symbols), а за ними — имена ПЗУ контроллера дисковода (roms/*.sym).
    const std::string* symAt(uint16_t a) const;
    bool symAddrOf(const std::string& name, uint16_t& a) const;
    std::vector<uint8_t> memSnap_;            // RAM snapshot for bk_diff_mem

    // --- отладка игр ---
    bk::JoyStandard joyStd_ = bk::JoyStandard::Standard;  // липкая раскладка джойстика
    bool    cyrState_ = false;                 // текущий регистр РУС/ЛАТ машины
    QString protoVer_ = "2024-11-05";          // согласованная версия MCP (для audio-блока)
    std::map<QString, std::vector<uint8_t>> stateSlots_;  // чекпоинты в памяти

    // Один шаг сценария ввода: что подать перед кадром `frame` (от начала прогона).
    struct InputStep {
        int      frame = 0;
        bool     hasKey = false;   uint16_t key = 0;
        bool     releaseKey = false;
        bool     hasJoy = false;   uint16_t joy = 0;
    };
    struct RunOutcome { int frames = 0; QString reason = "frame-limit"; QString extra; };

    // Прогнать кадры 50 Гц, применяя сценарий ввода; останов по точке останова,
    // точке наблюдения, HALT или лимиту кадров. afterFrame вызывается после
    // каждого кадра (bk_audio сливает им FIFO динамика, чтобы не терять звук).
    RunOutcome runFrames(int maxFrames, const std::vector<InputStep>& script = {},
                         const std::function<void()>& afterFrame = {});
    bool parseInputScript(const QJsonArray& arr, std::vector<InputStep>& out, QString& err);
    // Разобрать аргументы key/code в последовательность кодов КОИ-7 (с префиксом
    // РУС/ЛАТ, если нужен). present=false, если ни key, ни code не заданы.
    bool resolveKey(const QJsonObject& args, std::vector<uint16_t>& codes,
                    bool& present, QString& warn, QString& err);
    // Разобрать buttons/bits/add/release в значение порта 0177714.
    bool resolveJoy(const QJsonObject& args, uint16_t& value, bool& present, QString& err);
    QString runText(const RunOutcome& r);
    // Разложить фронты динамика (начиная с такта tick0) на сегменты-тоны.
    QString toneReport(uint64_t tick0) const;

    // --- JSON-RPC plumbing ---
    void send(const QJsonObject& msg) const;
    void handleMessage(const QJsonObject& req);
    void reply(const QJsonValue& id, const QJsonObject& result);
    void replyError(const QJsonValue& id, int code, const QString& message);

    QJsonArray  toolDefs() const;
    QJsonObject callTool(const QString& name, const QJsonObject& args);
#define BK_MCP_DECLARE(n) QJsonObject tool_##n(const QJsonObject& args);
    BK_MCP_TOOLS(BK_MCP_DECLARE)
#undef BK_MCP_DECLARE
    QJsonObject callersOrCallees(const QJsonObject& args, const QString& name);
    // frames > 0 у bk_profile/bk_calltree: профиль только за отрезок прогона.
    bool profileWindow(const QJsonObject& args, QString& note, QString& err);

    // --- helpers ---
    bool    resolveAddr(const QJsonObject& args, const char* key, uint16_t& out, QString& err);
    QString regsText();
    // Включить другую машину (как выключить одну и включить другую): ПЗУ, частота, карта
    // памяти, отрисовка по лучу на 11М, сброс. false — машина уже такая.
    bool    switchMachine(bool want11);
    QString machineText();              // сводка: модель, частота, AY, СМК, диск
    QString disasmText(uint16_t addr, int count);
};
