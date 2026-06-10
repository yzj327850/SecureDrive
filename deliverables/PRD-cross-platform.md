# SecureDrive 跨平台移植 — 增量 PRD

## 1. 产品目标

基于 Windows 最终版复刻 macOS 和 Linux 版本，实现"一次制作，三平台通用"的便携加密硬盘体验。

## 2. 变更摘要

### 2.1 新增功能

| 功能 | 描述 | 平台 |
|------|------|------|
| macOS 磁盘适配层 | diskutil 命令封装：分区创建、格式化、卸载、挂载检测 | macOS |
| Linux 磁盘适配层 | parted/mkfs.vfat/umount 命令封装：分区创建、格式化、卸载、挂载检测 | Linux |
| macOS 文件对话框 | osascript AppleScript 调用：单选/多选文件、选择文件夹 | macOS |
| Linux 文件对话框 | zenity/kdialog 命令调用：单选/多选文件、选择文件夹 | Linux |
| 三平台自动发现 | 启动时扫描同目录下的其他平台可执行文件 | 全平台 |
| 三平台自动打包 | 便携模式制作时自动复制所有发现的三平台文件到明文分区 | 全平台 |

### 2.2 修改功能

| 功能 | 变更内容 | 影响文件 |
|------|---------|---------|
| 分区创建 | 将 Windows IOCTL 实现改为命令行工具封装（diskutil/parted） | disk.cpp/disk.h |
| 格式化 | 将 format.exe 调用改为 diskutil/mkfs.vfat 调用 | disk.cpp |
| 等待挂载 | 将等待盘符改为等待挂载点（/Volumes/ /media/） | disk.cpp |
| 权限检测 | 补充 macOS/Linux root 检测 | app.cpp |
| 导出目录 | 补充 macOS `open` 和 Linux `xdg-open` 支持 | app.cpp |
| 时间/睡眠函数 | 将 GetTickCount64/Sleep 改为跨平台封装 | init_wizard.cpp |
| 异常处理 | 将 Windows SEH 改为条件编译或标准异常 | init_wizard.cpp |
| 头文件包含 | 将无条件 `#include <windows.h>` 改为条件编译 | init_wizard.cpp |
| 便携模式 UI | 将手动输入 macOS 路径改为自动发现 + 状态显示 | init_wizard.cpp/app.h |
| 便携模式说明 | 更新为三平台通用使用说明 | init_wizard.cpp |

### 2.3 删除功能

| 功能 | 原因 |
|------|------|
| 免格式化加密（inplace）相关 UI 和逻辑 | 已在前期删除，本次清理残留代码 |
| macOS 路径手动输入框 | 改为自动发现机制 |

## 3. 用户故事

### 故事1：跨平台制作加密 U 盘
> 作为 macOS 用户，我将一个空 U 盘插入 Mac，运行 SecureDrive，选择整盘初始化并启用便携模式。程序自动将 Windows exe、macOS app、Linux binary 复制到 U 盘的明文分区，剩余空间加密。我将这个 U 盘插入 Windows 电脑，可以直接运行明文分区上的 SecureDrive.exe 解锁使用。

### 故事2：跨平台解锁
> 作为 Linux 用户，我收到一个同事用 Windows 制作的 SecureDrive 加密 U 盘。我插入 Linux 电脑，运行 U 盘明文分区上的 Linux 版 SecureDrive，程序自动检测到加密分区，输入密码后解锁使用。

### 故事3：任意平台补充文件
> 作为 Windows 用户，我只下载了 Windows 版 SecureDrive.exe。制作便携 U 盘时，程序检测到同目录没有 macOS 和 Linux 版本，给出提示"缺少 macOS/Linux 版本，仅复制 Windows 版本"，但不阻止操作。我后续可以手动将其他版本拷贝到 U 盘明文分区。

## 4. 需求池

### P0（必须实现）
- [ ] macOS 磁盘枚举和分区枚举完善
- [ ] macOS 分区创建（diskutil partitionDisk MBR）
- [ ] macOS 格式化 FAT32（diskutil eraseVolume）
- [ ] macOS 卸载卷（diskutil unmountDisk）
- [ ] macOS 文件选择对话框（osascript）
- [ ] Linux 磁盘枚举和分区枚举完善
- [ ] Linux 分区创建（parted MBR）
- [ ] Linux 格式化 FAT32（mkfs.vfat）
- [ ] Linux 卸载卷（umount）
- [ ] Linux 文件选择对话框（zenity/kdialog）
- [ ] 三平台可执行文件自动发现
- [ ] 三平台可执行文件自动复制到明文分区
- [ ] 跨平台时间/睡眠函数封装
- [ ] 条件编译修复（移除无条件 windows.h 包含）

### P1（重要）
- [ ] 分区偏移信息精确获取（macOS/Linux 当前为 0）
- [ ] Linux `EXE_DIR()` 修复（使用 /proc/self/exe）
- [ ] macOS/Linux 窗口图标支持
- [ ] 构建系统完善（CMakeLists.txt 平台资源条件编译）

### P2（可选）
- [ ] macOS/Linux 文件拖拽导入优化
- [ ] Linux 桌面环境检测（GNOME/KDE）以选择 zenity/kdialog
- [ ] 多语言文件对话框提示

## 5. UI 变更

### 5.1 设备选择页
- 无变更（已有跨平台条件编译）

### 5.2 初始化向导 — 警告页
- **移除**：macOS 可执行文件手动输入框
- **新增**：三平台文件发现状态显示
  ```
  [已发现] Windows: SecureDrive.exe
  [未找到] macOS: SecureDrive.app
  [已发现] Linux: SecureDrive
  ```
- **修改**：便携模式说明文字更新为三平台通用
  ```
  启用后，磁盘将被分为两个区：
    • 分区1（100MB，FAT32，明文）：存放 SecureDrive 程序
      → Windows: 双击 SecureDrive.exe 运行
      → macOS: 双击 SecureDrive.app 运行
      → Linux: 双击 SecureDrive 运行
    • 分区2（剩余空间，加密）：存放您的加密文件
  ```

### 5.3 初始化向导 — 完成页
- **修改**：成功提示更新为三平台通用

### 5.4 文件管理器
- **新增 macOS/Linux**：导入/导出按钮调用跨平台文件对话框

## 6. 待确认问题

### 6.1 分区表类型
- **问题**：macOS 默认使用 GPT，但 Windows 对 GPT 的可移动磁盘支持良好。
- **建议**：保持 MBR 以确保最大兼容性（老旧系统也支持）。
- **决策**：使用 MBR。

### 6.2 Linux 分区工具选择
- **问题**：parted vs fdisk vs sgdisk？
- **建议**：parted 支持脚本化操作（`-s` 非交互），且支持 MBR。
- **决策**：使用 parted。

### 6.3 Linux 挂载点检测
- **问题**：不同发行版挂载点不同（/media/$USER/、/run/media/$USER/、/mnt/）
- **建议**：轮询多个常见路径，优先检测 /run/media/$USER/（现代标准）。
- **决策**：轮询 `/media/*/`、`/run/media/*/`、`/mnt/*/`、`/Volumes/`(macOS)。

### 6.4 macOS .app Bundle 复制
- **问题**：.app 是目录，需要递归复制。Windows xcopy / Linux cp -R 处理不同。
- **建议**：使用 C++17 `std::filesystem::copy(src, dst, std::filesystem::copy_options::recursive)`。
- **决策**：使用 std::filesystem 递归复制。

### 6.5 三平台可执行文件命名约定
- **问题**：如何自动发现其他平台文件？
- **建议**：
  - Windows: `SecureDrive.exe`
  - macOS: `SecureDrive.app`（目录）
  - Linux: `SecureDrive`
- **决策**：按上述约定文件名扫描。

### 6.6 异常处理方案
- **问题**：init_wizard.cpp 使用了 Windows SEH（`__try`/`__except`），macOS/Linux 不支持。
- **建议**：条件编译保留 Windows SEH，其他平台使用标准 `try`/`catch(...)`。
- **决策**：条件编译处理。
