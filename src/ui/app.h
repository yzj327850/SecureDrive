#pragma once
#include "../volume/volume.h"
#include "../vfs/vfs.h"
#include "../security/lockout.h"
#include "../disk/disk.h"
#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <unordered_set>
#include <unordered_map>

// ============================================================
//  App：应用控制器，协调 UI 屏幕和后端逻辑
// ============================================================

enum class Screen {
    DeviceSelect,  // 选择磁盘/分区
    InitWizard,    // 初始化新卷向导
    Unlock,        // 解锁屏幕
    FileManager,   // 文件管理器
};

struct AppCtx {
    // 当前屏幕
    Screen current_screen = Screen::DeviceSelect;

    // 设备信息
    std::vector<DiskInfo>       disks;
    std::vector<PartitionInfo>  partitions;
    int                         selected_disk  = -1;
    int                         selected_part  = -1;
    std::string                 device_path;    // 当前选中的分区路径（实际为父磁盘路径）
    uint64_t                    partition_offset = 0; // 分区在磁盘上的字节偏移
    uint64_t                    partition_size   = 0; // 分区大小（字节），免格式化加密时传入 create_inplace
    std::string                 partition_device_path; // 分区设备路径（如 \.\HarddiskN\PartitionM），用于数据 I/O

    // 核心对象
    Volume   volume;
    Vfs      vfs;
    Lockout  lockout;

    // 状态消息
    std::string status_msg;
    bool        status_is_error = false;

    // 后台任务进度（0.0 ~ 1.0，-1 = 不显示）
    std::atomic<float>  bg_progress{-1.0f};
    std::string         bg_progress_label; // 当前正在操作的文件名（UI 只读）
    std::string         bg_task_type;      // "import" / "delete" / 其他（用于区分进度条标题）

    // 初始化向导状态
    bool wizard_existing   = false; // true=加密已有分区，false=全新初始化
    bool wizard_portable   = false; // true=便携模式（双分区：明文boot+加密data）
    // 三平台可执行文件源路径（程序启动时自动扫描 exe_dir 发现）
    std::string portable_win_src;    // Windows exe 路径
    std::string portable_mac_src;    // macOS .app 路径
    std::string portable_linux_src;  // Linux binary 路径

    // 文件管理器当前路径
    std::string current_dir = "/";

    // 目录列表缓存（避免每帧调用 list_dir）
    std::vector<FileInfo> cached_entries;
    std::string            cached_dir;       // 当前缓存对应的路径
    bool                   cache_dirty = false; // 需要刷新

    // 左侧导航树：已展开节点的路径集合
    std::unordered_set<std::string> tree_expanded; // 已展开的目录路径
    // 导航树目录子项缓存：path → 子目录列表（仅目录）
    std::unordered_map<std::string, std::vector<FileInfo>> tree_dir_cache;

    // 锁定状态文件路径（公开分区上）
    std::string lockout_file;

    void set_status(const std::string& msg, bool is_err = false){
        status_msg      = msg;
        status_is_error = is_err;
    }
};

class App {
public:
    App();
    ~App();

    bool init(const std::string& exe_dir);
    void run();

private:
    void init_glfw();
    void init_imgui();
    void shutdown();

    void render_frame();
    void draw_device_select();
    void draw_init_wizard();
    void draw_unlock();
    void draw_file_manager();
    void draw_statusbar();

    // 辅助
    void do_scan_devices();
    bool auto_detect_portable_volume(); // 便携模式自动检测
    void do_unlock(const std::string& password);
    void do_lock();
    void do_import_file(const std::string& ext_path, const std::string& vfs_path);
    void do_export_file(const std::string& vfs_path, const std::string& ext_dir);
    void do_open_file(const std::string& vfs_path);
    void do_delete(const std::string& vfs_path, bool is_dir);
    bool delete_recursive(const std::string& vfs_path, bool is_dir);

    // 批量 / 文件夹导入
    void do_import_folder(const std::string& ext_folder, const std::string& vfs_parent);
    void do_import_paths(const std::vector<std::string>& paths);  // 支持文件+文件夹混合

    // 导航树辅助
    void draw_dir_tree_node(const std::string& path, const std::string& label);

    // 拖拽相关
    static void glfw_drop_callback(void* window, int count, const char** paths);
    void on_drop(int count, const char** paths);

    AppCtx ctx_;
    void*  window_ = nullptr; // GLFWwindow*

    // 后台任务状态
    std::atomic<bool>      bg_task_done{true};
    std::atomic<bool>      bg_task_success{true};
    std::string            bg_task_status;
    std::thread            bg_thread;

    // 解锁后台状态
    std::atomic<bool>      unlock_running{false};
    std::atomic<bool>      unlock_done{false};
    std::atomic<bool>      unlock_ok{false};
    std::thread            unlock_thread;

    // 拖拽队列（主线程 → 渲染帧处理）
    std::mutex                   drop_mutex_;
    std::queue<std::string>      drop_queue_;  // 待导入的外部路径（UTF-8）
};
