#include "McpServer.h"
#include "Disasm.h"
#include "Screen.h"
#include "BkKeys.h"
#include "ScreenOcr.h"
#include <QJsonDocument>
#include <QFile>
#include <QTextStream>
#include <QStringList>
#include <QRegularExpression>
#include <QImage>
#include <QBuffer>
#include <tuple>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

using bk::Board;

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
static QString oct6(uint16_t v) { return QString::asprintf("%06o", v); }

// Читаемая подпись кода КОИ-7: " <enter>", " 'A'", " 'Ю'" — или пусто.
static QString keyLabel(uint16_t code) {
    if (const char* n = bk::bkKeyName(code)) return QString(" <%1>").arg(n);
    if (code >= 0x20 && code < 0x7F) return QString(" '%1'").arg(QChar(code));
    if (code >= 0140 && code <= 0177) {
        const QString h1 = QString::fromUtf8(bk::kKoi7H1Utf8);
        const int idx = code - 0140;
        if (idx < h1.size()) return QString(" '%1'").arg(h1.at(idx));
    }
    return QString();
}

// Build a mono 16-bit PCM WAV image in memory.
static void writeWavBytes(QByteArray& out, const std::vector<int16_t>& s, int rate) {
    out.clear();
    auto u32 = [&](uint32_t v) { char b[4] = {char(v), char(v >> 8), char(v >> 16), char(v >> 24)}; out.append(b, 4); };
    auto u16 = [&](uint16_t v) { char b[2] = {char(v), char(v >> 8)}; out.append(b, 2); };
    const uint32_t dataBytes = (uint32_t)s.size() * 2;
    out.append("RIFF", 4); u32(36 + dataBytes); out.append("WAVE", 4);
    out.append("fmt ", 4); u32(16); u16(1); u16(1);       // PCM, 1 channel
    u32(rate); u32(rate * 2); u16(2); u16(16);            // byte rate, block align, bits
    out.append("data", 4); u32(dataBytes);
    if (dataBytes) out.append(reinterpret_cast<const char*>(s.data()), dataBytes);
}

// Write mono 16-bit PCM samples as a WAV file.
static bool writeWav(const QString& path, const std::vector<int16_t>& s, int rate) {
    QByteArray wav;
    writeWavBytes(wav, s, rate);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const bool ok = f.write(wav) == wav.size();
    f.close();
    return ok;
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

McpServer::McpServer(std::string romDir, bool smk512) : romDir_(std::move(romDir)) {
    romsOk_ = board_.loadRoms(romDir_);
    board_.setSmk512(smk512);             // --smk: блок расширения памяти в разъёме МПИ
    board_.reset();
    board_.trace().setEnabled(true);      // collect hot-spot data from the start
    board_.trace().setFlameEnabled(true); // maintain the shadow call stack (bk_backtrace)
    board_.setSpeakerLog(true);           // фронты динамика — для разбора звука в bk_audio
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
        protoVer_ = ver;   // блок content type:"audio" появился в 2025-03-26
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

    t.append(tool("bk_load", "Load a .BIN game/program (boots the monitor first) and start it. "
                  "Use frames to run it right away, and reset=true when loading a second game "
                  "so its trace/hotspot data is not mixed with the previous one.",
        schema({{"path", P("string", "Path to the .BIN file")},
                {"run", P("boolean", "Set PC to the entry point and start (default true)")},
                {"reset", P("boolean", "Power-on reset + re-boot the monitor first (default false)")},
                {"frames", P("integer", "Run this many 50 Hz frames after loading (default 0)")}}, {"path"})));
    t.append(tool("bk_reset", "Power-on reset the machine.", schema({})));
    t.append(tool("bk_run", "Run frames (with 50 Hz interrupts) until a breakpoint, HALT, or the frame "
                  "limit. `input` scripts keyboard/joystick over time, so a whole control sequence "
                  "is reproducible in ONE call — e.g. "
                  "[{\"frame\":0,\"joy\":\"right\"},{\"frame\":25,\"joy\":\"right+fire1\"},{\"frame\":40,\"release_joy\":true}].",
        schema({{"max_frames", P("integer", "Max 50 Hz frames to run (default 200 = ~4 s)")},
                {"input", P("array", "Input timeline: objects {frame, key|code, release_key, "
                                     "joy|joy_bits, release_joy}; frame is 0-based from the start of this run")}})));
    t.append(tool("bk_run_until", "Run until PC reaches an address/symbol (or a breakpoint / tick limit).",
        schema({{"addr", addrArg}, {"max_ticks", P("integer", "CPU-tick limit (default 20000000)")}}, {"addr"})));
    t.append(tool("bk_step", "Execute N single instructions.",
        schema({{"count", P("integer", "Instruction count (default 1)")}})));
    t.append(tool("bk_step_over", "Step one instruction, stepping over JSR/EMT calls.", schema({})));
    t.append(tool("bk_step_out", "Run until the current subroutine returns to its caller (finish).",
        schema({{"max_ticks", P("integer", "CPU-tick limit (default 20000000)")}})));
    t.append(tool("bk_regs", "Read the CPU registers R0-R7, SP, PC and PSW (with flags).", schema({})));
    t.append(tool("bk_set_reg", "Set a register (R0..R7, SP, PC, PSW).",
        schema({{"name", P("string", "R0..R7 / SP / PC / PSW")}, {"value", P("string", "New value")}}, {"name", "value"})));
    t.append(tool("bk_read_mem", "Read memory as octal words (or bytes).",
        schema({{"addr", addrArg}, {"len", P("integer", "Number of bytes (default 32)")},
                {"format", P("string", "words|bytes (default words)")}}, {"addr"})));
    t.append(tool("bk_write_mem", "Write words or bytes to memory. Addresses in the I/O page "
                  "(>= 0177600) go THROUGH THE BUS by default, so the device actually sees the "
                  "write (e.g. poking 0177712 restarts the timer); ordinary memory is written "
                  "directly, bypassing ROM protection so you can patch ROM. `bus` overrides.",
        schema({{"addr", addrArg}, {"words", P("array", "Array of 16-bit words")},
                {"bytes", P("array", "Array of bytes")},
                {"bus", P("boolean", "Force writing through the bus (true) or straight into "
                                     "memory (false); default: auto by address")}}, {"addr"})));
    t.append(tool("bk_disasm", "Disassemble instructions.",
        schema({{"addr", P("string", "Address/symbol, or 'pc' for the current PC (default pc)")},
                {"count", P("integer", "Instruction count (default 16)")}})));
    t.append(tool("bk_break", "Set a breakpoint at an address/symbol, optionally conditional: it "
                  "only stops when `when` holds. Syntax: 'Rn OP v', '@addr OP v' (word) or "
                  "'@addr.b OP v' (byte); OP is == != < > >= <=.",
        schema({{"addr", addrArg}, {"when", P("string", "Optional condition, e.g. 'R0==5' or '@035120.b>3'")}}, {"addr"})));
    t.append(tool("bk_unbreak", "Remove a breakpoint (or all).",
        schema({{"addr", addrArg}, {"all", P("boolean", "Remove all breakpoints")}})));
    t.append(tool("bk_breakpoints", "List active breakpoints.", schema({})));
    t.append(tool("bk_key", "Press a BK-0010 key. Prefer `key` with a NAME (enter, space, left, right, "
                  "up, down, backspace, delete, f1..f6, рус, лат) or a single character — including "
                  "Cyrillic, for which the РУС/ЛАТ switch is inserted automatically. "
                  "Games that poll the physical key-held bit (0177716, e.g. Digger's movement) only "
                  "register input WHILE HELD: use frames=N (press, run N frames, release) to drive "
                  "them in one call, or hold=true plus a separate bk_run.",
        schema({{"key", P("string", "Key name or a single character — the recommended form. "
                                  "Special: \"стоп\"/\"stop\" is the BK STOP key — not a KOI-7 code but a "
                                  "non-maskable interrupt through vector 4 (same as HALT/bus hang)")},
                {"code", P("string", "Raw KOI-7 code. QUOTE octal values (\"012\"=Enter, \"040\"=Space, "
                                     "\"031\"=right, \"010\"=left, \"032\"=up, \"033\"=down); a bare "
                                     "JSON integer is DECIMAL (12 means 000014 = СБР, not Enter)")},
                {"hold", P("boolean", "Keep the key physically down afterwards (default false)")},
                {"frames", P("integer", "Hold and run this many 50 Hz frames, then release (default 0 = do not run)")},
                {"release_frames", P("integer", "Extra frames to run after releasing (default 0)")}}, {})));
    t.append(tool("bk_joystick", "Drive the joystick on the parallel port 0177714 (active high). "
                  "Buttons are named per the selected layout, which is STICKY (set it once). "
                  "Layouts: standard (Джойвокс/gid, default) | breakhouse | swcorp | klad2. "
                  "The value stays set until changed (like a physically held stick), because games "
                  "sample the port once per THEIR frame, which is longer than a 50 Hz frame — use "
                  "frames=N to hold it for N frames and auto-release, or release=true to clear it.",
        schema({{"layout", P("string", "standard|breakhouse|swcorp|klad2 — sticky for later calls")},
                {"buttons", P("string", "e.g. \"up+fire1\", \"вправо\", or bit0..bit15 for raw bits "
                                        "(also accepts an array of names)")},
                {"bits", P("string", "Raw port value, OR-ed with `buttons` (quote octal: \"02000\")")},
                {"add", P("boolean", "OR into the current port value instead of replacing it")},
                {"release", P("boolean", "Clear the port to 0")},
                {"frames", P("integer", "Run this many frames with the value applied (default 0 = just set it)")},
                {"hold", P("boolean", "Keep the value after `frames` (default false = auto-release)")}})));
    t.append(tool("bk_joy_probe", "Find out which 0177714 bits a game reacts to (i.e. its joystick "
                  "layout) empirically: for each bit — checkpoint, set the bit, run, diff RAM against "
                  "a no-input reference run, roll back. A small ± delta is usually horizontal "
                  "movement, a large one vertical (one screen line). The machine state is restored.",
        schema({{"bits", P("integer", "How many low bits to probe (default 8, max 16)")},
                {"frames", P("integer", "Frames to run per bit (default 10)")},
                {"start", addrArg}, {"end", addrArg},
                {"max", P("integer", "Max changed cells to list per bit (default 8)")}})));
    t.append(tool("bk_screenshot", "Render the BK screen and return it as an inline PNG image (so you "
                  "can see it directly); optionally run frames first and/or write it to a file.",
        schema({{"path", P("string", "Optional output PNG path")},
                {"frames", P("integer", "Run this many 50 Hz frames before the shot (default 0)")},
                {"mono", P("boolean", "512x256 monochrome (default false=colour)")}})));
    t.append(tool("bk_audio", "Capture the speaker (piezo, 0177716 bit 6) as 44100 Hz mono PCM and "
                  "report peak/RMS, an ASCII envelope and the TONE breakdown (frequency + duration "
                  "per segment, derived from the speaker edges — exact, not guessed from the PCM). "
                  "Returns the WAV inline as audio content when the client protocol supports it; "
                  "`path` also writes it to disk, `raw_ms` dumps raw int16 samples.",
        schema({{"path", P("string", "Optional output WAV path")},
                {"frames", P("integer", "50 Hz frames to run while capturing (default 100 = 2 s)")},
                {"run", P("boolean", "false = do not run, just drain what is already buffered (~250 ms)")},
                {"input", P("array", "Input timeline while capturing, same format as bk_run")},
                {"raw_start_ms", P("integer", "Start of the raw-sample dump window")},
                {"raw_ms", P("integer", "Length of the raw-sample dump window in ms (max 200)")},
                {"inline", P("boolean", "Force the inline audio block on/off (default: auto by protocol version)")}})));
    t.append(tool("bk_state_save", "Save full emulator state (RAM, CPU, devices, input) to a file, or "
                  "to a named in-memory slot for quick A/B checkpoints.",
        schema({{"path", P("string", "Path")}, {"slot", P("string", "In-memory slot name (instead of path)")}})));
    t.append(tool("bk_state_load", "Restore full emulator state from a file or an in-memory slot.",
        schema({{"path", P("string", "Path")}, {"slot", P("string", "In-memory slot name (instead of path)")}})));
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
    t.append(tool("bk_type", "Type a string through the keyboard (a few frames per character, like "
                  "real typing), holding each key down so games that poll 0177716 see it. Cyrillic "
                  "works — the РУС/ЛАТ switch codes are inserted automatically. Newline = Enter.",
        schema({{"text", P("string", "String to type")},
                {"hold_frames", P("integer", "Frames each key stays held (default 2)")},
                {"max_frames", P("integer", "Frame budget (default 600)")}}, {"text"})));
    t.append(tool("bk_callers", "List subroutines that call a function (from the recorded call edges), "
                  "with call counts.", schema({{"addr", addrArg}}, {"addr"})));
    t.append(tool("bk_callees", "List subroutines called from within a function, with call counts.",
        schema({{"addr", addrArg}}, {"addr"})));
    t.append(tool("bk_frames", "Report game-frame timing from the programmable timer: frame count, "
                  "average/min/max duration (ms), frame rate and jitter.", schema({})));
    t.append(tool("bk_coverage", "Code-coverage summary from the trace: how many instructions ran, "
                  "and the largest un-executed gaps in a range (find dead / untested code).",
        schema({{"start", addrArg}, {"end", addrArg}, {"gaps", P("integer", "How many gaps to list (default 12)")}})));
    t.append(tool("bk_profile", "Export the call profile as Brendan-Gregg folded stacks to a file "
                  "(open in speedscope / flamegraph.pl). Weight = self CPU ticks.",
        schema({{"path", P("string", "Output .folded path")}}, {"path"})));
    t.append(tool("bk_vram", "Render the screen as ASCII art so you can 'see' it without a PNG. "
                  "mode=ascii (default) downsamples the whole screen by luminance and reports the "
                  "non-black pixel count / bounding box. mode=index prints the exact 2-bit palette "
                  "index per BK pixel from video RAM for a x/y/w/h window — use it to read a sprite "
                  "bitmap precisely (no scroll, no pixel doubling).",
        schema({{"mode", P("string", "ascii|index (default ascii)")},
                {"width", P("integer", "ascii mode: output width in characters (default 64)")},
                {"x", P("integer", "index mode: left BK pixel column (default 0)")},
                {"y", P("integer", "index mode: top line (default 0)")},
                {"w", P("integer", "index mode: width in BK pixels (default 64)")},
                {"h", P("integer", "index mode: height in lines (default 32)")},
                {"mono", P("boolean", "Interpret as 512x256 mono (default false=colour)")}})));
    t.append(tool("bk_ocr", "READ THE TEXT off the BK screen. Splits the screen into character "
                  "cells and matches each against the monitor ROM character generator, so menus, "
                  "prompts and score lines come back as real UTF-8 text (Cyrillic included) "
                  "instead of ASCII art. Handles both BK text modes — narrow (64 chars/line, 8x10 "
                  "cell) and wide (32 chars/line, 16x10 cell, where each glyph bit is doubled; "
                  "this is also the colour mode) — and auto-detects the mode and the vertical "
                  "offset of the text grid. A game with its OWN font: point font_addr at its glyph "
                  "table. Unusual cell widths: set cell_w / glyph_w / cell_h.",
        schema({{"mode", P("string", "auto (default) | narrow (64 cols) | wide (32 cols)")},
                {"frames", P("integer", "Run this many 50 Hz frames before reading (default 0)")},
                {"x", P("integer", "Left edge of the character grid in screen pixels (default 0)")},
                {"y", P("integer", "Top edge in scanlines; omit to auto-detect (monitor text starts "
                                   "at 16, below the 16-line service row)")},
                {"cols", P("integer", "Grid width in cells (default: as many as fit)")},
                {"rows", P("integer", "Grid height in cells (default: as many as fit)")},
                {"cell_w", P("integer", "Horizontal cell pitch in pixels (default 8 narrow / 16 wide)")},
                {"cell_h", P("integer", "Cell height in scanlines (default 10)")},
                {"glyph_w", P("integer", "How many glyph bits to compare (default 8)")},
                {"tolerance", P("integer", "Max mismatching pixels per cell (default 6; 0 = exact)")},
                {"inverse", P("boolean", "Also recognise inverted cells, e.g. the cursor (default true)")},
                {"font_addr", P("string", "Custom glyph table address (default 0112036 = monitor ROM)")},
                {"font_base", P("string", "Code of the first glyph in a custom table (default 020)")},
                {"font_count", P("integer", "Glyph count in a custom table")},
                {"codes", P("boolean", "Also dump the octal screen code of every cell")}})));
    t.append(tool("bk_emt_log", "List intercepted EMT 36 tape/disk file operations (which files the "
                  "game loaded/saved, load address, length, and result). Useful for multi-part loaders.",
        schema({{"count", P("integer", "How many recent ops to show (default 40)")}})));
    t.append(tool("bk_xrefs", "Cross-references from the recorded control-flow edges: who calls/jumps/"
                  "branches TO an address, and where it goes FROM there, with counts.",
        schema({{"addr", addrArg}}, {"addr"})));
    t.append(tool("bk_io_state", "Decode the current I/O-register state (keyboard, scroll, timer, "
                  "joystick port decoded under the active layout, system/speaker) — a hardware snapshot.",
        schema({})));
    t.append(tool("bk_io_log", "Log accesses to the I/O registers (0177600..0177776) with the PC of "
                  "the accessing instruction. enable=true starts capture (clears first), enable=false "
                  "stops; call with no args to dump. Polling the joystick or the keyboard is a READ, "
                  "so set reads=true — and pair it with addr, because the monitor polls the keyboard "
                  "continuously and would flood the buffer. "
                  "E.g. {\"enable\":true,\"reads\":true,\"addr\":\"0177714\"} finds the code that reads the joystick.",
        schema({{"enable", P("boolean", "Start (true) / stop (false) capture")},
                {"reads", P("boolean", "Also log reads (default false = writes only)")},
                {"addr", P("string", "Only log this register (recommended with reads=true)")},
                {"cap", P("integer", "Ring-buffer size (default 2048)")},
                {"count", P("integer", "How many recent entries to dump (default 60)")}})));
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
// ввод и прогон кадров (общая машинка для bk_run / bk_key / bk_joystick / …)
// ---------------------------------------------------------------------------
McpServer::RunOutcome McpServer::runFrames(int maxFrames, const std::vector<InputStep>& script,
                                           const std::function<void()>& afterFrame) {
    RunOutcome out;
    if (maxFrames < 0) maxFrames = 0;
    size_t si = 0;
    while (out.frames < maxFrames) {
        // Применить шаги сценария, назначенные на этот кадр (сценарий отсортирован).
        while (si < script.size() && script[si].frame <= out.frames) {
            const InputStep& s = script[si++];
            if (s.hasJoy)     board_.setJoystick(s.joy);
            if (s.hasKey)   { board_.pressKey(s.key); board_.setKeyHeld(true); }
            if (s.releaseKey) board_.setKeyHeld(false);
        }
        board_.runFrame();
        ++out.frames;
        if (afterFrame) afterFrame();
        if (board_.breakHit()) {
            if (board_.watchHit()) {
                out.reason = "watchpoint";
                out.extra = QString("\nWatch %1 %2 by PC=%3  %4")
                    .arg(oct6(board_.watchAddr())).arg(board_.watchWrite() ? "WRITE" : "READ")
                    .arg(oct6(board_.watchPc()))
                    .arg(QString::fromStdString(bk::disasm(board_.memory(), board_.watchPc()).text));
            } else out.reason = "breakpoint";
            board_.clearBreakHit();
            break;
        }
        if (board_.cpu().halted()) { out.reason = "halted"; break; }
    }
    return out;
}

QString McpServer::runText(const RunOutcome& r) {
    return QString("Stopped (%1) after %2 frames.%3").arg(r.reason).arg(r.frames).arg(r.extra);
}

// Разложить меандр динамика на сегменты постоянной высоты. Считаем период по
// каждой ПАРЕ фронтов (t[i+2]-t[i]) — это устойчиво к неравной скважности; соседние
// периоды в пределах ±12% сливаются в один тон. Пауза длиннее 30 мс — тишина.
QString McpServer::toneReport(uint64_t tick0) const {
    const auto& log = board_.speakerLog();
    std::vector<uint64_t> t;
    for (const auto& e : log)
        if (e.tick >= tick0) t.push_back(e.tick);
    if (t.size() < 3)
        return QString("тоны: фронтов динамика %1 — звук не формировался%2")
            .arg(t.size())
            .arg(board_.speakerLogOn() ? QString() : QString(" (лог фронтов выключен)"));

    const double tickHz = 3.0e6;                       // такт ЦП, Гц
    const uint64_t silenceTicks = (uint64_t)(0.030 * tickHz);
    struct Seg { uint64_t start, end; double freqSum; int n; bool silence; };
    std::vector<Seg> segs;
    auto push = [&](uint64_t a, uint64_t b, double f, bool sil) {
        if (!segs.empty() && segs.back().silence == sil &&
            (sil || std::abs(segs.back().freqSum / segs.back().n - f) <=
                        0.12 * (segs.back().freqSum / segs.back().n))) {
            segs.back().end = b;
            segs.back().freqSum += f;
            ++segs.back().n;
            return;
        }
        segs.push_back({a, b, f, 1, sil});
    };
    for (size_t i = 0; i + 2 < t.size(); ++i) {
        const uint64_t gap1 = t[i + 1] - t[i], gap2 = t[i + 2] - t[i + 1];
        if (gap1 > silenceTicks) { push(t[i], t[i + 1], 0, true); continue; }
        // Пауза во второй половине: период через неё считать нельзя — она станет
        // сегментом тишины на следующей итерации.
        if (gap2 > silenceTicks) continue;
        const uint64_t period = gap1 + gap2;
        if (!period) continue;
        push(t[i], t[i + 2], tickHz / (double)period, false);
    }

    QString out = "тоны (по фронтам 0177716, бит 6):\n";
    int shown = 0;
    for (const Seg& s : segs) {
        const double durMs = (s.end - s.start) * 1000.0 / tickHz;
        if (durMs < 8.0) continue;                     // мелкие огрызки не показываем
        if (++shown > 40) { out += "  … (список обрезан)\n"; break; }
        const double at = (s.start - tick0) * 1000.0 / tickHz;
        if (s.silence) out += QString("  %1 мс: тишина %2 мс\n").arg(at, 8, 'f', 1).arg(durMs, 0, 'f', 1);
        else out += QString("  %1 мс: %2 Гц, %3 мс\n")
                        .arg(at, 8, 'f', 1).arg(s.freqSum / s.n, 0, 'f', 0).arg(durMs, 0, 'f', 1);
    }
    if (!shown) out += "  (сегментов длиннее 8 мс нет — короткие щелчки/шум)\n";
    return out;
}

// Разобрать key/code в последовательность кодов КОИ-7. Имя клавиши («enter»,
// «вправо», «рус») ищется в таблице ядра; иначе строка кодируется как текст, и
// тогда перед кириллицей/латиницей может появиться префикс РУС/ЛАТ (016/017).
bool McpServer::resolveKey(const QJsonObject& args, std::vector<uint16_t>& codes,
                           bool& present, QString& warn, QString& err) {
    codes.clear();
    present = false;
    if (args.contains("key")) {
        const QString ks = args.value("key").toString();
        if (ks.isEmpty()) { err = "пустое имя клавиши"; return false; }
        const uint16_t c = bk::bkKeyByName(ks.toStdString());
        if (c != bk::BK_KEY_NONE) {
            codes.push_back(c);
        } else if (ks.size() == 1) {
            // Одиночный символ: кодируем как текст (для кириллицы добавится РУС/ЛАТ).
            codes = bk::bkEncodeText(ks.toStdString(), cyrState_);
            if (codes.empty()) { err = "непредставимый символ: '" + ks + "'"; return false; }
        } else {
            err = "неизвестная клавиша '" + ks + "' (имя из списка: enter, space, left, right, "
                  "up, down, backspace, delete, home, tab, f1..f6, повт, кт, рус, лат — "
                  "либо ОДИН символ; для строки используйте bk_type)";
            return false;
        }
        present = true;
        return true;
    }
    if (args.contains("code")) {
        long n;
        if (!parseNumber(args.value("code"), n)) { err = "плохое значение code"; return false; }
        // Голое JSON-число — ДЕСЯТИЧНОЕ. Коды БК принято писать восьмерично, так что
        // {"code":12} даёт 000014 (СБР), а не 000012 (ВВОД) — предупреждаем.
        if (args.value("code").isDouble() && n > 7) {
            const QString dec = QString::number(n);
            bool allOctDigits = true;
            for (QChar ch : dec) if (ch < '0' || ch > '7') { allOctDigits = false; break; }
            if (allOctDigits) {
                bool ok = false;
                const long asOct = dec.toLong(&ok, 8);
                if (ok && asOct != n)
                    warn = QString("\nвнимание: code=%1 разобран как ДЕСЯТИЧНОЕ (=%2). "
                                   "Если нужен код КОИ-7 %3 — передайте строку \"0%3\".")
                               .arg(dec).arg(oct6((uint16_t)n)).arg(dec);
            }
        }
        codes.push_back((uint16_t)(n & 0xFFFF));
        present = true;
        return true;
    }
    return true;   // ни key, ни code — это не ошибка (отпускание клавиши)
}

// Разобрать buttons/bits/add/release в значение параллельного порта 0177714.
bool McpServer::resolveJoy(const QJsonObject& args, uint16_t& value, bool& present, QString& err) {
    present = false;
    value = 0;
    if (args.value("release").toBool(false)) { present = true; return true; }
    QString spec;
    const QJsonValue bv = args.value("buttons");
    if (bv.isArray()) {
        QStringList parts;
        for (const auto& e : bv.toArray()) parts << e.toString();
        spec = parts.join('+');
    } else if (bv.isString()) {
        spec = bv.toString();
    }
    if (!spec.trimmed().isEmpty()) {
        std::string e2;
        const uint16_t m = bk::joyMask(joyStd_, spec.toStdString(), &e2);
        if (!e2.empty()) {
            err = QString::fromStdString(e2) +
                  " (допустимо: up/down/left/right, fire1..fire5, button5, bit0..bit15, "
                  "и русские варианты: вверх/вниз/влево/вправо/огонь)";
            return false;
        }
        value |= m;
        present = true;
    }
    if (args.contains("bits")) {
        long n;
        if (!parseNumber(args.value("bits"), n)) { err = "плохое значение bits"; return false; }
        value |= (uint16_t)(n & 0xFFFF);
        present = true;
    }
    if (present && args.value("add").toBool(false)) value |= board_.joystick();
    return true;
}

// Сценарий ввода для bk_run: массив {frame, key|code, release_key, joy|joy_bits, release_joy}.
bool McpServer::parseInputScript(const QJsonArray& arr, std::vector<InputStep>& out, QString& err) {
    out.clear();
    for (const auto& e : arr) {
        if (!e.isObject()) { err = "элемент input должен быть объектом"; return false; }
        const QJsonObject o = e.toObject();
        InputStep s;
        s.frame = o.value("frame").toInt(0);
        if (s.frame < 0) s.frame = 0;

        std::vector<uint16_t> codes;
        bool hasKey = false;
        QString warn;
        if (!resolveKey(o, codes, hasKey, warn, err)) return false;
        if (hasKey && !codes.empty()) {
            // Префиксы РУС/ЛАТ (если есть) подаются отдельными шагами: регистр кода
            // хранит ровно один код, поэтому между ними нужен кадр.
            for (size_t i = 0; i + 1 < codes.size(); ++i) {
                InputStep p;
                p.frame = s.frame + (int)i;
                p.hasKey = true;
                p.key = codes[i];
                out.push_back(p);
            }
            s.frame += (int)codes.size() - 1;
            s.hasKey = true;
            s.key = codes.back();
        }
        if (o.value("release_key").toBool(false)) s.releaseKey = true;

        if (o.value("release_joy").toBool(false)) { s.hasJoy = true; s.joy = 0; }
        else if (o.contains("joy") || o.contains("joy_bits")) {
            QJsonObject jo;
            if (o.contains("joy")) jo["buttons"] = o.value("joy");
            if (o.contains("joy_bits")) jo["bits"] = o.value("joy_bits");
            uint16_t v = 0; bool have = false;
            if (!resolveJoy(jo, v, have, err)) return false;
            s.hasJoy = true;   // "none"/пусто -> 0 (отпустить)
            s.joy = v;
        }
        out.push_back(s);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const InputStep& a, const InputStep& b) { return a.frame < b.frame; });
    return true;
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
        if (args.value("reset").toBool(false)) {
            board_.reset();          // заодно сбрасывает трассу: данные двух игр не смешиваются
            cyrState_ = false;
        }
        board_.ensureMonitorBooted();
        uint16_t a = 0, l = 0;
        bool run = args.value("run").toBool(true);
        if (!board_.loadBin(args.value("path").toString().toStdString(), run, &a, &l))
            return fail("cannot load " + args.value("path").toString());
        lastBin_ = args.value("path").toString();
        const uint16_t pc = board_.cpu().pc();
        QString entry;
        if (run) {
            entry = QString(" Точка входа %1").arg(oct6(pc));
            // Для образов ниже 01000 точка входа выбирается эвристикой автостарта
            // (заполнение стека) и может не совпадать с адресом загрузки.
            entry += (pc == a) ? QString(".") : QString(" (автостарт; адрес загрузки %1).").arg(oct6(a));
        }
        RunOutcome r;
        const int frames = args.value("frames").toInt(0);
        if (frames > 0) r = runFrames(frames);
        return textContent(QString("Loaded %1: addr=%2 len=%3 (octal).%4%5\n%6")
            .arg(lastBin_).arg(oct6(a)).arg(oct6(l)).arg(entry)
            .arg(frames > 0 ? "\n" + runText(r) : QString())
            .arg(regsText()));
    }
    if (name == "bk_reset") {
        board_.reset();
        cyrState_ = false;
        return textContent("Reset.\n" + regsText());
    }
    if (name == "bk_run") {
        const int maxFrames = args.value("max_frames").toInt(200);
        std::vector<InputStep> script;
        QString err;
        if (args.contains("input") && !parseInputScript(args.value("input").toArray(), script, err))
            return fail(err);
        const RunOutcome r = runFrames(maxFrames, script);
        return textContent(QString("%1\n%2\n%3")
            .arg(runText(r)).arg(regsText()).arg(disasmText(board_.cpu().pc(), 3)));
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
        // Страницу В-В пишем ЧЕРЕЗ ШИНУ, чтобы устройство увидело запись (иначе
        // правка 0177712 не перезапустила бы таймер, а 0177664 — не сдвинула экран).
        // Обычную память — poke, минуя защиту ПЗУ: отладчику нужно уметь пропатчить
        // и постоянную память. Явный флаг bus перекрывает это правило.
        const bool viaBus = args.contains("bus") ? args.value("bus").toBool()
                                                 : (a >= bk::ADDR_IO_PAGE);
        int n = 0;
        if (args.contains("words")) {
            QJsonArray w = args.value("words").toArray();
            for (const auto& v : w) {
                long x; parseNumber(v, x);
                const uint16_t ad = (uint16_t)(a + n * 2), val = (uint16_t)(x & 0xFFFF);
                if (viaBus) board_.memory().writeWord(ad, val); else board_.memory().pokeWord(ad, val);
                ++n;
            }
            return textContent(QString("Wrote %1 words at %2 (%3).").arg(n).arg(oct6(a))
                                   .arg(viaBus ? "через шину" : "напрямую в память"));
        }
        if (args.contains("bytes")) {
            QJsonArray b = args.value("bytes").toArray();
            for (const auto& v : b) {
                long x; parseNumber(v, x);
                const uint16_t ad = (uint16_t)(a + n);
                if (viaBus) board_.memory().writeByte(ad, (uint8_t)(x & 0xFF));
                else        board_.memory().pokeByte(ad, (uint8_t)(x & 0xFF));
                ++n;
            }
            return textContent(QString("Wrote %1 bytes at %2 (%3).").arg(n).arg(oct6(a))
                                   .arg(viaBus ? "через шину" : "напрямую в память"));
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
        QString when = args.value("when").toString().trimmed();
        if (!when.isEmpty()) {
            bk::Board::BreakCond c{};
            static const char* ops[] = {">=", "<=", "==", "!=", ">", "<"};
            static const uint8_t opc[] = {4, 5, 0, 1, 3, 2};
            int oi = -1, pos = -1;
            for (int k = 0; k < 6; ++k) { int p = when.indexOf(ops[k]); if (p >= 0) { oi = k; pos = p; break; } }
            if (oi < 0) return fail("condition needs one of == != < > >= <=");
            c.op = opc[oi];
            QString lhs = when.left(pos).trimmed();
            QString rhs = when.mid(pos + (int)QString(ops[oi]).size()).trimmed();
            long val; if (!parseNumber(QJsonValue(rhs), val)) return fail("bad value in condition");
            c.val = (uint16_t)(val & 0xFFFF);
            if (lhs.size() == 2 && (lhs[0] == 'R' || lhs[0] == 'r') && lhs[1].isDigit()) {
                c.kind = 0; c.a = lhs[1].digitValue();
            } else if (lhs.startsWith('@')) {
                bool byte = lhs.endsWith(".b");
                QString as = lhs.mid(1, lhs.size() - 1 - (byte ? 2 : 0)).trimmed();
                c.kind = byte ? 2 : 1;
                long n;
                if (parseNumber(QJsonValue(as), n)) c.a = (uint16_t)(n & 0xFFFF);
                else { auto it = symAddr_.find(as.toStdString()); if (it == symAddr_.end()) return fail("bad address in condition"); c.a = it->second; }
            } else return fail("condition LHS must be Rn or @addr");
            board_.setBreakCond(a, c);
        }
        return textContent(QString("Breakpoint set at %1%2.").arg(oct6(a))
            .arg(when.isEmpty() ? QString() : QString(" when %1").arg(when)));
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
        // Защёлкнуть код в 0177662 и выставить «клавиша физически нажата»
        // (0177716, бит 6, активный низкий). Игры, опрашивающие этот бит (Digger,
        // PITON), реагируют только пока клавиша удерживается — поэтому frames>0
        // (нажать / прогнать / отпустить) или hold=true + отдельный bk_run.
        // «СТОП» — не код КОИ-7: клавиша вынесена из матрицы и вызывает
        // внеприоритетное прерывание по вектору 4. Обрабатываем отдельно.
        {
            const std::string kn = bk::normName(args.value("key").toString().toStdString());
            if (kn == "stop" || kn == "стоп") {
                board_.pressStop();
                const int fr = args.value("frames").toInt(0);
                QString tail;
                if (fr > 0) tail = "\n" + runText(runFrames(fr)) + "\n" + regsText();
                return textContent("Клавиша СТОП: внеприоритетное прерывание по вектору 4." + tail);
            }
        }
        std::vector<uint16_t> codes;
        bool present = false;
        QString warn, err;
        if (!resolveKey(args, codes, present, warn, err)) return fail(err);
        const bool hold = args.value("hold").toBool(false);
        const int frames = args.value("frames").toInt(0);
        const int relFrames = args.value("release_frames").toInt(0);

        QString pre;
        if (present && codes.size() > 1) {
            // Префикс РУС/ЛАТ: регистр кода хранит один код, между ними нужен кадр.
            for (size_t i = 0; i + 1 < codes.size(); ++i) {
                board_.pressKey(codes[i]);
                board_.setKeyHeld(true);
                runFrames(1);
                board_.setKeyHeld(false);
            }
            pre = QString(" (с префиксом %1)").arg(oct6(codes.front()));
        }
        const uint16_t code = present ? codes.back() : 0;
        if (present) board_.pressKey(code);

        QString msg, tail;
        if (frames > 0) {
            board_.setKeyHeld(true);
            const RunOutcome r = runFrames(frames);
            tail = "\n" + runText(r);
            if (!hold) {
                board_.setKeyHeld(false);
                if (relFrames > 0) tail += "\n" + runText(runFrames(relFrames));
            }
            msg = present
                ? QString("Клавиша %1%2%3 удержана %4 кадр(ов)%5.")
                      .arg(oct6(code)).arg(keyLabel(code)).arg(pre).arg(frames)
                      .arg(hold ? ", осталась нажатой" : " и отпущена")
                : QString("Прогнано %1 кадр(ов) без нажатия.").arg(frames);
            tail += "\n" + regsText();
        } else {
            board_.setKeyHeld(hold);
            msg = present
                ? QString("Клавиша %1%2%3 %4.").arg(oct6(code)).arg(keyLabel(code)).arg(pre)
                      .arg(hold ? "нажата и удерживается" : "нажата (тап)")
                : (hold ? QString("Клавиша удерживается.") : QString("Клавиша отпущена."));
        }
        return textContent(msg + warn + tail);
    }
    if (name == "bk_joystick") {
        // Джойстик на параллельном порту 0177714 (нажато = 1). Раскладка липкая:
        // задал один раз — действует на все последующие вызовы и на bk_io_state.
        if (args.contains("layout")) {
            bk::JoyStandard s;
            if (!bk::joyParseStandard(args.value("layout").toString().toStdString(), s))
                return fail("неизвестная раскладка '" + args.value("layout").toString() +
                            "' (standard | breakhouse | swcorp | klad2)");
            joyStd_ = s;
        }
        uint16_t v = 0;
        bool present = false;
        QString err;
        if (!resolveJoy(args, v, present, err)) return fail(err);
        if (present) board_.setJoystick(v);
        else v = board_.joystick();

        const int frames = args.value("frames").toInt(0);
        const bool hold = args.value("hold").toBool(false);
        QString tail;
        if (frames > 0) {
            const RunOutcome r = runFrames(frames);
            tail = "\n" + runText(r);
            if (!hold) { board_.setJoystick(0); tail += "\n0177714 сброшен в 000000."; }
            tail += "\n" + regsText();
        }
        return textContent(QString("0177714 = %1 [%2]  раскладка=%3 (%4)%5")
            .arg(oct6(v)).arg(QString::fromStdString(bk::joyDecode(joyStd_, v)))
            .arg(bk::joyStandardName(joyStd_)).arg(QString::fromUtf8(bk::joyStandardTitle(joyStd_)))
            .arg(tail));
    }
    if (name == "bk_screenshot") {
        if (args.value("frames").toInt(0) > 0) runFrames(args.value("frames").toInt(0));
        board_.screen().setColorMode(!args.value("mono").toBool(false));
        board_.screen().render(board_.memory());
        QImage img = QImage(reinterpret_cast<const uchar*>(board_.screen().pixels()),
                            bk::Screen::TEX_W, bk::Screen::TEX_H, QImage::Format_ARGB32).copy();
        QString path = args.value("path").toString(), note = "BK screen";
        if (!path.isEmpty()) { if (!img.save(path)) return fail("cannot write " + path); note += " (also written to " + path + ")"; }
        QByteArray png; QBuffer buf(&png); buf.open(QIODevice::WriteOnly); img.save(&buf, "PNG"); buf.close();
        QJsonObject txt{{"type", "text"}, {"text", note}};
        QJsonObject im{{"type", "image"}, {"data", QString::fromLatin1(png.toBase64())}, {"mimeType", "image/png"}};
        return QJsonObject{{"content", QJsonArray{txt, im}}, {"isError", false}};
    }
    if (name == "bk_step_out") {
        size_t d = board_.trace().stackDepth();
        if (d <= 1) { board_.stepInstruction(); return textContent("At top level; stepped one instruction.\n" + regsText() + "\n" + disasmText(board_.cpu().pc(), 3)); }
        bool ret = board_.runUntilReturn(d, args.value("max_ticks").toInt(20000000));
        QString extra;
        if (board_.breakHit()) { extra = board_.watchHit() ? "\n(stopped on a watchpoint)" : "\n(stopped on a breakpoint)"; board_.clearBreakHit(); }
        return textContent(QString("%1%2\n%3\n%4")
            .arg(ret ? "Returned to caller." : "Did not return within the tick limit.").arg(extra)
            .arg(regsText()).arg(disasmText(board_.cpu().pc(), 3)));
    }
    if (name == "bk_audio") {
        // Прогнать кадры, захватывая выход пьезодинамика (0177716, бит 6) как PCM.
        // Отдаём: разбор (пик/RMS/огибающая/тоны), опционально WAV-файл и — если
        // клиент говорит на протоколе 2025-03-26 и новее — inline-блок type:"audio".
        const QString path = args.value("path").toString();
        const bool doRun = args.value("run").toBool(true);
        long frames = 100;
        if (args.contains("frames")) parseNumber(args.value("frames"), frames);
        if (frames < 1) frames = 1;
        const int rate = 44100;
        uint64_t tick0 = board_.totalTicks();

        board_.sound().setEnabled(true);
        std::vector<int16_t> samples;
        int16_t tmp[8192];
        auto drain = [&]() {
            size_t got;
            while ((got = board_.sound().read(tmp, 8192)) > 0)
                samples.insert(samples.end(), tmp, tmp + got);
        };
        RunOutcome r;
        if (doRun) {
            board_.sound().clear();            // отбросить накопленное до захвата
            board_.setSpeakerLog(true);
            board_.clearSpeakerLog();
            std::vector<InputStep> script;
            QString err;
            if (args.contains("input") && !parseInputScript(args.value("input").toArray(), script, err))
                return fail(err);
            // FIFO динамика сливаем после каждого кадра: он ограничен четвертью
            // секунды и иначе терял бы начало записи.
            r = runFrames((int)frames, script, drain);
        } else {
            drain();                            // просто забрать то, что уже в FIFO
            r.reason = "no-run";
            // Буфер — это ПРОШЛОЕ: отматываем начало окна назад на его длительность,
            // иначе разбор на тоны искал бы фронты в ещё не наступивших тактах.
            const uint64_t span = (uint64_t)(samples.size() * 3.0e6 / rate);
            tick0 = (tick0 > span) ? tick0 - span : 0;
        }

        if (!path.isEmpty() && !writeWav(path, samples, rate))
            return fail("cannot write " + path);

        // --- анализ: пик, RMS, доля «не тишины», длительность
        int peak = 0;
        long active = 0;
        double sumsq = 0;
        for (int16_t v : samples) {
            const int a = v < 0 ? -v : v;
            if (a > peak) peak = a;
            if (a > 400) ++active;
            sumsq += double(v) * v;
        }
        const double ms = samples.size() * 1000.0 / rate;
        const int rms = samples.empty() ? 0 : (int)std::sqrt(sumsq / samples.size());

        QString out = QString("Захвачено %1 сэмплов (%2 мс, 44100 Гц моно)%3\n"
                              "пик=%4/32767  RMS=%5  не тишина=%6%7")
            .arg(samples.size()).arg(ms, 0, 'f', 0)
            .arg(path.isEmpty() ? QString() : QString(", записано в " + path))
            .arg(peak).arg(rms)
            .arg(QString::number(samples.empty() ? 0 : active * 100 / (long)samples.size()) + "%")
            .arg(doRun ? "\n" + runText(r) : QString());

        // --- огибающая: RMS по окнам, ASCII-«спарклайн»
        if (!samples.empty()) {
            const int cols = std::min<int>(72, std::max<int>(1, (int)samples.size() / 64));
            const char* ramp = " .:-=+*#%@";
            QString spark;
            for (int c = 0; c < cols; ++c) {
                const size_t a = samples.size() * c / cols, b = samples.size() * (c + 1) / cols;
                double s = 0;
                for (size_t i = a; i < b; ++i) s += double(samples[i]) * samples[i];
                const int lv = (b > a) ? (int)std::sqrt(s / (b - a)) : 0;
                spark += ramp[std::clamp(lv * 9 / 12000, 0, 9)];
            }
            out += QString("\nогибающая (RMS, 0..%1 мс):\n%2\n").arg(ms, 0, 'f', 0).arg(spark);
        }

        // --- тоны: по фронтам динамика (точнее, чем нулевые пересечения PCM)
        out += "\n" + toneReport(tick0);

        // --- сырые сэмплы окном (по запросу)
        if (args.contains("raw_ms") || args.contains("raw_start_ms")) {
            const int startMs = args.value("raw_start_ms").toInt(0);
            const int lenMs = std::clamp(args.value("raw_ms").toInt(5), 1, 200);
            const size_t a = std::min(samples.size(), (size_t)(startMs * rate / 1000));
            const size_t b = std::min(samples.size(), a + (size_t)(lenMs * rate / 1000));
            out += QString("\nсырые сэмплы int16 [%1..%2 мс], %3 шт:\n").arg(startMs).arg(startMs + lenMs).arg(b - a);
            for (size_t i = a; i < b; ++i)
                out += QString::number(samples[i]) + ((i + 1 - a) % 16 == 0 ? "\n" : " ");
            out += "\n";
        }

        // --- inline-аудио (MCP AudioContent появился в протоколе 2025-03-26)
        const bool wantInline = args.contains("inline") ? args.value("inline").toBool()
                                                        : (protoVer_ >= QString("2025-03-26"));
        if (wantInline && !samples.empty()) {
            QByteArray wav;
            writeWavBytes(wav, samples, rate);
            QJsonObject txt{{"type", "text"}, {"text", out}};
            QJsonObject au{{"type", "audio"},
                           {"data", QString::fromLatin1(wav.toBase64())},
                           {"mimeType", "audio/wav"}};
            return QJsonObject{{"content", QJsonArray{txt, au}}, {"isError", false}};
        }
        return textContent(out);
    }
    if (name == "bk_state_save") {
        const QString slot = args.value("slot").toString();
        if (!slot.isEmpty()) {
            board_.saveStateMem(stateSlots_[slot]);
            return textContent(QString("Состояние сохранено в слот '%1' (%2 байт, в памяти).")
                                   .arg(slot).arg(stateSlots_[slot].size()));
        }
        if (!board_.saveState(args.value("path").toString().toStdString())) return fail("save failed");
        return textContent("State saved to " + args.value("path").toString());
    }
    if (name == "bk_state_load") {
        const QString slot = args.value("slot").toString();
        if (!slot.isEmpty()) {
            auto it = stateSlots_.find(slot);
            if (it == stateSlots_.end()) return fail("нет слота '" + slot + "'");
            if (!board_.loadStateMem(it->second)) return fail("слот повреждён");
            return textContent(QString("Состояние восстановлено из слота '%1'.\n%2").arg(slot).arg(regsText()));
        }
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
        // Текст в КОИ-7 (кириллица тоже — с автоматической вставкой РУС/ЛАТ).
        // Клавиша на время символа считается физически нажатой: игры, опрашивающие
        // бит 6 регистра 0177716 (Digger, PITON), иначе ввода не увидят.
        const std::vector<uint16_t> codes =
            bk::bkEncodeText(args.value("text").toString().toStdString(), cyrState_);
        const int budget = args.value("max_frames").toInt(600);
        const int holdFrames = std::max(1, args.value("hold_frames").toInt(2));
        size_t idx = 0;
        int frames = 0;
        while (idx < codes.size() && frames < budget) {
            if (!board_.keyReady()) {
                board_.pressKey(codes[idx++]);
                board_.setKeyHeld(true);
                for (int k = 0; k < holdFrames && frames < budget; ++k) { board_.runFrame(); ++frames; }
                board_.setKeyHeld(false);
            }
            board_.runFrame(); ++frames;
        }
        for (int k = 0; k < 12 && board_.keyReady() && frames < budget; ++k) { board_.runFrame(); ++frames; }
        board_.setKeyHeld(false);
        return textContent(QString("Набрано %1/%2 кодов за %3 кадров (регистр: %4).\n%5")
            .arg(idx).arg(codes.size()).arg(frames).arg(cyrState_ ? "РУС" : "ЛАТ").arg(regsText()));
    }

    if (name == "bk_callers" || name == "bk_callees") {
        uint16_t target; QString err; if (!resolveAddr(args, "addr", target, err)) return fail(err);
        const bk::Memory& mem = board_.memory();
        auto isJsr = [](uint16_t ir) { return ir >= 0004000 && ir <= 0004777; };
        std::set<uint16_t> subs;                         // subroutine entries = JSR targets
        for (auto& e : board_.trace().edges())
            if (isJsr(mem.peekWord(e.first >> 16))) subs.insert(e.first & 0xFFFF);
        auto funcOf = [&](uint16_t x) -> uint16_t { auto it = subs.upper_bound(x); if (it == subs.begin()) return 0; --it; return *it; };
        auto sym = [&](uint16_t x) { auto it = symName_.find(x); return it != symName_.end() ? QString(" <%1>").arg(QString::fromStdString(it->second)) : QString(); };
        std::map<uint16_t, uint32_t> agg;
        for (auto& e : board_.trace().edges()) {
            uint16_t from = e.first >> 16, to = e.first & 0xFFFF;
            if (!isJsr(mem.peekWord(from))) continue;
            if (name == "bk_callers") { if (to == target) agg[funcOf(from)] += e.second; }
            else                      { if (funcOf(from) == target) agg[to] += e.second; }
        }
        std::vector<std::pair<uint32_t, uint16_t>> v;
        for (auto& kv : agg) v.push_back({kv.second, kv.first});
        std::sort(v.rbegin(), v.rend());
        QString out = QString(name == "bk_callers" ? "Callers of %1%2:\n" : "Callees of %1%2:\n").arg(oct6(target)).arg(sym(target));
        for (auto& p : v) out += QString("  %1%2  x%3\n").arg(oct6(p.second)).arg(sym(p.second)).arg(p.first);
        if (v.empty()) out += "  (none recorded — run the game so calls are traced)\n";
        return textContent(out);
    }
    if (name == "bk_frames") {
        const auto& fb = board_.frameBoundaries();
        if (fb.size() < 2) return textContent("Not enough frame boundaries recorded "
            "(the game may pace in free-running mode, or hasn't run yet).");
        uint64_t avg = (fb.back() - fb.front()) / (fb.size() - 1), mn = ~0ull, mx = 0;
        for (size_t i = 1; i < fb.size(); ++i) { uint64_t d = fb[i] - fb[i - 1]; mn = std::min(mn, d); mx = std::max(mx, d); }
        auto ms = [](uint64_t t) { return t / 3000.0; };
        return textContent(QString("Frames: %1  avg %2 ms (%3 fps)  min %4 ms  max %5 ms  jitter %6 ms")
            .arg(fb.size()).arg(ms(avg), 0, 'f', 2).arg(avg ? 3.0e6 / avg : 0, 0, 'f', 1)
            .arg(ms(mn), 0, 'f', 2).arg(ms(mx), 0, 'f', 2).arg(ms(mx - mn), 0, 'f', 2));
    }
    if (name == "bk_coverage") {
        uint16_t start = 0, end = 0177777; QString err;
        if (args.contains("start") && !resolveAddr(args, "start", start, err)) return fail(err);
        if (args.contains("end")   && !resolveAddr(args, "end",   end,   err)) return fail(err);
        int nGaps = args.value("gaps").toInt(12);
        auto& tr = board_.trace();
        int nWords = (end - start) / 2 + 1;
        std::vector<char> covered(nWords, 0);
        uint64_t execInstrs = 0;
        for (uint32_t a = start; a <= end; a += 2) {
            uint32_t c = tr.execCount((uint16_t)a);
            if (!c) continue;
            execInstrs += c;
            int len = bk::disasm(board_.memory(), (uint16_t)a).words;   // cover all instruction words
            for (int w = 0; w < len; ++w) { uint32_t wa = a + w * 2; if (wa >= start && wa <= end) covered[(wa - start) / 2] = 1; }
        }
        int cov = 0; for (char x : covered) cov += x;
        std::vector<std::pair<uint16_t, int>> gaps;
        int gs = -1;
        for (int i = 0; i < nWords; ++i) {
            if (!covered[i]) { if (gs < 0) gs = i; }
            else if (gs >= 0) { gaps.push_back({(uint16_t)(start + gs * 2), i - gs}); gs = -1; }
        }
        if (gs >= 0) gaps.push_back({(uint16_t)(start + gs * 2), nWords - gs});
        std::sort(gaps.begin(), gaps.end(), [](auto& x, auto& y) { return x.second > y.second; });
        QString out = QString("Coverage [%1..%2]: %3/%4 words are code (%5%), %6 instructions executed.\n"
                              "Largest un-executed gaps (data or dead code):\n")
            .arg(oct6(start)).arg(oct6(end)).arg(cov).arg(nWords)
            .arg(nWords ? 100.0 * cov / nWords : 0, 0, 'f', 1).arg(execInstrs);
        for (int i = 0; i < nGaps && i < (int)gaps.size(); ++i)
            out += QString("  %1..%2  (%3 words)\n").arg(oct6(gaps[i].first))
                       .arg(oct6((uint16_t)(gaps[i].first + gaps[i].second * 2 - 2))).arg(gaps[i].second);
        if (gaps.empty()) out += "  (none)\n";
        return textContent(out);
    }
    if (name == "bk_profile") {
        QString path = args.value("path").toString();
        const auto& flame = board_.trace().flame();
        if (flame.size() < 2) return fail("no profile data — the call tree is empty (run the game first)");
        FILE* f = std::fopen(path.toStdString().c_str(), "wb");
        if (!f) return fail("cannot open " + path);
        auto label = [&](uint16_t fn) -> std::string {
            auto it = symName_.find(fn); if (it != symName_.end()) return it->second;
            char b[8]; std::snprintf(b, sizeof(b), "%06o", fn); return b;
        };
        int lines = 0;
        for (size_t i = 1; i < flame.size(); ++i) {
            if (flame[i].self == 0) continue;
            std::vector<uint16_t> path;                  // node -> root
            for (int cur = (int)i; cur > 0; cur = flame[cur].parent) path.push_back(flame[cur].func);
            std::string stack = "root";
            for (auto it = path.rbegin(); it != path.rend(); ++it) { stack += ";"; stack += label(*it); }
            std::fprintf(f, "%s %llu\n", stack.c_str(), (unsigned long long)flame[i].self);
            ++lines;
        }
        std::fclose(f);
        return textContent(QString("Wrote %1 folded-stack lines to %2 (open in speedscope or flamegraph.pl).").arg(lines).arg(path));
    }
    if (name == "bk_vram") {
        const bool mono = args.value("mono").toBool(false);
        if (args.value("mode").toString() == "index") {
            // Точная форма спрайта: читаем ВОЗУ напрямую (0040000, 64 байта на
            // строку) и печатаем индекс палитры 0-3 на пиксель БК. Мимо Screen —
            // значит без скролла и без удвоения пикселей до 512.
            const int maxX = mono ? 512 : 256;
            int x0 = std::clamp(args.value("x").toInt(0), 0, maxX - 1);
            int y0 = std::clamp(args.value("y").toInt(0), 0, 255);
            int w  = std::clamp(args.value("w").toInt(64), 1, maxX - x0);
            int h  = std::clamp(args.value("h").toInt(32), 1, 256 - y0);
            if (w * h > 8192) { h = std::max(1, 8192 / w); }   // не топить ответ
            const uint8_t* vram = board_.memory().videoRam();
            QString grid;
            for (int y = y0; y < y0 + h; ++y) {
                const uint8_t* line = vram + y * 64;
                for (int x = x0; x < x0 + w; ++x) {
                    if (mono) grid += ((line[x >> 3] >> (x & 7)) & 1) ? '1' : '.';
                    else {
                        const int v = (line[x >> 2] >> ((x & 3) * 2)) & 3;
                        grid += v ? QChar('0' + v) : QChar('.');
                    }
                }
                grid += "\n";
            }
            return textContent(QString("ВОЗУ %1, окно x=%2 y=%3 w=%4 h=%5 "
                                       "(индексы палитры 0-3, '.'=0=фон):\n%6")
                .arg(mono ? "моно 512x256" : "цвет 256x256 (на экране пиксели удвоены до 512)")
                .arg(x0).arg(y0).arg(w).arg(h).arg(grid));
        }
        int cols = std::clamp(args.value("width").toInt(64), 16, 160);
        board_.screen().setColorMode(!mono);
        board_.screen().render(board_.memory());
        const uint32_t* px = board_.screen().pixels();
        const int W = bk::Screen::TEX_W, H = bk::Screen::TEX_H;
        int nonblack = 0, minx = W, miny = H, maxx = -1, maxy = -1;
        for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x)
            if (px[y * W + x] & 0xFFFFFF) { ++nonblack; minx = std::min(minx, x); maxx = std::max(maxx, x); miny = std::min(miny, y); maxy = std::max(maxy, y); }
        int rows = std::max(1, cols * H / (W * 2));       // chars are ~2x taller than wide
        const char* ramp = " .:-=+*#%@";
        QString grid;
        for (int cy = 0; cy < rows; ++cy) {
            for (int cx = 0; cx < cols; ++cx) {
                int x0 = cx * W / cols, x1 = (cx + 1) * W / cols, y0 = cy * H / rows, y1 = (cy + 1) * H / rows;
                long sum = 0, cnt = 0;
                for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
                    uint32_t p = px[y * W + x];
                    sum += (((p >> 16) & 255) * 30 + ((p >> 8) & 255) * 59 + (p & 255) * 11) / 100; ++cnt;
                }
                grid += ramp[std::clamp(int(cnt ? sum / cnt : 0) * 9 / 255, 0, 9)];
            }
            grid += "\n";
        }
        QString head = QString("Screen %1x%2, non-black %3 px").arg(W).arg(H).arg(nonblack);
        if (maxx >= 0) head += QString(", bbox (%1,%2)-(%3,%4)").arg(minx).arg(miny).arg(maxx).arg(maxy);
        return textContent(head + ":\n" + grid);
    }

    if (name == "bk_ocr") {
        // Распознать текст по знакоместам: сравнить каждое знакоместо с глифами
        // знакогенератора (по умолчанию — таблица ПЗУ монитора по адресу 0112036).
        if (args.value("frames").toInt(0) > 0) runFrames(args.value("frames").toInt(0));

        std::vector<uint8_t> bits;
        bk::screenBitmap(board_.memory(), board_.peekReg(0177664), bits);

        bk::OcrOptions o;
        QString err;
        const QString mode = args.value("mode").toString("auto").toLower();
        bool fixedMode = false;
        if (mode == "narrow" || mode == "64") { o.wide = false; fixedMode = true; }
        else if (mode == "wide" || mode == "32") { o.wide = true; fixedMode = true; }
        else if (mode != "auto" && !mode.isEmpty()) return fail("mode: auto|narrow|wide");
        if (!fixedMode) {
            // Подсказка от самой машины: ячейка 0162 (DSIMB) — ширина знакоместа в
            // байтах, 1 = 64 символа, 2 = 32. Задаёт стартовый режим, но перебор
            // всё равно проверит оба — игры драйвером монитора не пользуются.
            const uint16_t dsimb = board_.memory().peekWord(0162);
            o.wide = (dsimb == 2);
        }
        const bool fixedY0 = args.contains("y");
        o.x0 = args.value("x").toInt(0);
        o.y0 = args.value("y").toInt(0);
        o.cols = args.value("cols").toInt(0);
        o.rows = args.value("rows").toInt(0);
        o.cellW = args.value("cell_w").toInt(0);
        o.glyphW = std::clamp(args.value("glyph_w").toInt(8), 1, 8);
        o.fontHeight = std::clamp(args.value("cell_h").toInt(bk::BK_FONT_HEIGHT), 1, 16);
        o.tolerance = std::clamp(args.value("tolerance").toInt(6), 0, 80);
        o.allowInverse = args.value("inverse").toBool(true);
        if (args.contains("font_addr")) {
            if (!resolveAddr(args, "font_addr", o.fontAddr, err)) return fail(err);
            o.bkLayout = false;            // своя таблица — без «дыры» 0200..0237
            long fb = 020;
            if (args.contains("font_base")) parseNumber(args.value("font_base"), fb);
            o.fontBase = (uint16_t)(fb & 0377);
            o.fontCount = args.value("font_count").toInt(0);
        }

        const bk::OcrResult r = ocrAuto(board_.memory(), bits, o, fixedMode, fixedY0);
        if (r.rows <= 0 || r.cols <= 0) return fail("пустая сетка знакомест — проверьте x/y/cols/rows");

        QString out = QString("Текст с экрана: режим %1 (%2 симв./строку, знакоместо %3x%4), "
                              "сетка %5x%6 от точки (%7,%8).\n"
                              "Знакомест непустых %9, распознано %10, не опознано %11%12.\n")
            .arg(r.opt.wide ? "широкий" : "узкий")
            .arg(512 / (r.opt.cellW > 0 ? r.opt.cellW : (r.opt.wide ? 16 : 8)))
            .arg(r.opt.cellW > 0 ? r.opt.cellW : (r.opt.wide ? 16 : 8)).arg(r.opt.fontHeight)
            .arg(r.cols).arg(r.rows).arg(r.opt.x0).arg(r.opt.y0)
            .arg(r.nonBlank).arg(r.recognised).arg(r.unknown)
            .arg(r.inverted ? QString(" (инверсных %1)").arg(r.inverted) : QString());
        if (r.nonBlank && r.recognised == 0)
            out += "Ни одно знакоместо не совпало со знакогенератором — вероятно, игра рисует\n"
                   "своим шрифтом: укажите font_addr (и font_base/font_count) либо поднимите tolerance.\n";
        out += "----\n";
        out += QString::fromUtf8(r.text().c_str());
        out += "----";

        if (args.value("codes").toBool(false)) {
            out += "\nВосьмеричные коды по знакоместам (?? — не опознано):\n";
            for (int row = 0; row < r.rows; ++row) {
                QString line = QString("%1:").arg(row, 2);
                for (int col = 0; col < r.cols; ++col) {
                    const bk::OcrCell& c = r.cells[(size_t)row * r.cols + col];
                    line += c.ok ? QString(" %1").arg(oct6(c.code).right(3)) : QString(" ???");
                }
                out += line + "\n";
            }
        }
        return textContent(out);
    }

    if (name == "bk_emt_log") {
        int count = args.value("count").toInt(40);
        const auto& log = board_.emtLog();
        static const char* cmds[] = {"STOP", "START", "WRITE", "READ", "FICT_READ"};
        static const char* resp[] = {"OK", "INCORRECT_NAME", "CRC_ERROR", "STOP"};
        QString out = QString("EMT 36 file operations (%1 recorded):\n").arg(log.size());
        int shown = 0, startIdx = std::max(0, (int)log.size() - count);
        for (int i = startIdx; i < (int)log.size(); ++i, ++shown) {
            const auto& e = log[i];
            out += QString("  %1 '%2'  addr=%3 len=%4  -> %5\n")
                       .arg(e.cmd < 5 ? cmds[e.cmd] : "?", -9)
                       .arg(QString::fromStdString(e.name), -16)
                       .arg(oct6(e.addr)).arg(oct6(e.len))
                       .arg(e.response < 4 ? resp[e.response] : "?");
        }
        if (log.empty()) out += "  (none — no EMT 36 file I/O yet)\n";
        return textContent(out);
    }

    if (name == "bk_xrefs") {
        uint16_t target; QString err; if (!resolveAddr(args, "addr", target, err)) return fail(err);
        const bk::Memory& mem = board_.memory();
        auto sym = [&](uint16_t x) { auto it = symName_.find(x); return it != symName_.end() ? QString(" <%1>").arg(QString::fromStdString(it->second)) : QString(); };
        auto kind = [&](uint16_t ir) -> const char* {
            if (ir >= 0004000 && ir <= 0004777) return "call";
            if (ir >= 0000100 && ir <= 0000177) return "jmp";
            if ((ir >= 0000400 && ir <= 0003477) || (ir >= 0100000 && ir <= 0103777)) return "branch";
            if ((ir >= 0000200 && ir <= 0000207) || ir == 02 || ir == 06) return "return";
            return "flow";
        };
        std::vector<std::tuple<uint32_t, uint16_t, const char*>> to, from;
        for (auto& e : board_.trace().edges()) {
            uint16_t f = e.first >> 16, t2 = e.first & 0xFFFF;
            if (t2 == target) to.push_back({e.second, f, kind(mem.peekWord(f))});
            if (f == target)  from.push_back({e.second, t2, kind(mem.peekWord(f))});
        }
        std::sort(to.rbegin(), to.rend()); std::sort(from.rbegin(), from.rend());
        QString out = QString("Cross-references for %1%2:\n  TO here (callers/jumps):\n").arg(oct6(target)).arg(sym(target));
        for (auto& x : to)   out += QString("    %1%2  %3  x%4\n").arg(oct6(std::get<1>(x))).arg(sym(std::get<1>(x))).arg(std::get<2>(x)).arg(std::get<0>(x));
        if (to.empty()) out += "    (none recorded)\n";
        out += "  FROM here (targets):\n";
        for (auto& x : from) out += QString("    %1%2  %3  x%4\n").arg(oct6(std::get<1>(x))).arg(sym(std::get<1>(x))).arg(std::get<2>(x)).arg(std::get<0>(x));
        if (from.empty()) out += "    (none recorded)\n";
        return textContent(out);
    }
    if (name == "bk_io_state") {
        auto r = [&](uint16_t a) { return board_.peekReg(a); };
        uint16_t csr = r(0177712);
        QString timerBits;
        if (csr & 0020) timerBits += "RUN ";
        if (csr & 0002) timerBits += "CAP(free) ";
        if (csr & 0010) timerBits += "OS(oneshot) ";
        if (csr & 0004) timerBits += "MON ";
        if (csr & 0200) timerBits += "FL(underflow) ";
        uint16_t sys = r(0177716);
        const uint16_t port = r(0177714);
        return textContent(QString(
            "I/O state:\n"
            "  Keyboard  status 0177660=%1  data 0177662=%2  (%3)%4\n"
            "  Scroll    0177664=%5\n"
            "  Timer     limit 0177706=%6  count 0177710=%7  csr 0177712=%8 [%9]\n"
            "  Port      0177714=%10  [%11]  раскладка=%12\n"
            "  System    0177716=%13  (key-held bit6=%14)\n"
            "  Развёртка строка %15/320 %16%17%18")
            .arg(oct6(r(0177660))).arg(oct6(r(0177662))).arg(board_.keyReady() ? "code ready" : "empty")
            .arg(keyLabel(r(0177662)))
            .arg(oct6(r(0177664)))
            .arg(oct6(r(0177706))).arg(oct6(r(0177710))).arg(oct6(csr)).arg(timerBits.trimmed().isEmpty() ? "stopped" : timerBits.trimmed())
            .arg(oct6(port)).arg(QString::fromStdString(bk::joyDecode(joyStd_, port)))
            .arg(bk::joyStandardName(joyStd_))
            .arg(oct6(sys)).arg((sys & 0100) ? "1(up) — отпущена" : "0(down) — УДЕРЖИВАЕТСЯ")
            .arg(board_.vp037().scanline())
            .arg(board_.vp037().vgate() ? "кадровое гашение"
                 : board_.vp037().hgate() ? "строчное гашение" : "видимая часть")
            .arg(board_.vp037().inActiveDisplay() ? " (037 занимает шину: такты ожидания ДОЗУ)"
                                                  : "")
            .arg(board_.smk512()
                 ? QString("\n  СМК-512   режим %1 (код %2), страница %3 (код %4)%5")
                       .arg(QString::fromUtf8(bk::Smk512::modeName(board_.smk().mode())))
                       .arg(oct6(bk::Smk512::modeCode(board_.smk().mode())))
                       .arg(board_.smk().page())
                       .arg(oct6(bk::Smk512::pageCode(board_.smk().page())))
                       .arg(board_.smk().armed() ? ", строб взведён" : "")
                 : QString()));
    }
    if (name == "bk_io_log") {
        if (args.contains("enable")) {
            const bool on = args.value("enable").toBool();
            uint16_t filt = 0;
            QString err;
            if (args.contains("addr") && !resolveAddr(args, "addr", filt, err)) return fail(err);
            const size_t cap = (size_t)std::max(16, args.value("cap").toInt(2048));
            if (on) board_.clearIoLog();
            board_.setIoLog(on, args.value("reads").toBool(false), cap, filt);
        }
        static const std::map<uint16_t, const char*> nm = {
            {0177660, "kbd.status"}, {0177662, "kbd.data"}, {0177664, "scroll"},
            {0177706, "timer.limit"}, {0177710, "timer.count"}, {0177712, "timer.csr"},
            {0177714, "port(джойстик)"}, {0177716, "system/speaker"}};
        const int count = args.value("count").toInt(60);
        const auto& log = board_.ioLog();
        QString out = QString("Обращения к В-В (%1 в буфере; чтения: %2%3):\n")
            .arg(log.size()).arg(board_.ioLogReads() ? "да" : "нет")
            .arg(board_.ioLogFilter() ? QString(", фильтр %1").arg(oct6(board_.ioLogFilter())) : QString());
        const int startIdx = std::max(0, (int)log.size() - count);
        for (int i = startIdx; i < (int)log.size(); ++i) {
            const auto& e = log[i];
            auto it = nm.find(e.addr);
            out += QString("  %1 %2 = %3   %4  PC=%5  %6\n")
                       .arg(e.isRead ? "R" : "W").arg(oct6(e.addr)).arg(oct6(e.value))
                       .arg(it != nm.end() ? it->second : "", -16).arg(oct6(e.pc))
                       .arg(QString::fromStdString(bk::disasm(board_.memory(), e.pc).text));
        }
        if (log.empty())
            out += "  (буфер пуст — включите захват: {\"enable\":true}, при нужде "
                   "{\"reads\":true,\"addr\":\"0177714\"}, затем прогоните и снимите дамп)\n";
        return textContent(out);
    }

    if (name == "bk_joy_probe") {
        // Определить раскладку джойстика неизвестной игры эмпирически: для каждого
        // бита порта — чекпоинт, выставить бит, прогнать, снять ОЗУ, откатиться.
        // Меняющиеся ячейки и знак дельты выдают координату игрока (см. docs/joystick.md).
        const int nbits = std::clamp(args.value("bits").toInt(8), 1, 16);
        const int frames = std::clamp(args.value("frames").toInt(10), 1, 200);
        const int maxCells = std::clamp(args.value("max").toInt(8), 1, 64);
        uint16_t start = 0, end = 0037777;
        QString err;
        if (args.contains("start") && !resolveAddr(args, "start", start, err)) return fail(err);
        if (args.contains("end") && !resolveAddr(args, "end", end, err)) return fail(err);
        if (end <= start) return fail("end должен быть больше start");

        std::vector<uint8_t> checkpoint;
        board_.saveStateMem(checkpoint);
        const uint16_t joySaved = board_.joystick();

        // Опорный прогон без ввода — чтобы отсеять то, что меняется само по себе
        // (таймеры, анимация, счётчики кадров).
        auto snapshot = [&]() {
            std::vector<uint16_t> s;
            for (uint32_t a = start; a + 1 <= end; a += 2) s.push_back(board_.memory().peekWord((uint16_t)a));
            return s;
        };
        board_.setJoystick(0);
        runFrames(frames);
        const std::vector<uint16_t> base = snapshot();
        board_.loadStateMem(checkpoint);

        QString out = QString("Проба джойстика: %1 бит(ов) x %2 кадров, диапазон %3..%4.\n"
                              "Показаны слова, изменившиеся ИНАЧЕ, чем в прогоне без ввода.\n")
            .arg(nbits).arg(frames).arg(oct6(start)).arg(oct6(end));
        for (int b = 0; b < nbits; ++b) {
            const uint16_t bit = (uint16_t)(1u << b);
            board_.loadStateMem(checkpoint);
            board_.setJoystick(bit);
            runFrames(frames);
            const std::vector<uint16_t> cur = snapshot();
            QString line;
            int shown = 0;
            for (size_t i = 0; i < cur.size() && i < base.size(); ++i) {
                if (cur[i] == base[i]) continue;
                if (++shown > maxCells) { line += " …"; break; }
                const uint16_t addr = (uint16_t)(start + i * 2);
                line += QString(" %1:%2%3").arg(oct6(addr))
                            .arg((int16_t)(cur[i] - base[i]) >= 0 ? "+" : "")
                            .arg((int16_t)(cur[i] - base[i]));
            }
            out += QString("  бит%1 (%2):%3\n").arg(b, -2).arg(oct6(bit))
                       .arg(line.isEmpty() ? QString(" — ничего") : line);
        }
        board_.loadStateMem(checkpoint);
        board_.setJoystick(joySaved);
        out += "Состояние машины восстановлено. Малая ±дельта обычно = горизонталь, "
               "большая = вертикаль (шаг строки экрана).";
        return textContent(out);
    }

    return fail("unknown tool: " + name);
}
