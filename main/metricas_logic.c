#include "metricas_logic.h"

float metricas_calcular_velocidade(float curso_cm, uint32_t frequencia_hz)
{
    if (curso_cm <= 0.0f || frequencia_hz == 0) {
        return 0.0f;
    }
    return curso_cm * (float)frequencia_hz;
}

float metricas_incrementar_distancia(float distancia_m, uint32_t velocidade_cm_s, float delta_segundos)
{
    if (velocidade_cm_s == 0 || delta_segundos <= 0.0f) {
        return distancia_m;
    }
    return distancia_m + (velocidade_cm_s / 100.0f) * delta_segundos;
}

