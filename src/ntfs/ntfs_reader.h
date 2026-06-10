#pragma once
// ===========================================================
// NtfsReader - simplified NTFS read-only parser
// ===========================================================
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

class Volume;

#pragma pack(push, 1)

// NTFS Boot Sector (VBR) - matches on-disk layout
struct NtfsBootSector {
    uint8_t  jump[3];               // 0x00
    char     oem_id[8];             // 0x03
    uint16_t bytes_per_sector;      // 0x0B
    uint8_t  sectors_per_cluster;   // 0x0D
    uint16_t reserved_sectors;      // 0x0E
    uint8_t  always_zero0[3];       // 0x10
    uint16_t unused_bpb;            // 0x13  FAT total_sectors_16 (0 on NTFS)
    uint8_t  media_descriptor;      // 0x15
    uint16_t always_zero1;          // 0x16
    uint16_t sectors_per_track;     // 0x18
    uint16_t number_of_heads;       // 0x1A
    uint32_t hidden_sectors;        // 0x1C
    uint32_t always_zero2;          // 0x20
    uint32_t unused_ntfs;           // 0x24  NTFS reserved
    uint64_t total_sectors;         // 0x28
    uint64_t mft_start_cluster;     // 0x30
    uint64_t mft_mirror_cluster;    // 0x38
    int8_t   mft_record_size;       // 0x40
    uint8_t  unused3[3];            // 0x41
    int8_t   index_buffer_size;     // 0x44
    uint8_t  unused4[3];            // 0x45
    uint64_t volume_serial;         // 0x48
    uint32_t checksum;              // 0x50
    uint8_t  boot_code[426];        // 0x54
    uint16_t end_marker;            // 0x1FE
};
static_assert(sizeof(NtfsBootSector) == 512, "NtfsBootSector size must be 512");

struct NtfsMftRecordHeader {
    uint8_t  signature[4];
    uint16_t update_seq_offset;
    uint16_t update_seq_count;
    uint64_t log_file_seq;
    uint16_t sequence_number;
    uint16_t hard_link_count;
    uint16_t attr_offset;
    uint16_t flags;
    uint32_t bytes_in_use;
    uint32_t bytes_allocated;
    uint64_t base_file_record;
    uint16_t next_attr_id;
    uint16_t pad;
    uint32_t mft_record_number;
};

struct NtfsAttrHeader {
    uint32_t type;
    uint32_t length;
    uint8_t  non_resident;
    uint8_t  name_length;
    uint16_t name_offset;
    uint16_t flags;
    uint16_t attr_id;
};

struct NtfsResidentAttr {
    uint32_t value_length;
    uint16_t value_offset;
    uint8_t  indexed_flag;
    uint8_t  pad;
};

struct NtfsNonResidentAttr {
    uint64_t lowest_vcn;
    uint64_t highest_vcn;
    uint16_t mapping_pairs_offset;
    uint16_t compression_unit;
    uint8_t  pad[4];
    uint64_t allocated_size;
    uint64_t data_size;
    uint64_t initialized_size;
};

struct NtfsFileNameAttr {
    uint64_t parent_directory;
    uint64_t creation_time;
    uint64_t change_time;
    uint64_t mft_change_time;
    uint64_t access_time;
    uint64_t allocated_size;
    uint64_t real_size;
    uint32_t flags;
    uint32_t reparse;
    uint8_t  filename_length;
    uint8_t  filename_namespace;
};

#pragma pack(pop)

// ===========================================================
// NtfsFileEntry - parsed file info
// ===========================================================
struct NtfsFileEntry {
    uint64_t    mft_number;
    uint64_t    parent_mft;
    std::string name;
    std::string full_path;
    uint64_t    file_size;
    bool        is_dir;
    bool        is_system;
};

using NtfsDataCallback = std::function<bool(uint64_t offset, const uint8_t* data, size_t len)>;

// ===========================================================
// NtfsReader
// ===========================================================
class NtfsReader {
public:
    NtfsReader() = default;
    ~NtfsReader() = default;

    bool open(Volume* vol);
    bool enumerate_files();
    const std::vector<NtfsFileEntry>& files() const { return files_; }

    bool read_file(const NtfsFileEntry& entry,
                   NtfsDataCallback cb,
                   size_t chunk_size = 1 << 20);

    uint32_t bytes_per_sector()   const { return bps_; }
    uint32_t sectors_per_cluster() const { return spc_; }

private:
    Volume*   vol_        = nullptr;
    uint32_t  bps_        = 512;
    uint32_t  spc_        = 8;
    uint32_t  bytes_per_cluster_ = 4096;
    uint32_t  file_record_size_ = 1024;
    uint64_t  mft_start_cluster_ = 0;
    uint64_t  total_sectors_     = 0;
    uint64_t  mft_byte_size_     = 0;

    std::vector<NtfsFileEntry> files_;
    std::unordered_map<uint64_t, std::string> path_cache_;

    bool read_sectors(uint64_t lba, void* buf, uint32_t sector_count);
    bool read_clusters(uint64_t vcn, void* buf, uint32_t cluster_count);

    bool     read_mft_record(uint64_t mft_num, std::vector<uint8_t>& rec);
    bool     apply_fixup(uint8_t* rec, uint32_t size);

    const uint8_t* find_attr(const uint8_t* rec, uint32_t rec_size,
                              uint32_t attr_type) const;
    bool read_resident_value(const uint8_t* attr, std::vector<uint8_t>& out) const;
    bool read_nonresident_data(const uint8_t* attr, uint64_t offset,
                               uint8_t* buf, uint64_t length);

    struct DataRun { uint64_t lcn; uint64_t length; };
    bool parse_data_runs(const uint8_t* pairs_ptr, uint64_t pairs_size,
                         std::vector<DataRun>& runs) const;

    std::string build_path(uint64_t mft_num);

    std::vector<DataRun> mft_runs_;
};
