# SecureDrive 跨平台移植代码修改清单

## 文件修改概览

| 文件 | 修改类型 | 平台 | 优先级 |
|------|---------|------|--------|
| `src/disk/disk.h` | 修改 | 全平台 | P0 |
| `src/disk/disk.cpp` | 大幅修改 | macOS/Linux | P0 |
| `src/volume/volume.cpp` | 修改 | macOS/Linux | P0 |
| `src/ui/app.h` | 修改 | 全平台 | P0 |
| `src/ui/app.cpp` | 大幅修改 | macOS/Linux | P0 |
| `src/ui/init_wizard.cpp` | 大幅修改 | macOS/Linux | P0 |
| `src/main.cpp` | 小幅修改 | 全平台 | P1 |
| `CMakeLists.txt` | 修改 | macOS/Linux | P1 |

---

## 1. src/disk/disk.h

### 1.1 将 Windows 特有的函数声明改为全平台通用
- `dismount_volumes_on_disk(int disk_num)` —— 添加 macOS/Linux 实现
- `extract_disk_number(const std::string&)` —— 保持 Windows 特有，macOS/Linux 不需要
- `create_portable_layout(...)` —— 添加 macOS/Linux 实现
- `format_volume_fat32(char, ...)` —— 改为接受挂载路径而非盘符：`format_volume_fat32(const std::string& device_path, const std::string& mount_point, const std::string& label)`
- `wait_for_drive_letter(...)` —— 改为 `wait_for_mount_point(...)` 返回挂载路径
- `copy_exe_to(...)` —— 保持通用

### 1.2 新增结构体/函数
```cpp
// 跨平台挂载信息
struct MountInfo {
    std::string device_path;   // 设备路径
    std::string mount_point;   // 挂载点路径
    std::string fs_type;       // 文件系统类型
};

// 等待分区挂载（返回挂载点路径）
std::string wait_for_mount_point(const std::string& device_path, int timeout_ms = 8000);

// 获取分区的挂载点
std::string get_mount_point(const std::string& device_path);

// 复制文件/目录（跨平台）
bool copy_file_or_dir(const std::string& src, const std::string& dst);
```

---

## 2. src/disk/disk.cpp

### 2.1 macOS 实现区（`#elif defined(__APPLE__)`）

**`create_portable_layout`：**
```cpp
// 使用 diskutil partitionDisk 命令
// MBR 分区表：100MB FAT32 + 剩余空间
std::string cmd = "diskutil partitionDisk " + disk_path +
    " MBR MS-DOS FAT32 SDRV_BOOT 100Mi Free Space SDRV_CRYPTO 0b";
```

**`format_volume_fat32`：**
```cpp
// diskutil eraseVolume FAT32 SDRV_BOOT /dev/diskNs1
```

**`dismount_volumes_on_disk`：**
```cpp
// diskutil unmountDisk /dev/diskN
```

**`wait_for_mount_point`：**
```cpp
// 轮询 /Volumes/ 目录
// 通过 diskutil info /dev/diskNsM 获取 Volume Name
```

**`enum_partitions` 改进：**
```cpp
// 使用 diskutil list -plist 获取精确的分区偏移和大小
```

### 2.2 Linux 实现区（`#else`）

**`create_portable_layout`：**
```cpp
// 使用 parted 命令
// parted -s /dev/sdX mklabel msdos
// parted -s /dev/sdX mkpart primary fat32 1MiB 101MiB
// parted -s /dev/sdX mkpart primary 101MiB 100%
// partprobe /dev/sdX  (刷新内核分区表)
```

**`format_volume_fat32`：**
```cpp
// mkfs.vfat -F 32 -n SDRV_BOOT /dev/sdX1
```

**`dismount_volumes_on_disk`：**
```cpp
// 读取 /proc/mounts，找到 /dev/sdX* 的挂载点，逐个 umount
```

**`wait_for_mount_point`：**
```cpp
// 轮询 /media/$USER/、/run/media/$USER/、/mnt/
// 通过 blkid /dev/sdX1 获取 LABEL
```

**`enum_partitions` 改进：**
```cpp
// 使用 lsblk -J -b -o NAME,SIZE,TYPE,START 获取 JSON 格式分区信息
```

### 2.3 通用辅助函数（所有平台）

**`copy_file_or_dir`：**
```cpp
// 使用 std::filesystem::copy 或系统命令
// 处理目录递归复制
```

---

## 3. src/volume/volume.cpp

### 3.1 `create_inplace` 简化
- 免格式化加密功能已删除，但 `create_inplace` 代码仍保留在 volume.cpp 中
- 该函数在 init_wizard.cpp 的 `inplace_mode` 分支中被调用
- 由于 `wizard_existing` 永远为 false（UI 已删除免格式化加密按钮），`inplace_mode` 永远不会触发
- **决策**：保留函数但大幅简化，删除 Windows 特有的双句柄切换复杂逻辑
- macOS/Linux 版本不需要卷设备锁定/解锁的复杂处理

### 3.2 Windows 特有辅助函数
- `find_volume_path_on_disk` 和 `lock_volume_by_path`（第14-73行）
- 这些只在 `_WIN32` 下编译，不影响 macOS/Linux

---

## 4. src/ui/app.cpp

### 4.1 文件选择对话框（所有非 Windows 平台）

**导入文件对话框（第1142-1191行）：**
- macOS：使用 `osascript` 多选文件
- Linux：使用 `zenity --file-selection --multiple`

**导出目录对话框（第893-900行）：**
- macOS：使用 `osascript` 选择文件夹
- Linux：使用 `zenity --file-selection --directory`

**打开外部文件（第964-988行）：**
- macOS：`open "file_path"`
- Linux：`xdg-open "file_path"`

### 4.2 权限检测（第23-104行）
- macOS/Linux：`geteuid() == 0`
- 已存在，但 init_wizard.cpp 中没有对应处理

### 4.3 编码转换函数（第42-100行）
- `wide_to_utf8`, `utf8_to_wide`, `ansi_to_utf8`, `utf8_to_ansi`
- 这些只在 `_WIN32` 下定义
- macOS/Linux 路径天然就是 UTF-8，不需要转换

### 4.4 便携模式自动检测（`auto_detect_portable_volume`，第584-611行）
- 当前实现已跨平台通用（使用 Volume::open 检测魔数）
- 无需修改

### 4.5 导出文件默认目录（第1307-1312行）
- 已跨平台（`USERPROFILE` vs `HOME`）

### 4.6 文件 I/O（第1254-1324行）
- Windows 使用宽字符路径，macOS/Linux 使用 UTF-8 路径
- 已条件编译处理

---

## 5. src/ui/app.h

### 5.1 新增字段
```cpp
// 三平台可执行文件源路径（自动发现）
std::string portable_win_src;   // Windows exe 路径
std::string portable_mac_src;   // macOS .app 路径
std::string portable_linux_src; // Linux binary 路径
```

### 5.2 删除字段
- 当前 `portable_exe_src` 和 `portable_mac_src` 存在但含义不清
- 需要重命名为更清晰的名称

---

## 6. src/ui/init_wizard.cpp

### 6.1 平台头文件（第15-16行）
```cpp
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif
```

### 6.2 `GetTickCount64()` 和 `Sleep()`（第231行等）
- Windows：`GetTickCount64()` / `Sleep(ms)`
- macOS/Linux：`clock_gettime(CLOCK_MONOTONIC)` / `usleep(ms * 1000)` 或 `std::this_thread::sleep_for`

### 6.3 `seh_init_volume`（第439-455行）
- Windows SEH：`__try` / `__except`
- macOS/Linux：使用标准 C++ `try` / `catch(...)`

### 6.4 `do_init_volume` 中的便携模式逻辑（第250-347行）
- `wait_for_drive_letter` → `wait_for_mount_point`
- `format_volume_fat32(drive_letter, ...)` → `format_volume_fat32(device_path, mount_point, ...)`
- `copy_exe_to(...)` → `copy_file_or_dir(...)`
- 三平台文件复制逻辑需要重写

### 6.5 便携模式 UI 提示（第678-687行）
- 当前只提到 Windows
- 需要更新为三平台通用说明

### 6.6 macOS 路径输入框（第541-552行）
- 当前 UI 有手动输入 macOS .app 路径的输入框
- **改为自动发现**：程序启动时扫描同目录，自动填充
- UI 上改为显示发现状态而非输入框

---

## 7. src/main.cpp

### 7.1 `EXE_DIR()` 宏（第8-19行）
- Windows：`GetModuleFileNameW`
- macOS：`_NSGetExecutablePath`
- Linux：当前返回空字符串，需要修复
  - 使用 `/proc/self/exe` 读取符号链接

### 7.2 `FreeConsole()`（第24-27行）
- 只在 Windows 下执行
- macOS/Linux 不需要

---

## 8. CMakeLists.txt

### 8.1 源文件中的平台资源
- `platform/windows/manifest.rc` 只在 Windows 时添加
- macOS 的 `platform/macos/Info.plist` 已配置
- 需要确保非 Windows 平台不编译 `manifest.rc`

### 8.2 Linux 依赖
- 可能需要 `libblkid` 用于分区信息读取
- 或只使用 `parted`/`lsblk` 命令行工具

### 8.3 macOS 依赖
- 已有 `IOKit`、`Cocoa`、`OpenGL` 等框架链接

---

## 9. 新增文件

### 9.1 `src/platform/platform_utils.h`（可选）
跨平台辅助函数：
- `get_timestamp_ms()` —— 替代 `GetTickCount64()`
- `sleep_ms(int)` —— 替代 `Sleep()`
- `run_command(const std::string&)` —— 执行系统命令并获取输出
- `is_root()` —— 检测管理员/root 权限

---

## 10. 测试清单

### 10.1 macOS 测试
- [ ] 枚举磁盘和分区
- [ ] 创建便携双分区布局
- [ ] 格式化 FAT32
- [ ] 复制三平台文件到明文分区
- [ ] 文件选择对话框
- [ ] 加密/解密功能
- [ ] VFS 文件管理

### 10.2 Linux 测试
- [ ] 枚举磁盘和分区
- [ ] 创建便携双分区布局
- [ ] 格式化 FAT32
- [ ] 复制三平台文件到明文分区
- [ ] 文件选择对话框（zenity/kdialog）
- [ ] 加密/解密功能
- [ ] VFS 文件管理
