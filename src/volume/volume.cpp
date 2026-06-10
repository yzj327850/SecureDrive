#include "volume.h"
#include "../crypto/argon2id.h"
#include "../crypto/aes.h"
#include "../crypto/sha256.h"
#include "../crypto/random.h"
#include <cstring>
#include <vector>
#include <cstdio>
#include <algorithm>

// ============================================================
//  Windows 卷设备辅助（免格式化加密 fallback）
// ============================================================
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>

// 枚举系统所有卷，找到属于指定磁盘、指定偏移的卷，返回其 GUID 路径
// （去掉尾部反斜杠，因为 CreateFile 需要无尾部反斜杠的路径）
static std::string find_volume_path_on_disk(int disk_number, uint64_t partition_offset) {
    char vol_name[MAX_PATH];
    HANDLE hFind = FindFirstVolumeA(vol_name, MAX_PATH);
    if (hFind == INVALID_HANDLE_VALUE) return "";

    do {
        // CreateFile 打开卷时需要去掉尾部反斜杠
        size_t len = strlen(vol_name);
        if (len > 0 && vol_name[len - 1] == '\\') vol_name[len - 1] = '\0';

        HANDLE hVol = CreateFileA(vol_name, GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
        if (hVol == INVALID_HANDLE_VALUE) continue;

        char buf[4096];
        DWORD bytes_returned = 0;
        if (DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                            nullptr, 0, buf, sizeof(buf), &bytes_returned, nullptr)) {
            VOLUME_DISK_EXTENTS* vde = (VOLUME_DISK_EXTENTS*)buf;
            for (DWORD i = 0; i < vde->NumberOfDiskExtents; i++) {
                if (vde->Extents[i].DiskNumber == (DWORD)disk_number &&
                    vde->Extents[i].StartingOffset.QuadPart == (LONGLONG)partition_offset) {
                    CloseHandle(hVol);
                    FindVolumeClose(hFind);
                    return vol_name;
                }
            }
        }
        CloseHandle(hVol);
    } while (FindNextVolumeA(hFind, vol_name, MAX_PATH));

    FindVolumeClose(hFind);
    return "";
}

// 打开卷、卸载并锁定它
static HANDLE lock_volume_by_path(const std::string& vol_path) {
    HANDLE hVol = CreateFileA(vol_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (hVol == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    DWORD dummy;
    DeviceIoControl(hVol, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &dummy, nullptr);
    if (DeviceIoControl(hVol, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &dummy, nullptr)) {
        fprintf(stderr, "[lock_volume] Locked %s\n", vol_path.c_str());
        fflush(stderr);
        return hVol;
    }
    CloseHandle(hVol);
    return INVALID_HANDLE_VALUE;
}
#endif

// ============================================================
//  失败计数器 HMAC 辅助
//
//  HMAC 密钥 = SHA256("SDRV01_FAIL_INTG_V1")，嵌入二进制中
//  防止攻击者通过 hex editor 直接篡改 fail_count
// ============================================================

static void compute_fail_hmac_key(uint8_t key[32]) {
    sha256((const uint8_t*)"SDRV01_FAIL_INTG_V1", 21, key);
}

static void compute_fail_hmac(uint32_t count, uint8_t mac[32]) {
    uint8_t key[32];
    compute_fail_hmac_key(key);
    uint8_t data[4];
    memcpy(data, &count, 4); // LE
    hmac_sha256(key, 32, data, 4, mac);
    memset(key, 0, 32);
}

static bool verify_fail_hmac(uint32_t count, const uint8_t mac[32]) {
    uint8_t computed[32];
    compute_fail_hmac(count, computed);
    uint8_t diff = 0;
    for(int i = 0; i < 32; i++) diff |= computed[i] ^ mac[i];
    return diff == 0;
}

// 从卷头 reserved 区域读取并校验 fail_count
// HMAC 校验失败 → 认为被篡改，返回 0（不惩罚合法用户）
static uint32_t read_fail_count_from_header(const VolumeHeader& hdr) {
    uint32_t count = 0;
    memcpy(&count, hdr.reserved, 4);
    if(!verify_fail_hmac(count, hdr.reserved + 4)) {
        return 0; // HMAC 不匹配，可能被篡改，重置为 0
    }
    return count;
}

// 将 fail_count + HMAC 写入卷头 reserved 区域
static void write_fail_count_to_header(VolumeHeader& hdr, uint32_t count) {
    memcpy(hdr.reserved, &count, 4);
    compute_fail_hmac(count, hdr.reserved + 4);
}

// ============================================================
//  AES-256-CBC 辅助（仅用于密码槽加解密，数据区用 XTS）
// ============================================================

static void aes256_cbc_encrypt(const uint8_t key[32], const uint8_t iv[16],
                                const uint8_t* in, uint8_t* out, size_t len)
{
    Aes256Ctx ctx;
    aes256_init(&ctx, key);
    uint8_t prev[16];
    memcpy(prev, iv, 16);
    for(size_t i=0; i<len; i+=16){
        uint8_t block[16];
        for(int j=0;j<16;j++) block[j]=in[i+j]^prev[j];
        aes256_encrypt_block(&ctx, block, out+i);
        memcpy(prev, out+i, 16);
    }
    aes256_clear(&ctx);
}

static void aes256_cbc_decrypt(const uint8_t key[32], const uint8_t iv[16],
                                const uint8_t* in, uint8_t* out, size_t len)
{
    Aes256Ctx ctx;
    aes256_init(&ctx, key);
    uint8_t prev[16];
    memcpy(prev, iv, 16);
    for(size_t i=0; i<len; i+=16){
        uint8_t block[16];
        aes256_decrypt_block(&ctx, in+i, block);
        for(int j=0;j<16;j++) out[i+j]=block[j]^prev[j];
        memcpy(prev, in+i, 16);
    }
    aes256_clear(&ctx);
}

// ============================================================
//  密码槽：密封（加密写入）
// ============================================================
bool Volume::seal_slot(PasswordSlot& slot,
                       const std::string& password,
                       const uint8_t master_key[MASTER_KEY_SIZE])
{
    // 1. 生成随机盐和 IV
    if(!secure_random(slot.salt, SALT_SIZE)) return false;
    if(!secure_random(slot.iv,   SLOT_IV_SIZE)) return false;

    // 2. 派生 KEK（128 字节）
    uint8_t kek[ARGON2_OUTPUT_SIZE]; // 64 字节
    if(argon2id_hash(
        (const uint8_t*)password.data(), password.size(),
        slot.salt, SALT_SIZE,
        kek, sizeof(kek),
        header_.argon2_t_cost,
        header_.argon2_m_cost,
        header_.argon2_parallelism) != 0) return false;

    // 3. 构造明文 = MasterKey(64B) + SLOT_MAGIC(16B)
    uint8_t plaintext[SLOT_PLAINTEXT];
    memcpy(plaintext,              master_key, MASTER_KEY_SIZE);
    memcpy(plaintext+MASTER_KEY_SIZE, SLOT_MAGIC, 16);

    // 4. 用 KEK[0..31] 做 AES-256-CBC 加密
    aes256_cbc_encrypt(kek, slot.iv, plaintext, slot.ciphertext, SLOT_PLAINTEXT);

    // 5. 计算 HMAC-SHA256(salt || iv || ciphertext, KEK[32..63])
    std::vector<uint8_t> msg(SALT_SIZE + SLOT_IV_SIZE + SLOT_CIPHERTEXT);
    memcpy(msg.data(),                          slot.salt,       SALT_SIZE);
    memcpy(msg.data() + SALT_SIZE,              slot.iv,         SLOT_IV_SIZE);
    memcpy(msg.data() + SALT_SIZE+SLOT_IV_SIZE, slot.ciphertext, SLOT_CIPHERTEXT);
    hmac_sha256(kek+32, 32, msg.data(), msg.size(), slot.mac);

    // 安全清除
    memset(kek, 0, sizeof(kek));
    memset(plaintext, 0, sizeof(plaintext));
    return true;
}

// ============================================================
//  密码槽：尝试解锁
// ============================================================
bool Volume::try_unlock_slot(const PasswordSlot& slot,
                              const std::string& password,
                              uint8_t master_key[MASTER_KEY_SIZE])
{
    // 1. 派生 KEK
    uint8_t kek[ARGON2_OUTPUT_SIZE];
    if(argon2id_hash(
        (const uint8_t*)password.data(), password.size(),
        slot.salt, SALT_SIZE,
        kek, sizeof(kek),
        header_.argon2_t_cost,
        header_.argon2_m_cost,
        header_.argon2_parallelism) != 0) return false;

    // 2. 验证 HMAC（constant-time 比较，防时序攻击）
    std::vector<uint8_t> msg(SALT_SIZE + SLOT_IV_SIZE + SLOT_CIPHERTEXT);
    memcpy(msg.data(),                          slot.salt,       SALT_SIZE);
    memcpy(msg.data() + SALT_SIZE,              slot.iv,         SLOT_IV_SIZE);
    memcpy(msg.data() + SALT_SIZE+SLOT_IV_SIZE, slot.ciphertext, SLOT_CIPHERTEXT);
    uint8_t computed_mac[32];
    hmac_sha256(kek+32, 32, msg.data(), msg.size(), computed_mac);

    // Constant-time compare
    uint8_t diff = 0;
    for(int i=0;i<32;i++) diff |= (computed_mac[i] ^ slot.mac[i]);
    if(diff != 0){ memset(kek,0,sizeof(kek)); return false; }

    // 3. AES-256-CBC 解密
    uint8_t plaintext[SLOT_PLAINTEXT];
    aes256_cbc_decrypt(kek, slot.iv, slot.ciphertext, plaintext, SLOT_PLAINTEXT);
    memset(kek, 0, sizeof(kek));

    // 4. 验证 SLOT_MAGIC
    if(memcmp(plaintext + MASTER_KEY_SIZE, SLOT_MAGIC, 16) != 0){
        memset(plaintext, 0, sizeof(plaintext));
        return false;
    }

    memcpy(master_key, plaintext, MASTER_KEY_SIZE);
    memset(plaintext, 0, sizeof(plaintext));
    return true;
}

// ============================================================
//  创建新卷
// ============================================================
bool Volume::create(const std::string& device_path,
                    uint64_t           offset,
                    const std::string& primary_pass,
                    const std::string& emerg_pass)
{
    if(!disk_.open(device_path, true)) return false;

    partition_offset_ = offset;
    uint64_t total = disk_.device_size();
    uint32_t ss    = disk_.sector_size();

    // 防护：sector_size=0 表示磁盘驱动未就绪（便携模式重新分区后常见）
    if (ss == 0) {
        fprintf(stderr, "[Volume::create] sector_size=0, 磁盘驱动未就绪，拒绝创建\n");
        fflush(stderr);
        return false;
    }
    // 如果是分区，容量应取分区大小；如果是整个设备，用设备总大小
    if (offset > 0 && offset < total) {
        total -= offset;
    }
    if(total < ss * 2) return false;

    // 初始化卷头
    memset(&header_, 0, sizeof(header_));
    memcpy(header_.magic, VOLUME_MAGIC, 8);
    header_.version            = VOLUME_VERSION;
    header_.data_sectors       = total / ss - 1; // 第 0 扇区留给卷头
    header_.sector_size        = ss;
    header_.argon2_t_cost      = ARGON2_TIME_COST;
    header_.argon2_m_cost      = ARGON2_MEMORY_KB;
    header_.argon2_parallelism = ARGON2_PARALLELISM;

    // 生成随机主密钥
    if(!secure_random(master_key_, MASTER_KEY_SIZE)) return false;

    // 加密两个密码槽
    if(!seal_slot(header_.primary_slot,   primary_pass, master_key_)) return false;
    if(!seal_slot(header_.emergency_slot, emerg_pass,   master_key_)) return false;

    // 初始化失败计数器（reserved 区域）
    fail_count_ = 0;
    write_fail_count_to_header(header_, 0);
    header_.reserved[36] = 0; // wiped_flag = false

    // 写入卷头（第 0 扇区，相对于分区起始）
    if(!disk_.write(partition_offset_, &header_, sizeof(header_))) return false;

    // 初始化 XTS 引擎
    aes_xts_init(&xts_, master_key_);
    state_ = VolumeState::Unlocked;
    return true;
}

// ============================================================
//  打开已有卷（支持 V1 头部在 Sector 0，V2 头部在分区末尾）
// ============================================================
bool Volume::open(const std::string& device_path, uint64_t offset) {
    if(!disk_.open(device_path, true)) return false;

    partition_offset_ = offset;
    uint8_t sector_buf[DEFAULT_SECTOR_SIZE];

    // 先尝试 V1：从 Sector 0 读取
    if(!disk_.read(partition_offset_, sector_buf, sizeof(sector_buf))) return false;
    memcpy(&header_, sector_buf, sizeof(VolumeHeader));

    if(memcmp(header_.magic, VOLUME_MAGIC, 8) == 0 &&
       (header_.version == VOLUME_VERSION || header_.version == VOLUME_VERSION_V2)) {
        // V1 或 V2 头部都在 Sector 0（正常卷或 V2 格式化卷）
        goto header_loaded;
    }

    // V1 不匹配，尝试 V2 尾部头部
    {
        uint64_t total = disk_.device_size();
        if(offset > 0 && offset < total) total -= offset;
        uint64_t tail_phys = partition_offset_ + total - (uint64_t)TAIL_HEADER_SECTORS * DEFAULT_SECTOR_SIZE;
        if(disk_.read(tail_phys, sector_buf, sizeof(sector_buf))) {
            VolumeHeader tail_hdr;
            memcpy(&tail_hdr, sector_buf, sizeof(VolumeHeader));
            if(memcmp(tail_hdr.magic, VOLUME_MAGIC, 8) == 0 &&
               tail_hdr.version == VOLUME_VERSION_V2 &&
               (tail_hdr.flags & HEADER_FLAG_TAIL)) {
                memcpy(&header_, &tail_hdr, sizeof(VolumeHeader));
                goto header_loaded;
            }
        }
    }

    return false; // 两种位置都没有找到有效头部

header_loaded:
    // 读取并校验失败计数器
    fail_count_ = (int)read_fail_count_from_header(header_);

    // 检查是否已被擦除
    if(header_.reserved[36] == 1 || fail_count_ >= WIPE_MAX_ATTEMPTS) {
        state_ = VolumeState::Wiped;
        return true;
    }

    state_ = VolumeState::Locked;
    return true;
}

// ============================================================
//  解锁
// ============================================================
bool Volume::unlock(const std::string& password) {
    if(state_ != VolumeState::Locked) return false;

    uint8_t mk[MASTER_KEY_SIZE];
    // 先尝试主密码槽，再尝试紧急密码槽
    bool ok = try_unlock_slot(header_.primary_slot,   password, mk) ||
              try_unlock_slot(header_.emergency_slot, password, mk);

    if(ok) {
        // 密码正确：重置失败计数器
        fail_count_ = 0;
        write_fail_count_to_header(header_, 0);
        header_.reserved[36] = 0; // wiped_flag = false
        // 写回卷头（V1→Sector 0，V2 tail→分区末尾）
        if(is_v2_tail()) {
            uint64_t total = disk_.device_size();
            if(partition_offset_ > 0 && partition_offset_ < total) total -= partition_offset_;
            uint64_t tail_phys = partition_offset_ + total - (uint64_t)TAIL_HEADER_SECTORS * DEFAULT_SECTOR_SIZE;
            disk_.write(tail_phys, &header_, sizeof(header_));
        } else {
            disk_.write(partition_offset_, &header_, sizeof(header_));
        }

        memcpy(master_key_, mk, MASTER_KEY_SIZE);
        memset(mk, 0, sizeof(mk));
        aes_xts_init(&xts_, master_key_);
        state_ = VolumeState::Unlocked;
        fprintf(stderr, "[UNLOCK] 密码正确，失败计数器已重置\n"); fflush(stderr);
        return true;
    }

    // 密码错误：递增失败计数器
    fail_count_++;
    fprintf(stderr, "[UNLOCK] 密码错误，连续失败次数=%d/%d\n",
            fail_count_, WIPE_MAX_ATTEMPTS); fflush(stderr);

    if(fail_count_ >= WIPE_MAX_ATTEMPTS) {
        // 达到上限 → 擦除数据
        fprintf(stderr, "[WIPE] 连续错误 %d 次，触发数据擦除！\n",
                WIPE_MAX_ATTEMPTS); fflush(stderr);
        wipe();
    } else {
        // 未达上限，只更新计数器
        write_fail_count_to_header(header_, (uint32_t)fail_count_);
        if(is_v2_tail()) {
            uint64_t total = disk_.device_size();
            if(partition_offset_ > 0 && partition_offset_ < total) total -= partition_offset_;
            uint64_t tail_phys = partition_offset_ + total - (uint64_t)TAIL_HEADER_SECTORS * DEFAULT_SECTOR_SIZE;
            disk_.write(tail_phys, &header_, sizeof(header_));
        } else {
            disk_.write(partition_offset_, &header_, sizeof(header_));
        }
    }

    memset(mk, 0, sizeof(mk));
    return false;
}

// ============================================================
//  锁定（清除内存中的密钥）
// ============================================================
void Volume::lock() {
    aes_xts_clear(&xts_);
    memset(master_key_, 0, MASTER_KEY_SIZE);
    if(state_ == VolumeState::Unlocked)
        state_ = VolumeState::Locked;
    // Wiped 状态保持不变
}

// ============================================================
//  修改主密码
// ============================================================
bool Volume::change_primary(const std::string& old_pass,
                             const std::string& new_pass)
{
    if(!is_unlocked()) return false;
    // 验证旧密码
    uint8_t mk[MASTER_KEY_SIZE];
    if(!try_unlock_slot(header_.primary_slot, old_pass, mk)) return false;

    if(!seal_slot(header_.primary_slot, new_pass, master_key_)) return false;
    // 写回卷头
    return disk_.write(partition_offset_, &header_, sizeof(header_));
}

bool Volume::change_emergency(const std::string& current_primary,
                               const std::string& new_emerg)
{
    if(!is_unlocked()) return false;
    uint8_t mk[MASTER_KEY_SIZE];
    if(!try_unlock_slot(header_.primary_slot, current_primary, mk)) return false;

    if(!seal_slot(header_.emergency_slot, new_emerg, master_key_)) return false;
    return disk_.write(partition_offset_, &header_, sizeof(header_));
}

// ============================================================
//  扇区读写（数据区，sector_num 相对于数据区第 0 扇区）
//  V1: 物理偏移 = partition_offset + (sector_num + 1) * sector_size（跳过卷头扇区）
//  V2 tail: 物理偏移 = partition_offset + sector_num * sector_size（从 Sector 0 开始加密）
// ============================================================
bool Volume::read_sector(uint64_t sector_num, void* buf) {
    if(!is_unlocked()) return false;
    if(sector_num >= header_.data_sectors) return false;
    uint32_t ss = header_.sector_size;
    uint64_t header_skip = is_v2_tail() ? 0 : ss;
    uint64_t phys_offset = partition_offset_ + header_skip + (uint64_t)sector_num * ss;

    std::vector<uint8_t> cipher(ss);
    if(!disk_.read(phys_offset, cipher.data(), ss)) return false;
    aes_xts_decrypt(&xts_, sector_num, cipher.data(), (uint8_t*)buf, ss);
    return true;
}

bool Volume::write_sector(uint64_t sector_num, const void* buf) {
    if(!is_unlocked()) return false;
    if(sector_num >= header_.data_sectors) return false;
    uint32_t ss = header_.sector_size;
    uint64_t header_skip = is_v2_tail() ? 0 : ss;
    uint64_t phys_offset = partition_offset_ + header_skip + (uint64_t)sector_num * ss;

    std::vector<uint8_t> cipher(ss);
    aes_xts_encrypt(&xts_, sector_num, (const uint8_t*)buf, cipher.data(), ss);
    return disk_.write(phys_offset, cipher.data(), ss);
}

// ============================================================
//  批量扇区读写（一次 seek + 大 I/O + 批量 XTS 加解密）
// ============================================================
bool Volume::read_sectors_batch(uint64_t sector_num, uint32_t sector_count, void* buf) {
    if(!is_unlocked()) return false;
    if(sector_count == 0) return true;
    if(sector_num + sector_count > header_.data_sectors) return false;
    uint32_t ss = header_.sector_size;
    uint64_t header_skip = is_v2_tail() ? 0 : (uint64_t)ss;
    uint64_t phys_offset = partition_offset_ + header_skip + (uint64_t)sector_num * ss;
    uint32_t total_bytes = sector_count * ss;

    // 一次性读取所有密文
    std::vector<uint8_t> cipher(total_bytes);
    if(!disk_.read(phys_offset, cipher.data(), total_bytes)) return false;

    // 逐扇区解密
    uint8_t* out_p = (uint8_t*)buf;
    for(uint32_t i = 0; i < sector_count; i++){
        aes_xts_decrypt(&xts_, sector_num + i,
                        cipher.data() + i * ss, out_p + i * ss, ss);
    }
    return true;
}

bool Volume::write_sectors_batch(uint64_t sector_num, uint32_t sector_count, const void* buf) {
    if(!is_unlocked()) return false;
    if(sector_count == 0) return true;
    if(sector_num + sector_count > header_.data_sectors) return false;
    uint32_t ss = header_.sector_size;
    uint64_t header_skip = is_v2_tail() ? 0 : (uint64_t)ss;
    uint64_t phys_offset = partition_offset_ + header_skip + (uint64_t)sector_num * ss;
    uint32_t total_bytes = sector_count * ss;

    // 逐扇区加密到连续缓冲区
    std::vector<uint8_t> cipher(total_bytes);
    const uint8_t* in_p = (const uint8_t*)buf;
    for(uint32_t i = 0; i < sector_count; i++){
        aes_xts_encrypt(&xts_, sector_num + i,
                        in_p + i * ss, cipher.data() + i * ss, ss);
    }

    // 一次性写入
    return disk_.write(phys_offset, cipher.data(), total_bytes);
}

// ============================================================
//  NTFS 直通读取（免格式化迁移专用）
//  lba 是分区内逻辑扇区号（0 = Boot Sector）
//  对 V2（tail header）卷，无头部跳过，lba 直接映射物理扇区
//  对 V1 卷理论上不支持 inplace，此处为安全兜底
// ============================================================
bool Volume::read_sectors(uint64_t lba, void* buf, uint32_t sector_count) {
    if(!is_unlocked()) return false;
    if(sector_count == 0) return true;
    uint32_t ss = header_.sector_size;

    // V2 tail header 卷：LBA 0 = 分区物理扇区 0（无头部跳过）
    // 数据区从 LBA 0 开始覆盖整个分区
    uint64_t phys_offset = partition_offset_ + (uint64_t)lba * ss;
    uint32_t total_bytes = sector_count * ss;

    std::vector<uint8_t> cipher(total_bytes);
    if(!disk_.read(phys_offset, cipher.data(), total_bytes)) return false;

    // 逐扇区解密（使用 lba 作为 XTS tweak）
    uint8_t* out_p = (uint8_t*)buf;
    for(uint32_t i = 0; i < sector_count; i++){
        aes_xts_decrypt(&xts_, lba + i,
                        cipher.data() + i * ss, out_p + i * ss, ss);
    }
    return true;
}

// ============================================================
//  数据擦除（零填充整个加密分区）
//  包括卷头和数据区，不可恢复
// ============================================================
bool Volume::wipe() {
    if(!disk_.is_open()) return false;
    uint32_t ss = header_.sector_size;
    uint64_t total_sectors = header_.data_sectors + 1; // +1 for header sector
    uint64_t total_bytes = total_sectors * ss;

    // 分批零填充（每批 1MB）
    static constexpr size_t WIPE_BATCH = 1 << 20; // 1MB
    std::vector<uint8_t> zeros(std::min((size_t)total_bytes, WIPE_BATCH), 0);

    uint64_t written = 0;
    while(written < total_bytes) {
        size_t chunk = std::min(zeros.size(), (size_t)(total_bytes - written));
        if(!disk_.write(partition_offset_ + written, zeros.data(), chunk)) {
            fprintf(stderr, "[WIPE] 写入失败 offset=%llu\n",
                    (unsigned long long)(partition_offset_ + written));
            fflush(stderr);
            // 继续尝试擦除剩余部分
        }
        written += chunk;
        // 每 100MB 打印一次进度
        if(written % (100 << 20) == 0) {
            fprintf(stderr, "[WIPE] 进度 %llu/%llu MB\n",
                    (unsigned long long)(written >> 20),
                    (unsigned long long)(total_bytes >> 20));
            fflush(stderr);
        }
    }

    // 标记卷头 wiped_flag
    header_.reserved[36] = 1;
    // 将计数器也写入，防止下次打开时重复擦除
    write_fail_count_to_header(header_, (uint32_t)WIPE_MAX_ATTEMPTS);
    disk_.write(partition_offset_, &header_, sizeof(header_));

    state_ = VolumeState::Wiped;
    fprintf(stderr, "[WIPE] 擦除完成，共 %llu MB\n",
            (unsigned long long)(total_bytes >> 20));
    fflush(stderr);
    return true;
}

// ============================================================
//  V2: 免格式化加密 — 就地加密已有数据
//  头部放在分区末尾，从 Sector 0 开始逐批加密
// ============================================================
uint64_t Volume::total_partition_sectors() const {
    uint64_t total = disk_.device_size();
    if(partition_offset_ > 0 && partition_offset_ < total) total -= partition_offset_;
    return total / header_.sector_size;
}

bool Volume::create_inplace(const std::string& device_path,
                            uint64_t           offset,
                            uint64_t           partition_size,
                            const std::string& primary_pass,
                            const std::string& emerg_pass,
                            std::function<void(float)> progress_cb,
                            const std::string& partition_device_path)
{
    // 免格式化加密功能已删除，此函数保留仅用于兼容性
    // 直接返回 false，不执行任何操作
    fprintf(stderr, "[CREATE_INPLACE] 免格式化加密功能已删除\n"); fflush(stderr);
    (void)device_path; (void)offset; (void)partition_size;
    (void)primary_pass; (void)emerg_pass; (void)progress_cb;
    (void)partition_device_path;
    return false;
}

// 免格式化加密功能已删除，旧代码不再使用
