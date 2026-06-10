#pragma once
#include <cstdint>
#include <cstring>

// ============================================================
//  SecureDrive 卷头格式（存储在加密分区的第 0 扇区，512 字节）
//
//  设计原则：
//  - Sector 0（本文件定义）：卷头，含盐值和加密后的主密钥槽
//  - Sector 1 .. N-1：数据区，用 AES-256-XTS 以扇区为单位加密
//
//  密钥派生流程：
//  1. Argon2id(password, salt) → KEK（64 字节密钥加密密钥）
//  2. 用 AES-256-CBC 解密卷头中的 EncryptedSlot
//  3. EncryptedSlot 解密后包含：MasterKey(64B) + MagicVerifier(16B)
//     若 MagicVerifier == SLOT_MAGIC，则密码正确，MasterKey 有效
//  4. 用 MasterKey 初始化 AES-256-XTS 引擎，逐扇区加解密数据
//
//  支持主密码 + 紧急密码两个独立密码槽，均可解锁相同的 MasterKey
// ============================================================

#pragma pack(push, 1)

static constexpr uint8_t  VOLUME_MAGIC[8]  = {'S','D','R','V','0','1','\0','\0'};
static constexpr uint32_t VOLUME_VERSION   = 1;
static constexpr uint32_t VOLUME_VERSION_V2 = 2; // 尾部头部版本（免格式化加密）
static constexpr uint8_t  SLOT_MAGIC[16]   = {
    0xDE,0xAD,0xBE,0xEF,0xCA,0xFE,0xBA,0xBE,
    0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF
};

// 每个密码槽占用的字节数
// EncryptedSlot = AES-256-CBC(KEK[0..31], IV, MasterKey[64B] + MagicVerifier[16B])
// 明文 = 64+16 = 80 字节；AES-CBC 输出 = 80 字节（恰好 5 个 AES 块）
static constexpr size_t SALT_SIZE        = 32;
static constexpr size_t MASTER_KEY_SIZE  = 64; // AES-256-XTS 需要 512 位
static constexpr size_t SLOT_PLAINTEXT   = MASTER_KEY_SIZE + 16; // 80 字节
static constexpr size_t SLOT_CIPHERTEXT  = 80; // AES-256-CBC 对齐后 80 字节
static constexpr size_t SLOT_IV_SIZE     = 16;
static constexpr size_t SLOT_MAC_SIZE    = 32; // HMAC-SHA-256

struct PasswordSlot {
    uint8_t salt[SALT_SIZE];                 // Argon2id 盐（随机生成）
    uint8_t iv  [SLOT_IV_SIZE];              // AES-CBC IV（随机生成）
    uint8_t ciphertext[SLOT_CIPHERTEXT];     // 加密后的 MasterKey + SLOT_MAGIC
    uint8_t mac[SLOT_MAC_SIZE];              // HMAC-SHA256(salt||iv||ciphertext, KEK[32..63])
    // 共 32+16+80+32 = 160 字节
};

struct VolumeHeader {
    uint8_t       magic[8];           //  8 字节: "SDRV01\0\0"
    uint32_t      version;            //  4 字节: = 1
    uint32_t      flags;              //  4 字节: 保留 = 0
    uint64_t      data_sectors;       //  8 字节: 数据区扇区数（不含卷头扇区）
    uint32_t      sector_size;        //  4 字节: 扇区大小（字节，通常 512 或 4096）
    // Argon2id 参数（初始化时写入，之后固定）
    uint32_t      argon2_t_cost;      //  4 字节
    uint32_t      argon2_m_cost;      //  4 字节: 单位 KB
    uint32_t      argon2_parallelism; //  4 字节
    // 密码槽
    PasswordSlot  primary_slot;       // 160 字节
    PasswordSlot  emergency_slot;     // 160 字节
    // 合计：8+4+4+8+4+4+4+4+160+160 = 360 字节
    uint8_t       reserved[152];      // 152 字节，补齐 512 字节
    // 总计 512 字节
};

// ============================================================
//  reserved[152] 布局（暴力破解防护）
//
//  [0..3]     uint32_t  fail_count       LE, 连续密码错误次数
//  [4..35]    uint8_t   fail_hmac[32]    HMAC-SHA256 保护 fail_count 完整性
//  [36]       uint8_t   wiped_flag       1 = 数据已被擦除
//  [37..151]  未使用
//
//  HMAC 密钥 = SHA256("SDRV01_FAIL_INTG_V1")，硬编码在二进制中
//  防止攻击者通过 hex editor 直接修改 fail_count
// ============================================================
static constexpr int     WIPE_MAX_ATTEMPTS = 10; // 连续错误达到此次数触发擦除

// ============================================================
//  V2: 免格式化加密（尾部隐藏头）
//
//  flags 字段标志位：
//  [0] HEADER_FLAG_TAIL = 0x01  → 头部在分区末尾
//  [1] HEADER_FLAG_INPLACE = 0x02 → 就地加密（保留原文件系统）
//
//  尾部头部布局：
//  分区末尾最后 TAIL_HEADER_SECTORS 个扇区存放 VolumeHeader + 加密状态
//  对于512字节扇区，每个扇区可存放1个 VolumeHeader
//
//  reserved[152] V2 扩展布局：
//  [0..3]      fail_count       (与 V1 相同)
//  [4..35]     fail_hmac[32]    (与 V1 相同)
//  [36]        wiped_flag       (与 V1 相同)
//  [37..38]    uint16_t encrypt_state   0=未开始, 1=加密中, 2=完成
//  [39..46]    uint64_t encrypted_up_to 已加密到此扇区号
//  [47..151]   备份尾部的原始数据（可扩展）
// ============================================================
static constexpr uint32_t HEADER_FLAG_TAIL     = 0x00000001;
static constexpr uint32_t HEADER_FLAG_INPLACE = 0x00000002;
static constexpr uint32_t TAIL_HEADER_SECTORS = 1; // 尾部头部占用扇区数（1个VolumeHeader）

static_assert(sizeof(PasswordSlot) == 160,  "PasswordSlot size mismatch");
static_assert(sizeof(VolumeHeader) == 512,  "VolumeHeader must be exactly 512 bytes");

#pragma pack(pop)
