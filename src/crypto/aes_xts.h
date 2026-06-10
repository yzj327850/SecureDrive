#pragma once
#include "aes.h"
#include <cstdint>
#include <cstddef>

// ============================================================
//  AES-256-XTS（IEEE Std 1619-2007）
//  专为磁盘扇区加密设计，密钥 = 512 位（两个 256 位 AES 密钥）
//  data_key 加密数据，tweak_key 加密 tweak（扇区号）
// ============================================================

static constexpr size_t XTS_KEY_SIZE = 64; // 2 × 256 位

struct alignas(16) AesXtsCtx {
    Aes256Ctx data_aes;  // 数据加密密钥
    Aes256Ctx tweak_aes; // tweak 加密密钥
};

// 初始化（key 必须为 64 字节：前 32 字节=data_key，后 32 字节=tweak_key）
void aes_xts_init (AesXtsCtx* ctx, const uint8_t key[XTS_KEY_SIZE]);

// 加密一个扇区
// sector_num: 扇区逻辑地址（LBA）
// data / out 长度均为 sector_size（必须是 AES_BLOCK_SIZE 的整数倍）
void aes_xts_encrypt(const AesXtsCtx* ctx,
                     uint64_t sector_num,
                     const uint8_t* data, uint8_t* out,
                     size_t sector_size);

// 解密一个扇区
void aes_xts_decrypt(const AesXtsCtx* ctx,
                     uint64_t sector_num,
                     const uint8_t* data, uint8_t* out,
                     size_t sector_size);

// 清除密钥材料
void aes_xts_clear(AesXtsCtx* ctx);
