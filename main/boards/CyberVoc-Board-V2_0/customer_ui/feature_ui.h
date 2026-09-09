#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAGE_RING_MENU "RING_MENU"
#define PAGE_TIMER "TIMER"
#define PAGE_CAR_SETTINGS "CAR_SETTINGS"
#define PAGE_SYSTEM_SETTINGS "SYSTEM_SETTINGS"
#define PAGE_DEVICE_INFO "DEVICE_INFO"
#define PAGE_EXPRESSION_PACKS "EXPRESSION_PACKS"

typedef enum {
    FEATURE_TIMER_IDLE = 0,
    FEATURE_TIMER_RUNNING,
    FEATURE_TIMER_PAUSED,
    FEATURE_TIMER_FINISHED,
} feature_timer_state_t;

void feature_ui_create(void);
void feature_ui_on_ready(void);
void feature_services_start(void);
void feature_timer_poll(void);
int32_t feature_timer_get_selected_minutes(void);
void feature_timer_adjust_minutes(int32_t delta);
bool feature_ui_is_feature_page(const char* page_id);
void feature_ui_open_menu(void);
void feature_ui_close_to_home(void);

bool feature_timer_start_minutes(int32_t minutes);
bool feature_timer_pause(void);
bool feature_timer_resume(void);
bool feature_timer_cancel(void);
feature_timer_state_t feature_timer_get_state(void);
int32_t feature_timer_get_remaining_seconds(void);

bool feature_car_is_enabled(void);
bool feature_car_set_enabled(bool enabled);
void feature_car_recalibrate(void);
bool feature_car_is_calibrated(void);
int feature_car_get_calibration_progress(void);
const char* feature_car_get_event_name(void);
const char* feature_car_get_status(void);
bool feature_car_is_debug_enabled(void);
bool feature_car_set_debug_enabled(bool enabled);
bool feature_car_set_pack(int value);
int feature_car_get_pack(void);
bool feature_car_set_axis(int value);
int feature_car_get_axis(void);
void feature_car_feed_sample(float ax, float ay, float az,
                             float gx, float gy, float gz,
                             int64_t timestamp_ms);
void feature_car_mark_sensor_lost(int64_t timestamp_ms);

#ifdef __cplusplus
}
#endif
