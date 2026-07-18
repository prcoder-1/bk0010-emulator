#include "Gamepad.h"

#ifdef __linux__
#include <linux/joystick.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cstdio>
#include <cstring>
#include <cerrno>

Gamepad::Gamepad()  { openDevice(); }
Gamepad::~Gamepad() { closeDevice(); }

void Gamepad::openDevice() {
    for (int i = 0; i < 4; ++i) {                 // /dev/input/js0 .. js3
        char path[32];
        std::snprintf(path, sizeof path, "/dev/input/js%d", i);
        int fd = ::open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        fd_ = fd;
        std::memset(axis_, 0, sizeof axis_);
        std::memset(button_, 0, sizeof button_);
        char nm[128] = {0};
        name_ = (ioctl(fd_, JSIOCGNAME(sizeof nm), nm) >= 0 && nm[0]) ? nm : "gamepad";
        std::fprintf(stderr, "Gamepad: подключён «%s» (%s)\n", name_.c_str(), path);
        return;
    }
}

void Gamepad::closeDevice() {
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    name_.clear();
}

uint16_t Gamepad::poll() {
    if (fd_ < 0) openDevice();   // попытка (пере)подключения
    if (fd_ < 0) return 0;

    // Вычитать все накопившиеся события (неблокирующе). Флаг JS_EVENT_INIT на
    // синтетических стартовых событиях игнорируем — они задают начальное состояние.
    struct js_event e;
    for (;;) {
        ssize_t r = ::read(fd_, &e, sizeof e);
        if (r == static_cast<ssize_t>(sizeof e)) {
            const uint8_t type = e.type & ~JS_EVENT_INIT;
            if (type == JS_EVENT_AXIS   && e.number < 16) axis_[e.number]   = e.value;
            else if (type == JS_EVENT_BUTTON && e.number < 32) button_[e.number] = e.value ? 1 : 0;
        } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;                       // событий больше нет
        } else {                         // устройство отключили или ошибка
            closeDevice();
            return 0;
        }
    }

    const int DEAD = 12000;              // мёртвая зона стика (диапазон оси ±32767)
    // X — ось 0 (плюс хат 6, если есть); Y — ось 1 (плюс хат 7). Крестовина ретро-
    // геймпадов и левый стик рапортуют по осям 0/1; у стик-контроллеров крестовина
    // часто приходит как хат на осях 6/7 — учитываем оба.
    const bool L = axis_[0] < -DEAD || axis_[6] < -DEAD;
    const bool R = axis_[0] >  DEAD || axis_[6] >  DEAD;
    const bool U = axis_[1] < -DEAD || axis_[7] < -DEAD;
    const bool D = axis_[1] >  DEAD || axis_[7] >  DEAD;

    // Раскладки: направления + «огонь» (f[0..3] — кнопки геймпада A/B/X/Y, f[4] —
    // доп. кнопка/Start). См. Gamepad.h; «Стандарт» — распайка «Джойвокс»/эмулятор gid.
    struct Layout { uint16_t up, down, left, right, f[5]; };
    static const Layout LAYOUTS[4] = {
        // Standard (Джойвокс / gid): ВВ=001 ВПР=002 ВН=004 ВЛ=010
        { 0001, 0004, 0010, 0002, { 0040, 0200, 0100, 0400, 0020 } },
        // Break House (BKBTL): кнопки 001-010; ВЛ=020 ВН=040 ВПР=0100 ВВ=0200
        { 0200, 0040, 0020, 0100, { 0001, 0002, 0004, 0010, 0001 } },
        // SWCorp (BKBTL): кнопки 001-010; ВПР=020 ВН=040 ВЛ=01000 ВВ=02000
        { 02000, 0040, 01000, 0020, { 0001, 0002, 0004, 0010, 0001 } },
        // Клад-2 (реверс игры): ВПР=001 ВЛ=002 ВВ=004 ВН=010; стрельба влево/вправо=020/040
        { 0004, 0010, 0002, 0001, { 0020, 0040, 0020, 0040, 0020 } },
    };
    const Layout& lay = LAYOUTS[static_cast<int>(standard_)];

    uint16_t v = 0;
    if (L) v |= lay.left;
    if (R) v |= lay.right;
    if (U) v |= lay.up;
    if (D) v |= lay.down;
    // Лицевые кнопки 0..3 -> огонь A/B/X/Y; порядок «сырых» кнопок зависит от пада,
    // но большинству игр нужен один «огонь» — любая лицевая его даёт. Плечевые 4/5 —
    // основной огонь, Select/Start 6/7 — доп. кнопка.
    if (button_[0]) v |= lay.f[0];
    if (button_[1]) v |= lay.f[1];
    if (button_[2]) v |= lay.f[2];
    if (button_[3]) v |= lay.f[3];
    if (button_[4] || button_[5]) v |= lay.f[0];
    if (button_[6] || button_[7]) v |= lay.f[4];
    return v;
}

#else  // не-Linux: заглушка — джойстик всегда «не нажат»

Gamepad::Gamepad()  {}
Gamepad::~Gamepad() {}
void Gamepad::openDevice()  {}
void Gamepad::closeDevice() {}
uint16_t Gamepad::poll() { return 0; }

#endif
