# SecureDrive 跨平台移植技术调研

## 1. macOS 平台实现方案

### 1.1 磁盘设备枚举（已有基础实现）
- 设备路径格式：`/dev/disk0`, `/dev/disk1`, ...
- 使用 `ioctl(fd, DKIOCGETBLOCKSIZE, &blksz)` 和 `ioctl(fd, DKIOCGETBLOCKCOUNT, &blkcnt)`
- 头文件：`#include <sys/disk.h>`

### 1.2 分区枚举（已有基础实现）
- 分区路径格式：`/dev/disk0s1`, `/dev/disk0s2`, ...（s = slice）
- 当前实现已支持，但缺少 `offset_bytes` 信息
- **改进**：用 `diskutil list -plist /dev/diskN` 获取精确的分区偏移和大小

### 1.3 分区创建（`create_portable_layout`）
- **方案**：使用 `diskutil partitionDisk` 命令
- 命令示例：
  ```bash
  diskutil partitionDisk /dev/diskN MBR \
    "MS-DOS FAT32" SDRV_BOOT 100Mi \
    "Free Space" SDRV_CRYPTO R
  ```
- 注意：macOS 需要 `diskutil` 的 `"Free Space"` 类型作为占位，第二个分区实际无文件系统
- **替代方案**：使用 `gpt` 命令创建 GUID 分区表（更现代，但 Windows 兼容性需要考虑）
- **决策**：保持 MBR 分区表以确保 Windows/macOS/Linux 三平台兼容读取

### 1.4 格式化 FAT32（`format_volume_fat32`）
- **方案**：`diskutil eraseVolume FAT32 SDRV_BOOT /dev/diskNs1`
- 等待分区出现在 `/Volumes/SDRV_BOOT`

### 1.5 卸载卷（`dismount_volumes_on_disk`）
- **方案**：`diskutil unmountDisk /dev/diskN`
- 或逐个卸载：`diskutil unmount /dev/diskNsM`

### 1.6 等待挂载点（`wait_for_drive_letter` 的替代）
- **方案**：轮询 `/Volumes/` 目录，查找对应分区标签的挂载点
- 分区标签可通过 `diskutil info /dev/diskNsM` 获取

### 1.7 复制可执行文件（`copy_exe_to`）
- 使用 `std::filesystem::copy`（C++17）或 `cp -R` 命令
- macOS `.app` 是目录，需要递归复制

### 1.8 文件选择对话框
- **方案**：`osascript` 调用 AppleScript
  ```bash
  osascript -e 'POSIX path of (choose file with prompt "选择文件")'
  ```
- 多选：
  ```bash
  osascript -e 'POSIX path of (choose file with prompt "选择文件" multiple selections allowed true)'
  ```
- 文件夹选择：
  ```bash
  osascript -e 'POSIX path of (choose folder with prompt "选择文件夹")'
  ```

### 1.9 权限检测
- `geteuid() == 0` 检测 root
- 或检测 `diskutil` 命令是否能成功执行

### 1.10 窗口图标
- macOS .app bundle 的图标在 `Info.plist` 和 `.icns` 文件中定义
- CMakeLists.txt 已配置 `MACOSX_BUNDLE` 和 `Info.plist`
- GLFW 窗口图标可从资源文件加载或跳过（macOS 通常用 .app bundle 图标）

---

## 2. Linux 平台实现方案

### 2.1 磁盘设备枚举（已有基础实现）
- 读取 `/proc/partitions` 获取块设备列表
- 当前实现已支持，但 `offset_bytes` 为 0（需要改进）
- **改进**：用 `lsblk -J -o NAME,SIZE,TYPE,RM,PHY-SEC` 获取 JSON 格式设备信息

### 2.2 分区枚举（已有基础实现）
- 当前实现筛选 `/proc/partitions` 中名称前缀匹配的分区
- 缺少 `offset_bytes`，可用 `parted /dev/sdX unit B print` 获取
- **改进**：用 `lsblk -J -b -o NAME,SIZE,TYPE,RM,PHY-SEC,START` 获取带偏移的分区信息

### 2.3 分区创建（`create_portable_layout`）
- **方案A**：`parted` 命令
  ```bash
  parted -s /dev/sdX mklabel msdos
  parted -s /dev/sdX mkpart primary fat32 1MiB 101MiB
  parted -s /dev/sdX mkpart primary 101MiB 100%
  parted -s /dev/sdX set 1 boot off
  ```
- **方案B**：`sgdisk`（GPT，更现代）
  - 但 MBR 兼容性更好，建议使用 `parted` + MBR
- 需要 root 权限

### 2.4 格式化 FAT32（`format_volume_fat32`）
- **方案**：`mkfs.vfat -F 32 -n SDRV_BOOT /dev/sdX1`
- 需要 `dosfstools` 包
- 等待分区出现在 `/media/$USER/SDRV_BOOT` 或 `/run/media/$USER/SDRV_BOOT`

### 2.5 卸载卷（`dismount_volumes_on_disk`）
- **方案**：`umount /dev/sdX1` 或 `umount /media/...`
- 或 `udisksctl unmount -b /dev/sdX1`

### 2.6 等待挂载点
- **方案**：轮询 `/media/$USER/`, `/run/media/$USER/`, `/mnt/`
- 现代桌面环境（GNOME/KDE）通常挂载到 `/run/media/$USER/label/`

### 2.7 复制可执行文件
- 使用 `std::filesystem::copy` 或 `cp` 命令

### 2.8 文件选择对话框
- **方案**：`zenity`（GTK）
  ```bash
  zenity --file-selection --title="选择文件"
  zenity --file-selection --multiple --title="选择多个文件"
  zenity --file-selection --directory --title="选择文件夹"
  ```
- **备选**：`kdialog`（KDE）
  ```bash
  kdialog --getopenfilename "" "*"
  kdialog --getopenfilename "" "*" --multiple
  kdialog --getexistingdirectory
  ```
- 优先检测 `zenity`，不存在时检测 `kdialog`

### 2.9 权限检测
- `geteuid() == 0` 检测 root

### 2.10 窗口图标
- Linux 下 GLFW 窗口图标可用 `glfwSetWindowIcon`
- 从文件加载 PNG 图标，或跳过

---

## 3. 三平台可执行文件打包方案

### 3.1 约定文件名
- Windows: `SecureDrive.exe`
- macOS: `SecureDrive.app`（目录 bundle）
- Linux: `SecureDrive`

### 3.2 自动发现机制
- 程序启动时，扫描可执行文件所在目录
- 查找同目录下的其他平台可执行文件
- 如果存在，记录路径供便携模式使用

### 3.3 复制逻辑
- 在 `do_init_volume()` 中，便携模式格式化明文分区后：
  1. 复制当前平台的可执行文件（必须有）
  2. 如果找到 Windows exe，复制到明文分区
  3. 如果找到 macOS .app，递归复制到明文分区
  4. 如果找到 Linux binary，复制到明文分区
  5. 如果某些平台文件缺失，给出提示但不阻止操作

### 3.4 UI 更新
- 便携模式说明文字需要更新为三平台通用
- 移除 macOS 路径手动输入框（改为自动发现）

---

## 4. 构建系统调整

### 4.1 CMakeLists.txt
- macOS/Linux 部分已有基本配置
- 需要确保 `platform/windows/manifest.rc` 只在 Windows 时包含
- macOS 需要 `platform/macos/Info.plist`

### 4.2 条件编译
- `init_wizard.cpp` 直接 `#include <windows.h>` 需要改为条件编译
- `GetTickCount64()` 和 `Sleep()` 需要跨平台封装
- Windows SEH (`__try`/`__except`) 需要改为标准 C++ 异常或平台条件编译

---

## 5. 关键风险与决策

| 决策点 | 选项 | 推荐 |
|--------|------|------|
| macOS 分区工具 | diskutil vs gpt | diskutil（更简单可靠） |
| Linux 分区工具 | parted vs sgdisk | parted（MBR 兼容） |
| Linux 文件对话框 | zenity vs kdialog | 优先 zenity，fallback kdialog |
| 分区表类型 | MBR vs GPT | MBR（三平台兼容性最好） |
| init_wizard SEH | 保留 Windows SEH / 标准异常 | 条件编译保留 Windows SEH，其他平台用标准异常 |
| 等待挂载方式 | 轮询 vs 通知 | 轮询（简单可靠） |
