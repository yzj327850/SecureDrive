#pragma once
#include <cstdint>
#include <ctime>
#include <string>

// ============================================================
//  暴力破解防护（锁定机制）
//
//  策略：连续 MAX_ATTEMPTS 次密码错误 → 锁定 LOCKOUT_SECONDS 秒
//  状态持久化到公开分区上的加密 JSON 文件
//  注：为防止攻击者重置文件绕过锁定，锁定记录也记录在 VFS 内部
// ============================================================

static constexpr int     MAX_ATTEMPTS      = 3;
static constexpr int64_t LOCKOUT_SECONDS   = 300; // 5 分钟

struct LockoutState {
    int     attempts;        // 累计连续错误次数
    int64_t lockout_until;   // Unix 时间戳，0 = 未锁定
};

class Lockout {
public:
    Lockout() = default;

    // 从文件加载状态（软件分区上的明文 JSON，仅记录时间戳和次数）
    bool load(const std::string& state_file);

    // 保存状态到文件
    bool save(const std::string& state_file) const;

    // 是否当前被锁定
    bool is_locked() const;

    // 还需等待多少秒（0 = 未锁定）
    int64_t seconds_remaining() const;

    // 记录一次密码错误
    void record_failure();

    // 密码正确，重置计数器
    void reset();

    // 当前错误次数
    int attempts() const { return state_.attempts; }
    int max_attempts() const { return MAX_ATTEMPTS; }

private:
    LockoutState state_{0, 0};
    std::string  file_path_;
};
