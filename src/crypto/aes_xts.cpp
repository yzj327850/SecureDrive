#include "aes_xts.h"
#include <cstring>

// ============================================================
//  AES-256-XTS 实现（IEEE 1619-2007）
// ============================================================

void aes_xts_init(AesXtsCtx* ctx, const uint8_t key[XTS_KEY_SIZE]) {
    aes256_init(&ctx->data_aes,  key);
    aes256_init(&ctx->tweak_aes, key + AES256_KEY_SIZE);
}

// GF(2^128) 乘以 α（x）即左移并条件异或 0x87
static void gf128_mul_x(uint8_t t[16]) {
    uint8_t carry = t[15] >> 7;
    for(int i=15;i>0;i--) t[i] = (t[i]<<1) | (t[i-1]>>7);
    t[0] <<= 1;
    if(carry) t[0] ^= 0x87;
}

// 将 sector_num 编码为小端 128 位整数
static void sector_to_tweak(uint64_t sector_num, uint8_t tweak[16]) {
    memset(tweak, 0, 16);
    for(int i=0;i<8;i++) tweak[i] = (sector_num >> (8*i)) & 0xFF;
}

void aes_xts_encrypt(const AesXtsCtx* ctx,
                     uint64_t sector_num,
                     const uint8_t* data, uint8_t* out,
                     size_t sector_size)
{
    uint8_t T[16]; // tweak
    sector_to_tweak(sector_num, T);
    aes256_encrypt_block(&ctx->tweak_aes, T, T);

    const uint8_t* in_p  = data;
    uint8_t*       out_p = out;

    for(size_t blk = 0; blk < sector_size / AES_BLOCK_SIZE; blk++){
        uint8_t tmp[16];
        // PP = P ^ T
        for(int i=0;i<16;i++) tmp[i] = in_p[i] ^ T[i];
        // CC = AES_encrypt(PP)
        aes256_encrypt_block(&ctx->data_aes, tmp, tmp);
        // C = CC ^ T
        for(int i=0;i<16;i++) out_p[i] = tmp[i] ^ T[i];

        gf128_mul_x(T);
        in_p  += 16;
        out_p += 16;
    }
    memset(T, 0, 16);
}

void aes_xts_decrypt(const AesXtsCtx* ctx,
                     uint64_t sector_num,
                     const uint8_t* data, uint8_t* out,
                     size_t sector_size)
{
    uint8_t T[16];
    sector_to_tweak(sector_num, T);
    aes256_encrypt_block(&ctx->tweak_aes, T, T);

    const uint8_t* in_p  = data;
    uint8_t*       out_p = out;

    for(size_t blk = 0; blk < sector_size / AES_BLOCK_SIZE; blk++){
        uint8_t tmp[16];
        for(int i=0;i<16;i++) tmp[i] = in_p[i] ^ T[i];
        aes256_decrypt_block(&ctx->data_aes, tmp, tmp);
        for(int i=0;i<16;i++) out_p[i] = tmp[i] ^ T[i];

        gf128_mul_x(T);
        in_p  += 16;
        out_p += 16;
    }
    memset(T, 0, 16);
}

void aes_xts_clear(AesXtsCtx* ctx) {
    aes256_clear(&ctx->data_aes);
    aes256_clear(&ctx->tweak_aes);
    memset(ctx, 0, sizeof(*ctx));
}
