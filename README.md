# ContadorDeFuros – ESP32-S3-Touch-LCD-4.3B

Projeto embarcado para medir PPM do motor, frequência e velocidade de furação em máquinas de tatuagem/dermógrafos, agora totalmente baseado em ESP-IDF + LVGL para a placa *ESP32-S3-Touch-LCD-4.3B* (módulo ESP32-S3-WROOM-1-N16R8, flash 16 MB + PSRAM octal 8 MB). Esta etapa prepara todo o ambiente para que você possa focar na lógica da aplicação (contagem de furos), UI interativa e integração com os sensores reais.

## Visão geral do que já foi feito

- Download do **ESP-IDF 6.1** diretamente no repositório (`./.esp-idf`) e instalação completa do toolchain (`./.esp-idf/install.sh esp32s3`).
- Ajuste do `CMakeLists.txt` para usar a árvore completa do ESP-IDF (sem `MINIMAL_BUILD`), permitindo habilitar PSRAM, LVGL e os drivers de LCD/touch.
- Criação do `sdkconfig.defaults` com os parâmetros críticos da placa:
  - `CONFIG_IDF_TARGET="esp32s3"`
  - Flash QIO, 16 MB @ 80 MHz
  - PSRAM octal habilitada, 80 MHz, fetch de instruções + rodata em PSRAM
- Inclusão automática do componente `espressif/touch_element` via `main/idf_component.yml` e materialização do pacote em `managed_components/espressif__touch_element`.
- `idf.py build` concluído com sucesso e artefatos em `build/`.
- O `main/main.c` agora traz uma demo LVGL que inicializa o painel RGB, configura o GT911 e exibe um contador interativo de toques.
- Interface principal atualizada com:
  - **Indicador de RPM** arco 0–13,5k com escala multicolor (verde → vermelho vibrante).
  - **Tela de velocidade** com barra de *Boost* (percentual calculado dinamicamente a partir de curso × frequência) e rodapé informando a frequência instantânea.
  - Cálculo correto da velocidade linear `curso_cm × frequência`, usado tanto nos cards quanto na barra.
- Justificativa para uso do ESP-IDF “puro”: precisamos de controle total da PSRAM octal, drivers RGB/touch oficiais, LVGL otimizado e FreeRTOS completo. Ambientes Arduino/PlatformIO podem ser usados para projetos simples, mas neste caso limitariam o acesso aos recursos avançados e exigiriam muito esforço para portar os drivers.

> **Observação**: O ESP-IDF clonado ocupa alguns GB. Como boa prática, mantenha `.esp-idf/`, `build/`, `.idf-tmp/`, `.idf-component-cache/` e `managed_components/` fora de commits (já existem regras em `.gitignore`).

## Pré-requisitos

1. Ubuntu 22.04/24.04 ou Windows/macOS com Python ≥ 3.8 (mantive Python 3.13 do instalador do IDF).
2. Cabos USB-C com capacidade de alimentação ≥ 1 A (a tela chega a ~550 mA).
3. Permissão para acessar porta serial (`/dev/ttyACM*` ou `COMx`).

## Estrutura do repositório

```
ContadorDeFuros/
├── .esp-idf/                # Cópia local do ESP-IDF 6.1 (git clone --depth=1)
├── .idf-component-cache/    # Cache local do component manager
├── .idf-tmp/                # TMPDIR local (evita permissões em /tmp)
├── build/                   # Saída de compilação (gerado pelo idf.py)
├── main/
│   ├── CMakeLists.txt
│   ├── main.c               # Aplicação LVGL do contador
│   ├── app_types.h
│   ├── armazenamento.c/.h   # Persistência de curso (NVS)
│   ├── metricas.c/.h        # Cálculo de frequência/RPM/etc.
│   └── interface_usuario.c/.h
├── managed_components/
│   └── espressif__touch_element/
├── sdkconfig                # Gerado a partir dos defaults
├── sdkconfig.defaults       # Define flash/PSRAM da placa
└── README.md
```

## Como reproduzir o ambiente

1. **Clonar o ESP-IDF dentro do projeto** (já feito, mas o comando é):

   ```bash
   git clone --depth 1 https://github.com/espressif/esp-idf.git ./.esp-idf
   ```

2. **Instalar as ferramentas**:

   ```bash
   cd ./.esp-idf
   ./install.sh esp32s3
   cd ..
   ```

3. **Criar diretórios auxiliares** (caso ainda não existam):

   ```bash
   mkdir -p .idf-tmp .idf-component-cache
   ```

4. **Ativar o ambiente em cada nova sessão**:

   ```bash
   export TMPDIR=$PWD/.idf-tmp
   . ./.esp-idf/export.sh
   export IDF_COMPONENT_CACHE_PATH=$PWD/.idf-component-cache
   idf.py --version
   ```

   > Se estiver usando o VS Code + extensão “Espressif IDF”, basta apontar para `./.esp-idf` e configurar as mesmas variáveis em *Open ESP-IDF Terminal*.

## Build, flash e monitor

Com o ambiente ativado:

```bash
# 1) Garantir o alvo correto (gera sdkconfig a partir dos defaults)
idf.py set-target esp32s3

# 2) Compilar
idf.py build

# 3) Gravar e abrir o monitor (ajuste a porta)
idf.py -p /dev/ttyACM0 flash monitor
```

Os artefatos principais ficam em `build/ContadorDeFuros.bin`, `build/bootloader/bootloader.bin` e `build/partition_table/partition-table.bin`.

## Aplicação LVGL

- Configura `esp_lcd_rgb_panel`, integra `esp_lvgl_port` e registra o touch GT911. A UI traz cards (frequência, RPM, velocidade, distância, curso, total de furos), modos de expansão por toque, gráficos circulares/oscíloscópio e animações com tela de inicialização.
- Pinagem mapeada nas macros de `main/main.c` (`LCD_PIN_*`, `s_lcd_data_pins[]`, `TOUCH_*`). Ajuste se sua revisão usar outros sinais.
- `LCD_RGB_TIMING()` usa ~18 MHz / 35 Hz como base; ajuste conforme estabilidade da tela.
- `LCD_DRAW_BUFFER_HEIGHT` e `LCD_BOUNCE_BUFFER_LINES` equilibram desempenho x uso de PSRAM.
- O backlight pode ser controlado via `LCD_PIN_BACKLIGHT` caso conectado.

## Configurações importantes já embutidas

| Item                       | Configuração atual                         | Origem                |
| ------------------------- | ------------------------------------------- | --------------------- |
| Alvo                      | `CONFIG_IDF_TARGET="esp32s3"`               | `sdkconfig.defaults`  |
| Flash                     | QIO, 16 MB, 80 MHz                          | `sdkconfig.defaults`  |
| PSRAM                     | Octal 8 MB @ 80 MHz + fetch/rodata em PSRAM | `sdkconfig.defaults`  |
| Componentes externos      | `espressif/esp_lvgl_port`, `espressif/esp_lcd_touch_gt911` | `main/idf_component.yml` |
| Copy local do ESP-IDF     | `./.esp-idf`                                | estrutura do repo     |

Caso precise alterar algo, execute `idf.py menuconfig` (salva diretamente em `sdkconfig`). Se quiser recomeçar do zero, remova `sdkconfig` e rode `idf.py set-target esp32s3` para regenerar a partir dos defaults.

## Próximos passos recomendados

1. **Personalizar a UI para o “Contador de Furos”**  
   Aproveite o esqueleto atual (contador + label) e insira widgets LVGL (botões, gráficos, telas múltiplas) de acordo com o processo real de contagem.
2. **Persistir dados / integração com sensores reais**  
   Conecte os sensores físicos de contagem, trate debounces em tasks separadas e atualize a interface protegendo as chamadas com `lvgl_port_lock()`.
3. **Refinar o hardware**  
   - Ajuste o `pclk_hz` ou os porches caso teste com displays diferentes.  
   - Caso queira controlar brilho, ligue o `LCD_PIN_BACKLIGHT` ao enable do conversor e faça PWM com LEDC.  
   - Para calibração do touch, use `esp_lcd_touch_set_swap_xy`/`mirror_x`/`mirror_y` conforme necessário.

## Dúvidas comuns

- **Preciso manter o ESP-IDF dentro do repo?**  
  Não é obrigatório; se preferir, instale em outro caminho (ex.: `~/esp-idf`) e ajuste `IDF_PATH`. A versão local apenas garante reprodutibilidade imediata.

- **Por que `TMPDIR` personalizado?**  
  Em ambientes com restrição de escrita em `/tmp`, o export do IDF falha. Setar `TMPDIR=$PWD/.idf-tmp` evita esse problema. Em PCs “normais” isso não é necessário.

- **Onde está o log da compilação?**  
  Cada execução do `idf.py` gera `build/log/idf_py_{stdout,stderr}_output_*.log`. Útil para depuração.

Com isso você tem o toolchain instalado, dependências resolvidas e o projeto compilando para a ESP32-S3-Touch-LCD-4.3B. O próximo passo é evoluir o firmware para acionar a tela e implementar a lógica da aplicação. Bons testes! 🚀
