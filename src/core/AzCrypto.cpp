#include "AzCrypto.h"
#include <cstring>

namespace bk {
namespace azcrypto {

namespace {

struct Tables {
    uint8_t sbox[256], inv[256];
    Tables() {
        // S-блок по определению: обратный элемент в GF(2^8) и аффинное преобразование.
        uint8_t p = 1, q = 1;
        do {
            p = static_cast<uint8_t>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
            q ^= static_cast<uint8_t>(q << 1);
            q ^= static_cast<uint8_t>(q << 2);
            q ^= static_cast<uint8_t>(q << 4);
            if (q & 0x80) q ^= 0x09;
            const uint8_t x = static_cast<uint8_t>(q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^
                                                   ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4)));
            sbox[p] = static_cast<uint8_t>(x ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
        for (int i = 0; i < 256; ++i) inv[sbox[i]] = static_cast<uint8_t>(i);
    }
};
const Tables& T() { static const Tables t; return t; }

uint8_t xt(uint8_t x) { return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
uint8_t mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; }
    return r;
}

// Расписание ключа AES-256: 15 раундовых ключей по 16 байт.
void expand(const uint8_t key[32], uint8_t rk[240]) {
    std::memcpy(rk, key, 32);
    uint8_t rcon = 1;
    for (int i = 32; i < 240; i += 4) {
        uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
        if (i % 32 == 0) {
            const uint8_t a = t[0];
            t[0] = static_cast<uint8_t>(T().sbox[t[1]] ^ rcon); t[1] = T().sbox[t[2]]; t[2] = T().sbox[t[3]]; t[3] = T().sbox[a];
            rcon = xt(rcon);
        } else if (i % 32 == 16) {
            for (uint8_t& b : t) b = T().sbox[b];
        }
        for (int k = 0; k < 4; ++k) rk[i + k] = static_cast<uint8_t>(rk[i - 32 + k] ^ t[k]);
    }
}

void addKey(uint8_t s[16], const uint8_t* k) { for (int i = 0; i < 16; ++i) s[i] ^= k[i]; }

} // namespace

void aes256EncryptBlock(const uint8_t key[32], uint8_t s[16]) {
    uint8_t rk[240];
    expand(key, rk);
    addKey(s, rk);
    for (int r = 1; r <= 14; ++r) {
        for (int i = 0; i < 16; ++i) s[i] = T().sbox[s[i]];
        uint8_t t[16];
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row) t[c * 4 + row] = s[((c + row) % 4) * 4 + row];
        if (r != 14)
            for (int c = 0; c < 4; ++c) {
                uint8_t* a = t + c * 4;
                const uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                a[0] = static_cast<uint8_t>(xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3);
                a[1] = static_cast<uint8_t>(a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3);
                a[2] = static_cast<uint8_t>(a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3);
                a[3] = static_cast<uint8_t>(xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3));
            }
        std::memcpy(s, t, 16);
        addKey(s, rk + r * 16);
    }
}

void aes256DecryptBlock(const uint8_t key[32], uint8_t s[16]) {
    uint8_t rk[240];
    expand(key, rk);
    addKey(s, rk + 14 * 16);
    for (int r = 13; r >= 0; --r) {
        uint8_t t[16];
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row) t[((c + row) % 4) * 4 + row] = s[c * 4 + row];
        for (int i = 0; i < 16; ++i) t[i] = T().inv[t[i]];
        addKey(t, rk + r * 16);
        if (r != 0)
            for (int c = 0; c < 4; ++c) {
                uint8_t* a = t + c * 4;
                const uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                a[0] = static_cast<uint8_t>(mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9));
                a[1] = static_cast<uint8_t>(mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13));
                a[2] = static_cast<uint8_t>(mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11));
                a[3] = static_cast<uint8_t>(mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14));
            }
        std::memcpy(s, t, 16);
    }
}

size_t ecbEncrypt(const uint8_t* in, size_t len, uint8_t* out, const char key[32]) {
    const size_t n = len + (16 - (len & 15));
    std::memset(out, 0, n);
    uint8_t blk[16] = {0};
    for (size_t i = 0; i < n; i += 16) {
        if (i + 16 < len) std::memcpy(blk, in + i, 16);
        else if (len > i) std::memcpy(blk, in + i, len - i);   // хвост блока — от предыдущего
        uint8_t b[16];
        std::memcpy(b, blk, 16);
        aes256EncryptBlock(reinterpret_cast<const uint8_t*>(key), b);
        std::memcpy(out + i, b, 16);
        std::memcpy(blk, b, 16);   // прошивка шифрует блок на месте: следующий начинается с шифртекста
    }
    return n;
}

bool ecbDecrypt(const uint8_t* in, size_t len, uint8_t* out, size_t* outLen, const char key[32]) {
    if (len & 15) return false;
    for (size_t i = 0; i < len; i += 16) {
        uint8_t b[16];
        std::memcpy(b, in + i, 16);
        aes256DecryptBlock(reinterpret_cast<const uint8_t*>(key), b);
        std::memcpy(out + i, b, 16);
    }
    if (outLen) *outLen = strnlen(reinterpret_cast<const char*>(out), len);
    return true;
}

namespace {
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
uint8_t decMap(uint8_t c) {
    if (c == '=') return 64;
    const char* p = c ? std::strchr(kB64, c) : nullptr;
    return p ? static_cast<uint8_t>(p - kB64) : 127;
}
}

int base64Decode(uint8_t* out, size_t cap, size_t* outLen, const uint8_t* in, size_t len) {
    size_t i, n = 0, eq = 0;
    for (i = 0; i < len; ++i) {
        size_t sp = 0;
        while (i < len && in[i] == ' ') { ++i; ++sp; }
        if (i == len) break;
        if ((len - i >= 2 && in[i] == '\r' && in[i + 1] == '\n') || in[i] == '\n') continue;
        if (sp) return -0x2C;
        if (in[i] == '=' && ++eq > 2) return -0x2C;
        if (in[i] > 127 || decMap(in[i]) == 127) return -0x2C;
        if (decMap(in[i]) < 64 && eq) return -0x2C;
        ++n;
    }
    if (n == 0) { *outLen = 0; return 0; }
    const size_t need = ((n * 6 + 7) >> 3) - eq;
    if (!out || cap < need) { *outLen = need; return -0x2A; }
    uint32_t x = 0;
    int j = 3, k = 0;
    uint8_t* p = out;
    for (size_t m = 0; m < i; ++m) {
        const uint8_t c = in[m];
        if (c == '\r' || c == '\n' || c == ' ') continue;
        j -= decMap(c) == 64;
        x = (x << 6) | (decMap(c) & 0x3F);
        if (++k == 4) {
            k = 0;
            if (j > 0) *p++ = static_cast<uint8_t>(x >> 16);
            if (j > 1) *p++ = static_cast<uint8_t>(x >> 8);
            if (j > 2) *p++ = static_cast<uint8_t>(x);
        }
    }
    *outLen = static_cast<size_t>(p - out);
    return 0;
}

int base64Encode(char* out, size_t cap, size_t* outLen, const uint8_t* in, size_t len) {
    if (len == 0) { *outLen = 0; return 0; }
    const size_t n = (len / 3 + (len % 3 != 0)) * 4;
    if (!out || cap < n + 1) { *outLen = n + 1; return -0x2A; }
    char* p = out;
    size_t i = 0;
    for (; i < (len / 3) * 3; i += 3) {
        const uint8_t a = in[i], b = in[i + 1], c = in[i + 2];
        *p++ = kB64[a >> 2];
        *p++ = kB64[((a & 3) << 4) | (b >> 4)];
        *p++ = kB64[((b & 15) << 2) | (c >> 6)];
        *p++ = kB64[c & 63];
    }
    if (i < len) {
        const uint8_t a = in[i], b = i + 1 < len ? in[i + 1] : 0;
        *p++ = kB64[a >> 2];
        *p++ = kB64[((a & 3) << 4) | (b >> 4)];
        *p++ = i + 1 < len ? kB64[(b & 15) << 2] : '=';
        *p++ = '=';
    }
    *p = 0;
    *outLen = static_cast<size_t>(p - out);
    return 0;
}

} // namespace azcrypto
} // namespace bk
