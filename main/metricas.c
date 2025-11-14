#include "metricas.h"

#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

#define GPIO_SINAL               GPIO_NUM_16
#define TEMPO_DEBOUNCE_US        1000
#define TEMPO_IDLE_MS            1000
#define TEMPO_RESET_MS           30000
#define PILHA_TAREFA_METRICAS    4096
#define PRIORIDADE_TAREFA        4
#define INTERVALO_METRICAS_MS    100

static metricas_callback_t s_callback = NULL;
static float s_curso_cm = CURSO_MAX_CM * 0.7f;

static portMUX_TYPE s_spinlock_pulso = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_periodo_us = 0;
static volatile uint32_t s_total_furos = 0;
static volatile int64_t s_ultimo_pulso_us = 0;
static volatile int64_t s_ultimo_interrupt_us = 0;
static volatile int64_t s_ultima_atualizacao_ms = 0;
static volatile bool s_sinal_ativo = false;
static volatile int64_t s_inicio_sinal_ms = 0;
static volatile uint64_t s_tempo_sinal_ms = 0;

static void configurar_gpio(void);
static void isr_pulso(void *arg);
static void tarefa_metricas(void *param);

esp_err_t metricas_inicializar(const configuracao_curso_t *config, metricas_callback_t callback)
{
    if (!config || !callback) {
        return ESP_ERR_INVALID_ARG;
    }
    s_callback = callback;
    s_curso_cm = config->curso_cm;
    configurar_gpio();
    BaseType_t criada = xTaskCreate(tarefa_metricas, "metricas", PILHA_TAREFA_METRICAS, NULL, PRIORIDADE_TAREFA, NULL);
    return criada == pdPASS ? ESP_OK : ESP_FAIL;
}

void metricas_atualizar_curso(float novo_curso_cm)
{
    if (novo_curso_cm < CURSO_MIN_CM) {
        novo_curso_cm = CURSO_MIN_CM;
    } else if (novo_curso_cm > CURSO_MAX_CM) {
        novo_curso_cm = CURSO_MAX_CM;
    }
    s_curso_cm = novo_curso_cm;
}

static void configurar_gpio(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << GPIO_SINAL,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(GPIO_SINAL, isr_pulso, NULL));
}

static void IRAM_ATTR isr_pulso(void *arg)
{
    const int64_t agora_us = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&s_spinlock_pulso);
    if (agora_us - s_ultimo_interrupt_us > TEMPO_DEBOUNCE_US) {
        s_periodo_us = (uint32_t)(agora_us - s_ultimo_pulso_us);
        s_ultimo_pulso_us = agora_us;
        s_ultima_atualizacao_ms = agora_us / 1000;
        s_total_furos++;
        if (!s_sinal_ativo) {
            s_sinal_ativo = true;
            s_inicio_sinal_ms = s_ultima_atualizacao_ms;
        }
    }
    s_ultimo_interrupt_us = agora_us;
    portEXIT_CRITICAL_ISR(&s_spinlock_pulso);
}

static void tarefa_metricas(void *param)
{
    int64_t ultimo_ms = esp_timer_get_time() / 1000;
    float distancia_m = 0.0f;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(INTERVALO_METRICAS_MS));
        const int64_t agora_ms = esp_timer_get_time() / 1000;

        uint32_t periodo_capturado;
        uint32_t furos;
        bool sinal_ativo;
        int64_t ultima_atualizacao;
        uint64_t tempo_ms;

        portENTER_CRITICAL(&s_spinlock_pulso);
        periodo_capturado = s_periodo_us;
        furos = s_total_furos;
        sinal_ativo = s_sinal_ativo;
        ultima_atualizacao = s_ultima_atualizacao_ms;
        tempo_ms = s_tempo_sinal_ms;
        portEXIT_CRITICAL(&s_spinlock_pulso);

        uint32_t frequencia = 0;
        uint32_t rpm = 0;
        uint32_t velocidade_cm_s = 0;

        if (periodo_capturado > 0) {
            frequencia = 1000000UL / periodo_capturado;
            rpm = frequencia * 60U;
            velocidade_cm_s = (uint32_t)lroundf(s_curso_cm * (float)frequencia);
            if (ultimo_ms > 0) {
                const float delta_s = (agora_ms - ultimo_ms) / 1000.0f;
                distancia_m += (velocidade_cm_s / 100.0f) * delta_s;
            }
            ultimo_ms = agora_ms;
        }

        if (sinal_ativo && ultima_atualizacao > 0 && (agora_ms - ultima_atualizacao) > TEMPO_IDLE_MS) {
            portENTER_CRITICAL(&s_spinlock_pulso);
            s_sinal_ativo = false;
            s_tempo_sinal_ms += agora_ms - s_inicio_sinal_ms;
            tempo_ms = s_tempo_sinal_ms;
            portEXIT_CRITICAL(&s_spinlock_pulso);
        } else if (!sinal_ativo && ultima_atualizacao > 0 && (agora_ms - ultima_atualizacao) > TEMPO_RESET_MS) {
            portENTER_CRITICAL(&s_spinlock_pulso);
            s_periodo_us = 0;
            s_total_furos = 0;
            s_ultimo_pulso_us = 0;
            s_ultimo_interrupt_us = 0;
            s_ultima_atualizacao_ms = 0;
            s_sinal_ativo = false;
            s_inicio_sinal_ms = 0;
            s_tempo_sinal_ms = 0;
            portEXIT_CRITICAL(&s_spinlock_pulso);
            distancia_m = 0;
            ultimo_ms = agora_ms;
            continue;
        }

        if (s_callback) {
            dados_medidos_t medicao = {
                .frequencia_hz = frequencia,
                .rpm = rpm,
                .velocidade_cm_s = velocidade_cm_s,
                .distancia_m = distancia_m,
                .furos = furos,
                .tempo_sinal_ms = tempo_ms,
            };
            s_callback(&medicao);
        }
    }
}
