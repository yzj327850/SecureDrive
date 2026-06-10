#include "disk.h"
#include <cstring>

// ============================================================
//  跨平台磁盘访问实现
// ============================================================

#ifdef _WIN32
//----------- Windows -----------
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <devguid.h>
#pragma comment(lib, "setupapi.lib")

bool RawDisk::open(const std::string& path, bool write_mode) {
    close();
    DWORD access = GENERIC_READ | (write_mode ? GENERIC_WRITE : 0);
    DWORD share  = FILE_SHARE_READ | FILE_SHARE_WRITE;
    handle_ = CreateFileA(path.c_str(), access, share, nullptr,
                          OPEN_EXISTING, 0, nullptr);
    if(handle_ == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        fprintf(stderr, "[RawDisk::open] CreateFile failed for %s, err=%lu, trying FILE_FLAG_NO_BUFFERING...\n",
                path.c_str(), (unsigned long)err);
        fflush(stderr);
        handle_ = CreateFileA(path.c_str(), access, share, nullptr,
                              OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);
        if(handle_ == INVALID_HANDLE_VALUE) {
            err = GetLastError();
            fprintf(stderr, "[RawDisk::open] CreateFile with NO_BUFFERING also failed, err=%lu\n",
                    (unsigned long)err);
            fflush(stderr);
            return false;
        }
        no_buffering_ = true;
        fprintf(stderr, "[RawDisk::open] Opened %s with FILE_FLAG_NO_BUFFERING\n", path.c_str());
        fflush(stderr);
    }

    // 获取磁盘几何参数
    DISK_GEOMETRY_EX geo{};
    DWORD ret = 0;
    if(DeviceIoControl(handle_, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                       nullptr, 0, &geo, sizeof(geo), &ret, nullptr)){
        sector_size_  = geo.Geometry.BytesPerSector;
        device_size_  = geo.DiskSize.QuadPart;
    } else {
        // 可能是分区，用 IOCTL_DISK_GET_LENGTH_INFO
        GET_LENGTH_INFORMATION gli{};
        if(DeviceIoControl(handle_, IOCTL_DISK_GET_LENGTH_INFO,
                           nullptr, 0, &gli, sizeof(gli), &ret, nullptr)){
            device_size_ = gli.Length.QuadPart;
        }
    }
    return true;
}

void RawDisk::close() {
    if(handle_ != INVALID_HANDLE_VALUE){
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    no_buffering_ = false;
}

bool RawDisk::lock_volume() {
#ifdef _WIN32
    if(!is_open()) return false;
    DWORD bytes_returned = 0;
    BOOL ok = DeviceIoControl(handle_, FSCTL_LOCK_VOLUME,
                              nullptr, 0, nullptr, 0,
                              &bytes_returned, nullptr);
    fprintf(stderr, "[RawDisk] FSCTL_LOCK_VOLUME ok=%d\n", (int)ok); fflush(stderr);
    return ok != 0;
#else
    return true; // Linux/macOS 不需要
#endif
}

bool RawDisk::dismount_volume() {
#ifdef _WIN32
    if(!is_open()) return false;
    DWORD bytes_returned = 0;
    BOOL ok = DeviceIoControl(handle_, FSCTL_DISMOUNT_VOLUME,
                              nullptr, 0, nullptr, 0,
                              &bytes_returned, nullptr);
    fprintf(stderr, "[RawDisk] FSCTL_DISMOUNT_VOLUME ok=%d\n", (int)ok); fflush(stderr);
    return ok != 0;
#else
    return true;
#endif
}

bool RawDisk::allow_extended_dasd_io() {
#ifdef _WIN32
    if(!is_open()) return false;
    DWORD bytes_returned = 0;
    BOOL ok = DeviceIoControl(handle_, FSCTL_ALLOW_EXTENDED_DASD_IO,
                              nullptr, 0, nullptr, 0,
                              &bytes_returned, nullptr);
    fprintf(stderr, "[RawDisk] FSCTL_ALLOW_EXTENDED_DASD_IO ok=%d\n", (int)ok); fflush(stderr);
    return ok != 0;
#else
    return true;
#endif
}

bool RawDisk::read(uint64_t offset, void* buf, size_t size) {
    if(!is_open()) {
        fprintf(stderr, "[RawDisk::read] FAIL: not open\n"); fflush(stderr);
        return false;
    }
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)offset;
    if(!SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) {
        DWORD err = GetLastError();
        fprintf(stderr, "[RawDisk::read] FAIL: SetFilePointerEx offset=%llu err=%lu\n",
                (unsigned long long)offset, (unsigned long)err); fflush(stderr);
        return false;
    }
    if (no_buffering_) {
        // FILE_FLAG_NO_BUFFERING 需要扇区对齐的缓冲区和大小
        size_t sec_mask = (size_t)sector_size_ - 1;
        size_t aligned_size = (size + sec_mask) & ~sec_mask;
        std::vector<uint8_t> tmp(aligned_size + sector_size_);
        uintptr_t align_mask = (uintptr_t)sector_size_ - 1;
        uint8_t* aligned_buf = (uint8_t*)(((uintptr_t)tmp.data() + align_mask) & ~align_mask);
        DWORD done = 0;
        BOOL ok = ReadFile(handle_, aligned_buf, (DWORD)aligned_size, &done, nullptr);
        if(!ok || done != aligned_size) {
            DWORD err = GetLastError();
            fprintf(stderr, "[RawDisk::read] FAIL: ReadFile offset=%llu size=%zu aligned_size=%zu done=%lu err=%lu\n",
                    (unsigned long long)offset, size, aligned_size, (unsigned long)done, (unsigned long)err); fflush(stderr);
            return false;
        }
        memcpy(buf, aligned_buf, size);
        return true;
    }
    DWORD done = 0;
    return ReadFile(handle_, buf, (DWORD)size, &done, nullptr) && (done == size);
}

bool RawDisk::write(uint64_t offset, const void* buf, size_t size) {
    if(!is_open()) {
        fprintf(stderr, "[RawDisk::write] FAIL: not open\n"); fflush(stderr);
        return false;
    }
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)offset;
    if(!SetFilePointerEx(handle_, li, nullptr, FILE_BEGIN)) {
        DWORD err = GetLastError();
        fprintf(stderr, "[RawDisk::write] FAIL: SetFilePointerEx offset=%llu err=%lu\n",
                (unsigned long long)offset, (unsigned long)err); fflush(stderr);
        return false;
    }
    if (no_buffering_) {
        // FILE_FLAG_NO_BUFFERING 需要扇区对齐的缓冲区和大小
        size_t sec_mask = (size_t)sector_size_ - 1;
        size_t aligned_size = (size + sec_mask) & ~sec_mask;
        std::vector<uint8_t> tmp(aligned_size + sector_size_);
        uintptr_t align_mask = (uintptr_t)sector_size_ - 1;
        uint8_t* aligned_buf = (uint8_t*)(((uintptr_t)tmp.data() + align_mask) & ~align_mask);
        memcpy(aligned_buf, buf, size);
        if (aligned_size > size) memset(aligned_buf + size, 0, aligned_size - size);
        DWORD done = 0;
        BOOL ok = WriteFile(handle_, aligned_buf, (DWORD)aligned_size, &done, nullptr);
        if(!ok || done != aligned_size) {
            DWORD err = GetLastError();
            fprintf(stderr, "[RawDisk::write] FAIL: WriteFile offset=%llu size=%zu aligned_size=%zu done=%lu err=%lu\n",
                    (unsigned long long)offset, size, aligned_size, (unsigned long)done, (unsigned long)err); fflush(stderr);
            return false;
        }
        return true;
    }
    DWORD done = 0;
    BOOL ok = WriteFile(handle_, buf, (DWORD)size, &done, nullptr);
    if(!ok || done != size) {
        DWORD err = GetLastError();
        fprintf(stderr, "[RawDisk::write] FAIL: WriteFile offset=%llu size=%zu done=%lu ok=%d err=%lu\n",
                (unsigned long long)offset, size, (unsigned long)done, (int)ok, (unsigned long)err); fflush(stderr);
        return false;
    }
    return true;
}

bool RawDisk::flush() {
    if(!is_open()) return false;
#ifdef _WIN32
    return FlushFileBuffers(handle_) != 0;
#else
    return ::fsync(handle_) == 0;
#endif
}

std::vector<DiskInfo> enum_disks() {
    std::vector<DiskInfo> result;
    for(int i=0;i<32;i++){
        std::string path = "\\\\.\\PhysicalDrive" + std::to_string(i);
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ|FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if(h == INVALID_HANDLE_VALUE) continue;

        DISK_GEOMETRY_EX geo{}; DWORD ret=0;
        DiskInfo di;
        di.device_path = path;
        di.display_name = "PhysicalDrive" + std::to_string(i);
        if(DeviceIoControl(h,IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                           nullptr,0,&geo,sizeof(geo),&ret,nullptr)){
            di.total_bytes  = geo.DiskSize.QuadPart;
            di.sector_size  = geo.Geometry.BytesPerSector;
        }
        // 检测是否可移动
        STORAGE_HOTPLUG_INFO hpi{}; ret=0;
        if(DeviceIoControl(h,IOCTL_STORAGE_GET_HOTPLUG_INFO,
                           nullptr,0,&hpi,sizeof(hpi),&ret,nullptr)){
            di.removable = hpi.MediaRemovable != 0;
        }
        CloseHandle(h);
        result.push_back(di);
    }
    return result;
}

std::vector<PartitionInfo> enum_partitions(const std::string& disk_path) {
    std::vector<PartitionInfo> result;
    HANDLE h = CreateFileA(disk_path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ|FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if(h == INVALID_HANDLE_VALUE) return result;

    // 提取磁盘编号
    int disk_num = -1;
    sscanf(disk_path.c_str(), "\\\\.\\PhysicalDrive%d", &disk_num);

    // 获取分区布局
    DWORD bufsz = sizeof(DRIVE_LAYOUT_INFORMATION_EX) + 127*sizeof(PARTITION_INFORMATION_EX);
    std::vector<uint8_t> buf(bufsz);
    DWORD ret=0;
    if(DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                       nullptr, 0, buf.data(), bufsz, &ret, nullptr)){
        auto* layout = (DRIVE_LAYOUT_INFORMATION_EX*)buf.data();
        for(DWORD i=0;i<layout->PartitionCount;i++){
            auto& pe = layout->PartitionEntry[i];
            if(pe.PartitionLength.QuadPart == 0) continue;
            PartitionInfo pi;
            pi.parent_disk  = disk_path;
            pi.offset_bytes = pe.StartingOffset.QuadPart;
            pi.size_bytes   = pe.PartitionLength.QuadPart;
            pi.index        = i;
            // 构造分区设备路径
            if (disk_num >= 0) {
                pi.device_path = "\\\\.\\Harddisk" + std::to_string(disk_num)
                               + "\\Partition" + std::to_string(i+1);
            } else {
                pi.device_path = disk_path;
            }
            pi.label = "Partition" + std::to_string(i+1);
            result.push_back(pi);
        }
    }
    CloseHandle(h);
    return result;
}

// ============================================================
//  卸载磁盘上所有已挂载的卷（为原始写操作腾出锁）
// ============================================================
int extract_disk_number(const std::string& device_path) {
    int n = -1;
    sscanf(device_path.c_str(), "\\\\.\\PhysicalDrive%d", &n);
    return n;
}

bool dismount_volumes_on_disk(int disk_num) {
    if (disk_num < 0) return false;
    bool all_ok = true;
    // 枚举盘符 A..Z
    for (char c = 'A'; c <= 'Z'; c++) {
        char vol_path[32];
        snprintf(vol_path, sizeof(vol_path), "\\\\.\\%c:", c);
        HANDLE hv = CreateFileA(vol_path, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
        if (hv == INVALID_HANDLE_VALUE) continue;

        // 查询该逻辑卷所在的 PhysicalDrive 编号
        VOLUME_DISK_EXTENTS vde{};
        DWORD ret = 0;
        if (DeviceIoControl(hv, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                            nullptr, 0, &vde, sizeof(vde), &ret, nullptr)) {
            for (DWORD i = 0; i < vde.NumberOfDiskExtents; i++) {
                if ((int)vde.Extents[i].DiskNumber == disk_num) {
                    // 找到目标磁盘上的卷，卸载它
                    DWORD d2 = 0;
                    DeviceIoControl(hv, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &d2, nullptr);
                    if (!DeviceIoControl(hv, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &d2, nullptr)) {
                        fprintf(stderr, "[DISMOUNT] 卸载 %c: 失败 (err=%lu)\n", c, GetLastError());
                        fflush(stderr);
                        all_ok = false;
                    } else {
                        fprintf(stderr, "[DISMOUNT] 已卸载 %c:\n", c);
                        fflush(stderr);
                    }
                    break;
                }
            }
        }
        CloseHandle(hv);
    }
    return all_ok;
}

// ============================================================
//  便携双分区布局
// ============================================================
PortableLayoutResult create_portable_layout(const std::string& disk_path, uint32_t boot_mb) {
    PortableLayoutResult res;

    // 先卸载磁盘上所有卷
    int disk_num = extract_disk_number(disk_path);
    dismount_volumes_on_disk(disk_num);

    HANDLE h = CreateFileA(disk_path.c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        res.error_msg = "无法打开磁盘（需要管理员权限）";
        return res;
    }

    // 获取磁盘几何
    DISK_GEOMETRY_EX geo{};
    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                         nullptr, 0, &geo, sizeof(geo), &ret, nullptr)) {
        CloseHandle(h);
        res.error_msg = "无法读取磁盘几何参数";
        return res;
    }
    uint64_t total_bytes = geo.DiskSize.QuadPart;
    uint64_t sector_size = geo.Geometry.BytesPerSector;
    if (sector_size == 0) sector_size = 512;

    // 对齐到 1MB（常见分区工具对齐粒度）
    uint64_t align = 1024ULL * 1024; // 1MB
    uint64_t boot_bytes = (uint64_t)boot_mb * 1024 * 1024;
    // 分区1 起始偏移 = 1MB（保留 MBR + 对齐）
    uint64_t p1_start = align;
    uint64_t p1_size  = boot_bytes; // 1MB 对齐，boot_mb 本身应是整数 MB
    // 分区2 起始偏移 = 1MB + boot_bytes
    uint64_t p2_start = p1_start + p1_size;
    uint64_t p2_size  = total_bytes - p2_start - align; // 末尾留 1MB
    if ((int64_t)p2_size <= 0) {
        CloseHandle(h);
        res.error_msg = "磁盘太小，无法创建双分区布局";
        return res;
    }

    // 构建 MBR 分区布局
    DWORD lay_size = sizeof(DRIVE_LAYOUT_INFORMATION_EX) + 3 * sizeof(PARTITION_INFORMATION_EX);
    std::vector<uint8_t> lay_buf(lay_size, 0);
    auto* layout = (DRIVE_LAYOUT_INFORMATION_EX*)lay_buf.data();
    layout->PartitionStyle = PARTITION_STYLE_MBR;
    layout->PartitionCount = 4; // MBR 固定 4 个槽

    // 分区1：FAT32 明文分区（0x0C = FAT32 LBA）
    auto& p1 = layout->PartitionEntry[0];
    p1.PartitionStyle              = PARTITION_STYLE_MBR;
    p1.StartingOffset.QuadPart     = (LONGLONG)p1_start;
    p1.PartitionLength.QuadPart    = (LONGLONG)p1_size;
    p1.PartitionNumber             = 1;
    p1.RewritePartition            = TRUE;
    p1.Mbr.PartitionType           = 0x0C; // FAT32 LBA
    p1.Mbr.BootIndicator           = FALSE;
    p1.Mbr.RecognizedPartition     = TRUE;
    p1.Mbr.HiddenSectors           = (DWORD)(p1_start / sector_size);

    // 分区2：自定义类型（0x83 = Linux，Windows 不会自动挂载）
    auto& p2 = layout->PartitionEntry[1];
    p2.PartitionStyle              = PARTITION_STYLE_MBR;
    p2.StartingOffset.QuadPart     = (LONGLONG)p2_start;
    p2.PartitionLength.QuadPart    = (LONGLONG)p2_size;
    p2.PartitionNumber             = 2;
    p2.RewritePartition            = TRUE;
    p2.Mbr.PartitionType           = 0x83; // 不被 Windows 自动挂载
    p2.Mbr.BootIndicator           = FALSE;
    p2.Mbr.RecognizedPartition     = FALSE;
    p2.Mbr.HiddenSectors           = (DWORD)(p2_start / sector_size);

    // 剩余 2 个槽置空
    for (int i = 2; i < 4; i++) {
        auto& pe = layout->PartitionEntry[i];
        pe.PartitionStyle           = PARTITION_STYLE_MBR;
        pe.StartingOffset.QuadPart  = 0;
        pe.PartitionLength.QuadPart = 0;
        pe.PartitionNumber          = 0;
        pe.RewritePartition         = TRUE;
        pe.Mbr.PartitionType        = 0;
    }

    // 写入分区表
    DWORD bytes_returned = 0;
    if (!DeviceIoControl(h, IOCTL_DISK_SET_DRIVE_LAYOUT_EX,
                         lay_buf.data(), lay_size,
                         nullptr, 0, &bytes_returned, nullptr)) {
        DWORD err = GetLastError();
        fprintf(stderr, "[PORTABLE] SET_DRIVE_LAYOUT_EX 失败 err=%lu\n", err);
        fflush(stderr);
        CloseHandle(h);
        res.error_msg = "写入分区表失败 (err=" + std::to_string(err) + ")";
        return res;
    }
    fprintf(stderr, "[PORTABLE] 分区表写入成功 p1_start=%llu p1_size=%llu p2_start=%llu p2_size=%llu\n",
            (unsigned long long)p1_start, (unsigned long long)p1_size,
            (unsigned long long)p2_start, (unsigned long long)p2_size);
    fflush(stderr);

    // 通知系统刷新磁盘
    DeviceIoControl(h, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &bytes_returned, nullptr);
    CloseHandle(h);

    res.ok            = true;
    res.boot_offset   = p1_start;
    res.boot_size     = p1_size;
    res.crypto_offset = p2_start;
    res.crypto_size   = p2_size;
    return res;
}

char wait_for_drive_letter(int disk_num, uint64_t part_offset, int timeout_ms) {
    DWORD start = GetTickCount();
    while ((int)(GetTickCount() - start) < timeout_ms) {
        for (char c = 'C'; c <= 'Z'; c++) {
            char vol_path[32];
            snprintf(vol_path, sizeof(vol_path), "\\\\.\\%c:", c);
            HANDLE hv = CreateFileA(vol_path, GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
            if (hv == INVALID_HANDLE_VALUE) continue;

            // 查磁盘号和偏移
            DWORD buf_size = sizeof(VOLUME_DISK_EXTENTS) + 4 * sizeof(DISK_EXTENT);
            std::vector<uint8_t> buf(buf_size);
            auto* vde = (VOLUME_DISK_EXTENTS*)buf.data();
            DWORD ret = 0;
            if (DeviceIoControl(hv, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                                nullptr, 0, vde, buf_size, &ret, nullptr)) {
                for (DWORD i = 0; i < vde->NumberOfDiskExtents; i++) {
                    if ((int)vde->Extents[i].DiskNumber == disk_num &&
                        (uint64_t)vde->Extents[i].StartingOffset.QuadPart == part_offset) {
                        CloseHandle(hv);
                        return c;
                    }
                }
            }
            CloseHandle(hv);
        }
        Sleep(500);
    }
    return '\0';
}

bool format_volume_fat32(char vol_letter, const std::string& label) {
    // 格式化前先强制卸载卷，释放 Windows 对分区的独占锁
    // （新建分区后 Windows 可能已挂载它，format /Y 有时仍报"访问被拒绝"）
    {
        char vol_path[16];
        snprintf(vol_path, sizeof(vol_path), "\\\\.\\%c:", vol_letter);
        HANDLE hv = CreateFileA(vol_path, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
        if (hv != INVALID_HANDLE_VALUE) {
            DWORD d = 0;
            DeviceIoControl(hv, FSCTL_LOCK_VOLUME,   nullptr, 0, nullptr, 0, &d, nullptr);
            DeviceIoControl(hv, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &d, nullptr);
            CloseHandle(hv);
            fprintf(stderr, "[PORTABLE] format 前强制卸载 %c:\n", vol_letter); fflush(stderr);
            Sleep(1000); // 等待 Windows 更新状态
        }
    }

    // 构建 format 命令（静默执行），最多重试 3 次
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "format %c: /FS:FAT32 /Q /V:%s /Y",
             vol_letter, label.c_str());
    fprintf(stderr, "[PORTABLE] 执行: %s\n", cmd); fflush(stderr);

    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            fprintf(stderr, "[PORTABLE] format 重试 attempt=%d\n", attempt); fflush(stderr);
            Sleep(2000);
        }

        STARTUPINFOA si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        char full_cmd[320];
        snprintf(full_cmd, sizeof(full_cmd), "cmd.exe /C %s", cmd);
        if (!CreateProcessA(nullptr, full_cmd, nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            fprintf(stderr, "[PORTABLE] 启动 format 失败 err=%lu\n", GetLastError());
            fflush(stderr);
            continue;
        }
        WaitForSingleObject(pi.hProcess, 60000); // 最多等 60 秒
        DWORD exit_code = 1;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        fprintf(stderr, "[PORTABLE] format 退出码=%lu\n", exit_code); fflush(stderr);
        if (exit_code == 0) return true;
    }
    return false;
}

bool copy_exe_to(const std::string& exe_src, const std::string& dest_path) {
    return CopyFileA(exe_src.c_str(), dest_path.c_str(), FALSE) != 0;
}

// Windows 跨平台兼容函数（供 init_wizard 统一调用）
bool format_volume_fat32(const std::string& device_or_path, const std::string& label) {
    if(device_or_path.empty()) return false;
    char drive = device_or_path[0];
    if(drive >= 'a' && drive <= 'z') drive = drive - 'a' + 'A';
    return format_volume_fat32(drive, label);
}

std::string wait_for_mount_point(const std::string& device_path, int timeout_ms) {
    // Windows: 等待盘符分配，盘符就是 "挂载点"
    int disk_num = extract_disk_number(device_path);
    if(disk_num < 0) return "";
    // 需要知道分区偏移才能匹配，这里简化处理：直接返回设备路径本身（带冒号）
    // 实际在 init_wizard 中 Windows 路径已经通过 wait_for_drive_letter 获取了盘符
    return device_path;
}

std::string get_mount_point(const std::string& device_path) {
    // Windows: 盘符本身就是挂载点
    if(device_path.size() >= 2 && device_path[1] == ':') return device_path;
    return "";
}

bool copy_file_or_dir(const std::string& src, const std::string& dst) {
    DWORD attr = GetFileAttributesA(src.c_str());
    if(attr == INVALID_FILE_ATTRIBUTES) return false;
    if(attr & FILE_ATTRIBUTE_DIRECTORY) {
        // 递归复制目录
        std::string cmd = "xcopy \"" + src + "\" \"" + dst + "\\\" /E /I /Y /Q";
        STARTUPINFOA si = {}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        if(CreateProcessA(nullptr, (LPSTR)cmd.c_str(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 120000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return true;
        }
        return false;
    }
    return CopyFileA(src.c_str(), dst.c_str(), FALSE) != 0;
}

#else
//----------- POSIX (macOS / Linux) -----------
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <array>
#include <memory>

// macOS does not have lseek64; lseek is already 64-bit on macOS & Linux 64-bit
#ifdef __APPLE__
    #define SDRV_LSEEK lseek
#else
    #define SDRV_LSEEK lseek64
#endif

namespace fs = std::filesystem;

#ifdef __APPLE__
#  include <sys/disk.h>
#  include <sys/stat.h>
#elif defined(__linux__)
#  include <linux/fs.h>
#endif

// ============================================================
//  跨平台辅助函数
// ============================================================

static std::string run_cmd(const std::string& cmd) {
    std::array<char, 256> buffer;
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        result += buffer.data();
    }
    pclose(pipe);
    return result;
}

static int run_cmd_exit_code(const std::string& cmd) {
    int rc = std::system(cmd.c_str());
    return WEXITSTATUS(rc);
}

// ============================================================
//  RawDisk I/O（保留原有实现）
// ============================================================

bool RawDisk::open(const std::string& path, bool write_mode) {
    close();
    int flags = write_mode ? O_RDWR : O_RDONLY;
#ifdef __linux__
    flags |= O_DIRECT; // 绕过页缓存，确保直接 IO
#endif
    handle_ = ::open(path.c_str(), flags | O_CLOEXEC);
    if(handle_ < 0){ handle_ = INVALID_NATIVE; return false; }

#ifdef __APPLE__
    uint32_t blksz = 512;
    uint64_t blkcnt = 0;
    ioctl(handle_, DKIOCGETBLOCKSIZE,  &blksz);
    ioctl(handle_, DKIOCGETBLOCKCOUNT, &blkcnt);
    sector_size_  = blksz;
    device_size_  = (uint64_t)blksz * blkcnt;
#elif defined(__linux__)
    ioctl(handle_, BLKSSZGET,  &sector_size_);
    ioctl(handle_, BLKGETSIZE64, &device_size_);
#endif
    return true;
}

void RawDisk::close() {
    if(handle_ >= 0){
        ::close(handle_);
        handle_ = INVALID_NATIVE;
    }
}

bool RawDisk::read(uint64_t offset, void* buf, size_t size) {
    if(!is_open()) return false;
    if(SDRV_LSEEK(handle_, (off_t)offset, SEEK_SET) < 0) return false;
    size_t done = 0;
    while(done < size){
        ssize_t n = ::read(handle_, (char*)buf+done, size-done);
        if(n <= 0) return false;
        done += n;
    }
    return true;
}

bool RawDisk::write(uint64_t offset, const void* buf, size_t size) {
    if(!is_open()) return false;
    if(SDRV_LSEEK(handle_, (off_t)offset, SEEK_SET) < 0) return false;
    size_t done = 0;
    while(done < size){
        ssize_t n = ::write(handle_, (const char*)buf+done, size-done);
        if(n <= 0) return false;
        done += n;
    }
    return true;
}

// ============================================================
//  设备枚举（完善版）
// ============================================================

std::vector<DiskInfo> enum_disks() {
    std::vector<DiskInfo> result;
#ifdef __linux__
    FILE* f = fopen("/proc/partitions", "r");
    if(!f) return result;
    char line[256];
    fgets(line,256,f); fgets(line,256,f);
    while(fgets(line,256,f)){
        unsigned maj,min; unsigned long long blocks; char name[64];
        if(sscanf(line,"%u %u %llu %63s",&maj,&min,&blocks,name)!=4) continue;
        std::string sname(name);
        bool is_disk = true;
        for(char c:sname) if(isdigit(c)){ is_disk=false; break; }
        if(!is_disk) continue;
        DiskInfo di;
        di.device_path  = "/dev/" + sname;
        di.display_name = sname;
        di.total_bytes  = blocks * 1024;
        di.sector_size  = 512;
        di.removable    = false;
        std::string rem_path = "/sys/block/" + sname + "/removable";
        FILE* rf = fopen(rem_path.c_str(),"r");
        if(rf){ int v=0; fscanf(rf,"%d",&v); di.removable=(v!=0); fclose(rf); }
        result.push_back(di);
    }
    fclose(f);
#elif defined(__APPLE__)
    for(int i=0;i<16;i++){
        std::string path="/dev/disk"+std::to_string(i);
        int fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC);
        if(fd<0) continue;
        DiskInfo di; di.device_path=path;
        di.display_name="disk"+std::to_string(i);
        uint32_t blksz=512; uint64_t blkcnt=0;
        ioctl(fd,DKIOCGETBLOCKSIZE,&blksz);
        ioctl(fd,DKIOCGETBLOCKCOUNT,&blkcnt);
        di.sector_size=blksz; di.total_bytes=(uint64_t)blksz*blkcnt;
        di.removable=false;
        // 检测是否可移动（通过 IOKit 或 diskutil）
        std::string info = run_cmd("diskutil info " + path + " 2>/dev/null | grep 'Removable Media'");
        if(info.find("Yes") != std::string::npos || info.find("Removable") != std::string::npos) {
            di.removable = true;
        }
        ::close(fd);
        result.push_back(di);
    }
#endif
    return result;
}

std::vector<PartitionInfo> enum_partitions(const std::string& disk_path) {
    std::vector<PartitionInfo> result;
#ifdef __linux__
    std::string base = disk_path.substr(disk_path.rfind('/')+1);
    // 使用 lsblk 获取精确的分区信息（含偏移）
    std::string cmd = "lsblk -b -n -o NAME,SIZE,TYPE,START /dev/" + base + " 2>/dev/null";
    std::string output = run_cmd(cmd);
    if(!output.empty()){
        uint32_t idx=0;
        char line[512];
        size_t pos=0;
        while(pos < output.size()){
            size_t end = output.find('\n', pos);
            if(end==std::string::npos) end=output.size();
            std::string l = output.substr(pos, end-pos);
            pos = end+1;
            char name[64]; uint64_t size=0; char type[32]; uint64_t start=0;
            if(sscanf(l.c_str(),"%63s %llu %31s %llu",name,&size,type,&start)>=3){
                if(strcmp(type,"part")==0){
                    PartitionInfo pi;
                    pi.device_path = "/dev/" + std::string(name);
                    pi.parent_disk = disk_path;
                    pi.label = name;
                    pi.size_bytes = size;
                    pi.offset_bytes = start;
                    pi.index = idx++;
                    result.push_back(pi);
                }
            }
        }
    }
    if(result.empty()){
        // fallback: 读 /proc/partitions
        FILE* f=fopen("/proc/partitions","r");
        if(f){
            char line[256];
            fgets(line,256,f); fgets(line,256,f);
            uint32_t idx=0;
            while(fgets(line,256,f)){
                unsigned maj,min; unsigned long long blocks; char name[64];
                if(sscanf(line,"%u %u %llu %63s",&maj,&min,&blocks,name)!=4) continue;
                std::string sname(name);
                if(sname==base) continue;
                if(sname.find(base)==0){
                    PartitionInfo pi;
                    pi.device_path="/dev/"+sname;
                    pi.parent_disk=disk_path;
                    pi.label=sname; pi.size_bytes=blocks*1024;
                    pi.offset_bytes=0; pi.index=idx++;
                    result.push_back(pi);
                }
            }
            fclose(f);
        }
    }
#elif defined(__APPLE__)
    // 使用 diskutil list 获取分区信息
    std::string cmd = "diskutil list -plist " + disk_path + " 2>/dev/null";
    std::string output = run_cmd(cmd);
    if(!output.empty()){
        // 简单解析：查找 <key>Offset</key> 和 <integer>...
        // 由于 plist 解析较复杂，这里用简化方案
        // fallback: 枚举 /dev/diskNs1 .. diskNs8
        for(int i=1;i<=8;i++){
            std::string path=disk_path+"s"+std::to_string(i);
            int fd=::open(path.c_str(),O_RDONLY|O_CLOEXEC);
            if(fd<0) continue;
            PartitionInfo pi; pi.device_path=path;
            pi.parent_disk=disk_path;
            pi.label="Slice"+std::to_string(i); pi.index=i-1;
            uint32_t blksz=512; uint64_t blkcnt=0;
            ioctl(fd,DKIOCGETBLOCKSIZE,&blksz);
            ioctl(fd,DKIOCGETBLOCKCOUNT,&blkcnt);
            pi.size_bytes=(uint64_t)blksz*blkcnt;
            // 获取偏移（通过 diskutil info）
            std::string info = run_cmd("diskutil info " + path + " 2>/dev/null | grep 'Partition Offset'");
            // 格式: Partition Offset: 1048576 Bytes (1.0 MB)
            uint64_t off = 0;
            sscanf(info.c_str(),"Partition Offset: %llu",&off);
            pi.offset_bytes = off;
            ::close(fd);
            result.push_back(pi);
        }
    }
#endif
    return result;
}

// ============================================================
//  卷管理（macOS / Linux）
// ============================================================

#ifndef _WIN32
bool dismount_volumes_on_disk(const std::string& disk_path) {
#ifdef __APPLE__
    std::string cmd = "diskutil unmountDisk " + disk_path + " 2>&1";
    std::string out = run_cmd(cmd);
    fprintf(stderr, "[DISMOUNT] macOS: %s\n", out.c_str()); fflush(stderr);
    return out.find("successful") != std::string::npos || out.find("unmounted") != std::string::npos;
#else
    // Linux: 读取 /proc/mounts 找到该磁盘上的所有挂载点，逐个 umount
    std::string base = disk_path.substr(disk_path.rfind('/')+1);
    FILE* f = fopen("/proc/mounts", "r");
    if(!f) return false;
    bool all_ok = true;
    char line[512];
    while(fgets(line, sizeof(line), f)){
        char dev[256], mnt[256], fstype[32];
        if(sscanf(line, "%255s %255s %31s", dev, mnt, fstype) != 3) continue;
        std::string dev_str(dev);
        // 匹配设备路径：/dev/sda1, /dev/nvme0n1p1 等
        if(dev_str.find(disk_path) == 0 || dev_str.find("/dev/" + base) == 0){
            std::string umount_cmd = std::string("umount '") + mnt + "' 2>&1";
            std::string out = run_cmd(umount_cmd);
            fprintf(stderr, "[DISMOUNT] Linux umount %s: %s\n", mnt, out.c_str()); fflush(stderr);
        }
    }
    fclose(f);
    return all_ok;
#endif
}
#endif

// ============================================================
//  便携双分区布局
// ============================================================

PortableLayoutResult create_portable_layout(const std::string& disk_path, uint32_t boot_mb) {
    PortableLayoutResult res;

    // 先卸载磁盘上所有卷
    dismount_volumes_on_disk(disk_path);

#ifdef __APPLE__
    // macOS: 使用 diskutil partitionDisk
    // 获取磁盘大小
    uint64_t total_bytes = 0;
    {
        int fd = ::open(disk_path.c_str(), O_RDONLY|O_CLOEXEC);
        if(fd >= 0){
            uint32_t blksz=512; uint64_t blkcnt=0;
            ioctl(fd, DKIOCGETBLOCKSIZE, &blksz);
            ioctl(fd, DKIOCGETBLOCKCOUNT, &blkcnt);
            total_bytes = (uint64_t)blksz * blkcnt;
            ::close(fd);
        }
    }
    if(total_bytes == 0){
        res.error_msg = "无法读取磁盘大小";
        return res;
    }

    uint64_t align = 1024ULL * 1024; // 1MB
    uint64_t boot_bytes = (uint64_t)boot_mb * 1024 * 1024;
    uint64_t p1_start = align;
    uint64_t p1_size  = boot_bytes;
    uint64_t p2_start = p1_start + p1_size;
    uint64_t p2_size  = total_bytes - p2_start - align;
    if((int64_t)p2_size <= 0){
        res.error_msg = "磁盘太小，无法创建双分区布局";
        return res;
    }

    // diskutil partitionDisk 会自动创建分区并格式化第一个分区
    // 但我们只需要它创建分区，格式化由后续 format_volume_fat32 处理
    // 注意：diskutil 的单位是 B (bytes)
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "diskutil partitionDisk %s MBR \"MS-DOS FAT32\" \"SDRV_BOOT\" %lluB \"Free Space\" \"SDRV_CRYPTO\" 0b 2>&1",
             disk_path.c_str(), (unsigned long long)boot_bytes);
    fprintf(stderr, "[PORTABLE] macOS cmd: %s\n", cmd); fflush(stderr);
    std::string out = run_cmd(cmd);
    fprintf(stderr, "[PORTABLE] macOS result: %s\n", out.c_str()); fflush(stderr);
    if(out.find("Finished partitioning") == std::string::npos &&
       out.find("successful") == std::string::npos){
        res.error_msg = "分区失败: " + out;
        return res;
    }

    res.ok = true;
    res.boot_offset = p1_start;
    res.boot_size = p1_size;
    res.crypto_offset = p2_start;
    res.crypto_size = p2_size;
    return res;

#else
    // Linux: 使用 parted
    uint64_t total_bytes = 0;
    {
        int fd = ::open(disk_path.c_str(), O_RDONLY|O_CLOEXEC);
        if(fd >= 0){
            ioctl(fd, BLKGETSIZE64, &total_bytes);
            ::close(fd);
        }
    }
    if(total_bytes == 0){
        res.error_msg = "无法读取磁盘大小";
        return res;
    }

    uint64_t align = 1024ULL * 1024;
    uint64_t boot_bytes = (uint64_t)boot_mb * 1024 * 1024;
    uint64_t p1_start = align;
    uint64_t p1_size  = boot_bytes;
    uint64_t p2_start = p1_start + p1_size;
    uint64_t p2_size  = total_bytes - p2_start - align;
    if((int64_t)p2_size <= 0){
        res.error_msg = "磁盘太小，无法创建双分区布局";
        return res;
    }

    // 使用 parted 创建 MBR 分区表
    std::string cmd1 = std::string("parted -s ") + disk_path + " mklabel msdos 2>&1";
    std::string out1 = run_cmd(cmd1);
    fprintf(stderr, "[PORTABLE] parted mklabel: %s\n", out1.c_str()); fflush(stderr);

    char cmd2[512];
    snprintf(cmd2, sizeof(cmd2),
             "parted -s %s mkpart primary fat32 %lluB %lluB 2>&1",
             disk_path.c_str(),
             (unsigned long long)p1_start,
             (unsigned long long)(p1_start + p1_size));
    std::string out2 = run_cmd(cmd2);
    fprintf(stderr, "[PORTABLE] parted mkpart1: %s\n", out2.c_str()); fflush(stderr);

    char cmd3[512];
    snprintf(cmd3, sizeof(cmd3),
             "parted -s %s mkpart primary %lluB %lluB 2>&1",
             disk_path.c_str(),
             (unsigned long long)p2_start,
             (unsigned long long)(total_bytes - align));
    std::string out3 = run_cmd(cmd3);
    fprintf(stderr, "[PORTABLE] parted mkpart2: %s\n", out3.c_str()); fflush(stderr);

    // 刷新内核分区表
    std::string cmd4 = std::string("partprobe ") + disk_path + " 2>&1";
    run_cmd(cmd4);

    res.ok = true;
    res.boot_offset = p1_start;
    res.boot_size = p1_size;
    res.crypto_offset = p2_start;
    res.crypto_size = p2_size;
    return res;
#endif
}

// ============================================================
//  格式化 FAT32
// ============================================================

bool format_volume_fat32(const std::string& device_or_letter, const std::string& label) {
#ifdef __APPLE__
    // macOS: diskutil eraseVolume FAT32 LABEL /dev/diskNs1
    std::string cmd = "diskutil eraseVolume FAT32 \"" + label + "\" " + device_or_letter + " 2>&1";
    fprintf(stderr, "[FORMAT] macOS: %s\n", cmd.c_str()); fflush(stderr);
    std::string out = run_cmd(cmd);
    fprintf(stderr, "[FORMAT] macOS result: %s\n", out.c_str()); fflush(stderr);
    return out.find("Finished") != std::string::npos || out.find("successful") != std::string::npos;
#else
    // Linux: mkfs.vfat -F 32 -n LABEL /dev/sdX1
    std::string cmd = std::string("mkfs.vfat -F 32 -n '") + label + "' " + device_or_letter + " 2>&1";
    fprintf(stderr, "[FORMAT] Linux: %s\n", cmd.c_str()); fflush(stderr);
    std::string out = run_cmd(cmd);
    fprintf(stderr, "[FORMAT] Linux result: %s\n", out.c_str()); fflush(stderr);
    return out.find("mkfs.fat") != std::string::npos || out.find("FAT32") != std::string::npos || run_cmd_exit_code(cmd) == 0;
#endif
}

// ============================================================
//  等待挂载点
// ============================================================

std::string wait_for_mount_point(const std::string& device_path, int timeout_ms) {
    auto start = std::chrono::steady_clock::now();
    while(true){
        std::string mp = get_mount_point(device_path);
        if(!mp.empty()) return mp;
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if(elapsed >= timeout_ms) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return "";
}

std::string get_mount_point(const std::string& device_path) {
#ifdef __APPLE__
    // diskutil info /dev/diskNs1 | grep 'Mount Point'
    std::string cmd = "diskutil info " + device_path + " 2>/dev/null | grep 'Mount Point'";
    std::string out = run_cmd(cmd);
    // 格式: Mount Point: /Volumes/SDRV_BOOT
    size_t pos = out.find(':');
    if(pos != std::string::npos){
        std::string mp = out.substr(pos+1);
        // trim
        size_t start = mp.find_first_not_of(" \t\n\r");
        if(start != std::string::npos){
            size_t end = mp.find_last_not_of(" \t\n\r");
            mp = mp.substr(start, end-start+1);
            if(mp != "(not mounted)") return mp;
        }
    }
    return "";
#else
    // Linux: 读取 /proc/mounts
    FILE* f = fopen("/proc/mounts", "r");
    if(!f) return "";
    char line[512];
    while(fgets(line, sizeof(line), f)){
        char dev[256], mnt[256];
        if(sscanf(line, "%255s %255s", dev, mnt) == 2){
            if(dev == device_path){
                fclose(f);
                return std::string(mnt);
            }
        }
    }
    fclose(f);
    return "";
#endif
}

// ============================================================
//  复制文件或目录
// ============================================================

bool copy_file_or_dir(const std::string& src, const std::string& dst) {
    try {
        fs::path src_p(src);
        fs::path dst_p(dst);
        if(fs::is_directory(src_p)){
            fs::copy(src_p, dst_p, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        } else {
            fs::copy_file(src_p, dst_p, fs::copy_options::overwrite_existing);
        }
        return true;
    } catch(const std::exception& e){
        fprintf(stderr, "[COPY] error: %s\n", e.what()); fflush(stderr);
        return false;
    }
}

bool copy_exe_to(const std::string& exe_src, const std::string& dest_path) {
    return copy_file_or_dir(exe_src, dest_path);
}

#endif // !_WIN32
