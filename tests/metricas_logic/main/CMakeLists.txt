idf_component_register(
    SRCS
        "test_metricas_logic.c"
        "test_runner.c"
        "../../../main/metricas_logic.c"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS "../../../main"
    REQUIRES unity
)
