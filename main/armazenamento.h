#pragma once

#include "esp_err.h"
#include "app_types.h"

esp_err_t armazenamento_inicializar(configuracao_curso_t *config);
esp_err_t armazenamento_salvar_curso(const configuracao_curso_t *config);
