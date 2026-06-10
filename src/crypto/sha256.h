#pragma once
#include <cstdint>
#include <cstddef>

// ============================================================
//  SHA-256 / HMAC-SHA-256
//  独立实现，无外部依赖
// ============================================================

static constexpr size_t SHA256_DIGEST_SIZE = 32;
static constexpr size_t SHA256_BLOCK_SIZE  = 64;

struct Sha256Ctx {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t  buf[SHA256_BLOCK_SIZE];
    size_t   buflen;
};

void sha256_init  (Sha256Ctx* ctx);
void sha256_update(Sha256Ctx* ctx, const uint8_t* data, size_t len);
void sha256_final (Sha256Ctx* ctx, uint8_t digest[SHA256_DIGEST_SIZE]);

// 一次性哈希
void sha256(const uint8_t* data, size_t len, uint8_t digest[SHA256_DIGEST_SIZE]);

// HMAC-SHA-256
void hmac_sha256(const uint8_t* key,  size_t key_len,
                 const uint8_t* msg,  size_t msg_len,
                 uint8_t mac[SHA256_DIGEST_SIZE]);
