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
int pack = 0, axis = 0, sign = 1;
int64_t last_sample = 0;
bool sampled = false;
bool online = false;
car::Event event = car::Event::Cruise;
esp_timer_handle_t heartbeat = nullptr;
std::atomic<bool> pending{false};

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
    return car::Name(event);
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
    char name[32];
    {
        std::lock_guard<std::mutex> lock(mutex);
        const int64_t now = esp_timer_get_time() / 1000;
        const char* state = !online || !sampled || now - last_sample > 1000 ? "signal_lost" :
                            !detector.Ready() ? "wait" : car::Name(event);
        std::snprintf(name, sizeof(name), "car_%s_%s", pack == 0 ? "tita" : "cat", state);
    }
    display->ShowCarEmotion(name);
}
}

extern "C" void feature_services_start() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        Settings settings("car_mode", false);
        enabled.store(settings.GetBool("enabled", false));
        pack = settings.GetInt("pack", 0) == 1 ? 1 : 0;
        const int saved_axis = settings.GetInt("axis", 0);
        axis = saved_axis >= 0 && saved_axis <= 2 ? saved_axis : 0;
        sign = settings.GetInt("sign", 1) < 0 ? -1 : 1;
        const int orientation = settings.GetInt("orientation", axis * 2 + (sign < 0 ? 1 : 0));
        if (orientation >= 0 && orientation <= 5) {
            axis = orientation / 2; sign = orientation % 2 ? -1 : 1;
        }
        detector.Reset(axis, sign);
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
    detector.Reset(axis, sign); event = car::Event::Cruise;
    enabled.store(value);
    return true;
}
extern "C" void feature_car_recalibrate() {
    std::lock_guard<std::mutex> lock(mutex);
    detector.Reset(axis, sign); event = car::Event::Cruise;
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
    detector.Reset(axis, sign); event = car::Event::Cruise;
    return true;
}
extern "C" bool feature_car_is_calibrated() {
    std::lock_guard<std::mutex> lock(mutex);
    return online && sampled && esp_timer_get_time()/1000 - last_sample <= 1000 && detector.Ready();
}
extern "C" const char* feature_car_get_event_name() {
    std::lock_guard<std::mutex> lock(mutex);
    return car::Name(!online || !sampled || esp_timer_get_time()/1000 - last_sample > 1000 ?
                     car::Event::SignalLost : event);
}
extern "C" const char* feature_car_get_status() {
    std::lock_guard<std::mutex> lock(mutex); return StatusLocked(esp_timer_get_time()/1000);
}
extern "C" void feature_car_feed_sample(float ax, float ay, float az,
                                        float gx, float gy, float gz, int64_t timestamp) {
    std::lock_guard<std::mutex> lock(mutex);
    last_sample = timestamp; sampled = online = true;
    if (enabled.load()) {
        event = detector.Feed({{ax, ay, az}, {gx, gy, gz}, timestamp});
        if (event == car::Event::SignalLost) online = false;
    }
}
extern "C" void feature_car_mark_sensor_lost(int64_t timestamp) {
    std::lock_guard<std::mutex> lock(mutex);
    if (sampled && timestamp - last_sample <= 1000) return;
    online = false;
    event = car::Event::SignalLost;
    detector.Reset(axis, sign);
}
