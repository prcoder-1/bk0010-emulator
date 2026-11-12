#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include "Screen.h"

namespace bk {

// GigaScreen — цвета сверх палитры из двух чередующихся кадров: программа каждый кадр
// переключает экранную страницу (БК-0011М: бит 15 регистра 0177662) или палитру, и на
// ЭЛТ глаз складывает соседние кадры в один цвет. Монитор хоста с другой частотой
// показывает кадры 48,83 Гц неравномерно, и вместо смеси видно мерцание. Поэтому точка,
// цвет которой идёт через кадр (A, B, A), показывается смесью A и B; остальные — как
// есть, так что движущиеся предметы не размываются.
class GigaScreen {
public:
    static constexpr int W = Screen::TEX_W, H = Screen::TEX_H;
    static constexpr int VRAM_BYTES = 64 * H;

    void reset() { frames_ = 0; }
    // Очередной досчитанный кадр: готовый (построчная отрисовка) или исходник — экранное
    // ОЗУ и режим/палитра/скролл из `how` (покадровая). Исходник рисуется, только когда
    // кадр попросят показать: рисовать каждый кадр впрок — заметная доля времени эмуляции.
    void push(const uint32_t* frame);
    void push(const Screen& how, const uint8_t* vram);
    // Что показать вместо `raw` — текущего содержимого экрана. Точки, изменившиеся после
    // последнего досчитанного кадра (шаг в отладчике), показываются как есть.
    const uint32_t* output(const uint32_t* raw);
    // Смесь двух цветов 0xAARRGGBB поровну в линейном свете: глаз усредняет свет
    // (закон Тальбота), а не коды, так что чёрный с белым дают 0xBCBCBC, а не 0x808080.
    static uint32_t mix(uint32_t a, uint32_t b);

private:
    struct Frame {
        std::vector<uint32_t> px;
        bool drawn = false;                // px готов; иначе рисовать из vram
        std::vector<uint8_t> vram;
        bool color = true;
        int palette = 0;
        uint16_t scroll = 0;
    };
    Frame& next();
    const uint32_t* pixels(Frame& f);
    Frame hist_[3];                        // три последних кадра, hist_[head_] — последний
    std::unique_ptr<Screen> scr_;          // рисует исходники
    std::vector<uint32_t> out_;
    int head_ = 0, frames_ = 0;
};

} // namespace bk
