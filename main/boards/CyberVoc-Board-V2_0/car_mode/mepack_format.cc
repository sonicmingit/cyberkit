#include "mepack_reader.h"

#include <algorithm>
#include <cstring>

namespace mepack {
namespace {

constexpr uint16_t kSchema = 1;
constexpr uint16_t kLoopFlag = 1;

uint16_t Le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | static_cast<uint16_t>(p[1]) << 8;
}

uint32_t Le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
           static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

uint64_t Le64(const uint8_t* p) {
    return static_cast<uint64_t>(Le32(p)) |
           static_cast<uint64_t>(Le32(p + 4)) << 32;
}

bool AddWithin(uint64_t a, uint64_t b, uint64_t limit, uint64_t& sum) {
    if (a > limit || b > limit - a) return false;
    sum = a + b;
    return sum <= limit;
}

bool IsAsciiId(const std::string& value) {
    if (value.empty() || value.size() > 31 || value[0] < 'a' || value[0] > 'z') return false;
    for (char c : value) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

}  // namespace

bool ParseHeader(const uint8_t* bytes, size_t size, uint64_t actual_file_size,
                 Header& out, std::string& error) {
    if (!bytes || size < kHeaderSize) { error = "文件短于固定包头"; return false; }
    if (std::memcmp(bytes, "MEPK", 4) != 0 || Le16(bytes + 4) != kSchema ||
        Le16(bytes + 6) != kHeaderSize) { error = "魔数、版本或包头长度无效"; return false; }
    if (Le32(bytes + 8) != 0 || Le32(bytes + 20) != kIndexEntrySize) {
        error = "包含不支持的标志或索引长度"; return false;
    }
    for (size_t i = 116; i < kHeaderSize; ++i) {
        if (bytes[i] != 0) { error = "包头保留区非零"; return false; }
    }
    out.schema = Le16(bytes + 4);
    out.manifest_size = Le32(bytes + 12);
    out.state_count = Le32(bytes + 16);
    out.index_size = Le32(bytes + 24);
    out.payload_offset = Le64(bytes + 28);
    out.payload_size = Le64(bytes + 36);
    out.total_size = Le64(bytes + 44);
    std::copy(bytes + 52, bytes + 84, out.manifest_sha256.begin());
    if (out.manifest_size == 0 || out.manifest_size > kMaxManifestSize ||
        out.state_count == 0 || out.state_count > kMaxStates ||
        out.index_size != static_cast<uint64_t>(out.state_count) * kIndexEntrySize ||
        out.total_size != actual_file_size || out.payload_offset % 4096 != 0) {
        error = "包大小、状态数量或对齐无效"; return false;
    }
    uint64_t metadata_end = 0, payload_end = 0;
    if (!AddWithin(kHeaderSize, out.manifest_size, out.total_size, metadata_end) ||
        !AddWithin(metadata_end, out.index_size, out.total_size, metadata_end) ||
        metadata_end > out.payload_offset ||
        !AddWithin(out.payload_offset, out.payload_size, out.total_size, payload_end) ||
        payload_end != out.total_size) {
        error = "清单、索引或 payload 越界"; return false;
    }
    return true;
}

bool ParseIndexEntry(const uint8_t* bytes, size_t size, StateRecord& out,
                     std::string& error) {
    if (!bytes || size < kIndexEntrySize) { error = "状态索引被截断"; return false; }
    size_t id_length = 0;
    while (id_length < 32 && bytes[id_length] != 0) ++id_length;
    if (id_length == 32) { error = "状态 ID 未终止"; return false; }
    out.id.assign(reinterpret_cast<const char*>(bytes), id_length);
    out.offset = Le64(bytes + 32);
    out.size = Le64(bytes + 40);
    std::copy(bytes + 48, bytes + 80, out.sha256.begin());
    out.fps = Le16(bytes + 80);
    const uint16_t flags = Le16(bytes + 82);
    out.loop = (flags & kLoopFlag) != 0;
    out.width = Le16(bytes + 84);
    out.height = Le16(bytes + 86);
    out.frames = Le32(bytes + 88);
    if (!IsAsciiId(out.id) || (flags & ~kLoopFlag) != 0 || Le32(bytes + 92) != 0 ||
        out.offset % 4 != 0 || out.size == 0 || out.size > kMaxStateSize ||
        out.fps == 0 || out.fps > 63 || out.width != 360 || out.height != 360 ||
        out.frames == 0) {
        error = "状态索引字段无效"; return false;
    }
    return true;
}

bool ValidateEaf(const uint8_t* bytes, size_t size, uint16_t width,
                 uint16_t height, uint32_t expected_frames, std::string& error) {
    if (!bytes || size < 16 || std::memcmp(bytes, "\x89" "EAF", 4) != 0) {
        error = "EAF 文件头无效"; return false;
    }
    const uint32_t count = Le32(bytes + 4);
    const uint32_t checksum = Le32(bytes + 8);
    const uint32_t body_size = Le32(bytes + 12);
    if (count == 0 || count != expected_frames || body_size != size - 16 ||
        count > (size - 16) / 8) { error = "EAF 帧数或长度无效"; return false; }
    uint32_t actual_checksum = 0;
    for (size_t i = 16; i < size; ++i) actual_checksum += bytes[i];
    if (actual_checksum != checksum) { error = "EAF 校验和无效"; return false; }
    const uint64_t payload_base = 16 + static_cast<uint64_t>(count) * 8;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t frame_size = Le32(bytes + 16 + i * 8);
        const uint32_t frame_offset = Le32(bytes + 20 + i * 8);
        uint64_t frame_start = 0, frame_end = 0;
        if (!AddWithin(payload_base, frame_offset, size, frame_start) ||
            !AddWithin(frame_start, frame_size, size, frame_end) || frame_size < 30) {
            error = "EAF 帧索引越界"; return false;
        }
        const uint8_t* frame = bytes + frame_start;
        if (frame[0] != 'Z' || frame[1] != 'Z' || frame[2] != '_' || frame[3] != 'S' || frame[11] != 8) {
            error = "EAF 帧编码无效"; return false;
        }
        const uint16_t frame_width = Le16(frame + 12);
        const uint16_t frame_height = Le16(frame + 14);
        const uint16_t blocks = Le16(frame + 16);
        const uint16_t block_height = Le16(frame + 18);
        if (frame_width != width || frame_height != height || block_height == 0 ||
            blocks != (height + block_height - 1) / block_height) {
            error = "EAF 帧尺寸或分块无效"; return false;
        }
        const uint64_t block_table_end = 20 + static_cast<uint64_t>(blocks) * 4;
        const uint64_t block_data_start = block_table_end + 256 * 4;
        if (block_data_start > frame_size) {
            error = "EAF 调色板或分块表越界"; return false;
        }
        uint64_t block_cursor = block_data_start;
        for (uint16_t block_index = 0; block_index < blocks; ++block_index) {
            const uint32_t block_size = Le32(frame + 20 + static_cast<size_t>(block_index) * 4);
            uint64_t block_end = 0;
            if (block_size == 0 || (block_size - 1) % 2 != 0 ||
                !AddWithin(block_cursor, block_size, frame_size, block_end)) {
                error = "EAF RLE 分块越界"; return false;
            }
            const uint8_t* block = frame + block_cursor;
            if (block[0] != 0) { error = "EAF RLE 分块标志无效"; return false; }
            uint64_t pixels = 0;
            for (uint32_t pos = 1; pos < block_size; pos += 2) pixels += block[pos];
            const uint32_t rows = std::min<uint32_t>(block_height,
                                                     height - block_index * block_height);
            if (pixels != static_cast<uint64_t>(width) * rows) {
                error = "EAF RLE 像素数无效"; return false;
            }
            block_cursor = block_end;
        }
        if (block_cursor != frame_size) { error = "EAF 帧存在尾部数据"; return false; }
    }
    return true;
}

}  // namespace mepack
