#include "ui/app.h"
#include <filesystem>
#include <cstdio>
#include <ctime>

#ifdef _WIN32
#  include <windows.h>
#  define EXE_DIR() []{ wchar_t path[MAX_PATH]; \
       GetModuleFileNameW(nullptr, path, MAX_PATH); \
       std::filesystem::path p(path); \
       return p.parent_path().string(); }()
#elif defined(__APPLE__)
#  include <mach-o/dyld.h>
#  define EXE_DIR() []{ char path[1024]; uint32_t sz=sizeof(path); \
       _NSGetExecutablePath(path,&sz); \
       return std::filesystem::path(path).parent_path().string(); }()
#else
#  define EXE_DIR() []{ char path[1024]; \
       ssize_t len = readlink("/proc/self/exe", path, sizeof(path)-1); \
       if(len > 0) path[len] = '\0'; else path[0] = '\0'; \
       return std::filesystem::path(path).parent_path().string(); }()
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    // 如果进程带有控制台窗口，立即释放它（确保纯 GUI 模式）
    if (GetConsoleWindow() != nullptr) {
        FreeConsole();
    }
#endif

    std::string exe_dir = EXE_DIR();

    // ---- 日志重定向到文件（界面不再显示任何日志）----
    std::string log_path = exe_dir + "/SecureDrive.log";
    FILE* log_fp = nullptr;
#ifdef _WIN32
    fopen_s(&log_fp, log_path.c_str(), "a");
#else
    log_fp = fopen(log_path.c_str(), "a");
#endif
    if (log_fp) {
        // 写入启动时间戳
        time_t now = time(nullptr);
        struct tm* tinfo = localtime(&now);
        char tbuf[64];
        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tinfo);
        fprintf(log_fp, "\n========== SecureDrive 启动 %s ==========\n", tbuf);
        fflush(log_fp);
        fclose(log_fp);

        // 将 stdout / stderr 重定向到日志文件
        freopen(log_path.c_str(), "a", stdout);
        freopen(log_path.c_str(), "a", stderr);
    }

    printf("SecureDrive v1.0 [build " __DATE__ " " __TIME__ "]\n");
    printf("可执行文件目录: %s\n", exe_dir.c_str());
    printf("注意：需要管理员/root 权限才能操作原始磁盘分区。\n");

    // 检查平台提示
#ifdef _WIN32
    printf("平台: Windows\n");
#elif defined(__APPLE__)
    printf("平台: macOS\n");
#else
    printf("平台: Linux\n");
#endif

    App app;
    if(!app.init(exe_dir)){
        fprintf(stderr, "初始化失败\n");
        return 1;
    }
    app.run();
    return 0;
}
