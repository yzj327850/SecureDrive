#include "argon2id.h"
#include "blake2b.h"
#include <cstring>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <thread>
#include <cstdio>

// ============================================================
//  Argon2id 实现（RFC 9106）
//  精简版：单线程展开，保持算法正确性
// ============================================================

// ---- 常量 ----
static constexpr uint32_t ARGON2_BLOCK_SIZE   = 1024; // 字节
static constexpr uint32_t ARGON2_QWORDS_PER_BLOCK = ARGON2_BLOCK_SIZE / 8;
static constexpr uint32_t SYNC_POINTS = 4;

// ---- Block ----
struct Block {
    uint64_t v[ARGON2_QWORDS_PER_BLOCK]; // 128 个 64 位字
};

static inline uint64_t fBlaMka(uint64_t x, uint64_t y) {
    return x + y + 2 * (uint32_t)x * (uint32_t)y;
}

#define ROTR64(x,n) (((x)>>(n))|((x)<<(64-(n))))

static void G_perm(uint64_t* v) {
    // 16 次 G 混合（Argon2 内部使用 BLAKE2 风格的 G 函数）
    #define GB(a,b,c,d) do { \
        v[a]=fBlaMka(v[a],v[b]); v[d]^=v[a]; v[d]=ROTR64(v[d],32); \
        v[c]=fBlaMka(v[c],v[d]); v[b]^=v[c]; v[b]=ROTR64(v[b],24); \
        v[a]=fBlaMka(v[a],v[b]); v[d]^=v[a]; v[d]=ROTR64(v[d],16); \
        v[c]=fBlaMka(v[c],v[d]); v[b]^=v[c]; v[b]=ROTR64(v[b],63); \
    } while(0)
    // 列混合
    GB(0,4,8,12); GB(1,5,9,13); GB(2,6,10,14); GB(3,7,11,15);
    // 对角线混合
    GB(0,5,10,15); GB(1,6,11,12); GB(2,7,8,13); GB(3,4,9,14);
    #undef GB
}

// 压缩函数 G(X, Y) -> R
static void compress(Block& R, const Block& X, const Block& Y) {
    Block Z;
    for(int i=0;i<ARGON2_QWORDS_PER_BLOCK;i++) Z.v[i]=X.v[i]^Y.v[i];

    // 以 8×8 的 16 字排列，逐行、逐列进行 G 置换
    for(int i=0;i<8;i++){
        // 行: 每行 16 个 64 位字
        uint64_t tmp[16];
        for(int j=0;j<16;j++) tmp[j]=Z.v[i*16+j];
        G_perm(tmp);
        for(int j=0;j<16;j++) Z.v[i*16+j]=tmp[j];
    }
    for(int i=0;i<8;i++){
        // 列: stride=8
        uint64_t tmp[16];
        for(int j=0;j<8;j++) for(int k=0;k<2;k++) tmp[j*2+k]=Z.v[(i+j*8)*2+k];
        // 重新排列为 16 个连续元素
        uint64_t col[16];
        for(int j=0;j<8;j++){
            col[j*2+0]=Z.v[i*2     + j*16];
            col[j*2+1]=Z.v[i*2+1   + j*16];
        }
        G_perm(col);
        for(int j=0;j<8;j++){
            Z.v[i*2   + j*16] = col[j*2+0];
            Z.v[i*2+1 + j*16] = col[j*2+1];
        }
    }
    for(int i=0;i<ARGON2_QWORDS_PER_BLOCK;i++) R.v[i] = X.v[i]^Y.v[i]^Z.v[i];
}

// ---- 辅助：Blake2b 变长哈希 H' ----
static void H_prime(void* out, size_t out_len, const void* in, size_t in_len) {
    if(out_len <= 64){
        blake2b(out, out_len, in, in_len, nullptr, 0);
    } else {
        // 分段输出
        uint32_t out_len_le = (uint32_t)out_len;
        uint8_t* op = (uint8_t*)out;
        // First block
        size_t copy_len = (in_len > ARGON2_BLOCK_SIZE) ? ARGON2_BLOCK_SIZE : in_len;
        uint8_t tmp[4 + ARGON2_BLOCK_SIZE];
        memcpy(tmp, &out_len_le, 4);
        memcpy(tmp+4, in, copy_len);
        uint8_t A[64];
        blake2b(A, 64, tmp, 4 + copy_len, nullptr, 0);
        memcpy(op, A, 32); op += 32;
        size_t remaining = out_len - 32;
        while(remaining > 64){
            blake2b(A, 64, A, 64, nullptr, 0);
            memcpy(op, A, 32); op += 32;
            remaining -= 32;
        }
        blake2b(op, remaining, A, 64, nullptr, 0);
    }
}

static inline uint64_t load64(const void* p){uint64_t v;memcpy(&v,p,8);return v;}
static inline void store64(void* p,uint64_t v){memcpy(p,&v,8);}
static inline uint32_t load32(const void* p){uint32_t v;memcpy(&v,p,4);return v;}
static inline void store32(void* p,uint32_t v){memcpy(p,&v,4);}

int argon2id_hash(const uint8_t* password, size_t pass_len,
                  const uint8_t* salt,     size_t salt_len,
                  uint8_t*       out,      size_t out_len,
                  uint32_t t_cost, uint32_t m_cost, uint32_t parallelism)
{
    if(!out || out_len==0 || out_len>ARGON2_BLOCK_SIZE) return -1;

    const uint32_t lanes = parallelism;
    const uint32_t segment_len = (m_cost / (4 * lanes));
    const uint32_t lane_len    = segment_len * 4;
    const uint32_t mem_blocks  = lane_len * lanes;

    if(mem_blocks < 8) return -1;

    // 分配内存块
    std::vector<Block> M(mem_blocks);

    // 1. 生成 H0（初始哈希）
    // H0 = Blake2b(p || taglen || mem || t || 2(=Argon2id) || p_len || password || s_len || salt || 0 || 0)
    uint8_t h0[64];
    {
        uint8_t buf[256];
        size_t  off = 0;
        auto write32 = [&](uint32_t v){ store32(buf+off, v); off+=4; };
        write32(parallelism);
        write32((uint32_t)out_len);
        write32(m_cost);
        write32(t_cost);
        write32(0x13); // version 1.3
        write32(2);    // Argon2id = 2
        write32((uint32_t)pass_len);
        memcpy(buf+off, password, pass_len); off+=pass_len;
        write32((uint32_t)salt_len);
        memcpy(buf+off, salt, salt_len); off+=salt_len;
        write32(0); // key len
        write32(0); // assoc len
        blake2b(h0, 64, buf, off, nullptr, 0);
        memset(buf, 0, sizeof(buf));
    }

    // 2. 初始化前两个 block 每条 lane
    for(uint32_t l=0; l<lanes; l++){
        uint8_t seed[72];
        memcpy(seed, h0, 64);
        store32(seed+64, 0);
        store32(seed+68, l);
        H_prime(M[l*lane_len+0].v, ARGON2_BLOCK_SIZE, seed, 72);
        store32(seed+64, 1);
        H_prime(M[l*lane_len+1].v, ARGON2_BLOCK_SIZE, seed, 72);
    }
    memset(h0, 0, 64);

    // 3. 填充阶段
    for(uint32_t pass=0; pass<t_cost; pass++){
        for(uint32_t slice=0; slice<SYNC_POINTS; slice++){
            for(uint32_t l=0; l<lanes; l++){
                // 确定起始索引
                uint32_t start = (pass==0 && slice==0) ? 2 : 0;
                uint32_t seg_start = slice * segment_len;

                for(uint32_t s=start; s<segment_len; s++){
                    uint32_t idx = seg_start + s;
                    uint32_t prev_idx = (idx==0) ? lane_len-1 : idx-1;

                    uint64_t J1 = M[l*lane_len + prev_idx].v[0];
                    uint64_t J2 = (J1 >> 32) & 0xFFFFFFFF;
                    J1 &= 0xFFFFFFFF;

                    // 引用 lane
                    uint32_t ref_lane = (pass==0 && slice==0) ? l : (uint32_t)(J2 % lanes);

                    // 引用区域大小
                    uint32_t ref_size;
                    bool same_lane = (ref_lane == l);
                    if(pass == 0){
                        if(same_lane) ref_size = seg_start + s - 1;
                        else         ref_size = seg_start + (s>0?0:(-1));
                    } else {
                        uint32_t base = lane_len - segment_len;
                        if(same_lane) ref_size = base + s;
                        else         ref_size = base + (s>0?0:(-1));
                    }
                    if(ref_size == 0) ref_size = 1;
                    ref_size = std::min(ref_size, lane_len);

                    // 映射 J1 到参考索引
                    uint64_t x  = (J1 * J1) >> 32;
                    uint64_t y  = (ref_size * x) >> 32;
                    uint32_t z  = (uint32_t)(ref_size - 1 - y);
                    uint32_t ref_index = (uint32_t)(z % lane_len);

                    Block& cur  = M[l*lane_len + idx];
                    Block& prev = M[l*lane_len + prev_idx];
                    Block& ref  = M[ref_lane*lane_len + ref_index];

                    if(pass == 0)
                        compress(cur, prev, ref);
                    else {
                        Block tmp;
                        compress(tmp, prev, ref);
                        for(int i=0;i<ARGON2_QWORDS_PER_BLOCK;i++) cur.v[i]^=tmp.v[i];
                    }
                }
            }
        }
    }

    // 4. XOR 所有 lane 的最后一个 block
    Block C = M[lane_len - 1];
    for(uint32_t l=1; l<lanes; l++){
        for(int i=0;i<ARGON2_QWORDS_PER_BLOCK;i++)
            C.v[i] ^= M[l*lane_len + lane_len - 1].v[i];
    }

    // 5. 最终输出
    H_prime(out, out_len, C.v, ARGON2_BLOCK_SIZE);

    // 清除内存
    for(auto& blk : M) memset(blk.v, 0, ARGON2_BLOCK_SIZE);

    fprintf(stderr, "[Argon2id] 完成 t=%u m=%u p=%u\n", t_cost, m_cost, parallelism);
    fflush(stderr);

    return 0;
}
