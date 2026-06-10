#include "lockout.h"
#include <ctime>
#include <cstdio>
#include <cstring>

// ============================================================
//  Lockout 实现
//  文件格式（纯文本）: "attempts=N\nlockout_until=T\n"
// ============================================================

bool Lockout::load(const std::string& state_file) {
    file_path_ = state_file;
    state_ = {0, 0};

    FILE* f = fopen(state_file.c_str(), "r");
    if(!f) return false;

    int attempts = 0; long long until = 0;
    fscanf(f, "attempts=%d\n", &attempts);
    fscanf(f, "lockout_until=%lld\n", &until);
    fclose(f);

    state_.attempts     = attempts;
    state_.lockout_until= (int64_t)until;
    return true;
}

bool Lockout::save(const std::string& state_file) const {
    FILE* f = fopen(state_file.c_str(), "w");
    if(!f) return false;
    fprintf(f, "attempts=%d\nlockout_until=%lld\n",
            state_.attempts, (long long)state_.lockout_until);
    fclose(f);
    return true;
}

bool Lockout::is_locked() const {
    if(state_.lockout_until == 0) return false;
    return (int64_t)time(nullptr) < state_.lockout_until;
}

int64_t Lockout::seconds_remaining() const {
    if(!is_locked()) return 0;
    return state_.lockout_until - (int64_t)time(nullptr);
}

void Lockout::record_failure() {
    state_.attempts++;
    if(state_.attempts >= MAX_ATTEMPTS){
        state_.lockout_until = (int64_t)time(nullptr) + LOCKOUT_SECONDS;
        // 不重置 attempts，让锁定到期后继续从 MAX_ATTEMPTS 计数
        // 即每次解锁后如果再次连续输错，立即锁定
    }
    if(!file_path_.empty()) save(file_path_);
}

void Lockout::reset() {
    state_.attempts     = 0;
    state_.lockout_until= 0;
    if(!file_path_.empty()) save(file_path_);
}
