<<<<<<< HEAD
# SecureDrive — 便携式移动硬盘加密软件

![Build](https://github.com/yzj327850/SecureDrive/actions/workflows/build.yml/badge.svg)

SecureDrive 是一款完全自包含的移动硬盘加密软件，**不依赖任何外部加密工具**（如 VeraCrypt/BitLocker/LUKS），无需在目标电脑上安装任何驱动或软件。

软件直接放置在移动硬盘的公开分区中，插入任意电脑即可运行，用于解锁并管理加密分区中的文件。

## 安全特性

| 层级 | 技术方案 |
|------|----------|
| 密钥派生 | **Argon2id**（内存 64MB, 迭代 3, 并行 4）— 每次解锁耗时约 1-2 秒 |
| 数据加密 | **AES-256-XTS**（IEEE 1619）— 扇区级别，每扇区独立加密 |
| 防暴力破解 | 连续 **3 次**密码错误 → 锁定 **5 分钟** |
| 时序攻击 | HMAC 和密码验证采用 **constant-time 比较** |
| 密钥安全 | Master Key 随机生成，加密封存在卷头，内存使用后**显式归零** |
| 双密码 | 支持**主密码 + 紧急密码**，两套独立密码均可解锁 |
| 密码更改 | 无需重新加密全盘，仅重加密卷头密钥槽 |

## 架构

```
移动硬盘布局:
┌──────────────┬─────────────────────────┐
│  公开分区      │  加密分区                  │
│  (~100MB)     │  (剩余空间)               │
│              │                         │
│ ┌──────────┐ │  Sector 0: 卷头           │
│ │SecureDrive.exe  │  (Argon2id 盐值,       │
│ │(可执行文件) │   加密的 Master Key)      │
│ ├──────────┤ │                         │
│ │README.md  │ │  Sector 1+:              │
│ └──────────┘ │  自研 VFS 文件系统          │
│              │  (超级块 + 位图+ Inode表 + │
│              │   数据块)                  │
│              │  → 所有数据 AES-256-XTS    │
│              │     扇区级加密             │
└──────────────┴─────────────────────────┘
```

## 功能

### 文件管理器
- 树状目录浏览与导航
- **导入文件**：从本机任意路径导入到加密区
- **导出文件**：从加密区导出到本机（默认桌面）
- 新建文件夹、重命名、删除
- 右键上下文菜单
- 自动排序（目录在前，文件在后）

### 初始化向导
- 四步引导：确认 → 设密码 → 初始化 → 完成
- 自动分区格式化与加密
- 密码强度校验（最少 8 位）

## 构建

### 前置条件
- CMake ≥ 3.20
- C++17 编译器
- Git（CMake 自动下载 ImGui + GLFW）

### 各平台构建

```bash
# 克隆或进入项目目录
cd SecureDrive

# 配置 + 构建
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release -j$(nproc)
```

### 平台特定说明

| 平台 | 权限要求 | 详细 |
|------|---------|------|
| Windows | **管理员** (UAC) | 程序自动请求提权；`CMakeLists.txt` 已配置 UAC manifest |
| macOS | **root / sudo** | `sudo ./SecureDrive`；需在系统隐私设置中允许磁盘访问 |
| Linux | **root / sudo** | `sudo ./SecureDrive` |

## CI / 自动构建

项目已配置 **GitHub Actions** 自动构建，每次推送代码或打 Tag 时自动编译三个平台的版本：

| 平台 | Runner | 产物 |
|------|--------|------|
| Windows | `windows-latest` | `SecureDrive.exe` |
| macOS | `macos-latest` | `SecureDrive.app` (zip) |
| Linux | `ubuntu-latest` | `SecureDrive` |

### 触发方式
- 每次 `git push` 到 `main`/`master`/`develop` 分支 → 自动编译并上传 Artifact
- 推送 `v*` Tag（如 `v1.0.0`）→ 自动创建 GitHub Release 并附带三个平台的二进制文件

### 手动触发
进入仓库 **Actions → Build SecureDrive → Run workflow** 即可手动触发构建。

## 使用

### 初始化（新盘）

1. 运行 `SecureDrive.exe`（需管理员/root 权限）
2. 选择磁盘 → 选择要加密的分区
3. 点击 **"初始化为新加密分区"**
4. 设置主密码（≥8 位）和紧急密码
5. 等待初始化完成（约 2-3 秒）
6. 自动进入文件管理器

### 日常使用

1. 插入移动硬盘
2. 运行公开分区中的 `SecureDrive.exe`
3. 选择设备 → 选择加密分区 → 点击"打开/解锁"
4. 输入主密码或紧急密码
5. 进入文件管理器，导入/导出/管理文件
6. 完成后点击"锁定"或关闭窗口

### 修改密码
- 主密码：文件管理器菜单 → 更改主密码（需要验证当前密码）
- 紧急密码：文件管理器菜单 → 更改紧急密码（需要验证主密码）

## 密码学细节

### 卷头结构 (Sector 0, 512 Bytes)
```
偏移    字段
0       魔数 "SDRV01\0\0" (8B)
8       版本号 = 1 (4B)
12      标志 (4B)
16      数据扇区数 (8B)
24      扇区大小 (4B)
28      Argon2id t_cost (4B)
32      Argon2id m_cost (4B)
36      Argon2id parallelism (4B)
40      主密码槽 (160B)
200     紧急密码槽 (160B)
360     保留 (152B)
```

### 密码槽结构 (160 Bytes)
```
偏移    字段
0       盐 (32B)
32      AES-CBC IV (16B)
48      加密载荷 (80B)  = AES-256-CBC(MasterKey[64B] || SLOT_MAGIC[16B])
128     HMAC-SHA256(salt || iv || ciphertext, KEK[32..63]) (32B)
```

### 文件系统 (VFS)
- 块大小: 4096 字节（与物理扇区无关）
- Inode 大小: 128 字节
- 直接/间接/二级间接块支持（最大文件 ~16GB per 4KB block）
- 文件系统层次: Superblock → Bitmap → Inode Table → Data Blocks

## 文件列表

```
SecureDrive/
├── CMakeLists.txt
├── README.md
├── build_win.bat   (Windows 一键构建)
├── build_mac.sh    (macOS 一键构建)
├── build_linux.sh  (Linux 一键构建)
├── platform/
│   ├── windows/
│   │   ├── manifest.rc
│   │   └── manifest.xml
│   └── macos/
│       └── Info.plist
└── src/
    ├── main.cpp
    ├── crypto/
    │   ├── sha256.h/cpp
    │   ├── aes.h/cpp
    │   ├── aes_xts.h/cpp
    │   ├── blake2b.h/cpp
    │   ├── argon2id.h/cpp
    │   └── random.h/cpp
    ├── disk/
    │   └── disk.h/cpp
    ├── volume/
    │   ├── volume_format.h
    │   └── volume.h/cpp
    ├── vfs/
    │   └── vfs.h/cpp
    ├── security/
    │   └── lockout.h/cpp
    └── ui/
        ├── app.h/cpp
        └── init_wizard.cpp
```

## 许可

本软件仅供学习和个人使用。使用前请务必备份重要数据。

## 免责声明

- 加密强度依赖于密码复杂度，建议使用 ≥12 位混合字符密码
- 忘记密码且无紧急密码 → 数据永久无法恢复
- 原始分区操作具有危险性，操作前请确认目标设备正确
- Argon2id 参数在当前主流 CPU 上解锁耗时 1-2 秒，旧设备可能更长
=======
# SecureDrive — 便携式移动硬盘加密软件

![Build](https://github.com/yzj327850/SecureDrive/actions/workflows/build.yml/badge.svg)

SecureDrive 是一款完全自包含的移动硬盘加密软件，**不依赖任何外部加密工具**（如 VeraCrypt/BitLocker/LUKS），无需在目标电脑上安装任何驱动或软件。

软件直接放置在移动硬盘的公开分区中，插入任意电脑即可运行，用于解锁并管理加密分区中的文件。

## 安全特性

| 层级 | 技术方案 |
|------|----------|
| 密钥派生 | **Argon2id**（内存 64MB, 迭代 3, 并行 4）— 每次解锁耗时约 1-2 秒 |
| 数据加密 | **AES-256-XTS**（IEEE 1619）— 扇区级别，每扇区独立加密 |
| 防暴力破解 | 连续 **3 次**密码错误 → 锁定 **5 分钟** |
| 时序攻击 | HMAC 和密码验证采用 **constant-time 比较** |
| 密钥安全 | Master Key 随机生成，加密封存在卷头，内存使用后**显式归零** |
| 双密码 | 支持**主密码 + 紧急密码**，两套独立密码均可解锁 |
| 密码更改 | 无需重新加密全盘，仅重加密卷头密钥槽 |

## 架构

```
移动硬盘布局:
┌──────────────┬─────────────────────────┐
│  公开分区      │  加密分区                  │
│  (~100MB)     │  (剩余空间)               │
│              │                         │
│ ┌──────────┐ │  Sector 0: 卷头           │
│ │SecureDrive.exe  │  (Argon2id 盐值,       │
│ │(可执行文件) │   加密的 Master Key)      │
│ ├──────────┤ │                         │
│ │README.md  │ │  Sector 1+:              │
│ └──────────┘ │  自研 VFS 文件系统          │
│              │  (超级块 + 位图+ Inode表 + │
│              │   数据块)                  │
│              │  → 所有数据 AES-256-XTS    │
│              │     扇区级加密             │
└──────────────┴─────────────────────────┘
```

## 功能

### 文件管理器
- 树状目录浏览与导航
- **导入文件**：从本机任意路径导入到加密区
- **导出文件**：从加密区导出到本机（默认桌面）
- 新建文件夹、重命名、删除
- 右键上下文菜单
- 自动排序（目录在前，文件在后）

### 初始化向导
- 四步引导：确认 → 设密码 → 初始化 → 完成
- 自动分区格式化与加密
- 密码强度校验（最少 8 位）

## 构建

### 前置条件
- CMake ≥ 3.20
- C++17 编译器
- Git（CMake 自动下载 ImGui + GLFW）

### 各平台构建

```bash
# 克隆或进入项目目录
cd SecureDrive

# 配置 + 构建
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release -j$(nproc)
```

### 平台特定说明

| 平台 | 权限要求 | 详细 |
|------|---------|------|
| Windows | **管理员** (UAC) | 程序自动请求提权；`CMakeLists.txt` 已配置 UAC manifest |
| macOS | **root / sudo** | `sudo ./SecureDrive`；需在系统隐私设置中允许磁盘访问 |
| Linux | **root / sudo** | `sudo ./SecureDrive` |

## CI / 自动构建

项目已配置 **GitHub Actions** 自动构建，每次推送代码或打 Tag 时自动编译三个平台的版本：

| 平台 | Runner | 产物 |
|------|--------|------|
| Windows | `windows-latest` | `SecureDrive.exe` |
| macOS | `macos-latest` | `SecureDrive.app` (zip) |
| Linux | `ubuntu-latest` | `SecureDrive` |

### 触发方式
- 每次 `git push` 到 `main`/`master`/`develop` 分支 → 自动编译并上传 Artifact
- 推送 `v*` Tag（如 `v1.0.0`）→ 自动创建 GitHub Release 并附带三个平台的二进制文件

### 手动触发
进入仓库 **Actions → Build SecureDrive → Run workflow** 即可手动触发构建。

## 使用

### 初始化（新盘）

1. 运行 `SecureDrive.exe`（需管理员/root 权限）
2. 选择磁盘 → 选择要加密的分区
3. 点击 **"初始化为新加密分区"**
4. 设置主密码（≥8 位）和紧急密码
5. 等待初始化完成（约 2-3 秒）
6. 自动进入文件管理器

### 日常使用

1. 插入移动硬盘
2. 运行公开分区中的 `SecureDrive.exe`
3. 选择设备 → 选择加密分区 → 点击"打开/解锁"
4. 输入主密码或紧急密码
5. 进入文件管理器，导入/导出/管理文件
6. 完成后点击"锁定"或关闭窗口

### 修改密码
- 主密码：文件管理器菜单 → 更改主密码（需要验证当前密码）
- 紧急密码：文件管理器菜单 → 更改紧急密码（需要验证主密码）

## 密码学细节

### 卷头结构 (Sector 0, 512 Bytes)
```
偏移    字段
0       魔数 "SDRV01\0\0" (8B)
8       版本号 = 1 (4B)
12      标志 (4B)
16      数据扇区数 (8B)
24      扇区大小 (4B)
28      Argon2id t_cost (4B)
32      Argon2id m_cost (4B)
36      Argon2id parallelism (4B)
40      主密码槽 (160B)
200     紧急密码槽 (160B)
360     保留 (152B)
```

### 密码槽结构 (160 Bytes)
```
偏移    字段
0       盐 (32B)
32      AES-CBC IV (16B)
48      加密载荷 (80B)  = AES-256-CBC(MasterKey[64B] || SLOT_MAGIC[16B])
128     HMAC-SHA256(salt || iv || ciphertext, KEK[32..63]) (32B)
```

### 文件系统 (VFS)
- 块大小: 4096 字节（与物理扇区无关）
- Inode 大小: 128 字节
- 直接/间接/二级间接块支持（最大文件 ~16GB per 4KB block）
- 文件系统层次: Superblock → Bitmap → Inode Table → Data Blocks

## 文件列表

```
SecureDrive/
├── CMakeLists.txt
├── README.md
├── build_win.bat   (Windows 一键构建)
├── build_mac.sh    (macOS 一键构建)
├── build_linux.sh  (Linux 一键构建)
├── platform/
│   ├── windows/
│   │   ├── manifest.rc
│   │   └── manifest.xml
│   └── macos/
│       └── Info.plist
└── src/
    ├── main.cpp
    ├── crypto/
    │   ├── sha256.h/cpp
    │   ├── aes.h/cpp
    │   ├── aes_xts.h/cpp
    │   ├── blake2b.h/cpp
    │   ├── argon2id.h/cpp
    │   └── random.h/cpp
    ├── disk/
    │   └── disk.h/cpp
    ├── volume/
    │   ├── volume_format.h
    │   └── volume.h/cpp
    ├── vfs/
    │   └── vfs.h/cpp
    ├── security/
    │   └── lockout.h/cpp
    └── ui/
        ├── app.h/cpp
        └── init_wizard.cpp
```

## 许可

本软件仅供学习和个人使用。使用前请务必备份重要数据。

## 免责声明

- 加密强度依赖于密码复杂度，建议使用 ≥12 位混合字符密码
- 忘记密码且无紧急密码 → 数据永久无法恢复
- 原始分区操作具有危险性，操作前请确认目标设备正确
- Argon2id 参数在当前主流 CPU 上解锁耗时 1-2 秒，旧设备可能更长
>>>>>>> 94669b0983ee94d61e25dec671a2d820f353fbe9
