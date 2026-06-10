#pragma once
#include "volume_format.h"
#include "../crypto/aes_xts.h"
#include "../disk/disk.h"
#include <string>
#include <cstdint>
#include <functional>

// ============================================================
//  卷管理：创建、打开（解锁）、锁定、扇区读写
// ============================================================

enum class VolumeState {
    Closed,   // 未打开
    Locked,   // 已打开但未解密
    Unlocked, // 已解锁，可读写
    Wiped     // 数据已被擦除（连续密码错误达到上限）
};

class Volume {
public:
    Volume() = default;
    ~Volume() { lock(); }

    // ---- 初始化流程 ----
    /**
     * @brief 在指定分区上创建新的加密卷
     * @param device_path  原始分区设备路径（或父磁盘路径）
     * @param offset       分区在磁盘上的字节偏移（0 = 从设备起始开始）
     * @param primary_pass 主密码
     * @param emerg_pass   紧急密码
     */
    bool create(const std::string& device_path,
                uint64_t           offset,
                const std::string& primary_pass,
                const std::string& emerg_pass);

    /**
     * @brief 免格式化加密：就地加密已有数据的分区
     * VolumeHeader 放在分区末尾，从 Sector 0 开始逐扇区加密
     * @param partition_size 分区字节大小（0=自动用磁盘剩余大小，非便携模式可用0）
     * @param progress_cb 进度回调 (0.0~1.0)，可为 nullptr
     */
    bool create_inplace(const std::string& device_path,
                        uint64_t           offset,
                        uint64_t           partition_size,
                        const std::string& primary_pass,
                        const std::string& emerg_pass,
                        std::function<void(float)> progress_cb = nullptr,
                        const std::string& partition_device_path = "");

    // ---- 打开 / 解锁 ----
    bool open  (const std::string& device_path, uint64_t offset = 0);
    bool unlock(const std::string& password);
    void lock  ();

    // ---- 修改密码 ----
    bool change_primary  (const std::string& old_pass, const std::string& new_pass);
    bool change_emergency(const std::string& current_primary, const std::string& new_emerg);

    // ---- 数据自毁 ----
    // 擦除整个加密分区（零填充），不可恢复
    bool wipe();

    // ---- 失败计数器 ----
    int  fail_count() const { return fail_count_; }

    // ---- 数据读写（扇区相对编号，0 = 数据区第一个扇区，即物理第 1 扇区） ----
    bool read_sector (uint64_t sector_num, void* buf);
    bool write_sector(uint64_t sector_num, const void* buf);

    // 批量扇区读写：一次 seek + 一次大 I/O + 批量 XTS 加密
    // sector_count 个连续扇区从 sector_num 开始
    bool read_sectors_batch (uint64_t sector_num, uint32_t sector_count, void* buf);
    bool write_sectors_batch(uint64_t sector_num, uint32_t sector_count, const void* buf);

    // ---- NTFS 直通读取（免格式化迁移专用）----
    // lba 是分区内逻辑扇区号（0 = Boot Sector），解密后返回明文
    // 用于 NtfsReader 解析已就地加密的 NTFS 分区
    bool read_sectors(uint64_t lba, void* buf, uint32_t sector_count);

    // ---- 状态查询 ----
    VolumeState state()        const { return state_; }
    uint64_t    data_sectors() const { return header_.data_sectors; }
    uint32_t    sector_size()  const { return header_.sector_size; }
    bool        is_unlocked()  const { return state_ == VolumeState::Unlocked; }
    bool        is_wiped()     const { return state_ == VolumeState::Wiped; }
    bool        is_v2_tail()   const { return (header_.flags & HEADER_FLAG_TAIL) != 0; }
    uint64_t    total_partition_sectors() const;

    // ---- 诊断接口（仅用于调试）----
    const AesXtsCtx& xts_ctx()          const { return xts_; }
    uint64_t         partition_offset()  const { return partition_offset_; }
    bool             raw_disk_write(uint64_t offset, const void* data, size_t len) {
        return disk_.write(offset, data, len);
    }
    bool             raw_disk_read(uint64_t offset, void* buf, size_t len) {
        return disk_.read(offset, buf, len);
    }

private:
    // 从密码派生 KEK，尝试解密密码槽，返回是否成功以及解密出的 master key
    bool try_unlock_slot(const PasswordSlot& slot,
                         const std::string&  password,
                         uint8_t master_key[MASTER_KEY_SIZE]);

    // 用 master_key 加密并写入密码槽
    bool seal_slot(PasswordSlot&       slot,
                   const std::string&  password,
                   const uint8_t       master_key[MASTER_KEY_SIZE]);

    RawDisk     disk_;
    VolumeHeader header_{};
    AesXtsCtx   xts_{};
    uint8_t     master_key_[MASTER_KEY_SIZE]{};
    VolumeState state_ = VolumeState::Closed;
    uint64_t    partition_offset_ = 0; // 分区在磁盘上的字节偏移
    int         fail_count_       = 0; // 连续密码错误次数（从卷头读取）
};
