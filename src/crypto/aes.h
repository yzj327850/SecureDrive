#pragma once
#include <cstdint>
#include <cstddef>

// AES-NI 支持检测（编译时平台检测）
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    #define USE_AES_NI 1
#elif defined(__GNUC__) && defined(__x86_64__)
    #define USE_AES_NI 1
#else
    #define USE_AES_NI 0
#endif

// ============================================================
//  AES-256 块加密（FIPS 197）
//  支持加密 / 解密，密钥长度固定 256 位
// ============================================================

static constexpr size_t AES_BLOCK_SIZE  = 16;
static constexpr size_t AES256_KEY_SIZE = 32;

struct alignas(16) Aes256Ctx {
    uint32_t enc_ks[60]; // 扩展密钥（加密）- 软件回退用
    uint32_t dec_ks[60]; // 扩展密钥（解密）- 软件回退用
#if USE_AES_NI
    alignas(16) uint8_t  aesni_enc_keys[240]; // AES-NI 加密轮密钥（15 × 16B）
    alignas(16) uint8_t  aesni_dec_keys[240]; // AES-NI 解密轮密钥
    bool     use_aesni;           // true = 使用 AES-NI 硬件加速
#endif
};

// 初始化（密钥调度）
void aes256_init(Aes256Ctx* ctx, const uint8_t key[AES256_KEY_SIZE]);

// 加密单个 16 字节块
void aes256_encrypt_block(const Aes256Ctx* ctx,
                          const uint8_t in[AES_BLOCK_SIZE],
                          uint8_t       out[AES_BLOCK_SIZE]);

// 解密单个 16 字节块
void aes256_decrypt_block(const Aes256Ctx* ctx,
                          const uint8_t in[AES_BLOCK_SIZE],
                          uint8_t       out[AES_BLOCK_SIZE]);

// 安全清除上下文
void aes256_clear(Aes256Ctx* ctx);
