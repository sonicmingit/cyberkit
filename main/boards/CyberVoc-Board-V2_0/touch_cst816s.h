#ifndef CST816S_TOUCH_H
#define CST816S_TOUCH_H

#include "i2c_device.h"
#include <driver/i2c_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_lcd_touch.h>
#include <esp_err.h>
#include <esp_lcd_panel_io.h>
#include <lvgl.h>

/**
 * @brief Wrapper class for CST816S touch controller initialization
 */
class Cst816sTouch {
public:
    Cst816sTouch(i2c_master_bus_handle_t i2c_bus)
        : i2c_bus_(i2c_bus),
          handle_(nullptr)
    {
    }

    ~Cst816sTouch()
    {
        if (handle_) esp_lcd_touch_del(handle_);
        if (io_) esp_lcd_panel_io_del(io_);
    }

    /**
     * @brief Initialize CST816S touch controller
     *
     * @param width Touch panel width
     * @param height Touch panel height
     * @param swap_xy Swap X and Y coordinates
     * @param mirror_x Mirror X axis
     * @param mirror_y Mirror Y axis
     * @return bool true on success
     */
    bool init(int width,
              int height,
              bool swap_xy,
              bool mirror_x,
              bool mirror_y);

    void attach_release_fallback(lv_indev_t* input);

    esp_lcd_touch_handle_t get_handle() const
    {
        return handle_;
    }

private:
    i2c_master_bus_handle_t i2c_bus_;
    esp_lcd_touch_handle_t handle_;
    esp_lcd_panel_io_handle_t io_ = nullptr;
    // One CST816S on this board. Both callbacks execute on the LVGL task.
    inline static Cst816sTouch* active_ = nullptr;
    esp_err_t (*driver_read_)(esp_lcd_touch_handle_t) = nullptr;
    lv_indev_read_cb_t adapter_read_ = nullptr;
    bool fresh_ = false;
    esp_err_t read_result_ = ESP_OK;
    lv_indev_data_t last_{};
    static esp_err_t tracked_read(esp_lcd_touch_handle_t handle);
    static void read_input(lv_indev_t* input, lv_indev_data_t* data);
};

#endif // CST816S_TOUCH_H
