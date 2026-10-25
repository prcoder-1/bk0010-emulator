#include "BkKeyboardWidget.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

// Геометрия — в координатах рисунка 1024x401 (как у экранных клавиатур GID); коды —
// позиции матрицы ВП1-014 без модификаторов (буквы — в «заглавном» виде 0100–0137).
enum KType : uint8_t { Reg, Ctrl, Ar2, Shift, Caps, Lower, Stop, Reserved };
enum KColor : uint8_t { Green, Pink, Blue, Yellow, Dark, Light, Red };

struct BkKeyboardWidget::Key {
    short x1, y1, x2, y2;
    uint8_t type, code;
    bool v274;            // клавиша сама даёт вектор 0274 (ТАБ, ШАГ, ПОВТ…)
    uint8_t color;
    const char* lab;      // надпись (латиница / знак / функция)
    const char* cyr;      // кириллица на буквенной клавише
    const char* sub;      // нижняя надпись (функция с НР/АР2)
};

namespace {
using K = BkKeyboardWidget::Key;

// Плёночная клавиатура БК-0010: 16 столбцов по 62–64 точки.
const K kMembrane[] = {
    {   4,   4,   66,  68, Ar2,   0,    false, Blue,   "НР",       "", "" },
    {  68,   4,  130,  68, Ctrl,  0,    false, Pink,   "СУ",       "", "" },
    { 132,   4,  193,  68, Stop,  0,    false, Pink,   "СТОП",     "", "" },
    { 195,   4,  256,  68, Reg,   0000, true,  Pink,   "ШАГ",      "", "" },
    { 258,   4,  320,  68, Reg,   0002, true,  Pink,   "ИНД\nСУ",  "", "" },
    { 322,   4,  384,  68, Reg,   0004, true,  Pink,   "БЛОК\nРЕД", "", "" },
    { 386,   4,  448,  68, Reg,   0005, true,  Pink,   "ГРАФ",     "", "" },
    { 450,   4,  511,  68, Reg,   0006, true,  Pink,   "ЗАП",      "", "" },
    { 513,   4,  574,  68, Reg,   0007, true,  Pink,   "СТИР",     "", "" },
    { 576,   4,  638,  68, Reg,   0015, false, Pink,   "УСТ\nТАБ", "", "" },
    { 640,   4,  702,  68, Reg,   0020, false, Pink,   "СБР\nТАБ", "", "" },
    { 704,   4,  766,  68, Reg,   0003, false, Blue,   "КТ",       "", "" },
    { 768,   4,  829,  68, Reg,   0023, false, Pink,   "ВС",       "", "" },
    { 831,   4,  893,  68, Reg,   0014, false, Pink,   "СБР",      "", "РП" },
    { 896,   4,  956,  68, Reg,   0024, false, Pink,   "ГТ",       "", "" },
    { 958,   4, 1020,  68, Reg,   0013, true,  Yellow, "СБР↦",     "", "" },

    {   4,  70,   66, 134, Reg,   0073, false, Green,  "; +",      "", "32/64" },
    {  68,  70,  130, 134, Reg,   0061, false, Green,  "1 !",      "", "К1" },
    { 132,  70,  193, 134, Reg,   0062, false, Green,  "2 \"",     "", "К2" },
    { 195,  70,  256, 134, Reg,   0063, false, Green,  "3 #",      "", "К3" },
    { 258,  70,  320, 134, Reg,   0064, false, Green,  "4 ¤",      "", "К4" },
    { 322,  70,  384, 134, Reg,   0065, false, Green,  "5 %",      "", "К5" },
    { 386,  70,  448, 134, Reg,   0066, false, Green,  "6 &",      "", "К6" },
    { 450,  70,  511, 134, Reg,   0067, false, Green,  "7 '",      "", "К7" },
    { 513,  70,  574, 134, Reg,   0070, false, Green,  "8 (",      "", "К8" },
    { 576,  70,  638, 134, Reg,   0071, false, Green,  "9 )",      "", "К9" },
    { 640,  70,  702, 134, Reg,   0060, false, Green,  "0",        "", "К10" },
    { 704,  70,  766, 134, Reg,   0055, false, Green,  "- =",      "", "ИНВ Э." },
    { 768,  70,  829, 134, Reg,   0034, false, Yellow, "↖",        "", "" },
    { 831,  70,  893, 134, Reg,   0032, false, Yellow, "↑",        "", "" },
    { 896,  70,  956, 134, Reg,   0035, false, Yellow, "↗",        "", "" },
    { 958,  70, 1020, 134, Reg,   0001, true,  Yellow, "ПОВТ",     "", "" },

    {   4, 136,   66, 200, Reg,   0112, false, Green,  "J",        "Й", "" },
    {  68, 136,  130, 200, Reg,   0103, false, Green,  "C",        "Ц", "" },
    { 132, 136,  193, 200, Reg,   0125, false, Green,  "U",        "У", "" },
    { 195, 136,  256, 200, Reg,   0113, false, Green,  "K",        "К", "" },
    { 258, 136,  320, 200, Reg,   0105, false, Green,  "E",        "Е", "" },
    { 322, 136,  384, 200, Reg,   0116, false, Green,  "N",        "Н", "" },
    { 386, 136,  448, 200, Reg,   0107, false, Green,  "G",        "Г", "" },
    { 450, 136,  511, 200, Reg,   0133, false, Green,  "[",        "Ш", "" },
    { 513, 136,  574, 200, Reg,   0135, false, Green,  "]",        "Щ", "" },
    { 576, 136,  638, 200, Reg,   0132, false, Green,  "Z",        "З", "" },
    { 640, 136,  702, 200, Reg,   0110, false, Green,  "H",        "Х", "" },
    { 704, 136,  766, 200, Reg,   0072, false, Green,  ": *",      "", "КУРСОР" },
    { 768, 136,  829, 200, Reg,   0010, false, Yellow, "←",        "", "" },
    { 831, 136,  893, 200, Reg,   0022, false, Yellow, "⇱",        "", "" },
    { 896, 136,  956, 200, Reg,   0031, false, Yellow, "→",        "", "" },
    { 958, 136, 1020, 200, Reserved, 0, false, Yellow, "",         "", "" },

    {   4, 202,   66, 265, Reg,   0106, false, Green,  "F",        "Ф", "" },
    {  68, 202,  130, 265, Reg,   0131, false, Green,  "Y",        "Ы", "" },
    { 132, 202,  193, 265, Reg,   0127, false, Green,  "W",        "В", "" },
    { 195, 202,  256, 265, Reg,   0101, false, Green,  "A",        "А", "" },
    { 258, 202,  320, 265, Reg,   0120, false, Green,  "P",        "П", "" },
    { 322, 202,  384, 265, Reg,   0122, false, Green,  "R",        "Р", "" },
    { 386, 202,  448, 265, Reg,   0117, false, Green,  "O",        "О", "" },
    { 450, 202,  511, 265, Reg,   0114, false, Green,  "L",        "Л", "" },
    { 513, 202,  574, 265, Reg,   0104, false, Green,  "D",        "Д", "" },
    { 576, 202,  638, 265, Reg,   0126, false, Green,  "V",        "Ж", "" },
    { 640, 202,  702, 265, Reg,   0134, false, Green,  "\\",       "Э", "" },
    { 704, 202,  766, 265, Reg,   0056, false, Green,  ". >",      "", "УСТ.ИНД." },
    { 768, 202,  829, 265, Reg,   0037, false, Yellow, "↙",        "", "" },
    { 831, 202,  893, 265, Reg,   0033, false, Yellow, "↓",        "", "" },
    { 896, 202,  956, 265, Reg,   0036, false, Yellow, "↘",        "", "" },
    { 958, 202, 1020, 265, Reserved, 0, false, Yellow, "",         "", "" },

    {   4, 267,   66, 331, Reg,   0121, false, Green,  "Q",        "Я", "" },
    {  68, 267,  130, 331, Reg,   0136, false, Green,  "¬",        "Ч", "" },
    { 132, 267,  193, 331, Reg,   0123, false, Green,  "S",        "С", "" },
    { 195, 267,  256, 331, Reg,   0115, false, Green,  "M",        "М", "" },
    { 258, 267,  320, 331, Reg,   0111, false, Green,  "I",        "И", "" },
    { 322, 267,  384, 331, Reg,   0124, false, Green,  "T",        "Т", "" },
    { 386, 267,  448, 331, Reg,   0130, false, Green,  "X",        "Ь", "" },
    { 450, 267,  511, 331, Reg,   0102, false, Green,  "B",        "Б", "" },
    { 513, 267,  574, 331, Reg,   0100, false, Green,  "@",        "Ю", "" },
    { 576, 267,  638, 331, Reg,   0054, false, Green,  ", <",      "", "ИНВ.С." },
    { 640, 267,  702, 331, Reg,   0057, false, Green,  "/ ?",      "", "ПОДЧ" },
    { 704, 267,  766, 331, Reg,   0137, false, Green,  "_",        "Ъ", "ЗБ" },
    { 768, 267,  829, 331, Reg,   0026, false, Yellow, "⇤",        "", "" },
    { 831, 267,  893, 331, Reg,   0030, false, Yellow, "↔",        "", "РЕД" },
    { 896, 267,  956, 331, Reg,   0027, false, Yellow, "⇥",        "", "" },
    { 958, 267, 1020, 331, Reserved, 0, false, Yellow, "",         "", "" },

    {   4, 333,   66, 397, Shift, 0,    false, Blue,   "ПР",       "", "" },
    {  68, 333,  130, 397, Caps,  0,    false, Blue,   "ЗАГЛ",     "", "" },
    { 132, 333,  256, 397, Reg,   0016, false, Blue,   "РУС",      "", "" },
    { 258, 333,  511, 397, Reg,   0040, false, Green,  "",         "", "" },
    { 513, 333,  638, 397, Reg,   0017, false, Blue,   "ЛАТ",      "", "" },
    { 640, 333,  702, 397, Lower, 0,    false, Blue,   "СТР",      "", "" },
    { 704, 333,  766, 397, Shift, 0,    false, Blue,   "ПР",       "", "" },
    { 768, 333,  829, 397, Reg,   0011, true,  Pink,   "ТАБ",      "", "" },
    { 831, 333,  956, 397, Reg,   0012, false, Pink,   "ВВОД",     "", "" },
    { 958, 333, 1020, 397, Reserved, 0, false, Yellow, "",         "", "" },
};

// Кнопочная клавиатура БК-0010-01. Надписи — как на колпачках (фото computer-museum.ru):
// кириллица и латиница чёрные, латиница курсивом; «{» под нулём и «}» на «Ъ» — только
// надписи, код клавиши от них не зависит (ПР+0 у ВП1-014 даёт пробел). "\x01" + буква —
// полая стрелка-пиктограмма (drawPictogram).
const K kButtons[] = {
    {   5,   5,  100,  68, Reg,   0001, true,  Dark,   "ПОВТ",     "", "" },
    { 102,   5,  199,  68, Reg,   0003, false, Dark,   "КТ",       "", "" },
    { 201,   5,  297,  68, Reg,   0013, true,  Dark,   "\x01" "B", "", "" },   // =|=>
    { 299,   5,  396,  68, Reg,   0026, false, Dark,   "\x01" "L", "", "" },   // |<==
    { 398,   5,  494,  68, Reg,   0027, false, Dark,   "\x01" "R", "", "" },   // ==>
    { 496,   5,  593,  68, Reg,   0002, true,  Dark,   "ИНД\nСУ",  "", "" },
    { 595,   5,  691,  68, Reg,   0004, true,  Dark,   "БЛОК\nРЕД", "", "" },
    { 693,   5,  789,  68, Reg,   0000, true,  Dark,   "ШАГ",      "", "" },
    { 792,   5,  904,  68, Reg,   0014, false, Dark,   "СБР",      "", "" },
    { 906,   5, 1019,  68, Stop,  0,    false, Red,    "СТОП",     "", "" },

    {   5,  70,   68, 134, Shift, 0,    false, Dark,   "\x01" "s", "", "" },   // ⇩ ПР
    {  70,  70,  134, 134, Reg,   0073, false, Light,  ";\n+",      "", "" },
    { 136,  70,  200, 134, Reg,   0061, false, Light,  "1\n!",      "", "" },
    { 201,  70,  265, 134, Reg,   0062, false, Light,  "2\n″",      "", "" },
    { 267,  70,  331, 134, Reg,   0063, false, Light,  "3\n#",      "", "" },
    { 333,  70,  396, 134, Reg,   0064, false, Light,  "4\n¤",      "", "" },
    { 398,  70,  462, 134, Reg,   0065, false, Light,  "5\n%",      "", "" },
    { 464,  70,  528, 134, Reg,   0066, false, Light,  "6\n&",      "", "" },
    { 530,  70,  593, 134, Reg,   0067, false, Light,  "7\n◢",      "", "" },
    { 595,  70,  659, 134, Reg,   0070, false, Light,  "8\n(",      "", "" },
    { 661,  70,  725, 134, Reg,   0071, false, Light,  "9\n)",      "", "" },
    { 726,  70,  790, 134, Reg,   0060, false, Light,  "0\n{",      "", "" },
    { 792,  70,  856, 134, Reg,   0055, false, Light,  "−\n=",      "", "" },
    { 857,  70,  921, 134, Reg,   0072, false, Light,  ":\n*",      "", "" },
    { 923,  70, 1019, 134, Reg,   0030, false, Dark,   "\x01" "S", "", "" },   // ⟸ ЗАБ

    {   5, 136,  100, 200, Reg,   0011, true,  Dark,   "ТАБ",      "", "" },
    { 102, 136,  166, 200, Reg,   0112, false, Light,  "J",        "Й", "" },
    { 168, 136,  232, 200, Reg,   0103, false, Light,  "C",        "Ц", "" },
    { 234, 136,  297, 200, Reg,   0125, false, Light,  "U",        "У", "" },
    { 299, 136,  363, 200, Reg,   0113, false, Light,  "K",        "К", "" },
    { 365, 136,  429, 200, Reg,   0105, false, Light,  "E",        "Е", "" },
    { 431, 136,  494, 200, Reg,   0116, false, Light,  "N",        "Н", "" },
    { 496, 136,  560, 200, Reg,   0107, false, Light,  "G",        "Г", "" },
    { 562, 136,  626, 200, Reg,   0133, false, Light,  "[",        "Ш", "" },
    { 628, 136,  691, 200, Reg,   0135, false, Light,  "]",        "Щ", "" },
    { 693, 136,  757, 200, Reg,   0132, false, Light,  "Z",        "З", "" },
    { 759, 136,  823, 200, Reg,   0110, false, Light,  "H",        "Х", "" },
    { 825, 136,  888, 200, Reg,   0137, false, Light,  "}",        "Ъ", "" },
    { 890, 136,  954, 200, Reg,   0057, false, Light,  "/\n?",      "", "" },
    { 956, 136, 1019, 200, Reg,   0023, false, Dark,   "ВС",       "", "" },

    {   5, 202,  117, 265, Ctrl,  0,    false, Dark,   "СУ",       "", "" },
    { 119, 202,  183, 265, Reg,   0106, false, Light,  "F",        "Ф", "" },
    { 184, 202,  248, 265, Reg,   0131, false, Light,  "Y",        "Ы", "" },
    { 250, 202,  314, 265, Reg,   0127, false, Light,  "W",        "В", "" },
    { 316, 202,  380, 265, Reg,   0101, false, Light,  "A",        "А", "" },
    { 382, 202,  445, 265, Reg,   0120, false, Light,  "P",        "П", "" },
    { 447, 202,  511, 265, Reg,   0122, false, Light,  "R",        "Р", "" },
    { 513, 202,  577, 265, Reg,   0117, false, Light,  "O",        "О", "" },
    { 579, 202,  642, 265, Reg,   0114, false, Light,  "L",        "Л", "" },
    { 644, 202,  708, 265, Reg,   0104, false, Light,  "D",        "Д", "" },
    { 710, 202,  773, 265, Reg,   0126, false, Light,  "V",        "Ж", "" },
    { 775, 202,  839, 265, Reg,   0134, false, Light,  "\\",       "Э", "" },
    { 841, 202,  905, 265, Reg,   0056, false, Light,  ".\n>",      "", "" },
    { 906, 202, 1019, 265, Reg,   0012, false, Dark,   "\x01" "E", "", "" },   // ВВОД

    {   5, 267,   85, 331, Caps,  0,    false, Dark,   "ЗАГЛ",     "", "" },
    {  87, 267,  150, 331, Lower, 0,    false, Dark,   "СТР",      "", "" },
    { 153, 267,  217, 331, Reg,   0121, false, Light,  "Q",        "Я", "" },
    { 218, 267,  282, 331, Reg,   0136, false, Light,  "¬",        "Ч", "" },
    { 284, 267,  348, 331, Reg,   0123, false, Light,  "S",        "С", "" },
    { 350, 267,  413, 331, Reg,   0115, false, Light,  "M",        "М", "" },
    { 415, 267,  479, 331, Reg,   0111, false, Light,  "I",        "И", "" },
    { 481, 267,  544, 331, Reg,   0124, false, Light,  "T",        "Т", "" },
    { 546, 267,  610, 331, Reg,   0130, false, Light,  "X",        "Ь", "" },
    { 612, 267,  676, 331, Reg,   0102, false, Light,  "B",        "Б", "" },
    { 678, 267,  741, 331, Reg,   0100, false, Light,  "@",        "Ю", "" },
    { 743, 267,  823, 331, Reg,   0054, false, Light,  ",\n<",      "", "" },
    { 824, 267,  888, 396, Reg,   0010, false, Dark,   "\x01" "l", "", "" },
    { 890, 267,  954, 331, Reg,   0032, false, Dark,   "\x01" "u", "", "" },
    { 956, 267, 1019, 396, Reg,   0031, false, Dark,   "\x01" "r", "", "" },

    {   5, 333,  134, 396, Reg,   0016, false, Dark,   "РУС",      "", "" },
    { 136, 333,  216, 396, Ar2,   0,    false, Dark,   "АР2",      "", "" },
    { 218, 333,  691, 396, Reg,   0040, false, Light,  "",         "", "" },
    { 693, 333,  822, 396, Reg,   0017, false, Dark,   "ЛАТ",      "", "" },
    { 890, 333,  954, 396, Reg,   0033, false, Dark,   "\x01" "d", "", "" },
};

QColor face(uint8_t c) {
    switch (c) {
    case Green:  return QColor(0xA4, 0xF7, 0xCF);
    case Pink:   return QColor(0xF5, 0x9C, 0x9C);
    case Blue:   return QColor(0x9F, 0xA8, 0xF5);
    case Yellow: return QColor(0xFA, 0xF7, 0x9E);
    case Dark:   return QColor(0x96, 0x96, 0x98);
    case Light:  return QColor(0xF0, 0xED, 0xE4);
    default:     return QColor(0xD2, 0x22, 0x22);
    }
}

// Полая стрелка: along — вдоль (0 — хвост, 1 — остриё), across — поперёк (±0.5 — край рамки).
QPolygonF arrowShape(const QRectF& b, char dir, double st, double hh, double hl) {
    const QPointF k[] = {{0, -st}, {1 - hl, -st}, {1 - hl, -hh}, {1, 0}, {1 - hl, hh}, {1 - hl, st}, {0, st}};
    QPolygonF poly;
    for (const QPointF& q : k) {
        const double a = q.x(), c = q.y();
        switch (dir) {
        case 'r': poly << QPointF(b.left() + a * b.width(), b.center().y() + c * b.height()); break;
        case 'l': poly << QPointF(b.right() - a * b.width(), b.center().y() + c * b.height()); break;
        case 'd': poly << QPointF(b.center().x() + c * b.width(), b.top() + a * b.height()); break;
        default:  poly << QPointF(b.center().x() + c * b.width(), b.bottom() - a * b.height()); break;
        }
    }
    return poly;
}

// Пиктограммы кнопочной клавиатуры: стрелки курсора и ПР, ЗАБ, ВВОД, =|=>, |<==, ==>.
void drawPictogram(QPainter& p, const QRectF& in, char what, double s) {
    p.setBrush(Qt::NoBrush);
    auto box = [&](double w, double h) {
        return QRectF(in.center().x() - w / 2, in.center().y() - h / 2, w, h);
    };
    switch (what) {
    case 'l': case 'r': p.drawPolygon(arrowShape(box(std::min(in.width() * 0.7, 46 * s), 30 * s), what, 0.18, 0.5, 0.42)); break;
    case 'u': case 'd': case 's':
        p.drawPolygon(arrowShape(box(30 * s, std::min(in.height() * 0.8, 40 * s)), what == 's' ? 'd' : what, 0.18, 0.5, 0.42));
        break;
    case 'S': p.drawPolygon(arrowShape(box(in.width() * 0.62, 26 * s), 'l', 0.17, 0.5, 0.28)); break;
    case 'R': p.drawPolygon(arrowShape(box(in.width() * 0.7, 26 * s), 'r', 0.17, 0.5, 0.28)); break;
    case 'B': {                                        // =|=>: черта поперёк стержня
        const QRectF b = box(in.width() * 0.7, 26 * s);
        p.drawPolygon(arrowShape(b, 'r', 0.17, 0.5, 0.36));
        const double x = b.left() + b.width() * 0.28;
        p.drawRect(QRectF(x - 2.5 * s, b.top() + b.height() * 0.08, 5 * s, b.height() * 0.84));
        break;
    }
    case 'L': {                                        // |<==: черта у острия
        const QRectF b = box(in.width() * 0.7, 26 * s);
        p.drawLine(QPointF(b.left(), b.top()), QPointF(b.left(), b.bottom()));
        p.drawPolygon(arrowShape(b.adjusted(4 * s, 0, 0, 0), 'l', 0.17, 0.5, 0.36));
        break;
    }
    case 'E': {                                        // ВВОД: стрелка влево с крюком вверх
        const QRectF b = box(in.width() * 0.6, in.height() * 0.62);
        const double W = b.width(), H = b.height(), y = b.top() + H * 0.66;
        const double st = H * 0.13, hh = H * 0.32, hl = W * 0.28, sw = W * 0.13;
        QPolygonF poly;
        poly << QPointF(b.left(), y) << QPointF(b.left() + hl, y - hh) << QPointF(b.left() + hl, y - st)
             << QPointF(b.right() - sw, y - st) << QPointF(b.right() - sw, b.top())
             << QPointF(b.right(), b.top()) << QPointF(b.right(), y + st)
             << QPointF(b.left() + hl, y + st) << QPointF(b.left() + hl, y + hh);
        p.drawPolygon(poly);
        break;
    }
    default: break;
    }
}
}

BkKeyboardWidget::BkKeyboardWidget(QWidget* parent) : QWidget(parent) {
    setKind(Kind::Buttons);
    setMinimumSize(512, 200);
    setMouseTracking(false);
    QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
}

void BkKeyboardWidget::setKind(Kind k) {
    kind_ = k;
    keys_ = k == Kind::Membrane ? kMembrane : kButtons;
    nKeys_ = k == Kind::Membrane ? int(std::size(kMembrane)) : int(std::size(kButtons));
    pressed_ = -1;
    ctrl_ = ar2_ = shift_ = false;
    update();
}

QRectF BkKeyboardWidget::keyRect(const Key& k) const {
    const double s = std::min(width() / 1024.0, height() / 401.0);
    const double ox = (width() - 1024 * s) / 2, oy = (height() - 401 * s) / 2;
    return QRectF(ox + k.x1 * s, oy + k.y1 * s, (k.x2 - k.x1) * s, (k.y2 - k.y1) * s);
}

int BkKeyboardWidget::keyAt(const QPointF& p) const {
    for (int i = 0; i < nKeys_; ++i)
        if (keyRect(keys_[i]).contains(p)) return i;
    return -1;
}

uint16_t BkKeyboardWidget::translate(const Key& k) const {
    uint8_t c = k.code;
    if (c >= 0100 && c <= 0137) {
        if (ctrl_) {
            c &= 037;
        } else {
            // КОИ-7: заглавная латиница и строчная кириллица — одни коды (0100–0137).
            const bool capital = caps_ != shift_;
            const bool rus = isRussian && isRussian();
            if (rus ? capital : !capital) c = static_cast<uint8_t>(c + 040);
        }
    } else if (shift_) {
        if (c >= 054 && c <= 057) c = static_cast<uint8_t>(c + 020);
        else if (c >= 060 && c <= 073) c = static_cast<uint8_t>(c - 020);
    }
    return (k.v274 || ar2_) ? static_cast<uint16_t>(c | 0200) : c;
}

void BkKeyboardWidget::mousePressEvent(QMouseEvent* e) {
    const int i = keyAt(e->position());
    if (i < 0 || e->button() != Qt::LeftButton) return;
    const Key& k = keys_[i];
    switch (k.type) {
    case Ctrl:  ctrl_ = !ctrl_; break;
    case Ar2:   ar2_ = !ar2_; break;
    case Shift: shift_ = !shift_; break;
    case Caps:  caps_ = true; break;
    case Lower: caps_ = false; break;
    case Stop:  pressed_ = i; emit stopPressed(); break;
    case Reg:
        pressed_ = i;
        emit keyDown(translate(k));
        if (k.code == 016 || k.code == 017) emit registerSwitched(k.code == 016);
        break;
    default: break;
    }
    update();
}

void BkKeyboardWidget::mouseReleaseEvent(QMouseEvent*) {
    if (pressed_ < 0) return;
    if (keys_[pressed_].type == Reg) {
        emit keyUp();
        ctrl_ = ar2_ = shift_ = false;    // модификатор действует на одну клавишу
    }
    pressed_ = -1;
    update();
}

void BkKeyboardWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), kind_ == Kind::Membrane ? QColor(0x80, 0x80, 0x80) : QColor(0x2E, 0x2E, 0x30));
    const double s = std::min(width() / 1024.0, height() / 401.0);
    QFont big = font(), small = font();
    big.setPixelSize(std::max(7, int(17 * s)));
    small.setPixelSize(std::max(6, int(11 * s)));
    for (int i = 0; i < nKeys_; ++i) {
        const Key& k = keys_[i];
        const QRectF r = keyRect(k).adjusted(1, 1, -1, -1);
        const bool on = i == pressed_ || (k.type == Ctrl && ctrl_) || (k.type == Ar2 && ar2_)
                        || (k.type == Shift && shift_) || (k.type == Caps && caps_) || (k.type == Lower && !caps_);
        QColor f = face(k.color);
        if (on) f = f.darker(135);
        QPainterPath path;
        path.addRoundedRect(r, 5 * s, 5 * s);
        p.fillPath(path, f);
        p.setPen(QPen(on ? QColor(0xFF, 0xE0, 0x40) : f.darker(150), on ? 2.5 : 1));
        p.drawPath(path);
        const QColor ink(0x10, 0x10, 0x10);
        const QString lab = QString::fromUtf8(k.lab), cyr = QString::fromUtf8(k.cyr), sub = QString::fromUtf8(k.sub);
        const QRectF in = r.adjusted(4 * s, 3 * s, -4 * s, -3 * s);
        p.setFont(big);
        if (!cyr.isEmpty()) {
            if (kind_ == Kind::Membrane) {
                p.setPen(ink);
                p.drawText(in, Qt::AlignHCenter | Qt::AlignTop, cyr + "  " + lab);
            } else {
                p.setPen(ink);
                p.drawText(in, Qt::AlignLeft | Qt::AlignTop, cyr);
                QFont it = big;
                it.setItalic(true);
                p.setFont(it);
                p.drawText(in, Qt::AlignHCenter | Qt::AlignBottom, lab);
            }
        } else if (lab.size() == 2 && lab[0] == QChar(1)) {
            p.setPen(QPen(ink, std::max(1.2, 1.7 * s)));
            drawPictogram(p, in, static_cast<char>(lab[1].unicode()), s);
        } else if (kind_ == Kind::Buttons && lab.size() <= 3 && lab.contains('\n')) {
            // Двузначные клавиши кнопочной клавиатуры: второй знак — под первым, как на колпачке.
            p.setPen(ink);
            p.setFont(big);
            const QStringList two = lab.split('\n');
            p.drawText(in, Qt::AlignTop | Qt::AlignHCenter, two[0]);
            p.drawText(in, Qt::AlignBottom | Qt::AlignHCenter, two[1]);
        } else {
            p.setPen(ink);
            // На колпачках кнопочной клавиатуры и двухстрочные надписи крупные.
            p.setFont(kind_ == Kind::Buttons || (!lab.contains('\n') && lab.size() <= 4) ? big : small);
            p.drawText(in, (sub.isEmpty() ? Qt::AlignVCenter : Qt::AlignTop) | Qt::AlignHCenter, lab);
        }
        if (!sub.isEmpty()) {
            p.setFont(small);
            p.setPen(ink);
            p.drawText(in, Qt::AlignBottom | Qt::AlignHCenter, sub);
        }
    }
}
