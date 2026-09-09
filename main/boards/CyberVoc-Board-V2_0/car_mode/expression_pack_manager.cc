#include "expression_pack_manager.h"

#include "../CyberVoc.h"
#include "application.h"
#include "display/emote_display.h"

#include <dirent.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace expression_packs {
namespace {

constexpr char kTag[] = "expression_packs";
constexpr char kPackDirectory[] = "/sdcard/miaoban/packs";
constexpr size_t kMaxTfPacks = 32;

struct Identity {
    Source source = Source::Builtin;
    std::string pack_id = "default";
    int version = 1;
};

struct Pack {
    UiDescriptor ui;
    mepack::Descriptor tf;
};

struct Request {
    uint64_t generation = 0;
    Identity identity;
    std::string state = "cruise";
    std::string debug;
    bool persist_after_success = false;
    bool display_after_load = true;
};

std::mutex mutex;
std::vector<Pack> packs;
Identity saved_target;
Identity active;
Request desired;
TaskHandle_t worker_task = nullptr;
bool tf_mounted = false;
bool scan_requested = false;
std::atomic<bool> scanning{false};
std::atomic<int> invalid_count{0};
std::atomic<uint32_t> revision{0};
char status_text[96] = "尚未扫描 TF 卡";
bool playback_stopped = true;

bool Same(const Identity& a, const Identity& b) {
    return a.source == b.source && a.pack_id == b.pack_id && a.version == b.version;
}

Identity IdentityOf(const UiDescriptor& ui) {
    return {ui.source, ui.pack_id, ui.version};
}

void CopyText(char* dst, size_t size, const std::string& value) {
    if (!dst || size == 0) return;
    std::snprintf(dst, size, "%s", value.c_str());
}

Pack Builtin(const char* id, const char* name) {
    Pack pack;
    pack.ui.source = Source::Builtin;
    CopyText(pack.ui.pack_id, sizeof(pack.ui.pack_id), id);
    CopyText(pack.ui.display_name, sizeof(pack.ui.display_name), name);
    pack.ui.version = 1;
    return pack;
}

bool SaveIdentity(const Identity& identity) {
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open("car_mode", NVS_READWRITE, &handle);
    if (err == ESP_OK) err = nvs_set_str(handle, "pack_source", identity.source == Source::Tf ? "tf" : "builtin");
    if (err == ESP_OK) err = nvs_set_str(handle, "pack_id", identity.pack_id.c_str());
    if (err == ESP_OK) err = nvs_set_i32(handle, "pack_version", identity.version);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle) nvs_close(handle);
    if (err != ESP_OK) ESP_LOGE(kTag, "Cannot save pack identity: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

Identity LoadIdentity() {
    Identity result;
    nvs_handle_t handle = 0;
    if (nvs_open("car_mode", NVS_READONLY, &handle) != ESP_OK) return result;
    char source[12]{};
    char id[32]{};
    size_t source_size = sizeof(source), id_size = sizeof(id);
    int32_t version = 1;
    const bool has_new = nvs_get_str(handle, "pack_source", source, &source_size) == ESP_OK &&
                         nvs_get_str(handle, "pack_id", id, &id_size) == ESP_OK &&
                         nvs_get_i32(handle, "pack_version", &version) == ESP_OK;
    if (has_new && (std::strcmp(source, "builtin") == 0 || std::strcmp(source, "tf") == 0) &&
        id[0] && version > 0) {
        result.source = std::strcmp(source, "tf") == 0 ? Source::Tf : Source::Builtin;
        result.pack_id = id;
        result.version = version;
    } else {
        int32_t legacy = 1;
        if (nvs_get_i32(handle, "pack", &legacy) != ESP_OK) legacy = 1;
        result = {Source::Builtin, legacy == 0 ? "tita" : "default", 1};
    }
    nvs_close(handle);
    if (!has_new) SaveIdentity(result);
    return result;
}

bool ValidFileName(const char* name) {
    if (!name || name[0] == '.' || std::strlen(name) > 95) return false;
    const std::string value(name);
    if (value.size() < 15 || value.substr(value.size() - 7) != ".mepack") return false;
    if (!(value[0] >= '0' && value[0] <= '9' && value[1] >= '0' && value[1] <= '9' &&
          value[2] >= '0' && value[2] <= '9' && value[3] == '-')) return false;
    const size_t marker = value.rfind("-v");
    if (marker == std::string::npos || marker <= 4 || marker + 2 >= value.size() - 7) return false;
    for (size_t i = 4; i < marker; ++i) {
        const char c = value[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    for (size_t i = marker + 2; i < value.size() - 7; ++i) {
        if (value[i] < '0' || value[i] > '9') return false;
    }
    return true;
}

void MarkSelectionLocked() {
    for (auto& pack : packs) pack.ui.selected = Same(IdentityOf(pack.ui), saved_target);
}

const Pack* FindLocked(const Identity& identity) {
    auto it = std::find_if(packs.begin(), packs.end(),
                           [&](const Pack& pack) { return Same(IdentityOf(pack.ui), identity); });
    return it == packs.end() ? nullptr : &*it;
}

void SetStatusLocked(const char* text) {
    std::snprintf(status_text, sizeof(status_text), "%s", text ? text : "");
    revision.fetch_add(1);
}

void ScanTf() {
    scanning.store(true);
    revision.fetch_add(1);
    std::vector<Pack> discovered;
    int rejected = 0;
    if (!tf_mounted) {
        auto* board = dynamic_cast<EspS3Cat*>(&Board::GetInstance());
        tf_mounted = board && board->EnsureTfCardMounted();
    }
    if (!tf_mounted) {
        std::lock_guard<std::mutex> lock(mutex);
        packs.erase(std::remove_if(packs.begin(), packs.end(),
                                   [](const Pack& item) { return item.ui.source == Source::Tf; }), packs.end());
        invalid_count.store(0);
        SetStatusLocked("未检测到 TF 卡；内置表情可正常使用");
        scanning.store(false);
        return;
    }
    DIR* dir = opendir(kPackDirectory);
    if (!dir) {
        std::lock_guard<std::mutex> lock(mutex);
        packs.erase(std::remove_if(packs.begin(), packs.end(),
                                   [](const Pack& item) { return item.ui.source == Source::Tf; }), packs.end());
        invalid_count.store(0);
        SetStatusLocked("未找到 /miaoban/packs；请创建目录后重新扫描");
        scanning.store(false);
        return;
    }
    while (dirent* entry = readdir(dir)) {
        if (discovered.size() >= kMaxTfPacks) { ++rejected; continue; }
        if (!ValidFileName(entry->d_name)) continue;
#ifdef DT_LNK
        if (entry->d_type == DT_LNK) { ++rejected; continue; }
#endif
#ifdef DT_DIR
        if (entry->d_type == DT_DIR) { ++rejected; continue; }
#endif
        std::string path = std::string(kPackDirectory) + "/" + entry->d_name;
        struct stat st{};
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) { ++rejected; continue; }
        mepack::Descriptor descriptor;
        std::string error;
        if (!mepack::ReadDescriptor(path, descriptor, error)) {
            ++rejected;
            ESP_LOGW(kTag, "Ignoring %s: %s", entry->d_name, error.c_str());
            continue;
        }
        Pack pack;
        pack.ui.source = Source::Tf;
        CopyText(pack.ui.pack_id, sizeof(pack.ui.pack_id), descriptor.pack_id);
        CopyText(pack.ui.display_name, sizeof(pack.ui.display_name), descriptor.display_name);
        pack.ui.version = descriptor.version;
        pack.tf = std::move(descriptor);
        discovered.push_back(std::move(pack));
    }
    closedir(dir);

    std::sort(discovered.begin(), discovered.end(), [](const Pack& a, const Pack& b) {
        const int name = std::strcmp(a.ui.display_name, b.ui.display_name);
        if (name != 0) return name < 0;
        if (std::strcmp(a.ui.pack_id, b.ui.pack_id) != 0) return std::strcmp(a.ui.pack_id, b.ui.pack_id) < 0;
        return a.ui.version > b.ui.version;
    });
    std::vector<Pack> unique;
    unique.reserve(discovered.size());
    for (size_t i = 0; i < discovered.size(); ++i) {
        const size_t matches = std::count_if(discovered.begin(), discovered.end(), [&](const Pack& item) {
            return std::strcmp(discovered[i].ui.pack_id, item.ui.pack_id) == 0 &&
                   discovered[i].ui.version == item.ui.version;
        });
        if (matches > 1) {
            ++rejected;  // Every conflicting file is rejected, regardless of directory order.
        } else {
            unique.push_back(std::move(discovered[i]));
        }
    }
    discovered = std::move(unique);

    {
        std::lock_guard<std::mutex> lock(mutex);
        packs.erase(std::remove_if(packs.begin(), packs.end(),
                                   [](const Pack& item) { return item.ui.source == Source::Tf; }), packs.end());
        packs.insert(packs.end(), std::make_move_iterator(discovered.begin()),
                     std::make_move_iterator(discovered.end()));
        invalid_count.store(rejected);
        MarkSelectionLocked();
        char text[96];
        const size_t valid = std::count_if(packs.begin(), packs.end(),
                                           [](const Pack& item) { return item.ui.source == Source::Tf; });
        std::snprintf(text, sizeof(text), "已发现 %u 个 TF 表情包%s", static_cast<unsigned>(valid),
                      rejected ? ("，已忽略 " + std::to_string(rejected) + " 个无效包").c_str() : "");
        SetStatusLocked(text);
        if (!playback_stopped && saved_target.source == Source::Tf && FindLocked(saved_target)) {
            desired.identity = saved_target;
            desired.persist_after_success = false;
            ++desired.generation;
            playback_stopped = false;
        }
    }
    scanning.store(false);
    revision.fetch_add(1);
}

std::string BuiltinName(const Identity& identity, const std::string& state) {
    const std::string id = identity.pack_id == "tita" ? "tita" : "default";
    return "car_" + id + "_" + state;
}

void CommitBuiltin(const Request& request) {
    if (!request.display_after_load) {
        std::lock_guard<std::mutex> lock(mutex);
        if (request.generation != desired.generation) return;
        if (request.persist_after_success) {
            if (!SaveIdentity(request.identity)) return;
            saved_target = request.identity;
        }
        MarkSelectionLocked();
        SetStatusLocked("表情包切换成功；返回桌面后生效");
        return;
    }
    Application::GetInstance().Schedule([request]() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (request.generation != desired.generation) return;
        }
        auto* display = dynamic_cast<emote::EmoteDisplay*>(Board::GetInstance().GetDisplay());
        if (!display) return;
        if (display->ShowCarEmotion(BuiltinName(request.identity, request.state).c_str(),
                                    request.debug.empty() ? nullptr : request.debug.c_str())) {
            std::lock_guard<std::mutex> lock(mutex);
            if (request.generation != desired.generation) return;
            active = request.identity;
            playback_stopped = false;
            if (request.persist_after_success && SaveIdentity(request.identity)) saved_target = request.identity;
            MarkSelectionLocked();
            SetStatusLocked("表情包切换成功");
        }
    });
}

void CommitTf(const Request& request, mepack::OwnedEafBuffer buffer, std::string loaded_state) {
    if (!request.display_after_load) {
        std::lock_guard<std::mutex> lock(mutex);
        if (request.generation != desired.generation) return;
        if (request.persist_after_success) {
            if (!SaveIdentity(request.identity)) return;
            saved_target = request.identity;
        }
        MarkSelectionLocked();
        SetStatusLocked("TF 表情包校验成功；返回桌面后生效");
        return;
    }
    Application::GetInstance().Schedule([request, buffer = std::move(buffer), loaded_state = std::move(loaded_state)]() mutable {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (request.generation != desired.generation) return;
        }
        auto* display = dynamic_cast<emote::EmoteDisplay*>(Board::GetInstance().GetDisplay());
        if (!display) return;
        const std::string animation_id = "car_tf_" + request.identity.pack_id + "_v" +
                                         std::to_string(request.identity.version) + "_" + loaded_state + "_" +
                                         std::to_string(request.generation);
        if (display->ShowOwnedCarEmotion(animation_id.c_str(), buffer.data, buffer.size, buffer.fps,
                                         buffer.loop, request.debug.empty() ? nullptr : request.debug.c_str())) {
            std::lock_guard<std::mutex> lock(mutex);
            if (request.generation != desired.generation) return;
            active = request.identity;
            playback_stopped = false;
            if (request.persist_after_success && SaveIdentity(request.identity)) saved_target = request.identity;
            MarkSelectionLocked();
            SetStatusLocked("TF 表情包加载成功");
        }
    });
}

void ProcessRequest(const Request& request) {
    if (request.identity.source == Source::Builtin) {
        CommitBuiltin(request);
        return;
    }
    mepack::Descriptor descriptor;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const Pack* pack = FindLocked(request.identity);
        if (pack) descriptor = pack->tf;
    }
    if (descriptor.file_path.empty()) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (request.generation == desired.generation) {
                desired.identity = saved_target;
                desired.persist_after_success = false;
                SetStatusLocked(request.display_after_load ?
                                "所选 TF 包当前不可用，已使用内置默认表情" :
                                "所选 TF 包当前不可用；选择未更改");
            }
        }
        if (request.display_after_load) {
            Request fallback = request;
            fallback.identity = {Source::Builtin, "default", 1};
            fallback.persist_after_success = false;
            CommitBuiltin(fallback);
        }
        return;
    }
    mepack::OwnedEafBuffer buffer;
    std::string error;
    std::string loaded_state = request.state;
    if (!mepack::LoadState(descriptor, request.state, buffer, error)) {
        ESP_LOGW(kTag, "Cannot load %s/%s: %s", descriptor.pack_id.c_str(), request.state.c_str(), error.c_str());
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (request.generation == desired.generation) {
                desired.identity = saved_target;
                desired.persist_after_success = false;
                std::string message = "加载失败：" + error +
                                      (request.display_after_load ? "；已回退内置表情" :
                                                                    "；选择未更改");
                SetStatusLocked(message.c_str());
            }
        }
        if (request.display_after_load) {
            Request fallback = request;
            fallback.identity = {Source::Builtin, "default", 1};
            fallback.persist_after_success = false;
            CommitBuiltin(fallback);
        }
        return;
    }
    if (std::none_of(descriptor.states.begin(), descriptor.states.end(),
                     [&](const mepack::StateRecord& item) { return item.id == request.state; })) {
        loaded_state = descriptor.fallback_state;
    }
    CommitTf(request, std::move(buffer), std::move(loaded_state));
}

void Worker(void*) {
    uint64_t processed_generation = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        bool do_scan = false;
        Request request;
        {
            std::lock_guard<std::mutex> lock(mutex);
            do_scan = scan_requested;
            scan_requested = false;
        }
        if (do_scan) ScanTf();
        {
            std::lock_guard<std::mutex> lock(mutex);
            request = desired;
        }
        if (request.generation && request.generation != processed_generation) {
            ProcessRequest(request);
            processed_generation = request.generation;
        }
    }
}

void NotifyWorker() {
    if (worker_task) xTaskNotifyGive(worker_task);
}

}  // namespace

void Start(bool mounted) {
    std::lock_guard<std::mutex> lock(mutex);
    if (worker_task) return;
    tf_mounted = mounted;
    packs = {Builtin("default", "默认表情"), Builtin("tita", "TITA")};
    saved_target = LoadIdentity();
    active = {Source::Builtin, "default", 1};
    desired.identity = saved_target;
    desired.state = "cruise";
    desired.generation = 0;
    MarkSelectionLocked();
    scan_requested = true;
    if (xTaskCreate(Worker, "mepack_worker", 8192, nullptr, 3, &worker_task) != pdPASS) {
        worker_task = nullptr;
        SetStatusLocked("表情包后台任务启动失败");
        ESP_LOGE(kTag, "Cannot create MEPACK worker");
        return;
    }
    NotifyWorker();
}

void RequestScan() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        scan_requested = true;
        SetStatusLocked("正在扫描 TF 表情包…");
    }
    NotifyWorker();
}

void RequestState(const char* logical_state, const char* debug_text) {
    const std::string state = logical_state && logical_state[0] ? logical_state : "cruise";
    const std::string debug = debug_text ? debug_text : "";
    bool update_debug_only = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!playback_stopped && desired.state == state) {
            if (desired.debug == debug) return;
            desired.debug = debug;
            update_debug_only = Same(desired.identity, active);
            if (!update_debug_only) return;
        } else {
            desired.state = state;
            desired.debug = debug;
            desired.display_after_load = true;
            desired.persist_after_success = false;
            ++desired.generation;
            playback_stopped = false;
        }
    }
    if (update_debug_only) {
        Application::GetInstance().Schedule([debug]() {
            auto* display = dynamic_cast<emote::EmoteDisplay*>(Board::GetInstance().GetDisplay());
            if (display) display->UpdateCarDebugText(debug.empty() ? nullptr : debug.c_str());
        });
        return;
    }
    NotifyWorker();
}

void StopPlayback() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (playback_stopped) return;
        playback_stopped = true;
        ++desired.generation;
    }
    Application::GetInstance().Schedule([]() {
        auto* display = dynamic_cast<emote::EmoteDisplay*>(Board::GetInstance().GetDisplay());
        if (display) display->StopCarEmotion();
    });
}

bool Select(Source source, const char* pack_id, int version) {
    if (!pack_id || !pack_id[0] || version < 1) return false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        Identity identity{source, pack_id, version};
        if (!FindLocked(identity)) return false;
        const bool was_playing = !playback_stopped;
        desired.identity = std::move(identity);
        desired.persist_after_success = true;
        desired.display_after_load = was_playing;
        if (desired.state.empty()) desired.state = "cruise";
        ++desired.generation;
        playback_stopped = !was_playing;
        SetStatusLocked(source == Source::Tf ? "正在校验并加载 TF 表情…" : "正在切换内置表情…");
    }
    NotifyWorker();
    return true;
}

size_t CopyUiDescriptors(UiDescriptor* output, size_t capacity) {
    std::lock_guard<std::mutex> lock(mutex);
    const size_t count = std::min(capacity, packs.size());
    if (output) for (size_t i = 0; i < count; ++i) output[i] = packs[i].ui;
    return count;
}

uint32_t Revision() { return revision.load(); }
bool IsScanning() { return scanning.load(); }
int InvalidCount() { return invalid_count.load(); }
bool IsTfSelected() { std::lock_guard<std::mutex> lock(mutex); return saved_target.source == Source::Tf; }

void CurrentLabel(char* output, size_t capacity) {
    std::lock_guard<std::mutex> lock(mutex);
    const Pack* pack = FindLocked(saved_target);
    if (pack) std::snprintf(output, capacity, "%s v%d", pack->ui.display_name, pack->ui.version);
    else std::snprintf(output, capacity, "%s v%d（暂不可用）", saved_target.pack_id.c_str(), saved_target.version);
}

void StatusText(char* output, size_t capacity) {
    std::lock_guard<std::mutex> lock(mutex);
    if (output && capacity) std::snprintf(output, capacity, "%s", status_text);
}

}  // namespace expression_packs
