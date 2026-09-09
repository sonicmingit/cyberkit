#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mepack {

constexpr size_t kHeaderSize = 128;
constexpr size_t kIndexEntrySize = 96;
constexpr size_t kMaxManifestSize = 64 * 1024;
constexpr size_t kMaxStates = 64;
constexpr size_t kMaxStateSize = 2 * 1024 * 1024;

struct Header {
    uint16_t schema = 0;
    uint32_t manifest_size = 0;
    uint32_t state_count = 0;
    uint32_t index_size = 0;
    uint64_t payload_offset = 0;
    uint64_t payload_size = 0;
    uint64_t total_size = 0;
    std::array<uint8_t, 32> manifest_sha256{};
};

struct StateRecord {
    std::string id;
    uint64_t offset = 0;
    uint64_t size = 0;
    std::array<uint8_t, 32> sha256{};
    uint16_t fps = 0;
    bool loop = false;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t frames = 0;
};

struct Descriptor {
    std::string number;
    std::string pack_id;
    std::string display_name;
    int version = 0;
    std::string file_path;
    std::string fallback_state = "cruise";
    uint64_t payload_offset = 0;
    uint64_t file_size = 0;
    std::vector<StateRecord> states;
};

struct OwnedEafBuffer {
    std::shared_ptr<uint8_t> data;
    size_t size = 0;
    uint8_t fps = 0;
    bool loop = false;
};

bool ParseHeader(const uint8_t* bytes, size_t size, uint64_t actual_file_size,
                 Header& out, std::string& error);
bool ParseIndexEntry(const uint8_t* bytes, size_t size, StateRecord& out,
                     std::string& error);
bool ValidateEaf(const uint8_t* bytes, size_t size, uint16_t width,
                 uint16_t height, uint32_t expected_frames, std::string& error);

// Reads only bounded metadata (header, manifest and index). Payload data is
// verified per state immediately before playback.
bool ReadDescriptor(const std::string& path, Descriptor& out, std::string& error);
bool LoadState(const Descriptor& descriptor, const std::string& state,
               OwnedEafBuffer& out, std::string& error);

}  // namespace mepack
