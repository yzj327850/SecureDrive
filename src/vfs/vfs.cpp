#include "vfs.h"
#include <cstring>
#include <ctime>
#include <algorithm>
#include <sstream>
#include <cstdio>

// ============================================================
//  VFS 实现
//  注：VFS_BLOCK_SIZE (4096B) 可能大于 Volume sector_size (512B)
//  因此每次块 I/O 会执行多次扇区读写
// ============================================================

// ---- 块缓存实现 ----
void Vfs::cache_put(uint32_t blk, const void* data) {
    auto& entry = block_cache_[blk];
    entry.assign((const uint8_t*)data, (const uint8_t*)data + VFS_BLOCK_SIZE);
}
bool Vfs::cache_get(uint32_t blk, void* buf) const {
    auto it = block_cache_.find(blk);
    if(it == block_cache_.end()) return false;
    memcpy(buf, it->second.data(), VFS_BLOCK_SIZE);
    return true;
}
void Vfs::cache_invalidate(uint32_t blk) {
    block_cache_.erase(blk);
}
void Vfs::cache_clear() {
    block_cache_.clear();
}

// ---- 块 I/O（带缓存 + 批量扇区）----
bool Vfs::read_block(uint32_t blk, void* buf) {
    if(!vol_) return false;
    // 先查缓存
    if(cache_get(blk, buf)) return true;
    // 缓存未命中：批量读取（一次 I/O 读所有扇区）
    uint32_t ss       = vol_->sector_size();
    uint32_t secs_per = VFS_BLOCK_SIZE / ss;
    uint64_t base_sec = (uint64_t)blk * secs_per;
    if(!vol_->read_sectors_batch(base_sec, secs_per, buf)) return false;
    // 放入缓存
    cache_put(blk, buf);
    return true;
}

bool Vfs::write_block(uint32_t blk, const void* buf) {
    if(!vol_) return false;
    // 批量写入（一次 I/O 写所有扇区）
    uint32_t ss       = vol_->sector_size();
    uint32_t secs_per = VFS_BLOCK_SIZE / ss;
    uint64_t base_sec = (uint64_t)blk * secs_per;
    if(!vol_->write_sectors_batch(base_sec, secs_per, buf)) return false;
    // 更新缓存
    cache_put(blk, buf);
    return true;
}

// ---- 超级块 ----
bool Vfs::read_superblock() {
    return read_block(0, &sb_);
}
bool Vfs::write_superblock() {
    return write_block(0, &sb_);
}

// ---- 块位图（全量内存缓存）----
// 位图在 mount/format 时一次性加载到 bitmap_mem_（内存向量）
// alloc_block / free_block 纯内存操作，零磁盘 I/O
// bitmap_commit() 将位图写回磁盘

// ---- 位图操作（全量内存缓存）----
void Vfs::bitmap_load() {
    uint32_t total_words = (uint32_t)((sb_.total_blocks + 63) / 64);
    bitmap_mem_.resize(total_words, 0);
    // 计算需要多少个位图块
    uint32_t words_per_block = VFS_BLOCK_SIZE / sizeof(uint64_t);
    uint32_t bitmap_blocks = (uint32_t)((total_words + words_per_block - 1) / words_per_block);
    // 批量加载：一次性读取所有位图块（比逐 word 读快得多）
    if(bitmap_blocks > 0){
        uint32_t ss       = vol_->sector_size();
        uint32_t secs_per = VFS_BLOCK_SIZE / ss;
        uint64_t base_sec = (uint64_t)sb_.bitmap_start * secs_per;
        uint32_t total_secs = bitmap_blocks * secs_per;
        std::vector<uint8_t> big_buf(bitmap_blocks * VFS_BLOCK_SIZE);
        if(vol_->read_sectors_batch(base_sec, total_secs, big_buf.data())){
            // 从批量缓冲区中提取每个 word
            for(uint32_t w = 0; w < total_words; w++){
                uint32_t blk_idx = w / words_per_block;
                uint32_t off     = (w % words_per_block) * sizeof(uint64_t);
                memcpy(&bitmap_mem_[w], big_buf.data() + blk_idx * VFS_BLOCK_SIZE + off, sizeof(uint64_t));
            }
        } else {
            // 批量读取失败：回退逐块读取
            fprintf(stderr, "[VFS] bitmap_load: 批量读取失败，回退逐块模式\n"); fflush(stderr);
            for(uint32_t w = 0; w < total_words; w++){
                uint32_t blk = sb_.bitmap_start + w / words_per_block;
                uint32_t off = (w % words_per_block) * sizeof(uint64_t);
                std::vector<uint8_t> buf(VFS_BLOCK_SIZE);
                uint32_t ss2       = vol_->sector_size();
                uint32_t secs_per2 = VFS_BLOCK_SIZE / ss2;
                uint64_t base_sec2 = (uint64_t)blk * secs_per2;
                bool ok = false;
                for(uint32_t i=0; i<secs_per2; i++){
                    ok = vol_->read_sector(base_sec2 + i, buf.data() + i*ss2);
                    if(!ok) break;
                }
                if(ok){
                    memcpy(&bitmap_mem_[w], buf.data() + off, sizeof(uint64_t));
                }
            }
        }
    }
}

void Vfs::bitmap_commit() {
    // 批量模式：仅标记脏，不执行实际 I/O
    if(bitmap_deferred_ > 0) {
        bitmap_dirty_ = true;
        return;
    }
    bitmap_dirty_ = false;
    uint32_t total_words = (uint32_t)bitmap_mem_.size();
    if(total_words == 0) return;
    uint32_t words_per_block = VFS_BLOCK_SIZE / sizeof(uint64_t);
    uint32_t ss       = vol_->sector_size();
    uint32_t secs_per = VFS_BLOCK_SIZE / ss;

    // ★ 优化：将整个内存位图序列化后，按块范围批量写回磁盘
    //   旧实现逐扇区 read_sector/write_sector（每次都有一次 AES-XTS 开销）
    //   新实现：先读整个位图区 → 覆盖 → 批量写回（只需两次大 I/O）

    // 计算需要的位图块数
    uint32_t bitmap_blocks = (uint32_t)((total_words + words_per_block - 1) / words_per_block);
    if(bitmap_blocks == 0) return;

    uint32_t total_secs = bitmap_blocks * secs_per;
    uint64_t base_sec   = (uint64_t)sb_.bitmap_start * secs_per;

    // 分配大缓冲区（覆盖所有位图块）
    std::vector<uint8_t> big_buf(bitmap_blocks * VFS_BLOCK_SIZE, 0);

    // 先读出已有内容（避免破坏块内其余字节）
    vol_->read_sectors_batch(base_sec, total_secs, big_buf.data());

    // 将内存位图写入缓冲区
    for(uint32_t w = 0; w < total_words; w++){
        uint32_t blk_idx = w / words_per_block;
        uint32_t off     = (w % words_per_block) * sizeof(uint64_t);
        memcpy(big_buf.data() + blk_idx * VFS_BLOCK_SIZE + off,
               &bitmap_mem_[w], sizeof(uint64_t));
    }

    // 批量写回（一次大 I/O）
    vol_->write_sectors_batch(base_sec, total_secs, big_buf.data());
}

void Vfs::begin_batch() {
    bitmap_deferred_++;
}

void Vfs::end_batch() {
    if(--bitmap_deferred_ <= 0) {
        bitmap_deferred_ = 0;
        if(bitmap_dirty_) {
            // 强制刷写：跳过 deferred 检查
            bitmap_dirty_ = false;
            uint32_t total_words = (uint32_t)bitmap_mem_.size();
            if(total_words == 0) return;
            uint32_t words_per_block = VFS_BLOCK_SIZE / sizeof(uint64_t);
            uint32_t ss       = vol_->sector_size();
            uint32_t secs_per = VFS_BLOCK_SIZE / ss;
            uint32_t bitmap_blocks = (uint32_t)((total_words + words_per_block - 1) / words_per_block);
            if(bitmap_blocks == 0) return;
            uint32_t total_secs = bitmap_blocks * secs_per;
            uint64_t base_sec   = (uint64_t)sb_.bitmap_start * secs_per;
            std::vector<uint8_t> big_buf(bitmap_blocks * VFS_BLOCK_SIZE, 0);
            vol_->read_sectors_batch(base_sec, total_secs, big_buf.data());
            for(uint32_t w = 0; w < total_words; w++){
                uint32_t blk_idx = w / words_per_block;
                uint32_t off     = (w % words_per_block) * sizeof(uint64_t);
                memcpy(big_buf.data() + blk_idx * VFS_BLOCK_SIZE + off,
                       &bitmap_mem_[w], sizeof(uint64_t));
            }
            vol_->write_sectors_batch(base_sec, total_secs, big_buf.data());
            write_superblock();
        }
    }
}

bool Vfs::read_bitmap_word(uint32_t word_idx, uint64_t& word) {
    if(word_idx >= bitmap_mem_.size()) return false;
    word = bitmap_mem_[word_idx];
    return true;
}
bool Vfs::write_bitmap_word(uint32_t word_idx, uint64_t word) {
    if(word_idx >= bitmap_mem_.size()) return false;
    bitmap_mem_[word_idx] = word;
    return true;
}

uint32_t Vfs::alloc_block() {
    uint32_t total_words = (uint32_t)((sb_.total_blocks + 63) / 64);
    uint32_t start_word = (next_alloc_hint_ / 64) % total_words;

    // 第一遍：从 hint 位置向后扫描
    for(uint32_t i=0; i<total_words; i++){
        uint32_t w = (start_word + i) % total_words;
        uint64_t word = 0;
        if(!read_bitmap_word(w, word)) return VFS_INVALID_BLOCK;
        if(word == 0xFFFFFFFFFFFFFFFFULL) continue;
        for(int b=0; b<64; b++){
            if(!((word >> b) & 1)){
                uint32_t blk_no = w*64 + b;
                if(blk_no >= sb_.total_blocks) break;
                if(blk_no < next_alloc_hint_) continue;
                word |= (1ULL << b);
                write_bitmap_word(w, word);  // 只更新缓存
                // 不再清零新块（write_file_data 会立即覆写）
                sb_.free_blocks--;
                next_alloc_hint_ = blk_no + 1;
                return blk_no;
            }
        }
    }

    // 第二遍：从 data_start 扫到 hint-1
    uint32_t ds_word = sb_.data_start / 64;
    uint32_t hint_word = next_alloc_hint_ / 64;
    for(uint32_t w = ds_word; w < hint_word && w < total_words; w++){
        uint64_t word = 0;
        if(!read_bitmap_word(w, word)) return VFS_INVALID_BLOCK;
        if(word == 0xFFFFFFFFFFFFFFFFULL) continue;
        for(int b=0; b<64; b++){
            if(!((word >> b) & 1)){
                uint32_t blk_no = w*64 + b;
                if(blk_no >= sb_.total_blocks || blk_no >= next_alloc_hint_) break;
                word |= (1ULL << b);
                write_bitmap_word(w, word);
                sb_.free_blocks--;
                next_alloc_hint_ = blk_no + 1;
                return blk_no;
            }
        }
    }

    return VFS_INVALID_BLOCK;
}

void Vfs::free_block(uint32_t blk) {
    if(blk == VFS_INVALID_BLOCK) return;
    uint32_t word_idx = blk / 64;
    uint32_t bit      = blk % 64;
    uint64_t word = 0;
    read_bitmap_word(word_idx, word);
    word &= ~(1ULL << bit);
    write_bitmap_word(word_idx, word);
    sb_.free_blocks++;
}

// ---- Inode 操作 ----
bool Vfs::read_inode(uint32_t ino, VfsInode& inode) {
    if(ino == VFS_INVALID_INODE) return false;
    uint32_t inodes_per_block = VFS_BLOCK_SIZE / VFS_INODE_SIZE;
    uint32_t blk = sb_.inode_table_start + (ino-1) / inodes_per_block;
    uint32_t off = ((ino-1) % inodes_per_block) * VFS_INODE_SIZE;
    std::vector<uint8_t> buf(VFS_BLOCK_SIZE);
    if(!read_block(blk, buf.data())) return false;
    memcpy(&inode, buf.data() + off, VFS_INODE_SIZE);
    return true;
}

bool Vfs::write_inode(uint32_t ino, const VfsInode& inode) {
    if(ino == VFS_INVALID_INODE) return false;
    uint32_t inodes_per_block = VFS_BLOCK_SIZE / VFS_INODE_SIZE;
    uint32_t blk = sb_.inode_table_start + (ino-1) / inodes_per_block;
    uint32_t off = ((ino-1) % inodes_per_block) * VFS_INODE_SIZE;
    std::vector<uint8_t> buf(VFS_BLOCK_SIZE);
    if(!read_block(blk, buf.data())) return false;
    memcpy(buf.data() + off, &inode, VFS_INODE_SIZE);
    return write_block(blk, buf.data());
}

uint32_t Vfs::alloc_inode() {
    // 从上次分配位置开始扫描（避免每次从 inode 1 开始遍历 65536 个 inode）
    uint32_t total = sb_.total_inodes;
    uint32_t start = next_inode_hint_;
    for(uint32_t i=0; i<total; i++){
        uint32_t ino = ((start - 1 + i) % total) + 1; // 1-based 循环
        VfsInode inode{};
        if(!read_inode(ino, inode)) continue;
        if(inode.mode == 0){
            next_inode_hint_ = ino + 1; // 下次从下一个开始
            if(next_inode_hint_ > total) next_inode_hint_ = 1;
            return ino;
        }
    }
    return VFS_INVALID_INODE;
}

void Vfs::free_inode(uint32_t ino) {
    VfsInode empty{};
    write_inode(ino, empty);
    sb_.free_inodes++;
    write_superblock();
}

// ---- 格式化 ----
bool Vfs::format(Volume* vol) {
    if(!vol) return false;
    vol_ = vol;

    uint64_t vol_sectors = vol->data_sectors();
    uint32_t ss          = vol->sector_size();

    // 防护：sector_size=0 或不能整除 VFS_BLOCK_SIZE 均拒绝格式化
    if (ss == 0 || VFS_BLOCK_SIZE % ss != 0) {
        fprintf(stderr, "[Vfs::format] 非法 sector_size=%u，拒绝格式化\n", ss);
        fflush(stderr);
        return false;
    }

    uint32_t secs_per_blk= VFS_BLOCK_SIZE / ss;
    uint64_t total_blocks= vol_sectors / secs_per_blk;
    if(total_blocks < 8) return false;

    // 计算布局
    uint32_t total_inodes     = (uint32_t)std::min((uint64_t)65536, total_blocks / 4);
    uint32_t inodes_per_block = VFS_BLOCK_SIZE / VFS_INODE_SIZE;
    uint32_t inode_blocks     = (total_inodes + inodes_per_block - 1) / inodes_per_block;
    uint32_t bitmap_bits      = (uint32_t)total_blocks;
    uint32_t bitmap_blocks    = (bitmap_bits + VFS_BLOCK_SIZE*8 - 1) / (VFS_BLOCK_SIZE*8);

    uint32_t inode_start  = 1;             // 块 0 = 超级块
    uint32_t bitmap_start = inode_start + inode_blocks;
    uint32_t data_start   = bitmap_start + bitmap_blocks;

    // 填超级块
    memset(&sb_, 0, sizeof(sb_));
    sb_.magic              = VFS_MAGIC;
    sb_.version            = 1;
    sb_.block_size         = VFS_BLOCK_SIZE;
    sb_.total_blocks       = total_blocks;
    sb_.free_blocks        = total_blocks - data_start;
    sb_.total_inodes       = total_inodes;
    sb_.free_inodes        = total_inodes - 1; // root 占用 1
    sb_.inode_table_start  = inode_start;
    sb_.inode_table_blocks = inode_blocks;
    sb_.bitmap_start       = bitmap_start;
    sb_.bitmap_blocks      = bitmap_blocks;
    sb_.data_start         = data_start;

    // 清零 Inode 表
    std::vector<uint8_t> zero(VFS_BLOCK_SIZE, 0);
    for(uint32_t b=0; b<inode_blocks; b++)
        write_block(inode_start + b, zero.data());

    // 在内存中构建完整位图，一次性写入磁盘
    // （原来的逐位 read-modify-write 经过 AES-XTS 加解密，2279 次循环
    //   = 4558 次块 I/O，在某些磁盘上会卡死或崩溃）
    {
        uint32_t bitmap_bytes = bitmap_blocks * VFS_BLOCK_SIZE;
        std::vector<uint8_t> bitmap(bitmap_bytes, 0);

        // 标记 data_start 以前的系统块为已用
        for(uint32_t b = 0; b < data_start; b++) {
            bitmap[b / 8] |= (1U << (b % 8));
        }

        // 一次性写入所有位图块
        for(uint32_t b = 0; b < bitmap_blocks; b++) {
            if(!write_block(bitmap_start + b, bitmap.data() + b * VFS_BLOCK_SIZE)) {
                return false;
            }
        }
    }

    // 将位图加载到内存
    bitmap_load();

    // 初始化块分配游标到 data_start
    next_alloc_hint_ = data_start;
    // Inode 分配游标从 2 开始（1 已被 root 占用）
    next_inode_hint_ = 2;

    // 写超级块
    write_superblock();

    // 创建根目录 inode (ino=1)
    VfsInode root{};
    root.mode       = 2; // 目录
    root.link_count = 1;
    root.ctime      = root.mtime = (uint32_t)time(nullptr);
    for(auto& d:root.direct) d=VFS_INVALID_BLOCK;
    root.indirect        = VFS_INVALID_BLOCK;
    root.double_indirect = VFS_INVALID_BLOCK;
    write_inode(VFS_ROOT_INODE, root);

    // 添加 . 和 .. 条目
    dir_add_entry(VFS_ROOT_INODE, VFS_ROOT_INODE, ".",  2);
    dir_add_entry(VFS_ROOT_INODE, VFS_ROOT_INODE, "..", 2);

    // 刷写所有缓存的位图块到磁盘
    bitmap_commit();

    mounted_ = true;
    return true;
}

// ---- 挂载 ----
bool Vfs::mount(Volume* vol) {
    vol_ = vol;
    if(!read_superblock()) return false;
    if(sb_.magic != VFS_MAGIC) return false;
    mounted_ = true;
    // 初始化块分配游标
    next_alloc_hint_ = sb_.data_start;
    // Inode 分配游标从 2 开始
    next_inode_hint_ = 2;
    // 加载位图到内存
    bitmap_load();
    return true;
}

// ---- 目录读取 ----
std::vector<FileInfo> Vfs::read_dir(uint32_t dir_ino) {
    std::vector<FileInfo> result;
    VfsInode inode{};
    if(!read_inode(dir_ino, inode) || inode.mode != 2){
        return result;
    }

    std::vector<uint8_t> data;
    read_file_data(dir_ino, inode, data);

    const uint8_t* p = data.data();
    size_t remaining = data.size();
    while(remaining >= 8){
        const VfsDirEntry* de = (const VfsDirEntry*)p;
        if(de->rec_len < 8 || de->rec_len > remaining) break;
        if(de->inode != VFS_INVALID_INODE && de->inode != 0){
            std::string nm(de->name, de->name_len);
            if(nm != "." && nm != ".."){
                VfsInode child{};
                if(read_inode(de->inode, child)){
                    FileInfo fi;
                    fi.name      = nm;
                    fi.raw_name  = nm;
                    fi.is_dir    = (child.mode == 2);
                    fi.size      = child.size;
                    fi.mtime     = (time_t)child.mtime;
                    fi.inode_num = de->inode;
                    result.push_back(fi);
                }
            }
        }
        p += de->rec_len; remaining -= de->rec_len;
    }
    return result;
}

// ---- 路径解析 ----
uint32_t Vfs::resolve_path(const std::string& path) {
    if(path.empty() || path == "/") return VFS_ROOT_INODE;

    uint32_t cur = VFS_ROOT_INODE;
    std::stringstream ss(path);
    std::string token;
    while(std::getline(ss, token, '/')){
        if(token.empty() || token == ".") continue;
        if(token == ".."){
            // 向上：找 ".." 条目
            VfsInode dir{}; read_inode(cur, dir);
            std::vector<uint8_t> data;
            read_file_data(cur, dir, data);
            const uint8_t* p=data.data(); size_t rem=data.size();
            while(rem>8){
                const VfsDirEntry* de=(const VfsDirEntry*)p;
                if(de->rec_len<8||de->rec_len>rem) break;
                std::string nm(de->name, de->name_len);
                if(nm==".."){cur=de->inode; break;}
                p+=de->rec_len; rem-=de->rec_len;
            }
            continue;
        }
        // 在当前目录中查找 token
        VfsInode dir{}; read_inode(cur, dir);
        std::vector<uint8_t> data;
        read_file_data(cur, dir, data);
        const uint8_t* p=data.data(); size_t rem=data.size();
        uint32_t found = VFS_INVALID_INODE;
        while(rem>8){
            const VfsDirEntry* de=(const VfsDirEntry*)p;
            if(de->rec_len<8||de->rec_len>rem) break;
            std::string nm(de->name, de->name_len);
            if(nm==token){ found=de->inode; break; }
            p+=de->rec_len; rem-=de->rec_len;
        }
        if(found==VFS_INVALID_INODE) return VFS_INVALID_INODE;
        cur=found;
    }
    return cur;
}

uint32_t Vfs::resolve_parent(const std::string& path, std::string& name) {
    size_t pos = path.rfind('/');
    if(pos==std::string::npos){ name=path; return VFS_ROOT_INODE; }
    name = path.substr(pos+1);
    std::string parent = path.substr(0, pos);
    if(parent.empty()) return VFS_ROOT_INODE;
    return resolve_path(parent);
}

// ---- 目录条目操作 ----
bool Vfs::dir_add_entry(uint32_t dir_ino, uint32_t child_ino,
                         const std::string& name, uint8_t type)
{
    VfsInode dir{}; read_inode(dir_ino, dir);

    if(dir.mode != 2) return false;
    if(dir.size > 100ULL * 1024 * 1024 * 1024) return false;

    std::vector<uint8_t> data;
    read_file_data(dir_ino, dir, data);

    VfsDirEntry de{};
    de.inode    = child_ino;
    de.name_len = (uint8_t)std::min(name.size(), (size_t)VFS_MAX_FILENAME);
    de.file_type= type;
    memcpy(de.name, name.data(), de.name_len);
    de.rec_len  = (uint16_t)((offsetof(VfsDirEntry, name) + de.name_len + 3) & ~3);
    if(de.rec_len < 8) de.rec_len = 8;

    size_t old_size = data.size();
    data.resize(old_size + de.rec_len, 0);
    memcpy(data.data() + old_size, &de, de.rec_len);

    dir.mtime = (uint32_t)time(nullptr);
    return write_file_data(dir_ino, dir, data.data(), data.size());
}

bool Vfs::dir_remove_entry(uint32_t dir_ino, const std::string& name) {
    VfsInode dir{}; read_inode(dir_ino, dir);
    std::vector<uint8_t> data;
    read_file_data(dir_ino, dir, data);

    uint8_t* p = data.data();
    size_t rem = data.size();
    std::vector<uint8_t> newdata;
    while(rem > 8){
        VfsDirEntry* de = (VfsDirEntry*)p;
        if(de->rec_len < 8) break;
        std::string nm(de->name, de->name_len);
        if(nm != name){
            newdata.insert(newdata.end(), p, p + de->rec_len);
        }
        p += de->rec_len; rem -= de->rec_len;
    }
    dir.mtime = (uint32_t)time(nullptr);
    return write_file_data(dir_ino, dir, newdata.data(), newdata.size());
}

// ---- 文件块访问 ----
uint32_t Vfs::get_block_of_file(VfsInode& inode, uint32_t idx, bool allocate) {
    uint32_t ptrs_per_blk = VFS_BLOCK_SIZE / sizeof(uint32_t);

    if(idx < VFS_DIRECT_BLOCKS){
        if(inode.direct[idx] == VFS_INVALID_BLOCK && allocate){
            inode.direct[idx] = alloc_block();
        }
        return inode.direct[idx];
    }
    idx -= VFS_DIRECT_BLOCKS;

    if(idx < ptrs_per_blk){
        if(inode.indirect == VFS_INVALID_BLOCK){
            if(!allocate) return VFS_INVALID_BLOCK;
            inode.indirect = alloc_block();
            // 初始化为全 FF
            std::vector<uint32_t> empty(ptrs_per_blk, VFS_INVALID_BLOCK);
            write_block(inode.indirect, empty.data());
        }
        std::vector<uint32_t> ptrs(ptrs_per_blk);
        read_block(inode.indirect, ptrs.data());
        if(ptrs[idx] == VFS_INVALID_BLOCK && allocate){
            ptrs[idx] = alloc_block();
            write_block(inode.indirect, ptrs.data());
        }
        return ptrs[idx];
    }
    idx -= ptrs_per_blk;

    if(idx < ptrs_per_blk * ptrs_per_blk){
        uint32_t l1 = idx / ptrs_per_blk;
        uint32_t l2 = idx % ptrs_per_blk;
        if(inode.double_indirect == VFS_INVALID_BLOCK){
            if(!allocate) return VFS_INVALID_BLOCK;
            inode.double_indirect = alloc_block();
            std::vector<uint32_t> empty(ptrs_per_blk, VFS_INVALID_BLOCK);
            write_block(inode.double_indirect, empty.data());
        }
        std::vector<uint32_t> l1_ptrs(ptrs_per_blk);
        read_block(inode.double_indirect, l1_ptrs.data());
        if(l1_ptrs[l1] == VFS_INVALID_BLOCK){
            if(!allocate) return VFS_INVALID_BLOCK;
            l1_ptrs[l1] = alloc_block();
            std::vector<uint32_t> empty(ptrs_per_blk, VFS_INVALID_BLOCK);
            write_block(l1_ptrs[l1], empty.data());
            write_block(inode.double_indirect, l1_ptrs.data());
        }
        std::vector<uint32_t> l2_ptrs(ptrs_per_blk);
        read_block(l1_ptrs[l1], l2_ptrs.data());
        if(l2_ptrs[l2] == VFS_INVALID_BLOCK && allocate){
            l2_ptrs[l2] = alloc_block();
            write_block(l1_ptrs[l1], l2_ptrs.data());
        }
        return l2_ptrs[l2];
    }
    return VFS_INVALID_BLOCK;
}

bool Vfs::write_file_data(uint32_t ino_num, VfsInode& inode,
                           const uint8_t* data, uint64_t size)
{
    uint64_t total_blocks_needed = (size + VFS_BLOCK_SIZE - 1) / VFS_BLOCK_SIZE;

    // 先释放旧块
    free_file_blocks(inode);
    for(auto& d:inode.direct) d=VFS_INVALID_BLOCK;
    inode.indirect=VFS_INVALID_BLOCK;
    inode.double_indirect=VFS_INVALID_BLOCK;
    inode.size=size;
    inode.mtime=(uint32_t)time(nullptr);

    uint64_t written=0;
    uint32_t blk_idx=0;
    while(written < size){
        uint32_t blk = get_block_of_file(inode, blk_idx++, true);
        if(blk==VFS_INVALID_BLOCK){
            return false;
        }
        size_t chunk = std::min((uint64_t)VFS_BLOCK_SIZE, size-written);
        uint8_t buf[VFS_BLOCK_SIZE];
        memset(buf, 0, VFS_BLOCK_SIZE);
        memcpy(buf, data+written, chunk);
        write_block(blk, buf);
        written += chunk;
    }

    // 一次性刷写：位图 + 超级块 + inode
    bitmap_commit();
    write_superblock();
    return write_inode(ino_num, inode);
}

// ---- 快速写入：纯内存分配 + 排序顺序写入 ----
bool Vfs::write_file_data_fast(uint32_t ino_num, VfsInode& inode,
                                const uint8_t* data, uint64_t size)
{
    uint32_t ptrs_per_blk = VFS_BLOCK_SIZE / sizeof(uint32_t); // 1024
    uint64_t total_blocks_needed = (size + VFS_BLOCK_SIZE - 1) / VFS_BLOCK_SIZE;

    // 先释放旧块
    free_file_blocks(inode);
    for(auto& d:inode.direct) d=VFS_INVALID_BLOCK;
    inode.indirect=VFS_INVALID_BLOCK;
    inode.double_indirect=VFS_INVALID_BLOCK;
    inode.size=size;
    inode.mtime=(uint32_t)time(nullptr);

    // ---- 阶段 1：顺序分配所有数据块（纯内存，零磁盘 I/O）----
    std::vector<uint32_t> blk_list(total_blocks_needed);
    for(uint64_t i=0; i<total_blocks_needed; i++){
        blk_list[i] = alloc_block(); // 纯内存位图操作
        if(blk_list[i] == VFS_INVALID_BLOCK) return false;
    }

    // ---- 构建指针树（全部在内存中，零磁盘 I/O）----
    // 收集需要写入磁盘的指针块
    struct PendingPtr { uint32_t blk_no; std::vector<uint32_t> data; };
    std::vector<PendingPtr> pending_ptrs;

    uint64_t b = 0; // 逻辑块游标

    // 直接块 (0-9)
    for(; b < VFS_DIRECT_BLOCKS && b < total_blocks_needed; b++){
        inode.direct[b] = blk_list[b];
    }

    if(b < total_blocks_needed){
        // 间接块：分配一个块存放指针
        inode.indirect = alloc_block();
        PendingPtr p{inode.indirect, std::vector<uint32_t>(ptrs_per_blk, VFS_INVALID_BLOCK)};
        for(uint32_t j=0; j < ptrs_per_blk && b < total_blocks_needed; j++, b++){
            p.data[j] = blk_list[b];
        }
        pending_ptrs.push_back(std::move(p));
    }

    if(b < total_blocks_needed){
        // 二级间接块
        inode.double_indirect = alloc_block();
        PendingPtr dbl{inode.double_indirect, std::vector<uint32_t>(ptrs_per_blk, VFS_INVALID_BLOCK)};
        uint32_t l1_idx = 0;
        while(b < total_blocks_needed && l1_idx < ptrs_per_blk){
            uint32_t l1_blk = alloc_block(); // 分配 L1 指针块
            dbl.data[l1_idx] = l1_blk;
            PendingPtr l1_p{l1_blk, std::vector<uint32_t>(ptrs_per_blk, VFS_INVALID_BLOCK)};
            for(uint32_t l2=0; l2 < ptrs_per_blk && b < total_blocks_needed; l2++, b++){
                l1_p.data[l2] = blk_list[b];
            }
            pending_ptrs.push_back(std::move(l1_p));
            l1_idx++;
        }
        // 二级间接块必须最后写入（依赖所有 L1 块已写入）
        pending_ptrs.push_back(std::move(dbl));
    }

    // ---- 阶段 2：按物理块号排序后顺序写入数据块 ----
    std::vector<std::pair<uint32_t,uint64_t>> write_order;
    write_order.reserve(total_blocks_needed);
    for(uint64_t i=0; i<total_blocks_needed; i++){
        write_order.push_back({blk_list[i], i});
    }
    std::sort(write_order.begin(), write_order.end(),
              [](const auto& a, const auto& b){ return a.first < b.first; });

    uint32_t ss       = vol_->sector_size();
    uint32_t secs_per = VFS_BLOCK_SIZE / ss;
    // 单次连续写入上限：2048 块 = 8MB，大幅减少磁盘 seek 次数
    const size_t MAX_CONSECUTIVE = 2048;

    for(size_t j=0; j<write_order.size(); ){
        uint64_t data_idx = write_order[j].second;
        uint32_t blk = write_order[j].first;
        size_t consecutive = 1;
        while(j + consecutive < write_order.size() &&
              write_order[j + consecutive].first == blk + (uint32_t)consecutive &&
              consecutive < MAX_CONSECUTIVE){
            consecutive++;
        }

        // 写入 consecutive 个连续数据块
        uint32_t total_bytes = (uint32_t)(consecutive * VFS_BLOCK_SIZE);
        std::vector<uint8_t> buf(total_bytes);
        for(size_t k=0; k<consecutive; k++){
            uint64_t di = write_order[j+k].second;
            size_t chunk = std::min((uint64_t)VFS_BLOCK_SIZE, size - di * VFS_BLOCK_SIZE);
            memcpy(buf.data() + k*VFS_BLOCK_SIZE, data + di * VFS_BLOCK_SIZE, chunk);
        }
        uint64_t base_sec = (uint64_t)blk * secs_per;
        vol_->write_sectors_batch(base_sec, (uint32_t)(consecutive * secs_per), buf.data());
        // 更新缓存
        for(size_t k=0; k<consecutive; k++){
            cache_put(write_order[j+k].first, buf.data() + k*VFS_BLOCK_SIZE);
        }

        j += consecutive;
    }

    // ---- 阶段 3：写入指针块到磁盘 ----
    for(size_t i=0; i<pending_ptrs.size(); i++){
        write_block(pending_ptrs[i].blk_no, pending_ptrs[i].data.data());
    }

    // 刷写元数据（batch 模式下 bitmap_commit 只标记脏）
    bitmap_commit();
    if(bitmap_deferred_ == 0) write_superblock();
    return write_inode(ino_num, inode);
}

bool Vfs::read_file_data(uint32_t /*ino_num*/, const VfsInode& inode,
                          std::vector<uint8_t>& out)
{
    out.resize((size_t)inode.size, 0);
    if(inode.size==0) return true;
    VfsInode tmp = inode; // get_block_of_file 需要 non-const
    uint64_t read_bytes=0;
    uint32_t blk_idx=0;
    while(read_bytes < inode.size){
        uint32_t blk = get_block_of_file(tmp, blk_idx++, false);
        if(blk==VFS_INVALID_BLOCK) break;
        size_t chunk = std::min((uint64_t)VFS_BLOCK_SIZE, inode.size-read_bytes);
        std::vector<uint8_t> buf(VFS_BLOCK_SIZE);
        read_block(blk, buf.data());
        memcpy(out.data()+read_bytes, buf.data(), chunk);
        read_bytes += chunk;
    }
    return true;
}

void Vfs::free_file_blocks(VfsInode& inode) {
    uint32_t ptrs_per_blk = VFS_BLOCK_SIZE / sizeof(uint32_t);
    for(auto& d:inode.direct){ if(d!=VFS_INVALID_BLOCK){ free_block(d); d=VFS_INVALID_BLOCK; } }
    if(inode.indirect!=VFS_INVALID_BLOCK){
        std::vector<uint32_t> ptrs(ptrs_per_blk);
        read_block(inode.indirect, ptrs.data());
        for(auto p:ptrs) if(p!=VFS_INVALID_BLOCK) free_block(p);
        free_block(inode.indirect); inode.indirect=VFS_INVALID_BLOCK;
    }
    if(inode.double_indirect!=VFS_INVALID_BLOCK){
        std::vector<uint32_t> l1(ptrs_per_blk);
        read_block(inode.double_indirect, l1.data());
        for(auto a:l1) if(a!=VFS_INVALID_BLOCK){
            std::vector<uint32_t> l2(ptrs_per_blk);
            read_block(a, l2.data());
            for(auto b:l2) if(b!=VFS_INVALID_BLOCK) free_block(b);
            free_block(a);
        }
        free_block(inode.double_indirect); inode.double_indirect=VFS_INVALID_BLOCK;
    }
}

// ---- 容量查询 ----
uint64_t Vfs::total_capacity() const {
    return (uint64_t)sb_.total_blocks * VFS_BLOCK_SIZE;
}

uint64_t Vfs::used_capacity() const {
    uint64_t free = (uint64_t)sb_.free_blocks * VFS_BLOCK_SIZE;
    uint64_t total = (uint64_t)sb_.total_blocks * VFS_BLOCK_SIZE;
    return (free > total) ? 0 : total - free;
}

uint64_t Vfs::free_capacity() const {
    return (uint64_t)sb_.free_blocks * VFS_BLOCK_SIZE;
}

// ---- 公开接口 ----
std::vector<FileInfo> Vfs::list_dir(const std::string& path) {
    uint32_t ino = resolve_path(path);
    if(ino==VFS_INVALID_INODE) return {};
    return read_dir(ino);
}

bool Vfs::make_dir(const std::string& path) {
    std::string name;
    uint32_t parent = resolve_parent(path, name);
    if(parent==VFS_INVALID_INODE || name.empty()) {
        return false;
    }

    uint32_t ino = alloc_inode();
    if(ino==VFS_INVALID_INODE) {
        return false;
    }

    VfsInode dir{};
    dir.mode=2; dir.link_count=1;
    dir.ctime=dir.mtime=(uint32_t)time(nullptr);
    for(auto& d:dir.direct) d=VFS_INVALID_BLOCK;
    dir.indirect=dir.double_indirect=VFS_INVALID_BLOCK;
    write_inode(ino, dir);

    bool ok = dir_add_entry(ino, ino,    ".",  2);
    if(ok) ok = dir_add_entry(ino, parent, "..", 2);
    if(ok) ok = dir_add_entry(parent, ino, name, 2);

    if(ok) {
        bitmap_commit();
        sb_.free_inodes--;
        write_superblock();
    }
    return ok;
}

bool Vfs::remove_dir(const std::string& path) {
    uint32_t ino = resolve_path(path);
    if(ino==VFS_INVALID_INODE||ino==VFS_ROOT_INODE) return false;
    auto entries = list_dir(path);
    if(!entries.empty()) return false; // 非空目录

    std::string name;
    uint32_t parent = resolve_parent(path, name);
    dir_remove_entry(parent, name);

    VfsInode inode{}; read_inode(ino, inode);
    free_file_blocks(inode);
    free_inode(ino);
    bitmap_commit();
    write_superblock();
    return true;
}

bool Vfs::write_file(const std::string& path,
                     const uint8_t* data, uint64_t size)
{
    std::string name;
    uint32_t parent = resolve_parent(path, name);
    if(parent==VFS_INVALID_INODE||name.empty()) return false;

    uint32_t ino = resolve_path(path);
    bool is_new = (ino==VFS_INVALID_INODE);

    if(is_new){
        ino = alloc_inode();
        if(ino==VFS_INVALID_INODE) return false;
        sb_.free_inodes--;
        if(bitmap_deferred_ == 0) write_superblock(); // 非 batch 模式才立即写
        dir_add_entry(parent, ino, name, 1);
    }
    VfsInode inode{};
    if(!is_new) read_inode(ino, inode);
    else {
        inode.mode=1; inode.link_count=1;
        inode.ctime=(uint32_t)time(nullptr);
        for(auto& d:inode.direct) d=VFS_INVALID_BLOCK;
        inode.indirect=inode.double_indirect=VFS_INVALID_BLOCK;
    }
    bool ok = write_file_data_fast(ino, inode, data, size);
    return ok;
}

bool Vfs::read_file(const std::string& path, std::vector<uint8_t>& out) {
    uint32_t ino = resolve_path(path);
    if(ino==VFS_INVALID_INODE) return false;
    VfsInode inode{}; read_inode(ino, inode);
    if(inode.mode!=1) return false;
    return read_file_data(ino, inode, out);
}

bool Vfs::remove_file(const std::string& path) {
    uint32_t ino = resolve_path(path);
    if(ino==VFS_INVALID_INODE) return false;
    VfsInode inode{}; read_inode(ino, inode);
    if(inode.mode!=1) return false;

    std::string name;
    uint32_t parent = resolve_parent(path, name);
    dir_remove_entry(parent, name);
    free_file_blocks(inode);
    free_inode(ino);
    bitmap_commit();
    write_superblock();
    return true;
}

bool Vfs::rename(const std::string& old_path, const std::string& new_path) {
    uint32_t ino = resolve_path(old_path);
    if(ino==VFS_INVALID_INODE) return false;
    VfsInode inode{}; read_inode(ino, inode);

    std::string old_name, new_name;
    uint32_t old_parent = resolve_parent(old_path, old_name);
    uint32_t new_parent = resolve_parent(new_path, new_name);

    dir_remove_entry(old_parent, old_name);
    dir_add_entry(new_parent, ino, new_name, (uint8_t)inode.mode);
    inode.mtime=(uint32_t)time(nullptr);
    write_inode(ino, inode);
    return true;
}

bool Vfs::exists(const std::string& path) {
    return resolve_path(path) != VFS_INVALID_INODE;
}

bool Vfs::stat(const std::string& path, FileInfo& info) {
    uint32_t ino = resolve_path(path);
    if(ino==VFS_INVALID_INODE) return false;
    VfsInode inode{}; read_inode(ino, inode);
    size_t pos = path.rfind('/');
    info.name    = (pos==std::string::npos) ? path : path.substr(pos+1);
    info.is_dir  = (inode.mode==2);
    info.size    = inode.size;
    info.mtime   = (time_t)inode.mtime;
    info.inode_num = ino;
    return true;
}
