#pragma once
#include <cstdint>
#include <cstddef>

// ============================================================
//  BLAKE2b — Argon2id 的依赖原语
//  符合 RFC 7693
// ============================================================

static constexpr size_t BLAKE2B_OUTBYTES = 64;
static constexpr size_t BLAKE2B_BLOCKBYTES = 128;

struct Blake2bCtx {
    uint64_t h[8];
    uint64_t t[2];
    uint64_t f[2];
    uint8_t  buf[BLAKE2B_BLOCKBYTES];
    size_t   buflen;
    size_t   outlen;
};

int  blake2b_init  (Blake2bCtx* ctx, size_t outlen);
int  blake2b_init_key(Blake2bCtx* ctx, size_t outlen,
                      const void* key, size_t keylen);
void blake2b_update(Blake2bCtx* ctx, const void* in, size_t inlen);
int  blake2b_final (Blake2bCtx* ctx, void* out, size_t outlen);

// 一次性哈希
int blake2b(void* out, size_t outlen,
            const void* in, size_t inlen,
            const void* key, size_t keylen);
