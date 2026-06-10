// ntfs_reader.cpp - simplified NTFS read-only parser
#include "ntfs_reader.h"
#include "../volume/volume.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <queue>

// ===================================================================
// Helper: read little-endian integers from raw bytes
// ===================================================================

static uint64_t read_le64(const void* vp) {
    const uint8_t* p = (const uint8_t*)vp;
    return (uint64_t)p[0]
         | ((uint64_t)p[1] << 8)
         | ((uint64_t)p[2] << 16)
         | ((uint64_t)p[3] << 24)
         | ((uint64_t)p[4] << 32)
         | ((uint64_t)p[5] << 40)
         | ((uint64_t)p[6] << 48)
         | ((uint64_t)p[7] << 56);
}

static uint32_t read_le32(const void* vp) {
    const uint8_t* p = (const uint8_t*)vp;
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint16_t read_le16(const void* vp) {
    const uint8_t* p = (const uint8_t*)vp;
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// ===================================================================
// UTF-16LE -> UTF-8
// ===================================================================

static std::string utf16le_to_utf8(const uint8_t* src, size_t len_chars) {
    std::string out;
    for (size_t i = 0; i < len_chars; i++) {
        uint32_t cp = read_le16(src + i * 2);
        if (cp == 0) break;
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// ===================================================================
// NtfsReader - implementation
// ===================================================================

// ---- raw I/O via Volume::read_sectors() ----

bool NtfsReader::read_sectors(uint64_t lba, void* buf, uint32_t sector_count) {
    if (!vol_) return false;
    return vol_->read_sectors(lba, buf, sector_count);
}

bool NtfsReader::read_clusters(uint64_t vcn, void* buf, uint32_t cluster_count) {
    uint64_t lba = vcn * spc_;
    uint32_t sectors = cluster_count * spc_;
    return read_sectors(lba, buf, sectors);
}

// ---- open ----

bool NtfsReader::open(Volume* vol) {
    vol_ = vol;
    if (!vol_) return false;

    // Read Boot Sector (LBA 0 of partition)
    uint8_t boot_buf[512];
    if (!read_sectors(0, boot_buf, 1)) return false;

    NtfsBootSector* bs = (NtfsBootSector*)boot_buf;

    // Verify OEM ID
    if (std::memcmp(bs->oem_id, "NTFS    ", 8) != 0) {
        fprintf(stderr, "[NTFS] Bad OEM ID\n");
        return false;
    }

    bps_               = read_le16(&bs->bytes_per_sector);
    spc_               = bs->sectors_per_cluster;
    bytes_per_cluster_  = bps_ * spc_;
    total_sectors_      = read_le64(&bs->total_sectors);
    mft_start_cluster_ = read_le64(&bs->mft_start_cluster);

    // Determine MFT record size
    int8_t rec_exp = bs->mft_record_size;
    if (rec_exp > 0) {
        file_record_size_ = spc_ * bps_ * rec_exp;
    } else {
        file_record_size_ = (uint32_t)1 << (-rec_exp);
    }

    fprintf(stderr, "[NTFS] bps=%u spc=%u mft_cluster=%llu rec_size=%u\n",
            bps_, spc_, (unsigned long long)mft_start_cluster_, file_record_size_);

    mft_byte_size_ = 0;  // will be set after reading MFT $DATA runs

    // Read MFT's own $DATA runs
    // First read MFT record #0 (the MFT itself)
    std::vector<uint8_t> mft_rec;
    if (!read_mft_record(0, mft_rec)) {
        fprintf(stderr, "[NTFS] Cannot read MFT record 0\n");
        return false;
    }

    // Find $DATA attribute in MFT record 0
    const uint8_t* data_attr = find_attr(mft_rec.data(), mft_rec.size(), 0x80);
    if (!data_attr) {
        fprintf(stderr, "[NTFS] MFT record 0 has no $DATA\n");
        return false;
    }

    // Parse data runs from $DATA
    const NtfsAttrHeader* ah = (const NtfsAttrHeader*)data_attr;
    if (ah->non_resident == 0) {
        // MFT is resident (should not happen for large volumes)
        fprintf(stderr, "[NTFS] MFT $DATA is resident (small volume?)\n");
        // For resident: MFT fits in one record
        mft_byte_size_ = file_record_size_ * 16;  // assume 16 MFT records
    } else {
        const NtfsNonResidentAttr* nra = (const NtfsNonResidentAttr*)(data_attr + sizeof(NtfsAttrHeader));
        const uint8_t* pairs = data_attr + read_le16(&ah->length) - read_le16(&nra->mapping_pairs_offset);
        // Actually, mapping_pairs_offset is from start of attr header
        const uint8_t* mapping = data_attr + read_le16(&nra->mapping_pairs_offset);
        parse_data_runs(mapping, (uint32_t)(read_le32(&ah->length) - read_le16(&nra->mapping_pairs_offset)), mft_runs_);
    }

    return true;
}

// ---- read_mft_record ----

bool NtfsReader::read_mft_record(uint64_t mft_num, std::vector<uint8_t>& rec) {
    rec.resize(file_record_size_);

    if (mft_runs_.empty()) {
        // MFT is contiguous from mft_start_cluster_
        uint64_t vcn = mft_num * (file_record_size_ / bytes_per_cluster_);
        if (!read_clusters(mft_start_cluster_ + vcn, rec.data(), 1)) return false;
    } else {
        // Use data runs to address the correct cluster
        uint64_t target_vcn = mft_num * (file_record_size_ / bytes_per_cluster_);
        uint64_t current_lcn = 0;
        uint64_t current_vcn = 0;
        bool found = false;
        for (const auto& run : mft_runs_) {
            if (target_vcn < current_vcn + run.length) {
                uint64_t offset_in_run = target_vcn - current_vcn;
                return read_clusters(run.lcn + offset_in_run, rec.data(), 1);
            }
            current_vcn += run.length;
        }
        return false;
    }

    // Apply fixup (update sequence)
    return apply_fixup(rec.data(), file_record_size_);
}

// ---- apply_fixup ----

bool NtfsReader::apply_fixup(uint8_t* rec, uint32_t size) {
    NtfsMftRecordHeader* hdr = (NtfsMftRecordHeader*)rec;
    uint16_t usn = read_le16(&hdr->update_seq_offset);
    uint16_t usc = read_le16(&hdr->update_seq_count);

    if (usc == 0) return true;

    uint16_t* us_array = (uint16_t*)(rec + usn);
    uint16_t usig = us_array[0];  // update sequence number

    // Every 512-byte sector ends with the stored value
    for (uint32_t i = 1; i < usc && (i * 512) < size; i++) {
        uint16_t* end_of_sector = (uint16_t*)(rec + i * 512 - 2);
        *end_of_sector = us_array[i];
    }
    return true;
}

// ---- find_attr ----

const uint8_t* NtfsReader::find_attr(const uint8_t* rec, uint32_t rec_size, uint32_t attr_type) const {
    const NtfsMftRecordHeader* hdr = (const NtfsMftRecordHeader*)rec;
    uint16_t off = read_le16(&hdr->attr_offset);
    while (off < rec_size) {
        const NtfsAttrHeader* ah = (const NtfsAttrHeader*)(rec + off);
        if (read_le32(&ah->type) == 0xFFFFFFFF) break;  // end marker
        if (read_le32(&ah->type) == attr_type) return rec + off;
        uint32_t alen = read_le32(&ah->length);
        if (alen == 0) break;
        off += alen;
    }
    return nullptr;
}

// ---- read_resident_value ----

bool NtfsReader::read_resident_value(const uint8_t* attr, std::vector<uint8_t>& out) const {
    const NtfsAttrHeader* ah = (const NtfsAttrHeader*)attr;
    const NtfsResidentAttr* ra = (const NtfsResidentAttr*)(attr + sizeof(NtfsAttrHeader));
    uint32_t vlen = read_le32(&ra->value_length);
    const uint8_t* vdata = attr + read_le16(&ra->value_offset);
    out.assign(vdata, vdata + vlen);
    return true;
}

// ---- read_nonresident_data ----

bool NtfsReader::read_nonresident_data(const uint8_t* attr, uint64_t offset,
                                           uint8_t* buf, uint64_t length) {
    const NtfsAttrHeader* ah = (const NtfsAttrHeader*)attr;
    if (ah->non_resident == 0) {
        // Resident: read from within the record
        const NtfsResidentAttr* ra = (const NtfsResidentAttr*)(attr + sizeof(NtfsAttrHeader));
        uint32_t voff = read_le16(&ra->value_offset);
        uint32_t vlen = read_le32(&ra->value_length);
        uint32_t to_copy = (uint32_t)std::min((uint64_t)vlen - offset, length);
        std::memcpy(buf, attr + voff + offset, to_copy);
        return true;
    }

    const NtfsNonResidentAttr* nra = (const NtfsNonResidentAttr*)(attr + sizeof(NtfsAttrHeader));
    const uint8_t* mapping = attr + read_le16(&nra->mapping_pairs_offset);
    uint64_t data_size = read_le64(&nra->data_size);

    std::vector<DataRun> runs;
    parse_data_runs(mapping, 512, runs);  // safe upper bound

    uint64_t bytes_copied = 0;
    uint64_t current_vcn = 0;

    for (const auto& run : runs) {
        uint64_t run_start_byte = current_vcn * bytes_per_cluster_;
        uint64_t run_end_byte = (current_vcn + run.length) * bytes_per_cluster_;

        if (offset + length <= run_start_byte) break;
        if (offset >= run_end_byte) {
            current_vcn += run.length;
            continue;
        }

        uint64_t chunk_offset = std::max(offset, run_start_byte);
        uint64_t chunk_end    = std::min(offset + length, run_end_byte);
        uint64_t chunk_size    = chunk_end - chunk_offset;
        uint64_t cluster_in_run = (chunk_offset - run_start_byte) / bytes_per_cluster_;

        read_clusters(run.lcn + cluster_in_run,
                     buf + (chunk_offset - offset),
                     (uint32_t)((chunk_size + bytes_per_cluster_ - 1) / bytes_per_cluster_));

        current_vcn += run.length;
    }
    return true;
}

// ---- parse_data_runs ----

bool NtfsReader::parse_data_runs(const uint8_t* pairs_ptr, uint64_t pairs_size,
                                      std::vector<DataRun>& runs) const {
    runs.clear();
    const uint8_t* p = pairs_ptr;
    const uint8_t* end = pairs_ptr + pairs_size;
    int64_t last_lcn = 0;

    while (p < end && *p != 0) {
        uint8_t header = *p++;
        uint8_t len_bytes  = header & 0x0F;
        uint8_t lcn_bytes  = (header >> 4) & 0x0F;

        if (len_bytes == 0 || lcn_bytes == 0) break;

        uint64_t length = 0;
        for (int i = 0; i < len_bytes; i++) {
            length |= (uint64_t)(*p++) << (i * 8);
        }

        int64_t lcn_delta = 0;
        for (int i = 0; i < lcn_bytes; i++) {
            lcn_delta |= (int64_t)(*p++) << (i * 8);
        }
        // Sign-extend
        if (lcn_bytes < 8 && (lcn_delta & ((int64_t)1 << (lcn_bytes * 8 - 1)))) {
            lcn_delta |= (int64_t)0xFFFFFFFFFFFFFFFF << (lcn_bytes * 8);
        }

        last_lcn += lcn_delta;
        if (last_lcn < 0) break;  // sanity

        DataRun run;
        run.lcn    = (uint64_t)last_lcn;
        run.length = length;
        runs.push_back(run);
    }
    return true;
}

// ---- build_path (memoized) ----

std::string NtfsReader::build_path(uint64_t mft_num) {
    auto it = path_cache_.find(mft_num);
    if (it != path_cache_.end()) return it->second;

    // Read the MFT record for this file
    std::vector<uint8_t> rec;
    if (!read_mft_record(mft_num, rec)) return "";

    const NtfsMftRecordHeader* hdr = (const NtfsMftRecordHeader*)rec.data();
    if ((read_le16(&hdr->flags) & 0x0001) == 0) return "";  // deleted

    // Find $FILE_NAME attribute (type 0x30)
    const uint8_t* fn_attr = find_attr(rec.data(), rec.size(), 0x30);
    if (!fn_attr) return "";

    const NtfsAttrHeader* ah = (const NtfsAttrHeader*)fn_attr;
    const uint8_t* fn_body = fn_attr + ((ah->non_resident == 0) ?
                        read_le16(&((const NtfsResidentAttr*)(fn_attr + sizeof(NtfsAttrHeader)))->value_offset) :
                        sizeof(NtfsAttrHeader) + sizeof(NtfsNonResidentAttr));
    // For resident $FILE_NAME (always resident):
    const NtfsFileNameAttr* fn = (const NtfsFileNameAttr*)(fn_attr + sizeof(NtfsAttrHeader) + 2);  // value_offset=24 for resident
    // Actually, compute properly:
    uint16_t voff = 0;
    if (ah->non_resident == 0) {
        voff = read_le16(&((const NtfsResidentAttr*)(fn_attr + sizeof(NtfsAttrHeader)))->value_offset);
    }
    const NtfsFileNameAttr* fn2 = (const NtfsFileNameAttr*)(fn_attr + voff);
    uint64_t parent_mft = read_le64(&fn2->parent_directory) & 0xFFFFFFFFFFFF;
    std::string name = utf16le_to_utf8((const uint8_t*)&fn2->filename_length + 1,
                                          fn2->filename_length);

    if (parent_mft == mft_num || mft_num == 5) {
        // Root or self-referencing
        path_cache_[mft_num] = "/" + name;
        return "/" + name;
    }

    std::string parent_path = build_path(parent_mft);
    std::string full = parent_path + "/" + name;
    path_cache_[mft_num] = full;
    return full;
}

// ---- enumerate_files ----

bool NtfsReader::enumerate_files() {
    files_.clear();
    path_cache_.clear();

    // Iterate MFT records 0..~20 (typical small FS) or until we've seen enough
    // Real implementation: walk the MFT $BITMAP to find valid records
    // For simplicity: scan MFT records sequentially

    // Read $BITMAP for MFT (record 4) to know which records are in use
    // Simpler approach: try reading records and check FILE signature

    // We'll scan the first reasonable number of MFT records
    // A typical small FS has a few hundred files
    uint64_t max_records = 4096;  // generous for a USB stick

    for (uint64_t i = 0; i < max_records; i++) {
        std::vector<uint8_t> rec;
        if (!read_mft_record(i, rec)) continue;

        NtfsMftRecordHeader* hdr = (NtfsMftRecordHeader*)rec.data();
        if (std::memcmp(hdr->signature, "FILE", 4) != 0) continue;
        if ((read_le16(&hdr->flags) & 0x0001) == 0) continue;  // not in use

        // Skip system files (MFT number < 12)
        if (i < 12) continue;

        // Get $FILE_NAME
        const uint8_t* fn_attr = find_attr(rec.data(), rec.size(), 0x30);
        if (!fn_attr) continue;

        const NtfsAttrHeader* ah = (const NtfsAttrHeader*)fn_attr;
        uint16_t voff = 0;
        if (ah->non_resident == 0) {
            voff = read_le16(&((const NtfsResidentAttr*)(fn_attr + sizeof(NtfsAttrHeader)))->value_offset);
        }
        const NtfsFileNameAttr* fn = (const NtfsFileNameAttr*)(fn_attr + voff);

        uint64_t parent_mft = read_le64(&fn->parent_directory) & 0xFFFFFFFFFFFF;
        std::string name = utf16le_to_utf8((const uint8_t*)(fn_attr + voff + 66),
                                              fn->filename_length);
        // 66 = offsetof(NtfsFileNameAttr, filename_length) + 1 = 64 + 2
        // Actually: NtfsFileNameAttr is 66 bytes before the filename
        const uint8_t* fn_name_start = fn_attr + voff + sizeof(NtfsFileNameAttr);
        name = utf16le_to_utf8(fn_name_start, fn->filename_length);

        // Get file size from $DATA
        uint64_t file_size = 0;
        const uint8_t* data_attr = find_attr(rec.data(), rec.size(), 0x80);
        if (data_attr) {
            const NtfsAttrHeader* dah = (const NtfsAttrHeader*)data_attr;
            if (dah->non_resident) {
                file_size = read_le64(&((const NtfsNonResidentAttr*)(data_attr + sizeof(NtfsAttrHeader)))->data_size);
            } else {
                file_size = read_le32(&((const NtfsResidentAttr*)(data_attr + sizeof(NtfsAttrHeader)))->value_length);
            }
        }

        bool is_dir = (read_le16(&hdr->flags) & 0x0002) != 0;

        NtfsFileEntry entry;
        entry.mft_number = i;
        entry.parent_mft = parent_mft;
        entry.name      = name;
        entry.file_size = file_size;
        entry.is_dir    = is_dir;
        entry.is_system = (i < 12);

        // Build path
        entry.full_path = build_path(i);

        files_.push_back(entry);
    }

    fprintf(stderr, "[NTFS] Enumerated %zu files/dirs\n", files_.size());
    return true;
}

// ---- read_file ----

bool NtfsReader::read_file(const NtfsFileEntry& entry,
                                NtfsDataCallback cb,
                                size_t chunk_size) {
    // Find $DATA attribute in the MFT record
    std::vector<uint8_t> rec;
    if (!read_mft_record(entry.mft_number, rec)) return false;

    const uint8_t* data_attr = find_attr(rec.data(), rec.size(), 0x80);
    if (!data_attr) return false;

    const NtfsAttrHeader* ah = (const NtfsAttrHeader*)data_attr;
    if (ah->non_resident == 0) {
        // Resident: small file
        const NtfsResidentAttr* ra = (const NtfsResidentAttr*)(data_attr + sizeof(NtfsAttrHeader));
        uint32_t voff = read_le16(&ra->value_offset);
        uint32_t vlen = read_le32(&ra->value_length);
        return cb(0, data_attr + voff, vlen);
    }

    // Non-resident: read by clusters
    const NtfsNonResidentAttr* nra = (const NtfsNonResidentAttr*)(data_attr + sizeof(NtfsAttrHeader));
    uint64_t data_size = read_le64(&nra->data_size);
    const uint8_t* mapping = data_attr + read_le16(&nra->mapping_pairs_offset);

    std::vector<DataRun> runs;
    parse_data_runs(mapping, 512, runs);

    uint64_t offset = 0;
    uint64_t current_vcn = 0;

    for (const auto& run : runs) {
        uint64_t run_bytes = run.length * bytes_per_cluster_;

        if (offset >= data_size) break;

        std::vector<uint8_t> buf((size_t)std::min(run_bytes, data_size - offset));
        if (!read_clusters(run.lcn, buf.data(), (uint32_t)((buf.size() + bytes_per_cluster_ - 1) / bytes_per_cluster_))) {
            return false;
        }

        if (!cb(offset, buf.data(), buf.size())) return false;
        offset += buf.size();

        current_vcn += run.length;
    }
    return true;
}
