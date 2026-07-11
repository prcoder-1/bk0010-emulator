#pragma once
#include <string>
#include <map>
#include <cstdint>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QString>
#include "Board.h"

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
