# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A BK-0010-01 (Soviet PDP-11 clone, K1801VM1 CPU) emulator-debugger in C++17/Qt6
with OpenGL screen output and a Soft-ICE style debugger. Loads and runs `.BIN`
game files. See `README.md` for the user-facing feature list and hotkeys, and
`docs/BK0010-hardware.md` for the verified hardware reference (memory map, I/O
registers, interrupt vectors, screen encoding, palette, .BIN format, timing).

## Build / test / run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/cpu_tests                      # unit tests (CPU decode, flags, save/restore)
ctest --test-dir build                 # same, via ctest
./build/bk0010-emulator game.bin       # GUI
```

- Qt6 `Core Gui Widgets OpenGLWidgets` are required; `Multimedia` is **optional**
  (guarded by `HAVE_QT_MULTIMEDIA`). It is installed here, so audio is wired:
  `Speaker` (core) generates samples, `AudioOut` (a `QIODevice` feeding a
  `QAudioSink` in pull mode) plays them. Without Multimedia the app still builds;
  audio is simply skipped and `Speaker` is disabled to avoid buffering.
- ROMs (`monit10.rom`, `basic10.rom`) live in `roms/`; the path is baked in via
  `BK_DEFAULT_ROM_DIR`, overridable with `--roms <dir>` or `BK_ROM_DIR`.

### Headless verification (no display needed — use this to check changes)

```sh
QT_QPA_PLATFORM=offscreen ./build/bk0010-emulator --frames 200 --shot out.png game.bin
```

Flags: `--frames N`, `--shot`, `--dbgshot` (Soft-ICE overlay), `--memvis`,
`--hotpath`, `--callgraph`, `--flame`, `--flamechart`, `--hotchart`, `--mono`,
`--no-arb037` (disable КР1801ВП1-037 memory-arbitration wait-states, on by default),
`--smk` / `--no-smk` (СМК-512 memory-expansion board, off by default),
`--scanline` (per-scanline rendering — each line drawn with the scroll register value
that was live when the beam crossed it, driven off the `Vp037` raster; OFF by default,
see `Board::setScanlineRender`), `--key <code>`, `--keyframe N`.
Screenshots render from the CPU-side pixel buffer / `QWidget::grab`, so no GL
context or display is needed.
This is the primary way to verify visual changes; the GUI itself needs `xvfb-run`.

## Architecture (the parts that span multiple files)

Two layers, deliberately decoupled:

- **`src/core/`** — pure C++, no Qt. `Board` owns everything and is the entry point:
  it wires `Cpu` + `Memory` + `Screen` + `Speaker` + `Trace`, implements the
  memory-mapped I/O registers (`IoBus`), drives the frame loop (48.83 Hz — see below), delivers
  interrupts, and does `.BIN` loading and save/restore of state.
- **`src/ui/`** — Qt6. `MainWindow` owns a `Board`, drives it from a 50 Hz `QTimer`
  (single-threaded — emulation runs in the GUI thread; one frame = 61440 ticks is
  fast enough), and hosts `GlScreen` + the debugger widgets. The GUI paces itself by
  REAL time (`QElapsedTimer`), not by timer ticks: a frame is 20.48 ms, which is not
  an integral number of milliseconds.
- **`src/mcp/`** — `McpServer`: a headless MCP server (`--server`) exposing the core
  as ~42 JSON-RPC tools (JSON-RPC 2.0, newline-delimited over stdio, QtCore JSON) —
  run/step/step-over/step-out, regs/mem, break (optionally conditional) / watch
  (data watchpoints), backtrace, xrefs, search/diff memory, type, callers/callees,
  frames, coverage, profile (speedscope folded stacks), vram (ASCII-art screen),
  io-state / io-log, emt-log (EMT 36 file I/O), hotspots, screenshot (inline PNG) /
  audio, state save/load, plus the game-debugging set: `bk_joystick` / `bk_joy_probe`
  (parallel port 0177714), timed input on `bk_key` / `bk_run` (`input` timeline),
  `bk_ocr` (read screen text via the character cells).
  Owns its own `Board`, reuses only the `Board`/`Cpu`/`Memory`/`Screen`/`Trace`
  public API + `bk::disasm` + the shared input tables. Entered at the very top of
  `main()` before any GUI setup; runs under an offscreen `QGuiApplication` so
  `bk_screenshot` can save PNGs. Registered for Claude Code via `.mcp.json` (server
  name `bk0010`). Tool args accept decimal / `0x` hex / leading-0 octal, and symbols
  loaded via `bk_symbols`. Recipes for driving a game: `docs/mcp-debugging.md`.

- **Shared input tables** live in the core as header-only files so the GUI, the MCP
  server and `cpu_tests` (which links only `bkcore`) all agree: `src/core/Joystick.h`
  (the four joystick layouts + name↔bit helpers; `Gamepad::Standard` is just an alias
  for `bk::JoyStandard`) and `src/core/BkKeys.h` (KOI-7 key names, KOI-7 H1 table,
  UTF-8 text → key codes with automatic РУС/ЛАТ). `src/ui/BkKeymap.cpp` stays the
  authority for *Qt key* → code; `BkKeys.h` is the authority for *name/text* → code.

Key cross-cutting facts to know before editing the CPU or screen:

- **CPU dispatch** (`Cpu::buildTable`): a 1024-entry table indexed by `ir >> 6`,
  mirroring the reference `bk` emulator's `itab.c`. Instruction semantics/flags are
  ported 1:1 from that emulator (`/home/prcoder/emulators/БК-0010-01/bk`). If you
  change flag logic, cross-check against `single.c`/`double.c`/`branch.c` there.
- **Effective address** load/store (`Cpu::loadSrc/loadDst/storeDst2/...`) ports
  `ea.c`; `storeDst2` writes back to the cached `eaAddr_` for modify-in-place ops.
- **T-bit trace trap** (`Cpu::step`): while PSW bit 4 (`020`) is set, EVERY instruction
  traps through vector `014`; only the `RTT` instruction itself suppresses it (so `RTI`
  restoring T=1 traps immediately, `RTT` lets exactly one instruction through). This is
  not just a debugger feature — BK programs use it to run music *interleaved* with a
  game one instruction at a time (`tetr-music.bin`). Dropping it makes such programs
  silent AND run at full CPU speed.
- **Timer power-on limit** is `0177777`, not the `011000` that `bk`/BKBTL use: the
  register is undefined on real hardware, and all-ones makes the counter a plain 16-bit
  down-counter that passes through the negative half — which is what the common
  `TST @#177710 / BPL` wait idiom (and any program that never programs the limit)
  needs. See `docs/BK0010-hardware.md`.
- **The VM1 C-flag bug** is emulated: after `MOVB xx,Rd` / `MFPS Rd` (register
  destination only) *conditional branches* read C as clear, while PSW keeps the correct
  value. Model it as a separate read path (`Cpu::brFlags()`) — clearing C in PSW instead
  is too broad and breaks games (tried, reverted). `MFPS Rd` is itself a trigger, so it
  does not clear the effect; any other instruction does.
- **A frame is 61440 CPU ticks (48.83 Hz), not 60000/50 Hz.** `Board::ticksPerFrame()`
  derives it from the raster geometry (`Vp037::CLKIN_PER_FRAME / 2`), so the emulator
  frame and the 037 frame coincide exactly and `syncToFrameTop()` truncates nothing.
  Documented in `~/БК0010/БК-docs/11-экран-и-клавиатура.md`.
- **Interrupts are latched, not fire-and-forget**: `Board::deliverFrameInterrupts()` only
  raises `irqFramePending_`; `tryDeliverInterrupts()` runs after every instruction and
  delivers once the PSW mask opens. A request must never be dropped because the CPU
  happened to be at priority 7 at the frame boundary.
- **`WAIT` + interrupt**: `op_wait()` rewinds PC so the instruction re-executes while
  idle; only `Cpu::interrupt()` clears `waiting_`, and it advances PC past the WAIT so
  the pushed return address is the *next* instruction. Do not clear the wait flag
  anywhere else (`runTicks` just breaks out) — otherwise `RTI` lands back on the WAIT and
  the program hangs. `RESET` must not touch PSW; it resets devices via `Cpu::setResetHook`.
- **Speaker is 3-bit**: the piezo sums bits 6, 5 and 2 of `0177716` (mask `0144`) into 8
  levels; `Speaker::feed()` takes a level 0..7, not a bit.
- **Interrupt gating**: reset PSW is `0340` (priority 7, masked) so the monitor ROM
  can install its vectors before the 50 Hz IRQ fires — do not reset to 0, games will
  jump to an unset vector and HALT. `Board::deliverFrameInterrupts` also refuses to
  deliver if the vector word is 0.
- **Running a game** requires the monitor ROM to boot first. The GUI does this via
  the continuous timer; headless mode explicitly runs ~25 frames before `loadBin`.
- **Screen mapping** (`Screen::render`) ports `scr.c`: video RAM is 0040000, 256
  lines × 64 bytes. Color mode = 2 bits/pixel (256 wide, doubled to 512); mono = 1
  bit/pixel (512 wide), LSB first. Palette 0 = {black, blue, green, red}.
- **Screen text / OCR** (`src/core/ScreenOcr.{h,cpp}`): the monitor ROM character
  generator is a flat table of 10-byte glyphs (one byte per scanline, LSB = leftmost
  pixel) at **0112036** = the glyph for code 020, covering codes 020..0177 then
  0240..0377 with **no slots for 0200..0237**. Text cells are 8x10 in the 64-column
  mode and 16x10 in the 32-column ("wide" = colour) mode, where the ROM doubles every
  glyph bit into a bit pair. Screen codes are 8-bit, *not* the KOI-7 the keyboard
  sends: Cyrillic is 0300..0337 (lower) / 0340..0377 (upper) in the `kKoi7H1Utf8`
  order. Latin `A B C E H K M O P T X` and Cyrillic `А В С Е Н К М О Р Т Х` have
  bit-identical glyphs — `ocrScreen` resolves the script per line by majority.
- **СМК-512 hooks into `Memory`, not into `IoBus`.** Its control register `0177130`
  sits BELOW the I/O page (`0177600`), and the board overrides ROM as well, so the
  interception point is a separate `MpiDevice` interface consulted first by
  `read/write/peek/poke` — and only for `addr >= 0100000`, because the real decoder
  is gated on `A15 = 1`. A write returns "did the controller swallow it": for the
  `W` cells of Табл. 1 the write must ALSO reach the BK's own register, which is
  what makes HALT-mode shadow RAM work. `Board` owns the `Smk512` and allocates its
  512 KB only while the board is enabled (`setSmk512`). Its mode register is reset by
  power-on and by the «СТОП» key (МПИ pin А1 = ОСТ, hence `Board::pressStop`), but
  NOT by the `RESET` instruction — that is bus INIT on pin Б19, a separate signal in
  the replica's CPLD. Mode/page show up in the
  Soft-ICE «СИСТ. РЕГИСТРЫ» panel (only when the board is in, so the no-board
  overlay layout is unchanged) and in `bk_io_state`. End-to-end coverage is the
  third-party `tests/data/SMKTEST.bin`, run automatically by `cpu_tests` — it OCRs
  the report off the screen, and the glyph table must come from a SEPARATE pristine
  `Memory`, because ОЗУ10/All cover the ROM font at `0112036`. Details and the
  deliberate omissions (controller ROM, FDD, HDD) are in `docs/smk512.md`.
- **Pixel format** is `0xAARRGGBB` uint32; uploaded to GL as `GL_BGRA` and wrapped as
  `QImage::Format_ARGB32` — keep these in sync if you touch either.
- **GL context**: `main.cpp` sets `Qt::AA_UseDesktopOpenGL` before `QApplication`.
  Without it, NVIDIA under a Wayland session fails to create the 3.3-core context
  (`QEGLPlatformContext ... EGL_BAD_MATCH (3009)`, spamming `QOpenGLWidget: Failed
  to create context`). Don't remove that line.

## Conventions

- Addresses/opcodes are written in **octal** (matching BK/PDP-11 docs and tooling).
- Comments and UI strings are in Russian; keep that style.
- `Memory::peek*/poke*` are side-effect-free (for debugger/disasm/screen);
  `read*/write*` go through the I/O bus and the access hook — use the right one.
