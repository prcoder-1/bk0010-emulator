#include "McpServer.h"
#include "Disasm.h"
#include "Screen.h"
#include <QJsonDocument>
#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QImage>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

using bk::Board;

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
static QString oct6(uint16_t v) { return QString::asprintf("%06o", v); }

// Write mono 16-bit PCM samples as a WAV file.
static bool writeWav(const QString& path, const std::vector<int16_t>& s, int rate) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    auto u32 = [&](uint32_t v) { char b[4] = {char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; f.write(b, 4); };
    auto u16 = [&](uint16_t v) { char b[2] = {char(v), char(v >> 8)}; f.write(b, 2); };
    const uint32_t dataBytes = (uint32_t)s.size() * 2;
    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(1);          // PCM, 1 channel
    u32(rate); u32(rate * 2); u16(2); u16(16);            // byte rate, block align, bits
    f.write("data", 4); u32(dataBytes);
    if (dataBytes) f.write(reinterpret_cast<const char*>(s.data()), dataBytes);
    f.close();
    return true;
}

static QJsonObject textContent(const QString& text, bool isError = false) {
    QJsonObject c; c["type"] = "text"; c["text"] = text;
    QJsonArray arr; arr.append(c);
    QJsonObject r; r["content"] = arr; r["isError"] = isError;
    return r;
}

// Parse a number given as JSON int or a string in C-style/octal/hex/decimal.
static bool parseNumber(const QJsonValue& v, long& out) {
    if (v.isDouble()) { out = (long)v.toDouble(); return true; }
    if (v.isString()) {
        QString s = v.toString().trimmed();
        bool ok = false;
        if (s.startsWith("0x") || s.startsWith("0X")) out = s.mid(2).toLong(&ok, 16);
        else if (s.startsWith("0o")) out = s.mid(2).toLong(&ok, 8);
        else if (s.startsWith("0") && s.size() > 1) out = s.toLong(&ok, 8); // leading 0 => octal (BK convention)
        else out = s.toLong(&ok, 10);
        return ok;
    }
    return false;
}

McpServer::McpServer(std::string romDir) : romDir_(std::move(romDir)) {
    romsOk_ = board_.loadRoms(romDir_);
    board_.reset();
    board_.trace().setEnabled(true);      // collect hot-spot data from the start
    board_.trace().setFlameEnabled(true); // maintain the shadow call stack (bk_backtrace)
}

// ---------------------------------------------------------------------------
// JSON-RPC transport (newline-delimited over stdio)
// ---------------------------------------------------------------------------
void McpServer::send(const QJsonObject& msg) const {
    QByteArray line = QJsonDocument(msg).toJson(QJsonDocument::Compact);
    std::cout.write(line.constData(), line.size());
    std::cout.put('\n');
    std::cout.flush();
}

void McpServer::reply(const QJsonValue& id, const QJsonObject& result) {
    QJsonObject o; o["jsonrpc"] = "2.0"; o["id"] = id; o["result"] = result;
    send(o);
}

void McpServer::replyError(const QJsonValue& id, int code, const QString& message) {
    QJsonObject err; err["code"] = code; err["message"] = message;
    QJsonObject o; o["jsonrpc"] = "2.0"; o["id"] = id; o["error"] = err;
    send(o);
}

int McpServer::run() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        QJsonParseError perr{};
        QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(line), &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
            std::cerr << "mcp: bad JSON: " << perr.errorString().toStdString() << "\n";
            continue;
        }
        handleMessage(doc.object());
    }
    return 0;
}

void McpServer::handleMessage(const QJsonObject& req) {
    const QString method = req.value("method").toString();
    const QJsonValue id = req.value("id");
    const bool isNotification = !req.contains("id");

    if (method == "initialize") {
        QJsonObject caps; caps["tools"] = QJsonObject{{"listChanged", false}};
        QJsonObject info; info["name"] = "bk0010-emulator"; info["version"] = "0.1";
        QString ver = req.value("params").toObject().value("protocolVersion").toString();
        if (ver.isEmpty()) ver = "2024-11-05";
        QJsonObject res;
        res["protocolVersion"] = ver;
        res["capabilities"] = caps;
        res["serverInfo"] = info;
        reply(id, res);
        return;
    }
    if (method == "notifications/initialized" || isNotification) return; // no response
    if (method == "ping") { reply(id, QJsonObject{}); return; }
    if (method == "tools/list") {
        QJsonObject res; res["tools"] = toolDefs();
        reply(id, res);
        return;
    }
    if (method == "resources/list") { reply(id, QJsonObject{{"resources", QJsonArray{}}}); return; }
    if (method == "prompts/list")   { reply(id, QJsonObject{{"prompts", QJsonArray{}}}); return; }
    if (method == "tools/call") {
        const QJsonObject params = req.value("params").toObject();
        const QString name = params.value("name").toString();
        const QJsonObject args = params.value("arguments").toObject();
        bool isError = false;
        QJsonObject result = callTool(name, args, isError);
        reply(id, result);
        return;
    }
    replyError(id, -32601, "Method not found: " + method);
}

// ---------------------------------------------------------------------------
// tool definitions (name, description, JSON-Schema of arguments)
// ---------------------------------------------------------------------------
static QJsonObject schema(std::initializer_list<std::pair<QString, QJsonObject>> props,
                          std::initializer_list<QString> required = {}) {
    QJsonObject p;
    for (auto& kv : props) p[kv.first] = kv.second;
    QJsonArray req; for (auto& r : required) req.append(r);
    QJsonObject s; s["type"] = "object"; s["properties"] = p; s["required"] = req;
    return s;
}
static QJsonObject P(const QString& type, const QString& desc) {
    return QJsonObject{{"type", type}, {"description", desc}};
}
static QJsonObject tool(const QString& name, const QString& desc, const QJsonObject& sch) {
    return QJsonObject{{"name", name}, {"description", desc}, {"inputSchema", sch}};
}

QJsonArray McpServer::toolDefs() const {
    QJsonArray t;
    const QJsonObject addrArg = P("string", "Address (decimal, 0x hex, or leading-0 octal) OR a symbol name");

    t.append(tool("bk_load", "Load a .BIN game/program (boots the monitor first) and start it.",
        schema({{"path", P("string", "Path to the .BIN file")},
                {"run", P("boolean", "Set PC to the load address and start (default true)")}}, {"path"})));
    t.append(tool("bk_reset", "Power-on reset the machine.", schema({})));
    t.append(tool("bk_run", "Run frames (with 50 Hz interrupts) until a breakpoint, HALT, or the frame limit.",
        schema({{"max_frames", P("integer", "Max 50 Hz frames to run (default 200 = ~4 s)")}})));
    t.append(tool("bk_run_until", "Run until PC reaches an address/symbol (or a breakpoint / tick limit).",
        schema({{"addr", addrArg}, {"max_ticks", P("integer", "CPU-tick limit (default 20000000)")}}, {"addr"})));
    t.append(tool("bk_step", "Execute N single instructions.",
        schema({{"count", P("integer", "Instruction count (default 1)")}})));
    t.append(tool("bk_step_over", "Step one instruction, stepping over JSR/EMT calls.", schema({})));
    t.append(tool("bk_regs", "Read the CPU registers R0-R7, SP, PC and PSW (with flags).", schema({})));
    t.append(tool("bk_set_reg", "Set a register (R0..R7, SP, PC, PSW).",
        schema({{"name", P("string", "R0..R7 / SP / PC / PSW")}, {"value", P("string", "New value")}}, {"name", "value"})));
    t.append(tool("bk_read_mem", "Read memory as octal words (or bytes).",
        schema({{"addr", addrArg}, {"len", P("integer", "Number of bytes (default 32)")},
                {"format", P("string", "words|bytes (default words)")}}, {"addr"})));
    t.append(tool("bk_write_mem", "Write words or bytes to memory.",
        schema({{"addr", addrArg}, {"words", P("array", "Array of 16-bit words")},
                {"bytes", P("array", "Array of bytes")}}, {"addr"})));
    t.append(tool("bk_disasm", "Disassemble instructions.",
        schema({{"addr", P("string", "Address/symbol, or 'pc' for the current PC (default pc)")},
                {"count", P("integer", "Instruction count (default 16)")}})));
    t.append(tool("bk_break", "Set a breakpoint at an address/symbol.", schema({{"addr", addrArg}}, {"addr"})));
    t.append(tool("bk_unbreak", "Remove a breakpoint (or all).",
        schema({{"addr", addrArg}, {"all", P("boolean", "Remove all breakpoints")}})));
    t.append(tool("bk_breakpoints", "List active breakpoints.", schema({})));
    t.append(tool("bk_key", "Press a BK-0010 key (KOI-7 code), optionally holding it down. "
                  "Games that poll the physical key-held bit (0177716, e.g. Digger's movement) "
                  "only register input while held: use hold=true then bk_run to drive them, and "
                  "hold=false (code optional) to release.",
        schema({{"code", P("integer", "BK key code, e.g. 012=Enter, 040=Space, 031=right, 010=left, 032=up, 033=down")},
                {"hold", P("boolean", "Hold the key physically down across frames (default false = tap)")}}, {})));
    t.append(tool("bk_screenshot", "Render the BK screen to a PNG file.",
        schema({{"path", P("string", "Output PNG path")}, {"mono", P("boolean", "512x256 monochrome (default false=colour)")}}, {"path"})));
    t.append(tool("bk_audio", "Run frames while capturing the speaker (piezo, 0177716 bit 6) "
                  "to a mono 16-bit WAV file, and report peak level, active fraction and rough "
                  "pitch. Lets you verify sound output (drive events with bk_key/bk_run first).",
        schema({{"path", P("string", "Output WAV path")},
                {"frames", P("integer", "50 Hz frames to run while capturing (default 100 = 2 s)")}}, {"path"})));
    t.append(tool("bk_state_save", "Save full emulator state to a file.", schema({{"path", P("string", "Path")}}, {"path"})));
    t.append(tool("bk_state_load", "Restore full emulator state from a file.", schema({{"path", P("string", "Path")}}, {"path"})));
    t.append(tool("bk_symbols", "Load a symbol table from a linker .map file (enables symbol names in break/read/disasm).",
        schema({{"path", P("string", "Path to the .map file")}}, {"path"})));
    t.append(tool("bk_hotspots", "List the most-executed instructions (hot code) from the trace.",
        schema({{"count", P("integer", "How many (default 20)")}})));
    t.append(tool("bk_watch", "Set a data watchpoint: stop the next bk_run/bk_run_until when an "
                  "address is read and/or written. Great for finding who touches a variable.",
        schema({{"addr", addrArg},
                {"mode", P("string", "read|write|rw (default write)")}}, {"addr"})));
    t.append(tool("bk_unwatch", "Remove a data watchpoint (or all).",
        schema({{"addr", addrArg}, {"all", P("boolean", "Remove all watchpoints")}})));
    t.append(tool("bk_watchpoints", "List active data watchpoints.", schema({})));
    t.append(tool("bk_backtrace", "Reconstruct the call stack at the current PC by scanning the "
                  "hardware stack (R6) for return addresses left by JSR. Shows how execution got here.",
        schema({{"depth", P("integer", "Max frames to report (default 24)")}})));
    t.append(tool("bk_search_mem", "Search memory for a value or string. Give ONE of: word, byte, "
                  "bytes (array), or text. Returns matching octal addresses.",
        schema({{"word", P("string", "16-bit word to find (dec/hex/octal)")},
                {"byte", P("string", "single byte to find")},
                {"bytes", P("array", "sequence of bytes to find")},
                {"text", P("string", "ASCII/KOI-7 string to find")},
                {"start", addrArg}, {"end", addrArg},
                {"max", P("integer", "Max hits to list (default 64)")}})));
    t.append(tool("bk_diff_mem", "Find changed memory. action=save snapshots RAM; action=diff lists "
                  "cells that changed since the snapshot. Snapshot before an action (e.g. a keypress), "
                  "diff after, to locate the variable it changed.",
        schema({{"action", P("string", "save|diff (default diff)")},
                {"start", addrArg}, {"end", addrArg},
                {"max", P("integer", "Max changed cells to list (default 64)")}})));
    t.append(tool("bk_type", "Type an ASCII string through the keyboard (one char per few frames, "
                  "like real typing). Newline = Enter. Use to drive menus / text entry.",
        schema({{"text", P("string", "String to type")},
                {"max_frames", P("integer", "Frame budget (default 600)")}}, {"text"})));
    return t;
}

// ---------------------------------------------------------------------------
// address / symbol resolution
// ---------------------------------------------------------------------------
bool McpServer::resolveAddr(const QJsonObject& args, const char* key, uint16_t& out, QString& err) {
    QJsonValue v = args.value(key);
    if (v.isString()) {
        QString s = v.toString().trimmed();
        if (s.compare("pc", Qt::CaseInsensitive) == 0) { out = board_.cpu().pc(); return true; }
        auto it = symAddr_.find(s.toStdString());
        if (it != symAddr_.end()) { out = it->second; return true; }
    }
    long n;
    if (parseNumber(v, n)) { out = (uint16_t)(n & 0xFFFF); return true; }
    err = QString("cannot resolve address/symbol '%1'").arg(v.toString());
    return false;
}

int McpServer::loadSymbols(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return -1;
    symName_.clear(); symAddr_.clear();
    QTextStream in(&f);
    // Match GNU-ld map lines like "  0x0000000000001234   symbol_name"
    // and simple "symbol = 0x1234 / 0NNN / decimal" definitions.
    QRegularExpression reLd("^\\s+0x([0-9a-fA-F]+)\\s+([A-Za-z_.$][\\w.$]*)\\s*$");
    QRegularExpression reEq("^\\s*([A-Za-z_.$][\\w.$]*)\\s*=\\s*(0x[0-9a-fA-F]+|0[0-7]*|\\d+)");
    int count = 0;
    while (!in.atEnd()) {
        QString line = in.readLine();
        auto m = reLd.match(line);
        if (m.hasMatch()) {
            bool ok = false; uint32_t a = m.captured(1).toUInt(&ok, 16);
            if (ok) { uint16_t a16 = a & 0xFFFF; std::string nm = m.captured(2).toStdString();
                      symName_[a16] = nm; symAddr_[nm] = a16; ++count; }
            continue;
        }
        auto e = reEq.match(line);
        if (e.hasMatch()) {
            long n; if (parseNumber(QJsonValue(e.captured(2)), n)) {
                uint16_t a16 = n & 0xFFFF; std::string nm = e.captured(1).toStdString();
                symName_[a16] = nm; symAddr_[nm] = a16; ++count;
            }
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// formatting helpers
// ---------------------------------------------------------------------------
QString McpServer::regsText() {
    const auto& c = board_.cpu();
    auto flag = [&](uint16_t m, char ch) { return (c.psw & m) ? ch : '-'; };
    QString f = QString("%1%2%3%4%5")
        .arg(flag(bk::Cpu::CC_T, 'T')).arg(flag(bk::Cpu::CC_N, 'N')).arg(flag(bk::Cpu::CC_Z, 'Z'))
        .arg(flag(bk::Cpu::CC_V, 'V')).arg(flag(bk::Cpu::CC_C, 'C'));
    QString s;
    for (int i = 0; i < 6; ++i) s += QString("R%1=%2  ").arg(i).arg(oct6(c.r[i]));
    s += QString("SP=%1  PC=%2\nPSW=%3 [%4]  %5")
        .arg(oct6(c.r[6])).arg(oct6(c.r[7])).arg(oct6(c.psw)).arg(f)
        .arg(c.halted() ? "HALTED" : "running");
    // symbol at PC, if known
    auto it = symName_.find(c.r[7]);
    if (it != symName_.end()) s += QString("  ; PC=<%1>").arg(QString::fromStdString(it->second));
    return s;
}

QString McpServer::disasmText(uint16_t addr, int count) {
    const bk::Memory& mem = board_.memory();
    QString out;
    for (int i = 0; i < count; ++i) {
        bk::DisasmLine d = bk::disasm(mem, addr);
        QString mark = (addr == board_.cpu().pc()) ? ">" : (board_.hasBreakpoint(addr) ? "*" : " ");
        QString sym; auto it = symName_.find(addr);
        if (it != symName_.end()) sym = QString(" <%1>").arg(QString::fromStdString(it->second));
        out += QString("%1%2%3  %4\n").arg(mark).arg(oct6(addr)).arg(sym).arg(QString::fromStdString(d.text));
        addr += d.words * 2;
    }
    return out;
}

// ---------------------------------------------------------------------------
// tool dispatch
// ---------------------------------------------------------------------------
QJsonObject McpServer::callTool(const QString& name, const QJsonObject& args, bool& isError) {
    isError = false;
    auto fail = [&](const QString& m) { isError = true; return textContent("Error: " + m, true); };
    const bk::Memory& cmem = board_.memory();
    (void)cmem;

    if (name == "bk_load") {
        if (!romsOk_) return fail("ROMs not loaded from " + QString::fromStdString(romDir_));
        board_.ensureMonitorBooted();
        uint16_t a = 0, l = 0;
        bool run = args.value("run").toBool(true);
        if (!board_.loadBin(args.value("path").toString().toStdString(), run, &a, &l))
            return fail("cannot load " + args.value("path").toString());
        lastBin_ = args.value("path").toString();
        return textContent(QString("Loaded %1: addr=%2 len=%3 (octal).%4\n%5")
            .arg(lastBin_).arg(oct6(a)).arg(oct6(l))
            .arg(run ? QString(" PC set to %1.").arg(oct6(a)) : QString())
            .arg(regsText()));
    }
    if (name == "bk_reset") {
        board_.reset();
        return textContent("Reset.\n" + regsText());
    }
    if (name == "bk_run") {
        int maxFrames = args.value("max_frames").toInt(200);
        QString reason = "frame-limit", extra; int ran = 0;
        for (; ran < maxFrames; ++ran) {
            board_.runFrame();
            if (board_.breakHit()) {
                ++ran;
                if (board_.watchHit()) {
                    reason = "watchpoint";
                    extra = QString("\nWatch %1 %2 by PC=%3  %4")
                        .arg(oct6(board_.watchAddr())).arg(board_.watchWrite() ? "WRITE" : "READ")
                        .arg(oct6(board_.watchPc()))
                        .arg(QString::fromStdString(bk::disasm(board_.memory(), board_.watchPc()).text));
                } else reason = "breakpoint";
                board_.clearBreakHit();
                break;
            }
            if (board_.cpu().halted()) { reason = "halted"; ++ran; break; }
        }
        return textContent(QString("Stopped (%1) after %2 frames.%3\n%4\n%5")
            .arg(reason).arg(ran).arg(extra).arg(regsText()).arg(disasmText(board_.cpu().pc(), 3)));
    }
    if (name == "bk_run_until") {
        uint16_t target; QString err;
        if (!resolveAddr(args, "addr", target, err)) return fail(err);
        int maxTicks = args.value("max_ticks").toInt(20000000);
        bool hit = board_.runUntil(target, maxTicks);
        board_.clearBreakHit();
        return textContent(QString("%1 target %2.\n%3")
            .arg(hit ? "Reached" : "Did NOT reach").arg(oct6(target)).arg(regsText()));
    }
    if (name == "bk_step") {
        int cnt = args.value("count").toInt(1);
        for (int i = 0; i < cnt && !board_.cpu().halted(); ++i) board_.stepInstruction();
        return textContent(regsText() + "\n" + disasmText(board_.cpu().pc(), 3));
    }
    if (name == "bk_step_over") {
        uint16_t pc = board_.cpu().pc();
        bk::DisasmLine d = bk::disasm(board_.memory(), pc);
        int idx = board_.memory().peekWord(pc) >> 6;
        bool isCall = (idx >= 040 && idx <= 047) || (idx >= 01040 && idx <= 01047);
        if (isCall) { board_.runUntil((uint16_t)(pc + d.words * 2), 20000000); board_.clearBreakHit(); }
        else board_.stepInstruction();
        return textContent(regsText() + "\n" + disasmText(board_.cpu().pc(), 3));
    }
    if (name == "bk_regs") return textContent(regsText());
    if (name == "bk_set_reg") {
        QString rn = args.value("name").toString().toUpper();
        long val; if (!parseNumber(args.value("value"), val)) return fail("bad value");
        uint16_t v = val & 0xFFFF;
        auto& c = board_.cpu();
        if (rn == "PSW") c.psw = v;
        else if (rn == "SP") c.r[6] = v;
        else if (rn == "PC") c.r[7] = v;
        else if (rn.size() == 2 && rn[0] == 'R' && rn[1].isDigit()) c.r[rn[1].digitValue()] = v;
        else return fail("unknown register " + rn);
        return textContent(regsText());
    }
    if (name == "bk_read_mem") {
        uint16_t a; QString err; if (!resolveAddr(args, "addr", a, err)) return fail(err);
        int len = args.value("len").toInt(32);
        bool bytes = args.value("format").toString() == "bytes";
        QString out;
        if (bytes) {
            for (int i = 0; i < len; i += 16) {
                QString line = oct6((uint16_t)(a + i)) + ":";
                for (int j = 0; j < 16 && i + j < len; ++j)
                    line += QString::asprintf(" %03o", board_.memory().peekByte((uint16_t)(a + i + j)));
                out += line + "\n";
            }
        } else {
            int words = (len + 1) / 2;
            for (int i = 0; i < words; i += 8) {
                QString line = oct6((uint16_t)(a + i * 2)) + ":";
                for (int j = 0; j < 8 && i + j < words; ++j)
                    line += " " + oct6(board_.memory().peekWord((uint16_t)(a + (i + j) * 2)));
                out += line + "\n";
            }
        }
        return textContent(out);
    }
    if (name == "bk_write_mem") {
        uint16_t a; QString err; if (!resolveAddr(args, "addr", a, err)) return fail(err);
        int n = 0;
        if (args.contains("words")) {
            QJsonArray w = args.value("words").toArray();
            for (const auto& v : w) { long x; parseNumber(v, x); board_.memory().pokeWord((uint16_t)(a + n * 2), x & 0xFFFF); ++n; }
            return textContent(QString("Wrote %1 words at %2.").arg(n).arg(oct6(a)));
        }
        if (args.contains("bytes")) {
            QJsonArray b = args.value("bytes").toArray();
            for (const auto& v : b) { long x; parseNumber(v, x); board_.memory().pokeByte((uint16_t)(a + n), x & 0xFF); ++n; }
            return textContent(QString("Wrote %1 bytes at %2.").arg(n).arg(oct6(a)));
        }
        return fail("provide 'words' or 'bytes'");
    }
    if (name == "bk_disasm") {
        uint16_t a = board_.cpu().pc(); QString err;
        if (args.contains("addr")) { if (!resolveAddr(args, "addr", a, err)) return fail(err); }
        return textContent(disasmText(a, args.value("count").toInt(16)));
    }
    if (name == "bk_break") {
        uint16_t a; QString err; if (!resolveAddr(args, "addr", a, err)) return fail(err);
        board_.addBreakpoint(a);
        return textContent(QString("Breakpoint set at %1.").arg(oct6(a)));
    }
    if (name == "bk_unbreak") {
        if (args.value("all").toBool(false)) {
            for (uint16_t bp : std::vector<uint16_t>(board_.breakpoints().begin(), board_.breakpoints().end()))
                board_.removeBreakpoint(bp);
            return textContent("All breakpoints removed.");
        }
        uint16_t a; QString err; if (!resolveAddr(args, "addr", a, err)) return fail(err);
        board_.removeBreakpoint(a);
        return textContent(QString("Breakpoint at %1 removed.").arg(oct6(a)));
    }
    if (name == "bk_breakpoints") {
        QString out = "Breakpoints:";
        if (board_.breakpoints().empty()) out += " (none)";
        for (uint16_t bp : board_.breakpoints()) {
            out += " " + oct6(bp);
            auto it = symName_.find(bp); if (it != symName_.end()) out += QString("<%1>").arg(QString::fromStdString(it->second));
        }
        return textContent(out);
    }
    if (name == "bk_key") {
        long code = -1;
        if (args.contains("code") && !parseNumber(args.value("code"), code)) return fail("bad code");
        const bool hold = args.value("hold").toBool(false);
        // Latch the scan code (0177662) and drive the physical key-held state
        // (0177716 MAG_KEY, active-low). Games that poll the held bit — e.g. Digger,
        // which requires both the code AND a held key — only register input while
        // held, so `hold: true` lets bk_run drive movement; call again with
        // hold:false (or omit code) to release.
        if (code >= 0) board_.pressKey((uint16_t)code);
        board_.setKeyHeld(hold);
        QString msg;
        if (code >= 0) msg = QString("Key %1 %2.").arg(oct6((uint16_t)code))
                                 .arg(hold ? "pressed and held" : "tapped");
        else           msg = hold ? "Key held." : "Key released.";
        return textContent(msg);
    }
    if (name == "bk_screenshot") {
        board_.screen().setColorMode(!args.value("mono").toBool(false));
        board_.screen().render(board_.memory());
        QImage img(reinterpret_cast<const uchar*>(board_.screen().pixels()),
                   bk::Screen::TEX_W, bk::Screen::TEX_H, QImage::Format_ARGB32);
        QString path = args.value("path").toString();
        if (!img.copy().save(path)) return fail("cannot write " + path);
        return textContent("Wrote screenshot to " + path);
    }
    if (name == "bk_audio") {
        // Run `frames` 50 Hz frames while capturing the speaker (0177716 bit 6)
        // output as PCM, write a mono 16-bit WAV, and report basic analysis.
        QString path = args.value("path").toString();
        if (path.isEmpty()) return fail("path required");
        long frames = 100;
        if (args.contains("frames")) parseNumber(args.value("frames"), frames);
        if (frames < 1) frames = 1;
        const int rate = 44100;
        board_.sound().setEnabled(true);
        board_.sound().clear();               // drop anything buffered before capture
        std::vector<int16_t> samples;
        int16_t tmp[8192];
        for (long f = 0; f < frames; ++f) {
            board_.runFrame();
            size_t got;                        // drain each frame so nothing is lost
            while ((got = board_.sound().read(tmp, 8192)) > 0)
                samples.insert(samples.end(), tmp, tmp + got);
            if (board_.breakHit()) { board_.clearBreakHit(); break; }
        }
        if (!writeWav(path, samples, rate)) return fail("cannot write " + path);
        // Analysis: peak amplitude, fraction of non-silent samples, and a rough
        // dominant pitch from the zero-crossing rate (good for single tones).
        int peak = 0; long active = 0, crossings = 0; int prev = 0;
        for (size_t i = 0; i < samples.size(); ++i) {
            int v = samples[i];
            if (v > peak) peak = v; else if (-v > peak) peak = -v;
            if (v > 400 || v < -400) ++active;
            int sign = (v > 200) ? 1 : (v < -200) ? -1 : prev;
            if (sign && prev && sign != prev) ++crossings;
            if (sign) prev = sign;
        }
        double ms = samples.size() * 1000.0 / rate;
        double freq = (ms > 0) ? (crossings / 2.0) / (ms / 1000.0) : 0; // half-cycles/sec
        return textContent(QString(
            "Captured %1 samples (%2 ms) to %3\npeak=%4/32767  active=%5%%  ~pitch=%6 Hz")
            .arg(samples.size()).arg(ms, 0, 'f', 0).arg(path)
            .arg(peak).arg(samples.empty() ? 0 : active * 100 / (long)samples.size())
            .arg(freq, 0, 'f', 0));
    }
    if (name == "bk_state_save") {
        if (!board_.saveState(args.value("path").toString().toStdString())) return fail("save failed");
        return textContent("State saved to " + args.value("path").toString());
    }
    if (name == "bk_state_load") {
        if (!board_.loadState(args.value("path").toString().toStdString())) return fail("load failed");
        return textContent("State restored.\n" + regsText());
    }
    if (name == "bk_symbols") {
        int n = loadSymbols(args.value("path").toString());
        if (n < 0) return fail("cannot open " + args.value("path").toString());
        return textContent(QString("Loaded %1 symbols from %2.").arg(n).arg(args.value("path").toString()));
    }
    if (name == "bk_hotspots") {
        int count = args.value("count").toInt(20);
        std::vector<std::pair<uint32_t, uint16_t>> hot;
        for (int a = 0; a < 0x10000; a += 2) {
            uint32_t c = board_.trace().execCount((uint16_t)a);
            if (c) hot.push_back({c, (uint16_t)a});
        }
        std::sort(hot.rbegin(), hot.rend());
        QString out = QString("Hot instructions (%1 unique executed):\n").arg(hot.size());
        for (int i = 0; i < count && i < (int)hot.size(); ++i) {
            uint16_t a = hot[i].second;
            bk::DisasmLine d = bk::disasm(board_.memory(), a);
            QString sym; auto it = symName_.find(a);
            if (it != symName_.end()) sym = QString(" <%1>").arg(QString::fromStdString(it->second));
            out += QString("%1%2  %3  x%4\n").arg(oct6(a)).arg(sym).arg(QString::fromStdString(d.text)).arg(hot[i].first);
        }
        return textContent(out);
    }

    if (name == "bk_watch") {
        uint16_t a; QString err; if (!resolveAddr(args, "addr", a, err)) return fail(err);
        QString mode = args.value("mode").toString("write").toLower();
        bool r = (mode == "read" || mode == "rw"), w = (mode == "write" || mode == "rw");
        if (!r && !w) w = true;
        board_.addWatch(a, r, w);
        return textContent(QString("Watch set at %1 (%2%3). Run bk_run / bk_run_until to trigger.")
            .arg(oct6(a)).arg(r ? "R" : "").arg(w ? "W" : ""));
    }
    if (name == "bk_unwatch") {
        if (args.value("all").toBool()) { board_.clearWatches(); return textContent("All watchpoints removed."); }
        uint16_t a; QString err; if (!resolveAddr(args, "addr", a, err)) return fail(err);
        board_.removeWatch(a);
        return textContent("Watch removed at " + oct6(a));
    }
    if (name == "bk_watchpoints") {
        QString out = "Watchpoints:\n";
        for (auto& kv : board_.watchpoints())
            out += QString("  %1  %2%3\n").arg(oct6(kv.first))
                       .arg(kv.second & 1 ? "R" : "").arg(kv.second & 2 ? "W" : "");
        if (board_.watchpoints().empty()) out += "  (none)\n";
        return textContent(out);
    }
    if (name == "bk_backtrace") {
        int depth = args.value("depth").toInt(24);
        const bk::Memory& mem = board_.memory();
        auto sym = [&](uint16_t x) {
            auto it = symName_.find(x);
            return it != symName_.end() ? QString(" <%1>").arg(QString::fromStdString(it->second)) : QString();
        };
        auto isJsr = [](uint16_t ir) { return ir >= 0004000 && ir <= 0004777; };
        uint16_t pc = board_.cpu().pc();

        // Preferred: the exact shadow call stack from the call tracker (JSR/return
        // + SP resync). Falls back to a heuristic stack scan if it's empty.
        std::vector<bk::Trace::Span> open;
        board_.trace().openFrames(open);
        if (!open.empty()) {
            QString out = "Call stack (from the call tracker, innermost first):\n";
            out += QString("  #0  PC=%1%2  %3\n").arg(oct6(pc)).arg(sym(pc))
                       .arg(QString::fromStdString(bk::disasm(mem, pc).text));
            int frame = 1;
            for (auto it = open.rbegin(); it != open.rend() && frame <= depth; ++it, ++frame)
                out += QString("  #%1  %2%3  (depth %4)\n").arg(frame).arg(oct6(it->func)).arg(sym(it->func)).arg(it->depth);
            return textContent(out);
        }

        QString out = "Call stack (heuristic, scanning the stack for JSR return addresses):\n";
        out += QString("  #0  %1%2  %3   ; PC\n").arg(oct6(pc)).arg(sym(pc))
                   .arg(QString::fromStdString(bk::disasm(mem, pc).text));
        int frame = 1;
        uint16_t sp = board_.cpu().sp();
        for (int i = 0; i < 1024 && frame < depth; i += 2) {
            uint16_t addr = (uint16_t)(sp + i);
            if (addr < sp) break;                       // wrapped past the top of memory
            uint16_t w = mem.peekWord(addr);
            if (w & 1) continue;                        // return addresses are even
            uint16_t jsrAt = 0; bool ok = false;        // JSR whose length lands exactly at w
            for (int back = 2; back <= 4; back += 2) {
                uint16_t j = (uint16_t)(w - back);
                if (isJsr(mem.peekWord(j)) && (uint16_t)(j + bk::disasm(mem, j).words * 2) == w) { jsrAt = j; ok = true; break; }
            }
            if (!ok) continue;
            out += QString("  #%1  %2%3  (call at %4)\n").arg(frame).arg(oct6(w)).arg(sym(w)).arg(oct6(jsrAt));
            ++frame;
        }
        if (frame == 1) out += "  (no return addresses found — leaf routine or non-JSR flow)\n";
        return textContent(out);
    }
    if (name == "bk_search_mem") {
        const bk::Memory& mem = board_.memory();
        uint16_t start = 0, end = 0177777; QString err;
        if (args.contains("start") && !resolveAddr(args, "start", start, err)) return fail(err);
        if (args.contains("end")   && !resolveAddr(args, "end",   end,   err)) return fail(err);
        int maxHits = args.value("max").toInt(64);
        std::vector<uint8_t> pat;
        if (args.contains("text")) { QByteArray b = args.value("text").toString().toLatin1(); for (char c : b) pat.push_back((uint8_t)c); }
        else if (args.contains("bytes")) { for (auto v : args.value("bytes").toArray()) pat.push_back((uint8_t)(v.toInt() & 0xFF)); }
        else if (args.contains("byte")) { long n; if (!parseNumber(args.value("byte"), n)) return fail("bad byte"); pat.push_back((uint8_t)(n & 0xFF)); }
        else if (args.contains("word")) { long n; if (!parseNumber(args.value("word"), n)) return fail("bad word"); pat.push_back((uint8_t)(n & 0xFF)); pat.push_back((uint8_t)((n >> 8) & 0xFF)); }
        else return fail("give one of: word, byte, bytes, text");
        QString out = QString("Search (%1 bytes) in [%2..%3]:\n").arg(pat.size()).arg(oct6(start)).arg(oct6(end));
        int hits = 0;
        for (uint32_t a = start; pat.size() && a + pat.size() - 1 <= end && hits < maxHits; ++a) {
            bool m = true;
            for (size_t k = 0; k < pat.size(); ++k) if (mem.peekByte((uint16_t)(a + k)) != pat[k]) { m = false; break; }
            if (m) { out += "  " + oct6((uint16_t)a) + "\n"; ++hits; }
        }
        out += hits ? (hits >= maxHits ? QString("  ... (stopped at %1)\n").arg(maxHits) : QString())
                    : QString("  (no matches)\n");
        return textContent(out);
    }
    if (name == "bk_diff_mem") {
        const bk::Memory& mem = board_.memory();
        QString action = args.value("action").toString("diff").toLower();
        if (action == "save") {
            memSnap_.assign(0x10000, 0);
            for (int a = 0; a < 0x10000; ++a) memSnap_[a] = mem.peekByte((uint16_t)a);
            return textContent("Snapshot saved (64 KB). Do an action, then bk_diff_mem action=diff.");
        }
        if (memSnap_.size() != 0x10000) return fail("no snapshot — call bk_diff_mem action=save first");
        uint16_t start = 0, end = 0177777; QString err;
        if (args.contains("start") && !resolveAddr(args, "start", start, err)) return fail(err);
        if (args.contains("end")   && !resolveAddr(args, "end",   end,   err)) return fail(err);
        int maxCells = args.value("max").toInt(64);
        QString out = QString("Changed bytes in [%1..%2]:\n").arg(oct6(start)).arg(oct6(end));
        int n = 0;
        for (uint32_t a = start; a <= end && n < maxCells; ++a) {
            uint8_t cur = mem.peekByte((uint16_t)a);
            if (cur != memSnap_[a]) {
                out += QString("  %1: %2 -> %3\n").arg(oct6((uint16_t)a))
                           .arg(QString::asprintf("%03o", memSnap_[a])).arg(QString::asprintf("%03o", cur));
                ++n;
            }
        }
        out += n ? (n >= maxCells ? QString("  ... (stopped at %1; narrow with start/end)\n").arg(maxCells) : QString())
                 : QString("  (no changes)\n");
        return textContent(out);
    }
    if (name == "bk_type") {
        QByteArray b = args.value("text").toString().toLatin1();
        int budget = args.value("max_frames").toInt(600);
        int idx = 0, frames = 0;
        while (idx < b.size() && frames < budget) {
            if (!board_.keyReady()) {
                char c = b[idx++];
                board_.pressKey(c == '\n' ? 012 : (uint16_t)((uint8_t)c & 0177));
            }
            board_.runFrame(); ++frames;
        }
        for (int k = 0; k < 12 && board_.keyReady() && frames < budget; ++k) { board_.runFrame(); ++frames; }
        return textContent(QString("Typed %1/%2 chars in %3 frames.\n%4")
            .arg(idx).arg(b.size()).arg(frames).arg(regsText()));
    }

    return fail("unknown tool: " + name);
}
