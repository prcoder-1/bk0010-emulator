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
`--key <code>`, `--keyframe N`.
Screenshots render from the CPU-side pixel buffer / `QWidget::grab`, so no GL
context or display is needed.
This is the primary way to verify visual changes; the GUI itself needs `xvfb-run`.

## Architecture (the parts that span multiple files)

Two layers, deliberately decoupled:

- **`src/core/`** — pure C++, no Qt. `Board` owns everything and is the entry point:
  it wires `Cpu` + `Memory` + `Screen` + `Speaker` + `Trace`, implements the
  memory-mapped I/O registers (`IoBus`), drives the 50 Hz frame loop, delivers
  interrupts, and does `.BIN` loading and save/restore of state.
- **`src/ui/`** — Qt6. `MainWindow` owns a `Board`, drives it from a 50 Hz `QTimer`
  (single-threaded — emulation runs in the GUI thread; one frame = ~60000 ticks is
  fast enough), and hosts `GlScreen` + the debugger widgets.
- **`src/mcp/`** — `McpServer`: a headless MCP server (`--server`) exposing the core
  as ~40 JSON-RPC tools (JSON-RPC 2.0, newline-delimited over stdio, QtCore JSON) —
  run/step/step-over/step-out, regs/mem, break (optionally conditional) / watch
  (data watchpoints), backtrace, xrefs, search/diff memory, type, callers/callees,
  frames, coverage, profile (speedscope folded stacks), vram (ASCII-art screen),
  io-state / io-log, emt-log (EMT 36 file I/O), hotspots, screenshot (inline PNG) /
  audio, state save/load.
  Owns its own `Board`, reuses only the `Board`/`Cpu`/`Memory`/`Screen`/`Trace`
  public API + `bk::disasm`. Entered at the very top of `main()` before any GUI
  setup; runs under an offscreen `QGuiApplication` so `bk_screenshot` can save PNGs.
  Registered for Claude Code via `.mcp.json` (server name `bk0010`). Tool args accept
  decimal / `0x` hex / leading-0 octal, and symbols loaded via `bk_symbols`.

Key cross-cutting facts to know before editing the CPU or screen:

- **CPU dispatch** (`Cpu::buildTable`): a 1024-entry table indexed by `ir >> 6`,
  mirroring the reference `bk` emulator's `itab.c`. Instruction semantics/flags are
  ported 1:1 from that emulator (`/home/prcoder/emulators/БК-0010-01/bk`). If you
  change flag logic, cross-check against `single.c`/`double.c`/`branch.c` there.
- **Effective address** load/store (`Cpu::loadSrc/loadDst/storeDst2/...`) ports
  `ea.c`; `storeDst2` writes back to the cached `eaAddr_` for modify-in-place ops.
- **Interrupt gating**: reset PSW is `0340` (priority 7, masked) so the monitor ROM
  can install its vectors before the 50 Hz IRQ fires — do not reset to 0, games will
  jump to an unset vector and HALT. `Board::deliverFrameInterrupts` also refuses to
  deliver if the vector word is 0.
- **Running a game** requires the monitor ROM to boot first. The GUI does this via
  the continuous timer; headless mode explicitly runs ~25 frames before `loadBin`.
- **Screen mapping** (`Screen::render`) ports `scr.c`: video RAM is 0040000, 256
  lines × 64 bytes. Color mode = 2 bits/pixel (256 wide, doubled to 512); mono = 1
  bit/pixel (512 wide), LSB first. Palette 0 = {black, blue, green, red}.
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
