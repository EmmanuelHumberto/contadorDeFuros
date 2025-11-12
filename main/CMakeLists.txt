idf_component_register(
    SRCS
        "main.c"
        "interface_usuario.c"
        "armazenamento.c"
        "metricas.c"
        "assets/liga_d_logo.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_lvgl_port esp_lcd_touch_gt911 esp_lcd driver esp_timer nvs_flash
)
