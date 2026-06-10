#include "app.h"
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <cstring>
#include <cstdio>
#include <thread>
#include <atomic>
#include <chrono>
#include <exception>
#include <string>
#include <vector>
#include <algorithm>
#include <memory>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif
#include "../crypto/aes_xts.h"
#include "../disk/disk.h"
#include "../platform/platform_utils.h"

// ntfs_reader.h 只在 Windows 下使用（已删除免格式化加密功能）
#ifdef _WIN32
#include "../ntfs/ntfs_reader.h"
#endif

// ============================================================
//  初始化向导界面（draw_init_wizard）
//  在 App 类中单独提取为一个大函数
// ============================================================

// 向导分 4 步
enum WizardStep { WZ_WARN=0, WZ_PASSWORD=1, WZ_PROGRESS=2, WZ_DONE=3 };
static WizardStep wz_step  = WZ_WARN;
static char wz_primary[256]  = {};
static char wz_primary2[256] = {};
static char wz_emerg[256]    = {};
static char wz_emerg2[256]   = {};
static char wz_error[256]    = {};
static bool wz_running       = false;
static bool wz_success       = false;
static std::atomic<bool> wz_thread_done;
static std::unique_ptr<std::thread> wz_thread;
static std::string       wz_progress_msg; // 进度说明

// SEH/异常包装函数所需的 POD 参数
struct InitParams {
    Volume*  volume;
    Vfs*     vfs;
    const char* device_path;
    uint64_t partition_offset;
    const char* primary_pw;
    const char* emerg_pw;
    // 便携模式额外参数
    bool     portable_mode;
    bool     wizard_existing; // true=免格式化加密（已删除功能，保留兼容）
    uint64_t partition_size;  // 加密分区的精确字节大小（0=自动）
    const char* partition_device_path; // 分区设备路径（免格式化加密时用于数据 I/O）
    // 三平台可执行文件源路径（程序启动时自动发现）
    char     win_src[512];   // Windows exe 路径
    char     mac_src[512];   // macOS .app 路径
    char     linux_src[512]; // Linux binary 路径
};
static InitParams g_init_params; // 线程启动前赋值

static bool aes256_known_answer_test() {
    // NIST AES-256 测试向量
    // Key: 00010203 04050607 08090A0B 0C0D0E0F 10111213 14151617 18191A1B 1C1D1E1F
    // PT:  00112233 44556677 8899AABB CCDDEEFF
    // CT:  8EA2B7CA 516745BF EAFC4990 4B496089
    const uint8_t nist_key[32] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
        0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x17,
        0x18,0x19,0x1A,0x1B,0x1C,0x1D,0x1E,0x1F
    };
    const uint8_t nist_pt[16]  = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
        0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF
    };
    const uint8_t nist_ct[16]  = {
        0x8E,0xA2,0xB7,0xCA,0x51,0x67,0x45,0xBF,
        0xEA,0xFC,0x49,0x90,0x4B,0x49,0x60,0x89
    };

    Aes256Ctx ctx;
    aes256_init(&ctx, nist_key);
    uint8_t enc_out[16], dec_out[16];
    aes256_encrypt_block(&ctx, nist_pt, enc_out);
    aes256_decrypt_block(&ctx, nist_ct, dec_out);

    bool enc_ok = (memcmp(enc_out, nist_ct, 16) == 0);
    bool dec_ok = (memcmp(dec_out, nist_pt, 16) == 0);

    fprintf(stderr, "[AES-256 KAT] use_aesni=%d  encrypt=%s  decrypt=%s\n",
            (int)ctx.use_aesni, enc_ok?"OK":"FAIL", dec_ok?"OK":"FAIL");
    if(!enc_ok) {
        fprintf(stderr, "[AES-256 KAT] encrypt expected: ");
        for(int i=0;i<16;i++) fprintf(stderr, "%02X", nist_ct[i]);
        fprintf(stderr, "\n[AES-256 KAT] encrypt actual:   ");
        for(int i=0;i<16;i++) fprintf(stderr, "%02X", enc_out[i]);
        fprintf(stderr, "\n");
    }
    if(!dec_ok) {
        fprintf(stderr, "[AES-256 KAT] decrypt expected: ");
        for(int i=0;i<16;i++) fprintf(stderr, "%02X", nist_pt[i]);
        fprintf(stderr, "\n[AES-256 KAT] decrypt actual:   ");
        for(int i=0;i<16;i++) fprintf(stderr, "%02X", dec_out[i]);
        fprintf(stderr, "\n");
    }
    fflush(stderr);

    // 也测试软件路径（强制禁用 AES-NI）
    bool sw_enc_ok = false, sw_dec_ok = false;
    {
        Aes256Ctx sw_ctx;
        aes256_init(&sw_ctx, nist_key);
#if USE_AES_NI
        sw_ctx.use_aesni = false;  // 强制走软件路径
#endif
        uint8_t sw_enc[16], sw_dec[16];
        aes256_encrypt_block(&sw_ctx, nist_pt, sw_enc);
        aes256_decrypt_block(&sw_ctx, nist_ct, sw_dec);
        sw_enc_ok = (memcmp(sw_enc, nist_ct, 16) == 0);
        sw_dec_ok = (memcmp(sw_dec, nist_pt, 16) == 0);
        fprintf(stderr, "[AES-256 KAT] software path  encrypt=%s  decrypt=%s\n",
                sw_enc_ok?"OK":"FAIL", sw_dec_ok?"OK":"FAIL");
        if(!sw_enc_ok) {
            fprintf(stderr, "[AES-256 KAT] sw_encrypt actual: ");
            for(int i=0;i<16;i++) fprintf(stderr, "%02X", sw_enc[i]);
            fprintf(stderr, "\n");
        }
        if(!sw_dec_ok) {
            fprintf(stderr, "[AES-256 KAT] sw_decrypt actual: ");
            for(int i=0;i<16;i++) fprintf(stderr, "%02X", sw_dec[i]);
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }

    return enc_ok && dec_ok && sw_enc_ok && sw_dec_ok;
}

// ---- AES-XTS 自检 ----
static bool aes_xts_selftest() {
    fprintf(stderr, "[AES-XTS] selftest 开始\n"); fflush(stderr);

    // 先跑 AES-256 单块已知答案测试
    if(!aes256_known_answer_test()) {
        fprintf(stderr, "[AES-XTS] AES-256 KAT 失败，跳过 XTS 测试\n"); fflush(stderr);
        return false;
    }

    uint8_t key[64];
    for(int i=0;i<64;i++) key[i] = (uint8_t)(i * 0x37);

    AesXtsCtx ctx;
    fprintf(stderr, "[AES-XTS] 调用 aes_xts_init...\n"); fflush(stderr);
    aes_xts_init(&ctx, key);
    fprintf(stderr, "[AES-XTS] aes_xts_init 完成\n"); fflush(stderr);

    // 测试数据：512 字节（一个扇区）
    uint8_t plain[512], cipher[512], recovered[512];
    for(int i=0;i<512;i++) plain[i] = (uint8_t)(i ^ 0xA5);

    fprintf(stderr, "[AES-XTS] 调用 encrypt...\n"); fflush(stderr);
    aes_xts_encrypt(&ctx, 42, plain, cipher, 512);
    fprintf(stderr, "[AES-XTS] 调用 decrypt...\n"); fflush(stderr);
    aes_xts_decrypt(&ctx, 42, cipher, recovered, 512);
    fprintf(stderr, "[AES-XTS] decrypt 完成\n"); fflush(stderr);
    aes_xts_clear(&ctx);

    if(memcmp(plain, recovered, 512) != 0) {
        fprintf(stderr, "[AES-XTS] *** 自检失败! 加密→解密不一致 ***\n");
        for(int i=0;i<32;i++){
            fprintf(stderr, "  [%3d] plain=%02X cipher=%02X recovered=%02X\n",
                    i, plain[i], cipher[i], recovered[i]);
        }
        fflush(stderr);
        return false;
    }

    // 额外测试：不同扇区号产生不同密文
    uint8_t cipher2[512];
    AesXtsCtx ctx2;
    aes_xts_init(&ctx2, key);
    aes_xts_encrypt(&ctx2, 43, plain, cipher2, 512);
    aes_xts_clear(&ctx2);
    if(memcmp(cipher, cipher2, 512) == 0) {
        fprintf(stderr, "[AES-XTS] *** 自检失败! 不同扇区号产生相同密文 ***\n");
        fflush(stderr);
        return false;
    }

    fprintf(stderr, "[AES-XTS] 自检通过 (sector 42 + 43)\n");
    fflush(stderr);
    return true;
}

// ---- 磁盘读写往返测试 ----
static bool disk_roundtrip_test(Volume* vol) {
    // 测试扇区 0（数据区的第一个扇区）
    uint8_t test_pat[512];
    for(int i=0;i<512;i++) test_pat[i] = (uint8_t)((i * 7 + 13) & 0xFF);

    if(!vol->write_sector(0, test_pat)) {
        fprintf(stderr, "[DISK-TEST] 写入扇区 0 失败!\n"); fflush(stderr);
        return false;
    }

    uint8_t readback[512];
    if(!vol->read_sector(0, readback)) {
        fprintf(stderr, "[DISK-TEST] 读取扇区 0 失败!\n"); fflush(stderr);
        return false;
    }

    if(memcmp(test_pat, readback, 512) != 0) {
        fprintf(stderr, "[DISK-TEST] *** 往返测试失败! 写入与读回不一致 ***\n");
        fprintf(stderr, "[DISK-TEST] 前 32 字节对比:\n  写入:   ");
        for(int i=0;i<32;i++) fprintf(stderr, "%02X ", test_pat[i]);
        fprintf(stderr, "\n  读回:   ");
        for(int i=0;i<32;i++) fprintf(stderr, "%02X ", readback[i]);
        fprintf(stderr, "\n");
        fflush(stderr);
        return false;
    }

    // 恢复扇区 0 为全零
    uint8_t zero[512] = {};
    vol->write_sector(0, zero);

    fprintf(stderr, "[DISK-TEST] 磁盘读写往返测试通过\n");
    fflush(stderr);
    return true;
}

// 实际初始化逻辑（单独函数，可被异常处理包装函数通过函数指针调用）
static bool do_init_volume() {
    uint64_t t0 = get_timestamp_ms();
    fprintf(stderr, "[SecureDrive] 开始初始化...\n"); fflush(stderr);

    // ---- 互斥检查：便携模式与免格式化加密不能同时启用 ----
    if (g_init_params.portable_mode && g_init_params.wizard_existing) {
        snprintf(wz_error, sizeof(wz_error),
                 "便携模式与免格式化加密不能同时使用。"
                 "便携模式会重新分区（删除所有数据），免格式化加密需要保留原有分区。"
                 "请返回重新选择。");
        fprintf(stderr, "[ERROR] portable_mode && wizard_existing 互斥，拒绝执行\n"); fflush(stderr);
        return false;
    }

    Volume* vol = g_init_params.volume;
    Vfs*    vfs = g_init_params.vfs;

    uint64_t use_offset = g_init_params.partition_offset;

    // ---- 便携模式：先建双分区布局 ----
    if (g_init_params.portable_mode) {
        wz_progress_msg = "正在创建双分区布局（明文引导区 + 加密数据区）…";
        fprintf(stderr, "[PORTABLE] 开始创建便携双分区布局...\n"); fflush(stderr);

        PortableLayoutResult lay = create_portable_layout(g_init_params.device_path, 100);
        if (!lay.ok) {
            snprintf(wz_error, sizeof(wz_error), "分区布局失败: %s", lay.error_msg.c_str());
            fprintf(stderr, "[PORTABLE] 失败: %s\n", lay.error_msg.c_str()); fflush(stderr);
            return false;
        }

        // 等待磁盘驱动重新枚举（最多30秒）
        {
            RawDisk chk;
            bool drv_ready = false;
            for (int retry = 0; retry < 60; ++retry) {
                sleep_ms(500);
                if (chk.open(g_init_params.device_path, /*write=*/false)) {
                    uint32_t chk_ss = chk.sector_size();
                    chk.close();
                    if (chk_ss > 0) {
                        fprintf(stderr, "[PORTABLE] 磁盘驱动已就绪 sector_size=%u (等待 %.1fs)\n",
                                chk_ss, retry * 0.5f);
                        fflush(stderr);
                        drv_ready = true;
                        break;
                    }
                }
                fprintf(stderr, "[PORTABLE] 等待驱动... retry=%d\n", retry); fflush(stderr);
            }
            if (!drv_ready) {
                fprintf(stderr, "[PORTABLE] 磁盘驱动等待超时（30s），继续尝试\n"); fflush(stderr);
            }
        }

        // 确定明文分区的设备路径
        std::string boot_device;
#ifdef _WIN32
        int disk_num = extract_disk_number(g_init_params.device_path);
        char drive_letter = wait_for_drive_letter(disk_num, lay.boot_offset, 10000);
        if (drive_letter != '\0') {
            boot_device = std::string(1, drive_letter) + ":";
        }
#else
        // macOS/Linux: 根据分区布局推断分区设备路径
        std::string disk = g_init_params.device_path;
#ifdef __APPLE__
        // macOS: /dev/diskN → /dev/diskNs1
        boot_device = disk + "s1";
#else
        // Linux: /dev/sdX → /dev/sdX1
        boot_device = disk + "1";
#endif
#endif

        if (!boot_device.empty()) {
            wz_progress_msg = "格式化明文分区为 FAT32…";
            fprintf(stderr, "[PORTABLE] 格式化 %s\n", boot_device.c_str()); fflush(stderr);
            format_volume_fat32(boot_device, "SDRV_BOOT");

            // 等待分区挂载
            wz_progress_msg = "等待明文分区挂载…";
            std::string mount_point = wait_for_mount_point(boot_device, 10000);
            if (mount_point.empty()) {
                // 某些情况下格式化后不会自动挂载，尝试手动获取挂载点
                mount_point = get_mount_point(boot_device);
            }

            if (!mount_point.empty()) {
                fprintf(stderr, "[PORTABLE] 明文分区挂载点: %s\n", mount_point.c_str()); fflush(stderr);

                // 复制 Windows exe
                if (g_init_params.win_src[0] != '\0') {
                    std::string dest = mount_point + "/SecureDrive.exe";
                    wz_progress_msg = "复制 Windows 程序到明文分区…";
                    fprintf(stderr, "[PORTABLE] 复制 win: %s -> %s\n",
                            g_init_params.win_src, dest.c_str()); fflush(stderr);
                    copy_file_or_dir(g_init_params.win_src, dest);
                }

                // 复制 macOS .app
                if (g_init_params.mac_src[0] != '\0') {
                    std::string dest = mount_point + "/SecureDrive.app";
                    wz_progress_msg = "复制 macOS 程序到明文分区…";
                    fprintf(stderr, "[PORTABLE] 复制 mac: %s -> %s\n",
                            g_init_params.mac_src, dest.c_str()); fflush(stderr);
                    copy_file_or_dir(g_init_params.mac_src, dest);
                }

                // 复制 Linux binary
                if (g_init_params.linux_src[0] != '\0') {
                    std::string dest = mount_point + "/SecureDrive";
                    wz_progress_msg = "复制 Linux 程序到明文分区…";
                    fprintf(stderr, "[PORTABLE] 复制 linux: %s -> %s\n",
                            g_init_params.linux_src, dest.c_str()); fflush(stderr);
                    copy_file_or_dir(g_init_params.linux_src, dest);
                }
            } else {
                fprintf(stderr, "[PORTABLE] 警告：明文分区未挂载，跳过文件复制\n"); fflush(stderr);
            }
        } else {
            fprintf(stderr, "[PORTABLE] 警告：明文分区设备路径为空，跳过格式化和复制\n");
            fflush(stderr);
        }

        // 更新偏移：加密分区 = 分区2
        use_offset = lay.crypto_offset;
        g_init_params.partition_size = lay.crypto_size;
        fprintf(stderr, "[PORTABLE] crypto_offset=%llu  crypto_size=%llu\n",
                (unsigned long long)lay.crypto_offset,
                (unsigned long long)lay.crypto_size); fflush(stderr);
        wz_progress_msg = "初始化加密卷…";
        fprintf(stderr, "[PORTABLE] 初始化加密分区 offset=%llu\n",
                (unsigned long long)use_offset); fflush(stderr);
    }

    // ---- 免格式化加密模式：就地加密，不格式化 VFS ----
    bool inplace_mode = false;
    if (g_init_params.wizard_existing) {
        inplace_mode = true;
    }

    if (inplace_mode) {
        wz_progress_msg = "免格式化加密：正在加密数据（请勿断电）…";
        fprintf(stderr, "[INPLACE] 开始就地加密...\n"); fflush(stderr);
        uint64_t t1 = get_timestamp_ms();
        bool ok = vol->create_inplace(
            g_init_params.device_path,
            use_offset,
            g_init_params.partition_size,   // 传入精确分区大小
            g_init_params.primary_pw,
            g_init_params.emerg_pw,
            [](float progress) {
                wz_progress_msg = "免格式化加密中... " + std::to_string((int)(progress * 100)) + "%";
            },
            g_init_params.partition_device_path ? g_init_params.partition_device_path : ""
        );
        uint64_t t2 = get_timestamp_ms();
        fprintf(stderr, "[INPLACE] 就地加密 %s (%.1f ms)\n",
                ok ? "成功" : "失败", (double)(t2 - t1));
        fflush(stderr);
        if (!ok) return false;

        // ---- 格式化 VFS ----
        wz_progress_msg = "正在格式化 VFS…";
        fprintf(stderr, "[INPLACE] 格式化 VFS...\n"); fflush(stderr);

        ok = vfs->format(vol);
        if (!ok) {
            fprintf(stderr, "[INPLACE] VFS format 失败\n"); fflush(stderr);
            vol->lock();
            return false;
        }

        if (!vfs->mount(vol)) {
            fprintf(stderr, "[INPLACE] VFS mount 失败\n"); fflush(stderr);
            vol->lock();
            return false;
        }

        uint64_t t3 = get_timestamp_ms();
        fprintf(stderr, "[INPLACE] 总耗时 %.1f ms\n", (double)(t3 - t1));
        fflush(stderr);

        vol->lock();
        return true;
    }

    bool ok = vol->create(g_init_params.device_path,
                           use_offset,
                           g_init_params.primary_pw,
                           g_init_params.emerg_pw);
    uint64_t t1 = get_timestamp_ms();
    fprintf(stderr, "[SecureDrive] Volume::create %s (%.1f ms)\n",
            ok ? "成功" : "失败", (double)(t1 - t0));
    fflush(stderr);

    // AES-XTS 自检
    if(ok) {
        wz_progress_msg = "AES-XTS 自检…";
        ok = aes_xts_selftest();
    }

    // 磁盘读写往返测试
    if(ok) {
        wz_progress_msg = "磁盘读写测试…";
        ok = disk_roundtrip_test(vol);
    }

    if(ok) {
        wz_progress_msg = "格式化 VFS…";
        ok = vfs->format(vol);
    }
    uint64_t t2 = get_timestamp_ms();
    fprintf(stderr, "[SecureDrive] Vfs::format %s (%.1f ms)\n",
            ok ? "成功" : "失败", (double)(t2 - t1));
    fprintf(stderr, "[SecureDrive] 总耗时 %.1f ms\n", (double)(t2 - t0));
    fflush(stderr);

    return ok;
}

// 异常处理包装：捕获致命异常（通过函数指针调用，避免内联）
#ifdef _WIN32
// Windows: 使用 SEH
typedef bool (*InitFunc)();
static DWORD WINAPI exception_wrapper(LPVOID) {
    InitFunc fn = do_init_volume;
    __try {
        wz_success = fn();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        DWORD code = GetExceptionCode();
        fprintf(stderr, "[SecureDrive] *** 崩溃! 异常代码: 0x%08X ***\n", code);
        fflush(stderr);
        wz_success = false;
        snprintf(wz_error, sizeof(wz_error),
                 "初始化崩溃 (异常 0x%08X)", code);
    }
    wz_thread_done.store(true, std::memory_order_release);
    return 0;
}
#else
// macOS/Linux: 使用标准 C++ 异常
typedef bool (*InitFunc)();
static void* exception_wrapper(void*) {
    InitFunc fn = do_init_volume;
    try {
        wz_success = fn();
    } catch(const std::exception& e) {
        fprintf(stderr, "[SecureDrive] *** 崩溃! 异常: %s ***\n", e.what());
        fflush(stderr);
        wz_success = false;
        snprintf(wz_error, sizeof(wz_error),
                 "初始化崩溃 (%s)", e.what());
    } catch(...) {
        fprintf(stderr, "[SecureDrive] *** 崩溃! 未知异常 ***\n");
        fflush(stderr);
        wz_success = false;
        snprintf(wz_error, sizeof(wz_error), "初始化崩溃 (未知异常)");
    }
    wz_thread_done.store(true, std::memory_order_release);
    return nullptr;
}
#endif

void App::draw_init_wizard() {
    float cx = ImGui::GetWindowWidth() * 0.5f;

    ImGui::SetCursorPosY(ImGui::GetContentRegionAvail().y * 0.05f);

    // 进度提示
    const char* steps[] = {"确认", "设置密码", "初始化中…", "完成"};
    for(int i=0;i<4;i++){
        if(i>0) ImGui::SameLine();
        ImGui::SetCursorPosX(cx - 200 + i*130.f);
        if(i==(int)wz_step)
            ImGui::TextColored(ImVec4(0.3f,0.8f,1,1), "[%s]", steps[i]);
        else if(i<(int)wz_step)
            ImGui::TextColored(ImVec4(0.5f,0.8f,0.5f,1), "[OK] %s", steps[i]);
        else
            ImGui::TextDisabled("[-] %s", steps[i]);
    }
    ImGui::Separator();

    // ---- 步骤 0：警告 + 便携模式选项 ----
    if(wz_step == WZ_WARN){
        ImGui::Spacing();
        ImGui::SetCursorPosX(cx - 250);

        if (ctx_.wizard_existing) {
            // 免格式化加密提示
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f,0.8f,1,1));
            ImGui::TextWrapped("免格式化加密模式\n");
            ImGui::PopStyleColor();
            ImGui::SetCursorPosX(cx - 250);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,0.3f,0.3f,1));
            ImGui::TextWrapped(
                "此模式将就地加密分区上的所有数据。\n"
                "加密后原有文件将不可直接访问，VFS 会初始化为空白。\n"
                "请确保已提前备份重要文件！\n\n"
                "注意：加密过程中请勿断电或拔出磁盘！\n\n"
                "目标分区：%s", ctx_.device_path.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,0.5f,0.2f,1));
            ImGui::TextWrapped("警告：初始化操作将格式化选中的分区\n"
                               "    分区上的所有现有数据将被永久删除！\n\n"
                               "    请确认目标设备：%s",
                               ctx_.device_path.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ---- 便携模式选项 ----
        ImGui::SetCursorPosX(cx - 250);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f,0.9f,1.0f,1));
        ImGui::TextWrapped("[便携] 便携模式（推荐用于移动磁盘）");
        ImGui::PopStyleColor();
        ImGui::SetCursorPosX(cx - 250);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f,0.75f,0.75f,1));
        ImGui::TextWrapped(
            "启用后，磁盘将被分为两个区：\n"
            "  - 分区1（100MB，FAT32，明文）：存放 SecureDrive 程序\n"
            "    -> Windows: 双击 SecureDrive.exe 运行\n"
            "    -> macOS: 双击 SecureDrive.app 运行\n"
            "    -> Linux: 双击 SecureDrive 运行\n"
            "  - 分区2（剩余空间，加密）：存放您的加密文件\n"
            "    -> 无软件的电脑上先运行明文分区的程序，再解密使用\n"
        );
        ImGui::PopStyleColor();
        ImGui::SetCursorPosX(cx - 250);

        // 免格式化加密与便携模式互斥
        if (ctx_.wizard_existing) {
            ImGui::BeginDisabled();
            bool dummy = false;
            ImGui::Checkbox("启用便携模式（推荐移动磁盘使用）", &dummy);
            ImGui::EndDisabled();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,0.8f,0.3f,1));
            ImGui::TextWrapped("[!] 免格式化加密模式下不可启用便携模式（将保留当前分区结构）");
            ImGui::PopStyleColor();
            ctx_.wizard_portable = false; // 强制关闭
        } else {
            ImGui::Checkbox("启用便携模式（推荐移动磁盘使用）", &ctx_.wizard_portable);
        }

        // 三平台可执行文件发现状态
        if (ctx_.wizard_portable) {
            ImGui::Spacing();
            ImGui::SetCursorPosX(cx - 250);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f,0.8f,0.6f,1));
            ImGui::TextWrapped("将自动复制以下程序到明文分区：");
            ImGui::PopStyleColor();
            ImGui::SetCursorPosX(cx - 250);
            if(!ctx_.portable_win_src.empty())
                ImGui::Text("[OK] Windows: SecureDrive.exe");
            else
                ImGui::TextDisabled("[未找到] Windows: SecureDrive.exe");
            ImGui::SetCursorPosX(cx - 250);
            if(!ctx_.portable_mac_src.empty())
                ImGui::Text("[OK] macOS: SecureDrive.app");
            else
                ImGui::TextDisabled("[未找到] macOS: SecureDrive.app");
            ImGui::SetCursorPosX(cx - 250);
            if(!ctx_.portable_linux_src.empty())
                ImGui::Text("[OK] Linux: SecureDrive");
            else
                ImGui::TextDisabled("[未找到] Linux: SecureDrive");
            ImGui::SetCursorPosX(cx - 250);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f,0.8f,0.4f,1));
            ImGui::TextWrapped("提示：将其他平台的可执行文件放在本程序同目录下，即可自动打包。");
            ImGui::PopStyleColor();
        }

        ImGui::Spacing();
        ImGui::SetCursorPosX(cx - 80);
        if(ImGui::Button("我已了解风险，继续", ImVec2(220, 30))){
            wz_step = WZ_PASSWORD;
        }
        ImGui::SameLine();
        if(ImGui::Button("取消")) {
            ctx_.wizard_portable = false;
            ctx_.current_screen = Screen::DeviceSelect;
        }
    }

    // ---- 步骤 1：设置密码 ----
    else if(wz_step == WZ_PASSWORD){
        ImGui::SetCursorPosX(cx - 180);
        ImGui::BeginGroup();
        ImGui::TextColored(ImVec4(0.3f,0.8f,1,1), "主密码：");
        ImGui::SetNextItemWidth(320);
        ImGui::InputText("##p1", wz_primary, sizeof(wz_primary),
                          ImGuiInputTextFlags_Password);
        ImGui::Text("确认主密码：");
        ImGui::SetNextItemWidth(320);
        ImGui::InputText("##p2", wz_primary2, sizeof(wz_primary2),
                          ImGuiInputTextFlags_Password);
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1,0.9f,0.3f,1), "紧急密码（用于忘记主密码时）：");
        ImGui::SetNextItemWidth(320);
        ImGui::InputText("##e1", wz_emerg, sizeof(wz_emerg),
                          ImGuiInputTextFlags_Password);
        ImGui::Text("确认紧急密码：");
        ImGui::SetNextItemWidth(320);
        ImGui::InputText("##e2", wz_emerg2, sizeof(wz_emerg2),
                          ImGuiInputTextFlags_Password);
        if(strlen(wz_error)>0)
            ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "[X] %s", wz_error);
        ImGui::Spacing();
        if(ImGui::Button("开始初始化", ImVec2(200,30))){
            // 验证
            if(strlen(wz_primary) < 8){
                strcpy(wz_error, "主密码至少 8 位");
            } else if(strcmp(wz_primary, wz_primary2) != 0){
                strcpy(wz_error, "两次主密码不一致");
            } else if(strlen(wz_emerg) < 8){
                strcpy(wz_error, "紧急密码至少 8 位");
            } else if(strcmp(wz_emerg, wz_emerg2) != 0){
                strcpy(wz_error, "两次紧急密码不一致");
            } else if(strcmp(wz_primary, wz_emerg) == 0){
                strcpy(wz_error, "主密码和紧急密码不能相同");
            } else {
                wz_error[0] = '\0';


                // ---- 普通模式：直接进入进度 ----
                wz_step     = WZ_PROGRESS;
                wz_running  = true;
                wz_progress_msg = "准备中…";
                // 填充 SEH 参数（POD only）
                g_init_params.volume          = &ctx_.volume;
                g_init_params.vfs             = &ctx_.vfs;
                g_init_params.device_path     = ctx_.device_path.c_str();
                g_init_params.partition_offset= ctx_.partition_offset;
                g_init_params.primary_pw      = wz_primary;
                g_init_params.emerg_pw        = wz_emerg;
                g_init_params.portable_mode   = ctx_.wizard_portable;
                g_init_params.wizard_existing = ctx_.wizard_existing;
                g_init_params.partition_size  = ctx_.partition_size;
                g_init_params.partition_device_path = ctx_.partition_device_path.c_str();
                strncpy(g_init_params.win_src,
                        ctx_.portable_win_src.c_str(),
                        sizeof(g_init_params.win_src) - 1);
                g_init_params.win_src[sizeof(g_init_params.win_src)-1] = '\0';
                strncpy(g_init_params.mac_src,
                        ctx_.portable_mac_src.c_str(),
                        sizeof(g_init_params.mac_src) - 1);
                g_init_params.mac_src[sizeof(g_init_params.mac_src)-1] = '\0';
                strncpy(g_init_params.linux_src,
                        ctx_.portable_linux_src.c_str(),
                        sizeof(g_init_params.linux_src) - 1);
                g_init_params.linux_src[sizeof(g_init_params.linux_src)-1] = '\0';
                // 在后台线程执行初始化
                wz_thread = std::make_unique<std::thread>([](){
                    exception_wrapper(nullptr);
                });
            }
        }
        ImGui::SameLine();
        if(ImGui::Button("取消")) ctx_.current_screen = Screen::DeviceSelect;
        ImGui::EndGroup();
    }


    // ---- 步骤 3：进行中 ----
    else if(wz_step == WZ_PROGRESS){
        if(wz_running){
            wz_running = false; // 线程已在 WZ_PASSWORD 中启动
        }
        // 检测后台线程是否完成
        if(wz_thread_done.load(std::memory_order_acquire)){
            wz_thread_done.store(false, std::memory_order_relaxed);
            if(wz_thread && wz_thread->joinable()) wz_thread->join();
            wz_step = WZ_DONE;
        }
        ImGui::SetCursorPosX(cx - 150);
        ImGui::Text("正在初始化，请稍候…");
        // 实时进度说明
        ImGui::SetCursorPosX(cx - 200);
        ImGui::TextColored(ImVec4(0.7f,0.9f,1,1), "%s", wz_progress_msg.c_str());
        // 简单动画
        static float t=0; t+=ImGui::GetIO().DeltaTime;
        ImGui::SetCursorPosX(cx - 50);
        ImGui::TextColored(ImVec4(0.3f,0.8f,1,1),
                            "[%s]", (int)(t*2)%2==0 ? "######   " : "   ######");
    }

    // ---- 步骤 3：完成 ----
    else if(wz_step == WZ_DONE){
        ImGui::Spacing();
        ImGui::SetCursorPosX(cx - 100);
        if(wz_success){
            ImGui::TextColored(ImVec4(0.3f,1,0.3f,1), "[OK] 初始化成功！");
            ImGui::SetCursorPosX(cx - 100);
            ImGui::TextDisabled("主密码和紧急密码已安全存储");

            // 便携模式提示
            if (ctx_.wizard_portable) {
                ImGui::Spacing();
                ImGui::SetCursorPosX(cx - 250);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f,1,0.6f,1));
                ImGui::TextWrapped(
                    "[OK] 便携模式布局完成！\n\n"
                    "使用方法：\n"
                    "  1. 将移动磁盘插入任意电脑（Windows/macOS/Linux）\n"
                    "  2. 打开磁盘（分区1，FAT32，可见）\n"
                    "  3. 双击对应平台的 SecureDrive 程序运行\n"
                    "     -> Windows: SecureDrive.exe（右键管理员运行）\n"
                    "     -> macOS: SecureDrive.app\n"
                    "     -> Linux: SecureDrive\n"
                    "  4. 程序自动检测加密分区，输入密码解锁即可使用"
                );
                ImGui::PopStyleColor();
            }

            ImGui::Spacing();
            ImGui::SetCursorPosX(cx - 60);
            if(ImGui::Button("进入文件管理器", ImVec2(200,30))){
                ctx_.current_screen = Screen::FileManager;
                ctx_.current_dir    = "/";
                // 清除密码
                memset(wz_primary,  0, sizeof(wz_primary));
                memset(wz_primary2, 0, sizeof(wz_primary2));
                memset(wz_emerg,    0, sizeof(wz_emerg));
                memset(wz_emerg2,   0, sizeof(wz_emerg2));
                ctx_.wizard_portable = false;
                wz_step = WZ_WARN; // 重置向导状态
            }
        } else {
            ImGui::TextColored(ImVec4(1,0.3f,0.3f,1),
                               "✗  初始化失败");
            ImGui::SetCursorPosX(cx - 180);
            ImGui::TextWrapped(
                "可能原因：\n"
                "1. 未以管理员身份运行（最常见）\n"
                "2. 目标磁盘/分区正被系统占用（有盘符挂载）\n"
                "3. 磁盘已损坏或写保护\n"
                "4. 磁盘太小（便携模式需要 > 150MB）");
            if (strlen(wz_error) > 0) {
                ImGui::SetCursorPosX(cx - 180);
                ImGui::TextColored(ImVec4(1,0.4f,0.4f,1), "错误：%s", wz_error);
            }
            ImGui::Spacing();
            ImGui::SetCursorPosX(cx-60);
            if(ImGui::Button("返回")){
                wz_step = WZ_WARN;
                ctx_.wizard_portable = false;
                ctx_.current_screen = Screen::DeviceSelect;
            }
        }
    }
}
