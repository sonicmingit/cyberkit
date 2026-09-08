#include "feature_ui.h"

#include "application.h"
#include "audio/audio_codec.h"
#include "assets/lang_config.h"
#include "board.h"
#include "backlight.h"
#include "font_awesome.h"
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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

#include <esp_log.h>
#include <esp_timer.h>
#include <lvgl.h>

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);

namespace {

constexpr char kTag[] = "feature_ui";
lv_obj_t* menu_container = nullptr;
lv_obj_t* timer_container = nullptr;
lv_obj_t* car_settings_container = nullptr;
lv_obj_t* system_settings_container = nullptr;
lv_obj_t* menu_arcs[6]{};
lv_obj_t* menu_icons[6]{};
lv_obj_t* menu_labels[6]{};
lv_obj_t* menu_time_label = nullptr;
lv_obj_t* menu_date_label = nullptr;
lv_obj_t* menu_status_label = nullptr;
lv_obj_t* timer_value_label = nullptr;
lv_obj_t* timer_action_label = nullptr;
lv_obj_t* settings_status_label = nullptr;
lv_obj_t* settings_debug_label = nullptr;
lv_obj_t* system_touch_label = nullptr;
lv_obj_t* system_volume_label = nullptr;
lv_obj_t* system_brightness_label = nullptr;
lv_timer_t* ui_refresh_timer = nullptr;
lv_obj_t* info_container = nullptr;
lv_obj_t* info_label = nullptr;
std::atomic<bool> ui_ready{false};
std::mutex info_mutex;
char info_text[384] = "正在读取设备信息...";
std::atomic<bool> info_dirty{true};
int pressed_item = -1;
bool menu_long_press_handled = false;

const char* CarStatusText(const char* status) {
    if (!status) return "未知";
    if (std::strcmp(status, "SIGNAL LOST") == 0) return "传感器异常";
    if (std::strcmp(status, "OFF / SENSOR OK") == 0) return "已关闭";
    if (std::strcmp(status, "CHECK AXIS") == 0) return "请检查方向";
    if (std::strcmp(status, "KEEP STILL") == 0) return "请保持静止";
    if (std::strcmp(status, "accelerate") == 0) return "加速";
    if (std::strcmp(status, "decelerate") == 0) return "减速";
    if (std::strcmp(status, "hard_brake") == 0) return "急刹";
    if (std::strcmp(status, "bump") == 0) return "颠簸";
    if (std::strcmp(status, "turn_left") == 0) return "左转";
    if (std::strcmp(status, "turn_right") == 0) return "右转";
    return "巡航";
}

void UpdateClockLabels() {
    std::time_t now = std::time(nullptr);
    struct tm local_time{};
    char time_text[16] = "--:--";
    char date_text[32] = "时间未同步";
    if (now > 1000000000 && localtime_r(&now, &local_time) != nullptr) {
        static const char* const kWeekdays[] = {
            "周日", "周一", "周二", "周三", "周四", "周五", "周六"
        };
        std::strftime(time_text, sizeof(time_text), "%H:%M", &local_time);
        std::snprintf(date_text, sizeof(date_text), "%d月%d日  %s",
                      local_time.tm_mon + 1, local_time.tm_mday, kWeekdays[local_time.tm_wday]);
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
                          feature_timer_get_state() == FEATURE_TIMER_RUNNING ? "暂停" :
                          feature_timer_get_state() == FEATURE_TIMER_PAUSED ? "继续" : "开始");
    }
}

void UpdateMenuState();

void UpdateSettingsLabel() {
    if (!settings_status_label || !ui_ready.load()) return;
    static const char* axes[] = {"X+", "X-", "Y+", "Y-", "Z+", "Z-"};
    char status[160];
    const bool calibrated = feature_car_is_calibrated();
    if (calibrated) {
        std::snprintf(status, sizeof(status), "状态：%s\n表情：%s  车头方向：%s",
                      CarStatusText(feature_car_get_status()),
                      feature_car_get_pack() == 0 ? "TITA" : "默认", axes[feature_car_get_axis()]);
    } else {
        std::snprintf(status, sizeof(status), "状态：%s %d%%\n表情：%s  车头方向：%s",
                      CarStatusText(feature_car_get_status()), feature_car_get_calibration_progress(),
                      feature_car_get_pack() == 0 ? "TITA" : "默认", axes[feature_car_get_axis()]);
    }
    lv_label_set_text(settings_status_label, status);
    if (settings_debug_label) {
        lv_label_set_text(settings_debug_label,
                          feature_car_is_debug_enabled() ? "调试显示：开" : "调试显示：关");
    }
}

void UpdateSystemSettingsLabels() {
    if (!ui_ready.load()) return;
    auto& board = Board::GetInstance();
    if (system_touch_label) {
        lv_label_set_text(system_touch_label, board.IsHeadTouchEnabled() ? "摸头触摸：开" : "摸头触摸：关");
    }
    if (system_volume_label) {
        auto* codec = board.GetAudioCodec();
        char text[32];
        std::snprintf(text, sizeof(text), "音量  %d%%", codec ? codec->output_volume() : 0);
        lv_label_set_text(system_volume_label, text);
    }
    if (system_brightness_label) {
        auto* backlight = board.GetBacklight();
        char text[32];
        std::snprintf(text, sizeof(text), "亮度  %u%%", backlight ? backlight->brightness() : 0);
        lv_label_set_text(system_brightness_label, text);
    }
}

void UiRefreshCallback(lv_timer_t*) {
    UpdateClockLabels();
    UpdateTimerLabels();
    if (!ui_ready.load()) return; // Board is still being constructed.
    UpdateMenuState();
    UpdateSettingsLabel();
    UpdateSystemSettingsLabels();
    const char* current_page = ui_bridge_get_current_page();
    if (info_label && current_page && std::strcmp(current_page, PAGE_DEVICE_INFO) == 0 &&
        info_dirty.exchange(false)) {
        std::lock_guard<std::mutex> lock(info_mutex);
        lv_label_set_text(info_label, info_text);
    }
}

void RefreshInfo() { // application task; builds one non-blocking snapshot per request
    auto& board = Board::GetInstance();
    int battery = 0; bool charging = false, discharging = false;
    auto* cat = dynamic_cast<EspS3Cat*>(&board);
    const bool battery_ok = cat && cat->GetCachedBatteryLevel(battery, charging, discharging);
    char battery_text[32];
    if (battery_ok) std::snprintf(battery_text, sizeof(battery_text), "%d%% %s", battery, charging ? "充电中" : "");
    else std::snprintf(battery_text, sizeof(battery_text), "暂不可用");
    const auto* app = esp_app_get_description();
    std::lock_guard<std::mutex> lock(info_mutex);
    std::snprintf(info_text, sizeof(info_text),
                  "设备：CyberVoc V2.0\n固件：%s\n系统：ESP-IDF %s\n网络：%s\n电量：%s\n闪存：%u MB  内存：%u MB\n可用堆：%u KB\n运行：%lld 秒\n陀螺仪：%s\n摸头触摸：%s\n唤醒词：小特小特",
                  app->version, esp_get_idf_version(),
                  cat ? (cat->GetNetworkType() == NetworkType::WIFI ? "Wi-Fi" : "4G") : "unknown",
                  battery_text, (unsigned)(SystemInfo::GetFlashSize()/1048576),
                  (unsigned)(esp_psram_get_size()/1048576), (unsigned)(esp_get_free_heap_size()/1024),
                  (long long)(esp_timer_get_time()/1000000), CarStatusText(feature_car_get_status()),
                  board.IsHeadTouchEnabled() ? "开" : "关");
    info_dirty.store(true);
}

int HitMenuItem(lv_coord_t x, lv_coord_t y) {
    const float dx = static_cast<float>(x - UI_BRIDGE_CENTER_X);
    const float dy = static_cast<float>(y - UI_BRIDGE_CENTER_Y);
    const float radius = std::sqrt(dx * dx + dy * dy);
    if (radius < 98.0f || radius > 174.0f) return -1;
    float angle = std::atan2(dx, -dy) * 180.0f / 3.14159265f;
    if (angle < 0) angle += 360.0f;
    const int item = static_cast<int>(angle / 60.0f);
    const float center = item * 60.0f + 30.0f;
    return std::fabs(angle - center) <= 25.0f ? item : -1;
}

void OpenFeaturePage(const char* page) {
    Application::GetInstance().SwitchToIdle();
    ui_bridge_switch_page(page);
}

struct MenuItem {
    const char* icon;
    const char* label;
    bool available;
    bool (*active)();
    void (*activate)(bool long_press);
};
const MenuItem kMenuItems[] = {
    {FONT_AWESOME_GEAR, "设备设置", true, nullptr,
        [](bool) { OpenFeaturePage(PAGE_SYSTEM_SETTINGS); }},
    {FONT_AWESOME_CIRCLE_INFO, "系统信息", true, nullptr,
        [](bool) { OpenFeaturePage(PAGE_DEVICE_INFO); RefreshInfo(); }},
    {FONT_AWESOME_HOUSE, "返回桌面", true, nullptr,
        [](bool) { feature_ui_close_to_home(); }},
    {FONT_AWESOME_LOCATION_ARROW, "车载模式", true, feature_car_is_enabled, [](bool detail) {
        if (detail) OpenFeaturePage(PAGE_CAR_SETTINGS);
        else if (!feature_car_set_enabled(!feature_car_is_enabled()))
            Board::GetInstance().GetDisplay()->ShowNotification("车载设置保存失败");
    }},
    {FONT_AWESOME_ALARM_CLOCK, "计时器", true, []() { return feature_timer_get_state() == FEATURE_TIMER_RUNNING ||
                                      feature_timer_get_state() == FEATURE_TIMER_PAUSED; },
        [](bool) { OpenFeaturePage(PAGE_TIMER); }},
    {"+", "功能模板", false, nullptr, nullptr},
};
static_assert(sizeof(kMenuItems)/sizeof(kMenuItems[0]) == 6);

void UpdateMenuState() {
    if (!menu_status_label) return;
    const bool car_on = feature_car_is_enabled();
    char status[64];
    std::snprintf(status, sizeof(status), "•  车载模式%s", car_on ? "已开启" : "已关闭");
    lv_label_set_text(menu_status_label, status);
    lv_obj_set_style_text_color(menu_status_label,
                                car_on ? lv_color_hex(0xFF5A1F) : lv_color_hex(0x8A8A8A), 0);
    for (int i = 0; i < 6; ++i) {
        const bool active = kMenuItems[i].active && kMenuItems[i].active();
        const lv_color_t segment_color = lv_color_hex(active ? 0xFF5A1F :
                                                       kMenuItems[i].available ? 0x182021 : 0x111718);
        const lv_color_t text_color = lv_color_hex(!kMenuItems[i].available ? 0x777777 : 0xF2F0EB);
        if (menu_arcs[i]) lv_obj_set_style_arc_color(menu_arcs[i], segment_color, LV_PART_MAIN);
        if (menu_icons[i]) lv_obj_set_style_text_color(menu_icons[i], text_color, 0);
        if (menu_labels[i]) lv_obj_set_style_text_color(menu_labels[i], text_color, 0);
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
    lv_obj_set_style_bg_color(button, lv_color_hex(0x273234), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x596769), 0);
    lv_obj_add_event_cb(button, [](lv_event_t*) { ui_bridge_track_pointer(); }, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(button, [](lv_event_t* e) {
        if (!ui_bridge_click_allowed()) lv_event_stop_processing(e);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &BUILTIN_TEXT_FONT, 0);
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
            Board::GetInstance().GetDisplay()->ShowNotification("表情包保存失败");
    });
}
void AxisEvent(lv_event_t*) {
    Application::GetInstance().Schedule([]() {
        if (!feature_car_set_axis((feature_car_get_axis() + 1) % 6))
            Board::GetInstance().GetDisplay()->ShowNotification("安装方向保存失败");
    });
}
void CalibrateEvent(lv_event_t*) {
    Application::GetInstance().Schedule([]() { feature_car_recalibrate(); });
}
void DebugEvent(lv_event_t*) {
    Application::GetInstance().Schedule([]() {
        if (!feature_car_set_debug_enabled(!feature_car_is_debug_enabled()))
            Board::GetInstance().GetDisplay()->ShowNotification("调试设置保存失败");
    });
}
void InfoRefreshEvent(lv_event_t*) {
    Application::GetInstance().Schedule([]() { RefreshInfo(); });
}
void HeadTouchEvent(lv_event_t*) {
    Application::GetInstance().Schedule([]() {
        auto& board = Board::GetInstance();
        board.SetHeadTouchEnabled(!board.IsHeadTouchEnabled());
    });
}
void VolumeAdjustEvent(lv_event_t* event) {
    const int delta = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    Application::GetInstance().Schedule([delta]() {
        auto* codec = Board::GetInstance().GetAudioCodec();
        if (!codec) return;
        codec->SetOutputVolume(std::clamp(codec->output_volume() + delta, 10, 100));
    });
}
void BrightnessAdjustEvent(lv_event_t* event) {
    const int delta = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    Application::GetInstance().Schedule([delta]() {
        auto* backlight = Board::GetInstance().GetBacklight();
        if (!backlight) return;
        const int value = std::clamp(static_cast<int>(backlight->brightness()) + delta, 10, 100);
        backlight->SetBrightness(static_cast<uint8_t>(value), true);
    });
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

    // Figma order, clockwise from the top-right. A 10-degree gap at each
    // boundary keeps the six actions visually and physically distinct.
    static constexpr int kScreenCenters[6] = {30, 90, 150, 210, 270, 330};

    for (int i = 0; i < 6; ++i) {
        lv_obj_t* arc = lv_arc_create(menu_container);
        menu_arcs[i] = arc;
        lv_obj_set_size(arc, 340, 340);
        lv_obj_center(arc);
        lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
        lv_obj_set_style_arc_width(arc, 70, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(arc, false, LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, lv_color_hex(!kMenuItems[i].available ? 0x111718 : 0x182021), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
        const int lvgl_center = (kScreenCenters[i] + 270) % 360;
        lv_arc_set_bg_angles(arc, (lvgl_center + 335) % 360, (lvgl_center + 25) % 360);
        lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);

        const float radians = kScreenCenters[i] * 3.14159265f / 180.0f;
        const int label_x = UI_BRIDGE_CENTER_X + static_cast<int>(134 * std::sin(radians));
        const int label_y = UI_BRIDGE_CENTER_Y - static_cast<int>(134 * std::cos(radians));

        menu_icons[i] = lv_label_create(menu_container);
        lv_label_set_text(menu_icons[i], kMenuItems[i].icon);
        lv_obj_set_style_text_font(menu_icons[i], i == 5 ? &lv_font_montserrat_20 : &BUILTIN_ICON_FONT, 0);
        lv_obj_set_style_text_color(menu_icons[i],
                                    kMenuItems[i].available ? lv_color_hex(0xF2F0EB) : lv_color_hex(0x777777), 0);
        lv_obj_align(menu_icons[i], LV_ALIGN_TOP_LEFT, label_x - 10, label_y - 22);

        menu_labels[i] = lv_label_create(menu_container);
        lv_label_set_text(menu_labels[i], kMenuItems[i].label);
        lv_obj_set_style_text_font(menu_labels[i], &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(menu_labels[i],
                                    kMenuItems[i].available ? lv_color_hex(0xF2F0EB) : lv_color_hex(0x777777), 0);
        lv_obj_set_style_text_align(menu_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(menu_labels[i], 96);
        lv_obj_align(menu_labels[i], LV_ALIGN_TOP_LEFT, label_x - 48, label_y + 3);
    }

    lv_obj_t* center = lv_obj_create(menu_container);
    lv_obj_set_size(center, 192, 192);
    lv_obj_center(center);
    lv_obj_set_style_radius(center, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(center, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(center, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(center, 2, 0);
    lv_obj_set_style_border_color(center, lv_color_hex(0xFF5A1F), 0);
    lv_obj_clear_flag(center, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(center, LV_OBJ_FLAG_CLICKABLE);

    menu_time_label = lv_label_create(center);
    lv_obj_set_style_text_font(menu_time_label, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(menu_time_label, lv_color_hex(0xF7F4EE), 0);
    lv_obj_align(menu_time_label, LV_ALIGN_CENTER, 0, -38);

    menu_date_label = lv_label_create(center);
    lv_obj_set_style_text_font(menu_date_label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(menu_date_label, lv_color_hex(0x8E9290), 0);
    lv_obj_align(menu_date_label, LV_ALIGN_CENTER, 0, 3);

    menu_status_label = lv_label_create(center);
    lv_obj_set_style_text_font(menu_status_label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(menu_status_label, lv_color_hex(0x8A8A8A), 0);
    lv_obj_set_style_text_align(menu_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(menu_status_label, 174);
    lv_obj_align(menu_status_label, LV_ALIGN_CENTER, 0, 39);

    lv_obj_t* hint = lv_label_create(center);
    lv_label_set_text(hint, "上滑收起");
    lv_obj_set_style_text_font(hint, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x555B59), 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 70);
    lv_obj_add_flag(menu_container, LV_OBJ_FLAG_HIDDEN);
}

void CreateTimerPage(lv_obj_t* screen) {
    timer_container = lv_obj_create(screen);
    lv_obj_set_size(timer_container, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_set_style_bg_color(timer_container, lv_color_black(), 0);
    lv_obj_set_style_border_width(timer_container, 0, 0);
    lv_obj_clear_flag(timer_container, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(timer_container);
    lv_label_set_text(title, "计时器");
    lv_obj_set_style_text_font(title, &BUILTIN_TEXT_FONT, 0);
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
    timer_action_label = CreateTextButton(timer_container, "开始", 118, 48, 0, 72, TimerActionEvent);
    CreateTextButton(timer_container, "取消", 96, 42, -57, 116, TimerCancelEvent);
    CreateTextButton(timer_container, "返回菜单", 106, 42, 62, 116, BackToMenuEvent);
    lv_obj_add_flag(timer_container, LV_OBJ_FLAG_HIDDEN);
}

void CreateCarSettingsPage(lv_obj_t* screen) {
    car_settings_container = lv_obj_create(screen);
    lv_obj_set_size(car_settings_container, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_set_style_bg_color(car_settings_container, lv_color_black(), 0);
    lv_obj_set_style_border_width(car_settings_container, 0, 0);
    lv_obj_clear_flag(car_settings_container, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(car_settings_container);
    lv_label_set_text(title, "车载模式设置");
    lv_obj_set_style_text_font(title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF8A35), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -116);

    settings_status_label = lv_label_create(car_settings_container);
    lv_obj_set_style_text_font(settings_status_label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(settings_status_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(settings_status_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(settings_status_label, LV_ALIGN_CENTER, 0, -88);

    lv_obj_t* help = lv_label_create(car_settings_container);
    lv_label_set_text(help, "静止约2秒完成校准；车头方向是设备朝车辆前方的轴");
    lv_obj_set_width(help, 286);
    lv_obj_set_style_text_font(help, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(help, lv_color_hex(0x929A98), 0);
    lv_obj_set_style_text_align(help, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(help, LV_ALIGN_CENTER, 0, -43);

    CreateTextButton(car_settings_container, "切换表情", 118, 42, -65, 6, PackEvent);
    CreateTextButton(car_settings_container, "车头方向", 118, 42, 65, 6, AxisEvent);
    CreateTextButton(car_settings_container, "重新校准", 142, 40, 0, 54, CalibrateEvent);
    settings_debug_label = CreateTextButton(car_settings_container, "调试显示：关", 154, 40, 0, 101, DebugEvent);
    CreateTextButton(car_settings_container, "返回菜单", 112, 36, 0, 145, BackToMenuEvent);
    lv_obj_add_flag(car_settings_container, LV_OBJ_FLAG_HIDDEN);
}

void CreateSystemSettingsPage(lv_obj_t* screen) {
    system_settings_container = lv_obj_create(screen);
    lv_obj_set_size(system_settings_container, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_set_style_bg_color(system_settings_container, lv_color_black(), 0);
    lv_obj_set_style_border_width(system_settings_container, 0, 0);
    lv_obj_clear_flag(system_settings_container, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(system_settings_container);
    lv_label_set_text(title, "系统设置");
    lv_obj_set_style_text_font(title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF8A35), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -126);

    system_touch_label = lv_label_create(system_settings_container);
    lv_obj_set_style_text_font(system_touch_label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(system_touch_label, lv_color_white(), 0);
    lv_obj_align(system_touch_label, LV_ALIGN_CENTER, -45, -75);
    CreateTextButton(system_settings_container, "切换", 82, 40, 93, -75, HeadTouchEvent);

    system_volume_label = lv_label_create(system_settings_container);
    lv_obj_set_style_text_font(system_volume_label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(system_volume_label, lv_color_white(), 0);
    lv_obj_align(system_volume_label, LV_ALIGN_CENTER, 0, -22);
    CreateTextButton(system_settings_container, "−", 62, 40, -77, 23, VolumeAdjustEvent,
                     reinterpret_cast<void*>(static_cast<intptr_t>(-10)));
    CreateTextButton(system_settings_container, "+", 62, 40, 77, 23, VolumeAdjustEvent,
                     reinterpret_cast<void*>(static_cast<intptr_t>(10)));

    system_brightness_label = lv_label_create(system_settings_container);
    lv_obj_set_style_text_font(system_brightness_label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(system_brightness_label, lv_color_white(), 0);
    lv_obj_align(system_brightness_label, LV_ALIGN_CENTER, 0, 68);
    CreateTextButton(system_settings_container, "−", 62, 40, -77, 108, BrightnessAdjustEvent,
                     reinterpret_cast<void*>(static_cast<intptr_t>(-10)));
    CreateTextButton(system_settings_container, "+", 62, 40, 77, 108, BrightnessAdjustEvent,
                     reinterpret_cast<void*>(static_cast<intptr_t>(10)));
    CreateTextButton(system_settings_container, "返回菜单", 118, 38, 0, 151, BackToMenuEvent);
    lv_obj_add_flag(system_settings_container, LV_OBJ_FLAG_HIDDEN);
}

void CreateInfoPage(lv_obj_t* screen) {
    info_container = lv_obj_create(screen);
    lv_obj_set_size(info_container, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_set_style_bg_color(info_container, lv_color_black(), 0);
    lv_obj_set_style_border_width(info_container, 0, 0);
    lv_obj_clear_flag(info_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* title = lv_label_create(info_container);
    lv_label_set_text(title, "系统信息");
    lv_obj_set_style_text_font(title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF8A35), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -136);
    lv_obj_t* scroll = lv_obj_create(info_container);
    lv_obj_set_size(scroll, 280, 218);
    lv_obj_align(scroll, LV_ALIGN_CENTER, 0, -16);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    info_label = lv_label_create(scroll);
    lv_obj_set_width(info_label, 235);
    lv_obj_set_style_text_font(info_label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(info_label, lv_color_white(), 0);
    lv_label_set_text(info_label, "正在读取设备信息...");
    CreateTextButton(info_container, "刷新", 106, 42, -62, 116, InfoRefreshEvent);
    CreateTextButton(info_container, "返回菜单", 112, 42, 62, 116, BackToMenuEvent);
    lv_obj_add_flag(info_container, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

extern "C" void feature_ui_create(void) {
    lv_obj_t* screen = lv_scr_act();
    CreateMenu(screen);
    CreateTimerPage(screen);
    CreateCarSettingsPage(screen);
    CreateSystemSettingsPage(screen);
    CreateInfoPage(screen);
    ui_bridge_register_page_with_cycle(PAGE_RING_MENU, &menu_container, false);
    ui_bridge_register_page_with_cycle(PAGE_TIMER, &timer_container, false);
    ui_bridge_register_page_with_cycle(PAGE_CAR_SETTINGS, &car_settings_container, false);
    ui_bridge_register_page_with_cycle(PAGE_SYSTEM_SETTINGS, &system_settings_container, false);
    ui_bridge_register_page_with_cycle(PAGE_DEVICE_INFO, &info_container, false);
    ui_refresh_timer = lv_timer_create(UiRefreshCallback, 250, nullptr);
    (void)ui_refresh_timer;
    UiRefreshCallback(nullptr);
    ESP_LOGI(kTag, "Feature UI initialized; services start after board construction");
}

extern "C" bool feature_ui_is_feature_page(const char* page_id) {
    return page_id && (std::strcmp(page_id, PAGE_RING_MENU) == 0 ||
                       std::strcmp(page_id, PAGE_TIMER) == 0 ||
                       std::strcmp(page_id, PAGE_CAR_SETTINGS) == 0 ||
                       std::strcmp(page_id, PAGE_SYSTEM_SETTINGS) == 0 ||
                       std::strcmp(page_id, PAGE_DEVICE_INFO) == 0);
}

extern "C" void feature_ui_open_menu(void) {
    OpenFeaturePage(PAGE_RING_MENU);
}

extern "C" void feature_ui_close_to_home(void) {
    OpenFeaturePage(UI_BRIDGE_PAGE_HOME);
}

extern "C" void feature_ui_on_ready() { ui_ready.store(true); }
