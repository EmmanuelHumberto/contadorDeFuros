#include "interface_usuario.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "wifi_manager.h"
#include "lvgl/src/widgets/span/lv_span.h"

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_28);
LV_FONT_DECLARE(lv_font_montserrat_48);
LV_IMAGE_DECLARE(liga_d_logo);

#define LCD_H_RES (800)
#define LCD_V_RES (480)

#define LCD_DRAW_BUFFER_HEIGHT  (80)
#define LCD_BOUNCE_BUFFER_LINES (10)

#define THEME_BG_DARK        0x05070A
#define THEME_BG_DARK_PANEL  0x0C121B

/* RGB interface timing (close to ST7262 reference timings) */
#define LCD_RGB_TIMING()                   \
    {                                      \
        .pclk_hz = 18 * 1000 * 1000,       \
        .h_res = LCD_H_RES,                \
        .v_res = LCD_V_RES,                \
        .hsync_pulse_width = 48,           \
        .hsync_back_porch = 40,            \
        .hsync_front_porch = 40,           \
        .vsync_pulse_width = 3,            \
        .vsync_back_porch = 29,            \
        .vsync_front_porch = 13,           \
        .flags.pclk_active_neg = true,     \
    }

/* GPIO mapping extracted do esquematico do kit Waveshare */
#define LCD_PIN_DE        (GPIO_NUM_5)
#define LCD_PIN_VSYNC     (GPIO_NUM_3)
#define LCD_PIN_HSYNC     (GPIO_NUM_46)
#define LCD_PIN_PCLK      (GPIO_NUM_7)
#define LCD_PIN_DISP_EN   (GPIO_NUM_NC)
#define LCD_BACKLIGHT_GPIO_NUM (-1)
#define LCD_PIN_BACKLIGHT ((gpio_num_t)LCD_BACKLIGHT_GPIO_NUM)

static const int s_lcd_data_pins[16] = {
    GPIO_NUM_14,
    GPIO_NUM_38,
    GPIO_NUM_18,
    GPIO_NUM_17,
    GPIO_NUM_10,
    GPIO_NUM_39,
    GPIO_NUM_0,
    GPIO_NUM_45,
    GPIO_NUM_48,
    GPIO_NUM_47,
    GPIO_NUM_21,
    GPIO_NUM_1,
    GPIO_NUM_2,
    GPIO_NUM_42,
    GPIO_NUM_41,
    GPIO_NUM_40,
};

/* GT911 touch controller */
#define TOUCH_I2C_PORT       (0)
#define TOUCH_I2C_CLK_HZ     (400000)
#define TOUCH_I2C_SCL        (GPIO_NUM_9)
#define TOUCH_I2C_SDA        (GPIO_NUM_8)
#define TOUCH_RST_GPIO       (GPIO_NUM_NC)
#define TOUCH_INT_GPIO       (GPIO_NUM_4)

#define SCOPE_POINT_COUNT            (100)
#define FREQUENCIA_MAXIMA_HZ         (220.0f)
#define WIFI_MAX_SSID_LEN            (32)
#define WIFI_MAX_PASS_LEN            (64)
#define WIFI_CONNECT_TIMEOUT_MS      (15000)

typedef dados_medidos_t ui_data_t;

typedef enum {
    DISPLAY_FREQUENCIA = 0,
    DISPLAY_RPM,
    DISPLAY_VELOCIDADE,
    DISPLAY_CURSO,
    DISPLAY_DISTANCIA,
    DISPLAY_FUROS,
    DISPLAY_MODE_COUNT,
} display_mode_t;

static const char *TAG = "interface_ui";

static esp_lcd_panel_handle_t panel_handle;
static lv_display_t *lvgl_disp;
static lv_indev_t *lvgl_touch_indev;
static esp_lcd_touch_handle_t touch_handle;

/* Objetos LVGL */
typedef struct {
    lv_obj_t *card;
    lv_obj_t *label_titulo;
    lv_obj_t *label_valor;
    lv_obj_t *label_unidade;
} card_ui_t;

typedef enum {
    UI_LAYOUT_GRID = 0,
    UI_LAYOUT_FULLSCREEN,
} ui_layout_t;

static lv_obj_t *s_grid_container;
static lv_obj_t *s_bottom_bar;
static lv_obj_t *s_fullscreen_container;
static lv_obj_t *s_full_title;
static lv_obj_t *s_full_value;
static lv_obj_t *s_full_unit;
static lv_obj_t *s_full_status;
static lv_obj_t *s_full_arc;
static lv_obj_t *s_full_arc_label;
static lv_obj_t *s_full_bar;
static lv_obj_t *s_full_bar_label;
static lv_obj_t *s_full_timer_label;
static lv_obj_t *s_full_scope_chart;
static lv_chart_series_t *s_full_scope_series;
static lv_obj_t *s_full_scope_axis_label;
static lv_obj_t *s_full_course_arc;
static lv_obj_t *s_speed_bar;
static lv_obj_t *s_speed_bar_label;
static lv_obj_t *s_furos_circle_left;
static lv_obj_t *s_furos_circle_right;
static lv_obj_t *s_wifi_button;
static lv_obj_t *s_wifi_status_label;
static lv_obj_t *s_wifi_quick_disconnect_btn;
static lv_obj_t *s_wifi_panel;
static lv_obj_t *s_wifi_ssid_ta;
static lv_obj_t *s_wifi_pass_ta;
static lv_obj_t *s_wifi_feedback_label;
static lv_obj_t *s_wifi_list;
static lv_obj_t *s_wifi_keyboard;
static lv_obj_t *s_wifi_keyboard_target;
static card_ui_t s_cards[DISPLAY_MODE_COUNT];
static ui_layout_t s_layout_mode = UI_LAYOUT_GRID;
static bool s_wifi_panel_visible = false;
static char s_wifi_selected_ssid[WIFI_MANAGER_MAX_SSID_LEN + 1] = {0};
static wifi_status_info_t s_wifi_status_cache = {0};
static wifi_ap_record_t s_wifi_network_cache[WIFI_MANAGER_MAX_SCAN_RESULTS];
static uint16_t s_wifi_network_count = 0;
static bool s_wifi_scanning = false;
static int64_t s_wifi_connect_start_us = 0;
static bool s_wifi_timeout_reported = false;
static lv_timer_t *s_wifi_status_timer = NULL;

static const char *const s_metric_titles[DISPLAY_MODE_COUNT] = {
    [DISPLAY_FREQUENCIA] = "Frequencia",
    [DISPLAY_RPM] = "RPM",
    [DISPLAY_VELOCIDADE] = "Velocidade",
    [DISPLAY_CURSO] = "Curso",
    [DISPLAY_DISTANCIA] = "Distancia",
    [DISPLAY_FUROS] = "Total de furos",
};

static const uint32_t s_metric_colors[DISPLAY_MODE_COUNT] = {
    [DISPLAY_FREQUENCIA] = 0x7FC8FF,
    [DISPLAY_RPM] = 0x4DD0E1,
    [DISPLAY_VELOCIDADE] = 0xC792EA,
    [DISPLAY_CURSO] = 0xFFB74D,
    [DISPLAY_DISTANCIA] = 0x8DE0AD,
    [DISPLAY_FUROS] = 0xFF88A5,
};

/* Estado dos dados exibidos */
static configuracao_curso_t s_config_curso = {.curso_cm = CURSO_MAX_CM * 0.7f};
static ui_callbacks_t s_callbacks = {0};
static bool s_modo_edicao = false;
static display_mode_t s_display_mode = DISPLAY_FREQUENCIA;
static ui_data_t s_ui_snapshot = {0};
static portMUX_TYPE s_ui_spinlock = portMUX_INITIALIZER_UNLOCKED;

/* Prototipacao */
static esp_err_t init_rgb_panel(void);
static esp_err_t init_lvgl_port(void);
static esp_err_t init_touch_panel(void);
static void build_ui(void);
static void show_startup_screen(void);
static void card_event_cb(lv_event_t *event);
static void fullscreen_event_cb(lv_event_t *event);
static void show_fullscreen(display_mode_t mode);
static void show_grid(void);
static void turn_on_backlight(void);
static void refresh_ui(void);
static void apply_ui_locked(const ui_data_t *data);
static void update_cards_ui(const ui_data_t *data);
static void update_fullscreen_ui(const ui_data_t *data);
static void get_metric_text(display_mode_t mode, const ui_data_t *data, char *valor, size_t valor_len, char *unidade, size_t unidade_len);
static void formatar_distancia(char *buffer, size_t len, float distancia_m);
static void formatar_tempo(uint64_t tempo_ms, char *buffer, size_t len);
static void limpar_status_spans(void);
static void append_plain_text(const char *fmt, ...);
static void append_colored_text(display_mode_t metric, const char *fmt, ...);
static uint32_t calcular_limite_distancia_cm(float distancia_m);
static void update_scope_wave(uint32_t freq_hz);
static void limitar_curso(void);
static void solicitar_salvar_curso(void);
static lv_color_t obter_cor_intervalo_rpm(uint32_t rpm);
static void wifi_settings_btn_event_cb(lv_event_t *event);
static void wifi_panel_btn_event_cb(lv_event_t *event);
static void wifi_scan_result_cb(const wifi_ap_record_t *records, uint16_t count);
static void wifi_network_btn_event_cb(lv_event_t *event);
static void wifi_password_focus_event_cb(lv_event_t *event);
static void wifi_keyboard_event_cb(lv_event_t *event);
static void wifi_quick_disconnect_event_cb(lv_event_t *event);
static void wifi_update_status_labels_locked(void);
static void wifi_set_panel_visible(bool show);
static void wifi_request_scan(void);
static void wifi_build_network_list(void);
static void wifi_refresh_feedback_from_status(const wifi_status_info_t *info);
static const char *wifi_reason_to_text(wifi_err_reason_t reason);
static const char *wifi_authmode_to_text(wifi_auth_mode_t mode);
static void wifi_status_timer_cb(lv_timer_t *timer);

esp_err_t interface_usuario_inicializar(const configuracao_curso_t *config, const ui_callbacks_t *callbacks)
{
    if (!config || !callbacks || !callbacks->ao_solicitar_salvar_curso) {
        return ESP_ERR_INVALID_ARG;
    }
    s_config_curso = *config;
    s_callbacks = *callbacks;

    ESP_RETURN_ON_ERROR(init_rgb_panel(), TAG, "Falha init painel RGB");
    ESP_RETURN_ON_ERROR(init_lvgl_port(), TAG, "Falha init LVGL");
    ESP_RETURN_ON_ERROR(init_touch_panel(), TAG, "Falha init touch");
    build_ui();
    turn_on_backlight();
    show_startup_screen();
    return ESP_OK;
}

void interface_usuario_atualizar(const dados_medidos_t *dados)
{
    if (!dados) {
        return;
    }

    ui_data_t snapshot;
    portENTER_CRITICAL(&s_ui_spinlock);
    s_ui_snapshot = *dados;
    snapshot = s_ui_snapshot;
    portEXIT_CRITICAL(&s_ui_spinlock);

    if (!lvgl_port_lock(portMAX_DELAY)) {
        ESP_LOGW(TAG, "Nao foi possivel travar LVGL para atualizar UI");
        return;
    }
    apply_ui_locked(&snapshot);
    lvgl_port_unlock();
}

void interface_usuario_configurar_curso(float curso_cm)
{
    s_config_curso.curso_cm = curso_cm;
    limitar_curso();
    refresh_ui();
}

static esp_err_t init_rgb_panel(void)
{
    esp_lcd_rgb_panel_config_t panel_config = {
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
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
        .bounce_buffer_size_px = LCD_H_RES * LCD_BOUNCE_BUFFER_LINES,
#endif
    };

    for (size_t i = 0; i < sizeof(s_lcd_data_pins) / sizeof(s_lcd_data_pins[0]); i++) {
        panel_config.data_gpio_nums[i] = s_lcd_data_pins[i];
    }

    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&panel_config, &panel_handle), TAG, "RGB panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel_handle), TAG, "Panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel_handle), TAG, "Panel start failed");

    esp_err_t err = esp_lcd_panel_disp_on_off(panel_handle, true);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "LCD on failed (%s)", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static esp_err_t init_lvgl_port(void)
{
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port init failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = panel_handle,
        .buffer_size = LCD_H_RES * LCD_DRAW_BUFFER_HEIGHT,
        .double_buffer = false,
        .trans_size = 0,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
#if LVGL_VERSION_MAJOR >= 9
        .color_format = LV_COLOR_FORMAT_RGB565,
#endif
        .flags = {
            .buff_dma = true,
            .buff_spiram = true,
#if LVGL_VERSION_MAJOR >= 9
            .swap_bytes = false,
#endif
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

    lvgl_disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    ESP_RETURN_ON_FALSE(lvgl_disp, ESP_FAIL, TAG, "Failed to register LVGL display");
    return ESP_OK;
}

static esp_err_t init_touch_panel(void)
{
    i2c_master_bus_handle_t i2c_handle = NULL;
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = TOUCH_I2C_SDA,
        .scl_io_num = TOUCH_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &i2c_handle), TAG, "I2C init failed");

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_cfg.scl_speed_hz = TOUCH_I2C_CLK_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_cfg, &tp_io_handle), TAG, "Touch IO init failed");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = TOUCH_RST_GPIO,
        .int_gpio_num = TOUCH_INT_GPIO,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &touch_handle), TAG, "GT911 init failed");

#ifdef ESP_LVGL_PORT_TOUCH_COMPONENT
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = lvgl_disp,
        .handle = touch_handle,
        .scale = {
            .x = 1.0f,
            .y = 1.0f,
        },
    };
    lvgl_touch_indev = lvgl_port_add_touch(&touch_cfg);
    ESP_RETURN_ON_FALSE(lvgl_touch_indev, ESP_FAIL, TAG, "Failed to register LVGL touch input");
#endif

    return ESP_OK;
}

static void build_ui(void)
{
    show_startup_screen();

    if (!lvgl_port_lock(portMAX_DELAY)) {
        ESP_LOGE(TAG, "Failed to lock LVGL");
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(THEME_BG_DARK), 0);
    s_layout_mode = UI_LAYOUT_GRID;

    s_grid_container = lv_obj_create(screen);
    lv_obj_remove_style_all(s_grid_container);
    lv_obj_set_size(s_grid_container, LV_PCT(100), LV_PCT(79));
    lv_obj_align(s_grid_container, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(s_grid_container, 16, 0);
    lv_obj_set_style_pad_row(s_grid_container, 12, 0);
    lv_obj_set_style_pad_column(s_grid_container, 16, 0);
    lv_obj_set_style_bg_opa(s_grid_container, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(s_grid_container, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_grid_container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_SPACE_EVENLY);

    for (int i = 0; i < DISPLAY_MODE_COUNT; i++) {
        lv_obj_t *card = lv_obj_create(s_grid_container);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, LV_PCT(48), 120);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_set_style_pad_all(card, 16, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(s_metric_colors[i]), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_60, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_BETWEEN);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, card_event_cb, LV_EVENT_ALL, (void *)(intptr_t)i);

        lv_obj_t *title = lv_label_create(card);
        lv_obj_set_style_text_color(title, lv_color_hex(0xF5F5F5), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_label_set_text(title, s_metric_titles[i]);

        lv_obj_t *value = lv_label_create(card);
        lv_obj_set_style_text_color(value, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(value, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(value, "--");

        lv_obj_t *unit = lv_label_create(card);
        lv_obj_set_style_text_color(unit, lv_color_hex(0xFFECB3), 0);
        lv_obj_set_style_text_font(unit, &lv_font_montserrat_20, 0);
        lv_label_set_text(unit, "");

        s_cards[i] = (card_ui_t){
            .card = card,
            .label_titulo = title,
            .label_valor = value,
            .label_unidade = unit,
        };
    }

    s_bottom_bar = lv_obj_create(screen);
    lv_obj_remove_style_all(s_bottom_bar);
    lv_obj_set_size(s_bottom_bar, LV_PCT(100), 78);
    lv_obj_align(s_bottom_bar, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(s_bottom_bar, lv_color_hex(0x0A1018), 0);
    lv_obj_set_style_bg_opa(s_bottom_bar, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(s_bottom_bar, 18, 0);
    lv_obj_set_style_pad_column(s_bottom_bar, 16, 0);
    lv_obj_set_flex_flow(s_bottom_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_bottom_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_wifi_status_label = lv_label_create(s_bottom_bar);
    lv_obj_set_style_text_color(s_wifi_status_label, lv_color_hex(0xECEFF1), 0);
    lv_obj_set_style_text_font(s_wifi_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_wifi_status_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_flex_grow(s_wifi_status_label, 1);
    lv_label_set_text(s_wifi_status_label, "");

    lv_obj_t *bottom_controls = lv_obj_create(s_bottom_bar);
    lv_obj_remove_style_all(bottom_controls);
    lv_obj_set_style_pad_column(bottom_controls, 12, 0);
    lv_obj_set_flex_flow(bottom_controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(bottom_controls, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    s_wifi_quick_disconnect_btn = lv_btn_create(bottom_controls);
    lv_obj_set_size(s_wifi_quick_disconnect_btn, 50, 50);
    lv_obj_set_style_radius(s_wifi_quick_disconnect_btn, 25, 0);
    lv_obj_set_style_bg_color(s_wifi_quick_disconnect_btn, lv_color_hex(0x8E3535), 0);
    lv_obj_set_style_bg_opa(s_wifi_quick_disconnect_btn, LV_OPA_80, 0);
    lv_obj_add_flag(s_wifi_quick_disconnect_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_wifi_quick_disconnect_btn, wifi_quick_disconnect_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *quick_disc_label = lv_label_create(s_wifi_quick_disconnect_btn);
    lv_label_set_text(quick_disc_label, LV_SYMBOL_POWER);
    lv_obj_center(quick_disc_label);

    s_wifi_button = lv_btn_create(bottom_controls);
    lv_obj_set_size(s_wifi_button, 50, 50);
    lv_obj_set_style_radius(s_wifi_button, 25, 0);
    lv_obj_set_style_bg_color(s_wifi_button, lv_color_hex(0x263238), 0);
    lv_obj_set_style_bg_opa(s_wifi_button, LV_OPA_80, 0);
    lv_obj_add_event_cb(s_wifi_button, wifi_settings_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gear_label = lv_label_create(s_wifi_button);
    lv_label_set_text(gear_label, LV_SYMBOL_SETTINGS);
    lv_obj_center(gear_label);

    s_wifi_panel = lv_obj_create(screen);
    lv_obj_remove_style_all(s_wifi_panel);
    lv_obj_set_size(s_wifi_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_wifi_panel, lv_color_hex(0x02060A), 0);
    lv_obj_set_style_bg_opa(s_wifi_panel, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_wifi_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *wifi_dialog = lv_obj_create(s_wifi_panel);
    lv_obj_remove_style_all(wifi_dialog);
    lv_obj_set_size(wifi_dialog, LV_PCT(96), LV_PCT(90));
    lv_obj_align(wifi_dialog, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_set_style_pad_all(wifi_dialog, 16, 0);
    lv_obj_set_style_bg_color(wifi_dialog, lv_color_hex(0x111822), 0);
    lv_obj_set_style_bg_opa(wifi_dialog, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(wifi_dialog, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wifi_dialog, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *left_col = lv_obj_create(wifi_dialog);
    lv_obj_remove_style_all(left_col);
    lv_obj_set_style_bg_opa(left_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(left_col, 0, 0);
    lv_obj_set_size(left_col, LV_PCT(40), LV_PCT(100));
    lv_obj_set_flex_flow(left_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *left_title = lv_label_create(left_col);
    lv_label_set_text(left_title, "Redes disponíveis");
    lv_obj_set_style_text_font(left_title, &lv_font_montserrat_20, 0);

    s_wifi_list = lv_list_create(left_col);
    lv_obj_set_size(s_wifi_list, LV_PCT(100), LV_PCT(80));
    lv_obj_set_style_bg_color(s_wifi_list, lv_color_hex(0x1C2532), 0);
    lv_obj_set_style_bg_opa(s_wifi_list, LV_OPA_COVER, 0);

    lv_obj_t *btn_refresh = lv_btn_create(left_col);
    lv_obj_set_width(btn_refresh, LV_PCT(100));
    lv_obj_add_event_cb(btn_refresh, wifi_panel_btn_event_cb, LV_EVENT_CLICKED, (void *)3);
    lv_obj_t *btn_refresh_label = lv_label_create(btn_refresh);
    lv_label_set_text(btn_refresh_label, LV_SYMBOL_REFRESH " Buscar redes");
    lv_obj_center(btn_refresh_label);

    lv_obj_t *right_col = lv_obj_create(wifi_dialog);
    lv_obj_remove_style_all(right_col);
    lv_obj_set_style_bg_opa(right_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(right_col, 0, 0);
    lv_obj_set_size(right_col, LV_PCT(58), LV_PCT(100));
    lv_obj_set_flex_flow(right_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(right_col, 8, 0);

    lv_obj_t *wifi_title = lv_label_create(right_col);
    lv_label_set_text(wifi_title, "Configurar conexao Wi-Fi");
    lv_obj_set_style_text_font(wifi_title, &lv_font_montserrat_20, 0);

    s_wifi_ssid_ta = lv_textarea_create(right_col);
    lv_textarea_set_placeholder_text(s_wifi_ssid_ta, "SSID");
    lv_textarea_set_one_line(s_wifi_ssid_ta, true);
    lv_obj_set_width(s_wifi_ssid_ta, LV_PCT(100));
    lv_obj_add_event_cb(s_wifi_ssid_ta, wifi_password_focus_event_cb, LV_EVENT_ALL, NULL);

    s_wifi_pass_ta = lv_textarea_create(right_col);
    lv_textarea_set_placeholder_text(s_wifi_pass_ta, "Senha");
    lv_textarea_set_one_line(s_wifi_pass_ta, true);
    lv_textarea_set_password_mode(s_wifi_pass_ta, true);
    lv_obj_set_width(s_wifi_pass_ta, LV_PCT(100));
    lv_obj_add_event_cb(s_wifi_pass_ta, wifi_password_focus_event_cb, LV_EVENT_ALL, NULL);

    s_wifi_feedback_label = lv_label_create(right_col);
    lv_label_set_text(s_wifi_feedback_label, "");
    lv_obj_set_width(s_wifi_feedback_label, LV_PCT(100));
    lv_obj_set_style_text_color(s_wifi_feedback_label, lv_color_hex(0xFFECB3), 0);

    lv_obj_t *btn_row = lv_obj_create(right_col);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_width(btn_row, LV_PCT(100));
    lv_obj_set_height(btn_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *btn_connect = lv_btn_create(btn_row);
    lv_obj_set_width(btn_connect, LV_PCT(30));
    lv_obj_add_event_cb(btn_connect, wifi_panel_btn_event_cb, LV_EVENT_CLICKED, (void *)1);
    lv_obj_t *btn_connect_label = lv_label_create(btn_connect);
    lv_label_set_text(btn_connect_label, "Conectar");
    lv_obj_center(btn_connect_label);

    lv_obj_t *btn_disconnect = lv_btn_create(btn_row);
    lv_obj_set_width(btn_disconnect, LV_PCT(30));
    lv_obj_add_event_cb(btn_disconnect, wifi_panel_btn_event_cb, LV_EVENT_CLICKED, (void *)2);
    lv_obj_t *btn_disconnect_label = lv_label_create(btn_disconnect);
    lv_label_set_text(btn_disconnect_label, "Desconectar");
    lv_obj_center(btn_disconnect_label);

    lv_obj_t *btn_cancel = lv_btn_create(btn_row);
    lv_obj_set_width(btn_cancel, LV_PCT(30));
    lv_obj_add_event_cb(btn_cancel, wifi_panel_btn_event_cb, LV_EVENT_CLICKED, (void *)0);
    lv_obj_t *btn_cancel_label = lv_label_create(btn_cancel);
    lv_label_set_text(btn_cancel_label, "Voltar");
    lv_obj_center(btn_cancel_label);

    s_wifi_keyboard = lv_keyboard_create(s_wifi_panel);
    lv_obj_set_size(s_wifi_keyboard, LV_PCT(100), 160);
    lv_obj_align(s_wifi_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_wifi_keyboard, wifi_keyboard_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_wifi_keyboard, wifi_keyboard_event_cb, LV_EVENT_CANCEL, NULL);
    if (!s_wifi_status_timer) {
        s_wifi_status_timer = lv_timer_create(wifi_status_timer_cb, 750, NULL);
    }


    s_fullscreen_container = lv_obj_create(screen);
    lv_obj_remove_style_all(s_fullscreen_container);
    lv_obj_set_size(s_fullscreen_container, LV_PCT(100), LV_PCT(100));
    lv_obj_align(s_fullscreen_container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_fullscreen_container, lv_color_hex(THEME_BG_DARK_PANEL), 0);
    lv_obj_set_style_bg_opa(s_fullscreen_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_fullscreen_container, 4, 0);
    lv_obj_set_style_border_color(s_fullscreen_container, lv_color_hex(s_metric_colors[DISPLAY_FREQUENCIA]), 0);
    lv_obj_set_style_outline_width(s_fullscreen_container, 0, 0);
    lv_obj_set_style_pad_all(s_fullscreen_container, 32, 0);
    lv_obj_add_flag(s_fullscreen_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_fullscreen_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_fullscreen_container, fullscreen_event_cb, LV_EVENT_ALL, NULL);

    s_full_title = lv_label_create(s_fullscreen_container);
    lv_obj_set_style_text_color(s_full_title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_full_title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(s_full_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_full_title, LV_ALIGN_TOP_MID, 0, 8);

    s_full_value = lv_label_create(s_fullscreen_container);
    lv_obj_set_style_text_color(s_full_value, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_full_value, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(s_full_value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_full_value, LV_ALIGN_CENTER, 0, -80);

    s_full_unit = lv_label_create(s_fullscreen_container);
    lv_obj_set_style_text_color(s_full_unit, lv_color_hex(0xF5F5F5), 0);
    lv_obj_set_style_text_font(s_full_unit, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(s_full_unit, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(s_full_unit, s_full_value, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);

    s_full_arc = lv_arc_create(s_fullscreen_container);
    lv_obj_set_size(s_full_arc, 288, 288);
    lv_arc_set_rotation(s_full_arc, 135);
    lv_arc_set_bg_angles(s_full_arc, 0, 270);
    lv_arc_set_value(s_full_arc, 0);
    lv_obj_remove_style(s_full_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_full_arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_full_arc, 14, LV_PART_INDICATOR);
    lv_obj_add_flag(s_full_arc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_full_arc, LV_ALIGN_CENTER, 0, 70);

    s_full_arc_label = lv_label_create(s_fullscreen_container);
    lv_obj_set_style_text_color(s_full_arc_label, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(s_full_arc_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_full_arc_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_full_arc_label, LV_ALIGN_CENTER, 0, 160);
    lv_obj_add_flag(s_full_arc_label, LV_OBJ_FLAG_HIDDEN);

    s_full_bar = lv_bar_create(s_fullscreen_container);
    lv_bar_set_range(s_full_bar, 0, 100000);
    lv_obj_set_size(s_full_bar, LV_PCT(80), 16);
    lv_obj_align(s_full_bar, LV_ALIGN_CENTER, 0, 60);
    lv_obj_add_flag(s_full_bar, LV_OBJ_FLAG_HIDDEN);

    s_full_bar_label = lv_label_create(s_fullscreen_container);
    lv_obj_set_style_text_color(s_full_bar_label, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(s_full_bar_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_full_bar_label, LV_ALIGN_CENTER, 0, 90);
    lv_obj_add_flag(s_full_bar_label, LV_OBJ_FLAG_HIDDEN);

    s_full_timer_label = lv_label_create(s_fullscreen_container);
    lv_obj_set_style_text_color(s_full_timer_label, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(s_full_timer_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_full_timer_label, LV_ALIGN_CENTER, 0, 120);
    lv_obj_add_flag(s_full_timer_label, LV_OBJ_FLAG_HIDDEN);

    s_full_scope_chart = lv_chart_create(s_fullscreen_container);
    lv_obj_set_size(s_full_scope_chart, LV_PCT(82), 162);
    lv_obj_align(s_full_scope_chart, LV_ALIGN_CENTER, 0, -5);
    lv_chart_set_point_count(s_full_scope_chart, SCOPE_POINT_COUNT);
    lv_chart_set_range(s_full_scope_chart, LV_CHART_AXIS_PRIMARY_Y, -100, 100);
    lv_chart_set_type(s_full_scope_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(s_full_scope_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_add_flag(s_full_scope_chart, LV_OBJ_FLAG_HIDDEN);
    s_full_scope_series = lv_chart_add_series(s_full_scope_chart, lv_color_hex(0x00FFC0), LV_CHART_AXIS_PRIMARY_Y);

    s_full_scope_axis_label = lv_label_create(s_fullscreen_container);
    lv_obj_set_style_text_color(s_full_scope_axis_label, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(s_full_scope_axis_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_full_scope_axis_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(s_full_scope_axis_label, s_full_scope_chart, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
    lv_obj_add_flag(s_full_scope_axis_label, LV_OBJ_FLAG_HIDDEN);

    s_full_course_arc = lv_arc_create(s_fullscreen_container);
    lv_arc_set_rotation(s_full_course_arc, 135);
    lv_arc_set_bg_angles(s_full_course_arc, 0, 270);
    lv_arc_set_range(s_full_course_arc, 100, 500);
    lv_arc_set_value(s_full_course_arc, 100);
    lv_obj_remove_style(s_full_course_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_full_course_arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_full_course_arc, 12, LV_PART_INDICATOR);
    lv_obj_add_flag(s_full_course_arc, LV_OBJ_FLAG_HIDDEN);

    s_speed_bar = lv_bar_create(s_fullscreen_container);
    lv_bar_set_range(s_speed_bar, 0, 100);
    lv_obj_set_size(s_speed_bar, LV_PCT(80), 18);
    lv_obj_align(s_speed_bar, LV_ALIGN_CENTER, 0, 60);
    lv_obj_set_style_bg_color(s_speed_bar, lv_color_hex(0x263238), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_speed_bar, LV_OPA_50, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_speed_bar, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_speed_bar, lv_color_hex(0xAA00FF), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_color(s_speed_bar, lv_color_hex(0x00E5FF), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_grad_dir(s_speed_bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_speed_bar, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_speed_bar, 12, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_add_flag(s_speed_bar, LV_OBJ_FLAG_HIDDEN);

    s_speed_bar_label = lv_label_create(s_fullscreen_container);
    lv_obj_set_style_text_color(s_speed_bar_label, lv_color_hex(0xCFD8DC), 0);
    lv_obj_set_style_text_font(s_speed_bar_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_speed_bar_label, LV_ALIGN_CENTER, 0, 110);
    lv_obj_add_flag(s_speed_bar_label, LV_OBJ_FLAG_HIDDEN);

    s_furos_circle_left = lv_arc_create(s_fullscreen_container);
    lv_obj_set_size(s_furos_circle_left, 72, 72);
    lv_arc_set_rotation(s_furos_circle_left, 270);
    lv_arc_set_bg_angles(s_furos_circle_left, 0, 360);
    lv_arc_set_range(s_furos_circle_left, 0, 500);
    lv_arc_set_value(s_furos_circle_left, 0);
    lv_obj_remove_style(s_furos_circle_left, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_furos_circle_left, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_furos_circle_left, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_furos_circle_left, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_furos_circle_left, lv_color_hex(0xFFE0B2), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_furos_circle_left, lv_color_hex(0xFF7043), LV_PART_INDICATOR);
    lv_obj_add_flag(s_furos_circle_left, LV_OBJ_FLAG_HIDDEN);

    s_furos_circle_right = lv_arc_create(s_fullscreen_container);
    lv_obj_set_size(s_furos_circle_right, 72, 72);
    lv_arc_set_rotation(s_furos_circle_right, 270);
    lv_arc_set_bg_angles(s_furos_circle_right, 0, 360);
    lv_arc_set_range(s_furos_circle_right, 0, 500);
    lv_arc_set_value(s_furos_circle_right, 0);
    lv_obj_remove_style(s_furos_circle_right, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_furos_circle_right, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_furos_circle_right, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_furos_circle_right, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_furos_circle_right, lv_color_hex(0xFFE0B2), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_furos_circle_right, lv_color_hex(0xFF7043), LV_PART_INDICATOR);
    lv_obj_add_flag(s_furos_circle_right, LV_OBJ_FLAG_HIDDEN);

    s_full_status = lv_spangroup_create(s_fullscreen_container);
    lv_obj_set_size(s_full_status, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(s_full_status, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_spangroup_set_mode(s_full_status, LV_SPAN_MODE_BREAK);
    lv_spangroup_set_align(s_full_status, LV_TEXT_ALIGN_CENTER);

    ui_data_t snapshot;
    portENTER_CRITICAL(&s_ui_spinlock);
    snapshot = s_ui_snapshot;
    portEXIT_CRITICAL(&s_ui_spinlock);
    apply_ui_locked(&snapshot);

    lvgl_port_unlock();
}

static void show_startup_screen(void)
{
    if (!lvgl_port_lock(portMAX_DELAY)) {
        ESP_LOGE(TAG, "Failed to lock LVGL for splash");
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);

    lv_obj_t *logo_img = lv_image_create(overlay);
    lv_image_set_src(logo_img, &liga_d_logo);
    lv_obj_align(logo_img, LV_ALIGN_CENTER, 0, -12);
    lv_obj_set_style_bg_opa(logo_img, LV_OPA_TRANSP, 0);

    lv_obj_update_layout(logo_img);
    int32_t logo_w = lv_obj_get_width(logo_img);
    int32_t logo_h = lv_obj_get_height(logo_img);
    if (logo_h <= 0) {
        logo_h = 1;
    }
    lv_obj_set_style_transform_pivot_x(logo_img, logo_w / 2, 0);
    lv_obj_set_style_transform_pivot_y(logo_img, logo_h / 2, 0);
    int32_t target_scale = (LCD_V_RES * 256) / logo_h;
    if (target_scale > 256) {
        target_scale = 256;
    }
    lv_obj_set_style_transform_scale(logo_img, target_scale, 0);

    lv_obj_t *status = lv_label_create(overlay);
    lv_obj_set_style_text_color(status, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(status, &lv_font_montserrat_20, 0);
    lv_label_set_text(status, "Inicializando sistema...");
    lv_obj_align(status, LV_ALIGN_BOTTOM_MID, 0, -40);

    lvgl_port_unlock();
    vTaskDelay(pdMS_TO_TICKS(3000));

    if (lvgl_port_lock(portMAX_DELAY)) {
        lv_label_set_text(status, "Sistema inicializado");
        lvgl_port_unlock();
    }

    vTaskDelay(pdMS_TO_TICKS(3000));

    if (lvgl_port_lock(portMAX_DELAY)) {
        lv_obj_del(overlay);
        lvgl_port_unlock();
    }
}

static void card_event_cb(lv_event_t *event)
{
    display_mode_t mode = (display_mode_t)(intptr_t)lv_event_get_user_data(event);
    lv_event_code_t code = lv_event_get_code(event);

    if (s_layout_mode == UI_LAYOUT_GRID && code == LV_EVENT_SHORT_CLICKED) {
        show_fullscreen(mode);
    }
}

static void fullscreen_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_DOUBLE_CLICKED) {
        show_grid();
        return;
    }

    if (s_display_mode != DISPLAY_CURSO) {
        return;
    }

    bool ui_changed = false;
    if (code == LV_EVENT_SHORT_CLICKED && s_modo_edicao) {
        s_config_curso.curso_cm += 0.01f;
        limitar_curso();
        solicitar_salvar_curso();
        ui_changed = true;
    } else if (code == LV_EVENT_LONG_PRESSED) {
        s_modo_edicao = !s_modo_edicao;
        if (!s_modo_edicao) {
            solicitar_salvar_curso();
        }
        ui_changed = true;
    } else if (code == LV_EVENT_LONG_PRESSED_REPEAT && s_modo_edicao) {
        s_config_curso.curso_cm -= 0.01f;
        limitar_curso();
        solicitar_salvar_curso();
        ui_changed = true;
    }

    if (ui_changed) {
        ui_data_t snapshot;
        portENTER_CRITICAL(&s_ui_spinlock);
        snapshot = s_ui_snapshot;
        portEXIT_CRITICAL(&s_ui_spinlock);
        apply_ui_locked(&snapshot);
    }
}

static void show_fullscreen(display_mode_t mode)
{
    s_layout_mode = UI_LAYOUT_FULLSCREEN;
    s_display_mode = mode;
    lv_obj_add_flag(s_grid_container, LV_OBJ_FLAG_HIDDEN);
    if (s_bottom_bar) {
        lv_obj_add_flag(s_bottom_bar, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_wifi_button) {
        lv_obj_add_flag(s_wifi_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_wifi_panel) {
        lv_obj_add_flag(s_wifi_panel, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_fullscreen_container, LV_OBJ_FLAG_HIDDEN);

    ui_data_t snapshot;
    portENTER_CRITICAL(&s_ui_spinlock);
    snapshot = s_ui_snapshot;
    portEXIT_CRITICAL(&s_ui_spinlock);
    apply_ui_locked(&snapshot);
}

static void show_grid(void)
{
    if (s_layout_mode == UI_LAYOUT_GRID) {
        return;
    }
    s_layout_mode = UI_LAYOUT_GRID;
    lv_obj_clear_flag(s_grid_container, LV_OBJ_FLAG_HIDDEN);
    if (s_bottom_bar) {
        lv_obj_clear_flag(s_bottom_bar, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_wifi_button) {
        lv_obj_clear_flag(s_wifi_button, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(s_fullscreen_container, LV_OBJ_FLAG_HIDDEN);

    ui_data_t snapshot;
    portENTER_CRITICAL(&s_ui_spinlock);
    snapshot = s_ui_snapshot;
    portEXIT_CRITICAL(&s_ui_spinlock);
    apply_ui_locked(&snapshot);
}

static void turn_on_backlight(void)
{
#if LCD_BACKLIGHT_GPIO_NUM >= 0
    const int bl_pin = LCD_BACKLIGHT_GPIO_NUM;
    gpio_config_t io_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << bl_pin,
    };
    gpio_config(&io_conf);
    gpio_set_level(bl_pin, 1);
#else
    // Backlight pin nao conectado
#endif
}

static void refresh_ui(void)
{
    ui_data_t snapshot;
    portENTER_CRITICAL(&s_ui_spinlock);
    snapshot = s_ui_snapshot;
    portEXIT_CRITICAL(&s_ui_spinlock);

    if (!lvgl_port_lock(portMAX_DELAY)) {
        ESP_LOGW(TAG, "Nao foi possivel travar LVGL para atualizar UI");
        return;
    }
    apply_ui_locked(&snapshot);
    lvgl_port_unlock();
}

static void apply_ui_locked(const ui_data_t *data)
{
    update_cards_ui(data);

    if (s_layout_mode == UI_LAYOUT_FULLSCREEN) {
        update_fullscreen_ui(data);
    }

    wifi_update_status_labels_locked();
}

static void update_cards_ui(const ui_data_t *data)
{
    for (int i = 0; i < DISPLAY_MODE_COUNT; i++) {
        card_ui_t *card = &s_cards[i];
        if (!card->label_valor) {
            continue;
        }
        char valor[48];
        char unidade[16];
        get_metric_text((display_mode_t)i, data, valor, sizeof(valor), unidade, sizeof(unidade));
        lv_label_set_text(card->label_titulo, s_metric_titles[i]);
        if (i == DISPLAY_FUROS) {
            lv_obj_set_style_text_align(card->label_valor, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_align(card->label_valor, LV_ALIGN_CENTER, 0, -10);
            lv_label_set_text(card->label_valor, valor);
            lv_obj_add_flag(card->label_unidade, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_set_style_text_align(card->label_valor, LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_align(card->label_valor, LV_ALIGN_CENTER, 0, -10);
            lv_label_set_text(card->label_valor, valor);
            lv_label_set_text(card->label_unidade, unidade);
            lv_obj_clear_flag(card->label_unidade, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void update_fullscreen_ui(const ui_data_t *data)
{
    if (!s_fullscreen_container) {
        return;
    }

    char valor[48];
    char unidade[16];
    get_metric_text(s_display_mode, data, valor, sizeof(valor), unidade, sizeof(unidade));
    lv_obj_set_style_bg_color(s_fullscreen_container, lv_color_hex(THEME_BG_DARK_PANEL), 0);
    lv_obj_set_style_border_color(s_fullscreen_container, lv_color_hex(s_metric_colors[s_display_mode]), 0);
    lv_label_set_text(s_full_title, s_metric_titles[s_display_mode]);
    lv_label_set_text(s_full_value, valor);
    lv_label_set_text(s_full_unit, unidade);
    if (s_full_arc) {
        lv_obj_add_flag(s_full_arc, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_full_arc_label) {
        lv_obj_add_flag(s_full_arc_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_full_bar) {
        lv_obj_add_flag(s_full_bar, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_full_bar_label) {
        lv_obj_add_flag(s_full_bar_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_full_timer_label) {
        lv_obj_add_flag(s_full_timer_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_full_scope_chart) {
        lv_obj_add_flag(s_full_scope_chart, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_full_scope_axis_label) {
        lv_obj_add_flag(s_full_scope_axis_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_full_course_arc) {
        lv_obj_add_flag(s_full_course_arc, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_speed_bar) {
        lv_obj_add_flag(s_speed_bar, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_speed_bar_label) {
        lv_obj_add_flag(s_speed_bar_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_furos_circle_left) {
        lv_obj_add_flag(s_furos_circle_left, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_furos_circle_right) {
        lv_obj_add_flag(s_furos_circle_right, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_display_mode == DISPLAY_FREQUENCIA && s_full_scope_chart && s_full_scope_series) {
        lv_obj_clear_flag(s_full_scope_chart, LV_OBJ_FLAG_HIDDEN);
        update_scope_wave(data->frequencia_hz);
        lv_obj_align(s_full_scope_chart, LV_ALIGN_CENTER, 0, -20);
        if (s_full_scope_axis_label) {
            lv_obj_add_flag(s_full_scope_axis_label, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_align_to(s_full_value, s_full_scope_chart, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
        lv_obj_align_to(s_full_unit, s_full_value, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    } else if (s_full_arc && s_display_mode == DISPLAY_RPM) {
        const uint32_t max_value = 13500;
        uint32_t current = data->rpm;
        if (current > max_value) {
            current = max_value;
        }
        lv_arc_set_range(s_full_arc, 0, max_value);
        lv_arc_set_value(s_full_arc, current);
        lv_obj_set_style_arc_color(s_full_arc, obter_cor_intervalo_rpm(current), LV_PART_INDICATOR | LV_STATE_DEFAULT);
        lv_obj_clear_flag(s_full_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_full_arc, LV_ALIGN_CENTER, 0, 10);
        lv_obj_align(s_full_value, LV_ALIGN_CENTER, 0, 10);
        lv_obj_align_to(s_full_unit, s_full_value, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
        if (s_full_arc_label) {
            lv_label_set_text(s_full_arc_label, "0 - 13.5k rpm");
            lv_obj_clear_flag(s_full_arc_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(s_full_arc_label, LV_ALIGN_CENTER, 0, 160);
        }
    } else if (s_display_mode == DISPLAY_VELOCIDADE && s_speed_bar && s_speed_bar_label) {
        float limite_speed_f = s_config_curso.curso_cm * FREQUENCIA_MAXIMA_HZ;
        if (limite_speed_f < 1.0f) {
            limite_speed_f = 1.0f;
        }
        uint32_t max_speed = (uint32_t)lroundf(limite_speed_f);
        if (max_speed == 0) {
            max_speed = 1;
        }

        uint32_t current = (uint32_t)lroundf(s_config_curso.curso_cm * (float)data->frequencia_hz);
        if (current > max_speed) {
            current = max_speed;
        }
        lv_bar_set_range(s_speed_bar, 0, (int32_t)max_speed);
        lv_bar_set_value(s_speed_bar, (int32_t)current, LV_ANIM_OFF);
        lv_obj_clear_flag(s_speed_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_speed_bar, LV_ALIGN_CENTER, 0, 60);

        uint32_t percentual = max_speed ? (current * 100U) / max_speed : 0;
        lv_label_set_text_fmt(s_speed_bar_label,
                              "Boost %" PRIu32"%%   |   Limite: %" PRIu32" cm/s",
                              percentual,
                              max_speed);
        lv_obj_clear_flag(s_speed_bar_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_speed_bar_label, LV_ALIGN_CENTER, 0, 100);

        lv_obj_align(s_full_value, LV_ALIGN_CENTER, 0, -90);
        lv_obj_align_to(s_full_unit, s_full_value, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    } else if (s_display_mode == DISPLAY_CURSO && s_full_course_arc) {
        float curso_mm = s_config_curso.curso_cm * 10.0f;
        if (curso_mm < 1.0f) curso_mm = 1.0f;
        if (curso_mm > 5.0f) curso_mm = 5.0f;
        uint32_t scaled = (uint32_t)(curso_mm * 100.0f);
        lv_arc_set_range(s_full_course_arc, 100, 500);
        lv_arc_set_value(s_full_course_arc, scaled);
        float ratio = (curso_mm - 1.0f) / 4.0f;
        uint32_t size_px = (uint32_t)(200.0f + ratio * 80.0f);
        lv_obj_set_size(s_full_course_arc, size_px, size_px);
        lv_obj_clear_flag(s_full_course_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_full_course_arc, LV_ALIGN_CENTER, 0, -5);
        lv_obj_align(s_full_value, LV_ALIGN_CENTER, 0, -5);
        lv_obj_align_to(s_full_unit, s_full_value, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    } else {
        lv_obj_align(s_full_value, LV_ALIGN_CENTER, 0, -80);
        lv_obj_align_to(s_full_unit, s_full_value, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
    }

    if (s_display_mode == DISPLAY_DISTANCIA && s_full_bar && s_full_bar_label) {
        float distancia_m = data->distancia_m;
        if (distancia_m < 0.0f) {
            distancia_m = 0.0f;
        }
        if (distancia_m > 100000.0f) {
            distancia_m = 100000.0f;
        }
        uint32_t distancia_cm = (uint32_t)lroundf(distancia_m * 100.0f);
        uint32_t limite_cm = calcular_limite_distancia_cm(distancia_m);
        if (limite_cm == 0U) {
            limite_cm = 1U;
        }
        if (distancia_cm > limite_cm) {
            distancia_cm = limite_cm;
        }

        lv_bar_set_range(s_full_bar, 0, (int32_t)limite_cm);
        lv_bar_set_value(s_full_bar, (int32_t)distancia_cm, LV_ANIM_OFF);
        lv_obj_clear_flag(s_full_bar, LV_OBJ_FLAG_HIDDEN);

        char distancia_txt[32];
        char limite_txt[32];
        formatar_distancia(distancia_txt, sizeof(distancia_txt), distancia_m);
        formatar_distancia(limite_txt, sizeof(limite_txt), limite_cm / 100.0f);
        uint32_t percentual = limite_cm ? (distancia_cm * 100U) / limite_cm : 0U;
        lv_label_set_text_fmt(s_full_bar_label,
                              "%s de %s (%" PRIu32"%%)",
                              distancia_txt,
                              limite_txt,
                              percentual);
        lv_obj_clear_flag(s_full_bar_label, LV_OBJ_FLAG_HIDDEN);
    } else if (s_display_mode == DISPLAY_FUROS) {
        lv_obj_align(s_full_value, LV_ALIGN_CENTER, 0, -10);
        lv_obj_align_to(s_full_unit, s_full_value, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);

        uint32_t furos = data->furos;
        uint32_t progress = furos % 500U;
        uint32_t ciclo = furos / 500U;
        bool direita_ativa = (ciclo % 2U) == 0U;

        lv_obj_t *ativo = direita_ativa ? s_furos_circle_right : s_furos_circle_left;
        lv_obj_t *inativo = direita_ativa ? s_furos_circle_left : s_furos_circle_right;

        if (inativo) {
            lv_arc_set_value(inativo, 0);
            lv_obj_add_flag(inativo, LV_OBJ_FLAG_HIDDEN);
        }
        if (ativo) {
            lv_arc_set_value(ativo, (int32_t)progress);
            lv_obj_clear_flag(ativo, LV_OBJ_FLAG_HIDDEN);
            if (ativo == s_furos_circle_right) {
                lv_obj_align_to(ativo, s_full_value, LV_ALIGN_OUT_RIGHT_MID, 80, 0);
            } else {
                lv_obj_align_to(ativo, s_full_value, LV_ALIGN_OUT_LEFT_MID, -80, 0);
            }
        }
    }

    bool tela_temporizada = (s_display_mode == DISPLAY_DISTANCIA || s_display_mode == DISPLAY_FUROS);
    if (s_full_timer_label) {
        if (tela_temporizada) {
            char tempo_txt[32];
            formatar_tempo(data->tempo_sinal_ms, tempo_txt, sizeof(tempo_txt));
            lv_label_set_text_fmt(s_full_timer_label, "Tempo: %s", tempo_txt);
            lv_obj_clear_flag(s_full_timer_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_full_timer_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    const char *hint = "Toque duplo para voltar";
    if (s_display_mode == DISPLAY_CURSO) {
        hint = s_modo_edicao ? "Modo edicao ativo: toque curto +1 mm, segure para reduzir"
                             : "Toque longo para editar o curso";
    }

    uint32_t freq = data->frequencia_hz;
    uint32_t rpm = data->rpm;
    uint32_t velocidade = data->velocidade_cm_s;
    float curso_mm = s_config_curso.curso_cm * 10.0f;

    limpar_status_spans();

    switch (s_display_mode) {
    case DISPLAY_DISTANCIA:
        append_colored_text(DISPLAY_FREQUENCIA, "Freq: %" PRIu32 " Hz", freq);
        append_plain_text(" | ");
        append_colored_text(DISPLAY_CURSO, "Curso: %.1f mm", curso_mm);
        append_plain_text(" | %s", hint);
        break;
    case DISPLAY_RPM:
        append_colored_text(DISPLAY_FREQUENCIA, "Freq: %" PRIu32 " Hz", freq);
        append_plain_text(" | %s", hint);
        break;
    case DISPLAY_FREQUENCIA:
        append_colored_text(DISPLAY_RPM, "RPM: %" PRIu32, rpm);
        append_plain_text(" | ");
        append_colored_text(DISPLAY_CURSO, "Curso: %.1f mm", curso_mm);
        append_plain_text(" | %s", hint);
        break;
    case DISPLAY_VELOCIDADE:
        append_colored_text(DISPLAY_FREQUENCIA, "Freq: %" PRIu32 " Hz", freq);
        append_plain_text(" | %s", hint);
        break;
    case DISPLAY_CURSO:
        append_colored_text(DISPLAY_FREQUENCIA, "Freq: %" PRIu32 " Hz", freq);
        append_plain_text(" | ");
        append_colored_text(DISPLAY_VELOCIDADE, "Vel: %" PRIu32 " cm/s", velocidade);
        append_plain_text(" | %s", hint);
        break;
    case DISPLAY_FUROS:
        append_colored_text(DISPLAY_FREQUENCIA, "Freq: %" PRIu32 " Hz", freq);
        append_plain_text(" | ");
        append_colored_text(DISPLAY_VELOCIDADE, "Vel: %" PRIu32 " cm/s", velocidade);
        append_plain_text(" | %s", hint);
        break;
    default:
        append_colored_text(DISPLAY_CURSO, "Curso: %.1f mm", curso_mm);
        append_plain_text(" | %s", hint);
        break;
    }

    lv_spangroup_refresh(s_full_status);

}

static void get_metric_text(display_mode_t mode, const ui_data_t *data,
                            char *valor, size_t valor_len,
                            char *unidade, size_t unidade_len)
{
    const char *unit = "";

    switch (mode) {
    case DISPLAY_FREQUENCIA:
        snprintf(valor, valor_len, "%" PRIu32, data->frequencia_hz);
        unit = "Hz";
        break;
    case DISPLAY_RPM:
        snprintf(valor, valor_len, "%" PRIu32, data->rpm);
        unit = "rpm";
        break;
    case DISPLAY_VELOCIDADE:
        snprintf(valor, valor_len, "%" PRIu32, data->velocidade_cm_s);
        unit = "cm/s";
        break;
    case DISPLAY_CURSO:
        snprintf(valor, valor_len, "%.1f", s_config_curso.curso_cm * 10.0f);
        unit = "mm";
        break;
    case DISPLAY_DISTANCIA:
        formatar_distancia(valor, valor_len, data->distancia_m);
        unit = "";
        break;
    case DISPLAY_FUROS:
        snprintf(valor, valor_len, "%" PRIu32, data->furos);
        unit = "";
        break;
    default:
        snprintf(valor, valor_len, "---");
        unit = "";
        break;
    }

    snprintf(unidade, unidade_len, "%s", unit);
}

static void formatar_distancia(char *buffer, size_t len, float distancia_m)
{
    if (distancia_m >= 1000.0f) {
        snprintf(buffer, len, "%.2f km", distancia_m / 1000.0f);
    } else {
        snprintf(buffer, len, "%.1f m", distancia_m);
    }
}

static void formatar_tempo(uint64_t tempo_ms, char *buffer, size_t len)
{
    uint64_t total_seg = tempo_ms / 1000ULL;
    uint64_t horas = total_seg / 3600ULL;
    uint64_t minutos = (total_seg % 3600ULL) / 60ULL;
    uint64_t segundos = total_seg % 60ULL;
    if (horas > 0) {
        snprintf(buffer, len, "%02llu:%02llu:%02llu", (unsigned long long)horas,
                 (unsigned long long)minutos, (unsigned long long)segundos);
    } else {
        snprintf(buffer, len, "%02llu:%02llu", (unsigned long long)minutos, (unsigned long long)segundos);
    }
}

static void wifi_settings_btn_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    wifi_set_panel_visible(true);
}

static void wifi_panel_btn_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    intptr_t action = (intptr_t)lv_event_get_user_data(event);
    switch (action) {
    case 0: // voltar
        wifi_set_panel_visible(false);
        break;
    case 1: { // conectar
        if (!s_wifi_ssid_ta || !s_wifi_pass_ta) {
            return;
        }
        const char *ssid_txt = lv_textarea_get_text(s_wifi_ssid_ta);
        if (!ssid_txt || strlen(ssid_txt) == 0) {
            if (s_wifi_feedback_label) {
                lv_label_set_text(s_wifi_feedback_label, "Informe o SSID da rede.");
            }
            return;
        }
        const char *senha_txt = lv_textarea_get_text(s_wifi_pass_ta);
        char ssid[WIFI_MANAGER_MAX_SSID_LEN + 1] = {0};
        char senha[WIFI_MANAGER_MAX_PASS_LEN + 1] = {0};
        snprintf(ssid, sizeof(ssid), "%.*s", WIFI_MANAGER_MAX_SSID_LEN, ssid_txt);
        if (senha_txt && strlen(senha_txt) > 0) {
            snprintf(senha, sizeof(senha), "%.*s", WIFI_MANAGER_MAX_PASS_LEN, senha_txt);
        }
        strlcpy(s_wifi_selected_ssid, ssid, sizeof(s_wifi_selected_ssid));
        wifi_build_network_list();
        if (s_callbacks.ao_solicitar_wifi_conectar) {
            s_callbacks.ao_solicitar_wifi_conectar(ssid, senha);
            if (s_wifi_feedback_label) {
                lv_label_set_text_fmt(s_wifi_feedback_label, "Conectando a %s...", ssid);
            }
            s_wifi_connect_start_us = esp_timer_get_time();
            s_wifi_timeout_reported = false;
            wifi_update_status_labels_locked();
        }
        break;
    }
    case 2: { // desconectar
        wifi_manager_disconnect();
        if (s_wifi_feedback_label) {
            lv_label_set_text(s_wifi_feedback_label, "Solicitando desconexao...");
        }
        s_wifi_connect_start_us = 0;
        s_wifi_timeout_reported = false;
        wifi_update_status_labels_locked();
        break;
    }
    case 3: // atualizar redes
        wifi_request_scan();
        break;
    default:
        break;
    }
}

static void wifi_request_scan(void)
{
    if (!s_wifi_list) {
        return;
    }
    if (s_wifi_scanning) {
        wifi_build_network_list();
        return;
    }

    s_wifi_scanning = true;
    s_wifi_network_count = 0;
    wifi_build_network_list();
    if (s_wifi_feedback_label) {
        lv_label_set_text(s_wifi_feedback_label, "Buscando redes proximas...");
    }

    esp_err_t err = wifi_manager_start_scan(wifi_scan_result_cb);
    if (err != ESP_OK) {
        s_wifi_scanning = false;
        if (s_wifi_feedback_label) {
            lv_label_set_text_fmt(s_wifi_feedback_label, "Falha ao buscar redes (0x%x)", (unsigned int)err);
        }
        wifi_build_network_list();
    }
}

static void wifi_scan_result_cb(const wifi_ap_record_t *records, uint16_t count)
{
    s_wifi_scanning = false;
    if (!records || count == 0) {
        s_wifi_network_count = 0;
    } else {
        if (count > WIFI_MANAGER_MAX_SCAN_RESULTS) {
            count = WIFI_MANAGER_MAX_SCAN_RESULTS;
        }
        memcpy(s_wifi_network_cache, records, sizeof(wifi_ap_record_t) * count);
        s_wifi_network_count = count;
        for (uint16_t i = 0; i < s_wifi_network_count; i++) {
            for (uint16_t j = i + 1; j < s_wifi_network_count; j++) {
                if (s_wifi_network_cache[j].rssi > s_wifi_network_cache[i].rssi) {
                    wifi_ap_record_t tmp = s_wifi_network_cache[i];
                    s_wifi_network_cache[i] = s_wifi_network_cache[j];
                    s_wifi_network_cache[j] = tmp;
                }
            }
        }
    }

    if (!lvgl_port_lock(portMAX_DELAY)) {
        return;
    }
    if (s_wifi_feedback_label) {
        if (s_wifi_network_count > 0) {
            lv_label_set_text(s_wifi_feedback_label, "Selecione uma rede detectada.");
        } else {
            lv_label_set_text(s_wifi_feedback_label, "Nenhuma rede detectada. Tente novamente.");
        }
    }
    wifi_build_network_list();
    lvgl_port_unlock();
}

static void wifi_build_network_list(void)
{
    if (!s_wifi_list) {
        return;
    }
    lv_obj_clean(s_wifi_list);

    if (s_wifi_scanning) {
        lv_obj_t *label = lv_label_create(s_wifi_list);
        lv_label_set_text(label, "Procurando redes Wi-Fi...");
        lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xECEFF1), 0);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    if (s_wifi_network_count == 0) {
        lv_obj_t *label = lv_label_create(s_wifi_list);
        lv_label_set_text(label, "Nenhuma rede encontrada.");
        lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0xECEFF1), 0);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    for (uint16_t i = 0; i < s_wifi_network_count; i++) {
        const wifi_ap_record_t *ap = &s_wifi_network_cache[i];
        lv_obj_t *btn = lv_btn_create(s_wifi_list);
        lv_obj_set_width(btn, LV_PCT(100));
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_pad_all(btn, 10, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x1F2A39), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x253245), LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2B3A50), LV_STATE_CHECKED);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, wifi_network_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        const char *ssid_ptr = (const char *)ap->ssid;
        char display_ssid[WIFI_MANAGER_MAX_SSID_LEN + 1];
        if (!ssid_ptr || ssid_ptr[0] == '\0') {
            snprintf(display_ssid, sizeof(display_ssid), "Rede oculta");
        } else {
            strlcpy(display_ssid, ssid_ptr, sizeof(display_ssid));
        }
        const char *lock_symbol = (ap->authmode == WIFI_AUTH_OPEN) ? "" : " " LV_SYMBOL_EYE_CLOSE;

        lv_obj_t *ssid_label = lv_label_create(btn);
        lv_obj_set_style_text_font(ssid_label, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(ssid_label, lv_color_hex(0xFFFFFF), 0);
        lv_label_set_text_fmt(ssid_label, "%s %s%s", LV_SYMBOL_WIFI, display_ssid, lock_symbol);

        lv_obj_t *detail_label = lv_label_create(btn);
        lv_obj_set_style_text_font(detail_label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(detail_label, lv_color_hex(0xB0BEC5), 0);
        lv_label_set_text_fmt(detail_label, "%d dBm  ·  %s", ap->rssi, wifi_authmode_to_text(ap->authmode));
        lv_obj_align_to(detail_label, ssid_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 4);

        if (strlen(s_wifi_selected_ssid) > 0 && ssid_ptr && strcmp(ssid_ptr, s_wifi_selected_ssid) == 0) {
            lv_obj_add_state(btn, LV_STATE_CHECKED);
        }
    }
}

static void wifi_network_btn_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    intptr_t idx = (intptr_t)lv_event_get_user_data(event);
    if (idx < 0 || idx >= s_wifi_network_count) {
        return;
    }
    const wifi_ap_record_t *ap = &s_wifi_network_cache[idx];
    if (!ap) {
        return;
    }
    if (!s_wifi_ssid_ta) {
        return;
    }
    if (ap->ssid[0] == '\0') {
        if (s_wifi_feedback_label) {
            lv_label_set_text(s_wifi_feedback_label, "Rede oculta selecionada. Digite o SSID manualmente.");
        }
        return;
    }

    lv_textarea_set_text(s_wifi_ssid_ta, (const char *)ap->ssid);
    strlcpy(s_wifi_selected_ssid, (const char *)ap->ssid, sizeof(s_wifi_selected_ssid));
    if (s_wifi_list) {
        uint32_t child_cnt = lv_obj_get_child_count(s_wifi_list);
        for (uint32_t i = 0; i < child_cnt; i++) {
            lv_obj_t *child = lv_obj_get_child(s_wifi_list, i);
            if (child) {
                lv_obj_clear_state(child, LV_STATE_CHECKED);
            }
        }
    }
    lv_obj_t *btn = lv_event_get_target(event);
    if (btn) {
        lv_obj_add_state(btn, LV_STATE_CHECKED);
    }
    if (s_wifi_feedback_label) {
        lv_label_set_text_fmt(s_wifi_feedback_label, "SSID selecionado: %s", (const char *)ap->ssid);
    }
}

static void wifi_password_focus_event_cb(lv_event_t *event)
{
    if (!s_wifi_keyboard) {
        return;
    }
    lv_event_code_t code = lv_event_get_code(event);
    lv_obj_t *target = lv_event_get_target(event);
    if (!target) {
        return;
    }

    switch (code) {
    case LV_EVENT_FOCUSED:
    case LV_EVENT_CLICKED:
        s_wifi_keyboard_target = target;
        lv_keyboard_set_textarea(s_wifi_keyboard, target);
        lv_obj_clear_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
        break;
    case LV_EVENT_DEFOCUSED:
    case LV_EVENT_READY:
    case LV_EVENT_CANCEL:
        if (s_wifi_keyboard_target == target) {
            s_wifi_keyboard_target = NULL;
            lv_obj_add_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        break;
    default:
        break;
    }
}

static void wifi_keyboard_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_READY && code != LV_EVENT_CANCEL) {
        return;
    }
    if (!s_wifi_keyboard) {
        return;
    }
    lv_obj_add_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
    s_wifi_keyboard_target = NULL;
}

static void wifi_quick_disconnect_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    wifi_manager_disconnect();
    s_wifi_connect_start_us = 0;
    s_wifi_timeout_reported = false;
    if (s_wifi_feedback_label) {
        lv_label_set_text(s_wifi_feedback_label, "Solicitando desconexao...");
    }
    wifi_update_status_labels_locked();
}

static void wifi_update_status_labels_locked(void)
{
    if (!s_wifi_status_label) {
        return;
    }
    wifi_status_info_t info = {0};
    if (wifi_manager_get_status(&info) != ESP_OK) {
        return;
    }
    s_wifi_status_cache = info;

    char buffer[128];
    buffer[0] = '\0';
    lv_color_t button_color = lv_color_hex(0x263238);
    bool show_text = false;

    if (info.connected) {
        const char *ssid = info.ssid[0] ? info.ssid : "Rede";
        if (info.ip[0]) {
            snprintf(buffer, sizeof(buffer), LV_SYMBOL_WIFI " %s (%s)", ssid, info.ip);
        } else {
            snprintf(buffer, sizeof(buffer), LV_SYMBOL_WIFI " %s", ssid);
        }
        button_color = lv_color_hex(0x1B5E20);
        show_text = true;
    } else if (info.connecting) {
        const char *ssid = info.ssid[0] ? info.ssid : "rede";
        snprintf(buffer, sizeof(buffer), LV_SYMBOL_REFRESH " Conectando a %s...", ssid);
        button_color = lv_color_hex(0xF57C00);
        show_text = true;
    }

    if (s_wifi_button) {
        lv_obj_set_style_bg_color(s_wifi_button, button_color, 0);
    }
    if (show_text) {
        lv_label_set_text(s_wifi_status_label, buffer);
    } else {
        lv_label_set_text(s_wifi_status_label, "");
    }

    if (s_wifi_quick_disconnect_btn) {
        if (info.connected || info.connecting) {
            lv_obj_clear_flag(s_wifi_quick_disconnect_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_wifi_quick_disconnect_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    wifi_refresh_feedback_from_status(&info);
}

static void wifi_refresh_feedback_from_status(const wifi_status_info_t *info)
{
    if (!info || !s_wifi_feedback_label) {
        return;
    }

    if (!info->initialized) {
        lv_label_set_text(s_wifi_feedback_label, "Wi-Fi ainda nao inicializado.");
        return;
    }

    if (info->connected) {
        const char *ssid = info->ssid[0] ? info->ssid : "Rede";
        if (info->ip[0]) {
            lv_label_set_text_fmt(s_wifi_feedback_label, "Conectado a %s\nIP: %s", ssid, info->ip);
        } else {
            lv_label_set_text_fmt(s_wifi_feedback_label, "Conectado a %s", ssid);
        }
        s_wifi_connect_start_us = 0;
        s_wifi_timeout_reported = false;
        return;
    }

    if (info->connecting) {
        if (s_wifi_connect_start_us == 0) {
            s_wifi_connect_start_us = esp_timer_get_time();
        }
        int64_t elapsed_ms = (esp_timer_get_time() - s_wifi_connect_start_us) / 1000;
        const char *ssid = info->ssid[0] ? info->ssid : "rede";
        lv_label_set_text_fmt(s_wifi_feedback_label,
                              "Conectando a %s... %llds",
                              ssid,
                              (long long)(elapsed_ms / 1000));
        if (elapsed_ms >= WIFI_CONNECT_TIMEOUT_MS && !s_wifi_timeout_reported) {
            s_wifi_timeout_reported = true;
            lv_label_set_text_fmt(s_wifi_feedback_label,
                                  "Nao foi possivel conectar a %s (tempo esgotado).",
                                  ssid);
            wifi_manager_disconnect();
            s_wifi_connect_start_us = 0;
        }
        return;
    }

    s_wifi_connect_start_us = 0;
    s_wifi_timeout_reported = false;
    if (info->last_disconnect_reason != WIFI_REASON_UNSPECIFIED) {
        lv_label_set_text_fmt(s_wifi_feedback_label,
                              "Conexao perdida: %s",
                              wifi_reason_to_text(info->last_disconnect_reason));
    } else {
        lv_label_set_text(s_wifi_feedback_label, "Sem conexao Wi-Fi.");
    }
}

static void wifi_set_panel_visible(bool show)
{
    if (!s_wifi_panel) {
        return;
    }

    if (show) {
        s_wifi_panel_visible = true;
        lv_obj_clear_flag(s_wifi_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_wifi_panel);
        wifi_update_status_labels_locked();
        wifi_request_scan();
    } else {
        s_wifi_panel_visible = false;
        lv_obj_add_flag(s_wifi_panel, LV_OBJ_FLAG_HIDDEN);
        if (s_wifi_keyboard) {
            lv_obj_add_flag(s_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
        s_wifi_keyboard_target = NULL;
    }
}

static void wifi_status_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    wifi_update_status_labels_locked();
}

static const char *wifi_reason_to_text(wifi_err_reason_t reason)
{
    switch (reason) {
    case WIFI_REASON_UNSPECIFIED:
        return "Sem conexao";
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        return "Falha de autenticacao";
    case WIFI_REASON_NO_AP_FOUND:
        return "Rede nao encontrada";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_TOOMANY:
    case WIFI_REASON_ASSOC_LEAVE:
    case WIFI_REASON_STA_LEAVING:
        return "Falha na associacao";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "Sinal perdido";
    case WIFI_REASON_CONNECTION_FAIL:
        return "Falha na conexao";
    case WIFI_REASON_AUTH_LEAVE:
    case WIFI_REASON_DISASSOC_DUE_TO_INACTIVITY:
        return "Desconectado pelo AP";
    default:
        return "Conexao encerrada";
    }
}

static const char *wifi_authmode_to_text(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN:
        return "Aberta";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
    default:
        return "Protegida";
    }
}

static void limpar_status_spans(void)
{
    if (!s_full_status) {
        return;
    }
    uint32_t total = lv_spangroup_get_span_count(s_full_status);
    while (total-- > 0) {
        lv_span_t *span = lv_spangroup_get_child(s_full_status, -1);
        if (!span) {
            break;
        }
        lv_spangroup_delete_span(s_full_status, span);
    }
}

static void append_plain_text(const char *fmt, ...)
{
    if (!s_full_status) {
        return;
    }
    lv_span_t *span = lv_spangroup_add_span(s_full_status);
    if (!span) {
        return;
    }
    lv_style_t *style = lv_span_get_style(span);
    if (style) {
        lv_style_set_text_color(style, lv_color_hex(0xECEFF1));
        lv_style_set_text_font(style, &lv_font_montserrat_20);
    }
    char buffer[96];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    lv_span_set_text(span, buffer);
}

static void append_colored_text(display_mode_t metric, const char *fmt, ...)
{
    if (!s_full_status) {
        return;
    }
    lv_span_t *span = lv_spangroup_add_span(s_full_status);
    if (!span) {
        return;
    }
    lv_style_t *style = lv_span_get_style(span);
    if (style) {
        uint32_t cor = s_metric_colors[metric];
        lv_style_set_text_color(style, lv_color_hex(cor));
        lv_style_set_text_font(style, &lv_font_montserrat_20);
    }
    char buffer[96];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    lv_span_set_text(span, buffer);
}

static uint32_t calcular_limite_distancia_cm(float distancia_m)
{
    static const uint32_t limites_cm[] = {
        10,        // 0.10 m
        25,        // 0.25 m
        50,        // 0.50 m
        100,       // 1 m
        250,       // 2.5 m
        500,       // 5 m
        1000,      // 10 m
        2500,      // 25 m
        5000,      // 50 m
        10000,     // 100 m
        25000,     // 250 m
        50000,     // 500 m
        100000,    // 1 km
        250000,    // 2.5 km
        500000,    // 5 km
        1000000,   // 10 km
        2500000,   // 25 km
        5000000,   // 50 km
        10000000,  // 100 km
    };

    uint32_t distancia_cm = (uint32_t)lroundf(distancia_m * 100.0f);
    const size_t total = sizeof(limites_cm) / sizeof(limites_cm[0]);
    for (size_t i = 0; i < total; i++) {
        if (distancia_cm <= limites_cm[i]) {
            return limites_cm[i];
        }
    }
    return limites_cm[total - 1];
}

static lv_color_t obter_cor_intervalo_rpm(uint32_t rpm)
{
    if (rpm <= 4000) {
        return lv_color_hex(0x4CAF50); // verde
    }
    if (rpm <= 5000) {
        return lv_color_hex(0x2E7D32); // verde escuro
    }
    if (rpm <= 6000) {
        return lv_color_hex(0xFFEB3B); // amarelo
    }
    if (rpm <= 7000) {
        return lv_color_hex(0xFBC02D); // amarelo escuro
    }
    if (rpm <= 8000) {
        return lv_color_hex(0xFB8C00); // laranja
    }
    if (rpm <= 9000) {
        return lv_color_hex(0xE53935); // vermelho padrão
    }
    return lv_color_hex(0xFF1744); // vermelho vibrante acima de 9000
}

static void update_scope_wave(uint32_t freq_hz)
{
    if (!s_full_scope_chart || !s_full_scope_series) {
        return;
    }

    uint16_t point_cnt = lv_chart_get_point_count(s_full_scope_chart);
    if (point_cnt == 0) {
        point_cnt = SCOPE_POINT_COUNT;
        lv_chart_set_point_count(s_full_scope_chart, point_cnt);
    }

    if (freq_hz > 220) {
        freq_hz = 220;
    }

    float cycles = 1.0f + ((float)freq_hz / 220.0f) * 4.0f;
    for (uint16_t i = 0; i < point_cnt; i++) {
        float phase = ((float)i / (float)(point_cnt - 1)) * cycles;
        float frac = phase - floorf(phase);
        float value = frac < 0.5f ? 90.0f : -90.0f;
        lv_chart_set_value_by_id(s_full_scope_chart, s_full_scope_series, i, (int32_t)value);
    }
    lv_chart_refresh(s_full_scope_chart);
}

static void limitar_curso(void)
{
    if (s_config_curso.curso_cm < CURSO_MIN_CM) {
        s_config_curso.curso_cm = CURSO_MIN_CM;
    } else if (s_config_curso.curso_cm > CURSO_MAX_CM) {
        s_config_curso.curso_cm = CURSO_MAX_CM;
    }
}

static void solicitar_salvar_curso(void)
{
    if (s_callbacks.ao_solicitar_salvar_curso) {
        s_callbacks.ao_solicitar_salvar_curso(s_config_curso.curso_cm);
    }
}
