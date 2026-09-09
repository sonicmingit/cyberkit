#include "mepack_reader.h"

#include "cJSON.h"
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sys/stat.h>

namespace mepack {
namespace {

constexpr char kTag[] = "mepack";
constexpr uint16_t kSchema = 1;

bool AddWithin(uint64_t a, uint64_t b, uint64_t limit, uint64_t& sum) {
    if (a > limit || b > limit - a) return false;
    sum = a + b;
    return sum <= limit;
}

bool Sha256(const uint8_t* data, size_t size, std::array<uint8_t, 32>& digest) {
    return mbedtls_sha256(data, size, digest.data(), 0) == 0;
}

bool ReadExact(FILE* file, void* data, size_t size) {
    return size == 0 || std::fread(data, 1, size, file) == size;
}

bool IsAsciiId(const std::string& value) {
    if (value.empty() || value.size() > 31 || value[0] < 'a' || value[0] > 'z') return false;
    for (char c : value) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

bool JsonString(cJSON* object, const char* key, std::string& out, size_t max_bytes) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    const size_t length = std::strlen(item->valuestring);
    if (length == 0 || length > max_bytes) return false;
    out.assign(item->valuestring, length);
    return true;
}

bool JsonUInt(cJSON* object, const char* key, uint64_t& out) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 ||
        item->valuedouble > static_cast<double>(std::numeric_limits<uint64_t>::max())) return false;
    const uint64_t value = static_cast<uint64_t>(item->valuedouble);
    if (static_cast<double>(value) != item->valuedouble) return false;
    out = value;
    return true;
}

bool HexDigest(const char* text, std::array<uint8_t, 32>& out) {
    if (!text || std::strlen(text) != 64) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i < out.size(); ++i) {
        const int hi = nibble(text[i * 2]);
        const int lo = nibble(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

std::string BaseName(const std::string& path) {
    const size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

bool VersionAtLeast(const char* current, const char* minimum) {
    if (!minimum || !minimum[0]) return true;
    auto parse = [](const char* text, std::array<uint32_t, 4>& parts) {
        if (!text || !text[0]) return false;
        const char* cursor = text;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (*cursor < '0' || *cursor > '9') return false;
            uint64_t value = 0;
            while (*cursor >= '0' && *cursor <= '9') {
                value = value * 10 + static_cast<unsigned>(*cursor - '0');
                if (value > UINT32_MAX) return false;
                ++cursor;
            }
            parts[i] = static_cast<uint32_t>(value);
            if (*cursor == '\0') return true;
            if (*cursor != '.' || cursor[1] == '\0') return false;
            ++cursor;
        }
        return *cursor == '\0';
    };
    std::array<uint32_t, 4> current_parts{}, minimum_parts{};
    if (!parse(current, current_parts) || !parse(minimum, minimum_parts)) return false;
    for (size_t part = 0; part < current_parts.size(); ++part) {
        if (current_parts[part] != minimum_parts[part]) {
            return current_parts[part] > minimum_parts[part];
        }
    }
    return true;
}

bool SameRecord(cJSON* json, const StateRecord& record) {
    uint64_t offset = 0, size = 0, width = 0, height = 0, frames = 0, fps = 0;
    std::string id, source, hash, end_behavior;
    cJSON* loop = cJSON_GetObjectItemCaseSensitive(json, "loop");
    return JsonString(json, "id", id, 31) && id == record.id &&
           JsonString(json, "source", source, 63) && source == "car_" + record.id + ".eaf" &&
           JsonString(json, "sha256", hash, 64) &&
           JsonUInt(json, "offset", offset) && offset == record.offset &&
           JsonUInt(json, "size", size) && size == record.size &&
           JsonUInt(json, "width", width) && width == record.width &&
           JsonUInt(json, "height", height) && height == record.height &&
           JsonUInt(json, "frames", frames) && frames == record.frames &&
           JsonUInt(json, "fps", fps) && fps == record.fps &&
           JsonString(json, "end_behavior", end_behavior, 31) &&
           end_behavior == (record.loop ? "loop" : "return_to_current_state") &&
           cJSON_IsBool(loop) && cJSON_IsTrue(loop) == record.loop &&
           [&]() { std::array<uint8_t, 32> parsed{}; return HexDigest(hash.c_str(), parsed) && parsed == record.sha256; }();
}

const StateRecord* FindState(const Descriptor& descriptor, const std::string& state) {
    auto it = std::find_if(descriptor.states.begin(), descriptor.states.end(),
                           [&](const StateRecord& item) { return item.id == state; });
    if (it != descriptor.states.end()) return &*it;
    if (state != descriptor.fallback_state) {
        it = std::find_if(descriptor.states.begin(), descriptor.states.end(),
                          [&](const StateRecord& item) { return item.id == descriptor.fallback_state; });
        if (it != descriptor.states.end()) return &*it;
    }
    return nullptr;
}

}  // namespace

bool ReadDescriptor(const std::string& path, Descriptor& out, std::string& error) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < static_cast<off_t>(kHeaderSize)) {
        error = "无法读取普通文件"; return false;
    }
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) { error = "打开文件失败"; return false; }
    std::array<uint8_t, kHeaderSize> raw_header{};
    Header header;
    bool ok = ReadExact(file, raw_header.data(), raw_header.size()) &&
              ParseHeader(raw_header.data(), raw_header.size(), st.st_size, header, error);
    if (!ok) {
        if (error.empty()) error = "读取包头失败";
        std::fclose(file); return false;
    }

    std::vector<uint8_t> manifest(header.manifest_size + 1, 0);
    std::vector<uint8_t> index(header.index_size);
    if (!ReadExact(file, manifest.data(), header.manifest_size) || !ReadExact(file, index.data(), index.size())) {
        error = "清单或索引短读"; std::fclose(file); return false;
    }
    std::array<uint8_t, 32> manifest_hash{};
    if (!Sha256(manifest.data(), header.manifest_size, manifest_hash) || manifest_hash != header.manifest_sha256) {
        error = "清单 SHA-256 无效"; std::fclose(file); return false;
    }
    const uint64_t metadata_end = kHeaderSize + header.manifest_size + header.index_size;
    for (uint64_t pos = metadata_end; pos < header.payload_offset; ++pos) {
        if (std::fgetc(file) != 0) { error = "元数据填充非零"; std::fclose(file); return false; }
    }
    std::fclose(file);

    const char* parse_end = nullptr;
    const char* manifest_text = reinterpret_cast<const char*>(manifest.data());
    cJSON* root = cJSON_ParseWithLengthOpts(manifest_text, header.manifest_size + 1,
                                           &parse_end, true);
    if (!root || parse_end != manifest_text + header.manifest_size) {
        if (root) cJSON_Delete(root);
        error = "清单 JSON 无效或包含尾随数据";
        return false;
    }
    auto cleanup = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>(root, cJSON_Delete);
    cJSON* format = cJSON_GetObjectItemCaseSensitive(root, "format");
    cJSON* schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    cJSON* package = cJSON_GetObjectItemCaseSensitive(root, "package");
    cJSON* compatibility = cJSON_GetObjectItemCaseSensitive(root, "compatibility");
    cJSON* fallback = cJSON_GetObjectItemCaseSensitive(root, "fallback");
    cJSON* signature = cJSON_GetObjectItemCaseSensitive(root, "signature");
    cJSON* states = cJSON_GetObjectItemCaseSensitive(root, "states");
    if (!cJSON_IsString(format) || std::strcmp(format->valuestring, "mepack") != 0 ||
        !cJSON_IsNumber(schema) || schema->valueint != kSchema || !cJSON_IsObject(package) ||
        !cJSON_IsObject(compatibility) || !cJSON_IsObject(fallback) || !cJSON_IsObject(signature) ||
        !cJSON_IsArray(states) ||
        cJSON_GetArraySize(states) != static_cast<int>(header.state_count)) {
        error = "清单根字段无效"; return false;
    }

    Descriptor descriptor;
    uint64_t version = 0, display_width = 0, display_height = 0, loader_schema = 0;
    std::string board, encoding, minimum, signature_algorithm, signature_status;
    if (!JsonString(package, "number", descriptor.number, 3) || descriptor.number.size() != 3 ||
        !std::all_of(descriptor.number.begin(), descriptor.number.end(),
                     [](char c) { return c >= '0' && c <= '9'; }) ||
        !JsonString(package, "pack_id", descriptor.pack_id, 31) || !IsAsciiId(descriptor.pack_id) ||
        !JsonString(package, "display_name", descriptor.display_name, 63) ||
        !JsonUInt(package, "version", version) || version < 1 || version > INT32_MAX ||
        !JsonString(compatibility, "board", board, 63) || board != "CyberVoc-Board-V2_0" ||
        !JsonUInt(compatibility, "display_width", display_width) || display_width != 360 ||
        !JsonUInt(compatibility, "display_height", display_height) || display_height != 360 ||
        !JsonUInt(compatibility, "loader_schema", loader_schema) || loader_schema != kSchema ||
        !JsonString(compatibility, "eaf_encoding", encoding, 63) ||
        encoding != "EAF 8-bit indexed BGRA palette + RLE" ||
        !JsonString(fallback, "default_state", descriptor.fallback_state, 31) ||
        descriptor.fallback_state != "cruise" ||
        !JsonString(signature, "algorithm", signature_algorithm, 15) || signature_algorithm != "none" ||
        !JsonString(signature, "status", signature_status, 15) || signature_status != "unsigned") {
        error = "包身份或兼容性无效"; return false;
    }
    cJSON* min_fw = cJSON_GetObjectItemCaseSensitive(compatibility, "min_firmware");
    if (cJSON_IsString(min_fw) && min_fw->valuestring) {
        if (!JsonString(compatibility, "min_firmware", minimum, 31)) {
            error = "最低固件版本字段无效"; return false;
        }
        if (!VersionAtLeast(esp_app_get_description()->version, minimum.c_str())) {
            error = "固件版本低于表情包要求"; return false;
        }
    } else if (!cJSON_IsNull(min_fw)) {
        error = "最低固件版本字段无效"; return false;
    }
    descriptor.version = static_cast<int>(version);
    descriptor.file_path = path;
    descriptor.payload_offset = header.payload_offset;
    descriptor.file_size = header.total_size;
    const std::string expected_name = descriptor.number + "-" + descriptor.pack_id + "-v" +
                                      std::to_string(descriptor.version) + ".mepack";
    if (BaseName(path) != expected_name) { error = "文件名与包身份不一致"; return false; }

    descriptor.states.reserve(header.state_count);
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    for (uint32_t i = 0; i < header.state_count; ++i) {
        StateRecord record;
        if (!ParseIndexEntry(index.data() + i * kIndexEntrySize, kIndexEntrySize, record, error)) return false;
        uint64_t end = 0;
        if (!AddWithin(record.offset, record.size, header.payload_size, end)) {
            error = "状态 payload 越界"; return false;
        }
        if (std::any_of(descriptor.states.begin(), descriptor.states.end(),
                        [&](const StateRecord& item) { return item.id == record.id; })) {
            error = "状态 ID 重复"; return false;
        }
        cJSON* manifest_record = nullptr;
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, states) {
            cJSON* id = cJSON_GetObjectItemCaseSensitive(item, "id");
            if (cJSON_IsString(id) && id->valuestring && record.id == id->valuestring) {
                manifest_record = item; break;
            }
        }
        if (!manifest_record || !SameRecord(manifest_record, record)) {
            error = "清单与固定索引不一致"; return false;
        }
        ranges.emplace_back(record.offset, end);
        descriptor.states.push_back(std::move(record));
    }
    std::sort(ranges.begin(), ranges.end());
    for (size_t i = 1; i < ranges.size(); ++i) {
        if (ranges[i].first < ranges[i - 1].second) { error = "状态 payload 重叠"; return false; }
    }
    for (const char* required : {"cruise", "idle_01", "idle_02", "idle_03"}) {
        if (!FindState(descriptor, required) ||
            std::none_of(descriptor.states.begin(), descriptor.states.end(),
                         [&](const StateRecord& item) { return item.id == required; })) {
            cJSON* legacy = cJSON_GetObjectItemCaseSensitive(root, "legacy_missing_idle_allowed");
            if (!cJSON_IsTrue(legacy)) { error = "缺少必备状态"; return false; }
            break;
        }
    }
    out = std::move(descriptor);
    return true;
}

bool LoadState(const Descriptor& descriptor, const std::string& requested,
               OwnedEafBuffer& out, std::string& error) {
    const StateRecord* record = FindState(descriptor, requested);
    if (!record) { error = "目标状态和巡航回退均不存在"; return false; }
    FILE* file = std::fopen(descriptor.file_path.c_str(), "rb");
    if (!file) { error = "表情包已不可访问"; return false; }
    const uint64_t absolute_offset = descriptor.payload_offset + record->offset;
    if (absolute_offset > LONG_MAX || std::fseek(file, static_cast<long>(absolute_offset), SEEK_SET) != 0) {
        error = "定位状态失败"; std::fclose(file); return false;
    }
    const size_t before_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t before_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    uint8_t* raw = static_cast<uint8_t*>(heap_caps_malloc(static_cast<size_t>(record->size),
                                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!raw) {
        error = "PSRAM 空间不足"; std::fclose(file);
        ESP_LOGE(kTag, "PSRAM allocation failed: need=%llu free=%u largest=%u",
                 static_cast<unsigned long long>(record->size), static_cast<unsigned>(before_free),
                 static_cast<unsigned>(before_largest));
        return false;
    }
    std::shared_ptr<uint8_t> owned(raw, [](uint8_t* p) { heap_caps_free(p); });
    const bool read_ok = ReadExact(file, raw, static_cast<size_t>(record->size));
    std::fclose(file);
    if (!read_ok) { error = "读取状态时发生短读"; return false; }
    std::array<uint8_t, 32> digest{};
    if (!Sha256(raw, static_cast<size_t>(record->size), digest) || digest != record->sha256) {
        error = "状态 SHA-256 校验失败"; return false;
    }
    if (!ValidateEaf(raw, static_cast<size_t>(record->size), record->width, record->height,
                     record->frames, error)) return false;
    out.data = std::move(owned);
    out.size = static_cast<size_t>(record->size);
    out.fps = static_cast<uint8_t>(record->fps);
    out.loop = record->loop;
    ESP_LOGI(kTag, "Loaded %s/%s: %u bytes, PSRAM free %u -> %u, largest=%u",
             descriptor.pack_id.c_str(), record->id.c_str(), static_cast<unsigned>(out.size),
             static_cast<unsigned>(before_free),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    return true;
}

}  // namespace mepack
