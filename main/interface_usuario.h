#pragma once

#include "esp_err.h"
#include "app_types.h"

typedef struct {
    void (*ao_solicitar_salvar_curso)(float novo_valor_cm);
    void (*ao_solicitar_wifi_conectar)(const char *ssid, const char *senha);
} ui_callbacks_t;

esp_err_t interface_usuario_inicializar(const configuracao_curso_t *config, const ui_callbacks_t *callbacks);
void interface_usuario_atualizar(const dados_medidos_t *dados);
void interface_usuario_configurar_curso(float curso_cm);
