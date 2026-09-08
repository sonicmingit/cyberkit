#include "car_motion.h"
#include "../customer_ui/feature_ui.h"
#include "../ui_bridge.h"
#include "application.h"
#include "board.h"
#include "display/emote_display.h"
#include "settings.h"
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <esp_timer.h>
#include <esp_log.h>
#include <nvs.h>

namespace {
std::mutex mutex;
car::Detector detector;
std::atomic<bool> enabled{false};
std::atomic<bool> debug_enabled{false};
int pack = 0, axis = 0, sign = 1;
int64_t last_sample = 0;
bool sampled = false;
bool online = false;
car::Event event = car::Event::Cruise;
int64_t event_since = 0;
esp_timer_handle_t heartbeat = nullptr;
std::atomic<bool> pending{false};

car::Event DisplayEventLocked(int64_t now);

const char* ChineseEvent(car::Event value) {
    switch (value) {
        case car::Event::Accelerate: return "加速";
        case car::Event::Decelerate: return "减速";
        case car::Event::HardBrake: return "急刹";
        case car::Event::Bump: return "颠簸";
        case car::Event::Left: return "左转";
        case car::Event::Right: return "右转";
        case car::Event::SignalLost: return "传感器异常";
        default: return "巡航";
    }
}

bool Save(const char* key, int value) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("car_mode", NVS_READWRITE, &handle);
    if (err != ESP_OK) return false;
    err = std::strcmp(key, "enabled") == 0 ? nvs_set_u8(handle, key, value ? 1 : 0) :
                                             nvs_set_i32(handle, key, value);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) ESP_LOGE("car_mode", "Cannot save %s: %s", key, esp_err_to_name(err));
    return err == ESP_OK;
}
const char* StatusLocked(int64_t now) {
    if (!online || !sampled || now - last_sample > 1000) return "SIGNAL LOST";
    if (!enabled.load()) return "OFF / SENSOR OK";
    if (detector.AxisInvalid()) return "CHECK AXIS";
    if (!detector.Ready()) return "KEEP STILL";
    return car::Name(DisplayEventLocked(now));
}

int64_t MaxDisplayDurationMs(car::Event value) {
    switch (value) {
        case car::Event::Bump: return 1000;
        case car::Event::HardBrake: return 1400;
        case car::Event::Accelerate:
        case car::Event::Decelerate: return 2600;
        case car::Event::Left:
        case car::Event::Right: return 4200;
        default: return 0;
    }
}

car::Event DisplayEventLocked(int64_t now) {
    const int64_t max_duration = MaxDisplayDurationMs(event);
    if (max_duration > 0 && event_since > 0 && now - event_since > max_duration) {
        return car::Event::Cruise;
    }
    return event;
}

void Poll() {
    feature_timer_poll();
    auto* display = dynamic_cast<emote::EmoteDisplay*>(Board::GetInstance().GetDisplay());
    if (!display) return;
    if (!enabled.load() || !ui_bridge_is_on_home_page() ||
        Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
        display->StopCarEmotion();
        return;
    }
    char name[40];
    char debug_text[48] = {};
    {
        std::lock_guard<std::mutex> lock(mutex);
        const int64_t now = esp_timer_get_time() / 1000;
        const bool signal_lost = !online || !sampled || now - last_sample > 1000;
        const car::Event shown_event = signal_lost ? car::Event::SignalLost : DisplayEventLocked(now);
        const char* state = signal_lost ? "signal_lost" : !detector.Ready() ? "wait" : car::Name(shown_event);
        std::snprintf(name, sizeof(name), "car_%s_%s", pack == 0 ? "tita" : "default", state);
        if (debug_enabled.load()) {
            if (!detector.Ready() && !signal_lost) {
                std::snprintf(debug_text, sizeof(debug_text), "表情：校准中 %d%%", detector.CalibrationProgress());
            } else {
                std::snprintf(debug_text, sizeof(debug_text), "表情：%s", ChineseEvent(shown_event));
            }
        }
    }
    display->ShowCarEmotion(name, debug_text[0] ? debug_text : nullptr);
}
}

extern "C" void feature_services_start() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        Settings settings("car_mode", false);
        enabled.store(settings.GetBool("enabled", false));
        debug_enabled.store(settings.GetBool("debug", false));
        pack = settings.GetInt("pack", 0) == 1 ? 1 : 0;
        const int saved_axis = settings.GetInt("axis", 0);
        axis = saved_axis >= 0 && saved_axis <= 2 ? saved_axis : 0;
        sign = settings.GetInt("sign", 1) < 0 ? -1 : 1;
        const int orientation = settings.GetInt("orientation", axis * 2 + (sign < 0 ? 1 : 0));
        if (orientation >= 0 && orientation <= 5) {
            axis = orientation / 2; sign = orientation % 2 ? -1 : 1;
        }
        detector.Reset(axis, sign);
        event_since = 0;
    }
    feature_ui_on_ready();
    if (heartbeat) return;
    esp_timer_create_args_t args = {};
    args.name = "features";
    args.callback = [](void*) {
        if (pending.exchange(true)) return;
        Application::GetInstance().Schedule([]() {
            Poll();
            pending.store(false);
        });
    };
    esp_err_t err = esp_timer_create(&args, &heartbeat);
    if (err == ESP_OK) err = esp_timer_start_periodic(heartbeat, 100000);
    ESP_ERROR_CHECK(err); // Services must not silently report working without a heartbeat.
}
extern "C" bool feature_car_is_enabled() { return enabled.load(); }
extern "C" bool feature_car_set_enabled(bool value) {
    std::lock_guard<std::mutex> lock(mutex);
    if (enabled.load() == value) return true;
    if (!Save("enabled", value)) return false;
    detector.Reset(axis, sign); event = car::Event::Cruise; event_since = 0;
    enabled.store(value);
    return true;
}
extern "C" void feature_car_recalibrate() {
    std::lock_guard<std::mutex> lock(mutex);
    detector.Reset(axis, sign); event = car::Event::Cruise; event_since = 0;
}
extern "C" bool feature_car_set_pack(int value) {
    if (value < 0 || value > 1) return false;
    std::lock_guard<std::mutex> lock(mutex);
    if (!Save("pack", value)) return false;
    pack = value; return true;
}
extern "C" int feature_car_get_pack() {
    std::lock_guard<std::mutex> lock(mutex); return pack;
}
extern "C" int feature_car_get_axis() {
    std::lock_guard<std::mutex> lock(mutex); return axis * 2 + (sign < 0 ? 1 : 0);
}
extern "C" bool feature_car_set_axis(int value) {
    if (value < 0 || value > 5) return false;
    std::lock_guard<std::mutex> lock(mutex);
    if (!Save("orientation", value)) return false;
    axis = value / 2; sign = value % 2 ? -1 : 1;
    detector.Reset(axis, sign); event = car::Event::Cruise; event_since = 0;
    return true;
}
extern "C" bool feature_car_is_calibrated() {
    std::lock_guard<std::mutex> lock(mutex);
    return online && sampled && esp_timer_get_time()/1000 - last_sample <= 1000 && detector.Ready();
}
extern "C" const char* feature_car_get_event_name() {
    std::lock_guard<std::mutex> lock(mutex);
    const int64_t now = esp_timer_get_time() / 1000;
    return car::Name(!online || !sampled || now - last_sample > 1000 ?
                     car::Event::SignalLost : DisplayEventLocked(now));
}
extern "C" int feature_car_get_calibration_progress() {
    std::lock_guard<std::mutex> lock(mutex);
    return detector.CalibrationProgress();
}
extern "C" bool feature_car_is_debug_enabled() { return debug_enabled.load(); }
extern "C" bool feature_car_set_debug_enabled(bool value) {
    std::lock_guard<std::mutex> lock(mutex);
    if (debug_enabled.load() == value) return true;
    if (!Save("debug", value)) return false;
    debug_enabled.store(value);
    return true;
}
extern "C" const char* feature_car_get_status() {
    std::lock_guard<std::mutex> lock(mutex); return StatusLocked(esp_timer_get_time()/1000);
}
extern "C" void feature_car_feed_sample(float ax, float ay, float az,
                                        float gx, float gy, float gz, int64_t timestamp) {
    std::lock_guard<std::mutex> lock(mutex);
    last_sample = timestamp; sampled = online = true;
    if (enabled.load()) {
        const car::Event next_event = detector.Feed({{ax, ay, az}, {gx, gy, gz}, timestamp});
        if (next_event != event) {
            event = next_event;
            event_since = timestamp;
        } else if (event_since == 0) {
            event_since = timestamp;
        }
        if (event == car::Event::SignalLost) online = false;
    }
}
extern "C" void feature_car_mark_sensor_lost(int64_t timestamp) {
    std::lock_guard<std::mutex> lock(mutex);
    if (sampled && timestamp - last_sample <= 1000) return;
    online = false;
    event = car::Event::SignalLost;
    event_since = timestamp;
    detector.Reset(axis, sign);
}
