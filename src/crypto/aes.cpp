#include "aes.h"
#include <cstring>
#include <cstdio>

// ============================================================
//  AES-256 实现（FIPS 197）
//  优先使用 AES-NI 硬件加速，回退到表驱动实现
// ============================================================

// ---- AES-NI 检测 ----
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    #define USE_AES_NI 1
    #include <intrin.h>
    #include <wmmintrin.h>
#elif defined(__GNUC__) && defined(__x86_64__)
    #define USE_AES_NI 1
    #include <x86intrin.h>
    #include <wmmintrin.h>
#else
    #define USE_AES_NI 0
#endif

static bool has_aes_ni() {
#if USE_AES_NI
#if defined(_MSC_VER)
    int cpuInfo[4] = {};
    __cpuid(cpuInfo, 1);
    return (cpuInfo[2] & (1 << 25)) != 0;
#else
    #include <cpuid.h>
    unsigned int eax=0, ebx=0, ecx=0, edx=0;
    if(__get_cpuid(1, &eax, &ebx, &ecx, &edx))
        return (ecx & (1 << 25)) != 0;
    return false;
#endif
#else
    return false;
#endif
}

// ============================================================
//  AES-NI 实现（当硬件支持时使用）
// ============================================================

#if USE_AES_NI

// 辅助函数：把 4 个 big-endian uint32_t word 转成 AES-NI __m128i
// 软件 enc_ks 中 word 的高字节对应 state[row0][col]，但 _mm_loadu_si128
// 按小端序 dword 解释内存。必须显式反序每个 word 的 byte，使 xmm 的
// byte 排列和 AES state 一致（byte0=state[0][c], byte1=state[1][c]...）
static inline __m128i ks_words_to_xmm(const uint32_t w[4]) {
    alignas(16) uint8_t bytes[16] = {
        (uint8_t)(w[0] >> 24), (uint8_t)(w[0] >> 16), (uint8_t)(w[0] >> 8), (uint8_t)w[0],
        (uint8_t)(w[1] >> 24), (uint8_t)(w[1] >> 16), (uint8_t)(w[1] >> 8), (uint8_t)w[1],
        (uint8_t)(w[2] >> 24), (uint8_t)(w[2] >> 16), (uint8_t)(w[2] >> 8), (uint8_t)w[2],
        (uint8_t)(w[3] >> 24), (uint8_t)(w[3] >> 16), (uint8_t)(w[3] >> 8), (uint8_t)w[3]
    };
    return _mm_loadu_si128((__m128i*)bytes);
}

// 从软件 enc_ks[60] 生成 AES-NI 加密轮密钥（15 个 128-bit）
static void aesni256_expand_key(const uint32_t enc_ks[60], uint8_t round_keys[240]) {
    __m128i* rk = (__m128i*)round_keys;
    for (int i = 0; i < 15; i++) {
        rk[i] = ks_words_to_xmm(&enc_ks[i * 4]);
    }
}

// 从软件 enc_ks[60] 生成 AES-NI 等效逆密码解密密钥
// _mm_aesdec_si128 使用等效逆密码，要求：
//   dk[0]  = K14（末轮，不做 InvMixColumns，直接 XOR）
//   dk[i]  = _mm_aesimc_si128(K_{14-i}), i=1..13（中间轮必须做 InvMixColumns）
//   dk[14] = K0（首轮，不做 InvMixColumns）
// 注意：这里传入的应为 enc_ks（加密密钥），AES-NI 的 IMC 变换与软件 dec_ks 不同
static void aesni256_make_dec_keys(const uint32_t enc_ks[60], uint8_t dec_keys[240]) {
    __m128i* dk = (__m128i*)dec_keys;
    dk[0]  = ks_words_to_xmm(&enc_ks[14 * 4]); // K14，末轮不做 IMC
    for (int i = 1; i <= 13; i++) {
        // 中间轮：必须用 _mm_aesimc_si128 做 InvMixColumns
        dk[i] = _mm_aesimc_si128(ks_words_to_xmm(&enc_ks[(14 - i) * 4]));
    }
    dk[14] = ks_words_to_xmm(&enc_ks[0]);       // K0，首轮不做 IMC
}

// AES-NI 加密单个块
static void aesni256_encrypt_block(const uint8_t round_keys[240],
                                    const uint8_t in[16], uint8_t out[16])
{
    const __m128i* rk = (const __m128i*)round_keys;
    __m128i state = _mm_loadu_si128((const __m128i*)in);

    state = _mm_xor_si128(state, _mm_loadu_si128(&rk[0]));
    state = _mm_aesenc_si128(state, _mm_loadu_si128(&rk[1]));
    state = _mm_aesenc_si128(state, _mm_loadu_si128(&rk[2]));
    state = _mm_aesenc_si128(state, _mm_loadu_si128(&rk[3]));
    state = _mm_aesenc_si128(state, _mm_loadu_si128(&rk[4]));
    state = _mm_aesenc_si128(state, _mm_loadu_si128(&rk[5]));
    state = _mm_aesenc_si128(state, _mm_loadu_si128(&rk[6]));
    state = _mm_aesenc_si128(state, _mm_loadu_si128(&rk[7]));
    state = _mm_aesenc_si128(state, _mm_loadu_si128(&rk[8]));
    state = _mm_aesenc_si128(state, _mm_loadu_si128(&rk[9]));
    state = _mm_aesenc_si128(state, _mm_loadu_si128(&rk[10]));
    state = _mm_aesenc_si128(state, _mm_loadu_si128(&rk[11]));
    state = _mm_aesenc_si128(state, _mm_loadu_si128(&rk[12]));
    state = _mm_aesenc_si128(state, _mm_loadu_si128(&rk[13]));
    state = _mm_aesenclast_si128(state, _mm_loadu_si128(&rk[14]));

    _mm_storeu_si128((__m128i*)out, state);
}

// AES-NI 解密：用等效逆密码密钥 + aesdec
static void aesni256_decrypt_block(const uint8_t round_keys[240],
                                    const uint8_t in[16], uint8_t out[16])
{
    const __m128i* rk = (const __m128i*)round_keys;
    __m128i state = _mm_loadu_si128((const __m128i*)in);

    state = _mm_xor_si128(state, _mm_loadu_si128(&rk[0]));
    state = _mm_aesdec_si128(state, _mm_loadu_si128(&rk[1]));
    state = _mm_aesdec_si128(state, _mm_loadu_si128(&rk[2]));
    state = _mm_aesdec_si128(state, _mm_loadu_si128(&rk[3]));
    state = _mm_aesdec_si128(state, _mm_loadu_si128(&rk[4]));
    state = _mm_aesdec_si128(state, _mm_loadu_si128(&rk[5]));
    state = _mm_aesdec_si128(state, _mm_loadu_si128(&rk[6]));
    state = _mm_aesdec_si128(state, _mm_loadu_si128(&rk[7]));
    state = _mm_aesdec_si128(state, _mm_loadu_si128(&rk[8]));
    state = _mm_aesdec_si128(state, _mm_loadu_si128(&rk[9]));
    state = _mm_aesdec_si128(state, _mm_loadu_si128(&rk[10]));
    state = _mm_aesdec_si128(state, _mm_loadu_si128(&rk[11]));
    state = _mm_aesdec_si128(state, _mm_loadu_si128(&rk[12]));
    state = _mm_aesdec_si128(state, _mm_loadu_si128(&rk[13]));
    state = _mm_aesdeclast_si128(state, _mm_loadu_si128(&rk[14]));

    _mm_storeu_si128((__m128i*)out, state);
}

#endif // USE_AES_NI

// ============================================================
//  软件回退：表驱动 AES-256（FIPS 197）
// ============================================================

// --- S-box 与逆 S-box ---
static const uint8_t SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t INV_SBOX[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

// --- GF(2^8) 乘法 ---
static inline uint8_t xtime(uint8_t x) {
    return (x<<1) ^ ((x>>7) ? 0x1b : 0x00);
}
static inline uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p=0;
    for(int i=0;i<8;i++){
        if(b&1) p^=a;
        a = xtime(a);
        b>>=1;
    }
    return p;
}

// --- 辅助宏 ---
#define ROTWORD(w) (((w)<<8)|((w)>>24))
#define SUBWORD(w) ( \
    ((uint32_t)SBOX[(w)>>24]<<24)| \
    ((uint32_t)SBOX[((w)>>16)&0xFF]<<16)| \
    ((uint32_t)SBOX[((w)>>8)&0xFF]<<8)| \
    ((uint32_t)SBOX[(w)&0xFF]) )

static const uint32_t RCON[11] = {
    0x00000000,0x01000000,0x02000000,0x04000000,0x08000000,
    0x10000000,0x20000000,0x40000000,0x80000000,0x1b000000,0x36000000
};

// --- 密钥扩展（AES-256: Nk=8, Nr=14） ---
void aes256_init(Aes256Ctx* ctx, const uint8_t key[AES256_KEY_SIZE]) {
    // === 步骤 1：总是用标准软件路径生成 enc_ks 和 dec_ks ===
    // 无论是否使用 AES-NI，软件密钥都先准备好，确保：
    //   1) 软件回退路径始终可用
    //   2) AES-NI 轮密钥可以从可靠的软件密钥转换生成
    uint32_t* W = ctx->enc_ks;
    for(int i=0;i<8;i++){
        W[i] = ((uint32_t)key[4*i]<<24)|((uint32_t)key[4*i+1]<<16)|
               ((uint32_t)key[4*i+2]<<8)|(uint32_t)key[4*i+3];
    }
    for(int i=8;i<60;i++){
        uint32_t tmp = W[i-1];
        if(i%8==0)          tmp = SUBWORD(ROTWORD(tmp)) ^ RCON[i/8];
        else if(i%8==4)     tmp = SUBWORD(tmp);
        W[i] = W[i-8] ^ tmp;
    }
    uint32_t* DW = ctx->dec_ks;
    memcpy(DW, W, 60*4);
    // 首轮和末轮不需要 InvMixColumns（等效逆密码）
    for(int i=4;i<56;i++){
        uint32_t w = DW[i];
        uint8_t b[4] = {(uint8_t)(w>>24),(uint8_t)(w>>16),(uint8_t)(w>>8),(uint8_t)w};
        uint8_t nb[4];
        nb[0]=gmul(b[0],0x0e)^gmul(b[1],0x0b)^gmul(b[2],0x0d)^gmul(b[3],0x09);
        nb[1]=gmul(b[0],0x09)^gmul(b[1],0x0e)^gmul(b[2],0x0b)^gmul(b[3],0x0d);
        nb[2]=gmul(b[0],0x0d)^gmul(b[1],0x09)^gmul(b[2],0x0e)^gmul(b[3],0x0b);
        nb[3]=gmul(b[0],0x0b)^gmul(b[1],0x0d)^gmul(b[2],0x09)^gmul(b[3],0x0e);
        DW[i]=((uint32_t)nb[0]<<24)|((uint32_t)nb[1]<<16)|((uint32_t)nb[2]<<8)|nb[3];
    }

#if USE_AES_NI
    ctx->use_aesni = has_aes_ni();
    fprintf(stderr, "[aes256_init] use_aesni=%d\n", (int)ctx->use_aesni); fflush(stderr);
    if(ctx->use_aesni) {
        fprintf(stderr, "[aes256_init] 从 enc_ks 转换 AES-NI 轮密钥...\n"); fflush(stderr);
        aesni256_expand_key(ctx->enc_ks, ctx->aesni_enc_keys);
        aesni256_make_dec_keys(ctx->enc_ks, ctx->aesni_dec_keys);
        fprintf(stderr, "[aes256_init] AES-NI 密钥转换完成\n"); fflush(stderr);
    }
#endif
}

// --- 状态操作 ---
typedef uint8_t State[4][4];

static inline void bytes_to_state(const uint8_t* b, State s){
    for(int r=0;r<4;r++) for(int c=0;c<4;c++) s[r][c]=b[c*4+r];
}
static inline void state_to_bytes(const State s, uint8_t* b){
    for(int r=0;r<4;r++) for(int c=0;c<4;c++) b[c*4+r]=s[r][c];
}
static inline void add_round_key(State s, const uint32_t* rk){
    for(int c=0;c<4;c++){
        uint32_t w=rk[c];
        s[0][c]^=(w>>24)&0xFF; s[1][c]^=(w>>16)&0xFF;
        s[2][c]^=(w>>8)&0xFF;  s[3][c]^=w&0xFF;
    }
}
static void sub_bytes(State s){
    for(int r=0;r<4;r++) for(int c=0;c<4;c++) s[r][c]=SBOX[s[r][c]];
}
static void inv_sub_bytes(State s){
    for(int r=0;r<4;r++) for(int c=0;c<4;c++) s[r][c]=INV_SBOX[s[r][c]];
}
static void shift_rows(State s){
    uint8_t t;
    // row1 left 1
    t=s[1][0]; s[1][0]=s[1][1]; s[1][1]=s[1][2]; s[1][2]=s[1][3]; s[1][3]=t;
    // row2 left 2
    t=s[2][0]; s[2][0]=s[2][2]; s[2][2]=t;
    t=s[2][1]; s[2][1]=s[2][3]; s[2][3]=t;
    // row3 left 3 (= right 1)
    t=s[3][3]; s[3][3]=s[3][2]; s[3][2]=s[3][1]; s[3][1]=s[3][0]; s[3][0]=t;
}
static void inv_shift_rows(State s){
    uint8_t t;
    t=s[1][3]; s[1][3]=s[1][2]; s[1][2]=s[1][1]; s[1][1]=s[1][0]; s[1][0]=t;
    t=s[2][0]; s[2][0]=s[2][2]; s[2][2]=t;
    t=s[2][1]; s[2][1]=s[2][3]; s[2][3]=t;
    t=s[3][0]; s[3][0]=s[3][1]; s[3][1]=s[3][2]; s[3][2]=s[3][3]; s[3][3]=t;
}
static void mix_columns(State s){
    for(int c=0;c<4;c++){
        uint8_t a=s[0][c],b=s[1][c],d=s[2][c],e=s[3][c];
        s[0][c]=gmul(a,2)^gmul(b,3)^d^e;
        s[1][c]=a^gmul(b,2)^gmul(d,3)^e;
        s[2][c]=a^b^gmul(d,2)^gmul(e,3);
        s[3][c]=gmul(a,3)^b^d^gmul(e,2);
    }
}
static void inv_mix_columns(State s){
    for(int c=0;c<4;c++){
        uint8_t a=s[0][c],b=s[1][c],d=s[2][c],e=s[3][c];
        s[0][c]=gmul(a,0x0e)^gmul(b,0x0b)^gmul(d,0x0d)^gmul(e,0x09);
        s[1][c]=gmul(a,0x09)^gmul(b,0x0e)^gmul(d,0x0b)^gmul(e,0x0d);
        s[2][c]=gmul(a,0x0d)^gmul(b,0x09)^gmul(d,0x0e)^gmul(e,0x0b);
        s[3][c]=gmul(a,0x0b)^gmul(b,0x0d)^gmul(d,0x09)^gmul(e,0x0e);
    }
}

// --- AES-256 加密（14 轮） ---
void aes256_encrypt_block(const Aes256Ctx* ctx,
                          const uint8_t in[16], uint8_t out[16])
{
#if USE_AES_NI
    if(ctx->use_aesni) {
        aesni256_encrypt_block(ctx->aesni_enc_keys, in, out);
        return;
    }
#endif
    State s;
    bytes_to_state(in, s);
    add_round_key(s, ctx->enc_ks);
    for(int rnd=1;rnd<14;rnd++){
        sub_bytes(s); shift_rows(s); mix_columns(s);
        add_round_key(s, ctx->enc_ks + rnd*4);
    }
    sub_bytes(s); shift_rows(s);
    add_round_key(s, ctx->enc_ks + 56);
    state_to_bytes(s, out);
}

// --- AES-256 解密（14 轮） ---
// 使用等效逆密码（FIPS 197 §5.3.5）：
//   首末轮：ISR → ISB → ARK（密钥未修改）
//   中间轮：ISR → ISB → IMC → ARK（密钥经过 InvMixColumns 预处理）
// 注意：IMC 必须在 ARK 之前，因为 IMC(state ⊕ K') = IMC(state) ⊕ K
//        其中 K' = IMC(K) 即 dec_ks 中预处理的密钥。
void aes256_decrypt_block(const Aes256Ctx* ctx,
                          const uint8_t in[16], uint8_t out[16])
{
#if USE_AES_NI
    if(ctx->use_aesni) {
        aesni256_decrypt_block(ctx->aesni_dec_keys, in, out);
        return;
    }
#endif
    State s;
    bytes_to_state(in, s);
    add_round_key(s, ctx->dec_ks + 56);
    for(int rnd=13;rnd>=1;rnd--){
        inv_shift_rows(s); inv_sub_bytes(s);
        inv_mix_columns(s);
        add_round_key(s, ctx->dec_ks + rnd*4);
    }
    inv_shift_rows(s); inv_sub_bytes(s);
    add_round_key(s, ctx->dec_ks);
    state_to_bytes(s, out);
}

void aes256_clear(Aes256Ctx* ctx) {
    memset(ctx, 0, sizeof(*ctx));
}
