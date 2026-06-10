#pragma once
#include "../volume/volume.h"
#include <string>
#include <vector>
#include <cstdint>
#include <ctime>
#include <unordered_map>

// ============================================================
//  SecureDrive 虚拟文件系统（VFS）
//
//  磁盘布局（相对于数据区，所有访问通过 Volume::read/write_sector）:
//  ┌──────────────────────────────────────────┐
//  │ Sector 0   : VFS 超级块（VfsSuperblock）  │
//  │ Sector 1-B : 块位图（1 bit/block）         │
//  │ Sector B+1 : Inode 表起始                  │
//  │ ...        : 数据块                         │
//  └──────────────────────────────────────────┘
//
//  设计目标：简洁、正确，支持千万级小文件
// ============================================================

// ---- 磁盘常量 ----
static constexpr uint32_t VFS_BLOCK_SIZE     = 4096; // 字节（与扇区大小无关）
static constexpr uint32_t VFS_INODE_SIZE     = 128;  // 字节
static constexpr uint32_t VFS_MAX_FILENAME   = 255;
static constexpr uint32_t VFS_DIRECT_BLOCKS  = 12;
static constexpr uint32_t VFS_MAGIC          = 0x53445246; // "SDRF"
static constexpr uint32_t VFS_ROOT_INODE     = 1;    // 根目录 inode 号
static constexpr uint32_t VFS_INVALID_BLOCK  = 0xFFFFFFFF;
static constexpr uint32_t VFS_INVALID_INODE  = 0;

#pragma pack(push, 1)

struct VfsSuperblock {
    uint32_t magic;          // VFS_MAGIC
    uint32_t version;        // = 1
    uint32_t block_size;     // = VFS_BLOCK_SIZE
    uint64_t total_blocks;   // 数据区总块数
    uint64_t free_blocks;    // 空闲块数
    uint32_t total_inodes;   // Inode 总数
    uint32_t free_inodes;    // 空闲 Inode 数
    uint32_t inode_table_start; // Inode 表起始块号
    uint32_t inode_table_blocks;// Inode 表占用块数
    uint32_t bitmap_start;   // 块位图起始块号
    uint32_t bitmap_blocks;  // 块位图块数
    uint32_t data_start;     // 数据区起始块号
    uint8_t  reserved[4040]; // 补齐到 4096 字节（一整块）
};
static_assert(sizeof(VfsSuperblock) == VFS_BLOCK_SIZE, "Superblock size mismatch");

struct VfsInode {
    uint32_t mode;           // 0=空, 1=文件, 2=目录
    uint32_t flags;          // 保留
    uint64_t size;           // 文件字节大小
    uint32_t ctime;          // 创建时间 (Unix 时间戳)
    uint32_t mtime;          // 修改时间
    uint32_t link_count;     // 硬链接数（目录项引用数）
    uint32_t direct[VFS_DIRECT_BLOCKS]; // 直接块指针
    uint32_t indirect;       // 一级间接块
    uint32_t double_indirect;// 二级间接块
    uint8_t  reserved[44];   // 补齐到 128 字节
};
static_assert(sizeof(VfsInode) == 128, "VfsInode size check");

// 目录条目（可变长，存储在目录文件的数据块中）
struct VfsDirEntry {
    uint32_t inode;          // 指向的 Inode（0=删除）
    uint16_t rec_len;        // 本条目总长度（字节，含 name）
    uint8_t  name_len;       // 文件名字节长度
    uint8_t  file_type;      // 1=文件, 2=目录
    char     name[VFS_MAX_FILENAME + 1]; // 文件名（name_len 字节有效）
};

#pragma pack(pop)

// ============================================================
//  文件信息（面向 UI 的结构）
// ============================================================
struct FileInfo {
    std::string name;       // 显示名称（UI 层保证为 UTF-8）
    std::string raw_name;   // 磁盘上的原始字节名称（用于 VFS 路径查询）
    bool        is_dir;
    uint64_t    size;
    time_t      mtime;
    uint32_t    inode_num;
};

// ============================================================
//  VFS 接口
// ============================================================
class Vfs {
public:
    Vfs()  = default;
    ~Vfs() = default;

    // 在已解锁的 Volume 上格式化（创建新 VFS）
    bool format(Volume* vol);

    // 挂载已有 VFS
    bool mount(Volume* vol);

    // 批量操作模式：延迟 bitmap_commit，最后一次性刷写
    void begin_batch();
    void end_batch();

    bool is_mounted() const { return vol_ && mounted_; }

    // ---- 容量查询 ----
    uint64_t total_capacity() const;   // 总容量（字节）
    uint64_t used_capacity()  const;   // 已用容量（字节）
    uint64_t free_capacity() const;   // 空闲容量（字节）

    // ---- 目录操作 ----
    std::vector<FileInfo> list_dir(const std::string& path);
    bool make_dir(const std::string& path);
    bool remove_dir(const std::string& path);

    // ---- 文件操作 ----
    bool write_file(const std::string& path,
                    const uint8_t* data, uint64_t size);
    bool read_file (const std::string& path,
                    std::vector<uint8_t>& out);
    bool remove_file(const std::string& path);
    bool rename(const std::string& old_path, const std::string& new_path);
    bool exists(const std::string& path);
    bool stat(const std::string& path, FileInfo& info);

private:
    Volume*       vol_     = nullptr;
    bool          mounted_ = false;
    VfsSuperblock sb_{};

    // ---- 内部辅助 ----
    // 块 I/O（将 VFS 块映射到 Volume 扇区，带缓存）
    bool read_block (uint32_t blk, void* buf);
    bool write_block(uint32_t blk, const void* buf);

    // 块缓存（读穿透 + 写穿透 + 写更新缓存）
    void cache_put(uint32_t blk, const void* data);
    bool cache_get(uint32_t blk, void* buf) const;
    void cache_invalidate(uint32_t blk);
    void cache_clear();

    // 超级块 I/O
    bool read_superblock ();
    bool write_superblock();

    // 位图操作
    uint32_t alloc_block();
    void     free_block(uint32_t blk);
    bool     read_bitmap_word(uint32_t word_idx, uint64_t& word);
    bool     write_bitmap_word(uint32_t word_idx, uint64_t word);

    // 位图全量内存缓存（mount/format 时加载）
    void     bitmap_load();   // 从磁盘加载整个位图到内存
    void     bitmap_commit();  // 将内存位图一次性写回磁盘
    std::vector<uint64_t> bitmap_mem_; // 内存位图（每 uint64_t 管理 64 块）

    // Inode 操作
    uint32_t alloc_inode();
    void     free_inode(uint32_t ino);
    bool     read_inode (uint32_t ino, VfsInode& inode);
    bool     write_inode(uint32_t ino, const VfsInode& inode);

    // 路径解析
    uint32_t resolve_path(const std::string& path);
    uint32_t resolve_parent(const std::string& path, std::string& name);

    // 目录操作
    bool dir_add_entry(uint32_t dir_ino, uint32_t child_ino,
                       const std::string& name, uint8_t type);
    bool dir_remove_entry(uint32_t dir_ino, const std::string& name);
    std::vector<FileInfo> read_dir(uint32_t dir_ino);

    // 文件块访问
    uint32_t get_block_of_file(VfsInode& ino, uint32_t block_idx,
                                bool allocate);
    bool     write_file_data(uint32_t ino_num, VfsInode& inode,
                              const uint8_t* data, uint64_t size);
    bool     write_file_data_fast(uint32_t ino_num, VfsInode& inode,
                                   const uint8_t* data, uint64_t size);
    bool     read_file_data (uint32_t ino_num, const VfsInode& inode,
                              std::vector<uint8_t>& out);
    void     free_file_blocks(VfsInode& inode);

    // 块缓存: block_num → 4KB 数据
    mutable std::unordered_map<uint32_t, std::vector<uint8_t>> block_cache_;

    // 块分配游标：alloc_block 从此位置开始搜索，避免立即重用刚释放的块
    uint32_t next_alloc_hint_ = 0;

    // Inode 分配游标：alloc_inode 从此位置开始搜索
    uint32_t next_inode_hint_ = 1;

    // 批量操作模式：延迟 bitmap_commit + write_superblock
    int      bitmap_deferred_ = 0;
    bool     bitmap_dirty_    = false;
};
