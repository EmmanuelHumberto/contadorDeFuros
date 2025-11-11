#pragma once

#include "app_types.h"
#include "esp_err.h"

typedef void (*metricas_callback_t)(const dados_medidos_t *dados);

esp_err_t metricas_inicializar(const configuracao_curso_t *config, metricas_callback_t callback);
void metricas_atualizar_curso(float novo_curso_cm);
