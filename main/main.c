#include <stdio.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "app_types.h"
#include "armazenamento.h"
#include "interface_usuario.h"
#include "metricas.h"

static const char *TAG = "app_main";

static configuracao_curso_t s_configuracao = {
    .curso_cm = CURSO_MAX_CM * 0.7f,
};

static void salvar_curso_callback(float novo_curso_cm)
{
    s_configuracao.curso_cm = novo_curso_cm;
    esp_err_t err = armazenamento_salvar_curso(&s_configuracao);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao salvar curso (0x%x)", err);
    }
    metricas_atualizar_curso(novo_curso_cm);
}

static void metricas_callback(const dados_medidos_t *dados)
{
    interface_usuario_atualizar(dados);
}

static void inicializar_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    inicializar_nvs();

    ESP_LOGI(TAG, "Carregando configuracoes persistentes...");
    ESP_ERROR_CHECK(armazenamento_inicializar(&s_configuracao));

    ui_callbacks_t callbacks = {
        .ao_solicitar_salvar_curso = salvar_curso_callback,
    };

    ESP_LOGI(TAG, "Inicializando interface grafica...");
    ESP_ERROR_CHECK(interface_usuario_inicializar(&s_configuracao, &callbacks));

    ESP_LOGI(TAG, "Inicializando modulo de metricas...");
    ESP_ERROR_CHECK(metricas_inicializar(&s_configuracao, metricas_callback));

    ESP_LOGI(TAG, "Sistema pronto. Toque na tela para navegar entre os cards.");
}
