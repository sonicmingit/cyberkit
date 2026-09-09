#include "../../main/boards/CyberVoc-Board-V2_0/car_mode/mepack_reader.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

uint32_t Le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
           static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

void Put16(uint8_t* p, uint16_t value) {
    p[0] = static_cast<uint8_t>(value);
    p[1] = static_cast<uint8_t>(value >> 8);
}

void Put32(uint8_t* p, uint32_t value) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (i * 8));
}

void Put64(uint8_t* p, uint64_t value) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(value >> (i * 8));
}

void Rechecksum(std::vector<uint8_t>& eaf) {
    uint32_t sum = 0;
    for (size_t i = 16; i < eaf.size(); ++i) sum += eaf[i];
    Put32(eaf.data() + 8, sum);
}

int failures = 0;

void Check(const char* name, bool ok) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}

template <typename Mutator>
void RejectHeader(const char* name, const std::vector<uint8_t>& original, Mutator mutate) {
    auto bytes = original;
    mutate(bytes);
    mepack::Header header;
    std::string error;
    Check(name, !mepack::ParseHeader(bytes.data(), mepack::kHeaderSize, original.size(), header, error));
}

template <typename Mutator>
void RejectIndex(const char* name, const std::vector<uint8_t>& original, Mutator mutate) {
    auto bytes = original;
    mutate(bytes);
    mepack::StateRecord state;
    std::string error;
    Check(name, !mepack::ParseIndexEntry(bytes.data(), bytes.size(), state, error));
}

template <typename Mutator>
void RejectEaf(const char* name, const std::vector<uint8_t>& original,
               const mepack::StateRecord& state, Mutator mutate) {
    auto bytes = original;
    mutate(bytes);
    std::string error;
    Check(name, !mepack::ValidateEaf(bytes.data(), bytes.size(), state.width, state.height,
                                    state.frames, error));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: mepack_format_review PACKAGE\n");
        return 2;
    }
    std::ifstream stream(argv[1], std::ios::binary);
    std::vector<uint8_t> package((std::istreambuf_iterator<char>(stream)), {});
    Check("read reference package", package.size() == 5415432);
    if (package.size() < mepack::kHeaderSize) return 1;

    mepack::Header header;
    std::string error;
    Check("valid header and 19 states",
          mepack::ParseHeader(package.data(), mepack::kHeaderSize, package.size(), header, error) &&
          header.state_count == 19 && header.payload_offset == 8192);

    RejectHeader("reject bad magic", package, [](auto& b) { b[0] = 'X'; });
    RejectHeader("reject schema", package, [](auto& b) { Put16(b.data() + 4, 2); });
    RejectHeader("reject header length", package, [](auto& b) { Put16(b.data() + 6, 64); });
    RejectHeader("reject reserved bytes", package, [](auto& b) { b[116] = 1; });
    RejectHeader("reject state limit", package, [](auto& b) { Put32(b.data() + 16, 65); });
    RejectHeader("reject index mismatch", package, [](auto& b) { Put32(b.data() + 24, 1); });
    RejectHeader("reject payload alignment", package, [](auto& b) { Put64(b.data() + 28, 8193); });
    RejectHeader("reject total length mismatch", package, [](auto& b) { Put64(b.data() + 44, 1); });
    RejectHeader("reject payload overflow", package, [](auto& b) { Put64(b.data() + 36, UINT64_MAX); });

    const size_t index_offset = mepack::kHeaderSize + header.manifest_size;
    Check("index is in package", index_offset + header.index_size <= package.size());
    std::vector<uint8_t> index(package.begin() + index_offset,
                               package.begin() + index_offset + mepack::kIndexEntrySize);
    mepack::StateRecord state;
    error.clear();
    Check("valid production index entry",
          mepack::ParseIndexEntry(index.data(), index.size(), state, error));
    RejectIndex("reject truncated index", std::vector<uint8_t>(index.begin(), index.end() - 1),
                [](auto&) {});
    RejectIndex("reject unterminated state id", index, [](auto& b) {
        for (size_t i = 0; i < 32; ++i) b[i] = 'a';
    });
    RejectIndex("reject uppercase state id", index, [](auto& b) { b[0] = 'A'; });
    RejectIndex("reject unaligned state", index, [](auto& b) { Put64(b.data() + 32, 1); });
    RejectIndex("reject oversized state", index, [](auto& b) {
        Put64(b.data() + 40, mepack::kMaxStateSize + 1);
    });
    RejectIndex("reject unknown flags", index, [](auto& b) { Put16(b.data() + 82, 2); });
    RejectIndex("reject invalid fps", index, [](auto& b) { Put16(b.data() + 80, 0); });
    RejectIndex("reject invalid dimensions", index, [](auto& b) { Put16(b.data() + 84, 359); });
    RejectIndex("reject index reserved bytes", index, [](auto& b) { Put32(b.data() + 92, 1); });

    const uint64_t eaf_offset = header.payload_offset + state.offset;
    Check("state payload is in package", eaf_offset + state.size <= package.size());
    std::vector<uint8_t> eaf(package.begin() + static_cast<size_t>(eaf_offset),
                             package.begin() + static_cast<size_t>(eaf_offset + state.size));
    error.clear();
    Check("valid production EAF and RLE",
          mepack::ValidateEaf(eaf.data(), eaf.size(), state.width, state.height, state.frames, error));
    RejectEaf("reject bad EAF magic", eaf, state, [](auto& b) { b[0] = 0; });
    RejectEaf("reject EAF body length", eaf, state, [](auto& b) { Put32(b.data() + 12, 1); });
    RejectEaf("reject EAF checksum", eaf, state, [](auto& b) { b.back() ^= 1; });
    RejectEaf("reject frame index overflow", eaf, state, [](auto& b) {
        Put32(b.data() + 20, UINT32_MAX); Rechecksum(b);
    });
    RejectEaf("reject frame encoding", eaf, state, [](auto& b) {
        const size_t frame = 16 + Le32(b.data() + 4) * 8 + Le32(b.data() + 20);
        b[frame] = 'X'; Rechecksum(b);
    });
    RejectEaf("reject frame dimensions", eaf, state, [](auto& b) {
        const size_t frame = 16 + Le32(b.data() + 4) * 8 + Le32(b.data() + 20);
        Put16(b.data() + frame + 12, 359); Rechecksum(b);
    });
    RejectEaf("reject RLE block bounds", eaf, state, [](auto& b) {
        const size_t frame = 16 + Le32(b.data() + 4) * 8 + Le32(b.data() + 20);
        Put32(b.data() + frame + 20, UINT32_MAX); Rechecksum(b);
    });

    return failures ? 1 : 0;
}
