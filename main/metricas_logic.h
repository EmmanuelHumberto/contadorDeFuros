#pragma once

#include <stdint.h>

/**
 * Calcula a velocidade linear (cm/s) a partir do curso em centímetros e da frequência em Hz.
 */
float metricas_calcular_velocidade(float curso_cm, uint32_t frequencia_hz);

/**
 * Incrementa a distância percorrida (em metros) considerando a velocidade em cm/s e o delta de tempo.
 */
float metricas_incrementar_distancia(float distancia_m, uint32_t velocidade_cm_s, float delta_segundos);

