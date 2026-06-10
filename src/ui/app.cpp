#include "app.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#ifdef _WIN32
#include <windows.h>
#include "resource.h"
#endif
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <atomic>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#endif

namespace fs = std::filesystem;

// ============================================================
//  权限检测
// ============================================================
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
static bool is_admin() {
    BOOL elevated = FALSE;
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation{};
        DWORD sz = sizeof(elevation);
        if (GetTokenInformation(hToken, TokenElevation, &elevation, sz, &sz)) {
            elevated = elevation.TokenIsElevated;
        }
        CloseHandle(hToken);
    }
    return elevated != FALSE;
}

// ---- 编码转换（Windows 系统编码 GBK/ANSI ↔ UTF-8）----
static std::string wide_to_utf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string str(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, str.data(), len, nullptr, nullptr);
    str.pop_back(); // remove null terminator
    return str;
}

static std::wstring utf8_to_wide(const std::string& str) {
    if (str.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring wstr(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, wstr.data(), len);
    wstr.pop_back();
    return wstr;
}

static std::string ansi_to_utf8(const std::string& ansi) {
    if (ansi.empty()) return "";
    int wlen = MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return ansi;
    std::wstring wstr(wlen, 0);
    MultiByteToWideChar(CP_ACP, 0, ansi.c_str(), -1, wstr.data(), wlen);
    return wide_to_utf8(wstr);
}

static std::string utf8_to_ansi(const std::string& utf8) {
    if (utf8.empty()) return "";
    std::wstring wstr = utf8_to_wide(utf8);
    if (wstr.empty()) return utf8;
    int alen = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (alen <= 0) return utf8;
    std::string ansi(alen, 0);
    WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, ansi.data(), alen, nullptr, nullptr);
    ansi.pop_back();
    return ansi;
}

// 检测字符串是否为合法 UTF-8
static bool is_valid_utf8(const std::string& s) {
    int bytes = 0;
    for (unsigned char c : s) {
        if (bytes == 0) {
            if ((c & 0x80) == 0) continue;
            else if ((c & 0xE0) == 0xC0) bytes = 1;
            else if ((c & 0xF0) == 0xE0) bytes = 2;
            else if ((c & 0xF8) == 0xF0) bytes = 3;
            else return false;
        } else {
            if ((c & 0xC0) != 0x80) return false;
            bytes--;
        }
    }
    return bytes == 0;
}

#else
static bool is_admin() { return geteuid() == 0; }
#endif

// ============================================================
//  构造 / 析构
// ============================================================
App::App() {}
App::~App() { shutdown(); }

// ============================================================
//  初始化
// ============================================================
bool App::init(const std::string& exe_dir) {
    ctx_.lockout_file = exe_dir + "/lockout.dat";
    ctx_.lockout.load(ctx_.lockout_file);

    // ---- 三平台可执行文件自动发现 ----
    // 扫描 exe_dir 目录，查找其他平台的 SecureDrive 可执行文件
    namespace fs = std::filesystem;
    fs::path dir(exe_dir);

    // Windows
    fs::path win_exe = dir / "SecureDrive.exe";
    if (fs::exists(win_exe)) {
        ctx_.portable_win_src = win_exe.string();
        fprintf(stderr, "[AUTO-DISC] Found Windows exe: %s\n", ctx_.portable_win_src.c_str());
    }

    // macOS
    fs::path mac_app = dir / "SecureDrive.app";
    if (fs::exists(mac_app) && fs::is_directory(mac_app)) {
        ctx_.portable_mac_src = mac_app.string();
        fprintf(stderr, "[AUTO-DISC] Found macOS app: %s\n", ctx_.portable_mac_src.c_str());
    }

    // Linux
    fs::path linux_bin = dir / "SecureDrive";
    if (fs::exists(linux_bin) && fs::is_regular_file(linux_bin)) {
        ctx_.portable_linux_src = linux_bin.string();
        fprintf(stderr, "[AUTO-DISC] Found Linux binary: %s\n", ctx_.portable_linux_src.c_str());
    }

    // 记录当前平台的 exe 源路径（向后兼容）
#ifdef _WIN32
    ctx_.portable_win_src = (dir / "SecureDrive.exe").string();
#endif

    init_glfw();
    init_imgui();
    do_scan_devices();

    // 便携模式：自动检测可移动磁盘上的 SecureDrive 卷
    auto_detect_portable_volume();

    return true;
}

void App::init_glfw() {
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    window_ = glfwCreateWindow(900, 600, "SecureDrive", nullptr, nullptr);
    glfwMakeContextCurrent((GLFWwindow*)window_);
    // 设置窗口图标（从exe资源加载）
#ifdef _WIN32
    {
        // 从exe资源中加载图标
        HICON hIcon = (HICON)LoadImage(
            GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_APP_ICON),
            IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
        if(hIcon){
            GLFWimage icon;
            // 提取图标位图
            ICONINFO info;
            GetIconInfo(hIcon, &info);
            BITMAP bmp;
            GetObject(info.hbmColor, sizeof(bmp), &bmp);
            icon.width = bmp.bmWidth;
            icon.height = bmp.bmHeight;
            int sz = icon.width * icon.height * 4;
            icon.pixels = new unsigned char[sz];
            // 使用GetDIBits获取BGRA像素
            BITMAPINFOHEADER bi = {};
            bi.biSize = sizeof(bi);
            bi.biWidth = icon.width;
            bi.biHeight = -icon.height; // 自顶向下
            bi.biPlanes = 1;
            bi.biBitCount = 32;
            bi.biCompression = BI_RGB;
            HDC hdc = GetDC(nullptr);
            GetDIBits(hdc, info.hbmColor, 0, icon.height, icon.pixels,
                      (BITMAPINFO*)&bi, DIB_RGB_COLORS);
            ReleaseDC(nullptr, hdc);
            glfwSetWindowIcon((GLFWwindow*)window_, 1, &icon);
            delete[] icon.pixels;
            DeleteObject(info.hbmColor);
            DeleteObject(info.hbmMask);
            DestroyIcon(hIcon);
        }
    }
#endif
    glfwSwapInterval(1);

    // 注册拖拽回调，支持拖文件/文件夹到窗口
    glfwSetWindowUserPointer((GLFWwindow*)window_, this);
    glfwSetDropCallback((GLFWwindow*)window_, [](GLFWwindow* w, int count, const char** paths){
        App* app = (App*)glfwGetWindowUserPointer(w);
        if(app) app->on_drop(count, paths);
    });
}

void App::init_imgui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // ---- 加载中文字体 ----
    // 优先加载系统自带的中文字体
    const char* cn_font_paths[] = {
#ifdef _WIN32
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/simsun.ttc",
        "C:/Windows/Fonts/simhei.ttf",
#endif
#ifdef __APPLE__
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/STHeiti Light.ttc",
#endif
#ifdef __linux__
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
#endif
        nullptr
    };
    bool font_loaded = false;
    for (int i = 0; cn_font_paths[i] != nullptr; ++i) {
        if (fs::exists(cn_font_paths[i])) {
            io.Fonts->AddFontFromFileTTF(cn_font_paths[i], 16.0f, nullptr,
                io.Fonts->GetGlyphRangesChineseFull());
            font_loaded = true;
            break;
        }
    }
    if (!font_loaded) {
        // 如果找不到中文字体，至少加载一个 16px 的默认字体（无中文支持）
        io.Fonts->AddFontDefault();
    }

    // 深色主题 + 自定义颜色
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding   = 6.0f;
    style.FrameRounding    = 4.0f;
    style.GrabRounding     = 4.0f;
    style.Colors[ImGuiCol_Header]        = ImVec4(0.24f,0.52f,0.88f,0.80f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.29f,0.60f,0.98f,0.80f);
    style.Colors[ImGuiCol_Button]        = ImVec4(0.24f,0.52f,0.88f,0.90f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.29f,0.60f,0.98f,1.00f);
    // 状态栏颜色
    style.Colors[ImGuiCol_MenuBarBg]     = ImVec4(0.14f,0.14f,0.14f,1.00f);

    ImGui_ImplGlfw_InitForOpenGL((GLFWwindow*)window_, true);
    ImGui_ImplOpenGL3_Init("#version 330");
}

void App::shutdown() {
    if(!window_) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow((GLFWwindow*)window_);
    glfwTerminate();
    window_ = nullptr;
}

// ============================================================
//  主循环
// ============================================================
void App::run() {
    GLFWwindow* win = (GLFWwindow*)window_;
    while(!glfwWindowShouldClose(win)){
        glfwPollEvents();

        // ---- 处理拖拽队列（只在文件管理器界面且无后台任务时触发）----
        if(ctx_.current_screen == Screen::FileManager
           && bg_task_done.load(std::memory_order_acquire))
        {
            std::vector<std::string> pending;
            {
                std::lock_guard<std::mutex> lk(drop_mutex_);
                while(!drop_queue_.empty()){
                    pending.push_back(drop_queue_.front());
                    drop_queue_.pop();
                }
            }
            if(!pending.empty()){
                // ★ 后台线程执行，避免阻塞 UI 渲染
                bg_task_done.store(false, std::memory_order_release);
                ctx_.bg_task_type = "import";
                bg_thread = std::make_unique<std::thread>([this, pending](){
                    do_import_paths(pending);
                    bg_task_done.store(true, std::memory_order_release);
                });
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        render_frame();

        ImGui::Render();
        int w,h; glfwGetFramebufferSize(win,&w,&h);
        glViewport(0,0,w,h);
        glClearColor(0.1f,0.1f,0.1f,1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(win);
    }
}

// ============================================================
//  帧渲染
// ============================================================
void App::render_frame() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0,0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_MenuBar |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##main", nullptr, flags);

    // ---- 菜单栏 ----
    if(ImGui::BeginMenuBar()){
        ImGui::TextDisabled("SecureDrive v1.0");
        ImGui::SameLine(ImGui::GetWindowWidth() - 200);
        if(ctx_.current_screen == Screen::FileManager){
            if(ImGui::Button("锁定")) do_lock();
            ImGui::SameLine();
        }
        if(ImGui::Button("重新扫描设备")){
            do_scan_devices();
            ctx_.current_screen = Screen::DeviceSelect;
        }
        ImGui::EndMenuBar();
    }

    // ---- 主内容 ----
    switch(ctx_.current_screen){
        case Screen::DeviceSelect: draw_device_select(); break;
        case Screen::InitWizard:   draw_init_wizard();   break;
        case Screen::Unlock:       draw_unlock();        break;
        case Screen::FileManager:  draw_file_manager();  break;
    }

    draw_statusbar();
    ImGui::End();
}

// ============================================================
//  设备选择界面
// ============================================================

// 检测指定分区偏移处是否有 Windows 文件系统（FAT/NTFS/exFAT），
// 返回文件系统标签串，如 "NTFS"/"FAT32"/"exFAT"/"RAW"
static std::string detect_fs_label(const std::string& disk_path, uint64_t offset) {
#ifdef _WIN32
    RawDisk tmp;
    if (!tmp.open(disk_path, false)) return "?";
    uint8_t buf[512] = {};
    if (!tmp.read(offset, buf, 512)) return "?";
    // NTFS: 偏移 3..10 = "NTFS    "
    if (memcmp(buf + 3, "NTFS    ", 8) == 0) return "NTFS";
    // FAT32: 偏移 82..89 = "FAT32   "
    if (memcmp(buf + 82, "FAT32   ", 8) == 0) return "FAT32";
    // FAT16/12: 偏移 54..61
    if (memcmp(buf + 54, "FAT16   ", 8) == 0) return "FAT16";
    if (memcmp(buf + 54, "FAT12   ", 8) == 0) return "FAT12";
    // exFAT: 偏移 3..10 = "EXFAT   "
    if (memcmp(buf + 3, "EXFAT   ", 8) == 0) return "exFAT";
    // SecureDrive 自身卷头
    if (memcmp(buf, "SDRV01", 6) == 0) return "SecureDrive";
    return "RAW";
#else
    return "?";
#endif
}

void App::draw_device_select() {
    // 权限警告横幅
    if (!is_admin()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.2f, 0.2f, 1));
        ImGui::TextWrapped("[!] 警告：当前未以管理员身份运行，无法对磁盘进行写操作！请右键以管理员身份运行本程序。");
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    ImGui::Text("选择要操作的磁盘或分区：");
    ImGui::Separator();

    // 磁盘列表（左侧）
    ImGui::BeginChild("disks", ImVec2(280, 400), true);
    for(int i=0; i<(int)ctx_.disks.size(); i++){
        const auto& d = ctx_.disks[i];
        char label[128];
        snprintf(label, sizeof(label), "%s  (%.1f GB)%s",
                 d.display_name.c_str(),
                 d.total_bytes / 1e9,
                 d.removable ? " [可移动]" : "");
        if(ImGui::Selectable(label, ctx_.selected_disk==i)){
            ctx_.selected_disk = i;
            ctx_.selected_part = -1;
            ctx_.partitions = enum_partitions(d.device_path);
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // 分区列表（右侧）
    ImGui::BeginChild("parts", ImVec2(0, 400), true);
    if(ctx_.selected_disk >= 0){
        const auto& d = ctx_.disks[ctx_.selected_disk];

        // ---- 整盘选项 ----
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.9f, 0.4f, 1));
        char whole_label[128];
        snprintf(whole_label, sizeof(whole_label),
                 "[整盘]  %s  (%.2f GB)",
                 d.device_path.c_str(), d.total_bytes / 1e9);
        // selected_part == -2 表示选中整盘
        if(ImGui::Selectable(whole_label, ctx_.selected_part == -2)) {
            ctx_.selected_part = -2;
            // 虚拟一个 PartitionInfo 代表整盘
            if (ctx_.partitions.empty()) {
                PartitionInfo pi;
                pi.device_path  = d.device_path;
                pi.parent_disk  = d.device_path;
                pi.offset_bytes = 0;
                pi.size_bytes   = d.total_bytes;
                pi.label        = "整盘";
                ctx_.partitions.push_back(pi);
            }
        }
        ImGui::PopStyleColor();
        ImGui::Separator();

        // ---- 分区列表 ----
        ImGui::Text("分区列表（点击选择）：");
        for(int i=0; i<(int)ctx_.partitions.size(); i++){
            const auto& p = ctx_.partitions[i];
            // 检测文件系统
            std::string fs = detect_fs_label(
                p.parent_disk.empty() ? p.device_path : p.parent_disk,
                p.offset_bytes);
            char label[180];
            snprintf(label, sizeof(label), "%s  (%.2f GB)  [%s]",
                     p.device_path.c_str(), p.size_bytes / 1e9,
                     fs.c_str());
            // 文件系统颜色提示
            if (fs == "SecureDrive")
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.6f, 1));
            else if (fs != "RAW" && fs != "?")
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.4f, 1));
            else
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.9f, 0.9f, 1));
            if(ImGui::Selectable(label, ctx_.selected_part==i))
                ctx_.selected_part = i;
            ImGui::PopStyleColor();
        }
    } else {
        ImGui::TextDisabled("← 请先选择磁盘");
    }
    ImGui::EndChild();

    // 操作按钮
    ImGui::Separator();

    // 获取当前选中的分区信息
    PartitionInfo cur_part{};
    bool whole_disk_selected = false;
    bool part_selected = false;
    if (ctx_.selected_disk >= 0) {
        const auto& d = ctx_.disks[ctx_.selected_disk];
        if (ctx_.selected_part == -2) {
            // 整盘模式
            cur_part.device_path  = d.device_path;
            cur_part.parent_disk  = d.device_path;
            cur_part.offset_bytes = 0;
            cur_part.size_bytes   = d.total_bytes;
            whole_disk_selected   = true;
            part_selected         = true;
        } else if (ctx_.selected_part >= 0 && ctx_.selected_part < (int)ctx_.partitions.size()) {
            cur_part      = ctx_.partitions[ctx_.selected_part];
            part_selected = true;
        }
    }

    if(!part_selected) ImGui::BeginDisabled();

    if(ImGui::Button("打开 / 解锁已有加密分区")){
        ctx_.device_path      = cur_part.parent_disk.empty() ? cur_part.device_path : cur_part.parent_disk;
        ctx_.partition_offset = cur_part.offset_bytes;
        if(ctx_.volume.open(ctx_.device_path, ctx_.partition_offset)){
            ctx_.current_screen = Screen::Unlock;
            ctx_.set_status("分区已读取，请输入密码解锁");
        } else {
            ctx_.set_status("此分区没有 SecureDrive 卷，请先初始化", true);
        }
    }
    ImGui::SameLine();

#ifdef _WIN32
    // 检测是否有 Windows 文件系统需要先卸载
    bool has_fs = false;
    if (part_selected) {
        std::string disk_p = cur_part.parent_disk.empty() ? cur_part.device_path : cur_part.parent_disk;
        std::string fs = detect_fs_label(disk_p, cur_part.offset_bytes);
        has_fs = (fs != "RAW" && fs != "?" && fs != "SecureDrive");
    }

    if (has_fs) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.3f, 0.1f, 1));
        if(ImGui::Button("[!] 卸载文件系统 → 初始化为加密分区（清空数据！）")){
            // 先卸载该磁盘上所有卷，再进初始化向导
#ifdef _WIN32
            std::string disk_p = cur_part.parent_disk.empty() ? cur_part.device_path : cur_part.parent_disk;
            int disk_num = extract_disk_number(disk_p);
            if (disk_num >= 0) {
                dismount_volumes_on_disk(disk_num);
            }
#endif
            ctx_.device_path      = cur_part.parent_disk.empty() ? cur_part.device_path : cur_part.parent_disk;
            ctx_.partition_offset = cur_part.offset_bytes;
            ctx_.wizard_existing  = false;
            ctx_.current_screen   = Screen::InitWizard;
        }
        ImGui::PopStyleColor();
    } else {
        if(ImGui::Button("初始化为新加密分区（会清空数据！）")){
            ctx_.device_path      = cur_part.parent_disk.empty() ? cur_part.device_path : cur_part.parent_disk;
            ctx_.partition_offset = cur_part.offset_bytes;
            ctx_.wizard_existing  = false;
            ctx_.current_screen   = Screen::InitWizard;
        }
    }
#else
    if(ImGui::Button("初始化为新加密分区（会清空数据！）")){
        ctx_.device_path      = cur_part.parent_disk.empty() ? cur_part.device_path : cur_part.parent_disk;
        ctx_.partition_offset = cur_part.offset_bytes;
        ctx_.wizard_existing  = false;
        ctx_.current_screen   = Screen::InitWizard;
    }
#endif

    if(!part_selected) ImGui::EndDisabled();

    // 提示说明
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f,0.6f,0.6f,1));
    ImGui::TextWrapped(
        "提示：\n"
        "- [绿色] SecureDrive 加密卷，可直接解锁\n"
        "- [黄色] 已有文件系统（NTFS/FAT32/exFAT），初始化前会自动卸载\n"
        "- [白色] RAW 未格式化分区，可直接初始化\n"
        "- [整盘] 整个物理磁盘，适合 U 盘整盘加密\n"
    );
    ImGui::PopStyleColor();
}

// ============================================================
//  扫描设备
// ============================================================
void App::do_scan_devices() {
    ctx_.disks = enum_disks();
    ctx_.partitions.clear();
    ctx_.selected_disk = ctx_.selected_part = -1;
    ctx_.set_status("设备扫描完成，找到 " + std::to_string(ctx_.disks.size()) + " 块磁盘");
}

// ============================================================
//  便携模式：自动检测可移动磁盘上的 SecureDrive 卷
//  如果找到，自动选中并跳转到解锁界面
//  返回 true 表示找到了自动选中的卷
// ============================================================
bool App::auto_detect_portable_volume() {
    for(int i = 0; i < (int)ctx_.disks.size(); i++){
        const auto& d = ctx_.disks[i];
        if(!d.removable) continue; // 只检测可移动磁盘

        auto parts = enum_partitions(d.device_path);
        for(int j = 0; j < (int)parts.size(); j++){
            const auto& p = parts[j];
            // 尝试打开分区，读取第 0 扇区检查 SDRV01 魔数
            std::string dp = p.parent_disk.empty() ? p.device_path : p.parent_disk;
            Volume probe;
            if(probe.open(dp, p.offset_bytes)){
                // 找到了 SecureDrive 卷！
                ctx_.device_path      = dp;
                ctx_.partition_offset = p.offset_bytes;
                ctx_.volume.open(dp, p.offset_bytes);
                ctx_.current_screen = Screen::Unlock;
                ctx_.set_status("自动检测到加密卷（" + p.device_path + "），请输入密码解锁");
                fprintf(stderr, "[PORTABLE] 自动检测到卷: disk=%s part=%s offset=%llu\n",
                        d.device_path.c_str(), p.device_path.c_str(),
                        (unsigned long long)p.offset_bytes);
                fflush(stderr);
                return true;
            }
        }
    }
    return false;
}

// ============================================================
//  解锁界面
// ============================================================
void App::draw_unlock() {
    static char pass_buf[256] = {};

    ImGui::SetCursorPosY(ImGui::GetContentRegionAvail().y * 0.3f);
    ImGui::SetNextItemWidth(300);
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 300) * 0.5f);

    // ---- 数据已擦除状态 ----
    if(ctx_.volume.is_wiped()){
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 500) * 0.5f);
        ImGui::TextColored(ImVec4(1,0.15f,0.15f,1),
            "⚠  数据已被永久擦除！");
        ImGui::Spacing();
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 500) * 0.5f);
        ImGui::TextWrapped(
            "连续 %d 次密码错误触发安全擦除，\n"
            "加密分区所有数据已被零填充覆盖，无法恢复。\n\n"
            "如需继续使用此分区，请返回设备选择页面重新初始化。",
            WIPE_MAX_ATTEMPTS);
        ImGui::Spacing();
        float bx = (ImGui::GetWindowWidth() - 300) * 0.5f;
        ImGui::SetCursorPosX(bx);
        if(ImGui::Button("返回设备选择", ImVec2(300, 30))){
            ctx_.volume.lock();
            ctx_.current_screen = Screen::DeviceSelect;
        }
        return;
    }

    // 锁定状态显示
    if(ctx_.lockout.is_locked()){
        auto secs = ctx_.lockout.seconds_remaining();
        char buf[64];
        snprintf(buf,sizeof(buf),"账户已锁定，请等待 %lld 秒", (long long)secs);
        ImGui::SetCursorPosX((ImGui::GetWindowWidth()-ImGui::CalcTextSize(buf).x)*0.5f);
        ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "%s", buf);
        return;
    }

    // 检查后台解锁是否完成
    if(unlock_running && unlock_done.load(std::memory_order_acquire)){
        unlock_running = false;
        if(unlock_thread && unlock_thread->joinable()) unlock_thread->join();

        if(unlock_ok.load()){
            ctx_.lockout.reset();
            // VFS mount
            if(!ctx_.vfs.mount(&ctx_.volume)){
                ctx_.set_status("卷解锁成功，但 VFS 挂载失败", true);
                return;
            }
            ctx_.current_screen = Screen::FileManager;
            ctx_.current_dir    = "/";
            ctx_.set_status("解锁成功！");
        } else {
            ctx_.lockout.record_failure();
            int fc = ctx_.volume.fail_count();
            if(ctx_.volume.is_wiped()){
                // 数据已被擦除（由 volume.cpp 内部触发）
                ctx_.set_status("密码错误次数过多，数据已被永久擦除！", true);
            } else if(ctx_.lockout.is_locked()){
                ctx_.set_status("密码错误！已连续错误 " +
                                std::to_string(ctx_.lockout.attempts()) +
                                " 次，锁定 5 分钟（连续错误 " +
                                std::to_string(fc) + "/" +
                                std::to_string(WIPE_MAX_ATTEMPTS) + "）", true);
            } else {
                ctx_.set_status("密码错误！连续错误 " +
                                std::to_string(fc) + "/" +
                                std::to_string(WIPE_MAX_ATTEMPTS) +
                                " 次，达到 " + std::to_string(WIPE_MAX_ATTEMPTS) +
                                " 次将永久擦除数据", true);
            }
        }
        return;
    }

    // 后台解锁进行中
    if(unlock_running){
        float cx = (ImGui::GetWindowWidth() - 300) * 0.5f;
        ImGui::SetCursorPosX(cx);
        ImGui::TextColored(ImVec4(0.3f,0.8f,1,1), "正在解锁，请稍候…");
        // 简单动画
        static float t=0; t+=ImGui::GetIO().DeltaTime;
        ImGui::SetCursorPosX(cx);
        ImGui::TextColored(ImVec4(0.3f,0.8f,1,1),
                            "[%s]", (int)(t*2)%2==0 ? "######   " : "   ######");
        return;
    }

    float cx = (ImGui::GetWindowWidth() - 300) * 0.5f;

    ImGui::SetCursorPosX(cx);
    ImGui::Text("[锁定] 请输入密码解锁：");

    ImGui::SetCursorPosX(cx);
    ImGui::SetNextItemWidth(300);
    bool enter = ImGui::InputText("##pass", pass_buf, sizeof(pass_buf),
                                  ImGuiInputTextFlags_Password |
                                  ImGuiInputTextFlags_EnterReturnsTrue);

    ImGui::SetCursorPosX(cx);
    char btn_label[64];
    int fc = ctx_.volume.fail_count();
    int remaining = WIPE_MAX_ATTEMPTS - fc;
    snprintf(btn_label, sizeof(btn_label), "解锁  (剩余: %d/%d)",
             remaining, WIPE_MAX_ATTEMPTS);

    if(enter || ImGui::Button(btn_label, ImVec2(300, 30))){
        do_unlock(std::string(pass_buf));
        memset(pass_buf, 0, sizeof(pass_buf));
    }

    ImGui::SetCursorPosX(cx);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10);
    ImGui::TextDisabled("提示：也可使用紧急密码解锁");

    // 擦除警告（连续错误 >= 7 次时显示醒目警告）
    if(fc >= WIPE_MAX_ATTEMPTS - 3 && fc > 0) {
        ImGui::Spacing();
        ImGui::SetCursorPosX(cx);
        ImGui::PushStyleColor(ImGuiCol_Text,
            ImVec4(1, 0.3f + 0.7f * ((float)fc / WIPE_MAX_ATTEMPTS), 0.1f, 1));
        ImGui::TextWrapped("[!] 连续错误 %d 次，再错 %d 次将永久擦除所有数据！",
                           fc, WIPE_MAX_ATTEMPTS - fc);
        ImGui::PopStyleColor();
    }
}

void App::do_unlock(const std::string& password) {
    if(ctx_.lockout.is_locked()){
        ctx_.set_status("账户已锁定，请稍后再试", true);
        return;
    }
    // 在后台线程执行 Argon2id 密钥派生（CPU 密集，可能需要数秒）
    unlock_done.store(false, std::memory_order_release);
    unlock_ok.store(false, std::memory_order_release);
    unlock_running = true;
    unlock_thread = std::make_unique<std::thread>([this, password](){
        fprintf(stderr, "[UNLOCK] 后台线程开始 Argon2id...\n"); fflush(stderr);
        unlock_ok.store(ctx_.volume.unlock(password), std::memory_order_release);
        fprintf(stderr, "[UNLOCK] Argon2id 完成, result=%d\n", unlock_ok.load());
        fflush(stderr);
        unlock_done.store(true, std::memory_order_release);
    });
}

void App::do_lock() {
    ctx_.volume.lock();
    ctx_.current_screen = Screen::Unlock;
    ctx_.set_status("已锁定");
}

// ============================================================
//  左侧导航树：递归渲染一个目录节点
//  path: VFS 绝对路径（如 "/" 或 "/docs/project"）
//  label: 显示名称
// ============================================================
void App::draw_dir_tree_node(const std::string& path, const std::string& label) {
    // 从缓存获取子目录；若缓存不存在则重新加载（刷新由调用方清 tree_dir_cache 触发）
    bool cache_miss = (ctx_.tree_dir_cache.find(path) == ctx_.tree_dir_cache.end());
    if(cache_miss) {
        auto all = ctx_.vfs.list_dir(path);
        std::vector<FileInfo> subdirs;
        for(auto& f : all) {
            if(f.is_dir) {
#ifdef _WIN32
                if(!is_valid_utf8(f.name)) f.name = ansi_to_utf8(f.name);
#endif
                subdirs.push_back(f);
            }
        }
        std::sort(subdirs.begin(), subdirs.end(),
                  [](const FileInfo& a, const FileInfo& b){ return a.name < b.name; });
        ctx_.tree_dir_cache[path] = std::move(subdirs);
    }

    auto& subdirs = ctx_.tree_dir_cache[path];
    bool has_children = !subdirs.empty();

    // 构造 ImGui 唯一 ID（使用路径）
    ImGui::PushID(path.c_str());

    bool is_selected = (ctx_.current_dir == path);
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_OpenOnArrow;
    if(!has_children) flags |= ImGuiTreeNodeFlags_Leaf;
    if(is_selected)   flags |= ImGuiTreeNodeFlags_Selected;

    // 检查是否已展开（由 tree_expanded 驱动，而不是 ImGui 内部 storage）
    bool was_open = (ctx_.tree_expanded.count(path) > 0);
    if(was_open) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    else         ImGui::SetNextItemOpen(false, ImGuiCond_Always);

    // 图标（使用 ASCII 字符避免字体不支持 emoji）
    const char* icon = has_children ? (was_open ? "[+] " : "[-] ") : "[-] ";
    std::string node_label = std::string(icon) + label;

    bool now_open = ImGui::TreeNodeEx("##node", flags, "%s", node_label.c_str());

    // 单击 → 导航到该目录（注意：单击箭头只展开，单击文字才导航）
    if(ImGui::IsItemClicked(0) && !ImGui::IsItemToggledOpen()) {
        ctx_.current_dir  = path;
        ctx_.cache_dirty  = true;
    }
    // 双击 → 展开/收起（箭头区域也可触发；双击文字切换展开状态）
    if(ImGui::IsItemClicked(0) && ImGui::IsMouseDoubleClicked(0)) {
        if(was_open) ctx_.tree_expanded.erase(path);
        else         ctx_.tree_expanded.insert(path);
    }
    // 箭头点击（IsItemToggledOpen）→ 展开/收起
    if(ImGui::IsItemToggledOpen()) {
        if(was_open) ctx_.tree_expanded.erase(path);
        else         ctx_.tree_expanded.insert(path);
    }

    if(now_open) {
        for(auto& sub : subdirs) {
            std::string child_path = (path == "/" ? "" : path) + "/" + sub.raw_name;
            draw_dir_tree_node(child_path, sub.name);
        }
        ImGui::TreePop();
    }

    ImGui::PopID();
}

// ============================================================
//  文件管理器
// ============================================================
void App::draw_file_manager() {
    static char new_dir_buf[128] = {};
    static char rename_buf[256]  = {};
    static std::string rename_target;
    static bool show_rename = false;

    // ---- 检查后台任务是否完成 ----
    if(!bg_task_done.load(std::memory_order_acquire)){
        // 后台任务进行中，显示进度状态
        const char* title = "操作进行中，请稍候…";
        if(!ctx_.bg_task_type.empty()){
            if(ctx_.bg_task_type == "delete") title = "正在删除，请稍候…";
            else if(ctx_.bg_task_type == "import") title = "正在导入，请稍候…";
        }
        ImGui::TextColored(ImVec4(0.3f,0.8f,1,1), "%s", title);
        ImGui::Spacing();
        // 进度条
        float prog = ctx_.bg_progress.load(std::memory_order_relaxed);
        if(prog >= 0.0f){
            char prog_buf[64];
            snprintf(prog_buf, sizeof(prog_buf), "%.0f%%", prog * 100.0f);
            ImGui::ProgressBar(prog, ImVec2(-1, 0), prog_buf);
            // 当前文件名
            if(!ctx_.bg_progress_label.empty()){
                const char* label_prefix = "正在处理";
                if(ctx_.bg_task_type == "delete") label_prefix = "正在删除";
                else if(ctx_.bg_task_type == "import") label_prefix = "正在导入";
                ImGui::TextDisabled("%s: %s", label_prefix, ctx_.bg_progress_label.c_str());
            }
        }
        ImGui::Spacing();
        return;
    } else if(bg_thread && bg_thread->joinable()){
        bg_thread->join();
        ctx_.bg_progress.store(-1.0f, std::memory_order_relaxed);
        ctx_.bg_progress_label.clear();
        ctx_.bg_task_type.clear();
        ctx_.set_status(bg_task_status);
        ctx_.cache_dirty = true; // 操作完成，刷新目录
        ctx_.tree_dir_cache.clear(); // 同时刷新导航树缓存
    }

    // ---- 刷新目录缓存 ----
    if(ctx_.cache_dirty || ctx_.cached_dir != ctx_.current_dir){
        ctx_.cached_entries = ctx_.vfs.list_dir(ctx_.current_dir);
        // 修复旧文件可能存储的 GBK/ANSI 文件名：转换为 UTF-8 供 ImGui 显示
        // raw_name 保留原始字节，用于实际 VFS 路径操作
#ifdef _WIN32
        for(auto& fi : ctx_.cached_entries){
            if(!is_valid_utf8(fi.name)){
                fi.name = ansi_to_utf8(fi.name);
                // raw_name 已在 read_dir 中设为原始字节，不需要再修改
            }
        }
#endif
        std::sort(ctx_.cached_entries.begin(), ctx_.cached_entries.end(),
                  [](const FileInfo& a, const FileInfo& b){
            if(a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
            return a.name < b.name;
        });
        ctx_.cached_dir  = ctx_.current_dir;
        ctx_.cache_dirty = false;
    }
    auto& entries = ctx_.cached_entries;

    // ---- 磁盘容量信息条 ----
    {
        uint64_t total = ctx_.vfs.total_capacity();
        uint64_t used  = ctx_.vfs.used_capacity();
        uint64_t free_ = ctx_.vfs.free_capacity();

        // 格式化容量显示
        auto fmt_size = [](uint64_t bytes) -> const char* {
            static char buf[64];
            if(bytes < 1ULL << 20)      snprintf(buf, sizeof(buf), "%.0f B", (double)bytes);
            else if(bytes < 1ULL << 30) snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
            else if(bytes < 1ULL << 40) snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0*1024));
            else                         snprintf(buf, sizeof(buf), "%.2f GB", bytes / (1024.0*1024*1024));
            return buf;
        };

        ImGui::TextDisabled("总容量: %s | 已用: %s | 可用: %s",
                           fmt_size(total), fmt_size(used), fmt_size(free_));

        // 使用进度条可视化已用空间占比
        if(total > 0) {
            ImGui::SameLine();
            float usage = (float)((double)used / (double)total);
            char usage_pct[32];
            snprintf(usage_pct, sizeof(usage_pct), "%.1f%%", usage * 100.0f);
            ImGui::ProgressBar(usage, ImVec2(120, 16), usage_pct);
        }
    }

    ImGui::Spacing();

    // ---- 主内容区：左侧导航树 + 右侧工具栏+文件列表 ----
    // 底部留 40px 给状态栏；导航树和文件列表等高
    float content_height = ImGui::GetContentRegionAvail().y - 40.0f;

    // ===== 左侧：目录导航树 =====
    ImGui::BeginChild("nav_tree", ImVec2(220, content_height), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    // 根节点"/"始终显示
    draw_dir_tree_node("/", "[加密盘]");
    ImGui::EndChild();

    ImGui::SameLine();

    // ===== 右侧：工具栏 + 文件列表 =====
    ImGui::BeginChild("right_panel", ImVec2(0, content_height), false);

    // ---- 工具栏 ----
    if(ImGui::Button("导入文件")){
        ImGui::OpenPopup("导入文件##dlg");
    }
    ImGui::SameLine();
    if(ImGui::Button("导入文件夹")){
        std::string folder_path;
#ifdef _WIN32
        BROWSEINFOW bi = {};
        bi.hwndOwner  = GetForegroundWindow();
        bi.lpszTitle  = L"选择要导入的文件夹";
        bi.ulFlags    = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
        LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
        if(pidl){
            wchar_t folderPath[MAX_PATH] = {};
            if(SHGetPathFromIDListW(pidl, folderPath)){
                folder_path = wide_to_utf8(folderPath);
            }
            CoTaskMemFree(pidl);
        }
#elif defined(__APPLE__)
        std::string cmd = "osascript -e 'POSIX path of (choose folder with prompt \"选择要导入的文件夹\")' 2>/dev/null";
        FILE* pipe = popen(cmd.c_str(), "r");
        if(pipe){
            char buf[1024];
            if(fgets(buf, sizeof(buf), pipe)){
                folder_path = buf;
                while(!folder_path.empty() && (folder_path.back()== '\n' || folder_path.back()=='\r'))
                    folder_path.pop_back();
            }
            pclose(pipe);
        }
#else
        std::string cmd = "zenity --file-selection --directory --title=\"选择要导入的文件夹\" 2>/dev/null";
        FILE* pipe = popen(cmd.c_str(), "r");
        if(!pipe){
            cmd = "kdialog --getexistingdirectory \"\" 2>/dev/null";
            pipe = popen(cmd.c_str(), "r");
        }
        if(pipe){
            char buf[1024];
            if(fgets(buf, sizeof(buf), pipe)){
                folder_path = buf;
                while(!folder_path.empty() && (folder_path.back()== '\n' || folder_path.back()=='\r'))
                    folder_path.pop_back();
            }
            pclose(pipe);
        }
#endif
        if(!folder_path.empty()){
            bg_task_done.store(false, std::memory_order_release);
            ctx_.bg_task_type = "import";
            bg_thread = std::make_unique<std::thread>([this, folder_path](){
                ctx_.vfs.begin_batch();
                do_import_folder(folder_path, ctx_.current_dir);
                ctx_.vfs.end_batch();
                bg_task_done.store(true, std::memory_order_release);
            });
        }
    }
    ImGui::SameLine();
    if(ImGui::Button("新建文件夹")){
        ImGui::OpenPopup("新建文件夹##dlg");
    }
    ImGui::SameLine();
    if(ImGui::Button("刷新")){
        ctx_.cache_dirty = true;
        ctx_.tree_dir_cache.clear(); // 同时清空导航树缓存
    }
    ImGui::SameLine();
    // 当前路径
    ImGui::TextDisabled("路径：%s", ctx_.current_dir.c_str());
    if(ctx_.current_dir != "/"){
        ImGui::SameLine();
        if(ImGui::Button("↑ 上级")){
            size_t pos = ctx_.current_dir.rfind('/');
            if(pos == 0) ctx_.current_dir = "/";
            else ctx_.current_dir = ctx_.current_dir.substr(0,pos);
            ctx_.cache_dirty = true;
        }
    }

    ImGui::Separator();

    // ---- 文件列表（使用缓存数据）----
    float list_height = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("filelist", ImVec2(0, list_height), true);
    if(ImGui::BeginTable("files", 4,
        ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY))
    {
        ImGui::TableSetupColumn("名称",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("类型",   ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("大小",   ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("修改时间",ImGuiTableColumnFlags_WidthFixed, 150);
        ImGui::TableHeadersRow();

        for(auto& f : entries){
            ImGui::TableNextRow();
            ImGui::TableNextColumn();

            ImGui::PushID(f.name.c_str());
            if(ImGui::Selectable(f.name.c_str(), false,
                                 ImGuiSelectableFlags_SpanAllColumns |
                                 ImGuiSelectableFlags_AllowDoubleClick))
            {
                if(ImGui::IsMouseDoubleClicked(0)){
                    // 使用 raw_name 构造 VFS 路径（磁盘上的真实字节名称）
                    std::string full = (ctx_.current_dir=="/" ? "" : ctx_.current_dir)
                                       + "/" + f.raw_name;
                    if(f.is_dir){
                        // 双击目录：进入该目录，并在导航树中展开到该路径
                        ctx_.current_dir = full;
                        ctx_.cache_dirty = true;
                        // 展开导航树中的父路径直到当前目录
                        {
                            std::string p = full;
                            while(p != "/" && !p.empty()) {
                                ctx_.tree_expanded.insert(p);
                                size_t pos = p.rfind('/');
                                if(pos == 0) { ctx_.tree_expanded.insert("/"); break; }
                                p = p.substr(0, pos);
                            }
                            ctx_.tree_expanded.insert("/");
                        }
                    } else {
                        // 双击文件 → 导出到临时目录并用系统程序打开
                        do_open_file(full);
                    }
                }
            }
            // 右键菜单
            if(ImGui::BeginPopupContextItem("##ctx")){
                // 使用 raw_name 构造 VFS 路径
                std::string full = (ctx_.current_dir=="/" ? "" : ctx_.current_dir)
                                   + "/" + f.raw_name;
                if(!f.is_dir && ImGui::MenuItem("打开"))
                    do_open_file(full);
                if(!f.is_dir && ImGui::MenuItem("导出到桌面"))
                    do_export_file(full, "");
                if(ImGui::MenuItem("重命名")){
                    rename_target = full;
                    strncpy(rename_buf, f.name.c_str(), sizeof(rename_buf));
                    show_rename = true;
                }
                if(ImGui::MenuItem("删除")){
                    do_delete(full, f.is_dir);
                }
                ImGui::EndPopup();
            }

            ImGui::TableNextColumn();
            ImGui::TextDisabled(f.is_dir ? "目录" : "文件");
            ImGui::TableNextColumn();
            if(!f.is_dir){
                if(f.size < 1024)      ImGui::Text("%llu B",  (unsigned long long)f.size);
                else if(f.size<1<<20)  ImGui::Text("%.1f KB", f.size/1024.0);
                else if(f.size<1<<30)  ImGui::Text("%.1f MB", f.size/(1024.0*1024));
                else                   ImGui::Text("%.2f GB", f.size/(1024.0*1024*1024));
            } else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            char tbuf[32];
            struct tm* t = localtime((time_t*)&f.mtime);
            strftime(tbuf,sizeof(tbuf),"%Y-%m-%d %H:%M",t);
            ImGui::TextDisabled("%s", tbuf);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild(); // filelist

    ImGui::EndChild(); // right_panel

    // ---- 底部状态 ----
    ImGui::TextDisabled("共 %d 个项目", (int)entries.size());

    // ---- 弹窗：新建文件夹 ----
    if(ImGui::BeginPopupModal("新建文件夹##dlg", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("文件夹名称：");
        ImGui::InputText("##newdir", new_dir_buf, sizeof(new_dir_buf));
        if(ImGui::Button("创建") && strlen(new_dir_buf)>0){
            std::string path = (ctx_.current_dir=="/" ? "" : ctx_.current_dir)
                               + "/" + new_dir_buf;
            // 启动后台任务
            bg_task_done.store(false, std::memory_order_release);
            std::string p = path;
            bg_thread = std::make_unique<std::thread>([this, p](){
                bool ok = ctx_.vfs.make_dir(p);
                bg_task_status   = ok ? ("文件夹已创建: " + p) : "创建失败";
                bg_task_success  = ok;
                bg_task_done.store(true, std::memory_order_release);
            });
            memset(new_dir_buf,0,sizeof(new_dir_buf));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if(ImGui::Button("取消")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- 弹窗：导入文件 ----
    static char import_src[512] = {};
    if(ImGui::BeginPopupModal("导入文件##dlg", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("选择要导入的文件：");
        ImGui::SetNextItemWidth(400);
        ImGui::InputText("##src", import_src, sizeof(import_src));
        ImGui::SameLine();
        if(ImGui::Button("浏览...##browse")){
            std::vector<std::string> selected_paths;
#ifdef _WIN32
            // Windows: GetOpenFileNameW 多选
            static wchar_t multi_buf[65536] = {};
            memset(multi_buf, 0, sizeof(multi_buf));
            OPENFILENAMEW ofn = {};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner   = GetForegroundWindow();
            ofn.lpstrFilter = L"所有文件\0*.*\0";
            ofn.lpstrFile   = multi_buf;
            ofn.nMaxFile    = (DWORD)(sizeof(multi_buf)/sizeof(wchar_t));
            ofn.Flags       = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT | OFN_EXPLORER;
            ofn.lpstrTitle  = L"选择要导入的文件（可多选）";
            if(GetOpenFileNameW(&ofn)){
                const wchar_t* p = multi_buf;
                std::wstring dir = p;
                p += dir.size() + 1;
                if(*p == L'\0'){
                    selected_paths.push_back(wide_to_utf8(dir));
                } else {
                    while(*p != L'\0'){
                        std::wstring fname_w = p;
                        selected_paths.push_back(wide_to_utf8(dir + L"\\" + fname_w));
                        p += fname_w.size() + 1;
                    }
                }
            }
#elif defined(__APPLE__)
            // macOS: osascript 多选文件
            std::string cmd = "osascript -e 'POSIX path of (choose file with prompt \"选择要导入的文件\" multiple selections allowed true)' 2>/dev/null";
            FILE* pipe = popen(cmd.c_str(), "r");
            if(pipe){
                char buf[4096];
                while(fgets(buf, sizeof(buf), pipe)){
                    std::string path(buf);
                    // 去掉末尾换行
                    while(!path.empty() && (path.back()== '\n' || path.back()=='\r'))
                        path.pop_back();
                    if(!path.empty()) selected_paths.push_back(path);
                }
                pclose(pipe);
            }
#else
            // Linux: zenity 多选文件
            std::string cmd = "zenity --file-selection --multiple --title=\"选择要导入的文件\" --separator='|' 2>/dev/null";
            FILE* pipe = popen(cmd.c_str(), "r");
            if(!pipe){
                // fallback: kdialog
                cmd = "kdialog --getopenfilename \"\" \"*\" --multiple --separator='|' 2>/dev/null";
                pipe = popen(cmd.c_str(), "r");
            }
            if(pipe){
                char buf[4096];
                if(fgets(buf, sizeof(buf), pipe)){
                    std::string line(buf);
                    while(!line.empty() && (line.back()== '\n' || line.back()=='\r'))
                        line.pop_back();
                    // zenity 用 | 分隔多个文件
                    size_t pos = 0;
                    while(pos < line.size()){
                        size_t sep = line.find('|', pos);
                        if(sep == std::string::npos) sep = line.size();
                        std::string path = line.substr(pos, sep-pos);
                        if(!path.empty()) selected_paths.push_back(path);
                        pos = sep + 1;
                    }
                }
                pclose(pipe);
            }
#endif
            if(!selected_paths.empty()){
                strncpy(import_src, selected_paths[0].c_str(), sizeof(import_src)-1);
                import_src[sizeof(import_src)-1] = '\0';
                if(selected_paths.size() > 1){
                    bg_task_done.store(false, std::memory_order_release);
                    ctx_.bg_task_type = "import";
                    bg_thread = std::make_unique<std::thread>([this, selected_paths](){
                        do_import_paths(selected_paths);
                        bg_task_done.store(true, std::memory_order_release);
                    });
                    memset(import_src,0,sizeof(import_src));
                    ImGui::CloseCurrentPopup();
                }
            } else {
                ctx_.set_status("未选择文件", true);
            }
        }
        ImGui::Separator();
        if(ImGui::Button("导入", ImVec2(120, 0)) && strlen(import_src)>0){
            std::string src(import_src); // UTF-8 路径
            // 用宽字符路径提取文件名，避免 GBK/UTF-8 混淆
#ifdef _WIN32
            std::wstring wsrc = utf8_to_wide(src);
            std::string fname = wide_to_utf8(fs::path(wsrc).filename().wstring());
#else
            std::string fname = fs::path(src).filename().string();
#endif
            std::string dst = (ctx_.current_dir=="/" ? "" : ctx_.current_dir)
                              + "/" + fname;
            // 启动后台任务
            bg_task_done.store(false, std::memory_order_release);
            ctx_.bg_task_type = "import";
            bg_thread = std::make_unique<std::thread>([this, src, dst](){
                do_import_file(src, dst);
                bg_task_done.store(true, std::memory_order_release);
            });
            memset(import_src,0,sizeof(import_src));
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if(ImGui::Button("取消", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- 弹窗：重命名 ----
    if(show_rename){
        ImGui::OpenPopup("重命名##dlg");
        show_rename = false;
    }
    if(ImGui::BeginPopupModal("重命名##dlg", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::Text("新名称：");
        ImGui::SetNextItemWidth(300);
        ImGui::InputText("##ren", rename_buf, sizeof(rename_buf));
        if(ImGui::Button("确认") && strlen(rename_buf)>0){
            std::string parent = rename_target.substr(0, rename_target.rfind('/'));
            std::string new_path = parent + "/" + rename_buf;
            if(ctx_.vfs.rename(rename_target, new_path))
                ctx_.set_status("已重命名");
            else
                ctx_.set_status("重命名失败", true);
            ctx_.cache_dirty = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if(ImGui::Button("取消")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ============================================================
//  文件操作实现
// ============================================================
void App::do_import_file(const std::string& ext_path,
                          const std::string& vfs_path)
{
    // 提取文件名用于进度显示
#ifdef _WIN32
    std::wstring wsrc = utf8_to_wide(ext_path);
    std::string fname = wide_to_utf8(fs::path(wsrc).filename().wstring());
#else
    std::string fname = fs::path(ext_path).filename().string();
#endif
    ctx_.bg_progress_label = fname;
    ctx_.bg_progress.store(0.0f, std::memory_order_relaxed);

    // 用宽字符路径打开文件，兼容中文路径
#ifdef _WIN32
    std::ifstream f(utf8_to_wide(ext_path), std::ios::binary);
#else
    std::ifstream f(ext_path, std::ios::binary);
#endif
    if(!f){
        ctx_.set_status("无法打开文件: " + ext_path, true);
        return;
    }
    // 读取文件
    f.seekg(0, std::ios::end);
    std::streamsize file_size = f.tellg();
    f.seekg(0, std::ios::beg);
    ctx_.bg_progress_label = "读取: " + fname;
    ctx_.bg_progress.store(0.2f, std::memory_order_relaxed);
    std::vector<uint8_t> data;
    if(file_size > 0){
        data.resize((size_t)file_size);
        if(!f.read((char*)data.data(), file_size)){
            ctx_.set_status("读取文件失败: " + ext_path, true);
            return;
        }
    }
    // 写入 VFS
    ctx_.bg_progress_label = "写入: " + fname;
    ctx_.bg_progress.store(0.6f, std::memory_order_relaxed);
    if(ctx_.vfs.write_file(vfs_path, data.data(), data.size()))
        ctx_.set_status("已导入: " + vfs_path + " (" +
                        std::to_string(data.size()) + " 字节)");
    else
        ctx_.set_status("导入失败: " + vfs_path, true);
    ctx_.bg_progress.store(1.0f, std::memory_order_relaxed);
}

void App::do_export_file(const std::string& vfs_path,
                          const std::string& ext_dir)
{
    std::vector<uint8_t> data;
    if(!ctx_.vfs.read_file(vfs_path, data)){
        ctx_.set_status("读取文件失败: " + vfs_path, true); return;
    }
    // 导出到桌面或指定目录
    std::string dst_dir = ext_dir;
    if(dst_dir.empty()){
#ifdef _WIN32
        dst_dir = std::string(getenv("USERPROFILE")) + "\\Desktop";
#else
        dst_dir = std::string(getenv("HOME")) + "/Desktop";
#endif
    }
    // 用宽字符路径提取文件名（vfs_path 内是 UTF-8）
#ifdef _WIN32
    std::wstring wvfs = utf8_to_wide(vfs_path);
    std::string fname = wide_to_utf8(fs::path(wvfs).filename().wstring());
    std::string dst   = dst_dir + "\\" + fname;
    std::ofstream f(utf8_to_wide(dst), std::ios::binary);
#else
    std::string fname = fs::path(vfs_path).filename().string();
    std::string dst   = dst_dir + "/" + fname;
    std::ofstream f(dst, std::ios::binary);
#endif
    if(!f){ ctx_.set_status("无法写入: " + dst, true); return; }
    f.write((const char*)data.data(), data.size());
    ctx_.set_status("已导出至: " + dst);
}

// 递归导入外部文件夹到 VFS（两阶段：扫描 → 逐个导入，带进度条）
// ext_folder: 外部目录（UTF-8）
// vfs_parent: VFS 目标父目录（如 "/" 或 "/资料"）
void App::do_import_folder(const std::string& ext_folder, const std::string& vfs_parent)
{
    // 取文件夹名
#ifdef _WIN32
    std::wstring wfolder = utf8_to_wide(ext_folder);
    std::string folder_name = wide_to_utf8(fs::path(wfolder).filename().wstring());
    if(folder_name.empty()) folder_name = wide_to_utf8(fs::path(wfolder).stem().wstring());
#else
    std::string folder_name = fs::path(ext_folder).filename().string();
#endif

    // ---- 阶段 1：递归扫描外部文件系统，收集所有文件条目 ----
    // 每个条目: {外部绝对路径, VFS目标路径, 文件名}
    struct ImportItem {
        std::string ext_path;  // 外部文件完整路径
        std::string vfs_path;  // VFS 内目标路径
        std::string name;      // 文件名（用于进度显示）
    };

    std::vector<ImportItem> items;
    std::vector<std::string> vfs_dirs; // 需要创建的 VFS 目录

    // 提取文件夹名用于进度显示
    ctx_.bg_progress_label = "扫描: " + folder_name;

    std::function<void(const std::string&, const std::string&)> scan =
        [&](const std::string& ext_dir, const std::string& vfs_dir) {
        // 记录需要创建的目录
        vfs_dirs.push_back(vfs_dir);
#ifdef _WIN32
        for(auto& entry : fs::directory_iterator(utf8_to_wide(ext_dir)))
#else
        for(auto& entry : fs::directory_iterator(ext_dir))
#endif
        {
#ifdef _WIN32
            std::string item_path = wide_to_utf8(entry.path().wstring());
            std::string item_name = wide_to_utf8(entry.path().filename().wstring());
#else
            std::string item_path = entry.path().string();
            std::string item_name = entry.path().filename().string();
#endif
            if(entry.is_directory()){
                std::string child_vfs = (vfs_dir == "/" ? "" : vfs_dir) + "/" + item_name;
                scan(item_path, child_vfs);
            } else if(entry.is_regular_file()){
                std::string child_vfs = (vfs_dir == "/" ? "" : vfs_dir) + "/" + item_name;
                items.push_back({item_path, child_vfs, item_name});
            }
        }
    };

    std::string vfs_dir = (vfs_parent == "/" ? "" : vfs_parent) + "/" + folder_name;
    scan(ext_folder, vfs_dir);

    int total_files = (int)items.size();
    int total_dirs  = (int)vfs_dirs.size();

    // ---- 阶段 2：逐个创建目录 + 导入文件 ----
    int ok_cnt = 0, fail_cnt = 0;

    // 先创建所有目录（VFS 侧）
    for(int i = 0; i < total_dirs; i++){
        ctx_.vfs.make_dir(vfs_dirs[i]); // 已存在则忽略
    }

    // 逐个导入文件，更新进度
    for(int i = 0; i < total_files; i++){
        auto& item = items[i];
        ctx_.bg_progress_label = item.name;
        ctx_.bg_progress.store((float)i / (float)std::max(total_files, 1),
                                std::memory_order_relaxed);
        // 读取外部文件
#ifdef _WIN32
        std::ifstream f(utf8_to_wide(item.ext_path), std::ios::binary);
#else
        std::ifstream f(item.ext_path, std::ios::binary);
#endif
        if(!f){
            fail_cnt++;
            continue;
        }
        f.seekg(0, std::ios::end);
        std::streamsize fsize = f.tellg();
        f.seekg(0, std::ios::beg);
        std::vector<uint8_t> data;
        if(fsize > 0){
            data.resize((size_t)fsize);
            if(!f.read((char*)data.data(), fsize)){
                fail_cnt++;
                continue;
            }
        }
        if(ctx_.vfs.write_file(item.vfs_path, data.data(), data.size()))
            ok_cnt++;
        else
            fail_cnt++;
    }
    ctx_.bg_progress.store(1.0f, std::memory_order_relaxed);
    ctx_.set_status("已导入: " + vfs_dir + " (" + std::to_string(ok_cnt) + " 个文件" +
                    (fail_cnt > 0 ? "，" + std::to_string(fail_cnt) + " 个失败" : "") + ")");
    ctx_.cache_dirty = true;
}

// 批量导入一组路径（文件/文件夹混合，来自多选对话框或拖拽）
void App::do_import_paths(const std::vector<std::string>& paths)
{
    int ok_cnt = 0, fail_cnt = 0;
    int total = (int)paths.size();

    // ★ 批量模式：延迟 bitmap_commit，整个导入只刷一次位图
    ctx_.vfs.begin_batch();

    for(int idx = 0; idx < total; idx++){
        const auto& p = paths[idx];
        // 更新进度
        ctx_.bg_progress.store((float)idx / (float)std::max(total, 1),
                                std::memory_order_relaxed);
#ifdef _WIN32
        bool is_dir = fs::is_directory(utf8_to_wide(p));
        ctx_.bg_progress_label = wide_to_utf8(fs::path(utf8_to_wide(p)).filename().wstring());
#else
        bool is_dir = fs::is_directory(p);
        ctx_.bg_progress_label = fs::path(p).filename().string();
#endif
        if(is_dir){
            do_import_folder(p, ctx_.current_dir);
            ok_cnt++;
        } else {
#ifdef _WIN32
            std::string fname = wide_to_utf8(fs::path(utf8_to_wide(p)).filename().wstring());
#else
            std::string fname = fs::path(p).filename().string();
#endif
            std::string vfs_path = (ctx_.current_dir == "/" ? "" : ctx_.current_dir) + "/" + fname;
            // ★ 单文件也用 batch 模式包裹
            ctx_.vfs.begin_batch();
            do_import_file(p, vfs_path);
            ctx_.vfs.end_batch();
            ok_cnt++;
        }
    }
    ctx_.bg_progress.store(1.0f, std::memory_order_relaxed);
    bg_task_status = "批量导入完成：" + std::to_string(ok_cnt) + " 个成功，" + std::to_string(fail_cnt) + " 个失败";
    ctx_.cache_dirty = true;

    // ★ 结束批量模式，一次性刷写位图到磁盘
    ctx_.vfs.end_batch();
}

// GLFW drop 回调：把路径推入队列（GLFW 回调在主线程，安全）
void App::on_drop(int count, const char** paths)
{
    if(ctx_.current_screen != Screen::FileManager) return;
    std::lock_guard<std::mutex> lk(drop_mutex_);
    for(int i = 0; i < count; i++){
#ifdef _WIN32
        // GLFW on Windows 传来的 path 是 UTF-8
        drop_queue_.push(std::string(paths[i]));
#else
        drop_queue_.push(std::string(paths[i]));
#endif
    }
}

// 递归删除：先删子项，再删自身（同步版本，用于少量文件快速删除）
bool App::delete_recursive(const std::string& vfs_path, bool is_dir) {
    if(!is_dir){
        bool ok = ctx_.vfs.remove_file(vfs_path);
        if(!ok){
            ok = ctx_.vfs.remove_dir(vfs_path);
        }
        return ok;
    }

    // 列出子项，递归删除
    auto entries = ctx_.vfs.list_dir(vfs_path);

    // 构造"干净"父路径前缀：去掉尾部斜杠，确保只有一个 /
    std::string prefix = vfs_path;
    while(prefix.size() > 1 && prefix.back() == '/') prefix.pop_back();

    for(auto& e : entries){
        std::string child = (prefix == "/" ? "" : prefix) + "/" + e.raw_name;
        if(!delete_recursive(child, e.is_dir)){
            return false;
        }
    }
    // 子项清空后再删目录本身
    bool ok = ctx_.vfs.remove_dir(vfs_path);
    return ok;
}

// 后台异步删除：扫描 → batch 删除 → 进度更新
void App::do_delete(const std::string& vfs_path, bool is_dir) {
    bg_task_done.store(false, std::memory_order_release);
    ctx_.bg_task_type = "delete";

    // 提取文件名用于进度显示
    std::string display_name = vfs_path;
    size_t last_slash = vfs_path.rfind('/');
    if(last_slash != std::string::npos) display_name = vfs_path.substr(last_slash + 1);

    bg_thread = std::make_unique<std::thread>([this, vfs_path, is_dir, display_name](){
        if(!is_dir){
            // 单文件：无需 batch，直接删
            ctx_.bg_progress.store(0.5f, std::memory_order_relaxed);
            ctx_.bg_progress_label = display_name;
            bool ok = ctx_.vfs.remove_file(vfs_path);
            if(!ok){
                ok = ctx_.vfs.remove_dir(vfs_path); // 兼容 mode 异常
            }
            bg_task_status  = ok ? ("已删除: " + vfs_path) : ("删除失败: " + vfs_path);
            bg_task_success = ok;
            ctx_.bg_progress.store(1.0f, std::memory_order_relaxed);
            bg_task_done.store(true, std::memory_order_release);
            return;
        }

        // 目录：两阶段删除
        // 阶段 1：递归扫描，收集所有待删除条目（文件 + 子目录 + 自身）
        // 条目顺序：文件在前，目录在后（从叶子到根），确保目录删时已空
        std::vector<std::pair<std::string,bool>> items; // path, is_dir
        std::function<bool(const std::string&, bool)> scan =
            [&](const std::string& path, bool dir) -> bool {
            if(!dir){
                items.push_back({path, false});
                return true;
            }
            auto entries = ctx_.vfs.list_dir(path);
            std::string prefix = path;
            while(prefix.size() > 1 && prefix.back() == '/') prefix.pop_back();

            // 先递归子目录
            for(auto& e : entries){
                std::string child = (prefix == "/" ? "" : prefix) + "/" + e.raw_name;
                if(e.is_dir){
                    if(!scan(child, true)) return false;
                }
            }
            // 再收集文件
            for(auto& e : entries){
                std::string child = (prefix == "/" ? "" : prefix) + "/" + e.raw_name;
                if(!e.is_dir){
                    items.push_back({child, false});
                }
            }
            // 目录自身（此时子项已全部在 items 中，删除完子项后目录为空）
            items.push_back({path, true});
            return true;
        };

        ctx_.bg_progress.store(0.0f, std::memory_order_relaxed);
        ctx_.bg_progress_label = "扫描: " + display_name;
        bool scan_ok = scan(vfs_path, true);

        if(!scan_ok){
            bg_task_status  = "删除失败: 无法扫描 " + vfs_path;
            bg_task_success = false;
            bg_task_done.store(true, std::memory_order_release);
            return;
        }

        int total = (int)items.size();

        // 阶段 2：batch 模式逐个删除
        ctx_.vfs.begin_batch();
        bool all_ok = true;
        int deleted = 0;
        for(int i = 0; i < total; i++){
            auto& [item_path, item_is_dir] = items[i];
            // 提取文件名用于标签
            std::string item_name = item_path;
            size_t pos = item_path.rfind('/');
            if(pos != std::string::npos) item_name = item_path.substr(pos + 1);
            ctx_.bg_progress_label = item_name;
            ctx_.bg_progress.store((float)i / (float)std::max(total, 1),
                                  std::memory_order_relaxed);

            bool ok;
            if(item_is_dir){
                ok = ctx_.vfs.remove_dir(item_path);
            } else {
                ok = ctx_.vfs.remove_file(item_path);
                if(!ok) ok = ctx_.vfs.remove_dir(item_path); // 兼容
            }
            if(!ok){
                all_ok = false;
                // 不中止，尽量多删
            }
            deleted++;
        }
        ctx_.vfs.end_batch(); // 一次性刷写位图
        ctx_.bg_progress.store(1.0f, std::memory_order_relaxed);

        if(all_ok){
            bg_task_status = "已删除: " + vfs_path + " (" + std::to_string(deleted) + " 个条目)";
        } else {
            bg_task_status = "删除部分完成: " + vfs_path + " (" + std::to_string(deleted) + "/" + std::to_string(total) + ")";
        }
        bg_task_success = all_ok;
        bg_task_done.store(true, std::memory_order_release);
    });
}

void App::do_open_file(const std::string& vfs_path) {
    std::vector<uint8_t> data;
    if(!ctx_.vfs.read_file(vfs_path, data)){
        ctx_.set_status("读取文件失败: " + vfs_path, true); return;
    }
#ifdef _WIN32
    // 提取文件名（raw_name 可能是 GBK 也可能是 UTF-8）
    std::string raw_fname(fs::path(utf8_to_wide(vfs_path)).filename() == L""
        ? wide_to_utf8(utf8_to_wide(vfs_path))  // fallback
        : wide_to_utf8(fs::path(utf8_to_wide(vfs_path)).filename().wstring()));
    // 如果 vfs_path 本身不是合法 UTF-8，说明是旧 GBK 文件名，先转成 UTF-8
    std::string raw_path = vfs_path;
    std::wstring wraw;
    if(!is_valid_utf8(vfs_path)){
        // GBK raw → wide → UTF-8
        int wlen = MultiByteToWideChar(CP_ACP, 0, vfs_path.c_str(), -1, nullptr, 0);
        wraw.resize(wlen);
        MultiByteToWideChar(CP_ACP, 0, vfs_path.c_str(), -1, wraw.data(), wlen);
        wraw.pop_back();
    } else {
        wraw = utf8_to_wide(vfs_path);
    }
    std::string fname = wide_to_utf8(fs::path(wraw).filename().wstring());

    // 获取系统临时目录
    wchar_t tmpdir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpdir);
    std::wstring wtmpdir(tmpdir);
    // 在临时目录下建立 SecureDrive 子目录
    std::wstring wsubdir = wtmpdir + L"SecureDrive\\";
    CreateDirectoryW(wsubdir.c_str(), nullptr);

    std::wstring wfname = utf8_to_wide(fname);
    std::wstring wdst = wsubdir + wfname;

    // 写出临时文件
    std::ofstream f(wdst, std::ios::binary);
    if(!f){ ctx_.set_status("无法创建临时文件: " + fname, true); return; }
    f.write((const char*)data.data(), data.size());
    f.close();

    // 用系统默认程序打开
    HINSTANCE result = ShellExecuteW(nullptr, L"open", wdst.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if((INT_PTR)result <= 32){
        ctx_.set_status("无法打开文件（没有关联的程序）: " + fname, true);
    } else {
        ctx_.set_status("已用系统程序打开: " + fname + "（临时副本）");
    }
#else
    // Linux/macOS: 导出到 /tmp 并用 xdg-open/open 打开
    std::string fname = fs::path(vfs_path).filename().string();
    std::string dst   = "/tmp/" + fname;
    {
        std::ofstream f(dst, std::ios::binary);
        if(!f){ ctx_.set_status("无法创建临时文件", true); return; }
        f.write((const char*)data.data(), data.size());
    }
#  ifdef __APPLE__
    system(("open \"" + dst + "\" &").c_str());
#  else
    system(("xdg-open \"" + dst + "\" &").c_str());
#  endif
    ctx_.set_status("已用系统程序打开: " + fname + "（临时副本）");
#endif
}

// ============================================================
//  状态栏
// ============================================================
void App::draw_statusbar() {
    ImGui::Separator();
    if(ctx_.status_is_error)
        ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "[!] %s", ctx_.status_msg.c_str());
    else
        ImGui::TextDisabled("[OK] %s", ctx_.status_msg.c_str());
}
