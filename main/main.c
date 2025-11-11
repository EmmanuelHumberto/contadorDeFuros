#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

LV_FONT_DECLARE(lv_font_montserrat_14);
LV_FONT_DECLARE(lv_font_montserrat_20);
LV_FONT_DECLARE(lv_font_montserrat_28);
LV_FONT_DECLARE(lv_font_montserrat_48);
LV_IMAGE_DECLARE(liga_d_logo);

#define LCD_H_RES (800)
#define LCD_V_RES (480)

#define LCD_DRAW_BUFFER_HEIGHT  (80)
#define LCD_BOUNCE_BUFFER_LINES (10)

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

/* Entrada para contagem de pulsos (usar um GPIO livre do conector PORT1) */
#define INPUT_SIGNAL_GPIO    GPIO_NUM_16

/* Constantes da aplicacao */
#define METRICS_TASK_STACK           4096
#define METRICS_TASK_PRIO            4
#define METRICS_PERIOD_MS            100
#define SIGNAL_IDLE_TIMEOUT_MS       1000
#define RESET_TIMEOUT_MS             30000
#define COURSE_DEFAULT_CM            3.5f
#define COURSE_MIN_MM                1.0f
#define COURSE_MAX_MM                5.0f
#define COURSE_MIN_CM                (COURSE_MIN_MM / 10.0f)
#define COURSE_MAX_CM                (COURSE_MAX_MM / 10.0f)
#define STORAGE_NAMESPACE            "cfg"
#define STORAGE_KEY_CURSO            "curso"
#define SCOPE_POINT_COUNT            (100)

typedef struct {
    uint32_t frequencia_hz;
    uint32_t rpm;
    uint32_t velocidade_cm_s;
    float distancia_m;
    uint32_t furos;
    uint64_t tempo_sinal_ms;
} ui_data_t;

typedef enum {
    DISPLAY_FREQUENCIA = 0,
    DISPLAY_RPM,
    DISPLAY_VELOCIDADE,
    DISPLAY_CURSO,
    DISPLAY_DISTANCIA,
    DISPLAY_FUROS,
    DISPLAY_MODE_COUNT,
} display_mode_t;

static const char *TAG = "ContadorDeFuros";

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
static lv_obj_t *s_status_label;
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
static lv_obj_t *s_full_furos_timer_label;
static card_ui_t s_cards[DISPLAY_MODE_COUNT];
static ui_layout_t s_layout_mode = UI_LAYOUT_GRID;

static const char *s_metric_titles[DISPLAY_MODE_COUNT] = {
    [DISPLAY_FREQUENCIA] = "Frequencia",
    [DISPLAY_RPM] = "RPM",
    [DISPLAY_VELOCIDADE] = "Velocidade",
    [DISPLAY_CURSO] = "Curso",
    [DISPLAY_DISTANCIA] = "Distancia",
    [DISPLAY_FUROS] = "Total de furos",
};

static const uint32_t s_metric_colors[DISPLAY_MODE_COUNT] = {
    [DISPLAY_FREQUENCIA] = 0x1976D2,
    [DISPLAY_RPM] = 0x00897B,
    [DISPLAY_VELOCIDADE] = 0x6A1B9A,
    [DISPLAY_CURSO] = 0xF57C00,
    [DISPLAY_DISTANCIA] = 0x5D4037,
    [DISPLAY_FUROS] = 0xC62828,
};

/* Estado dos dados exibidos */
static float s_curso_cm = COURSE_DEFAULT_CM;
static bool s_modo_edicao = false;
static display_mode_t s_display_mode = DISPLAY_FREQUENCIA;
static ui_data_t s_ui_snapshot = {0};

/* Controle do sinal de entrada */
static portMUX_TYPE s_pulse_spinlock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_periodo_us = 0;
static volatile uint32_t s_total_furos = 0;
static volatile int64_t s_last_pulse_us = 0;
static volatile int64_t s_last_interrupt_us = 0;
static volatile int64_t s_last_update_ms = 0;
static volatile bool s_signal_active = false;
static volatile int64_t s_signal_start_ms = 0;
static volatile uint64_t s_signal_time_ms = 0;

/* Protecao para o snapshot usado pela interface */
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
static void configure_input_gpio(void);
static void pulse_isr(void *arg);
static void reset_metrics_locked(void);
static void metrics_task(void *arg);
static void refresh_ui(void);
static void apply_ui_locked(const ui_data_t *data);
static void update_cards_ui(const ui_data_t *data);
static void update_fullscreen_ui(const ui_data_t *data);
static void get_metric_text(display_mode_t mode, const ui_data_t *data, char *valor, size_t valor_len, char *unidade, size_t unidade_len);
static void salvar_curso(void);
static void carregar_curso(void);
static void formatar_distancia(char *buffer, size_t len, float distancia_m);
static void formatar_tempo(uint64_t tempo_ms, char *buffer, size_t len);
static void update_scope_wave(uint32_t freq_hz);
static void limitar_curso(void);

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    carregar_curso();
    configure_input_gpio();

    ESP_ERROR_CHECK(init_rgb_panel());
    ESP_ERROR_CHECK(init_lvgl_port());
    ESP_ERROR_CHECK(init_touch_panel());
    build_ui();
    turn_on_backlight();

    ESP_LOGI(TAG, "Sistema pronto. Tocando na tela muda a pagina; toque longo alterna modo de edicao do curso.");

    xTaskCreate(metrics_task, "metrics", METRICS_TASK_STACK, NULL, METRICS_TASK_PRIO, NULL);
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
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101418), 0);
    s_layout_mode = UI_LAYOUT_GRID;

    s_grid_container = lv_obj_create(screen);
    lv_obj_remove_style_all(s_grid_container);
    lv_obj_set_size(s_grid_container, LV_PCT(100), LV_PCT(85));
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
        lv_obj_set_style_bg_opa(card, LV_OPA_40, 0);
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

    s_status_label = lv_label_create(screen);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_label_set_text(s_status_label, "Toque em um painel para ampliar");

    s_fullscreen_container = lv_obj_create(screen);
    lv_obj_remove_style_all(s_fullscreen_container);
    lv_obj_set_size(s_fullscreen_container, LV_PCT(100), LV_PCT(100));
    lv_obj_align(s_fullscreen_container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_fullscreen_container, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(s_fullscreen_container, LV_OPA_COVER, 0);
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

    s_full_furos_timer_label = lv_label_create(s_fullscreen_container);
    lv_obj_set_style_text_color(s_full_furos_timer_label, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(s_full_furos_timer_label, &lv_font_montserrat_20, 0);
    lv_obj_align(s_full_furos_timer_label, LV_ALIGN_CENTER, 0, 130);
    lv_obj_add_flag(s_full_furos_timer_label, LV_OBJ_FLAG_HIDDEN);

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

    s_full_status = lv_label_create(s_fullscreen_container);
    lv_obj_set_style_text_color(s_full_status, lv_color_hex(0xECEFF1), 0);
    lv_obj_set_style_text_font(s_full_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_full_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_full_status, LV_ALIGN_BOTTOM_MID, 0, -12);

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
        s_curso_cm += 0.01f;
        limitar_curso();
        salvar_curso();
        ui_changed = true;
    } else if (code == LV_EVENT_LONG_PRESSED) {
        s_modo_edicao = !s_modo_edicao;
        if (!s_modo_edicao) {
            salvar_curso();
        }
        ui_changed = true;
    } else if (code == LV_EVENT_LONG_PRESSED_REPEAT && s_modo_edicao) {
        s_curso_cm -= 0.01f;
        limitar_curso();
        salvar_curso();
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
    lv_obj_add_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_clear_flag(s_status_label, LV_OBJ_FLAG_HIDDEN);
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

static void configure_input_gpio(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << INPUT_SIGNAL_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(INPUT_SIGNAL_GPIO, pulse_isr, NULL));
}

static void IRAM_ATTR pulse_isr(void *arg)
{
    const int64_t now_us = esp_timer_get_time();

    portENTER_CRITICAL_ISR(&s_pulse_spinlock);
    if (now_us - s_last_interrupt_us > 1000) {
        s_periodo_us = (uint32_t)(now_us - s_last_pulse_us);
        s_last_pulse_us = now_us;
        s_last_update_ms = now_us / 1000;
        s_total_furos++;
        if (!s_signal_active) {
            s_signal_active = true;
            s_signal_start_ms = s_last_update_ms;
        }
    }
    s_last_interrupt_us = now_us;
    portEXIT_CRITICAL_ISR(&s_pulse_spinlock);
}

static void reset_metrics_locked(void)
{
    s_periodo_us = 0;
    s_total_furos = 0;
    s_last_pulse_us = 0;
    s_last_interrupt_us = 0;
    s_last_update_ms = 0;
    s_signal_active = false;
    s_signal_start_ms = 0;
    s_signal_time_ms = 0;
}

static void metrics_task(void *arg)
{
    int64_t last_distance_ms = esp_timer_get_time() / 1000;
    float distancia_m = 0.0f;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(METRICS_PERIOD_MS));

        const int64_t now_ms = esp_timer_get_time() / 1000;

        uint32_t period_us;
        uint32_t furos;
        bool signal_active;
        int64_t last_update_ms;
        uint64_t tempo_ms;

        portENTER_CRITICAL(&s_pulse_spinlock);
        period_us = s_periodo_us;
        furos = s_total_furos;
        signal_active = s_signal_active;
        last_update_ms = s_last_update_ms;
        tempo_ms = s_signal_time_ms;
        portEXIT_CRITICAL(&s_pulse_spinlock);

        uint32_t frequencia = 0;
        uint32_t rpm = 0;
        uint32_t velocidade_cm_s = 0;

        if (period_us > 0) {
            frequencia = 1000000UL / period_us;
            rpm = frequencia * 60U;
            velocidade_cm_s = (uint32_t)(frequencia * s_curso_cm / 10.0f);

            if (last_distance_ms > 0) {
                const float delta_s = (now_ms - last_distance_ms) / 1000.0f;
                distancia_m += (velocidade_cm_s / 100.0f) * delta_s;
            }
            last_distance_ms = now_ms;
        }

        if (signal_active && last_update_ms > 0 && (now_ms - last_update_ms) > SIGNAL_IDLE_TIMEOUT_MS) {
            portENTER_CRITICAL(&s_pulse_spinlock);
            s_signal_active = false;
            s_signal_time_ms += now_ms - s_signal_start_ms;
            tempo_ms = s_signal_time_ms;
            portEXIT_CRITICAL(&s_pulse_spinlock);
        } else if (!signal_active && last_update_ms > 0 && (now_ms - last_update_ms) > RESET_TIMEOUT_MS) {
            portENTER_CRITICAL(&s_pulse_spinlock);
            reset_metrics_locked();
            portEXIT_CRITICAL(&s_pulse_spinlock);
            distancia_m = 0;
            last_distance_ms = now_ms;
            continue;
        }

        ui_data_t snapshot = {
            .frequencia_hz = frequencia,
            .rpm = rpm,
            .velocidade_cm_s = velocidade_cm_s,
            .distancia_m = distancia_m,
            .furos = furos,
            .tempo_sinal_ms = tempo_ms,
        };

        portENTER_CRITICAL(&s_ui_spinlock);
        s_ui_snapshot = snapshot;
        portEXIT_CRITICAL(&s_ui_spinlock);

        refresh_ui();
    }
}

static void refresh_ui(void)
{
    ui_data_t snapshot;
    portENTER_CRITICAL(&s_ui_spinlock);
    snapshot = s_ui_snapshot;
    portEXIT_CRITICAL(&s_ui_spinlock);

    if (lvgl_port_lock(pdMS_TO_TICKS(50))) {
        apply_ui_locked(&snapshot);
        lvgl_port_unlock();
    }
}

static void apply_ui_locked(const ui_data_t *data)
{
    update_cards_ui(data);

    if (s_layout_mode == UI_LAYOUT_FULLSCREEN) {
        update_fullscreen_ui(data);
    } else if (s_status_label) {
        lv_label_set_text_fmt(s_status_label,
                              "Curso: %.1f mm | Toque em um painel para ampliar (duplo clique para voltar)",
                              s_curso_cm * 10.0f);
    }
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
    lv_obj_set_style_bg_color(s_fullscreen_container, lv_color_hex(s_metric_colors[s_display_mode]), 0);
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
    if (s_full_furos_timer_label) {
        lv_obj_add_flag(s_full_furos_timer_label, LV_OBJ_FLAG_HIDDEN);
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
    } else if (s_full_arc && (s_display_mode == DISPLAY_RPM || s_display_mode == DISPLAY_VELOCIDADE)) {
        uint32_t max_value;
        uint32_t current;
        const char *scale_text;

        if (s_display_mode == DISPLAY_RPM) {
            max_value = 15000;
            current = data->rpm;
            scale_text = "0 - 15k rpm";
        } else {
            max_value = 500;
            current = data->velocidade_cm_s;
            scale_text = "Velocidade (cm/s)";
        }

        if (current > max_value) {
            current = max_value;
        }
        lv_arc_set_range(s_full_arc, 0, max_value);
        lv_arc_set_value(s_full_arc, current);
        lv_obj_clear_flag(s_full_arc, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_full_arc, LV_ALIGN_CENTER, 0, 10);
        lv_obj_align(s_full_value, LV_ALIGN_CENTER, 0, 10);
        lv_obj_align_to(s_full_unit, s_full_value, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
        if (s_full_arc_label) {
            lv_label_set_text(s_full_arc_label, scale_text);
            lv_obj_clear_flag(s_full_arc_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_align(s_full_arc_label, LV_ALIGN_CENTER, 0, 160);
        }
    } else if (s_display_mode == DISPLAY_CURSO && s_full_course_arc) {
        float curso_mm = s_curso_cm * 10.0f;
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
        if (distancia_m < 0) {
            distancia_m = 0;
        }
        if (distancia_m > 100000.0f) {
            distancia_m = 100000.0f;
        }
        lv_bar_set_range(s_full_bar, 0, 100000);
        lv_bar_set_value(s_full_bar, (int32_t)distancia_m, LV_ANIM_OFF);
        lv_obj_clear_flag(s_full_bar, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_full_bar_label, "0 m ---------------------------- 100 km");
        lv_obj_clear_flag(s_full_bar_label, LV_OBJ_FLAG_HIDDEN);
        if (s_full_timer_label) {
            char tempo_txt[32];
            formatar_tempo(data->tempo_sinal_ms, tempo_txt, sizeof(tempo_txt));
            lv_label_set_text_fmt(s_full_timer_label, "Tempo: %s", tempo_txt);
            lv_obj_clear_flag(s_full_timer_label, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (s_display_mode == DISPLAY_FUROS && s_full_furos_timer_label) {
        char tempo_txt[32];
        formatar_tempo(data->tempo_sinal_ms, tempo_txt, sizeof(tempo_txt));
        lv_label_set_text_fmt(s_full_furos_timer_label, "Tempo: %s", tempo_txt);
        lv_obj_clear_flag(s_full_furos_timer_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_full_value, LV_ALIGN_CENTER, 0, -10);
        lv_obj_align_to(s_full_unit, s_full_value, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    }

    const char *hint = "Toque duplo para voltar";
    if (s_display_mode == DISPLAY_CURSO) {
        hint = s_modo_edicao ? "Modo edicao ativo: toque curto +1 mm, segure para reduzir"
                             : "Toque longo para editar o curso";
    }

    lv_label_set_text_fmt(s_full_status,
                          "Curso: %.1f mm | %s",
                          s_curso_cm * 10.0f, hint);
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
        snprintf(valor, valor_len, "%.1f", s_curso_cm * 10.0f);
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
    if (s_curso_cm < COURSE_MIN_CM) {
        s_curso_cm = COURSE_MIN_CM;
    } else if (s_curso_cm > COURSE_MAX_CM) {
        s_curso_cm = COURSE_MAX_CM;
    }
}

static void salvar_curso(void)
{
    nvs_handle_t handle;
    if (nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_blob(handle, STORAGE_KEY_CURSO, &s_curso_cm, sizeof(s_curso_cm));
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void carregar_curso(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Falha ao abrir NVS (%s), usando curso padrao", esp_err_to_name(err));
        s_curso_cm = COURSE_DEFAULT_CM;
        limitar_curso();
        return;
    }

    size_t size = sizeof(s_curso_cm);
    err = nvs_get_blob(handle, STORAGE_KEY_CURSO, &s_curso_cm, &size);
    if (err != ESP_OK) {
        s_curso_cm = COURSE_DEFAULT_CM;
        nvs_set_blob(handle, STORAGE_KEY_CURSO, &s_curso_cm, sizeof(s_curso_cm));
        nvs_commit(handle);
    }
    limitar_curso();
    nvs_close(handle);
}
