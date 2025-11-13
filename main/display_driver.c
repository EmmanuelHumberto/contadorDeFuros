#include "display_driver.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

#define LCD_DRAW_BUFFER_HEIGHT 80
#define LCD_BOUNCE_BUFFER_LINES 10

#define LCD_RGB_TIMING()                   \
    {                                      \
        .pclk_hz = 18 * 1000 * 1000,       \
        .h_res = DISPLAY_H_RES,            \
        .v_res = DISPLAY_V_RES,            \
        .hsync_pulse_width = 48,           \
        .hsync_back_porch = 40,            \
        .hsync_front_porch = 40,           \
        .vsync_pulse_width = 3,            \
        .vsync_back_porch = 29,            \
        .vsync_front_porch = 13,           \
        .flags.pclk_active_neg = true,     \
    }

#define LCD_PIN_DE        GPIO_NUM_5
#define LCD_PIN_VSYNC     GPIO_NUM_3
#define LCD_PIN_HSYNC     GPIO_NUM_46
#define LCD_PIN_PCLK      GPIO_NUM_7
#define LCD_PIN_DISP_EN   GPIO_NUM_NC
#define LCD_BACKLIGHT_GPIO_NUM (-1)

static const int s_lcd_data_pins[16] = {
    GPIO_NUM_14, GPIO_NUM_38, GPIO_NUM_18, GPIO_NUM_17,
    GPIO_NUM_10, GPIO_NUM_39, GPIO_NUM_0,  GPIO_NUM_45,
    GPIO_NUM_48, GPIO_NUM_47, GPIO_NUM_21, GPIO_NUM_1,
    GPIO_NUM_2,  GPIO_NUM_42, GPIO_NUM_41, GPIO_NUM_40,
};

#define TOUCH_I2C_PORT   0
#define TOUCH_I2C_CLK_HZ 400000
#define TOUCH_I2C_SCL    GPIO_NUM_9
#define TOUCH_I2C_SDA    GPIO_NUM_8
#define TOUCH_RST_GPIO   GPIO_NUM_NC
#define TOUCH_INT_GPIO   GPIO_NUM_4

static const char *TAG = "display_driver";

static esp_err_t init_rgb_panel(esp_lcd_panel_handle_t *panel_handle);
static esp_err_t init_lvgl_port(esp_lcd_panel_handle_t panel_handle, lv_display_t **display);
static esp_err_t init_touch_panel(lv_display_t *display, esp_lcd_touch_handle_t *touch_handle, lv_indev_t **indev);

esp_err_t display_driver_init(display_driver_t *driver)
{
    if (!driver) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(driver, 0, sizeof(*driver));

    ESP_RETURN_ON_ERROR(init_rgb_panel(&driver->panel), TAG, "Falha painel RGB");
    ESP_RETURN_ON_ERROR(init_lvgl_port(driver->panel, &driver->lvgl_display), TAG, "Falha LVGL");
    ESP_RETURN_ON_ERROR(init_touch_panel(driver->lvgl_display, &driver->touch_handle, &driver->touch_indev),
                        TAG, "Falha touch");
    display_driver_set_backlight(true);
    return ESP_OK;
}

void display_driver_set_backlight(bool enabled)
{
#if LCD_BACKLIGHT_GPIO_NUM >= 0
    gpio_config_t io_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << LCD_BACKLIGHT_GPIO_NUM,
    };
    gpio_config(&io_conf);
    gpio_set_level(LCD_BACKLIGHT_GPIO_NUM, enabled ? 1 : 0);
#else
    (void)enabled;
#endif
}

static esp_err_t init_rgb_panel(esp_lcd_panel_handle_t *panel_handle)
{
    esp_lcd_rgb_panel_config_t config = {
        .clk_src = LCD_CLK_SRC_PLL160M,
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 3, 0)
        .psram_trans_align = 64,
#else
        .dma_burst_size = 64,
#endif
        .data_width = 16,
        .de_gpio_num = LCD_PIN_DE,
        .pclk_gpio_num = LCD_PIN_PCLK,
        .vsync_gpio_num = LCD_PIN_VSYNC,
        .hsync_gpio_num = LCD_PIN_HSYNC,
        .disp_gpio_num = LCD_PIN_DISP_EN,
        .timings = LCD_RGB_TIMING(),
        .flags = {
            .fb_in_psram = 1,
        },
        .num_fbs = 2,
    };

    for (size_t i = 0; i < 16; i++) {
        config.data_gpio_nums[i] = s_lcd_data_pins[i];
    }

    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&config, panel_handle), TAG, "esp_lcd_new_rgb_panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel_handle), TAG, "panel_reset");
    esp_err_t err = esp_lcd_panel_init(*panel_handle);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "panel_init not supported, ignoring");
    } else {
        ESP_RETURN_ON_ERROR(err, TAG, "panel_init");
    }

    err = esp_lcd_panel_disp_on_off(*panel_handle, true);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_RETURN_ON_ERROR(err, TAG, "disp_on");
    }
    return ESP_OK;
}

static esp_err_t init_lvgl_port(esp_lcd_panel_handle_t panel_handle, lv_display_t **display)
{
    if (!display) {
        return ESP_ERR_INVALID_ARG;
    }

    const lvgl_port_cfg_t lv_port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lv_port_cfg), TAG, "lvgl_port_init");

    lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = panel_handle,
        .buffer_size = DISPLAY_H_RES * LCD_DRAW_BUFFER_HEIGHT,
        .double_buffer = false,
        .hres = DISPLAY_H_RES,
        .vres = DISPLAY_V_RES,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
            .full_refresh = false,
            .direct_mode = true,
        },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = true,
        },
    };

    *display = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    ESP_RETURN_ON_FALSE(*display != NULL, ESP_FAIL, TAG, "lvgl_port_add_disp_rgb");
    return ESP_OK;
}

static esp_err_t init_touch_panel(lv_display_t *display, esp_lcd_touch_handle_t *touch_handle, lv_indev_t **indev)
{
    if (!display || !touch_handle || !indev) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_bus_handle_t i2c_bus = NULL;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = TOUCH_I2C_SDA,
        .scl_io_num = TOUCH_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &i2c_bus), TAG, "i2c_new_master_bus");

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_cfg.scl_speed_hz = TOUCH_I2C_CLK_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io_handle), TAG, "new_panel_io_i2c");

    const esp_lcd_touch_config_t touch_cfg = {
        .x_max = DISPLAY_H_RES,
        .y_max = DISPLAY_V_RES,
        .rst_gpio_num = TOUCH_RST_GPIO,
        .int_gpio_num = TOUCH_INT_GPIO,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(tp_io_handle, &touch_cfg, touch_handle), TAG,
                        "touch_new_gt911");

    const lvgl_port_touch_cfg_t lv_touch_cfg = {
        .disp = display,
        .handle = *touch_handle,
    };
    *indev = lvgl_port_add_touch(&lv_touch_cfg);
    ESP_RETURN_ON_FALSE(*indev != NULL, ESP_FAIL, TAG, "lvgl_port_add_touch");
    return ESP_OK;
}
