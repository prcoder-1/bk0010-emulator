#pragma once
#include <cstddef>
#include <cstdint>

namespace bk {

// Шифрование облачного клиента AZБК (HOF): AES-256 ECB и base64 — как в прошивке 00019
// (aes256.c Ильи Левина, base64 в духе mbedtls; RE_ARM/decompiled/include/crypto.h).
namespace azcrypto {

void aes256EncryptBlock(const uint8_t key[32], uint8_t block[16]);
void aes256DecryptBlock(const uint8_t key[32], uint8_t block[16]);

// aes256_ecb_encrypt_buf прошивки: длина результата — len, дополненная до следующей
// кратной 16 (кратная 16 получает лишний блок). Хвост последнего блока прошивка не
// заполняет — там остаются байты предыдущего блока; так и здесь.
size_t ecbEncrypt(const uint8_t* in, size_t len, uint8_t* out, const char key[32]);
// aes256_ecb_decrypt_buf: false, если len не кратна 16; длина результата — strlen.
bool ecbDecrypt(const uint8_t* in, size_t len, uint8_t* out, size_t* outLen, const char key[32]);

// 0 — успех; иначе код ошибки mbedtls (-0x2A — мало места, -0x2C — не base64).
int base64Decode(uint8_t* out, size_t cap, size_t* outLen, const uint8_t* in, size_t len);
int base64Encode(char* out, size_t cap, size_t* outLen, const uint8_t* in, size_t len);

} // namespace azcrypto
} // namespace bk
