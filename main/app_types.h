#pragma once

#include <stdint.h>
#include <stdbool.h>

#define CURSO_MIN_MM 1.0f
#define CURSO_MAX_MM 5.0f
#define CURSO_MIN_CM (CURSO_MIN_MM / 10.0f)
#define CURSO_MAX_CM (CURSO_MAX_MM / 10.0f)

typedef struct {
    uint32_t frequencia_hz;
    uint32_t rpm;
    uint32_t velocidade_cm_s;
    float distancia_m;
    uint32_t furos;
    uint64_t tempo_sinal_ms;
} dados_medidos_t;

typedef struct {
    float curso_cm;
} configuracao_curso_t;
