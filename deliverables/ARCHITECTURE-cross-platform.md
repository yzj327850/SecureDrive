# SecureDrive 跨平台移植 — 架构设计文档

## 1. 实现方案概述

### 总体策略：命令行工具封装 + 条件编译

由于 macOS 和 Linux 没有 Windows 那样的磁盘 IOCTL API，我们采用**调用系统命令行工具**的方式实现磁盘管理功能：

- **macOS**：`diskutil`（Apple 官方磁盘管理工具）
- **Linux**：`parted` + `mkfs.vfat` + `umount`（标准 GNU 工具）

核心加密算法（AES-XTS）、VFS、UI（Dear ImGui）已经是跨平台的，不需要修改。

### 架构分层

```
┌─────────────────────────────────────┐
│  UI Layer (app.cpp / init_wizard.cpp) │  ← 添加文件对话框跨平台封装
├─────────────────────────────────────┤
│  Volume Layer (volume.cpp)          │  ← 简化 create_inplace，删除 Windows 复杂逻辑
├─────────────────────────────────────┤
│  Disk Layer (disk.cpp / disk.h)     │  ← 核心修改：添加 macOS/Linux 分区/格式化/卸载
├─────────────────────────────────────┤
│  Crypto/VFS/Security                │  ← 无需修改（已跨平台）
└─────────────────────────────────────┘
```

---

## 2. 文件列表及相对路径

### 2.1 新增文件

| 文件路径 | 说明 |
|---------|------|
| `src/platform/platform_utils.h` | 跨平台辅助函数：时间、睡眠、命令执行、root 检测 |
| `src/platform/macos/Info.plist` | 已存在，可能需要更新 |

### 2.2 修改文件

| 文件路径 | 修改范围 | 说明 |
|---------|---------|------|
| `src/disk/disk.h` | 新增函数声明、结构调整 | 跨平台挂载相关接口 |
| `src/disk/disk.cpp` | 大幅修改 | macOS/Linux 分区/格式化/卸载实现 |
| `src/volume/volume.cpp` | 简化 | 删除 create_inplace 中 Windows 特有复杂逻辑 |
| `src/ui/app.h` | 新增字段 | 三平台可执行文件路径 |
| `src/ui/app.cpp` | 中等修改 | 文件对话框、权限检测、导出打开 |
| `src/ui/init_wizard.cpp` | 大幅修改 | 平台头文件、时间函数、SEH、便携模式逻辑 |
| `src/main.cpp` | 小幅修改 | Linux EXE_DIR 修复 |
| `CMakeLists.txt` | 小幅修改 | 平台资源条件编译 |

---

## 3. 数据结构和接口设计

### 3.1 disk.h — 新增/修改

```cpp
// ---- 挂载信息 ----
struct MountInfo {
    std::string device_path;   // 设备路径，如 /dev/disk1s1
    std::string mount_point;   // 挂载点，如 /Volumes/SDRV_BOOT
    std::string fs_type;       // 文件系统类型
};

// ---- 卷管理（跨平台通用化） ----
// 原有 Windows 特有函数改为全平台可用，内部实现用条件编译

// 格式化指定分区为 FAT32
// macOS/Linux: 传入设备路径和标签
// Windows: 传入盘符（向后兼容）
bool format_volume_fat32(const std::string& device_or_letter,
                         const std::string& label = "SDRV_BOOT");

// 等待分区挂载，返回挂载点路径
// macOS: 轮询 /Volumes/
// Linux: 轮询 /media/*/, /run/media/*/
// Windows: 轮询盘符（向后兼容）
std::string wait_for_mount_point(const std::string& device_path,
                                 int timeout_ms = 8000);

// 获取设备当前的挂载点
std::string get_mount_point(const std::string& device_path);

// 复制文件或目录（跨平台递归）
bool copy_file_or_dir(const std::string& src, const std::string& dst);

// 卸载指定磁盘上的所有卷（跨平台）
// macOS: diskutil unmountDisk
// Linux: 读取 /proc/mounts 逐个 umount
// Windows: 原有 IOCTL 实现
bool dismount_volumes_on_disk(const std::string& disk_path);

// 执行系统命令并返回输出（辅助函数）
std::string run_command(const std::string& cmd);
```

### 3.2 app.h — 新增字段

```cpp
// 三平台可执行文件源路径（自动发现，程序启动时填充）
std::string portable_win_src;    // Windows: SecureDrive.exe
std::string portable_mac_src;    // macOS: SecureDrive.app
std::string portable_linux_src;  // Linux: SecureDrive

// 删除旧字段（在实现时处理）
// portable_exe_src → 重命名为 portable_win_src
```

### 3.3 platform_utils.h — 新增文件

```cpp
#pragma once
#include <string>
#include <cstdint>

// 获取当前时间戳（毫秒）
uint64_t get_timestamp_ms();

// 睡眠指定毫秒
void sleep_ms(int ms);

// 执行命令并返回 stdout
std::string run_command(const std::string& cmd);

// 检测是否以管理员/root 运行
bool is_admin_or_root();

// 获取环境变量
std::string get_env(const std::string& name);

// 轮询等待条件满足（带超时）
bool poll_wait(std::function<bool()> condition, int timeout_ms, int interval_ms = 500);
```

---

## 4. 程序调用流程

### 4.1 便携模式初始化 — macOS 完整时序

```
用户选择整盘 + 启用便携模式 → 进入初始化向导
    ↓
draw_init_wizard() 设置 wizard_portable = true
    ↓
用户确认 → 后台线程启动 do_init_volume()
    ↓
【便携模式】create_portable_layout("/dev/diskN", 100)
    → 调用 diskutil partitionDisk /dev/diskN MBR ...
    → 分区1: /dev/diskNs1 (FAT32, 100MB)
    → 分区2: /dev/diskNs2 (剩余空间)
    ↓
wait_for_mount_point("/dev/diskNs1", 10000)
    → 轮询 /Volumes/ 查找 SDRV_BOOT
    → 返回 "/Volumes/SDRV_BOOT"
    ↓
format_volume_fat32("/dev/diskNs1", "SDRV_BOOT")
    → diskutil eraseVolume FAT32 SDRV_BOOT /dev/diskNs1
    ↓
复制三平台文件到明文分区
    → copy_file_or_dir(portable_win_src, "/Volumes/SDRV_BOOT/SecureDrive.exe")
    → copy_file_or_dir(portable_mac_src, "/Volumes/SDRV_BOOT/SecureDrive.app")
    → copy_file_or_dir(portable_linux_src, "/Volumes/SDRV_BOOT/SecureDrive")
    ↓
vol->create("/dev/diskN", crypto_offset, primary, emerg)
    → 在分区2上创建加密卷头
    ↓
vfs->format(vol) → vfs->mount(vol)
    ↓
完成，进入文件管理器
```

### 4.2 便携模式初始化 — Linux 完整时序

```
用户选择整盘 + 启用便携模式 → 进入初始化向导
    ↓
【便携模式】create_portable_layout("/dev/sdX", 100)
    → 调用 parted -s /dev/sdX mklabel msdos
    → 调用 parted -s /dev/sdX mkpart primary fat32 1MiB 101MiB
    → 调用 parted -s /dev/sdX mkpart primary 101MiB 100%
    → 调用 partprobe /dev/sdX (刷新分区表)
    → 分区1: /dev/sdX1, 分区2: /dev/sdX2
    ↓
wait_for_mount_point("/dev/sdX1", 10000)
    → 轮询 /media/*/, /run/media/*/, /mnt/
    → 返回 "/run/media/username/SDRV_BOOT"
    ↓
format_volume_fat32("/dev/sdX1", "SDRV_BOOT")
    → mkfs.vfat -F 32 -n SDRV_BOOT /dev/sdX1
    ↓
复制三平台文件到明文分区
    → copy_file_or_dir(...) 到挂载点
    ↓
vol->create("/dev/sdX", crypto_offset, primary, emerg)
    → 在分区2上创建加密卷头
    ↓
vfs->format(vol) → vfs->mount(vol)
    ↓
完成
```

---

## 5. 任务列表（按实现顺序排列）

### 阶段1：基础设施（前置依赖）

| 编号 | 任务名 | 目标文件 | 功能描述 | 依赖 | 平台 |
|------|--------|---------|---------|------|------|
| T1 | 创建跨平台工具头文件 | `src/platform/platform_utils.h` | 时间、睡眠、命令执行、root 检测 | 无 | 全平台 |
| T2 | Linux EXE_DIR 修复 | `src/main.cpp` | 使用 /proc/self/exe 获取程序目录 | 无 | Linux |
| T3 | 条件编译修复 init_wizard | `src/ui/init_wizard.cpp` | 将 `#include <windows.h>` 改为条件编译 | 无 | 全平台 |
| T4 | 跨平台时间/睡眠函数 | `src/ui/init_wizard.cpp` | 替换 GetTickCount64/Sleep | T1 | 全平台 |
| T5 | 跨平台异常处理 | `src/ui/init_wizard.cpp` | SEH 改为条件编译 | 无 | 全平台 |

### 阶段2：磁盘层适配（核心）

| 编号 | 任务名 | 目标文件 | 功能描述 | 依赖 | 平台 |
|------|--------|---------|---------|------|------|
| T6 | disk.h 接口扩展 | `src/disk/disk.h` | 新增跨平台函数声明 | T1 | 全平台 |
| T7 | macOS 分区创建 | `src/disk/disk.cpp` | create_portable_layout (diskutil) | T6 | macOS |
| T8 | macOS 格式化 | `src/disk/disk.cpp` | format_volume_fat32 (diskutil) | T6 | macOS |
| T9 | macOS 卸载 | `src/disk/disk.cpp` | dismount_volumes_on_disk (diskutil) | T6 | macOS |
| T10 | macOS 挂载检测 | `src/disk/disk.cpp` | wait_for_mount_point / get_mount_point | T6 | macOS |
| T11 | Linux 分区创建 | `src/disk/disk.cpp` | create_portable_layout (parted) | T6 | Linux |
| T12 | Linux 格式化 | `src/disk/disk.cpp` | format_volume_fat32 (mkfs.vfat) | T6 | Linux |
| T13 | Linux 卸载 | `src/disk/disk.cpp` | dismount_volumes_on_disk (umount) | T6 | Linux |
| T14 | Linux 挂载检测 | `src/disk/disk.cpp` | wait_for_mount_point / get_mount_point | T6 | Linux |
| T15 | 跨平台文件复制 | `src/disk/disk.cpp` | copy_file_or_dir (std::filesystem) | T6 | 全平台 |

### 阶段3：磁盘枚举完善

| 编号 | 任务名 | 目标文件 | 功能描述 | 依赖 | 平台 |
|------|--------|---------|---------|------|------|
| T16 | macOS 分区枚举完善 | `src/disk/disk.cpp` | enum_partitions 添加 offset_bytes | 无 | macOS |
| T17 | Linux 分区枚举完善 | `src/disk/disk.cpp` | enum_partitions 添加 offset_bytes | 无 | Linux |

### 阶段4：UI 层适配

| 编号 | 任务名 | 目标文件 | 功能描述 | 依赖 | 平台 |
|------|--------|---------|---------|------|------|
| T18 | app.h 三平台字段 | `src/ui/app.h` | 新增 portable_win/mac/linux_src | 无 | 全平台 |
| T19 | 三平台自动发现 | `src/ui/app.cpp` | init() 中扫描同目录可执行文件 | T18 | 全平台 |
| T20 | macOS 文件对话框 | `src/ui/app.cpp` | 导入/导出 osascript | 无 | macOS |
| T21 | Linux 文件对话框 | `src/ui/app.cpp` | 导入/导出 zenity/kdialog | 无 | Linux |
| T22 | macOS 打开外部文件 | `src/ui/app.cpp` | do_open_file 使用 `open` 命令 | 无 | macOS |
| T23 | Linux 打开外部文件 | `src/ui/app.cpp` | do_open_file 使用 `xdg-open` 命令 | 无 | Linux |

### 阶段5：初始化向导适配

| 编号 | 任务名 | 目标文件 | 功能描述 | 依赖 | 平台 |
|------|--------|---------|---------|------|------|
| T24 | 便携模式逻辑适配 | `src/ui/init_wizard.cpp` | do_init_volume 中使用跨平台函数 | T7-T15 | 全平台 |
| T25 | 三平台复制逻辑 | `src/ui/init_wizard.cpp` | 复制所有发现的平台文件 | T15, T19 | 全平台 |
| T26 | 便携模式 UI 更新 | `src/ui/init_wizard.cpp` | 移除手动输入，显示发现状态 | T19 | 全平台 |

### 阶段6：构建与清理

| 编号 | 任务名 | 目标文件 | 功能描述 | 依赖 | 平台 |
|------|--------|---------|---------|------|------|
| T27 | volume.cpp 清理 | `src/volume/volume.cpp` | 简化 create_inplace 删除 Windows 复杂逻辑 | 无 | 全平台 |
| T28 | CMakeLists.txt 调整 | `CMakeLists.txt` | manifest.rc 条件编译 | 无 | 全平台 |
| T29 | 全局一致性审查 | 所有修改文件 | 确保接口一致、无编译错误 | T1-T28 | 全平台 |

---

## 6. 共享知识（跨文件约定）

### 6.1 三平台可执行文件命名约定

程序启动时，在 `EXE_DIR()` 目录下扫描以下文件：

| 平台 | 文件名 | 类型 |
|------|--------|------|
| Windows | `SecureDrive.exe` | 文件 |
| macOS | `SecureDrive.app` | 目录 bundle |
| Linux | `SecureDrive` | 文件 |

扫描逻辑：
```cpp
// 伪代码
auto exe_dir = EXE_DIR();
if (fs::exists(exe_dir / "SecureDrive.exe")) portable_win_src = ...;
if (fs::exists(exe_dir / "SecureDrive.app")) portable_mac_src = ...;
if (fs::exists(exe_dir / "SecureDrive")) portable_linux_src = ...;
```

### 6.2 挂载点轮询路径

| 平台 | 轮询路径 | 检测方式 |
|------|---------|---------|
| macOS | `/Volumes/` | 查找与分区标签同名的目录 |
| Linux | `/media/*/`、`/run/media/*/`、`/mnt/` | 查找与分区标签同名的目录 |
| Windows | 盘符 `C:` ~ `Z:` | IOCTL 查询 |

### 6.3 命令行工具调用封装规范

所有命令行调用统一使用 `run_command()` 辅助函数：
- 返回 stdout 字符串
- 调用者负责检查返回值是否为空或包含错误信息
- 超时由外部控制（不封装在 run_command 内）

### 6.4 分区偏移对齐

- 分区1 起始：1MB 对齐（跳过 MBR + 保留空间）
- 分区1 大小：100MB（boot_mb 参数）
- 分区2 起始：1MB + 100MB = 101MB
- 分区2 大小：磁盘总大小 - 101MB - 1MB（末尾保留）
- 与 Windows 版本保持一致

---

## 7. 待明确事项

### 7.1 Linux 桌面环境检测
- **问题**：zenity（GTK）和 kdialog（KDE）哪个优先？
- **建议**：先检测 zenity（更常见），不存在时检测 kdialog。
- **决策权**：工程师实现时决定。

### 7.2 Linux parted 可用性
- **问题**：如果 parted 不存在，是否使用 fdisk 作为 fallback？
- **建议**：先检测 parted，不存在时给出错误提示要求安装。fdisk 非脚本友好，不推荐。
- **决策权**：工程师实现时决定。

### 7.3 macOS diskutil 权限
- **问题**：diskutil partitionDisk 需要管理员权限，如何提示用户？
- **建议**：在 UI 层统一提示"需要管理员/root 权限"。macOS 用户可通过 `sudo` 运行程序。
- **决策权**：工程师实现时与 UI 层配合。

### 7.4 std::filesystem 复制大目录性能
- **问题**：std::filesystem::copy 递归复制 .app bundle 可能较慢。
- **建议**：macOS 可用 `ditto` 命令（原生支持 .app 复制），Linux 可用 `cp -R`，Windows 保持现有方案。
- **决策权**：工程师实现时决定。

### 7.5 分区表类型
- **问题**：是否在所有平台统一使用 MBR？
- **决策**：使用 MBR。GPT 虽然更现代，但 MBR 在三平台上的兼容性最好，且与 Windows 版本保持一致。
