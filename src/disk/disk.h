#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

// ============================================================
//  跨平台原始磁盘 / 分区访问
//  抽象出：枚举磁盘、打开分区、扇区级读写
// ============================================================

static constexpr size_t DEFAULT_SECTOR_SIZE = 512;

struct DiskInfo {
    std::string device_path;  // 设备路径（原始）
    std::string display_name; // 用户可读名称
    uint64_t    total_bytes;  // 总容量
    uint32_t    sector_size;  // 物理扇区大小
    bool        removable;    // 是否为可移动设备
};

struct PartitionInfo {
    std::string device_path;  // 分区设备路径（如 \\.\Harddisk0\Partition2 或 /dev/sda1）
    std::string parent_disk;  // 父磁盘路径（如 \\.\PhysicalDrive0 或 /dev/sda）
    std::string label;        // 卷标
    uint64_t    offset_bytes; // 相对磁盘起始的偏移量
    uint64_t    size_bytes;   // 分区大小
    uint32_t    index;        // 分区序号（0-based）
};

// 平台相关不透明句柄
#ifdef _WIN32
#  include <windows.h>
using NativeHandle = HANDLE;
static const NativeHandle INVALID_NATIVE = INVALID_HANDLE_VALUE;
#else
using NativeHandle = int;
static const NativeHandle INVALID_NATIVE = -1;
#endif

class RawDisk {
public:
    RawDisk() = default;
    ~RawDisk() { close(); }

    // 禁止拷贝
    RawDisk(const RawDisk&) = delete;
    RawDisk& operator=(const RawDisk&) = delete;

    // 打开设备（write_mode=true 需要管理员/root）
    bool open(const std::string& device_path, bool write_mode = false);
    void close();
    bool is_open() const { return handle_ != INVALID_NATIVE; }

    // 读/写扇区（offset 和 size 均为字节，建议对齐至 sector_size）
    bool read (uint64_t offset, void* buf, size_t size);
    bool write(uint64_t offset, const void* buf, size_t size);

    // 锁定卷（Windows: FSCTL_LOCK_VOLUME，防止卷管理器干扰原始写入）
    bool lock_volume();

    // 卸载卷（Windows: FSCTL_DISMOUNT_VOLUME，释放文件系统对卷的锁定）
    bool dismount_volume();

    // 允许扩展 DASD I/O（Windows: FSCTL_ALLOW_EXTENDED_DASD_IO）
    // 解除卷末尾文件系统保护区域（如 NTFS 备份引导扇区）的写入限制
    bool allow_extended_dasd_io();

    // 刷新磁盘写缓存（确保所有挂起写入落盘）
    bool flush();

    // 获取设备大小
    uint64_t device_size() const { return device_size_; }
    uint32_t sector_size() const { return sector_size_; }

    // 是否使用了 FILE_FLAG_NO_BUFFERING 打开
    bool no_buffering() const { return no_buffering_; }

private:
    NativeHandle handle_      = INVALID_NATIVE;
    uint64_t     device_size_ = 0;
    uint32_t     sector_size_ = DEFAULT_SECTOR_SIZE;
    bool         no_buffering_ = false;
};

// ---- 设备枚举 ----
std::vector<DiskInfo>      enum_disks();
std::vector<PartitionInfo> enum_partitions(const std::string& disk_path);

// ---- 卷管理（跨平台） ----

#ifdef _WIN32
// 卸载指定磁盘上所有已挂载的卷（Windows 版本，按磁盘编号）
bool dismount_volumes_on_disk(int disk_num);

// 从设备路径 " \\.\PhysicalDriveN " 提取磁盘编号
int extract_disk_number(const std::string& device_path);

// 等待 Windows 为指定偏移的分区分配盘符（最多等待 timeout_ms）
char wait_for_drive_letter(int disk_num, uint64_t part_offset, int timeout_ms);
#endif

// 卸载指定磁盘上所有已挂载的卷（macOS/Linux 版本，按设备路径）
#ifndef _WIN32
bool dismount_volumes_on_disk(const std::string& disk_path);
#endif

// ---- 便携双分区布局（跨平台） ----
struct PortableLayoutResult {
    bool    ok               = false;
    uint64_t boot_offset     = 0;  // 明文分区起始字节偏移
    uint64_t boot_size       = 0;  // 明文分区大小（字节）
    uint64_t crypto_offset   = 0;  // 加密分区起始字节偏移
    uint64_t crypto_size     = 0;  // 加密分区大小（字节）
    std::string error_msg;
};

// 在整个磁盘上创建 MBR 双分区布局：
//   分区1 = boot_mb MB 的 FAT32 明文分区
//   分区2 = 剩余空间，无文件系统（SecureDrive 写入）
// disk_path: Windows=\\.\PhysicalDriveN, macOS=/dev/diskN, Linux=/dev/sdX
// boot_mb: 明文分区大小（默认 100MB）
PortableLayoutResult create_portable_layout(const std::string& disk_path, uint32_t boot_mb = 100);

// 格式化指定分区为 FAT32
// Windows: 传入盘符字符（如 'E'），label 为卷标
// macOS/Linux: 传入设备路径（如 /dev/disk1s1），label 为卷标
bool format_volume_fat32(const std::string& device_or_letter, const std::string& label = "SDRV_BOOT");

// 等待分区挂载/分配盘符，返回挂载点路径或盘符，超时返回空字符串
// Windows: 返回盘符（如 "E:"）
// macOS: 返回挂载点（如 "/Volumes/SDRV_BOOT"）
// Linux: 返回挂载点（如 "/run/media/user/SDRV_BOOT"）
std::string wait_for_mount_point(const std::string& device_path, int timeout_ms = 8000);

// 获取设备当前的挂载点/盘符
std::string get_mount_point(const std::string& device_path);

// 复制文件或目录（跨平台递归复制）
bool copy_file_or_dir(const std::string& src, const std::string& dst);

// 将 exe/可执行文件复制到目标路径（保持向后兼容的包装）
bool copy_exe_to(const std::string& exe_src, const std::string& dest_path);
