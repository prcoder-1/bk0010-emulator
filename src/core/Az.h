#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "Memory.h"
#include "AzBlitter.h"
#include "AzCtrl.h"

namespace bk {

// Контроллер AZБК (MAXIOL) в разъёме МПИ — всё, что видит программа, кроме сети
// (docs/azbk.md). Модель — по GID BKemu 4.4 (devemu/AZBK*) с поправками retro-tang-bk
// под прошивки 18/19 и по разбору прошивки 00019:
//  * память 32 Мбайт страницами по 4 Кбайт: 0–037 — тень ОЗУ БК-0011М, 040–077 — служебная
//    (040 — заставка, 076/077 — теневые регистры видео и палитра для снимка), 0100–0177 —
//    слоты ПЗУ (всегда только чтение), 0200–0377 — эмуляция СМК-512, дальше — ОЗУ;
//  * маппер: 16 окон по 4 Кбайт (0177300–0177336), маски «подключено» 0177340, «только
//    чтение» 0177342, «тень» 0177344, управление 0177346, копии 0177350/0177352; запись
//    в 0177716 (карта БК-0011М) и 0177130 (режим СМК) переводится в окна;
//  * VGA 1024×768, 60 Гц: восемь режимов, три слоя, палитра 338 ячеек, блиттер;
//  * звук: два AY (0177172–0177175), Covox 16/8 бит (0177200–0177212), DMA с IMA ADPCM;
//  * контроллер дисков и карты SD (AzCtrl), ГПСЧ 0177550, версия 0177370, ИРПС-заглушка.
//
// На шине плата стоит ВСЯ (fullBus): активное окно отвечает вместо БК, теневое копирует
// записи БК, регистры 0177000–0177577 — её; 0177716 она складывает с ответом БК
// (адрес пуска 0170000 — стартовое ПЗУ в окне 15).
class Azbk : public MpiDevice {
public:
    static constexpr uint32_t MEM_BYTES = 32u << 20;
    static constexpr int VGA_W = 1024, VGA_H = 768, VGA_LINES = 806, VGA_HZ = 60;
    static constexpr int PALETTE = 338;
    static constexpr uint16_t START_ADDRESS = 0170000;

    // Разряды 0177346 (раскладка GID и БК-docs §17.4).
    enum : uint16_t {
        MMU_SMK_WND1 = 0100000, MMU_REVTYPE = 040000, MMU_WND1_REV = 020000, MMU_BRDTYPE = 010000,
        MMU_BK11EMU = 04000, MMU_014_OFF = 02000, MMU_037_OFF = 01000, MMU_ROM11 = 040,
        MMU_V100 = 010, MMU_50HZ = 4,
    };

    struct Config {
        std::string sdDir, writeDir;
        bool rw = false;                     // образы дисков на запись
        bool mod037 = false;                 // доработка 037 (REVTYPE = 1): эмуляция 11М на БК-0010
        std::map<int, std::string> units;    // AZn поверх [DISKS]
    };

    // Связь с платой.
    std::function<void(int chip, bool address, uint8_t v)> ayWrite;
    // Модель обоих AY: 0177212, разряд 3 — AY-3-8910 вместо YM2149 (Setup, «AY Emulation»).
    std::function<void(bool ym)> ayModel;
    std::function<void(double l, double r)> sound;   // ЦАП AZ: Covox + DMA, -1..1
    std::function<void()> irq2;                      // кадровое прерывание AZ (60 Гц)
    std::function<void()> irqVector;                 // контроллер, вектор 0174
    std::function<void()> resetMachine;              // команда 037
    std::function<void()> busError;                  // запись в окно «только чтение»

    Azbk();
    // Включить плату: память, регистры, AZ.INI (ПЗУ, заставка, диски). false — нет карты SD.
    bool powerOn(const Config& c, bool bk11, int cpuHz, std::string* err = nullptr);
    void release();                                  // вынуть плату (память освобождается)
    bool on() const { return !mem_.empty(); }
    void setMachine(bool bk11, int cpuHz) { bk11_ = bk11; cpuHz_ = cpuHz; }
    const Config& config() const { return cfg_; }
    // Сброс БК (кнопка, включение, команда 037): ResetCold платы — окна сняты, в окне 15
    // стартовое ПЗУ; разряды частоты 177346 и память сохраняются. mcu — заодно
    // перезапустить микроконтроллер (заново AZ.INI).
    void coldReset(bool mcu);

    // MpiDevice
    bool fullBus() const override { return true; }
    bool mpiRead(uint16_t addr, uint16_t& value) override;
    bool mpiPeek(uint16_t addr, uint16_t& value) const override;
    bool mpiWrite(uint16_t addr, uint16_t value, bool isByte) override;
    bool mpiPoke(uint16_t addr, uint16_t value, bool isByte) override;
    // Обмен обслуживает сама плата (активное окно или её регистр) — для модели шины.
    bool serves(uint16_t addr, bool write) const;
    // Ответ пуска 0177716 (без бит БК): 0100200, +040000 в эмуляции 11М.
    uint16_t sel1() const;

    void advance(int ticks);                         // растр VGA, блиттер, DMA
    bool timer48() const;                            // кадровый таймер AZ «48 Гц» (по кадру БК)
    void ar2Lat();                                   // АР2+ЛАТ: 512/256 точек в старых режимах
    bool legacyCovoxOff() const { return (cvxCsr_ & 2) != 0; }
    bool ym() const { return !(cvxCsr_ & 010); }     // AY платы — YM2149 (иначе AY-3-8910)
    // 177662 на БК-0010 отвечает плата (эмуляция 11М): адрес и значение — у неё.
    bool emu11() const { return (mmu_ & (MMU_BK11EMU | MMU_BRDTYPE)) == (MMU_BK11EMU | MMU_BRDTYPE); }

    // Кадр VGA 1024×768 (0xAARRGGBB), последний досчитанный.
    const uint32_t* frame() const { return front_.data(); }
    uint64_t frames() const { return frames_; }

    // Отладка, MCP, тесты.
    uint8_t*       mem()       { return mem_.data(); }
    const uint8_t* mem() const { return mem_.data(); }
    uint16_t memWord(uint32_t byteAddr) const;
    void     setMemWord(uint32_t byteAddr, uint16_t v);
    struct Window { uint16_t page; bool active, ro, shadow; };
    Window window(int w) const {
        const int i = w & 15;
        return {map_[i], ((winCtrl_ >> i) & 1) != 0, ((winRo_ >> i) & 1) != 0, ((winShdw_ >> i) & 1) != 0};
    }
    uint16_t mmu() const { return mmu_; }
    uint16_t reg(uint16_t addr) const;               // значение регистра без побочных эффектов
    std::string summary() const;                     // одна строка
    std::string details() const;                     // несколько строк: окна, видео, звук, диски
    AzCtrl&       ctrl()       { return ctrl_; }
    const AzCtrl& ctrl() const { return ctrl_; }
    const AzBlitter& blitter() const { return blt_; }
    uint32_t paletteRgb(int i) const { return pal_[i % PALETTE]; }

    void saveState(std::vector<uint8_t>& out) const;
    bool loadState(const uint8_t*& p, const uint8_t* end);

private:
    // Маппер.
    bool active(int w) const { return (winCtrl_ >> w) & 1; }
    bool shadowOnly(int w) const { return ((winShdw_ & ~winCtrl_) >> w) & 1; }
    bool writable(int w) const { return active(w) && !((winRo_ >> w) & 1) && !romPage(map_[w]); }
    static bool romPage(uint16_t p) { return p >= 0100 && p < 0200; }
    uint32_t phys(uint16_t addr) const { return ((uint32_t(map_[addr >> 12]) << 12) | (addr & 07777)) & (MEM_BYTES - 1); }
    void setMap(int w, uint16_t page) { map_[w] = page & 017777; }
    bool in716(uint16_t w);
    void mm11();                                     // AZ_MemoryManager
    void in130(uint16_t w);
    void mmSmk();                                    // AltPro_MemoryManager

    // Регистры 0177000–0177577 (и 0177716 на чтение).
    bool regRead(uint16_t addr, uint16_t& v, bool peek);
    bool regWrite(uint16_t addr, uint16_t v, bool isByte);

    // Видео.
    void setMode(uint16_t v);
    void vgaLine();
    void renderLine(uint32_t* out);
    uint16_t vword(uint32_t wordAddr) const { return reinterpret_cast<const uint16_t*>(mem_.data())[wordAddr & (MEM_BYTES / 2 - 1)]; }
    void resetPalette();
    void prepareScreenshot();

    // Звук.
    void dmaWrite(uint16_t v);
    void dmaSample();
    void soundOut();
    int16_t imaDecode(int ch, int nibble);

    Config cfg_;
    bool bk11_ = true;
    int  cpuHz_ = 4000000;
    std::vector<uint8_t> mem_;
    AzCtrl ctrl_;
    AzBlitter blt_;

    // Маппер.
    uint16_t map_[16] = {0};
    uint16_t winCtrl_ = 0, winRo_ = 0, winShdw_ = 0, mmu_ = 0, az130_ = 0, az716_ = 0;
    uint8_t  preB_[8] = {0}, preS_[8] = {0};
    uint16_t preBCtrl_ = 0, preBShdw_ = 0, preSCtrl_ = 0, preSRo_ = 0;
    bool     smkTrigger_ = false;

    // Видео: регистры.
    uint16_t scrCsr_ = 012100;
    uint16_t pageL0_ = 4, pageL1_ = 4, pageL2_ = 4;
    uint16_t palCell_ = 0;
    uint16_t vscr_[3] = {0};                         // 177250 (слой 0), 177246 (1), 177244 (2)
    uint8_t  hscr_[3] = {0};                         // 177252 (0), 177254 (1), 177256 (2)
    uint16_t legacyPal_ = 0;
    uint16_t az662_ = 0;
    bool     smallScreen_ = false;                   // 177664 бит 9 = 0: строки за 192-й гаснут
    uint32_t pal_[PALETTE];
    // Видео: развёртка и кадр.
    uint64_t lineAcc_ = 0;                           // такты·строк в секунду
    int      vline_ = 0;                             // строка VGA 0..805
    int      srcLine_ = 0, rep_ = 0;                 // строка изображения, повтор строки
    struct FrameRegs { int mode, llen, xs, ys, lines; uint16_t vscr[3]; uint8_t hscr[3]; uint16_t page[3]; } fr_{};
    bool     sync_ = false;
    std::vector<uint32_t> back_, front_;
    uint32_t lineBuf_[VGA_W];
    uint64_t frames_ = 0;

    // Звук.
    uint16_t dmaCsr_ = 0, dmaPage_ = 0, dmaCur_ = 0;
    uint32_t dmaLen_ = 0;
    bool     dmaPlaying_ = false, dmaDone_ = false;
    uint32_t dmaAddr_ = 0, dmaLeft_ = 0;             // слово памяти, сколько слов осталось
    uint16_t dmaWord_ = 0;
    int      dmaUnits_ = 0;                          // отсчётов в текущем слове
    int16_t  pcmL_ = 0, pcmR_ = 0;
    int      imaCur_[2] = {0, 0}, imaIdx_[2] = {0, 0};
    uint64_t dmaAcc_ = 0;
    uint8_t  ay1a_ = 0, ay1d_ = 0, ay2a_ = 0, ay2d_ = 0;
    uint16_t cvxL_ = 0, cvxR_ = 0, cvxM_ = 0, cvxCsr_ = 0;

    // Прочее.
    uint16_t com_[4] = {0, 0, 0200, 0};             // ИРПС: передатчик всегда свободен
    uint32_t rnd_[4] = {0x35355353, 0x12345678, 0x9ABCDEF0, 0x13579BDF};
};

} // namespace bk
