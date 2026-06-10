#include "blake2b.h"
#include <cstring>
#include <cstdlib>

// ============================================================
//  BLAKE2b 实现（RFC 7693）
// ============================================================

static const uint64_t IV[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
    0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
    0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
};

static const uint8_t SIGMA[12][16] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15},
    {14,10, 4, 8, 9,15,13, 6, 1,12, 0, 2,11, 7, 5, 3},
    {11, 8,12, 0, 5, 2,15,13,10,14, 3, 6, 7, 1, 9, 4},
    { 7, 9, 3, 1,13,12,11,14, 2, 6, 5,10, 4, 0,15, 8},
    { 9, 0, 5, 7, 2, 4,10,15,14, 1,11,12, 6, 8, 3,13},
    { 2,12, 6,10, 0,11, 8, 3, 4,13, 7, 5,15,14, 1, 9},
    {12, 5, 1,15,14,13, 4,10, 0, 7, 6, 3, 9, 2, 8,11},
    {13,11, 7,14,12, 1, 3, 9, 5, 0,15, 4, 8, 6, 2,10},
    { 6,15,14, 9,11, 3, 0, 8,12, 2,13, 7, 1, 4,10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5,15,11, 9,14, 3,12,13, 0},
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15},
    {14,10, 4, 8, 9,15,13, 6, 1,12, 0, 2,11, 7, 5, 3}
};

#define ROTR64(x,n) (((x)>>(n))|((x)<<(64-(n))))

#define G(r,i,a,b,c,d) do { \
    a += b + m[SIGMA[r][2*i+0]]; \
    d ^= a; d = ROTR64(d,32); \
    c += d; b ^= c; b = ROTR64(b,24); \
    a += b + m[SIGMA[r][2*i+1]]; \
    d ^= a; d = ROTR64(d,16); \
    c += d; b ^= c; b = ROTR64(b,63); \
} while(0)

static inline uint64_t load64(const void* p) {
    uint64_t v; memcpy(&v, p, 8); return v;
}
static inline void store64(void* p, uint64_t v) {
    memcpy(p, &v, 8);
}

static void blake2b_compress(Blake2bCtx* ctx, const uint8_t blk[BLAKE2B_BLOCKBYTES], bool last) {
    uint64_t m[16], v[16];
    for(int i=0;i<16;i++) m[i]=load64(blk+i*8);
    for(int i=0;i<8;i++) v[i]=ctx->h[i];
    v[ 8]=IV[0]; v[ 9]=IV[1]; v[10]=IV[2]; v[11]=IV[3];
    v[12]=IV[4]^ctx->t[0]; v[13]=IV[5]^ctx->t[1];
    v[14]=last ? IV[6]^0xFFFFFFFFFFFFFFFFULL : IV[6];
    v[15]=IV[7];

    for(int r=0;r<12;r++){
        G(r,0,v[0],v[4],v[ 8],v[12]);
        G(r,1,v[1],v[5],v[ 9],v[13]);
        G(r,2,v[2],v[6],v[10],v[14]);
        G(r,3,v[3],v[7],v[11],v[15]);
        G(r,4,v[0],v[5],v[10],v[15]);
        G(r,5,v[1],v[6],v[11],v[12]);
        G(r,6,v[2],v[7],v[ 8],v[13]);
        G(r,7,v[3],v[4],v[ 9],v[14]);
    }
    for(int i=0;i<8;i++) ctx->h[i]^=v[i]^v[i+8];
}

int blake2b_init(Blake2bCtx* ctx, size_t outlen) {
    return blake2b_init_key(ctx, outlen, nullptr, 0);
}

int blake2b_init_key(Blake2bCtx* ctx, size_t outlen,
                     const void* key, size_t keylen) {
    if(outlen==0||outlen>64) return -1;
    if(keylen>64) return -1;
    memset(ctx, 0, sizeof(*ctx));
    for(int i=0;i<8;i++) ctx->h[i]=IV[i];
    ctx->h[0] ^= 0x01010000ULL ^ ((uint64_t)keylen<<8) ^ (uint64_t)outlen;
    ctx->outlen = outlen;
    if(keylen > 0){
        uint8_t block[BLAKE2B_BLOCKBYTES] = {};
        memcpy(block, key, keylen);
        blake2b_update(ctx, block, BLAKE2B_BLOCKBYTES);
        memset(block, 0, sizeof(block));
    }
    return 0;
}

void blake2b_update(Blake2bCtx* ctx, const void* in, size_t inlen) {
    const uint8_t* p = (const uint8_t*)in;
    while(inlen > 0){
        size_t space = BLAKE2B_BLOCKBYTES - ctx->buflen;
        size_t take  = inlen < space ? inlen : space;
        memcpy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take;
        p += take; inlen -= take;
        if(ctx->buflen == BLAKE2B_BLOCKBYTES && inlen > 0){
            ctx->t[0] += BLAKE2B_BLOCKBYTES;
            if(ctx->t[0] < BLAKE2B_BLOCKBYTES) ctx->t[1]++;
            blake2b_compress(ctx, ctx->buf, false);
            ctx->buflen = 0;
        }
    }
}

int blake2b_final(Blake2bCtx* ctx, void* out, size_t outlen) {
    if(!out||outlen==0||outlen>ctx->outlen) return -1;
    ctx->t[0] += ctx->buflen;
    if(ctx->t[0] < ctx->buflen) ctx->t[1]++;
    ctx->f[0] = 0xFFFFFFFFFFFFFFFFULL;
    // zero-pad buffer
    memset(ctx->buf + ctx->buflen, 0, BLAKE2B_BLOCKBYTES - ctx->buflen);
    blake2b_compress(ctx, ctx->buf, true);

    uint8_t tmp[64];
    for(int i=0;i<8;i++) store64(tmp+i*8, ctx->h[i]);
    memcpy(out, tmp, outlen);
    memset(ctx, 0, sizeof(*ctx));
    memset(tmp, 0, sizeof(tmp));
    return 0;
}

int blake2b(void* out, size_t outlen,
            const void* in, size_t inlen,
            const void* key, size_t keylen) {
    Blake2bCtx ctx;
    if(blake2b_init_key(&ctx, outlen, key, keylen) < 0) return -1;
    blake2b_update(&ctx, in, inlen);
    return blake2b_final(&ctx, out, outlen);
}
