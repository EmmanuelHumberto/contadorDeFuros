#include "interface_usuario.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "driver/gpio.h"

static const char *TAG = "interface_ui";

#define LCD_H_RES 800
#define LCD_V_RES 480
#define LCD_DRAW_BUFFER_HEIGHT 80
#define LCD_BOUNCE_BUFFER_LINES 10

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

static esp_lcd_panel_handle_t s_panel_handle;
static lv_display_t *s_display = NULL;
static lv_indev_t *s_touch_indev = NULL;
static esp_lcd_touch_handle_t s_touch_handle = NULL;

typedef enum {
    TELA_FREQUENCIA = 0,
    TELA_RPM,
    TELA_VELOCIDADE,
    TELA_CURSO,
    TELA_DISTANCIA,
    TELA_FUROS,
    TOTAL_TELAS,
} tela_id_t;

typedef struct {
    lv_obj_t *card;
    lv_obj_t *titulo;
    lv_obj_t *valor;
    lv_obj_t *unidade;
} widget_card_t;

static widget_card_t s_cards[TOTAL_TELAS];

static lv_obj_t *s_status;
static lv_obj_t *s_tela_expandida;
static lv_obj_t *s_titulo_expandido;
static lv_obj_t *s_valor_expandido;
static lv_obj_t *s_unidade_expandida;
static lv_obj_t *s_status_expandido;
static lv_obj_t *s_gauge_arc;
static lv_obj_t *s_gauge_legenda;
static lv_obj_t *s_barra_distancia;
static lv_obj_t *s_barra_legenda;
static lv_obj_t *s_barra_timer;
static lv_obj_t *s_grafico_scope;
static lv_chart_series_t *s_scope_serie;
static lv_obj_t *s_scope_legenda;
static lv_obj_t *s_visor_curso;
static lv_obj_t *s_timer_furos;

static configuracao_curso_t s_config_curso;
static ui_callbacks_t s_callbacks = {0};
static tela_id_t s_tela_atual = TELA_FREQUENCIA;
static bool s_modo_edicao = false;

static void inicializar_rgb(void);
static void inicializar_lvgl(void);
static void construir_interface(void);
static void atualizar_cards(const dados_medidos_t *dados);
static void atualizar_tela_expandida(const dados_medidos_t *dados);
static void evento_card(lv_event_t *evento);
static void evento_tela_expandida(lv_event_t *evento);
static void atualizar_scope(uint32_t freq_hz);
static void atualizar_legendas(void);
static void configurar_backlight(void);

esp_err_t interface_usuario_inicializar(const configuracao_curso_t *config, const ui_callbacks_t *callbacks)
{
    if (!config || !callbacks || !callbacks->ao_solicitar_salvar_curso) {
        return ESP_ERR_INVALID_ARG;
    }
    s_config_curso = *config;
    s_callbacks = *callbacks;
    inicializar_rgb();
    inicializar_lvgl();
    construir_interface();
    return ESP_OK;
}

void interface_usuario_atualizar(const dados_medidos_t *dados)
{
    if (!dados) {
        return;
    }
    atualizar_cards(dados);
    atualizar_tela_expandida(dados);
}

void interface_usuario_configurar_curso(float curso_cm)
{
    if (curso_cm < CURSO_MIN_CM) curso_cm = CURSO_MIN_CM;
    if (curso_cm > CURSO_MAX_CM) curso_cm = CURSO_MAX_CM;
    s_config_curso.curso_cm = curso_cm;
    atualizar_legendas();
}

static void inicializar_rgb(void)
{
    esp_lcd_rgb_panel_config_t config = {
        .clk_src = LCD_CLK_SRC_PLL160M,
        .data_width = 16,
        .de_gpio_num = LCD_PIN_DE,
        .pclk_gpio_num = LCD_PIN_PCLK,
        .vsync_gpio_num = LCD_PIN_VSYNC,
        .hsync_gpio_num = LCD_PIN_HSYNC,
        .disp_gpio_num = LCD_PIN_DISP_EN,
        .timings = LCD_RGB_TIMING(),
        .flags.fb_in_psram = 1,
        .num_fbs = 2,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0)
        .bounce_buffer_size_px = LCD_H_RES * LCD_BOUNCE_BUFFER_LINES,
#else
        .psram_trans_align = 64,
#endif
    };
    for (size_t i = 0; i < 16; i++) {
        config.data_gpio_nums[i] = s_lcd_data_pins[i];
    }
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&config, &s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));
    esp_err_t err = esp_lcd_panel_disp_on_off(s_panel_handle, true);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_ERROR_CHECK(err);
    }
    configurar_backlight();
}

static void inicializar_lvgl(void)
{
    const lvgl_port_cfg_t lv_port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lv_port_cfg));

    lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = s_panel_handle,
        .buffer_size = LCD_H_RES * LCD_DRAW_BUFFER_HEIGHT,
        .double_buffer = false,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
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

    s_display = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    ESP_ERROR_CHECK(s_display ? ESP_OK : ESP_FAIL);

    i2c_master_bus_handle_t i2c_bus = NULL;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = TOUCH_I2C_SDA,
        .scl_io_num = TOUCH_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus));

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_cfg.scl_speed_hz = TOUCH_I2C_CLK_HZ;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io_handle));

    const esp_lcd_touch_config_t touch_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = TOUCH_RST_GPIO,
        .int_gpio_num = TOUCH_INT_GPIO,
        .levels = { .reset = 0, .interrupt = 0 },
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io_handle, &touch_cfg, &s_touch_handle));

    const lvgl_port_touch_cfg_t lv_touch_cfg = {
        .disp = s_display,
        .handle = s_touch_handle,
    };
    s_touch_indev = lvgl_port_add_touch(&lv_touch_cfg);
    ESP_ERROR_CHECK(s_touch_indev ? ESP_OK : ESP_FAIL);
}

static void construir_interface(void)
{
    if (!lvgl_port_lock(portMAX_DELAY)) {
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101418), 0);

    s_tela_atual = TELA_FREQUENCIA;
    s_modo_edicao = false;

    lv_obj_t *grid = lv_obj_create(screen);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LV_PCT(100), LV_PCT(85));
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_all(grid, 16, 0);
    lv_obj_set_style_pad_row(grid, 12, 0);
    lv_obj_set_style_pad_column(grid, 16, 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_SPACE_EVENLY);

    static const char *TITULOS[TOTAL_TELAS] = {
        "Frequencia", "RPM", "Velocidade", "Curso", "Distancia", "Total de furos"
    };
    static const uint32_t CORES[TOTAL_TELAS] = {
        0x1976D2, 0x00897B, 0x6A1B9A, 0xF57C00, 0x5D4037, 0xC62828
    };

    for (int i = 0; i < TOTAL_TELAS; i++) {
        lv_obj_t *card = lv_obj_create(grid);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, LV_PCT(48), 120);
        lv_obj_set_style_radius(card, 14, 0);
        lv_obj_set_style_pad_all(card, 16, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(CORES[i]), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_40, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, evento_card, LV_EVENT_ALL, (void *)(intptr_t)i);

        lv_obj_t *title = lv_label_create(card);
        lv_obj_set_style_text_color(title, lv_color_hex(0xF5F5F5), 0);
        lv_label_set_text(title, TITULOS[i]);

        lv_obj_t *value = lv_label_create(card);
        lv_obj_set_style_text_color(value, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(value, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(value, LV_ALIGN_CENTER, 0, -10);
        lv_label_set_text(value, "--");

        lv_obj_t *unit = lv_label_create(card);
        lv_obj_set_style_text_color(unit, lv_color_hex(0xFFECB3), 0);
        lv_obj_set_style_text_font(unit, &lv_font_montserrat_20, 0);
        lv_label_set_text(unit, "");

        s_cards[i] = (widget_card_t){
            .card = card,
            .titulo = title,
            .valor = value,
            .unidade = unit,
        };
    }

    s_status = lv_label_create(screen);
    lv_obj_set_style_text_color(s_status, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, 0);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_label_set_text(s_status, "Toque em um painel para ampliar");

    s_tela_expandida = lv_obj_create(screen);
    lv_obj_remove_style_all(s_tela_expandida);
    lv_obj_set_size(s_tela_expandida, LV_PCT(100), LV_PCT(100));
    lv_obj_align(s_tela_expandida, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_tela_expandida, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(s_tela_expandida, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_tela_expandida, 32, 0);
    lv_obj_add_flag(s_tela_expandida, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_tela_expandida, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_tela_expandida, evento_tela_expandida, LV_EVENT_ALL, NULL);

    s_titulo_expandido = lv_label_create(s_tela_expandida);
    lv_obj_set_style_text_color(s_titulo_expandido, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_titulo_expandido, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(s_titulo_expandido, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_titulo_expandido, LV_ALIGN_TOP_MID, 0, 8);

    s_valor_expandido = lv_label_create(s_tela_expandida);
    lv_obj_set_style_text_color(s_valor_expandido, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_valor_expandido, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(s_valor_expandido, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_valor_expandido, LV_ALIGN_CENTER, 0, -80);

    s_unidade_expandida = lv_label_create(s_tela_expandida);
    lv_obj_set_style_text_color(s_unidade_expandida, lv_color_hex(0xF5F5F5), 0);
    lv_obj_set_style_text_font(s_unidade_expandida, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_align(s_unidade_expandida, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(s_unidade_expandida, s_valor_expandido, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);

    s_gauge_arc = lv_arc_create(s_tela_expandida);
    lv_obj_set_size(s_gauge_arc, 288, 288);
    lv_arc_set_rotation(s_gauge_arc, 135);
    lv_arc_set_bg_angles(s_gauge_arc, 0, 270);
    lv_arc_set_value(s_gauge_arc, 0);
    lv_obj_remove_style(s_gauge_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_gauge_arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_gauge_arc, 14, LV_PART_INDICATOR);
    lv_obj_add_flag(s_gauge_arc, LV_OBJ_FLAG_HIDDEN);

    s_gauge_legenda = lv_label_create(s_tela_expandida);
    lv_obj_set_style_text_color(s_gauge_legenda, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(s_gauge_legenda, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_gauge_legenda, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_gauge_legenda, LV_ALIGN_CENTER, 0, 160);
    lv_obj_add_flag(s_gauge_legenda, LV_OBJ_FLAG_HIDDEN);

    s_barra_distancia = lv_bar_create(s_tela_expandida);
    lv_bar_set_range(s_barra_distancia, 0, 100000);
    lv_obj_set_size(s_barra_distancia, LV_PCT(80), 16);
    lv_obj_align(s_barra_distancia, LV_ALIGN_CENTER, 0, 60);
    lv_obj_add_flag(s_barra_distancia, LV_OBJ_FLAG_HIDDEN);

    s_barra_legenda = lv_label_create(s_tela_expandida);
    lv_obj_set_style_text_color(s_barra_legenda, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(s_barra_legenda, &lv_font_montserrat_20, 0);
    lv_obj_align(s_barra_legenda, LV_ALIGN_CENTER, 0, 90);
    lv_obj_add_flag(s_barra_legenda, LV_OBJ_FLAG_HIDDEN);

    s_barra_timer = lv_label_create(s_tela_expandida);
    lv_obj_set_style_text_color(s_barra_timer, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(s_barra_timer, &lv_font_montserrat_20, 0);
    lv_obj_align(s_barra_timer, LV_ALIGN_CENTER, 0, 120);
    lv_obj_add_flag(s_barra_timer, LV_OBJ_FLAG_HIDDEN);

    s_grafico_scope = lv_chart_create(s_tela_expandida);
    lv_obj_set_size(s_grafico_scope, LV_PCT(82), 162);
    lv_obj_align(s_grafico_scope, LV_ALIGN_CENTER, 0, -20);
    lv_chart_set_point_count(s_grafico_scope, 100);
    lv_chart_set_range(s_grafico_scope, LV_CHART_AXIS_PRIMARY_Y, -100, 100);
    lv_chart_set_type(s_grafico_scope, LV_CHART_TYPE_LINE);
    lv_chart_set_update_mode(s_grafico_scope, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_add_flag(s_grafico_scope, LV_OBJ_FLAG_HIDDEN);
    s_scope_serie = lv_chart_add_series(s_grafico_scope, lv_color_hex(0x00FFC0), LV_CHART_AXIS_PRIMARY_Y);

    s_scope_legenda = lv_label_create(s_tela_expandida);
    lv_obj_set_style_text_color(s_scope_legenda, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(s_scope_legenda, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_scope_legenda, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(s_scope_legenda, s_grafico_scope, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    lv_obj_add_flag(s_scope_legenda, LV_OBJ_FLAG_HIDDEN);

    s_visor_curso = lv_arc_create(s_tela_expandida);
    lv_arc_set_rotation(s_visor_curso, 135);
    lv_arc_set_bg_angles(s_visor_curso, 0, 270);
    lv_arc_set_range(s_visor_curso, 100, 500);
    lv_arc_set_value(s_visor_curso, 100);
    lv_obj_remove_style(s_visor_curso, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_visor_curso, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_visor_curso, 12, LV_PART_INDICATOR);
    lv_obj_add_flag(s_visor_curso, LV_OBJ_FLAG_HIDDEN);

    s_timer_furos = lv_label_create(s_tela_expandida);
    lv_obj_set_style_text_color(s_timer_furos, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_style_text_font(s_timer_furos, &lv_font_montserrat_20, 0);
    lv_obj_align(s_timer_furos, LV_ALIGN_CENTER, 0, 130);
    lv_obj_add_flag(s_timer_furos, LV_OBJ_FLAG_HIDDEN);

    s_status_expandido = lv_label_create(s_tela_expandida);
    lv_obj_set_style_text_color(s_status_expandido, lv_color_hex(0xECEFF1), 0);
    lv_obj_set_style_text_font(s_status_expandido, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_status_expandido, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status_expandido, LV_ALIGN_BOTTOM_MID, 0, -12);

    atualizar_legendas();
    lvgl_port_unlock();
}

static void atualizar_cards(const dados_medidos_t *dados)
{
    for (int i = 0; i < TOTAL_TELAS; i++) {
        widget_card_t *card = &s_cards[i];
        char texto_valor[48];
        char texto_unidade[32];
        switch (i) {
        case TELA_FREQUENCIA:
            snprintf(texto_valor, sizeof(texto_valor), "%" PRIu32, dados->frequencia_hz);
            snprintf(texto_unidade, sizeof(texto_unidade), "Hz");
            break;
        case TELA_RPM:
            snprintf(texto_valor, sizeof(texto_valor), "%" PRIu32, dados->rpm);
            snprintf(texto_unidade, sizeof(texto_unidade), "rpm");
            break;
        case TELA_VELOCIDADE:
            snprintf(texto_valor, sizeof(texto_valor), "%" PRIu32, dados->velocidade_cm_s);
            snprintf(texto_unidade, sizeof(texto_unidade), "cm/s");
            break;
        case TELA_CURSO:
            snprintf(texto_valor, sizeof(texto_valor), "%.1f", s_config_curso.curso_cm * 10.0f);
            snprintf(texto_unidade, sizeof(text
