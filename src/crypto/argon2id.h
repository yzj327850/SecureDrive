#pragma once
#include <cstdint>
#include <cstddef>

// ============================================================
//  Argon2id — 密码哈希（PHC 获奖方案）
//  参数: time_cost=3, memory_kb=65536(64MB), parallelism=4
//  输出: 64 字节（用于 AES-256-XTS 的 512 位密钥）
// ============================================================

static constexpr size_t ARGON2_OUTPUT_SIZE = 64; // 字节

// 适用于纯软件实现的参数（无 SIMD 优化，需平衡安全性和速度）
// OWASP 推荐: t=2, m=19456(19MB), p=1；这里适度提高
static constexpr uint32_t ARGON2_TIME_COST    = 2;
static constexpr uint32_t ARGON2_MEMORY_KB    = 32768; // 32 MB
static constexpr uint32_t ARGON2_PARALLELISM  = 2;

/**
 * @brief 使用 Argon2id 从密码派生密钥
 *
 * @param password   明文密码
 * @param pass_len   密码字节长度
 * @param salt       随机盐（建议 32 字节）
 * @param salt_len   盐长度
 * @param out        输出缓冲区
 * @param out_len    输出字节数（最大 64）
 * @param t_cost     迭代次数
 * @param m_cost     内存（KB）
 * @param parallelism并行度
 * @return 0 成功，< 0 失败
 */
int argon2id_hash(const uint8_t* password, size_t pass_len,
                  const uint8_t* salt,     size_t salt_len,
                  uint8_t*       out,      size_t out_len,
                  uint32_t t_cost, uint32_t m_cost, uint32_t parallelism);
