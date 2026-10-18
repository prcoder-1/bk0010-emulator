# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A BK-0010-01 (Soviet PDP-11 clone, K1801VM1 CPU) emulator-debugger in C++17/Qt6
with OpenGL screen output and a Soft-ICE style debugger. Loads and runs `.BIN`
game files. Also emulates the **BK-0011M** — the DEFAULT machine of the app (GUI without a
saved choice, headless, MCP server), with the AY on; a `.BIN` without an explicit machine (CLI,
GUI «Файл», MCP `bk_load`) picks it itself — `Board::binTargetsBk11`: writes 0177662/3 and neither
polls it as the keyboard (CMP/BIT/TST) nor sets vector 4 more than once, OR an odd-numbered EMT
(a BOS call) within the first 48 instructions reachable from the entry (no own EMT vector 030; `MOV #n,@#30` with n in the BOS ROM 0140000–0157777 is BOS's, not own)
→ BK-0011M, else BK-0010 (14 11M demos, 0/571 BK-0010 programs). `Board`
itself still defaults to the BK-0010 (tests rely on it). The 11M is selected via the menu «Машина», `--machine bk11m`, MCP
`bk_reset {machine}`; it has 4 MHz, 128 KB paged RAM, BOS ROMs, 16 palettes switchable
mid-frame, the AY-3-8910 on 0177714 — see `docs/bk0011m.md`. See `README.md` for the user-facing feature list and hotkeys, and
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
- ROMs (`monit10.rom`, `basic10.rom`; for the 11M `bk11m_324_bos.rom`, `bk11m_325_ext.rom`,
  `bk11m_327..329_basic*.rom`, `bk11m_330_mstd.rom`) live in `roms/`; the path is baked in via
  `BK_DEFAULT_ROM_DIR`, overridable with `--roms <dir>` or `BK_ROM_DIR`.

### Headless verification (no display needed — use this to check changes)

```sh
QT_QPA_PLATFORM=offscreen ./build/bk0010-emulator --frames 200 --shot out.png game.bin
```

`--help` / `-h` prints every option (keep `printHelp` in `src/main.cpp` in sync when adding one).
Flags: `--frames N`, `--shot`, `--dbgshot` (Soft-ICE overlay), `--memvis`,
`--hotpath`, `--callgraph`, `--flame`, `--flamechart`, `--hotchart`, `--mono`,
`--no-arb037` (disable the КР1801ВП1-037 DRAM-window model, on by default),
`--cpu-phase 0|1` (CPU clock phase vs the 037 window, see below),
`--smk` / `--no-smk` (СМК-512 memory-expansion board, ON by default in the GUI, headless and
MCP since 2026-10-16; `Board` itself still starts without it. On the BK-0010 it covers the BASIC
ROM 0120000–0157777, so programs calling into it — tetr-music, TIMERTST, overclk — need
`--no-smk`),
`--hdd <img.hdi>` / `--hdd-b` / `--hdd-rw` / `--hdd-ctl auto|smk|samara` (IDE hard disk on
the СМК-512 or the «Самара» controller, `docs/hdd.md`),
`--machine bk10|bk11m` (`--bk10`, `--bk11m`), `--ay` / `--no-ay` (AY-3-8910 on 0177714; default on
for 11M), `--2ay` / `--no-2ay` and `--covox` / `--no-covox` (second AY and Covox, default on for 11M), `--no-scanline` (11M renders per scanline by default), `--mstd` (11M test ROM block),
`--video <file.avi|.mp4>` / `--video-codec zmbv|h264` (default: by extension) / `--video-size 1024x768|512x256` /
`--video-audio` / `--no-video-audio` (screen capture with sound, GUI and headless, `docs/video.md`),
`--scanline` (per-scanline rendering — each line drawn with the palette/screen values live
when the beam crossed it and the scroll latched for the field, driven off the `Vp037` raster; OFF by default,
see `Board::setScanlineRender`), `--key <code>`, `--keyframe N`.
GUI-only: `--aspect34` / `--no-aspect34` (non-square BK pixels, ON by default).
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
  as ~46 JSON-RPC tools (JSON-RPC 2.0, newline-delimited over stdio, QtCore JSON) —
  run/step/step-over/step-out, `bk_machine` (show / switch БК-0010-01 ↔ БК-0011М), regs/mem, break (optionally conditional) / watch
  (data watchpoints), backtrace, xrefs, search/diff memory, type, callers/callees,
  frames, coverage, profile (speedscope folded stacks) / calltree (text CCT) / profile-reset
  (`frames: N` = profile only the next N frames), vram (ASCII-art screen),
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
  АР2 (GUI: Alt; MCP: `"ар2+X"`) is the key's own code `| 0200` → vector `0274` with the
  7-bit code in `0177662`; the monitor's `0101362` maps it to the "lower register".

Key cross-cutting facts to know before editing the CPU or screen:

- **Two machines, one `Board`** (`Board::setMachine`, `bk11()`). The 11M differs in: clock
  (4 MHz; the 037 stays at 6 MHz, so a frame is 81920 ticks and 1 tick = 1.5 CLKIN —
  `advanceDevices` counts half-CLKINs), memory (`Memory` maps 8 blocks of 8 KB: identity on
  the 0010, pages/ROM slots on the 11M; the page code in 0177716 is NOT binary,
  `{1,5,2,3,4,7,0,6}`), 0177716 writes (bit 11 = 1 memory map, 0 tape/speaker/STOP lock),
  0177662 writes (palette, screen page 5/6, bit 14 = 1 DISABLES the 0100 frame IRQ — on the
  0010 that write must stay a bus error, the СМК firmware relies on it), start address
  0140000, ROM writes trap (vector 4) and empty ROM sockets trap on read, no EMT 36 intercept.
  `.BIN` on the 11M = what the BOS monitor does for «L», «R6/1000», «G»: power-on with a key
  held (BOS checks it at 0140376 and drops into its monitor), the file into the USER page map
  `040402` (014400) — NOT `0114`, which is BOS's copy of the write-only 0177716 and holds the
  system 054002 while BOS runs — then `040420=01000` and BOS's own «addrG» at 0141060 (R3 = addr).
  СМК-512 works on the 11M too: its ВЭД signal (modes SYS/Std10/ОЗУ10/All)
  disconnects the 11M window 1 (`Memory::setWindow1Off`, driven by `Smk512::setModeHook`),
  without the SMK firmware image the board starts (power-on, СТОП) in Std11 the way the
  firmware would, and the floppy controller stays on the bus even with no disks — BOS probes
  it with a write to 0177130, which the SMK accepts, then calls 0160000. On the 11M `IoBus::ioWrite` gets the UNALIGNED address for
  byte writes (the odd byte goes to the high half, as in GID).
- **The emulator frame starts at the frame IRQ** (`Vp037::syncToLine`): on the BK-0010 at the
  037 nVSYNC pulse, line 277 (21st blanking line, RTL `va_037.v:239-241`), 43 lines before the
  field; on the BK-0011M at VGATE + 452 CLKIN = line 256, CLKIN 324 (`Vp037::IRQ11_LINE`,
  `IRQ11_CLKIN`; VGATE rises at line 255 word 32) — the board's WTI/SYNCO missing-pulse detector,
  traced and measured on the 037 netlist in OneChipBook (`src/peripheral/bk_evnt.sv`). Babylona
  (mid-line palette, self-calibrating screen) needs it to the word; Prince of Persia's SOB-timed
  switches (rows 130/221) stay in blanking. Babylona still shows one 037 window early (right frame
  line of its calibration red) — unexplained, do not "fix" by moving the IRQ without evidence.
  An interrupt taken FROM WAIT has no discarded prefetch (VM1 RTL: IRQ2 at 0 → PSW push SYNC at 9,
  vs 19 mid-stream): `Board::busStep(…, fromWait)`.
  `Vp037::fieldLine()` is the field line within the current frame (negative before the field);
  per-scanline rendering uses it, and a write to 0177662/0177664 first finishes the current line
  up to `lineWord()` (`Board::flushRaster`, `Screen::renderSpan`). Inside an instruction such a
  write takes effect at the END of the instruction (`Board::rasterWrite`) — the bus write is the
  last cycle; applying it at the instruction start moved palette splits 5–6 words early.
  On the 11M a BYTE write to 0177663 goes to the high half, as in GID/BKBTL: the "200%" demo
  enables the frame IRQ only with `MOVB #14,@#177663` and hangs on WAIT without it. INSULT
  part 19 then shows its per-line page/palette "checker floor"; a whole-frame renderer shows
  that frame in palette 0 instead (its last write is `CLR @#177662`) — do not "fix" that.
- **The scroll register is latched once per field**: 037 loads `0177664` into the line
  address only at ALOAD (lines 278–279, `Vp037::SCROLL_LATCH_LINE`); afterwards the address
  just counts lines. A mid-field write shows from the NEXT frame (`Board::latchScroll`,
  `rasterScroll_`); only bit 9 (small screen) acts live. 0330 is "no scroll" because the
  address counts 40 blank lines (050) before the field.
- **Frames are exactly ticksPerFrame() apart in absolute ticks** (`Board::beginFrame`,
  `frameEnd_`): the overshoot of the last instruction and the frame-IRQ entry carry into the next
  frame, and the beam is advanced by it at the frame start. Starting each frame from "now" made
  the emulated frame tens of ticks too long, so anything locked to the VM1 timer crawled up the
  screen (INSULT part 19). `runFrame`, `runFrameSlice`, `runUntil`, `runUntilReturn` all go
  through `beginFrame`.
- **WAIT idles in time**: `runTicks` no longer skips the rest of the frame on WAIT; it calls
  `idleTicks` in 64-tick chunks so sound (AY music runs from the frame IRQ), the floppy, the
  VM1 timer and the beam keep going. Interrupt entry ticks also go through `advanceDevices`.
- **CPU dispatch** (`Cpu::buildTable`): a 1024-entry table indexed by `ir >> 6`,
  mirroring the reference `bk` emulator's `itab.c`. Instruction semantics/flags are
  ported 1:1 from that emulator (`/home/prcoder/emulators/БК-0010-01/bk`). If you
  change flag logic, cross-check against `single.c`/`double.c`/`branch.c` there.
- **Effective address** load/store (`Cpu::loadSrc/loadDst/storeDst2/...`) ports
  `ea.c`; `storeDst2` writes back to the cached `eaAddr_` for modify-in-place ops.
- **Instruction timing is a bus model, not a table, for DRAM.** When every access of an
  instruction hits BK DRAM (A15=0), `Board::busStep` times it from the VM1 bus schedule
  (`src/core/Vm1Timing.cpp`: per-access pauses in half-ticks, measured on the VM1+037 RTL)
  and the 037 window (a request gets the first 037 decision >= q+2.5 half-ticks; decisions
  every 4 CPU ticks; the access ends on the next CPU tick >= decision+5.5). Two things
  carry over between instructions and MUST stay: the fractional bus clock (`busFrac_`) and
  whether the previous instruction fetched this one by "early prefetch" (`prevTeDram_`:
  NOP/CCC/SCC and every ALU op with a register destination) — the next instruction then
  starts half a tick later, which is exactly why XOP2N (MOV+NOP) is not XOP2 + 12. The
  window phase is `setCpuPhase(0|1)` / `--cpu-phase`: a real BK draws it at power-on
  (D8:B and the 037 `PC[2:0]` are never reset) and gets one of two tables; 0 = the user's
  machine (`MOV (R0),R1` = 28). Verified: XOP2 both phases, XOP2N, 2000/2001 RTL programs,
  and `45com-lo` 45/45 in phase 1 from the same machine (all in `cpu_tests`). `busStep` works
  in units of 1/12 tick (U): 0010 values are the old quarter-tick ones ×3 (results identical);
  the 11M window is 64 U and its request/response/phase (15/39/0 U) were fitted to Manwe's
  BK-0011 table (122/128 forms exact; `docs/bk0011m.md`). On the 11M an instruction that
  touched DRAM and ENDS with a fast write (`MOV #n,(R0)` to 0177714) ends one tick early — 7
  windows, as in OneChipBook's hardware-calibrated model; all-fast instructions keep the full
  tail (real F/G tone legs). A frame restarted mid-run (`beginFrame` when the run stopped
  short) starts on the same tick of the window period as after reset — the window↔beam phase
  is fixed in the 037, and letting it drift made raster timing depend on load history.
  Memory above 0100000 is NOT behind the 037. IO and СМК RAM answer at once (end = request
  + 4 / 5 / 7 half-ticks for read / write / RMW write — eCat3's model, which reproduces
  Manwe's СМК table 192/192). ROM (К1801РЕ2) is slower on reads by `Board::kRomReadExtra`
  quarter-ticks: measured with `tests/hw/ROMIO.BIN` on the user's BK, where all ROM reads
  lose a 037 window in phase 1 (fit: 1..14), and `tests/hw/ROMSOB.BIN` (SOB loop running
  entirely in ROM = 17 + delay) gave 18.00 in both phases, so the delay is 4 (one tick). The
  board keeps a per-instruction access log (`acc_`) and times any mix of DRAM and fast
  memory. Some microcode work is visible only after a FAST exchange (`xf`/`pfXf`/`teFast`
  in `Vm1Sched`): after a DRAM wait it is hidden — do not apply it to DRAM, that breaks
  the XOP2 MOVB / XOP2N photos. Interrupt entry (IRQ2, СТОП, keyboard with IAKO) and the
  T-bit trap go through the same model (the T-trap used to cost 0 ticks). The chain phase
  (`busSkew_`, `busFrac_`, tails) is saved in snapshots. Only forms without a schedule
  (WAIT/HALT/RESET/MARK/illegal) fall back to `timingFor` + `arbReadPenalty` /
  `timingFast`. eCat3's 037 parameters are fitted to Manwe's machine — do not import them.
  See `docs/slow-memory-timing.md`.
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
  needs. See `docs/BK0010-hardware.md`. In reload mode there is NO count with the value 0: 0 is
  replaced by the limit at once, so the period is exactly the limit (5,4,3,2,1,5…; verified on
  real hardware, and K1801VM1 manual §6.6) — also when many periods pass between register reads.
  The `RESET` instruction stops the timer but does NOT touch `TVE_COUNT` (§6.6: nINIT/nDCLO do not
  change it), so `resetDevices` must not go through `timerSetMode` (a CSR write copies LIMIT→COUNT).
- **The VM1 C-flag bug** is emulated: after `MOVB xx,Rd` / `MFPS Rd` (register
  destination only) *conditional branches* read C as clear, while PSW keeps the correct
  value. Model it as a separate read path (`Cpu::brFlags()`) — clearing C in PSW instead
  is too broad and breaks games (tried, reverted). `MFPS Rd` is itself a trigger, so it
  does not clear the effect; any other instruction does.
- **A frame is 61440 CPU ticks (48.83 Hz), not 60000/50 Hz** (81920 on the 11M). `Board::ticksPerFrame()`
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
  anywhere else (`runTicks` only idles through `idleTicks`) — otherwise `RTI` lands back on the WAIT and
  the program hangs. `RESET` must not touch PSW; it resets devices via `Cpu::setResetHook`.
- **Speaker is 3-bit**: the piezo sums bits 6, 5 and 2 of `0177716` (mask `0144`) into 8
  levels; `Speaker::feed()` takes a level 0..7, not a bit. The AY-3-8910 (`Ay8910`, emu2149
  semantics, 1.714 MHz) is mixed into the same mono stream past the piezo RC filter; on
  0177714 a WORD write selects the register, a BYTE write sends data, both inverted. The "2AY"
  board (`--2ay`, default ON on the 11M with the AY and Covox) understands BOTH chip-select
  protocols at once (`Board::ayWrite`): TurboSound — a word whose logical value has bits 3–7 set
  selects the chip by bit 0 (1 = first); Gryphon — bits 15/14 = 0 of the address word (both →
  both); a plain `~reg` goes to the TurboSound-selected chip. The second chip joins the mix only
  once selected ALONE (CSIDOS polls the mouse with MOV #10,@#177714 = "both"). Covox (`--covox`)
  is a mono DAC on the port's LOW byte (odd-byte writes don't touch it); it stays silent while
  writes mix words and bytes (AY protocol, the Multizvuk auto-detect) and within a frame of a
  read of 0177714 (joystick/mouse polling). The `RESET` instruction resets both AYs (all
  registers 0 → silence, TurboSound back to chip 1): INIT reaches the AY through contact А1
  «СБРОС» of the УП connector, GID does the same, and programs rely on it (INSULT's music-off
  routine 035204 starts with RESET). Details in `docs/sound.md`.
- **Interrupt gating**: reset PSW is `0340` (priority 7, masked) so the monitor ROM
  can install its vectors before the 50 Hz IRQ fires — do not reset to 0, games will
  jump to an unset vector and HALT. `Board::deliverFrameInterrupts` also refuses to
  deliver if the vector word is 0.
- **Running a game** requires the monitor ROM to boot first. The GUI does this via
  the continuous timer; headless mode explicitly runs ~25 frames before `loadBin`.
  `loadBin(run)` sets SP=01000, as the start monitor does (BASIC idles with SP=01762, and a
  program that never sets its own stack would overwrite its own code — 45com-lo).
- **Screen mapping** (`Screen::render`) ports `scr.c`: video RAM is 0040000 (on the 11M —
  RAM page 5 or 6 by bit 15 of 0177662, `Memory::videoRam()`), 256
  lines × 64 bytes. Color mode = 2 bits/pixel (256 wide, doubled to 512); mono = 1
  bit/pixel (512 wide), LSB first. Palette 0 = {black, blue, green, red}.
- **The BK pixel is not square, and that lives only in `GlScreen`.** The core buffer is
  always 512×256; by default (`F11` / `--no-aspect34` turn it off) the window draws one
  colour-mode pixel as a **4×3** block of host pixels (texel = 2×3, frame = 1024×768 —
  the 4:3 TV field the raster geometry implies). The scale is always an integer multiple
  (`GlScreen::applyViewport`, recomputed in `paintGL` so the toggle works without a
  resize; the letterbox remainder stays black); below one full frame it falls back to
  aspect-preserving stretching. `MainWindow::showEvent` fits the window once so the GL
  area is exactly `preferredSize()`, and turning 3:4 ON grows the window if it is too
  short for a whole frame (turning it off never shrinks it back). Screenshots
  (`--shot`, MCP `bk_screenshot`), OCR, memvis and the tests see the raw 512×256 buffer
  and are deliberately untouched by the mode.
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
  what makes HALT-mode shadow RAM work. On the register page (`>= 0177600`) the board
  does NOT replace the BK registers: `Memory::busRead` ORs both answers (wired-OR —
  "closing" the registers in SYS/All), except the VM1-internal `0177700–0177712` and the
  `0177660–0177664` read hole (`Smk512::readHole`). `Board` owns the `Smk512` and allocates its
  512 KB only while the board is enabled (`setSmk512`). Its mode register is reset by
  power-on and by the «СТОП» key (МПИ pin А1 = ОСТ, hence `Board::pressStop`), but
  NOT by the `RESET` instruction — that is bus INIT on pin Б19, a separate signal in
  the replica's CPLD. Mode/page show up in the
  Soft-ICE «СИСТ. РЕГИСТРЫ» panel (only when the board is in, so the no-board
  overlay layout is unchanged) and in `bk_io_state`; the DRAM itself has its own
  window (`src/ui/SmkRamWidget.cpp`, Ctrl+D — per-page hex dump that also resolves
  each row to the BK address it is visible at right now, and edits words straight
  into `smk().ram()`, since unmapped pages have no BK address to poke) and is selectable as a
  source in the memory visualiser (`MemCanvas::smkPage`). The heatmap only works
  for the mapped page — `Trace` is indexed by 16-bit BK addresses. End-to-end coverage is the
  third-party `tests/data/SMKTEST.bin`, run automatically by `cpu_tests` — it OCRs
  the report off the screen, and the glyph table must come from a SEPARATE pristine
  `Memory`, because ОЗУ10/All cover the ROM font at `0112036`. Details and the
  deliberate omissions (controller ROM, FDD, HDD) are in `docs/smk512.md`.
- **Floppy (КНГМД)** — `src/core/Fdd.{h,cpp}` (chip 1801ВП1-128 + drives) behind
  `src/core/Kngmd.{h,cpp}` (MpiDevice: driver ROM `roms/disk326.rom` at
  `0160000–0167777` + the two registers). The chip does NOT find sectors: it streams
  a raw IBM-format track and flags a word every 64 µs (`Fdd::PERIOD_TICKS`, pumped
  after EVERY instruction in `stepCore`), so the model is the track stream, ported
  from BKBTL and cross-checked against BKemu. Three details the driver depends on:
  the marker search starts on the FALLING edge of GDR (and clears TR), `RDY` follows
  the motor, and the index window sits at the START of the track. `Board::attachDisk`
  + `bootFromDisk` (entry `0160000`). The disk and СМК-512 live on the bus TOGETHER
  through `Board::MpiMux`: the FDD owns `0177130`/`0177132` (a write to `0177130`
  goes to BOTH — mode latch for СМК, command for the drive), СМК RAM wins wherever
  Табл. 1 maps it, and only then the `0160000` window falls to the driver ROM. Many
  systems need that combo — a bare BK-0010-01 leaves ~16 KB of RAM, too little for
  a kernel; BKUNIX (`disks/bkunix_bk0010.bkd`) boots with `--smk --disk`. A
  positional `.bkd`/`.img` argument (any case) is treated as a disk and boots it,
  and it turns СМК on for that run unless `--no-smk` says otherwise. Diagnostics:
  field lengths (ID = 4-5 words, data = 258) in the Soft-ICE panel and `bk_io_state`,
  plus a ring log with the CPU PC behind `{"fdd_log": true}`. Details in
  `docs/floppy.md`.
- **Hard disk (НЖМД, IDE of the СМК-512)** — `src/core/Hdd.{h,cpp}` (`HddDrive` = ATA drive
  ported from GID `devemu/HDD.cpp`, commands run synchronously; `Ide` = master + slave) on
  `0177740–0177757`, wired in `Board::MpiMux` only while СМК is on AND an image is attached.
  Images are GID's **HDI**: 512-byte IDENTIFY sector (C/H/S in words 1/3/6, `0xA5` + checksum in
  bytes 510/511) + raw sectors stored INVERTED vs the CPU — the board's buffers invert both
  ways (`MpiMux::hddBus`), the drive model is plain ATA. Odd `0177741`/`0177743` are separate
  CS1 registers selected by A0 even for WORD accesses (the SMK ROM driver resets the drive with
  `CLR @#177743`), so `Memory` passes the raw address (with A0) to `mpiRead/mpiWrite`; the mux
  aligns it for СМК/КНГМД. The HDD driver lives only in `roms/smk512.rom`, so attaching an HDD
  (no explicit `--disk-rom`) swaps the controller ROM to it and booting = power-on (the firmware
  captures the start address). Read-only by default (BK writes kept in an in-memory overlay),
  `--hdd-rw` writes through. Flags `--hdd`, `--hdd-b`, `--hdd-rw`, positional `.hdi`; MCP
  `bk_reset {hdd, hdd_rw, hdd_ctl}`. Second controller: **«Самара»** (Lugov's HDD controller
  for the 11M, the ancestor of the SMK's IDE): the same ATA block at `0177640–0177656` with the
  CS1 registers at EVEN `0177620/0177622`, also inverted (`Ide::decodeSamara`); its driver lives
  in the floppy controller's ROM (`roms/samara.rom`, entry 0160000 tries the HDD first), so it
  needs no SMK — but on the BK-0010 the boot block (read to 01000–01777) needs RAM in place of
  BASIC (stack at 01762), which the SMK provides, as for floppies. `--hdd-ctl auto|smk|samara`
  (auto = by the image's partition layout: Samara table in block 1, else SMK;
  `Board::resolveHddController`). `bk::readHddMap` parses both layouts (AltPro block 7, Samara
  block 1) for `bk_io_state`. Details in `docs/hdd.md`.
- **Controller ROM symbols**: `roms/<rom>.sym` (`disk326`, `smk512`, `samara`) load with the
  controller ROM (`Board::loadRomSymbols`) into a separate table that is the FALLBACK layer of
  `Board::symbols()` (`SymbolTable::setFallback`) while the controller is on the bus — user
  symbols win and `.bkdb` annotations never include ROM names. MCP resolves through
  `McpServer::symAt/symAddrOf` (its own `bk_symbols` maps first, then `Board::romSymbols()`).
  The labels were bound to the ROM images by instruction-pattern matching against the
  commented sources (see `docs/floppy.md`) — regenerate the same way if a ROM changes.
- **Video capture** (`src/core/VideoCapture`, `Zmbv`, `AviWriter`; core links zlib + threads):
  one AVI frame per EMULATED BK frame, rate/scale = cpuFreqHz/ticksPerFrame reduced (3125/64 on
  both machines) — never wall-clock. The GUI feeds a frame only when the LAST slice of a frame
  actually ran (`MainWindow::frameDone_`), so debugger pauses add nothing; headless feeds after
  each `runFrame`. Frames are copied into a bounded queue and encoded on a worker thread (the
  producer blocks rather than drop frames). ZMBV is DOSBox's format, 8 bpp with a palette that
  grows as colours appear (index 0 = black = out-of-frame for motion vectors), keyframe every
  300 frames, one deflate stream reset only on keyframes. Files split at ~1 GB (AVI 1.0 limit)
  into `name_002.avi`… with a fresh keyframe. 512x256 mode adds an OpenDML `vprp` 4:3 aspect.
  SIGINT/SIGTERM close the GUI normally (`MainWindow::requestQuit`) so the AVI index is written.
  Sound: a second PCM stream (16-bit mono, Speaker's 44.1 kHz) from a separate lossless tap
  (`Speaker::setCapture/takeCaptured`) — independent of the sound-card FIFO, its trimming, mute
  and `setEnabled(false)` (feed() still runs while capturing). Each frame's samples go into an
  `01wb` chunk right after its `00dc`; the tap is armed at a frame START (GUI: `frameRec_` at
  phase 0), so audio and video start together and stay in sync (903.168 samples/frame).
  Formats are `VideoSink`s (`src/core/VideoSink`: container + codecs, registry
  `videoCodecList/findVideoCodec/videoCodecForPath/makeVideoSink`): `AviSink` (ZMBV + PCM,
  splittable) and `Mp4Sink` (`src/core/FfmpegVideo.cpp`, H.264 libx264 + AAC, faststart — the
  Telegram format). FFmpeg is OPTIONAL: CMake pkg-config `libavcodec libavformat libavutil
  libswscale` → `BK_HAVE_FFMPEG` (PUBLIC on bkcore); `h264` is listed only if libx264 and aac
  exist at runtime. MP4 forces the 1024x768 square-pixel frame (`squarePixelsOnly`) — Telegram
  players may ignore `pasp`. New format: another `VideoSink` + a row in `knownCodecs()`.
- **`MpiDevice` has two read paths**: `mpiRead` (side effects allowed — reading
  `0177132` clears TR) for the CPU and `mpiPeek` (const, side-effect-free) for the
  debugger/disassembler/screen. `Memory::peek*` must never call `mpiRead`.
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
