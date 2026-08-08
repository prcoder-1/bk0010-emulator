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

// Minimal Model Context Protocol server (JSON-RPC 2.0 over newline-delimited
// stdio) exposing the BK-0010 emulator as debugging tools for an MCP client
// such as Claude Code. Reuses the Qt-free core (bk::Board) plus QtCore JSON.
class McpServer {
public:
    explicit McpServer(std::string romDir);

    // Run the stdio message loop until EOF. Returns a process exit code.
    int run();

private:
    bk::Board   board_;
    std::string romDir_;
    bool        romsOk_ = false;
    QString     lastBin_;
    std::map<uint16_t, std::string> symName_; // addr -> symbol
    std::map<std::string, uint16_t> symAddr_; // symbol -> addr
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
    QJsonObject callTool(const QString& name, const QJsonObject& args, bool& isError);

    // --- helpers ---
    bool    resolveAddr(const QJsonObject& args, const char* key, uint16_t& out, QString& err);
    int     loadSymbols(const QString& path);
    QString regsText();
    QString disasmText(uint16_t addr, int count);
};
