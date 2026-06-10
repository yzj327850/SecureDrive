#include "sha256.h"
#include <cstring>

// ============================================================
//  SHA-256 实现（符合 FIPS 180-4）
// ============================================================

static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROTR32(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(e,f,g)   (((e)&(f))^(~(e)&(g)))
#define MAJ(a,b,c)  (((a)&(b))^((a)&(c))^((b)&(c)))
#define EP0(a) (ROTR32(a,2)^ROTR32(a,13)^ROTR32(a,22))
#define EP1(e) (ROTR32(e,6)^ROTR32(e,11)^ROTR32(e,25))
#define SIG0(x)(ROTR32(x,7)^ROTR32(x,18)^((x)>>3))
#define SIG1(x)(ROTR32(x,17)^ROTR32(x,19)^((x)>>10))

static inline uint32_t load_be32(const uint8_t* p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|
           ((uint32_t)p[2]<<8)|(uint32_t)p[3];
}
static inline void store_be32(uint8_t* p, uint32_t v) {
    p[0]=(v>>24)&0xFF; p[1]=(v>>16)&0xFF;
    p[2]=(v>>8)&0xFF;  p[3]=v&0xFF;
}
static inline void store_be64(uint8_t* p, uint64_t v) {
    for(int i=7;i>=0;i--){ p[i]=v&0xFF; v>>=8; }
}

static void sha256_compress(Sha256Ctx* ctx, const uint8_t blk[64]) {
    uint32_t W[64], a,b,c,d,e,f,g,h, t1,t2;
    for(int i=0;i<16;i++) W[i]=load_be32(blk+i*4);
    for(int i=16;i<64;i++) W[i]=SIG1(W[i-2])+W[i-7]+SIG0(W[i-15])+W[i-16];

    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; d=ctx->state[3];
    e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];

    for(int i=0;i<64;i++){
        t1=h+EP1(e)+CH(e,f,g)+K[i]+W[i];
        t2=EP0(a)+MAJ(a,b,c);
        h=g; g=f; f=e; e=d+t1;
        d=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

void sha256_init(Sha256Ctx* ctx) {
    ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85;
    ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c;
    ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
    ctx->bitcount=0; ctx->buflen=0;
}

void sha256_update(Sha256Ctx* ctx, const uint8_t* data, size_t len) {
    ctx->bitcount += (uint64_t)len * 8;
    while(len > 0){
        size_t space = SHA256_BLOCK_SIZE - ctx->buflen;
        size_t take  = len < space ? len : space;
        memcpy(ctx->buf + ctx->buflen, data, take);
        ctx->buflen += take; data += take; len -= take;
        if(ctx->buflen == SHA256_BLOCK_SIZE){
            sha256_compress(ctx, ctx->buf);
            ctx->buflen = 0;
        }
    }
}

void sha256_final(Sha256Ctx* ctx, uint8_t digest[SHA256_DIGEST_SIZE]) {
    uint64_t bc = ctx->bitcount;
    // padding
    uint8_t pad = 0x80;
    sha256_update(ctx, &pad, 1);
    while(ctx->buflen != 56){
        uint8_t z = 0;
        sha256_update(ctx, &z, 1);
    }
    uint8_t len_be[8];
    store_be64(len_be, bc);
    sha256_update(ctx, len_be, 8);
    for(int i=0;i<8;i++) store_be32(digest+i*4, ctx->state[i]);
    // 清除内存
    memset(ctx, 0, sizeof(*ctx));
}

void sha256(const uint8_t* data, size_t len, uint8_t digest[SHA256_DIGEST_SIZE]) {
    Sha256Ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, digest);
}

void hmac_sha256(const uint8_t* key,  size_t key_len,
                 const uint8_t* msg,  size_t msg_len,
                 uint8_t mac[SHA256_DIGEST_SIZE])
{
    uint8_t k_pad[SHA256_BLOCK_SIZE] = {};
    uint8_t inner[SHA256_DIGEST_SIZE];

    // 若 key > block size，先哈希
    if(key_len > SHA256_BLOCK_SIZE){
        sha256(key, key_len, k_pad);
    } else {
        memcpy(k_pad, key, key_len);
    }

    // ipad = k_pad ^ 0x36
    uint8_t ipad[SHA256_BLOCK_SIZE], opad[SHA256_BLOCK_SIZE];
    for(int i=0;i<SHA256_BLOCK_SIZE;i++){
        ipad[i] = k_pad[i] ^ 0x36;
        opad[i] = k_pad[i] ^ 0x5c;
    }

    // inner hash
    Sha256Ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, SHA256_BLOCK_SIZE);
    sha256_update(&ctx, msg,  msg_len);
    sha256_final(&ctx, inner);

    // outer hash
    sha256_init(&ctx);
    sha256_update(&ctx, opad,  SHA256_BLOCK_SIZE);
    sha256_update(&ctx, inner, SHA256_DIGEST_SIZE);
    sha256_final(&ctx, mac);

    memset(k_pad, 0, sizeof(k_pad));
    memset(ipad, 0, sizeof(ipad));
    memset(opad, 0, sizeof(opad));
    memset(inner, 0, sizeof(inner));
}
