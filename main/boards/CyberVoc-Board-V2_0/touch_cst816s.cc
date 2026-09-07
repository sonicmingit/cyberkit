#include "touch_cst816s.h"
#include "config.h"
#include "ui_bridge.h"
#include <esp_log.h>
#include <esp_lcd_touch_cst816s.h>
#include <esp_lcd_panel_io.h>

#define TAG "Cst816s"

bool Cst816sTouch::init(int width,
                        int height,
                        bool swap_xy,
                        bool mirror_x,
                        bool mirror_y)
{
    ESP_LOGI(TAG, "Initializing CST816S touch controller");

    if (i2c_bus_ == NULL) {
        ESP_LOGE(TAG, "I2C bus handle is NULL");
        return false;
    }

    // Configure touch panel parameters
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = static_cast<uint16_t>(width),
        .y_max = static_cast<uint16_t>(height),
        .rst_gpio_num = TP_PIN_NUM_RST,
        .int_gpio_num = TP_PIN_NUM_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
    };

    // Create panel IO handle for touch controller
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = {
        .dev_addr = ESP_LCD_TOUCH_IO_I2C_CST816S_ADDRESS,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 0,
        .flags = {
            .dc_low_on_data = 0,
            .disable_control_phase = 1,
        },
        .scl_speed_hz = 400000,
    };
    
    esp_err_t ret = esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create panel IO for touch controller: %s", esp_err_to_name(ret));
        return false;
    }

    io_ = tp_io_handle;
    vTaskDelay(pdMS_TO_TICKS(100));

    // Initialize CST816S touch controller
    ret = esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, &handle_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create CST816S touch driver: %s", esp_err_to_name(ret));
        return false;
    }

    ESP_LOGI(TAG, "CST816S touch controller initialized successfully");
    return true;
}

// Retain adapter IRQ wake-up, but do not trust a cached press indefinitely.
void Cst816sTouch::attach_release_fallback(lv_indev_t* input) {
    active_ = this;
    driver_read_ = handle_->read_data;
    handle_->read_data = tracked_read;
    adapter_read_ = lv_indev_get_read_cb(input);
    last_.state = LV_INDEV_STATE_RELEASED;
    lv_indev_set_read_cb(input, read_input);
}

esp_err_t Cst816sTouch::tracked_read(esp_lcd_touch_handle_t handle) {
    auto* self = active_;
    self->fresh_ = true;
    self->read_result_ = self->driver_read_(handle);
    return self->read_result_;
}

void Cst816sTouch::read_input(lv_indev_t* input, lv_indev_data_t* data) {
    auto* self = active_;
    self->fresh_ = false;
    self->adapter_read_(input, data);
    if (!self->fresh_) {
        *data = self->last_;
        if (data->state == LV_INDEV_STATE_PRESSED) {
            // Poll only until release; sleeping idle controllers are left alone.
            const auto err = tracked_read(self->handle_);
            esp_lcd_touch_point_data_t point{};
            uint8_t count = 0;
            if (err == ESP_OK && esp_lcd_touch_get_data(self->handle_, &point, &count, 1) == ESP_OK && count) {
                data->point.x = point.x;
                data->point.y = point.y;
            } else data->state = LV_INDEV_STATE_RELEASED;
        }
    }
    if ((self->fresh_ && self->read_result_ != ESP_OK) ||
        (data->state == LV_INDEV_STATE_PRESSED &&
         (data->point.x < 0 || data->point.x >= self->handle_->config.x_max ||
          data->point.y < 0 || data->point.y >= self->handle_->config.y_max))) {
        // Bus errors must not become a synthetic successful click.
        ui_bridge_cancel_pointer();
        data->state = LV_INDEV_STATE_RELEASED;
        data->point = self->last_.point;
    }
    data->continue_reading = false;
    self->last_ = *data;
}
