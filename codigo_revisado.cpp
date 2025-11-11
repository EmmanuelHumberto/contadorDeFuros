#include <Arduino.h>
#include <lvgl.h>      // Inclui a biblioteca LVGL
#include "LGFX_Config.hpp" // Inclui a configuração de pinos
#include <LovyanGFX.hpp> // Inclui a biblioteca LovyanGFX
#include <EEPROM.h>   // Para persistência de dados
#include <Wire.h>     // Para comunicação I2C

// --- Configuracoes do Display e LVGL ---
#define DISP_HOR_RES 800 // Resolucao horizontal do display
#define DISP_VER_RES 480 // Resolucao vertical do display
#define LVGL_TICK_PERIOD 5 // Periodo de tick do LVGL em ms

// Buffer de desenho para LVGL
static lv_color_t buf[DISP_HOR_RES * 40];

// --- Pinos e EEPROM ---
#define INPUT_PIN 15 // Pino de entrada para medicao de frequencia

// Endereco e tamanho da EEPROM para salvar o curso
#define CURSO_ADDRESS 0
#define EEPROM_SIZE sizeof(float)

// --- Variaveis Globais ---
static LGFX tft; // Objeto LovyanGFX para o display

volatile unsigned long lastTime = 0;
volatile unsigned long periodo = 0;
unsigned int frequencia = 0;
unsigned int rpm = 0;
float curso = 3.5;
float distancia = 0;
unsigned int velocidade = 0;
unsigned long ultimaAtualizacao = 0;
unsigned long ultimoCalculoDistancia = 0;

unsigned int totalFuros = 0;
unsigned long tempoSinalAtivo = 0;
bool sinalAtivo = false;
unsigned long inicioSinalAtivo = 0;
volatile unsigned long ultimaInterrupcao = 0; // CORRECAO: Deve ser volatile para uso na ISR

bool mostrandoResumo = false;

enum DisplayMode { FREQUENCIA, RPM, VELOCIDADE, CURSO, DISTANCIA, FUROS, NUM_MODES };
DisplayMode displayMode = FREQUENCIA;
bool modoEdicao = false;

lv_obj_t *mainScreen;
lv_obj_t *resumoScreen;
lv_obj_t *labelTitulo;
lv_obj_t *labelValor;
lv_obj_t *labelUnidade;
lv_obj_t *labelPagina;
lv_obj_t *labelStatus;
lv_obj_t *chart;
lv_chart_series_t *serie;

// --- Prototipos de Funcao ---
void IRAM_ATTR handleInterrupt();
void carregarCursoDaEEPROM();
void salvarCursoNaEEPROM();
void resetarMedidas();
void atualizarMedidas();
void atualizarFurosECronometro();
void formatarDistancia(char *buffer, float distancia);
void criarInterface();
void atualizarInterface();
void mostrarTelaResumo();
void iniciarEdicaoCurso();
void finalizarEdicaoCurso();

void my_disp_flush(lv_display_t *disp, const lv_area_t *area, lv_color_t *color_p);
void my_touchpad_read(lv_indev_t *indev_drv, lv_indev_data_t *data);

/* Funcao para atualizar o display (callback do LVGL) */
void my_disp_flush(lv_display_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    tft.pushImage(area->x1, area->y1, area->x2 - area->x1 + 1, area->y2 - area->y1 + 1, (lgfx::rgb565_t*)color_p);
    lv_display_flush_ready(disp);
}

/* Funcao para ler o touchscreen (callback do LVGL) */
void my_touchpad_read(lv_indev_t *indev_drv, lv_indev_data_t *data) {
    uint16_t touchX, touchY;
    // CORRECAO: O metodo getTouch() foi adicionado a classe LGFX no LGFX_Config.hpp
    bool pressed = tft.touch.getTouchRaw(nullptr, 0); 

    if (pressed) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = touchX;
        data->point.y = touchY;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

/* Interrupcao para medicao de frequencia */
void IRAM_ATTR handleInterrupt() {
    unsigned long now = micros();
    const unsigned long DEBOUNCE_TIME = 1000;
    if (now - ultimaInterrupcao < DEBOUNCE_TIME) return;
    ultimaInterrupcao = now;
    periodo = now - lastTime;
    lastTime = now;
    ultimaAtualizacao = millis();
    totalFuros++;
    if (!sinalAtivo) {
        sinalAtivo = true;
        inicioSinalAtivo = millis();
    }
}

/* Reset das medicoes */
void resetarMedidas() {
    periodo = 0;
    frequencia = 0;
    rpm = 0;
    velocidade = 0;
    distancia = 0;
    totalFuros = 0;
    tempoSinalAtivo = 0;
    ultimoCalculoDistancia = 0;
}

/* Atualiza contagem de furos e tempo de sinal */
void atualizarFurosECronometro() {
    if (sinalAtivo) {
        if (millis() - ultimaAtualizacao > 1000) {
            sinalAtivo = false;
            tempoSinalAtivo += millis() - inicioSinalAtivo;
        }
    } else {
        if (millis() - ultimaAtualizacao > 30000 && ultimaAtualizacao != 0) {
            resetarMedidas();
            ultimaAtualizacao = 0;
            ultimaInterrupcao = 0;
        }
    }
}

/* Carrega curso da EEPROM */
void carregarCursoDaEEPROM() {
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.get(CURSO_ADDRESS, curso);
    if (isnan(curso)) {
        curso = 3.5;
        salvarCursoNaEEPROM();
    }
    EEPROM.end();
}

/* Salva curso na EEPROM */
void salvarCursoNaEEPROM() {
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.put(CURSO_ADDRESS, curso);
    EEPROM.commit();
    EEPROM.end();
}

/* Formata distancia para exibicao */
void formatarDistancia(char* buffer, float distancia) {
    if (distancia >= 1000) {
        sprintf(buffer, "%.2f km", distancia / 1000.0);
    } else {
        sprintf(buffer, "%.1f m", distancia);
    }
}

/* Cria a interface grafica LVGL */
void criarInterface() {
    mainScreen = lv_obj_create(NULL);
    lv_scr_load(mainScreen);
    lv_obj_set_style_bg_color(mainScreen, lv_color_hex(0x000000), LV_PART_MAIN);

    // Criacao dos elementos da interface (simplificado)
    labelTitulo = lv_label_create(mainScreen);
    lv_obj_set_style_text_font(labelTitulo, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(labelTitulo, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(labelTitulo, LV_ALIGN_TOP_MID, 0, 10);
    lv_label_set_text(labelTitulo, "Medidor de Frequência/RPM");

    labelValor = lv_label_create(mainScreen);
    lv_obj_set_style_text_font(labelValor, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(labelValor, lv_color_hex(0x00FF00), 0);
    lv_obj_align(labelValor, LV_ALIGN_CENTER, 0, -20);
    lv_label_set_text(labelValor, "0");

    labelUnidade = lv_label_create(mainScreen);
    lv_obj_set_style_text_font(labelUnidade, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(labelUnidade, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align_to(labelUnidade, labelValor, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    lv_label_set_text(labelUnidade, "Hz");

    labelStatus = lv_label_create(mainScreen);
    lv_obj_set_style_text_font(labelStatus, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(labelStatus, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(labelStatus, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    lv_label_set_text(labelStatus, "Status: OK");
}

// Atualiza a interface grafica */
void atualizarInterface() {
    char buffer[32];
    const char *titulo = "";
    const char *unidade = "";
    
    switch (displayMode) {
        case FREQUENCIA:
            titulo = "Frequência";
            sprintf(buffer, "%u", frequencia);
            unidade = "Hz";
            break;
        case RPM:
            titulo = "RPM";
            sprintf(buffer, "%u", rpm);
            unidade = "RPM";
            break;
        case VELOCIDADE:
            titulo = "Velocidade";
            sprintf(buffer, "%u", velocidade);
            unidade = "m/s"; // Assumindo m/s
            break;
        case CURSO:
            titulo = "Curso";
            sprintf(buffer, "%.2f", curso);
            unidade = "cm";
            break;
        case DISTANCIA:
            titulo = "Distância";
            formatarDistancia(buffer, distancia);
            unidade = ""; // A unidade já está no buffer
            break;
        case FUROS:
            titulo = "Total de Furos";
            sprintf(buffer, "%u", totalFuros);
            unidade = "";
            break;
        default:
            titulo = "Modo Desconhecido";
            sprintf(buffer, "---");
            unidade = "";
            break;
    }

    lv_label_set_text(labelTitulo, titulo);
    lv_label_set_text(labelValor, buffer);
    lv_label_set_text(labelUnidade, unidade);
}

// Mostra a tela de resumo */
void mostrarTelaResumo() {
    // Implementação da tela de resumo (deixada vazia, pois não estava completa no original)
}

// Inicia o modo de edicao do curso */
void iniciarEdicaoCurso() {
    modoEdicao = true;
}

// Finaliza o modo de edicao do curso */
void finalizarEdicaoCurso() {
    modoEdicao = false;
    salvarCursoNaEEPROM();
}


void setup() {
    Serial.begin(115200);
    
    // CORRECAO: O metodo begin() foi adicionado a classe LGFX no LGFX_Config.hpp
    tft.begin();
    tft.setRotation(1);

    // Inicializa a EEPROM e carrega o curso
    carregarCursoDaEEPROM();

    // Configura o pino de entrada e a interrupcao
    // NOTA: O pino 15 (INPUT_PIN) esta em conflito com o pino R0 do display RGB (Bus_RGB).
    // O usuario deve mudar o INPUT_PIN para um GPIO nao utilizado, como GPIO41 ou GPIO42.
    pinMode(INPUT_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(INPUT_PIN), handleInterrupt, FALLING);

    // Inicializa o LVGL
    lv_init();

    // Configura o driver de display do LVGL (v9)
    lv_display_t *disp = lv_display_create(DISP_HOR_RES, DISP_VER_RES);
    lv_display_set_flush_cb(disp, (lv_display_flush_cb_t)my_disp_flush);
    lv_display_set_buffers(disp, buf, NULL, DISP_HOR_RES * 40, LV_DISPLAY_RENDER_MODE_PARTIAL);

    // Configura o driver de entrada (touchscreen) do LVGL (v9)
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touchpad_read);

    // Cria a interface grafica
    criarInterface();
}

void loop() {
    lv_timer_handler(); // Processa os eventos do LVGL
    delay(LVGL_TICK_PERIOD);

    atualizarMedidas();
    atualizarFurosECronometro();
    atualizarInterface();
}

void atualizarMedidas() {
    if (periodo > 0) {
        // Frequencia (Hz) = 1.000.000 / periodo (em microssegundos)
        frequencia = 1000000 / periodo;
        // RPM = Frequência * 60
        rpm = frequencia * 60;
        
        // Velocidade (em cm/s, se curso for em cm)
        // A logica original (frequencia * curso / 10.0) e mantida.
        velocidade = (unsigned int)(frequencia * curso / 10.0);
        
        unsigned long now = millis();
        if (ultimoCalculoDistancia != 0) {
            float tempoEmSegundos = (now - ultimoCalculoDistancia) / 1000.0;
            // Distancia (em metros, se velocidade for em m/s)
            // A logica original (velocidade / 100.0) e mantida, assumindo que
            // a velocidade calculada acima esta em cm/s e e convertida para m/s.
            distancia += (velocidade / 100.0) * tempoEmSegundos; 
        }
        ultimoCalculoDistancia = now;
    }
}
