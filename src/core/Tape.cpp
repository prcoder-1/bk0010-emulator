#include "Tape.h"
#include "ScreenOcr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace bk {

// ---------------------------------------------------------------------------
// WAV
// ---------------------------------------------------------------------------
namespace {
uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }
}

bool readWavMono(const std::string& path, int& rate, std::vector<float>& out, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "не открывается " + path; return false; }
    std::vector<uint8_t> b;
    uint8_t buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0; ) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) {
        err = "не WAV (нет RIFF/WAVE)";
        return false;
    }
    unsigned fmt = 0, ch = 0, bits = 0, align = 0;
    rate = 0;
    const uint8_t* data = nullptr;
    size_t dataLen = 0;
    for (size_t at = 12; at + 8 <= b.size(); ) {
        const uint32_t len = le32(b.data() + at + 4);
        const uint8_t* body = b.data() + at + 8;
        const size_t avail = b.size() - (at + 8);
        if (!std::memcmp(b.data() + at, "fmt ", 4) && avail >= 16) {
            fmt = le16(body); ch = le16(body + 2); rate = static_cast<int>(le32(body + 4));
            align = le16(body + 12); bits = le16(body + 14);
            if (fmt == 0xFFFE && len >= 26 && avail >= 26) fmt = le16(body + 24);   // подформат EXTENSIBLE
        } else if (!std::memcmp(b.data() + at, "data", 4)) {
            data = body;
            dataLen = std::min<size_t>(len, avail);   // потоковые записи пишут длину 0 или ~0
            if (len == 0) dataLen = avail;
            break;
        }
        at += 8 + len + (len & 1);
    }
    if (!data || !rate || !ch || !align) { err = "в WAV нет fmt или data"; return false; }
    const unsigned bps = bits / 8;
    const bool isFloat = fmt == 3;
    if (!(fmt == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32))
        && !(isFloat && (bits == 32 || bits == 64))) {
        err = "формат WAV не поддерживается (нужен PCM 8/16/24/32 или float)";
        return false;
    }
    const size_t frames = dataLen / align;
    out.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        const uint8_t* fr = data + i * align;
        double sum = 0;
        for (unsigned c = 0; c < ch; ++c) {
            const uint8_t* s = fr + c * bps;
            double v;
            if (isFloat) {
                if (bits == 32) { float x; std::memcpy(&x, s, 4); v = x; }
                else { double x; std::memcpy(&x, s, 8); v = x; }
            } else if (bits == 8) {
                v = (s[0] - 128) / 128.0;
            } else if (bits == 16) {
                v = static_cast<int16_t>(le16(s)) / 32768.0;
            } else if (bits == 24) {
                const int32_t x = static_cast<int32_t>((s[0] << 8) | (s[1] << 16) | (uint32_t(s[2]) << 24)) >> 8;
                v = x / 8388608.0;
            } else {
                v = static_cast<int32_t>(le32(s)) / 2147483648.0;
            }
            sum += v;
        }
        out[i] = static_cast<float>(sum / ch);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Компаратор входа: порог посередине между огибающими максимума и минимума (они
// следуют за сигналом с постоянной 10 мс — так уходит постоянная составляющая и
// медленный дрейф уровня записи), гистерезис 10% размаха. Тишина (размах меньше
// 2% шкалы) уровень не меняет.
// ---------------------------------------------------------------------------
void tapeBinarize(const std::vector<float>& x, int rate, std::vector<uint8_t>& bits) {
    bits.assign(x.size(), 0);
    if (x.empty() || rate <= 0) return;
    const float k = static_cast<float>(1.0 - std::exp(-1.0 / (rate * 0.010)));
    float hi = x[0], lo = x[0];
    uint8_t state = 0;
    for (size_t n = 0; n < x.size(); ++n) {
        const float v = x[n];
        hi = v > hi ? v : hi + (v - hi) * k;
        lo = v < lo ? v : lo + (v - lo) * k;
        const float range = hi - lo;
        if (range > 0.02f) {
            const float mid = (hi + lo) * 0.5f, h = range * 0.1f;
            if (!state && v > mid + h) state = 1;
            else if (state && v < mid - h) state = 0;
        }
        bits[n] = state;
    }
}

// ---------------------------------------------------------------------------
// Декодер
// ---------------------------------------------------------------------------
uint16_t tapeChecksum(const uint8_t* d, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i < n; ++i) {
        s += d[i];
        if (s > 0xFFFF) s = (s & 0xFFFF) + 1;
    }
    return static_cast<uint16_t>(s);
}

namespace {

// Отрезки одного уровня: длина в сэмплах и начало.
struct Runs {
    std::vector<uint32_t> len, at;
    uint8_t lvl0 = 0;
    size_t n() const { return len.size(); }
    uint8_t level(size_t i) const { return static_cast<uint8_t>(lvl0 ^ (i & 1)); }
    uint32_t sample(size_t i) const { return i < at.size() ? at[i] : (at.empty() ? 0 : at.back() + len.back()); }
};

Runs makeRuns(const std::vector<uint8_t>& bits) {
    Runs r;
    if (bits.empty()) return r;
    r.lvl0 = bits[0] ? 1 : 0;
    uint32_t start = 0;
    for (uint32_t i = 1; i <= bits.size(); ++i) {
        if (i == bits.size() || bits[i] != bits[i - 1]) {
            r.at.push_back(start);
            r.len.push_back(i - start);
            start = i;
        }
    }
    return r;
}

constexpr int kPilotMin = 256;      // периодов пилот-тона (у ПЗУ — 04000 + 0200)
constexpr int kMarkerScan = 64;     // отрезков от конца пилот-тона до маркера
constexpr int kGapMax = 8192;       // отрезков короткого пилот-тона перед маркером блока

// Пилот-тон: не меньше kPilotMin периодов подряд, каждый в пределах ±25% среднего.
bool findPilot(const Runs& r, size_t from, size_t& s, size_t& e, double& L) {
    size_t k = from;
    while (k + 1 < r.n()) {
        const size_t st = k;
        double sum = 0;
        int cnt = 0;
        while (k + 1 < r.n()) {
            const double p = r.len[k] + r.len[k + 1];
            if (cnt) {
                const double avg = sum / cnt;
                if (std::fabs(p - avg) > (cnt < 8 ? 0.4 : 0.25) * avg) break;
            }
            sum += p; ++cnt; k += 2;
        }
        if (cnt >= kPilotMin && sum / cnt >= 3.0) { s = st; e = k; L = sum / cnt; return true; }
        if (k == st) ++k;
    }
    return false;
}

// Чтение стандартного файла — так, как его читает драйвер ПЗУ. Фаза P — уровень,
// который драйвер считает «высоким» (ячейка 300), порог thr = 1,5 периода пилот-тона
// (ячейка 314): полупериод длиннее — маркер, вдвое длиннее порога — сбой.
struct StdReader {
    const Runs& r;
    double thr = 0;
    uint8_t P = 1;
    size_t c = 0;

    // 117376: дождаться конца текущего периода и измерить следующий.
    bool period(double& p) {
        if (c < r.n() && r.level(c) == P) ++c;
        if (c < r.n() && r.level(c) != P) ++c;
        if (c + 1 >= r.n()) return false;
        p = r.len[c] + r.len[c + 1];
        c += 2;
        return true;
    }
    // 117260: пропустить короткий пилот-тон до полупериода-маркера.
    bool marker() {
        for (int g = 0; c < r.n() && g < kGapMax; ++c) {
            if (r.level(c) != P) continue;
            if (r.len[c] >= thr) {
                if (r.len[c] > 2 * thr) return false;
                ++c;
                return true;
            }
            ++g;
        }
        return false;
    }
    bool byte(uint8_t& b) {
        b = 0;
        for (int i = 0; i < 8; ++i) {
            double p;
            if (!period(p) || p > 1.85 * thr) return false;   // маркер вместо бита — запись кончилась
            if (p > thr) b |= static_cast<uint8_t>(1 << i);
        }
        return true;
    }
};

std::string nameUtf8(const uint8_t raw[16]) {
    int n = 16;
    while (n > 0 && (raw[n - 1] == ' ' || raw[n - 1] == 0)) --n;
    std::string s;
    for (int i = 0; i < n; ++i) {
        const uint8_t c = raw[i];
        if (c < 040 || (c >= 0200 && c < 0240)) s += '?';
        else s += bkScreenCodeToUtf8(c);
    }
    return s;
}

uint16_t wordAt(const std::vector<uint8_t>& d, size_t i) {
    return 2 * i + 1 < d.size() ? static_cast<uint16_t>(d[2 * i] | (d[2 * i + 1] << 8)) : 0;
}

// Загрузчик Maxim (maxim-loader.mac, вставляется конвертером thesands.ru): после
// восьми слов автозапуска — MOV #адрес,R1 / MOV #длина+2,R0 / MOV R1,@#322 / MOV R0,@#324.
bool isMaximLoader(const TapeFile& f) {
    const auto& d = f.data;
    return f.addr < 01000 && wordAt(d, 8) == 012701 && wordAt(d, 10) == 012700
        && wordAt(d, 12) == 010137 && wordAt(d, 13) == 000322
        && wordAt(d, 14) == 010037 && wordAt(d, 15) == 000324;
}

// Адрес пуска загрузчика: CMP R0,-(R1) / BNE ERROR / JMP @#адрес.
uint16_t maximEntry(const TapeFile& f) {
    for (size_t i = 16; i + 3 < f.data.size() / 2; ++i)
        if (wordAt(f.data, i) == 020041 && wordAt(f.data, i + 1) == 001002 && wordAt(f.data, i + 2) == 000137)
            return wordAt(f.data, i + 3);
    return wordAt(f.data, 9);
}

// Турбо-поток Maxim: загрузчик меряет только «высокий» уровень. Пилот — 1024 периода
// с одинаковым высоким полупериодом H, маркер — высокий ≈ 4H, бит — высокий короче
// 2/3 H — «0», длиннее — «1» (thesands: 0 = 1 сэмпл, 1 = 3 сэмпла при H = 3 на 40 кГц).
bool decodeMaxim(const Runs& r, size_t from, const TapeFile& loader, int rate, TapeFile& out, size_t& end) {
    const uint16_t addr = wordAt(loader.data, 9), size = wordAt(loader.data, 11);
    if (size < 2) return false;
    size_t k = from;
    if (k < r.n() && r.level(k) != 1) ++k;
    double H = 0;
    size_t pilotStart = 0;
    for (; k + 1 < r.n(); ) {
        const size_t st = k;
        double sum = 0;
        int cnt = 0;
        while (k < r.n()) {
            const double h = r.len[k];
            if (cnt && std::fabs(h - sum / cnt) > 0.35 * (sum / cnt)) break;
            sum += h; ++cnt; k += 2;
        }
        if (cnt >= 128) { H = sum / cnt; pilotStart = st; break; }
        if (k == st) k += 2;
        if (k > from + 200000) return false;   // турбо-поток идёт сразу за загрузчиком
    }
    if (H <= 0) return false;
    // Маркер: высокий полупериод ≈ 4H (загрузчик: |h − 4·ср| ≤ 6 отсчётов).
    bool found = false;
    for (int g = 0; k < r.n() && g < kMarkerScan; k += 2, ++g) {
        if (r.len[k] >= 2.5 * H && r.len[k] <= 6 * H) { found = true; k += 2; break; }
    }
    if (!found) return false;
    out = TapeFile{};
    out.kind = TapeFile::Kind::MaximTurbo;
    std::memcpy(out.rawName, loader.rawName, 16);
    out.name = loader.name;
    out.addr = addr;
    out.len = static_cast<uint16_t>(size - 2);
    out.entry = maximEntry(loader);
    out.start = r.sample(pilotStart);
    out.periodUs = 2 * H * 1e6 / rate;
    std::vector<uint8_t> bytes;
    bytes.reserve(size);
    const double thr = H * 2.0 / 3.0;
    while (bytes.size() < size && k < r.n()) {
        uint8_t b = 0;
        int i = 0;
        for (; i < 8 && k < r.n(); ++i, k += 2) {
            if (r.len[k] > 3 * H) break;        // не бит — поток оборвался
            if (r.len[k] > thr) b |= static_cast<uint8_t>(1 << i);
        }
        if (i < 8) break;
        bytes.push_back(b);
    }
    out.complete = bytes.size() == size;
    out.data.assign(bytes.begin(), bytes.begin() + std::min<size_t>(bytes.size(), out.len));
    if (out.complete) out.crcTape = static_cast<uint16_t>(bytes[size - 2] | (bytes[size - 1] << 8));
    out.crcCalc = tapeChecksum(out.data.data(), out.data.size());
    end = k;
    out.end = r.sample(std::min(k, r.n()));
    return true;
}

} // namespace

std::vector<TapeFile> tapeDecode(const std::vector<uint8_t>& bits, int rate) {
    std::vector<TapeFile> files;
    const Runs r = makeRuns(bits);
    size_t from = 0;
    while (from < r.n()) {
        size_t s = 0, e = 0;
        double L = 0;
        if (!findPilot(r, from, s, e, L)) break;
        StdReader rd{r, 1.5 * L, 1, 0};
        // 117012: первый полупериод длиннее порога — маркер; его уровень задаёт фазу.
        size_t j = e >= 2 ? e - 2 : e;
        bool marker = false;
        for (int g = 0; j < r.n() && g < kMarkerScan; ++j, ++g) {
            if (r.len[j] < rd.thr) continue;
            marker = r.len[j] <= 2 * rd.thr;
            break;
        }
        if (!marker) { from = std::max(e, j + 1); continue; }
        rd.P = r.level(j);
        rd.c = j + 1;
        double p;
        TapeFile f;
        f.start = r.sample(s);
        f.inverted = rd.P == 0;
        f.periodUs = L * 1e6 / rate;
        uint8_t hdr[20];
        bool ok = rd.period(p) && rd.marker() && rd.period(p);   // «1», короткий пилот, маркер, «1»
        for (int i = 0; ok && i < 20; ++i) ok = rd.byte(hdr[i]);
        if (!ok) { from = std::max(e, j + 1); continue; }
        f.addr = static_cast<uint16_t>(hdr[0] | (hdr[1] << 8));
        f.len = static_cast<uint16_t>(hdr[2] | (hdr[3] << 8));
        std::memcpy(f.rawName, hdr + 4, 16);
        f.name = nameUtf8(f.rawName);
        f.data.reserve(f.len);
        ok = rd.marker() && rd.period(p);
        for (uint32_t i = 0; ok && i < f.len; ++i) {
            uint8_t b;
            if ((ok = rd.byte(b))) f.data.push_back(b);
        }
        uint8_t cs[2];
        if (ok && rd.byte(cs[0]) && rd.byte(cs[1])) {
            f.complete = true;
            f.crcTape = static_cast<uint16_t>(cs[0] | (cs[1] << 8));
        }
        f.crcCalc = tapeChecksum(f.data.data(), f.data.size());
        f.end = r.sample(std::min(rd.c, r.n()));
        from = std::max(rd.c, e);
        f.maximLoader = f.crcOk() && isMaximLoader(f);
        files.push_back(std::move(f));
        if (files.back().maximLoader) {
            TapeFile t;
            size_t end = 0;
            if (decodeMaxim(r, from, files.back(), rate, t, end)) {
                files.push_back(std::move(t));
                from = end;
            }
        }
    }
    return files;
}

int TapeImage::mainProgram() const {
    for (size_t i = 0; i < files.size(); ++i)
        if (files[i].kind == TapeFile::Kind::MaximTurbo && files[i].crcOk()) return static_cast<int>(i);
    for (size_t i = 0; i < files.size(); ++i)
        if (files[i].crcOk()) return static_cast<int>(i);
    for (size_t i = 0; i < files.size(); ++i)
        if (!files[i].data.empty()) return static_cast<int>(i);
    return -1;
}

std::shared_ptr<const TapeImage> makeTapeImage(const std::vector<float>& x, int rate, const std::string& path) {
    auto img = std::make_shared<TapeImage>();
    img->path = path;
    img->rate = rate;
    tapeBinarize(x, rate, img->bits);
    img->files = tapeDecode(img->bits, rate);
    return img;
}

std::shared_ptr<const TapeImage> loadTapeImage(const std::string& path, std::string& err) {
    static std::shared_ptr<const TapeImage> cache;
    static long long cacheMtime = 0, cacheSize = -1;
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) { err = "нет файла " + path; return nullptr; }
    if (cache && cache->path == path && cacheMtime == static_cast<long long>(st.st_mtime)
        && cacheSize == static_cast<long long>(st.st_size))
        return cache;
    int rate = 0;
    std::vector<float> x;
    if (!readWavMono(path, rate, x, err)) return nullptr;
    cache = makeTapeImage(x, rate, path);
    cacheMtime = static_cast<long long>(st.st_mtime);
    cacheSize = static_cast<long long>(st.st_size);
    return cache;
}

bool parseTapeSpec(const std::string& spec, std::string& path, int& index) {
    auto isWav = [](const std::string& p) {
        if (p.size() < 4) return false;
        std::string e = p.substr(p.size() - 4);
        for (char& ch : e) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return e == ".wav";
    };
    index = 0;
    path = spec;
    const size_t h = spec.rfind('#');
    if (h != std::string::npos && h + 1 < spec.size()
        && spec.find_first_not_of("0123456789", h + 1) == std::string::npos && isWav(spec.substr(0, h))) {
        path = spec.substr(0, h);
        index = std::atoi(spec.c_str() + h + 1);
        return index > 0;
    }
    return isWav(spec);
}

std::string tapeHostName(const TapeFile& f, int index) {
    std::string s;
    for (char ch : f.name) {
        const unsigned char u = static_cast<unsigned char>(ch);
        s += (u < 040 || std::strchr("/\\:*?\"<>|", ch)) ? '_' : ch;
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    if (s.empty()) s = "FILE" + std::to_string(index);
    return s;
}

std::string tapeCatalog(const TapeImage& img) {
    std::string out;
    char line[256];
    std::snprintf(line, sizeof line, "%s: %.1f с, %d Гц, файлов: %zu\n", img.path.c_str(), img.seconds(),
                  img.rate, img.files.size());
    out += line;
    const int main = img.mainProgram();
    for (size_t i = 0; i < img.files.size(); ++i) {
        const TapeFile& f = img.files[i];
        const double t = img.rate ? double(f.start) / img.rate : 0;
        std::string kind;
        if (f.kind == TapeFile::Kind::MaximTurbo) {
            char e[48];
            std::snprintf(e, sizeof e, "турбо Maxim, пуск %06o", f.entry);
            kind = e;
        } else {
            kind = f.maximLoader ? "загрузчик Maxim" : "стандартная";
            if (f.inverted) kind += ", инверсная фаза";
        }
        const char* crc = !f.complete ? "оборван" : f.crcOk() ? "верна" : "НЕВЕРНА";
        std::snprintf(line, sizeof line, "%c%2zu  %-16s  %06o  %06o  %-7s  %02d:%04.1f  период %.0f мкс, %s\n",
                      int(i) == main ? '*' : ' ', i + 1, f.name.c_str(), f.addr, f.len, crc,
                      int(t) / 60, t - 60 * (int(t) / 60), f.periodUs, kind.c_str());
        out += line;
    }
    if (img.files.empty()) out += "  записей БК не найдено\n";
    else if (main >= 0) out += "  * — программа, которую грузит открытие .wav\n";
    return out;
}

bool saveTapeFileBin(const TapeFile& f, const std::string& path) {
    FILE* o = std::fopen(path.c_str(), "wb");
    if (!o) return false;
    const uint16_t len = static_cast<uint16_t>(f.data.size());
    const uint8_t hdr[4] = {static_cast<uint8_t>(f.addr & 0377), static_cast<uint8_t>(f.addr >> 8),
                            static_cast<uint8_t>(len & 0377), static_cast<uint8_t>(len >> 8)};
    bool ok = std::fwrite(hdr, 1, 4, o) == 4
              && (f.data.empty() || std::fwrite(f.data.data(), 1, f.data.size(), o) == f.data.size());
    ok = (std::fclose(o) == 0) && ok;
    return ok;
}

// ---------------------------------------------------------------------------
// Магнитофон
// ---------------------------------------------------------------------------
void Tape::insert(std::shared_ptr<const TapeImage> img, uint64_t now) {
    img_ = std::move(img);
    pos_ = 0;
    anchorTick_ = now;
    playing_ = auto_ = false;
}

void Tape::eject() {
    img_.reset();
    pos_ = 0;
    playing_ = auto_ = false;
}

double Tape::posAt(uint64_t now) const {
    if (!img_) return 0;
    const double len = static_cast<double>(img_->bits.size());
    if (!playing_ || (remote_ && !motor_) || now < anchorTick_) return std::min(pos_, len);
    return std::min(len, pos_ + static_cast<double>(now - anchorTick_) * img_->rate / cpuHz_);
}

void Tape::anchor(uint64_t now) {
    pos_ = posAt(now);
    anchorTick_ = now;
}

void Tape::setClock(int cpuHz, uint64_t now) {
    anchor(now);
    if (cpuHz > 0) cpuHz_ = cpuHz;
}

void Tape::rebase(uint64_t now) {
    anchor(now);
    anchorTick_ = 0;
}

bool Tape::level(uint64_t now) const {
    if (!img_ || img_->bits.empty()) return false;
    size_t p = static_cast<size_t>(posAt(now));
    if (p >= img_->bits.size()) p = img_->bits.size() - 1;
    return (img_->bits[p] != 0) != invert_;
}

void Tape::setMotor(bool on, uint64_t now) {
    if (on == motor_) return;
    anchor(now);
    motor_ = on;
    // Автопуск ведёт себя как ДУ: драйвер выключил двигатель — лента встаёт до
    // следующего чтения. Кроме загрузчика Maxim — он читает турбо-поток сразу
    // после своего файла, не включая двигатель.
    if (!on && auto_ && playing_) {
        const int f = fileAt(static_cast<uint64_t>(pos_));
        const bool turbo = f >= 0 && (img_->files[f].maximLoader
                                      || img_->files[f].kind == TapeFile::Kind::MaximTurbo);
        if (!turbo) playing_ = auto_ = false;
    }
}

void Tape::noteRead(uint64_t now, bool tapeDriver) {
    if (!autoStart_ || !tapeDriver || !img_ || playing_) return;
    anchor(now);
    if (pos_ >= static_cast<double>(img_->bits.size())) return;
    playing_ = auto_ = true;
}

void Tape::poll(uint64_t now) {
    if (playing_ && img_ && posAt(now) >= static_cast<double>(img_->bits.size())) stop(now);
}

void Tape::play(uint64_t now) {
    anchor(now);
    playing_ = img_ != nullptr;
    auto_ = false;
}

void Tape::stop(uint64_t now) {
    anchor(now);
    playing_ = auto_ = false;
}

void Tape::seek(uint64_t sample, uint64_t now) {
    anchor(now);
    pos_ = static_cast<double>(std::min<uint64_t>(sample, length()));
}

uint64_t Tape::position(uint64_t now) const { return static_cast<uint64_t>(posAt(now)); }

double Tape::seconds(uint64_t now) const { return img_ && img_->rate ? posAt(now) / img_->rate : 0.0; }

bool Tape::moving(uint64_t now) const {
    return img_ && playing_ && (!remote_ || motor_) && posAt(now) < static_cast<double>(img_->bits.size());
}

void Tape::setRemote(bool on, uint64_t now) {
    anchor(now);
    remote_ = on;
}

int Tape::fileAt(uint64_t sample) const {
    if (!img_) return -1;
    int f = -1;
    for (size_t i = 0; i < img_->files.size() && img_->files[i].start <= sample; ++i) f = static_cast<int>(i);
    return f;
}

void Tape::saveState(std::vector<uint8_t>& out, uint64_t now) const {
    const uint64_t pos = static_cast<uint64_t>(posAt(now) * 256.0);   // 1/256 сэмпла
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(pos >> (8 * i)));
    out.push_back(static_cast<uint8_t>((playing_ ? 1 : 0) | (motor_ ? 2 : 0) | (auto_ ? 4 : 0)
                                       | (remote_ ? 8 : 0) | (img_ ? 16 : 0)));
    out.push_back(0);
}

void Tape::loadState(const uint8_t* p, size_t n, uint64_t now) {
    if (n < STATE_SIZE) return;
    uint64_t pos = 0;
    for (int i = 0; i < 8; ++i) pos |= uint64_t(p[i]) << (8 * i);
    const uint8_t fl = p[8];
    motor_ = fl & 2;
    remote_ = fl & 8;
    anchorTick_ = now;
    if (!img_ || !(fl & 16)) { playing_ = auto_ = false; return; }   // кассеты нет — только двигатель
    pos_ = std::min(pos / 256.0, static_cast<double>(img_->bits.size()));
    playing_ = fl & 1;
    auto_ = fl & 4;
}

} // namespace bk
