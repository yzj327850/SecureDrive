#pragma once
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <functional>
#include <array>
#include <memory>
#include <stdexcept>

// ============================================================
//  跨平台辅助函数
// ============================================================

// 获取当前时间戳（毫秒）
inline uint64_t get_timestamp_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// 睡眠指定毫秒
inline void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// 执行命令并返回 stdout（stderr 被忽略）
inline std::string run_command(const std::string& cmd) {
    std::array<char, 128> buffer;
    std::string result;
#ifdef _WIN32
    std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(cmd.c_str(), "r"), _pclose);
#else
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
#endif
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

// 检测是否以管理员/root 运行
inline bool is_admin_or_root() {
#ifdef _WIN32
    // Windows 版本在 app.cpp 中单独实现
    return false; // placeholder
#else
    return geteuid() == 0;
#endif
}

// 获取环境变量
inline std::string get_env(const std::string& name) {
    const char* val = std::getenv(name.c_str());
    return val ? std::string(val) : "";
}

// 轮询等待条件满足（带超时）
inline bool poll_wait(std::function<bool()> condition, int timeout_ms, int interval_ms = 500) {
    auto start = std::chrono::steady_clock::now();
    while (true) {
        if (condition()) return true;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_ms) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }
}
