#include "armazenamento.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "armazenamento";
static const char *ESPACO = "cfg";
static const char *CHAVE_CURSO = "curso";

static void aplicar_limites_curso(configuracao_curso_t *config)
{
    if (config->curso_cm < CURSO_MIN_CM) {
        config->curso_cm = CURSO_MIN_CM;
    } else if (config->curso_cm > CURSO_MAX_CM) {
        config->curso_cm = CURSO_MAX_CM;
    }
}

esp_err_t armazenamento_inicializar(configuracao_curso_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(ESPACO, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Falha ao abrir NVS (%s), usando valor padrao", esp_err_to_name(err));
        config->curso_cm = CURSO_MAX_CM * 0.7f;
        aplicar_limites_curso(config);
        return err;
    }

    size_t tamanho = sizeof(config->curso_cm);
    err = nvs_get_blob(handle, CHAVE_CURSO, &config->curso_cm, &tamanho);
    if (err != ESP_OK) {
        config->curso_cm = CURSO_MAX_CM * 0.7f;
        aplicar_limites_curso(config);
        nvs_set_blob(handle, CHAVE_CURSO, &config->curso_cm, sizeof(config->curso_cm));
        nvs_commit(handle);
    } else {
        aplicar_limites_curso(config);
    }
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t armazenamento_salvar_curso(const configuracao_curso_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ESPACO, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS indisponivel (%s)", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(handle, CHAVE_CURSO, &config->curso_cm, sizeof(config->curso_cm));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
