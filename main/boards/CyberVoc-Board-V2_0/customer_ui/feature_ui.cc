#include "feature_ui.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "system_info.h"
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_psram.h>
#include <esp_idf_version.h>
#include <string>
#include "../CyberVoc.h"
#include "display/display.h"
#include "settings.h"
#include "../ui_bridge.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>

namespace {

constexpr char kTag[] = "feature_ui";
lv_obj_t* menu_container = nullptr;
lv_obj_t* timer_container = nullptr;
lv_obj_t* settings_container = nullptr;
lv_obj_t* menu_arcs[6]{};
lv_obj_t* menu_time_label = nullptr;
lv_obj_t* menu_date_label = nullptr;
lv_obj_t* menu_status_label = nullptr;
lv_obj_t* timer_value_label = nullptr;
lv_obj_t* timer_action_label = nullptr;
lv_obj_t* settings_status_label = nullptr;
lv_timer_t* ui_refresh_timer = nullptr;
lv_obj_t* info_container = nullptr;
lv_obj_t* info_label = nullptr;
std::atomic<bool> ui_ready{false};
std::mutex info_mutex;
std::string info_text = "Loading...";
int pressed_item = -1;
bool menu_long_press_handled = false;

void UpdateClockLabels() {
    std::time_t now = std::time(nullptr);
    struct tm local_time{};
    char time_text[16] = "--:--";
    char date_text[24] = "TIME NOT SET";
    if (now > 1000000000 && localtime_r(&now, &local_time) != nullptr) {
        std::strftime(time_text, sizeof(time_text), "%H:%M", &local_time);
        std::strftime(date_text, sizeof(date_text), "%Y-%m-%d", &local_time);
    }
    if (menu_time_label) lv_label_set_text(menu_time_label, time_text);
    if (menu_date_label) lv_label_set_text(menu_date_label, date_text);
}

void UpdateTimerLabels() {
    const int32_t remaining = feature_timer_get_remaining_seconds();
    char value[24];
    std::snprintf(value, sizeof(value), "%02ld:%02ld:%02ld",
                  static_cast<long>(remaining / 3600),
                  static_cast<long>((remaining / 60) % 60),
                  static_cast<long>(remaining % 60));
    if (timer_value_label) lv_label_set_text(timer_value_label, value);
    if (timer_action_label) {
        lv_label_set_text(timer_action_label,
                          feature_timer_get_state() == FEATURE_TIMER_RUNNING ? "PAUSE" :
                          feature_timer_get_state() == FEATURE_TIMER_PAUSED ? "RESUME" : "START");
    }
}

void UpdateMenuState();

void UpdateSettingsLabel() {
    if (!settings_status_label || !ui_ready.load()) return;
    static const char* axes[] = {"X+", "X-", "Y+", "Y-", "Z+", "Z-"};
    char status[120];
    std::snprintf(status, sizeof(status), "IMU: %s\nPack: %s  Axis: %s",
                  feature_car_get_status(), feature_car_get_pack() == 0 ? "TITA" : "CAT",
                  axes[feature_car_get_axis()]);
    lv_label_set_text(settings_status_label, status);
}

void UiRefreshCallback(lv_timer_t*) {
    UpdateClockLabels();
    UpdateTimerLabels();
    if (!ui_ready.load()) return; // Board is still being constructed.
    UpdateMenuState();
    UpdateSettingsLabel();
    if (info_label) {
        std::lock_guard<std::mutex> lock(info_mutex);
        lv_label_set_text(info_label, info_text.c_str());
    }
}

void RefreshInfo() { // application task, never the LVGL task
    auto& board = Board::GetInstance();
    int battery = 0; bool charging = false, discharging = false;
    const bool battery_ok = board.GetBatteryLevel(battery, charging, discharging);
    char text[480], battery_text[32];
    if (battery_ok) std::snprintf(battery_text, sizeof(battery_text), "%d%% %s", battery, charging ? "charging" : "");
    else std::snprintf(battery_text, sizeof(battery_text), "unavailable");
    auto* cat = dynamic_cast<EspS3Cat*>(&board);
    const auto* app = esp_app_get_description();
    std::snprintf(text, sizeof(text),
                  "CyberVoc V2.0 / ESP32-S3\nFW %s\nIDF %s\nNetwork: %s\nBattery: %s\nFlash: %u MB  PSRAM: %u MB\nFree heap: %u KB\nUptime: %lld s\nIMU: %s\nHead touch: %s\nWake: Xiao Te Xiao Te",
                  app->version, esp_get_idf_version(),
                  cat ? (cat->GetNetworkType() == NetworkType::WIFI ? "Wi-Fi" : "4G") : "unknown",
                  battery_text, (unsigned)(SystemInfo::GetFlashSize()/1048576),
                  (unsigned)(esp_psram_get_size()/1048576), (unsigned)(esp_get_free_heap_size()/1024),
                  (long long)(esp_timer_get_time()/1000000), feature_car_get_status(),
                  board.IsHeadTouchEnabled() ? "ON" : "OFF");
    std::lock_guard<std::mutex> lock(info_mutex); info_text = text;
}

int HitMenuItem(lv_coord_t x, lv_coord_t y) {
    const float dx = static_cast<float>(x - UI_BRIDGE_CENTER_X);
    const float dy = static_cast<float>(y - UI_BRIDGE_CENTER_Y);
    const float radius = std::sqrt(dx * dx + dy * dy);
    if (radius < 82.0f || radius > 174.0f) return -1;
    float angle = std::atan2(dx, -dy) * 180.0f / 3.14159265f;
    if (angle < 0) angle += 360.0f;
    return static_cast<int>((angle + 30.0f) / 60.0f) % 6;
}

void OpenFeaturePage(const char* page) {
    Application::GetInstance().SwitchToIdle();
    ui_bridge_switch_page(page);
}

struct MenuItem {
    const char* label;
    bool available;
    bool (*active)();
    void (*activate)(bool long_press);
};
const MenuItem kMenuItems[] = {
    {"CAR", true, feature_car_is_enabled, [](bool detail) {
        if (detail) OpenFeaturePage(PAGE_DEVICE_SETTINGS);
        else if (!feature_car_set_enabled(!feature_car_is_enabled()))
            Board::GetInstance().GetDisplay()->ShowNotification("Cannot save car setting");
    }},
    {"TIMER", true, []() { return feature_timer_get_state() == FEATURE_TIMER_RUNNING ||
                                      feature_timer_get_state() == FEATURE_TIMER_PAUSED; },
        [](bool) { OpenFeaturePage(PAGE_TIMER); }},
    {"HEAD", true, []() { return Board::GetInstance().IsHeadTouchEnabled(); },
        [](bool) { auto& board = Board::GetInstance(); board.SetHeadTouchEnabled(!board.IsHeadTouchEnabled()); }},
    {"INFO", true, nullptr, [](bool) { RefreshInfo(); OpenFeaturePage(PAGE_DEVICE_INFO); }},
    {"SET", true, nullptr, [](bool) { OpenFeaturePage(PAGE_DEVICE_SETTINGS); }},
    {"+", false, nullptr, nullptr},
};
static_assert(sizeof(kMenuItems)/sizeof(kMenuItems[0]) == 6);

void UpdateMenuState() {
    if (!menu_status_label) return;
    char status[96];
    std::snprintf(status, sizeof(status), "CAR %s\nTOUCH %s",
                  feature_car_is_enabled() ? "ON" : "OFF",
                  Board::GetInstance().IsHeadTouchEnabled() ? "ON" : "OFF");
    lv_label_set_text(menu_status_label, status);
    for (int i = 0; i < 6; ++i) {
        if (menu_arcs[i]) lv_obj_set_style_arc_color(menu_arcs[i],
            lv_color_hex(kMenuItems[i].active && kMenuItems[i].active() ? 0xFF8A35 :
                         kMenuItems[i].available ? 0x3B3B3B : 0x252525), LV_PART_MAIN);
    }
}
void HandleMenuItem(int index, bool long_press) {
    if (!ui_ready.load() || index < 0 || index >= 6 || !kMenuItems[index].available) return;
    Application::GetInstance().Schedule([index, long_press]() { kMenuItems[index].activate(long_press); });
}

void MenuEvent(lv_event_t* event) {
    const auto code = lv_event_get_code(event);
    auto* input = lv_indev_get_act();
    if (!input) return;
    lv_point_t point; lv_indev_get_point(input, &point);
    const int item = HitMenuItem(point.x, point.y);
    if (code == LV_EVENT_PRESSED) {
        pressed_item = item;
        menu_long_press_handled = false;
    } else if (code == LV_EVENT_PRESSING) {
        ui_bridge_track_pointer();
    } else if (code == LV_EVENT_PRESS_LOST) {
        pressed_item = -1;
        menu_long_press_handled = true;
    } else if (item >= 0 && item == pressed_item && ui_bridge_click_allowed()) {
        if (code == LV_EVENT_LONG_PRESSED && !menu_long_press_handled) {
            menu_long_press_handled = true;
            ui_bridge_cancel_pointer();
            HandleMenuItem(item, true);
        } else if (code == LV_EVENT_CLICKED && !menu_long_press_handled) HandleMenuItem(item, false);
    }
}

lv_obj_t* CreateTextButton(lv_obj_t* parent, const char* text, lv_coord_t width,
                           lv_coord_t height, lv_coord_t x, lv_coord_t y,
                           lv_event_cb_t callback, void* user_data = nullptr) {
    lv_obj_t* button = lv_btn_create(parent);
    lv_obj_set_size(button, width, height);
    lv_obj_align(button, LV_ALIGN_CENTER, x, y);
    lv_obj_set_style_radius(button, 22, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x343434), 0);
    lv_obj_add_event_cb(button, [](lv_event_t*) { ui_bridge_track_pointer(); }, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(button, [](lv_event_t* e) {
        if (!ui_bridge_click_allowed()) lv_event_stop_processing(e);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
    return label;
}

void TimerAdjustEvent(lv_event_t* event) {
    const int delta = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    Application::GetInstance().Schedule([delta]() { feature_timer_adjust_minutes(delta); });
}
void TimerActionEvent(lv_event_t*) {
    Application::GetInstance().Schedule([]() {
        const auto state = feature_timer_get_state();
        if (state == FEATURE_TIMER_RUNNING) feature_timer_pause();
        else if (state == FEATURE_TIMER_PAUSED) feature_timer_resume();
        else feature_timer_start_minutes(feature_timer_get_selected_minutes());
    });
}
void TimerCancelEvent(lv_event_t*) {
    Application::GetInstance().Schedule([]() { feature_timer_cancel(); });
}
void BackToMenuEvent(lv_event_t*) {
    Application::GetInstance().Schedule([]() { feature_ui_open_menu(); });
}
void PackEvent(lv_event_t*) {
    Application::GetInstance().Schedule([]() {
        if (!feature_car_set_pack(1 - feature_car_get_pack()))
            Board::GetInstance().GetDisplay()->ShowNotification("Cannot save pack");
    });
}
void AxisEvent(lv_event_t*) {
    Application::GetInstance().Schedule([]() {
        if (!feature_car_set_axis((feature_car_get_axis() + 1) % 6))
            Board::GetInstance().GetDisplay()->ShowNotification("Cannot save axis");
    });
}
void CalibrateEvent(lv_event_t*) {
    Application::GetInstance().Schedule([]() { feature_car_recalibrate(); });
}
void InfoRefreshEvent(lv_event_t*) {
    Application::GetInstance().Schedule([]() { RefreshInfo(); });
}

void CreateMenu(lv_obj_t* screen) {
    menu_container = lv_obj_create(screen);
    lv_obj_set_size(menu_container, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_set_style_bg_color(menu_container, lv_color_black(), 0);
    lv_obj_set_style_border_width(menu_container, 0, 0);
    lv_obj_set_style_pad_all(menu_container, 0, 0);
    lv_obj_clear_flag(menu_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(menu_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(menu_container, MenuEvent, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(menu_container, MenuEvent, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(menu_container, MenuEvent, LV_EVENT_PRESS_LOST, nullptr);
    lv_obj_add_event_cb(menu_container, MenuEvent, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_add_event_cb(menu_container, MenuEvent, LV_EVENT_CLICKED, nullptr);

    static constexpr int kCenters[6] = {270, 330, 30, 90, 150, 210};

    for (int i = 0; i < 6; ++i) {
        lv_obj_t* arc = lv_arc_create(menu_container);
        menu_arcs[i] = arc;
        lv_obj_set_size(arc, 326, 326);
        lv_obj_center(arc);
        lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
        lv_obj_set_style_arc_width(arc, 52, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, lv_color_hex(!kMenuItems[i].available ? 0x252525 : 0x3B3B3B), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_arc_set_bg_angles(arc, kCenters[i] - 25, kCenters[i] + 25);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);

        const float radians = kCenters[i] * 3.14159265f / 180.0f;
        lv_obj_t* label = lv_label_create(menu_container);
        lv_label_set_text(label, kMenuItems[i].label);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(label, !kMenuItems[i].available ? lv_color_hex(0x777777) : lv_color_white(), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(label, 76);
        lv_obj_set_pos(label,
                       UI_BRIDGE_CENTER_X + static_cast<int>(126 * std::cos(radians)) - 38,
                       UI_BRIDGE_CENTER_Y + static_cast<int>(126 * std::sin(radians)) - 10);
    }

    menu_time_label = lv_label_create(menu_container);
    lv_obj_set_style_text_font(menu_time_label, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(menu_time_label, lv_color_white(), 0);
    lv_obj_align(menu_time_label, LV_ALIGN_CENTER, 0, -28);

    menu_date_label = lv_label_create(menu_container);
    lv_obj_set_style_text_font(menu_date_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(menu_date_label, lv_color_hex(0xB0B0B0), 0);
    lv_obj_align(menu_date_label, LV_ALIGN_CENTER, 0, 13);

    menu_status_label = lv_label_create(menu_container);
    lv_obj_set_style_text_color(menu_status_label, lv_color_hex(0xA0A0A0), 0);
    lv_obj_set_style_text_align(menu_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(menu_status_label, 220);
    lv_obj_align(menu_status_label, LV_ALIGN_CENTER, 0, 47);
    lv_obj_add_flag(menu_container, LV_OBJ_FLAG_HIDDEN);
}

void CreateTimerPage(lv_obj_t* screen) {
    timer_container = lv_obj_create(screen);
    lv_obj_set_size(timer_container, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_set_style_bg_color(timer_container, lv_color_black(), 0);
    lv_obj_set_style_border_width(timer_container, 0, 0);
    lv_obj_clear_flag(timer_container, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(timer_container);
    lv_label_set_text(title, "TIMER");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xBBBBBB), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -112);

    timer_value_label = lv_label_create(timer_container);
    lv_obj_set_style_text_font(timer_value_label, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(timer_value_label, lv_color_white(), 0);
    lv_obj_align(timer_value_label, LV_ALIGN_CENTER, 0, -54);

    CreateTextButton(timer_container, "-5", 72, 44, -68, 12, TimerAdjustEvent,
                     reinterpret_cast<void*>(static_cast<intptr_t>(-5)));
    CreateTextButton(timer_container, "+5", 72, 44, 68, 12, TimerAdjustEvent,
                     reinterpret_cast<void*>(static_cast<intptr_t>(5)));
    timer_action_label = CreateTextButton(timer_container, "START", 118, 48, 0, 72, TimerActionEvent);
    CreateTextButton(timer_container, "CANCEL", 96, 42, -57, 116, TimerCancelEvent);
    CreateTextButton(timer_container, "MENU", 96, 42, 57, 116, BackToMenuEvent);
    lv_obj_add_flag(timer_container, LV_OBJ_FLAG_HIDDEN);
}

void CreateSettingsPage(lv_obj_t* screen) {
    settings_container = lv_obj_create(screen);
    lv_obj_set_size(settings_container, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_set_style_bg_color(settings_container, lv_color_black(), 0);
    lv_obj_set_style_border_width(settings_container, 0, 0);
    lv_obj_clear_flag(settings_container, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(settings_container);
    lv_label_set_text(title, "CAR SETTINGS");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF8A35), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -116);

    settings_status_label = lv_label_create(settings_container);
    lv_obj_set_style_text_font(settings_status_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(settings_status_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(settings_status_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(settings_status_label, LV_ALIGN_CENTER, 0, -55);
    CreateTextButton(settings_container, "PACK", 108, 44, -62, 15, PackEvent);
    CreateTextButton(settings_container, "AXIS", 108, 44, 62, 15, AxisEvent);
    CreateTextButton(settings_container, "CALIBRATE", 166, 44, 0, 70, CalibrateEvent);
    CreateTextButton(settings_container, "MENU", 108, 44, 0, 126, BackToMenuEvent);
    lv_obj_add_flag(settings_container, LV_OBJ_FLAG_HIDDEN);
}

void CreateInfoPage(lv_obj_t* screen) {
    info_container = lv_obj_create(screen);
    lv_obj_set_size(info_container, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_set_style_bg_color(info_container, lv_color_black(), 0);
    lv_obj_set_style_border_width(info_container, 0, 0);
    lv_obj_clear_flag(info_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* scroll = lv_obj_create(info_container);
    lv_obj_set_size(scroll, 270, 225);
    lv_obj_align(scroll, LV_ALIGN_CENTER, 0, -20);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    info_label = lv_label_create(scroll);
    lv_obj_set_width(info_label, 235);
    lv_obj_set_style_text_font(info_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(info_label, lv_color_white(), 0);
    lv_label_set_text(info_label, "Loading...");
    CreateTextButton(info_container, "REFRESH", 116, 42, -62, 110, InfoRefreshEvent);
    CreateTextButton(info_container, "MENU", 96, 42, 62, 110, BackToMenuEvent);
    lv_obj_add_flag(info_container, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

extern "C" void feature_ui_create(void) {
    lv_obj_t* screen = lv_scr_act();
    CreateMenu(screen);
    CreateTimerPage(screen);
    CreateSettingsPage(screen);
    CreateInfoPage(screen);
    ui_bridge_register_page_with_cycle(PAGE_RING_MENU, &menu_container, false);
    ui_bridge_register_page_with_cycle(PAGE_TIMER, &timer_container, false);
    ui_bridge_register_page_with_cycle(PAGE_DEVICE_SETTINGS, &settings_container, false);
    ui_bridge_register_page_with_cycle(PAGE_DEVICE_INFO, &info_container, false);
    ui_refresh_timer = lv_timer_create(UiRefreshCallback, 250, nullptr);
    (void)ui_refresh_timer;
    UiRefreshCallback(nullptr);
    ESP_LOGI(kTag, "Feature UI initialized; services start after board construction");
}

extern "C" bool feature_ui_is_feature_page(const char* page_id) {
    return page_id && (std::strcmp(page_id, PAGE_RING_MENU) == 0 ||
                       std::strcmp(page_id, PAGE_TIMER) == 0 ||
                       std::strcmp(page_id, PAGE_DEVICE_SETTINGS) == 0 || std::strcmp(page_id, PAGE_DEVICE_INFO) == 0);
}

extern "C" void feature_ui_open_menu(void) {
    OpenFeaturePage(PAGE_RING_MENU);
}

extern "C" void feature_ui_close_to_home(void) {
    OpenFeaturePage(UI_BRIDGE_PAGE_HOME);
}

extern "C" void feature_ui_on_ready() { ui_ready.store(true); }
