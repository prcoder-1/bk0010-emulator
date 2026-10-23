#include "Az.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace bk {

Azbk::Azbk() {
    ctrl_.memLoad = [this](uint32_t a, const uint8_t* p, size_t n) {
        if (mem_.empty() || a >= MEM_BYTES) return;
        n = std::min<size_t>(n, MEM_BYTES - a);
        std::memcpy(mem_.data() + a, p, n);
    };
    ctrl_.memRead = [this](uint32_t wa) { return memWord(wa * 2); };
    ctrl_.memWrite = [this](uint32_t wa, uint16_t w) { setMemWord(wa * 2, w); };
    ctrl_.screenshotPrepare = [this]() { prepareScreenshot(); };
    ctrl_.irq = [this]() { if (irqVector) irqVector(); };
    ctrl_.resetMachine = [this]() { if (resetMachine) resetMachine(); };
    resetPalette();
}

bool Azbk::powerOn(const Config& c, bool bk11, int cpuHz, std::string* err) {
    cfg_ = c;
    bk11_ = bk11; cpuHz_ = cpuHz;
    mem_.assign(MEM_BYTES, 0);
    blt_.attach(reinterpret_cast<uint16_t*>(mem_.data()), MEM_BYTES / 2);
    blt_.reset();
    back_.assign(VGA_W * VGA_H, 0xff000000u);
    front_.assign(VGA_W * VGA_H, 0xff000000u);
    std::fill(std::begin(map_), std::end(map_), 0);
    winCtrl_ = winRo_ = winShdw_ = 0;
    mmu_ = 0; az130_ = az716_ = 0;
    scrCsr_ = 0; setMode(012100);
    pageL0_ = pageL1_ = pageL2_ = 4;
    palCell_ = 0;
    std::fill(std::begin(vscr_), std::end(vscr_), 0);
    std::fill(std::begin(hscr_), std::end(hscr_), 0);
    legacyPal_ = 0; az662_ = 0; smallScreen_ = false;
    resetPalette();
    lineAcc_ = 0; vline_ = 0; srcLine_ = rep_ = 0; frames_ = 0;
    dmaCsr_ = dmaPage_ = dmaCur_ = 0; dmaLen_ = 0; dmaPlaying_ = dmaDone_ = false;
    dmaLeft_ = dmaAddr_ = 0; dmaUnits_ = 0; pcmL_ = pcmR_ = 0; dmaAcc_ = 0;
    imaCur_[0] = imaCur_[1] = imaIdx_[0] = imaIdx_[1] = 0;
    ay1a_ = ay1d_ = ay2a_ = ay2d_ = 0;
    cvxL_ = cvxR_ = cvxM_ = 0x8000; cvxCsr_ = 0;
    com_[0] = 0; com_[1] = 0; com_[2] = 0200; com_[3] = 0;
    AzCtrl::Config cc;
    cc.sdDir = c.sdDir; cc.writeDir = c.writeDir; cc.rw = c.rw; cc.units = c.units; cc.net = c.net;
    const bool ok = ctrl_.init(cc, err);
    // Заставка из [LOGO] — 256×192, 256 цветов в странице 040 (так её показывает GID).
    if (ctrl_.logoLoaded()) { pageL0_ = 040; setMode(063223); }
    coldReset(false);
    soundOut();
    if (ayModel) ayModel(ym());
    return ok;
}

bool Azbk::usable(const Config& c, std::string* err) {
    AzCtrl::Config cc;
    cc.sdDir = c.sdDir; cc.writeDir = c.writeDir;
    return AzCtrl::usable(cc, err);
}

void Azbk::setUnit(int unit, const std::string& host) {
    if (host.empty()) cfg_.units.erase(unit); else cfg_.units[unit] = host;
    if (on()) ctrl_.setUnit(unit, host);
}

void Azbk::release() {
    ctrl_.close();
    mem_.clear(); mem_.shrink_to_fit();
    back_.clear(); front_.clear();
    blt_.attach(nullptr, 1);
}

// ResetCold: окна и маски сняты, в окне 15 — стартовое ПЗУ (слот 0, страница 0100),
// сброшены SMK_WND1, V100, 014_OFF, 037_OFF; REVTYPE — по наличию доработки 037.
// Разряды частоты (8:6) и всё прочее в 0177346 переживают сброс — на этом стоит
// двухпроходный пуск AZBOOT.
void Azbk::coldReset(bool mcu) {
    if (!on()) return;
    if (mcu) ctrl_.reboot();
    blt_.reset();
    winCtrl_ = winRo_ = winShdw_ = 0;
    mmu_ &= static_cast<uint16_t>(~(MMU_SMK_WND1 | MMU_V100 | MMU_014_OFF | MMU_037_OFF));
    if (cfg_.mod037) { mmu_ |= MMU_REVTYPE; mmu_ &= static_cast<uint16_t>(~MMU_WND1_REV); }
    else mmu_ &= static_cast<uint16_t>(~MMU_REVTYPE);
    std::fill(std::begin(preB_), std::end(preB_), 0);
    std::fill(std::begin(preS_), std::end(preS_), 0);
    preBCtrl_ = preBShdw_ = preSCtrl_ = preSRo_ = 0;
    smkTrigger_ = false;
    setMap(15, 0100);
    winCtrl_ |= 0100000;
    winRo_ |= 0100000;
    dmaPlaying_ = false; pcmL_ = pcmR_ = 0;
    soundOut();
}

uint16_t Azbk::sel1() const {
    uint16_t w = 0100200;
    if ((mmu_ & (MMU_REVTYPE | MMU_037_OFF | MMU_BK11EMU)) == (MMU_REVTYPE | MMU_037_OFF | MMU_BK11EMU)) w |= 040000;
    return w;
}

bool Azbk::timer48() const {
    return on() && (mmu_ & (MMU_V100 | MMU_50HZ)) == (MMU_V100 | MMU_50HZ);
}

// ---- Маппер -----------------------------------------------------------------------

// Запись в 0177716 с разрядом 11 — карта памяти БК-0011М (AZ_716_In GID). Её видит
// маппер, если машина — БК-0011М или это БК-0010 с доработкой 037 и отключённой 037-й.
bool Azbk::in716(uint16_t w) {
    if (!(w & 04000)) return false;
    const bool brd10 = mmu_ & MMU_BRDTYPE;
    if (brd10 && (mmu_ & (MMU_BRDTYPE | MMU_037_OFF | MMU_REVTYPE)) != (MMU_BRDTYPE | MMU_037_OFF | MMU_REVTYPE))
        return false;
    az716_ = w;
    // Код страницы 177716 — как есть (не номер страницы 11М): теневые страницы AZ 0–037
    // идут по кодам, страница кода c — это 4c..4c+3.
    int m0 = ((w >> 12) & 7) << 2;
    for (int i = 0; i < 4; ++i) preB_[i] = static_cast<uint8_t>(m0++);
    if (w & 2)      { preB_[4] = 0124; preB_[5] = 0125; preB_[6] = 0122; preB_[7] = 0123; }
    else if (w & 1) { preB_[4] = 0126; preB_[5] = 0127; preB_[6] = 0130; preB_[7] = 0131; }
    else { int m1 = ((w >> 8) & 7) << 2; for (int i = 4; i < 8; ++i) preB_[i] = static_cast<uint8_t>(m1++); }
    const bool roms01 = w & 3, roms34 = w & 030;
    preBCtrl_ = ((roms01 && (mmu_ & MMU_ROM11)) || (!roms34 && (mmu_ & MMU_037_OFF))) ? 0x0f00 : 0;
    preBShdw_ = (!(roms01 || roms34) && !(mmu_ & MMU_037_OFF)) ? 0x0f00 : 0;
    mm11();
    return true;
}

void Azbk::mm11() {
    for (int i = 0; i < 4; ++i) setMap(i, static_cast<uint16_t>(030 + i));
    for (int i = 0; i < 4; ++i) setMap(4 + i, preB_[i]);
    const uint16_t bits = (mmu_ & MMU_037_OFF) ? 0xff : 0;
    winCtrl_ = static_cast<uint16_t>((winCtrl_ & ~0xff) | bits);
    winShdw_ = static_cast<uint16_t>((winShdw_ & ~0xff) | (~bits & 0xff));
    winRo_ &= static_cast<uint16_t>(~0xff);
    if (!(mmu_ & MMU_SMK_WND1)) {
        for (int i = 0; i < 4; ++i) setMap(8 + i, preB_[4 + i]);
        winCtrl_ = static_cast<uint16_t>((winCtrl_ & ~0x0f00) | preBCtrl_);
        winShdw_ = static_cast<uint16_t>((winShdw_ & ~0x0f00) | preBShdw_);
        winRo_ &= static_cast<uint16_t>(~0x0f00);
    } else {
        for (int i = 0; i < 4; ++i) setMap(8 + i, preS_[i]);
        winCtrl_ = static_cast<uint16_t>((winCtrl_ & ~0x0f00) | (preSCtrl_ & 0x0f00));
        winShdw_ &= static_cast<uint16_t>(~0x0f00);
        winRo_ = static_cast<uint16_t>((winRo_ & ~0x0f00) | (preSRo_ & 0x0f00));
    }
}

// Режим СМК-512 через 0177130 (AZ_130_In): строб (разряды 1 и 2), следом — слово режима.
void Azbk::in130(uint16_t w) {
    if (!smkTrigger_ && (w & 6) == 6) { smkTrigger_ = true; return; }
    if (!smkTrigger_ || (w & 6) == 6) return;
    smkTrigger_ = false;
    az130_ = w;
    mmSmk();
}

namespace {
enum class Bank : uint8_t { Off, Ram, Rom };
struct SmkMode { bool wnd1; struct { uint16_t page; Bank mode; } bank[8]; };
// Таблица режимов СМК в окнах 8–15 (GID AZBK_MemMapper.cpp, m_SMK512_MemStatus), по
// разрядам 6:4 слова режима. ПЗУ платы в окне 0160000 в режимах Std10 и Std11 — слот 8
// (AZ337, загрузчик дисков), как в SYS и как у retro-tang-bk: у GID там «страница 0», и
// программа, оставившая Std11 (демо leCSIcon проверяет СМК и уходит в него), звала по
// 0160004 тень ОЗУ вместо драйвера диска. Std10, окна 0120000/0130000 — страницы 4, 5 по
// порядку: у GID для AZ они переставлены (5, 4), а его же FDD_SMK512.cpp, Табл. 1 СМК,
// наш Smk.cpp (SMKTEST) и OneChipBook/BkEmu дают 4, 5.
const SmkMode kSmkModes[8] = {
    {false, {{0, Bank::Off}, {0, Bank::Off}, {0, Bank::Off}, {0, Bank::Off}, {4, Bank::Ram}, {5, Bank::Ram}, {6, Bank::Ram}, {7, Bank::Ram}}},       // HLT11
    {true,  {{4, Bank::Ram}, {5, Bank::Ram}, {6, Bank::Ram}, {7, Bank::Ram}, {0, Bank::Ram}, {1, Bank::Ram}, {2, Bank::Ram}, {3, Bank::Ram}}},       // ALL
    {false, {{0, Bank::Off}, {0, Bank::Off}, {0, Bank::Off}, {0, Bank::Off}, {4, Bank::Ram}, {5, Bank::Ram}, {6, Bank::Ram}, {7, Bank::Ram}}},       // ОЗУ11
    {true,  {{0, Bank::Off}, {0, Bank::Off}, {2, Bank::Ram}, {3, Bank::Ram}, {4, Bank::Ram}, {5, Bank::Ram}, {0110, Bank::Rom}, {7, Bank::Ram}}},    // STD10
    {true,  {{0, Bank::Ram}, {1, Bank::Ram}, {2, Bank::Ram}, {3, Bank::Ram}, {4, Bank::Ram}, {5, Bank::Ram}, {6, Bank::Ram}, {7, Bank::Ram}}},       // HLT10
    {true,  {{0, Bank::Ram}, {1, Bank::Ram}, {2, Bank::Ram}, {3, Bank::Ram}, {4, Bank::Ram}, {5, Bank::Ram}, {6, Bank::Ram}, {7, Bank::Ram}}},       // ОЗУ10
    {false, {{0, Bank::Off}, {0, Bank::Off}, {0, Bank::Off}, {0, Bank::Off}, {0, Bank::Off}, {0, Bank::Off}, {0110, Bank::Rom}, {7, Bank::Ram}}},    // STD11
    {true,  {{0, Bank::Off}, {0, Bank::Off}, {6, Bank::Ram}, {7, Bank::Ram}, {0, Bank::Ram}, {1, Bank::Ram}, {0110, Bank::Rom}, {0100, Bank::Rom}}}, // SYS
};
}

void Azbk::mmSmk() {
    const int mode = (az130_ & 0160) >> 4;
    const SmkMode& ms = kSmkModes[mode];
    int base = 0200;
    if (az130_ & 02000) base += 010;
    if (az130_ & 4) base += 020;
    if (az130_ & 010) base += 040;
    if (az130_ & 1) base += 0100;
    preSCtrl_ = preSRo_ = 0;
    for (int n = 0; n < 8; ++n) {
        const auto& b = ms.bank[n];
        if (b.mode == Bank::Off) continue;
        preSCtrl_ |= static_cast<uint16_t>(1 << (8 + n));
        preS_[n] = static_cast<uint8_t>(b.mode == Bank::Ram ? b.page + base : b.page);
    }
    if (mode == 4) preSRo_ |= 1 << 8;                   // HLT10: окно 8 только на чтение
    for (int i = 12; i < 16; ++i) setMap(i, preS_[i - 8]);
    winCtrl_ = static_cast<uint16_t>((winCtrl_ & ~0xf000) | (preSCtrl_ & 0xf000));
    winRo_ = static_cast<uint16_t>((winRo_ & ~0xf000) | (preSRo_ & 0xf000));
    winShdw_ &= static_cast<uint16_t>(~0xf000);        // вся память СМК — память AZ
    if (ms.wnd1) {
        for (int i = 8; i < 12; ++i) setMap(i, preS_[i - 8]);
        winCtrl_ = static_cast<uint16_t>((winCtrl_ & ~0x0f00) | (preSCtrl_ & 0x0f00));
        winRo_ = static_cast<uint16_t>((winRo_ & ~0x0f00) | (preSRo_ & 0x0f00));
        winShdw_ &= static_cast<uint16_t>(~0x0f00);
        mmu_ |= MMU_SMK_WND1;
    } else {
        for (int i = 8; i < 12; ++i) setMap(i, preB_[i - 4]);
        winCtrl_ = static_cast<uint16_t>((winCtrl_ & ~0x0f00) | preBCtrl_);
        winRo_ &= static_cast<uint16_t>(~0x0f00);
        winShdw_ = static_cast<uint16_t>((winShdw_ & ~0x0f00) | preBShdw_);
        mmu_ &= static_cast<uint16_t>(~MMU_SMK_WND1);
    }
}

// ---- Шина -------------------------------------------------------------------------

uint16_t Azbk::memWord(uint32_t a) const {
    if (mem_.empty()) return 0;
    a &= MEM_BYTES - 2;
    return static_cast<uint16_t>(mem_[a] | (mem_[a + 1] << 8));
}

void Azbk::setMemWord(uint32_t a, uint16_t v) {
    if (mem_.empty()) return;
    a &= MEM_BYTES - 2;
    mem_[a] = static_cast<uint8_t>(v); mem_[a + 1] = static_cast<uint8_t>(v >> 8);
}

bool Azbk::serves(uint16_t addr, bool write) const {
    if (!on()) return false;
    const uint16_t a = addr & ~1;
    if (a >= 0177600) return false;
    if (a >= 0177000) return true;
    const int w = a >> 12;
    return write ? writable(w) : active(w);
}

bool Azbk::mpiRead(uint16_t addr, uint16_t& v) {
    if (!on()) return false;
    const uint16_t a = addr & ~1;
    if (a >= 0177600) {
        if (a == 0177716) { v = sel1(); return true; }
        return false;
    }
    if (a >= 0177000) return regRead(a, v, false);
    if (!active(a >> 12)) return false;
    v = memWord(phys(a));
    return true;
}

bool Azbk::mpiPeek(uint16_t addr, uint16_t& v) const {
    if (!on()) return false;
    const uint16_t a = addr & ~1;
    if (a >= 0177600) {
        if (a == 0177716) { v = sel1(); return true; }
        return false;
    }
    if (a >= 0177000) return const_cast<Azbk*>(this)->regRead(a, v, true);
    if (!active(a >> 12)) return false;
    v = memWord(phys(a));
    return true;
}

bool Azbk::mpiWrite(uint16_t addr, uint16_t value, bool isByte) {
    if (!on()) return false;
    const uint16_t a = addr & ~1;
    const int w = a >> 12;
    // Значение, выровненное по байту (нечётный байт — в старшую половину).
    const uint16_t wide = isByte ? static_cast<uint16_t>((addr & 1) ? (value & 0377) << 8 : value & 0377) : value;
    if (a >= 0177600) {
        switch (a) {
        case 0177716:
            if (wide & 04000) in716(static_cast<uint16_t>((az716_ & ~077433) | (wide & 077433)));
            return false;
        case 0177662: {
            const uint16_t v = isByte ? static_cast<uint16_t>((addr & 1) ? (az662_ & 0377) | wide : (az662_ & 0177400) | wide) : value;
            az662_ = v;
            const uint16_t t = mmu_ & (MMU_BK11EMU | MMU_BRDTYPE);
            const bool emu = t == (MMU_BK11EMU | MMU_BRDTYPE);
            if (emu || !t) {
                legacyPal_ = (v >> 8) & 017;
                pageL0_ = (v & 0100000) ? 034 : 4;
            }
            if (emu) {
                if (v & 040000) mmu_ &= static_cast<uint16_t>(~(MMU_V100 | MMU_50HZ));
                else mmu_ |= MMU_V100 | MMU_50HZ;
            }
            return emu;                                    // на БК-0010 регистр — платы
        }
        case 0177664: {
            // Байт меняет только свою половину: прежнее слово собирается из скролла и
            // бита малого экрана (иначе MOVB в младший байт включала малый экран).
            const uint16_t prev = static_cast<uint16_t>(((vscr_[0] + 0330) & 0377) | (smallScreen_ ? 0 : 01000));
            const uint16_t v = isByte ? static_cast<uint16_t>((addr & 1) ? (prev & 0377) | wide : (prev & 0177400) | wide) : value;
            vscr_[0] = static_cast<uint16_t>((v & 0377) - 0330);
            smallScreen_ = !(v & 01000);
            return false;
        }
        default:
            return false;
        }
    }
    if (a >= 0177000) return regWrite(addr, value, isByte);
    // Тень: запись идёт в БК, а плата держит копию — из неё рисуются старые видеорежимы.
    if (!active(w) && shadowOnly(w) && !romPage(map_[w])) {
        const uint32_t p = phys(a);
        if (isByte) mem_[p | (addr & 1)] = static_cast<uint8_t>(value);
        else setMemWord(p, value);
        return false;
    }
    if (!active(w)) return false;
    if (!writable(w)) { if (busError) busError(); return true; }
    const uint32_t p = phys(a);
    if (isByte) mem_[p | (addr & 1)] = static_cast<uint8_t>(value);
    else setMemWord(p, value);
    return true;
}

bool Azbk::mpiPoke(uint16_t addr, uint16_t value, bool isByte) {
    if (!on()) return false;
    const uint16_t a = addr & ~1;
    if (a >= 0177000) return false;
    const int w = a >> 12;
    if (!active(w) && !shadowOnly(w)) return false;
    const uint32_t p = phys(a);
    if (isByte) mem_[p | (addr & 1)] = static_cast<uint8_t>(value);
    else setMemWord(p, value);
    return active(w);
}

// ---- Регистры ---------------------------------------------------------------------

bool Azbk::regRead(uint16_t a, uint16_t& v, bool peek) {
    v = 0;
    if (a >= 0177300 && a <= 0177336) { v = map_[(a - 0177300) >> 1]; return true; }
    switch (a) {
    case 0177160: v = static_cast<uint16_t>((dmaCsr_ & ~010) | (dmaDone_ ? 010 : 0)); return true;
    case 0177162: v = dmaPage_; return true;
    case 0177164: v = static_cast<uint16_t>((dmaLen_ >> 16) & 0377); return true;
    case 0177166: v = static_cast<uint16_t>(dmaLen_); return true;
    case 0177170: v = dmaCur_; return true;
    case 0177172: v = static_cast<uint16_t>((ay1d_ << 8) | ay1a_); return true;
    case 0177174: v = static_cast<uint16_t>((ay2d_ << 8) | ay2a_); return true;
    case 0177176: return true;                                  // OPL2 — убран в прошивке 19
    case 0177200: v = cvxL_; return true;
    case 0177202: v = cvxR_; return true;
    case 0177204: v = cvxM_; return true;
    case 0177206: v = static_cast<uint16_t>((cvxL_ >> 8) | (cvxR_ & 0177400)); return true;
    case 0177212: v = cvxCsr_; return true;
    case 0177220: case 0177222: case 0177224: case 0177226:
        v = ctrl_.read((a - 0177220) >> 1, peek);
        return true;
    case 0177230: v = scrCsr_; return true;
    case 0177232: v = pageL0_; return true;
    case 0177234: v = palCell_; return true;
    case 0177236: {
        if (palCell_ >= PALETTE) return true;
        const uint32_t c = pal_[palCell_];
        v = static_cast<uint16_t>((((c >> 19) & 037) << 10) | (((c >> 11) & 037) << 5) | ((c >> 3) & 037));
        return true;
    }
    case 0177240: v = pageL1_; return true;
    case 0177242: v = pageL2_; return true;
    case 0177244: v = vscr_[2]; return true;
    case 0177246: v = vscr_[1]; return true;
    case 0177250: v = vscr_[0]; return true;
    case 0177252: v = hscr_[0]; return true;
    case 0177254: v = hscr_[1]; return true;
    case 0177256: v = hscr_[2]; return true;
    case 0177270: v = blt_.readCtrl(); return true;
    case 0177272: v = blt_.page(); return true;
    case 0177340: v = winCtrl_; return true;
    case 0177342: v = winRo_; return true;
    case 0177344: v = winShdw_; return true;
    case 0177346: v = mmu_; return true;
    case 0177350: v = az130_; return true;
    case 0177352: v = az716_; return true;
    case 0177370: v = 19; return true;                          // версия ПЛИС: сборка 00019
    case 0177550: {
        // 128-разрядный РСЛОС: каждое чтение — новое слово.
        if (peek) { v = static_cast<uint16_t>(rnd_[0]); return true; }
        for (int i = 0; i < 16; ++i) {
            const uint32_t b = ((rnd_[3] >> 31) ^ (rnd_[3] >> 29) ^ (rnd_[3] >> 4) ^ (rnd_[3] >> 2)) & 1;
            rnd_[3] = (rnd_[3] << 1) | (rnd_[2] >> 31); rnd_[2] = (rnd_[2] << 1) | (rnd_[1] >> 31);
            rnd_[1] = (rnd_[1] << 1) | (rnd_[0] >> 31); rnd_[0] = (rnd_[0] << 1) | b;
        }
        v = static_cast<uint16_t>(rnd_[0]);
        return true;
    }
    case 0177560: case 0177562: case 0177564: case 0177566:
        v = com_[(a - 0177560) >> 1];
        if (a == 0177564) v = 0200;                              // передатчик всегда свободен
        return true;
    case 0177130: case 0177132: return true;
    default: return false;
    }
}

bool Azbk::regWrite(uint16_t addr, uint16_t value, bool isByte) {
    const uint16_t a = addr & ~1;
    // Байтовая запись: нечётный байт значим только у данных AY, прочее — как слово с
    // нулевым старшим байтом (GID).
    if (isByte) {
        value &= 0377;
        if (addr & 1) {
            if (a == 0177172) { ay1d_ = static_cast<uint8_t>(value); if (ayWrite) ayWrite(0, false, ay1d_); }
            else if (a == 0177174) { ay2d_ = static_cast<uint8_t>(value); if (ayWrite) ayWrite(1, false, ay2d_); }
            return true;
        }
        if (a == 0177172) { ay1a_ = static_cast<uint8_t>(value); if (ayWrite) ayWrite(0, true, ay1a_); return true; }
        if (a == 0177174) { ay2a_ = static_cast<uint8_t>(value); if (ayWrite) ayWrite(1, true, ay2a_); return true; }
    }
    if (a >= 0177300 && a <= 0177336) { setMap((a - 0177300) >> 1, value); return true; }
    switch (a) {
    case 0177160: dmaWrite(value); return true;
    case 0177162: dmaPage_ = value & 017777; return true;
    case 0177164: dmaLen_ = (dmaLen_ & 0xffff) | (uint32_t(value & 0377) << 16); return true;
    case 0177166: dmaLen_ = (dmaLen_ & 0xff0000) | value; return true;
    case 0177170: return true;
    case 0177172:
        ay1a_ = static_cast<uint8_t>(value); ay1d_ = static_cast<uint8_t>(value >> 8);
        if (ayWrite) { ayWrite(0, true, ay1a_); ayWrite(0, false, ay1d_); }
        return true;
    case 0177174:
        ay2a_ = static_cast<uint8_t>(value); ay2d_ = static_cast<uint8_t>(value >> 8);
        if (ayWrite) { ayWrite(1, true, ay2a_); ayWrite(1, false, ay2d_); }
        return true;
    case 0177176: return true;
    case 0177200: cvxL_ = value; soundOut(); return true;
    case 0177202: cvxR_ = value; soundOut(); return true;
    case 0177204: cvxL_ = cvxR_ = cvxM_ = value; soundOut(); return true;
    case 0177206:
        if (isByte) cvxM_ = cvxL_ = cvxR_ = static_cast<uint16_t>((value & 0377) << 8);
        else { cvxL_ = static_cast<uint16_t>((value & 0377) << 8); cvxR_ = value & 0177400; }
        soundOut();
        return true;
    case 0177212:
        cvxCsr_ = value & 017;
        if (ayModel) ayModel(ym());
        return true;
    case 0177220: case 0177222: case 0177224: case 0177226:
        ctrl_.write((a - 0177220) >> 1, value);
        return true;
    case 0177230: setMode(value); return true;
    case 0177232: pageL0_ = value & 017777; return true;
    case 0177240: pageL1_ = value & 017777; return true;
    case 0177242: pageL2_ = value & 017777; return true;
    case 0177234: palCell_ = value & 0777; return true;
    case 0177236:
        if (palCell_ < PALETTE) {
            auto c5 = [](unsigned x) { x &= 037; return (x << 3) | (x >> 2); };
            pal_[palCell_] = 0xff000000u | (c5(value >> 10) << 16) | (c5(value >> 5) << 8) | c5(value);
        }
        return true;
    case 0177244: vscr_[2] = value & 0777; return true;
    case 0177246: vscr_[1] = value & 0777; return true;
    case 0177250: vscr_[0] = value & 0777; return true;
    case 0177252: hscr_[0] = static_cast<uint8_t>(value); return true;
    case 0177254: hscr_[1] = static_cast<uint8_t>(value); return true;
    case 0177256: hscr_[2] = static_cast<uint8_t>(value); return true;
    case 0177270: blt_.writeCtrl(value); return true;
    case 0177272: blt_.writePage(value); return true;
    case 0177340: winCtrl_ = value; return true;
    case 0177342: winRo_ = value; return true;
    case 0177344: winShdw_ = value; return true;
    case 0177346:
        // REVTYPE (разряд 14) — только чтение, разряды 1:0 — тип платы.
        mmu_ = static_cast<uint16_t>((mmu_ & (MMU_REVTYPE | 3)) | (value & ~(MMU_REVTYPE | 3)));
        return true;
    case 0177350: case 0177352: case 0177370: case 0177550: return true;
    case 0177130: in130(value); return true;
    case 0177132: return true;
    case 0177560: case 0177562: case 0177564: case 0177566:
        com_[(a - 0177560) >> 1] = value;
        return true;
    default: return false;
    }
}

uint16_t Azbk::reg(uint16_t addr) const {
    uint16_t v = 0;
    if (addr == 0177716) return sel1();
    const_cast<Azbk*>(this)->regRead(addr & ~1, v, true);
    return v;
}

// ---- Сводки ------------------------------------------------------------------------

// printf в std::string без ограничения длины (кириллица в UTF-8 — по два байта).
static std::string strf(const char* f, ...) __attribute__((format(printf, 1, 2)));
static std::string strf(const char* f, ...) {
    va_list ap, ap2;
    va_start(ap, f);
    va_copy(ap2, ap);
    const int n = std::vsnprintf(nullptr, 0, f, ap);
    va_end(ap);
    std::string s(n > 0 ? size_t(n) : 0, '\0');
    if (n > 0) std::vsnprintf(&s[0], size_t(n) + 1, f, ap2);
    va_end(ap2);
    return s;
}

std::string Azbk::summary() const {
    if (!on()) return "AZБК: нет";
    return strf("AZБК: 177346=%06o окна=%06o RO=%06o тень=%06o видео 177230=%06o стр %o/%o/%o",
                mmu_, winCtrl_, winRo_, winShdw_, scrCsr_, pageL0_, pageL1_, pageL2_);
}

std::string Azbk::details() const {
    if (!on()) return "AZБК не установлен";
    std::string s = "Окна маппера (адрес: страница [активно/RO/тень]):\n";
    for (int w = 0; w < 16; ++w)
        s += strf("  %06o: %05o %c%c%c%s", w << 12, map_[w], active(w) ? 'A' : '-',
                  ((winRo_ >> w) & 1) ? 'R' : '-', ((winShdw_ >> w) & 1) ? 'S' : '-', (w & 3) == 3 ? "\n" : "");
    static const char* kTimer[] = {"выкл", "выкл", "60 Гц", "48 Гц"};
    s += strf("177346=%06o: %s, доработка 037 %s, 037 %s, эмуляция 11М %s, ПЗУ 11М %s, частота %d, таймер %s%s\n",
              mmu_, (mmu_ & MMU_BRDTYPE) ? "БК-0010" : "БК-0011М", (mmu_ & MMU_REVTYPE) ? "есть" : "нет",
              (mmu_ & MMU_037_OFF) ? "откл" : "вкл", (mmu_ & MMU_BK11EMU) ? "вкл" : "выкл",
              (mmu_ & MMU_ROM11) ? "вкл" : "выкл", (mmu_ >> 6) & 7, kTimer[(mmu_ >> 2) & 3],
              (mmu_ & MMU_SMK_WND1) ? ", окно 1 — СМК" : "");
    s += strf("СМК 177350=%06o, 11М 177352=%06o\n", az130_, az716_);
    static const int kBpp[] = {1, 2, 4, 8};
    s += strf("Видео 177230=%06o: %d бит/точку%s, строка %d слов, ×%d по X, ×%d по Y, рулон %d строк%s; "
              "страницы %o/%o/%o, скролл В %o/%o/%o Г %o/%o/%o, легаси-палитра %d%s; кадров %llu%s\n",
              scrCsr_, kBpp[scrCsr_ & 3], (scrCsr_ & 4) ? ", слои" : "", fr_.llen, fr_.xs, fr_.ys, fr_.lines,
              (scrCsr_ & 04000) ? ", синхр." : "", pageL0_, pageL1_, pageL2_, vscr_[0], vscr_[1], vscr_[2],
              hscr_[0], hscr_[1], hscr_[2], legacyPal_, smallScreen_ ? ", малый экран" : "",
              static_cast<unsigned long long>(frames_), live_ ? "" : " (рисуется по запросу)");
    s += strf("Блиттер 177270=%06o стр %o, запусков %llu\n", blt_.readCtrl(), blt_.page(),
              static_cast<unsigned long long>(blt_.runs()));
    s += strf("DMA 177160=%06o стр %o длина %u %s; Covox L=%06o R=%06o 177212=%o; AY (%s) 1: %o/%o 2: %o/%o\n",
              dmaCsr_, dmaPage_, dmaLen_, dmaPlaying_ ? "играет" : "стоит", cvxL_, cvxR_, cvxCsr_,
              ym() ? "YM2149" : "AY-3-8910", ay1a_, ay1d_, ay2a_, ay2d_);
    s += "Контроллер: " + ctrl_.summary() + "\n";
    s += "Карта SD: " + cfg_.sdDir + (cfg_.rw ? " (запись в образы)" : "") + "\n";
    s += ctrl_.net().summary();
    if (!ctrl_.netStatus().empty()) s += "  кабель: " + ctrl_.netStatus() + "\n";
    for (const auto& u : ctrl_.units())
        s += strf("  AZ%d: %s (%u блоков)%s%s%s\n", u.unit, u.name.c_str(), u.blocks,
                  u.open ? "" : " НЕ ОТКРЫТ", u.rw ? " запись" : "", u.dirty ? " изменён в памяти" : "");
    return s;
}

// ---- Снимок -------------------------------------------------------------------------

namespace {
void putS(std::vector<uint8_t>& o, const std::string& s) {
    const uint32_t n = static_cast<uint32_t>(s.size());
    o.insert(o.end(), reinterpret_cast<const uint8_t*>(&n), reinterpret_cast<const uint8_t*>(&n) + 4);
    o.insert(o.end(), s.begin(), s.end());
}
bool getS(const uint8_t*& p, const uint8_t* e, std::string& s) {
    uint32_t n;
    if (e - p < 4) return false;
    std::memcpy(&n, p, 4); p += 4;
    if (static_cast<uint32_t>(e - p) < n) return false;
    s.assign(reinterpret_cast<const char*>(p), n); p += n;
    return true;
}
}

void Azbk::saveState(std::vector<uint8_t>& out) const {
    auto put = [&](const void* v, size_t n) { const uint8_t* b = static_cast<const uint8_t*>(v); out.insert(out.end(), b, b + n); };
    putS(out, cfg_.sdDir); putS(out, cfg_.writeDir);
    const uint8_t fl = static_cast<uint8_t>((cfg_.rw ? 1 : 0) | (cfg_.mod037 ? 2 : 0) | (smkTrigger_ ? 4 : 0)
                                            | (smallScreen_ ? 8 : 0) | (sync_ ? 16 : 0) | (dmaPlaying_ ? 32 : 0) | (dmaDone_ ? 64 : 0));
    put(&fl, 1);
    const uint32_t nu = static_cast<uint32_t>(cfg_.units.size());
    put(&nu, 4);
    for (const auto& kv : cfg_.units) { const int32_t u = kv.first; put(&u, 4); putS(out, kv.second); }
    put(map_, sizeof map_);
    const uint16_t r[] = {winCtrl_, winRo_, winShdw_, mmu_, az130_, az716_, preBCtrl_, preBShdw_, preSCtrl_, preSRo_,
                          scrCsr_, pageL0_, pageL1_, pageL2_, palCell_, vscr_[0], vscr_[1], vscr_[2], legacyPal_, az662_,
                          dmaCsr_, dmaPage_, dmaCur_, dmaWord_, cvxL_, cvxR_, cvxM_, cvxCsr_, com_[0], com_[1], com_[2], com_[3]};
    put(r, sizeof r);
    put(preB_, 8); put(preS_, 8); put(hscr_, 3);
    const uint8_t ay[4] = {ay1a_, ay1d_, ay2a_, ay2d_};
    put(ay, 4);
    put(pal_, sizeof pal_);
    const int32_t vi[6] = {vline_, srcLine_, rep_, dmaUnits_, imaCur_[0], imaCur_[1]};
    put(vi, sizeof vi);
    const int32_t vj[2] = {imaIdx_[0], imaIdx_[1]};
    put(vj, sizeof vj);
    put(&fr_, sizeof fr_);
    put(&lineAcc_, 8); put(&dmaAcc_, 8); put(&dmaLen_, 4); put(&dmaAddr_, 4); put(&dmaLeft_, 4);
    put(&pcmL_, 2); put(&pcmR_, 2); put(rnd_, sizeof rnd_); put(&frames_, 8);
    blt_.saveState(out);
    ctrl_.saveState(out);
    // Память — только ненулевые страницы по 4 Кбайт (32 Мбайт почти всегда пусты).
    std::vector<uint32_t> pages;
    for (uint32_t pg = 0; pg < MEM_BYTES / 4096; ++pg) {
        const uint8_t* q = mem_.data() + pg * 4096;
        if (std::any_of(q, q + 4096, [](uint8_t x) { return x != 0; })) pages.push_back(pg);
    }
    const uint32_t np = static_cast<uint32_t>(pages.size());
    put(&np, 4);
    for (uint32_t pg : pages) { put(&pg, 4); put(mem_.data() + pg * 4096, 4096); }
    // Сеть — меткой в конце (в снимках до неё её нет).
    put("NET1", 4);
    ctrl_.net().saveState(out);
}

bool Azbk::loadState(const uint8_t*& p, const uint8_t* e) {
    auto get = [&](void* v, size_t n) { if (static_cast<size_t>(e - p) < n) return false; std::memcpy(v, p, n); p += n; return true; };
    Config c;
    uint8_t fl; uint32_t nu;
    if (!getS(p, e, c.sdDir) || !getS(p, e, c.writeDir) || !get(&fl, 1) || !get(&nu, 4)) return false;
    for (uint32_t i = 0; i < nu; ++i) {
        int32_t u; std::string s;
        if (!get(&u, 4) || !getS(p, e, s)) return false;
        c.units[u] = s;
    }
    c.rw = fl & 1; c.mod037 = fl & 2;
    // Плата заново «вставляется» (диски открываются по конфигурации снимка), потом
    // поверх неё — состояние.
    c.net = cfg_.net;                                // кабель — хоста, не снимка
    if (!on() || cfg_.sdDir != c.sdDir || cfg_.units != c.units || cfg_.rw != c.rw) powerOn(c, bk11_, cpuHz_);
    cfg_ = c;
    smkTrigger_ = fl & 4; smallScreen_ = fl & 8; sync_ = fl & 16; dmaPlaying_ = fl & 32; dmaDone_ = fl & 64;
    uint16_t r[32];
    if (!get(map_, sizeof map_) || !get(r, sizeof r) || !get(preB_, 8) || !get(preS_, 8) || !get(hscr_, 3)) return false;
    winCtrl_ = r[0]; winRo_ = r[1]; winShdw_ = r[2]; mmu_ = r[3]; az130_ = r[4]; az716_ = r[5];
    preBCtrl_ = r[6]; preBShdw_ = r[7]; preSCtrl_ = r[8]; preSRo_ = r[9];
    scrCsr_ = 0; setMode(r[10]); pageL0_ = r[11]; pageL1_ = r[12]; pageL2_ = r[13]; palCell_ = r[14];
    vscr_[0] = r[15]; vscr_[1] = r[16]; vscr_[2] = r[17]; legacyPal_ = r[18]; az662_ = r[19];
    dmaCsr_ = r[20]; dmaPage_ = r[21]; dmaCur_ = r[22]; dmaWord_ = r[23];
    cvxL_ = r[24]; cvxR_ = r[25]; cvxM_ = r[26]; cvxCsr_ = r[27];
    for (int i = 0; i < 4; ++i) com_[i] = r[28 + i];
    uint8_t ay[4];
    int32_t vi[6], vj[2];
    if (!get(ay, 4) || !get(pal_, sizeof pal_) || !get(vi, sizeof vi) || !get(vj, sizeof vj) || !get(&fr_, sizeof fr_)
        || !get(&lineAcc_, 8) || !get(&dmaAcc_, 8) || !get(&dmaLen_, 4) || !get(&dmaAddr_, 4) || !get(&dmaLeft_, 4)
        || !get(&pcmL_, 2) || !get(&pcmR_, 2) || !get(rnd_, sizeof rnd_) || !get(&frames_, 8)) return false;
    ay1a_ = ay[0]; ay1d_ = ay[1]; ay2a_ = ay[2]; ay2d_ = ay[3];
    vline_ = vi[0]; srcLine_ = vi[1]; rep_ = vi[2]; dmaUnits_ = vi[3]; imaCur_[0] = vi[4]; imaCur_[1] = vi[5];
    imaIdx_[0] = vj[0]; imaIdx_[1] = vj[1];
    if (!blt_.loadState(p, e) || !ctrl_.loadState(p, e)) return false;
    uint32_t np;
    if (!get(&np, 4)) return false;
    std::fill(mem_.begin(), mem_.end(), 0);
    for (uint32_t i = 0; i < np; ++i) {
        uint32_t pg;
        if (!get(&pg, 4) || pg >= MEM_BYTES / 4096 || !get(mem_.data() + pg * 4096, 4096)) return false;
    }
    if (e - p >= 4 && std::memcmp(p, "NET1", 4) == 0) {
        p += 4;
        if (!ctrl_.net().loadState(p, e)) return false;
    }
    soundOut();
    if (ayModel) ayModel(ym());
    return true;
}

} // namespace bk
