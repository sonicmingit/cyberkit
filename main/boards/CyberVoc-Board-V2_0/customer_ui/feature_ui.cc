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
#include "../CyberVoc.h"
#include "display/display.h"
#include "settings.h"
#include "../ui_bridge.h"
#include "../car_mode/expression_pack_manager.h"

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
lv_obj_t* expression_packs_container = nullptr;
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
lv_obj_t* system_shake_label = nullptr;
lv_obj_t* system_aec_label = nullptr;
lv_obj_t* system_doa_label = nullptr;
lv_obj_t* system_volume_label = nullptr;
lv_obj_t* system_brightness_label = nullptr;
lv_obj_t* packs_current_label = nullptr;
lv_obj_t* packs_status_label = nullptr;
lv_obj_t* packs_list = nullptr;
uint32_t packs_ui_revision = UINT32_MAX;
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
    char status[192];
    char pack_label[80]{};
    expression_packs::CurrentLabel(pack_label, sizeof(pack_label));
    const char* raw_status = feature_car_get_status();
    if (std::strcmp(raw_status, "KEEP STILL") == 0) {
        std::snprintf(status, sizeof(status), "状态：%s %d%%\n表情：%s  车头方向：%s",
                      CarStatusText(raw_status), feature_car_get_calibration_progress(),
                      pack_label, axes[feature_car_get_axis()]);
    } else {
        std::snprintf(status, sizeof(status), "状态：%s\n表情：%s  车头方向：%s",
                      CarStatusText(raw_status),
                      pack_label, axes[feature_car_get_axis()]);
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
    auto set_switch = [](lv_obj_t* label, bool on) {
        if (!label) return;
        lv_label_set_text(label, on ? "开" : "关");
        lv_obj_set_style_bg_color(lv_obj_get_parent(label),
                                  lv_color_hex(on ? 0xFF5A1F : 0x263032), 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
    };
    if (system_touch_label) {
        set_switch(system_touch_label, board.IsHeadTouchEnabled());
    }
    if (system_shake_label) {
        set_switch(system_shake_label, board.IsShakeEnabled());
    }
    if (system_aec_label) {
        set_switch(system_aec_label, Application::GetInstance().GetAecMode() != kAecOff);
    }
    if (system_doa_label) {
        auto* cat = dynamic_cast<EspS3Cat*>(&board);
        set_switch(system_doa_label,
                   cat && cat->GetAudioAnalysisMode() == AudioAnalysisMode::DOA_FOLLOW);
    }
    if (system_volume_label) {
        auto* codec = board.GetAudioCodec();
        char text[32];
        std::snprintf(text, sizeof(text), "%d", codec ? codec->output_volume() : 0);
        lv_label_set_text(system_volume_label, text);
    }
    if (system_brightness_label) {
        auto* backlight = board.GetBacklight();
        char text[32];
        std::snprintf(text, sizeof(text), "%u", backlight ? backlight->brightness() : 0);
        lv_label_set_text(system_brightness_label, text);
    }
}

void RebuildPackList();

void UpdatePackLabels() {
    if (!ui_ready.load()) return;
    const uint32_t current_revision = expression_packs::Revision();
    if (packs_ui_revision != current_revision) {
        packs_ui_revision = current_revision;
        RebuildPackList();
    }
    if (packs_current_label) {
        char current[96]{};
        expression_packs::CurrentLabel(current, sizeof(current));
        char text[120];
        std::snprintf(text, sizeof(text), "当前：%s", current);
        lv_label_set_text(packs_current_label, text);
    }
    if (packs_status_label) {
        char text[112]{};
        expression_packs::StatusText(text, sizeof(text));
        lv_label_set_text(packs_status_label, text);
    }
}

void UiRefreshCallback(lv_timer_t*) {
    UpdateClockLabels();
    UpdateTimerLabels();
    if (!ui_ready.load()) return; // Board is still being constructed.
    UpdateMenuState();
    UpdateSettingsLabel();
    UpdateSystemSettingsLabels();
    UpdatePackLabels();
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
    {FONT_AWESOME_SD_CARD, "TF表情包", true, expression_packs::IsTfSelected,
        [](bool) { OpenFeaturePage(PAGE_EXPRESSION_PACKS); }},
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
    Application::GetInstance().Schedule([]() { OpenFeaturePage(PAGE_EXPRESSION_PACKS); });
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
void ShakeEvent(lv_event_t*) {
    Application::GetInstance().Schedule([]() {
        auto& board = Board::GetInstance();
        board.SetShakeEnabled(!board.IsShakeEnabled());
    });
}
void AecEvent(lv_event_t*) {
    Application::GetInstance().Schedule([]() {
        auto& app = Application::GetInstance();
        app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
    });
}
void DoaEvent(lv_event_t*) {
    Application::GetInstance().Schedule([]() {
        auto* cat = dynamic_cast<EspS3Cat*>(&Board::GetInstance());
        if (!cat) return;
        cat->SetAudioAnalysisMode(cat->GetAudioAnalysisMode() == AudioAnalysisMode::DOA_FOLLOW ?
                                  AudioAnalysisMode::DISABLED : AudioAnalysisMode::DOA_FOLLOW);
    });
}
void PackRescanEvent(lv_event_t*) {
    Application::GetInstance().Schedule([]() { expression_packs::RequestScan(); });
}
void PackSelectEvent(lv_event_t* event) {
    const size_t index = static_cast<size_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
    Application::GetInstance().Schedule([index]() {
        expression_packs::UiDescriptor items[34]{};
        const size_t count = expression_packs::CopyUiDescriptors(items, 34);
        if (index >= count || !expression_packs::Select(items[index].source, items[index].pack_id,
                                                        items[index].version)) {
            Board::GetInstance().GetDisplay()->ShowNotification("表情包切换失败");
        }
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
        lv_obj_set_style_text_font(menu_icons[i], &BUILTIN_ICON_FONT, 0);
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

    CreateTextButton(car_settings_container, "表情包", 118, 42, -65, 6, PackEvent);
    CreateTextButton(car_settings_container, "车头方向", 118, 42, 65, 6, AxisEvent);
    CreateTextButton(car_settings_container, "重新校准", 142, 40, 0, 54, CalibrateEvent);
    settings_debug_label = CreateTextButton(car_settings_container, "调试显示：关", 154, 40, 0, 101, DebugEvent);
    CreateTextButton(car_settings_container, "返回菜单", 112, 36, 0, 145, BackToMenuEvent);
    lv_obj_add_flag(car_settings_container, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* CreateSwitchRow(lv_obj_t* parent, const char* name, lv_coord_t y, lv_event_cb_t callback) {
    lv_obj_t* name_label = lv_label_create(parent);
    lv_label_set_text(name_label, name);
    lv_obj_set_style_text_font(name_label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(name_label, lv_color_white(), 0);
    lv_obj_set_pos(name_label, 18, y + 8);

    lv_obj_t* button = lv_btn_create(parent);
    lv_obj_set_size(button, 72, 36);
    lv_obj_set_pos(button, 190, y);
    lv_obj_set_style_radius(button, 18, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x263032), 0);
    lv_obj_set_style_border_width(button, 1, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x596769), 0);
    lv_obj_add_event_cb(button, [](lv_event_t*) { ui_bridge_track_pointer(); }, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(button, [](lv_event_t* event) {
        if (!ui_bridge_click_allowed()) lv_event_stop_processing(event);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* value = lv_label_create(button);
    lv_label_set_text(value, "关");
    lv_obj_set_style_text_font(value, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(value, lv_color_hex(0xFF6A24), 0);
    lv_obj_center(value);
    return value;
}

lv_obj_t* CreateSmallControl(lv_obj_t* parent, const char* text, lv_coord_t x, lv_coord_t y,
                             lv_event_cb_t callback, intptr_t value) {
    lv_obj_t* button = lv_btn_create(parent);
    lv_obj_set_size(button, 42, 36);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_style_radius(button, 18, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x263032), 0);
    lv_obj_add_event_cb(button, [](lv_event_t*) { ui_bridge_track_pointer(); }, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(button, [](lv_event_t* event) {
        if (!ui_bridge_click_allowed()) lv_event_stop_processing(event);
    }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(value));
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
    return label;
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
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -145);

    lv_obj_t* scroll = lv_obj_create(system_settings_container);
    lv_obj_set_size(scroll, 294, 252);
    lv_obj_align(scroll, LV_ALIGN_CENTER, 0, -5);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_pad_all(scroll, 4, 0);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_ACTIVE);

    system_touch_label = CreateSwitchRow(scroll, "头部触摸", 4, HeadTouchEvent);
    system_shake_label = CreateSwitchRow(scroll, "摇晃响应", 48, ShakeEvent);
    system_aec_label = CreateSwitchRow(scroll, "实时聊天", 92, AecEvent);
    system_doa_label = CreateSwitchRow(scroll, "声源跟随", 136, DoaEvent);

    lv_obj_t* volume_name = lv_label_create(scroll);
    lv_label_set_text(volume_name, "音量");
    lv_obj_set_style_text_font(volume_name, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(volume_name, lv_color_white(), 0);
    lv_obj_set_pos(volume_name, 18, 190);
    system_volume_label = lv_label_create(scroll);
    lv_obj_set_style_text_font(system_volume_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(system_volume_label, lv_color_white(), 0);
    lv_obj_set_pos(system_volume_label, 205, 190);
    CreateSmallControl(scroll, "-", 148, 182, VolumeAdjustEvent, -10);
    CreateSmallControl(scroll, "+", 232, 182, VolumeAdjustEvent, 10);

    lv_obj_t* brightness_name = lv_label_create(scroll);
    lv_label_set_text(brightness_name, "亮度");
    lv_obj_set_style_text_font(brightness_name, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(brightness_name, lv_color_white(), 0);
    lv_obj_set_pos(brightness_name, 18, 242);
    system_brightness_label = lv_label_create(scroll);
    lv_obj_set_style_text_font(system_brightness_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(system_brightness_label, lv_color_white(), 0);
    lv_obj_set_pos(system_brightness_label, 205, 242);
    CreateSmallControl(scroll, "-", 148, 234, BrightnessAdjustEvent, -10);
    CreateSmallControl(scroll, "+", 232, 234, BrightnessAdjustEvent, 10);

    CreateTextButton(system_settings_container, "返回菜单", 118, 38, 0, 148, BackToMenuEvent);
    lv_obj_add_flag(system_settings_container, LV_OBJ_FLAG_HIDDEN);
}

void RebuildPackList() {
    if (!packs_list) return;
    lv_obj_clean(packs_list);
    expression_packs::UiDescriptor items[34]{};
    const size_t count = expression_packs::CopyUiDescriptors(items, 34);
    for (size_t i = 0; i < count; ++i) {
        lv_obj_t* button = lv_btn_create(packs_list);
        lv_obj_set_size(button, 274, 50);
        lv_obj_set_pos(button, 4, static_cast<lv_coord_t>(i * 56 + 2));
        lv_obj_set_style_radius(button, 15, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x151D1E), 0);
        lv_obj_set_style_border_width(button, items[i].selected ? 2 : 1, 0);
        lv_obj_set_style_border_color(button,
                                      lv_color_hex(items[i].selected ? 0xFF5A1F : 0x465153), 0);
        lv_obj_add_event_cb(button, [](lv_event_t*) { ui_bridge_track_pointer(); }, LV_EVENT_PRESSING,
                            nullptr);
        lv_obj_add_event_cb(button, [](lv_event_t* event) {
            if (!ui_bridge_click_allowed()) lv_event_stop_processing(event);
        }, LV_EVENT_CLICKED, nullptr);
        lv_obj_add_event_cb(button, PackSelectEvent, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<uintptr_t>(i)));

        char name[92];
        std::snprintf(name, sizeof(name), "%s%s v%d", items[i].selected ? "● " : "○ ",
                      items[i].display_name, items[i].version);
        lv_obj_t* name_label = lv_label_create(button);
        lv_label_set_text(name_label, name);
        lv_obj_set_style_text_font(name_label, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(name_label,
                                    lv_color_hex(items[i].selected ? 0xFF8A35 : 0xF2F0EB), 0);
        lv_obj_align(name_label, LV_ALIGN_LEFT_MID, 6, 0);

        lv_obj_t* source = lv_label_create(button);
        lv_label_set_text(source, items[i].source == expression_packs::Source::Tf ? "TF卡" : "内置");
        lv_obj_set_style_text_font(source, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(source, lv_color_hex(0x9CA5A3), 0);
        lv_obj_align(source, LV_ALIGN_RIGHT_MID, -6, 0);
    }
}

void CreateExpressionPacksPage(lv_obj_t* screen) {
    expression_packs_container = lv_obj_create(screen);
    lv_obj_set_size(expression_packs_container, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_set_style_bg_color(expression_packs_container, lv_color_black(), 0);
    lv_obj_set_style_border_width(expression_packs_container, 0, 0);
    lv_obj_clear_flag(expression_packs_container, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* title = lv_label_create(expression_packs_container);
    lv_label_set_text(title, "表情包");
    lv_obj_set_style_text_font(title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFF8A35), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -145);

    packs_current_label = lv_label_create(expression_packs_container);
    lv_obj_set_width(packs_current_label, 280);
    lv_obj_set_style_text_font(packs_current_label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(packs_current_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(packs_current_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(packs_current_label, LV_ALIGN_CENTER, 0, -112);

    packs_list = lv_obj_create(expression_packs_container);
    lv_obj_set_size(packs_list, 294, 190);
    lv_obj_align(packs_list, LV_ALIGN_CENTER, 0, -4);
    lv_obj_set_style_bg_opa(packs_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(packs_list, 0, 0);
    lv_obj_set_style_pad_all(packs_list, 2, 0);
    lv_obj_set_scroll_dir(packs_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(packs_list, LV_SCROLLBAR_MODE_ACTIVE);

    packs_status_label = lv_label_create(expression_packs_container);
    lv_obj_set_width(packs_status_label, 292);
    lv_obj_set_style_text_font(packs_status_label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(packs_status_label, lv_color_hex(0x909896), 0);
    lv_obj_set_style_text_align(packs_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(packs_status_label, LV_LABEL_LONG_DOT);
    lv_obj_align(packs_status_label, LV_ALIGN_CENTER, 0, 105);

    CreateTextButton(expression_packs_container, "重新扫描", 118, 40, -65, 145, PackRescanEvent);
    CreateTextButton(expression_packs_container, "返回菜单", 118, 40, 65, 145, BackToMenuEvent);
    lv_obj_add_flag(expression_packs_container, LV_OBJ_FLAG_HIDDEN);
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
    CreateExpressionPacksPage(screen);
    CreateInfoPage(screen);
    ui_bridge_register_page_with_cycle(PAGE_RING_MENU, &menu_container, false);
    ui_bridge_register_page_with_cycle(PAGE_TIMER, &timer_container, false);
    ui_bridge_register_page_with_cycle(PAGE_CAR_SETTINGS, &car_settings_container, false);
    ui_bridge_register_page_with_cycle(PAGE_SYSTEM_SETTINGS, &system_settings_container, false);
    ui_bridge_register_page_with_cycle(PAGE_EXPRESSION_PACKS, &expression_packs_container, false);
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
                        std::strcmp(page_id, PAGE_EXPRESSION_PACKS) == 0 ||
                        std::strcmp(page_id, PAGE_DEVICE_INFO) == 0);
}

extern "C" void feature_ui_open_menu(void) {
    OpenFeaturePage(PAGE_RING_MENU);
}

extern "C" void feature_ui_close_to_home(void) {
    OpenFeaturePage(UI_BRIDGE_PAGE_HOME);
}

extern "C" void feature_ui_on_ready() { ui_ready.store(true); }
