#pragma once
#include <cstdint>
#include <cstddef>

// ============================================================
//  跨平台安全随机数生成
//  Windows: BCryptGenRandom / CryptGenRandom
//  macOS / Linux: /dev/urandom
// ============================================================

/**
 * @brief 填充 buf 中 len 字节的密码学安全随机数
 * @return true 成功
 */
bool secure_random(uint8_t* buf, size_t len);
