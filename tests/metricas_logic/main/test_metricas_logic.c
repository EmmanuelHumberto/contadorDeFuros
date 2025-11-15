#include "unity.h"
#include "metricas_logic.h"

TEST_CASE("velocidade zera quando frequencia eh zero", "[metricas]")
{
    float velocidade = metricas_calcular_velocidade(0.5f, 0);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, velocidade);
}

TEST_CASE("velocidade considera curso em cm", "[metricas]")
{
    float velocidade = metricas_calcular_velocidade(0.5f, 94); // 5 mm curso, 94 Hz
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 47.0f, velocidade);
}

TEST_CASE("distancia para de crescer sem velocidade", "[metricas]")
{
    float distancia = metricas_incrementar_distancia(1.0f, 0, 0.5f);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, distancia);
}

TEST_CASE("distancia acumula proporcional ao tempo", "[metricas]")
{
    float distancia = metricas_incrementar_distancia(0.0f, 100, 2.0f); // 100 cm/s por 2 s = 2 m
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, distancia);
}

