# SecureDrive Windows/Mac 双平台可执行文件可行性分析

> 分析日期: 2026-06-02
> 目标: 在非加密分区放置 Windows 和 macOS 两套可执行文件，实现双平台加密磁盘访问

---

## 一、总体结论

| 维度 | 评估 |
|------|------|
| **加密卷格式兼容性** | ✅ 完全兼容 — 三平台可互相读写 |
| **磁盘底层访问** | ✅ 已有 macOS 实现 — RawDisk / enum_disks / enum_partitions |
| **CMake 构建配置** | ⚠️ 部分就绪 — APPLE 分支存在但有致命缺陷 |
| **UI 层跨平台** | ❌ 严重不足 — 文件对话框、便携模式、初始化向导均仅 Windows |
| **macOS 可编译** | ❌ 当前不可编译 — 3 个致命阻塞点 |
| **预估工作量** | 约 3~5 天（假设有 macOS 开发环境） |

**核心判断：技术上完全可行，但当前代码距离 macOS 可编译状态还有明确的阻塞项需要逐一修复。**

---

## 二、已就绪的跨平台基础设施

### 2.1 加密卷格式（VolumeHeader）— 完美跨平台

- `volume_format.h` 使用 `#pragma pack(push, 1)` 确保结构体紧凑布局
- 所有字段使用固定宽度类型（`uint8_t`, `uint32_t`, `uint64_t`）
- 无任何平台依赖，`static_assert` 编译期验证大小
- 密钥派生流程（Argon2id → AES-CBC → HMAC-SHA256）纯软件实现
- AES-256-XTS 逐扇区加解密无平台 API 依赖

**结论：Windows 上创建的加密卷可以在 macOS 上解锁，反之亦然。**

### 2.2 磁盘底层（disk.cpp）— 已有完整 macOS 分支

| 功能 | Windows | macOS | Linux |
|------|---------|-------|-------|
| RawDisk::open | CreateFileA | open() + ioctl | open() + ioctl |
| RawDisk::read/write | SetFilePointerEx + Read/WriteFile | lseek64 + read/write | 同 macOS |
| enum_disks | SetupAPI PhysicalDrive0..31 | /dev/disk0..9 | /proc/partitions |
| enum_partitions | IOCTL_DISK_GET_DRIVE_LAYOUT_EX | /dev/diskNs1..8 | /proc/partitions 前缀匹配 |
| 可移动检测 | STORAGE_HOTPLUG_INFO | 未实现（固定false） | /sys/block/removable |

### 2.3 CMake APPLE 构建分支

```cmake
# 第 105-122 行已配置：
- find_library: OpenGL, Cocoa, IOKit, CoreVideo
- -Wno-deprecated-declarations
- MACOSX_BUNDLE TRUE
- MACOSX_BUNDLE_INFO_PLIST
```

### 2.4 其他已就绪项

| 项目 | 位置 | 说明 |
|------|------|------|
| macOS 可执行路径获取 | main.cpp:11-15 | `_NSGetExecutablePath` |
| OpenGL 前向兼容 | app.cpp:151-153 | `GLFW_OPENGL_FORWARD_COMPAT` |
| 中文字体 | app.cpp:220-223 | PingFang.ttc / STHeiti Light |
| 管理员检测 | app.cpp:102-103 | `geteuid() == 0` |
| 文件打开 | app.cpp:1606-1610 | `open` 命令 |
| macOS 构建脚本 | build_mac.sh | 一键构建 |
| macOS Bundle 配置 | platform/macos/Info.plist | CFBundleIdentifier 等 |

---

## 三、致命阻塞点（macOS 编译失败）

### 阻塞点 1：CMakeLists.txt 无条件包含 .rc 文件

**位置**: CMakeLists.txt 第 71-73 行

```cmake
add_executable(SecureDrive ${SOURCES} ${IMGUI_SOURCES}
    platform/windows/manifest.rc    # ← 始终包含！macOS 无法编译
)
```

**修复方案**:
```cmake
if(WIN32)
    set(PLATFORM_RESOURCES platform/windows/manifest.rc)
endif()
add_executable(SecureDrive ${SOURCES} ${IMGUI_SOURCES} ${PLATFORM_RESOURCES})
```

### 阻塞点 2：CMakeLists.txt 无条件包含 Windows 头文件目录

**位置**: CMakeLists.txt 第 78-83 行

```cmake
target_include_directories(SecureDrive PRIVATE
    ...
    platform/windows    # ← macOS 无需此目录
)
```

**修复方案**: 用 `if(WIN32)` 包裹。

### 阻塞点 3：init_wizard.cpp 无条件 #include <windows.h>

**位置**: init_wizard.cpp 第 13 行

```cpp
#include <windows.h>    // ← macOS 无此头文件，直接编译失败
```

**影响的代码范围**: init_wizard.cpp 全文（545 行）深度依赖 Windows API：

| 使用的 Windows API | 出现行 | macOS 替代 |
|---|---|---|
| `GetTickCount64()` | 134, 222, 243, 264 | `clock_gettime(CLOCK_MONOTONIC)` |
| `Sleep(ms)` | 157 | `usleep(ms * 1000)` |
| `GetFileAttributesA()` | 181 | `stat()` |
| `CreateProcessA()` | 190-191 | `fork()` + `exec()` |
| `xcopy` 命令 | 184 | `cp -R` |
| `SEH __try/__except` | 281-293 | macOS 无 SEH，需用 C++ 异常或返回值检查 |
| `STARTUPINFOA/PROCESS_INFORMATION` | 188-194 | POSIX 进程 API |
| `GetExceptionCode()` | 284 | 无替代，需删除 SEH |
| `CloseHandle()` | 193-194 | `close()` / `waitpid()` |

---

## 四、缺失的 macOS 功能（需新增实现）

### 4.1 文件/文件夹选择对话框（高优先级）

**当前状态**: 仅 Windows 实现

| 功能 | Windows 实现 | 位置 |
|------|---|---|
| 多选文件 | `GetOpenFileNameW(OFN_ALLOWMULTISELECT)` | app.cpp:1056-1099 |
| 选择文件夹 | `SHBrowseForFolderW()` | app.cpp:891-916 |

**macOS 替代方案**:
- **方案 A**: 使用 [NFD (Native File Dialog)](https://github.com/mlabbe/nativefiledialog) — 跨平台，C 语言，无需额外依赖
- **方案 B**: 使用 `GLFW` 内置的文件对话框回调（GLFW 3.4+ 支持，但功能有限）
- **方案 C**: macOS 原生 `NSOpenPanel` / `NSSavePanel`（需要 ObjC 混编）
- **方案 D**: 命令行 `osascript` 调用（最简单但体验差）

**推荐方案 A（NFD）**：C 语言库，单文件集成，支持 macOS/Windows/Linux 三平台原生对话框。

### 4.2 便携模式双分区布局（高优先级）

**当前状态**: 完全仅 Windows 实现

| 功能 | Windows API | 位置 |
|------|---|---|
| MBR 分区表写入 | `IOCTL_DISK_SET_DRIVE_LAYOUT_EX` | disk.cpp:187-300 |
| 等待盘符分配 | `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS` | disk.cpp:302-333 |
| FAT32 格式化 | `format.exe` 子进程 | disk.cpp:335-364 |
| 卸载卷 | `FSCTL_DISMOUNT_VOLUME` | disk.cpp:145-182 |
| 复制文件 | `CopyFileA` | disk.cpp:366-368 |

**macOS 替代方案**:

| Windows 功能 | macOS 等价操作 |
|---|---|
| MBR 分区表操作 | `fdisk` / `diskutil` 命令行工具 |
| FAT32 格式化 | `diskutil eraseDisk FAT32 SDRV_BOOT /dev/diskXs1` |
| 卷卸载 | `diskutil unmount /dev/diskXsY` |
| 文件复制 | `cp -R` / `std::filesystem::copy` |
| 等待挂载 | `diskutil mount` + 轮询 `/Volumes/` |

**注意**: macOS 便携模式还需要处理 macOS 不自动给 MBR 分区分配盘符的特性。macOS 的 `/Volumes/` 挂载点需要轮询检测。

### 4.3 文件系统检测（中优先级）

**当前**: `detect_fs_label()` 仅 Windows 实现（app.cpp:364-385）

**macOS 替代**: 读取分区第一扇区，同样检测 "NTFS", "FAT32", "EXFAT", "SDRV01" 魔数。逻辑与平台无关，只需确保 `RawDisk::read()` 能正常工作（已实现）。

### 4.4 免格式化加密 UI（中优先级）

**当前**: "卸载文件系统" 和 "免格式化加密" 按钮仅在 `#ifdef _WIN32` 内（app.cpp:511-570）

**修复**: 需要在 macOS 分支提供等价的卷卸载功能（`diskutil unmount`）。

### 4.5 macOS 特有配置修正（低优先级）

| 问题 | 位置 | 修复 |
|------|---|---|
| Info.plist 权限错误 | platform/macos/Info.plist:17 | `device.audio-input` → 删除或改为正确权限声明 |
| 缺少 .app 图标 | 无 | 需要制作 .icns 文件 |
| 代码签名 | 未配置 | 需 `entitlements.plist` 声明磁盘访问权限 |
| 沙盒禁用 | 未配置 | macOS 裸设备访问不能在沙盒内运行 |

---

## 五、架构层面的跨平台改造建议

### 5.1 条件编译策略

```
┌─────────────────────────────────────────────────┐
│  平台抽象层（需要补全）                           │
│  ├── os_sleep(ms)          → Sleep/usleep/nanosleep │
│  ├── os_copy_recursive()    → xcopy/cp -R          │
│  ├── os_file_dialog()      → NFD (跨平台)          │
│  ├── os_folder_dialog()    → NFD (跨平台)          │
│  ├── os_detect_fs()        → 读取扇区魔数 (已可复用) │
│  ├── os_dismount()         → FSCTL/diskutil unmount │
│  └── os_elapsed_ms()       → GetTickCount64/clock_gettime │
├─────────────────────────────────────────────────┤
│  便携模式布局（需要 macOS 实现）                  │
│  ├── create_portable_layout() → IOCTL / fdisk      │
│  ├── format_fat32()          → format.exe / diskutil│
│  ├── wait_for_mount()       → 盘符轮询 / /Volumes/  │
│  └── copy_portable_files()  → CopyFile / cp -R     │
├─────────────────────────────────────────────────┤
│  核心层（已跨平台）                               │
│  ├── Volume / VolumeHeader / AES-XTS / Argon2id  │
│  ├── VFS / Inode / Bitmap / 目录操作              │
│  └── RawDisk / enum_disks / enum_partitions      │
├─────────────────────────────────────────────────┤
│  UI 层 (Dear ImGui + GLFW, 已跨平台)             │
│  ├── 渲染 / 布局 / 交互逻辑                      │
│  └── 文件操作（导入/导出/打开）需适配              │
└─────────────────────────────────────────────────┘
```

### 5.2 init_wizard.cpp 重构策略

当前 `init_wizard.cpp` 将 SEH 包装、便携模式、免格式化加密全部耦合在一起。建议重构为：

1. **分离 SEH**: macOS 不支持 `__try/__except`，用函数返回值 + C++ try/catch 替代
2. **平台相关代码移入 disk.cpp**: 便携布局（分区/格式化/复制）抽象为跨平台接口
3. **时间测量函数**: `os_elapsed_ms()` 替代 `GetTickCount64()`

---

## 六、实施路线图

### 阶段 1：修复编译阻塞（约 1 天）

- [ ] CMakeLists.txt: `manifest.rc` 和 `platform/windows` 用 `if(WIN32)` 包裹
- [ ] init_wizard.cpp: `#include <windows.h>` 用 `#ifdef _WIN32` 包裹
- [ ] init_wizard.cpp: 所有 Windows API 调用（SEH, Sleep, GetTickCount64, CreateProcessA, xcopy 等）用条件编译包裹，macOS 分支提供等价实现
- [ ] disk.h / disk.cpp: 便携模式函数声明和实现添加 `#ifdef _WIN32` 保护
- [ ] app.cpp: 设备选择页的"卸载/免格式化"按钮添加 macOS 分支

### 阶段 2：补充 macOS 基础设施（约 1~2 天）

- [ ] 集成 NFD 文件对话框库（文件多选 + 文件夹选择）
- [ ] 实现 macOS 便携模式布局（diskutil / fdisk 命令行封装）
- [ ] 实现 macOS 卷卸载功能
- [ ] 修复 Info.plist 权限配置
- [ ] 创建 macOS entitlements.plist（磁盘访问权限）

### 阶段 3：测试与优化（约 1~2 天）

- [ ] macOS 环境下编译测试（需要 Mac 机器或 CI）
- [ ] 交叉加密卷读写测试（Windows 创建 → macOS 解锁，反之）
- [ ] 便携模式双分区测试（Windows 创建 → macOS 读取，反之）
- [ ] macOS .app 图标和代码签名

---

## 七、关键风险

| 风险 | 概率 | 影响 | 缓解措施 |
|------|------|------|----------|
| macOS 裸设备需要 SIP 特殊配置 | 高 | macOS 10.13+ 需禁用 SIP 或使用 `diskutil` 授权 | 提示用户 sudo 运行，文档说明 |
| macOS .app bundle 在 FAT32 上的兼容性 | 中 | .app 实际是目录，FAT32 不支持符号链接 | 直接复制目录，需递归保留结构 |
| macOS 的 Gatekeeper 阻止未签名应用 | 高 | 用户首次打开需要手动确认 | 文档说明如何绕过 Gatekeeper |
| MBR 分区表在 macOS 上的自动挂载行为 | 中 | macOS 可能自动挂载加密分区导致读写冲突 | 初始化前显式 unmount |
| macOS 没有开发环境 | — | 无法验证 macOS 编译 | 先在 Windows 上完成条件编译重构，后续在 Mac 上验证 |

---

## 八、便携分区文件布局设计

### Windows 侧
```
分区1 (FAT32, 100MB, 明文):
├── SecureDrive.exe          ← Windows 可执行文件
├── SecureDrive.app/         ← macOS .app bundle（目录结构）
│   └── Contents/
│       ├── Info.plist
│       └── MacOS/
│           └── SecureDrive  ← macOS 可执行文件
└── (可选) README.txt         ← 使用说明
```

### macOS 侧自动检测流程

1. 用户插入移动磁盘
2. macOS 自动挂载分区1 为 `/Volumes/SDRV_BOOT`
3. 用户双击 `SecureDrive.app`
4. 应用启动后 `enum_disks()` 扫描所有磁盘
5. `auto_detect_portable_volume()` 检测分区2 的 `SDRV01` 魔数
6. 跳转到解锁界面

---

## 九、结论

**可行性评估：技术上完全可行，当前代码基础良好。**

核心优势：
- 加密卷格式从设计之初就是跨平台的（`#pragma pack` + 固定宽度类型 + 纯软件加密）
- 磁盘底层已有 macOS 分支
- UI 框架（Dear ImGui + GLFW）天然跨平台

主要工作量在：
- 清理 Windows 专属代码的条件编译（约 1 天）
- 补充 macOS 文件对话框和便携模式支持（约 1~2 天）
- 测试验证（约 1 天）

**建议优先修复 CMakeLists.txt 和 init_wizard.cpp 的条件编译问题，使项目至少能在 macOS 上编译通过，再逐步补充功能。**
