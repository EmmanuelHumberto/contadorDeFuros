#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#define DISPLAY_H_RES 800
#define DISPLAY_V_RES 480

typedef struct {
    esp_lcd_panel_handle_t panel;
    lv_display_t *lvgl_display;
    lv_indev_t *touch_indev;
    esp_lcd_touch_handle_t touch_handle;
} display_driver_t;

esp_err_t display_driver_init(display_driver_t *driver);
void display_driver_set_backlight(bool enabled);
