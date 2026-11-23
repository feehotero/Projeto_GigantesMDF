# Gigantes de MDF

Robô de combate de MDF desenvolvido para a disciplina **Programação de Hardware 2026**. Ele é controlado à distância por um **controle de PS4 (DualShock 4) via Bluetooth** ou, como alternativa sem biblioteca externa, por uma **página web pelo Wi-Fi do ESP32**. Ele usa um **laser** para atingir o adversário e um **sensor LDR** para detectar quando é atingido. As vidas são mostradas em LEDs.

> **Equipe:** _Antonio Luis Villar Coelho, Arthur Contato Medeiros, Enzo Calvo Ellner e Felipe Lourenço Otero_

## Sumário

1. [Visão geral](#visão-geral)
2. [Estrutura do repositório](#estrutura-do-repositório)
3. [Hardware](#hardware)
4. [Como o software funciona](#como-o-software-funciona)
5. [Como parear o controle](#como-parear-o-controle)
6. [Documentação do software (Doxygen)](#documentação-do-software-doxygen)
7. [Dependências externas](#dependências-externas)

## Visão geral

O robô tem duas placas que conversam por uma ligação serial:

Há duas versões do ESP32, e **só uma é usada por vez**. O ATmega328P é o mesmo nas duas.

```mermaid
flowchart LR
    C[Controle de PS4] -- Bluetooth --> E[ESP32]
    P[Celular com a página] -. "Wi-Fi (versão alternativa)" .-> E
    E -- "UART 9600 8N1<br/>F B L R S" --> A[ATmega328P]
    A --> M[Motores<br/>PWM + ponte H]
    A --> L[Laser]
    LDR[Sensor LDR] --> A
    A --> V[LEDs de vida]
    B[Botão de reinício] --> A
```

- **ESP32:** recebe o comando e o converte em 1 caractere para o ATmega. Na versão com controle (`esp32/controle_ps4/my_platform.c`) o comando vem do controle de PS4 por Bluetooth. Na versão com site (`esp32/site_wifi/main.c`) ele vem de uma página web pelo Wi-Fi.
- **ATmega328P** (`atmega/main.c`): executa os comandos, aciona os motores, pisca o laser, lê o LDR e controla as vidas.

### Regras do robô

- O laser pisca **1 s ligado e 1 s desligado**.
- Cada robô começa com **3 vidas**, mostradas em 3 LEDs.
- Quando o LDR detecta o laser adversário, o robô perde uma vida (apaga um LED), **gira 180°** e fica **5 s parado**, com motores e receptor desativados.
- No terceiro acerto (sem vidas), o robô **não gira**: fica **eliminado**, com o laser apagado, até o botão de reinício ser apertado.
- Ao ser ligado, o robô começa em **modo manual**: aceita comandos de movimento, mas o laser fica apagado e o LDR é ignorado. Ao apertar o botão de reinício, o jogo começa.

## Estrutura do repositório

```
.
├── atmega/main.c        # Firmware do ATmega328P (avr-gcc)
├── esp32/
│   ├── controle_ps4/my_platform.c  # ESP32 com controle de PS4 (ESP-IDF + Bluepad32)
│   └── site_wifi/main.c            # ESP32 com Wi-Fi e site de controle (só ESP-IDF)
├── Doxyfile                        # Configuração da documentação do software
├── .gitignore
└── README.md
```

Use **uma** das duas versões do ESP32. A do site existe para o caso de a biblioteca Bluepad32 não poder ser usada.

## Hardware

### Componentes

- ESP32 comum (por exemplo ESP32-WROOM-32D). Os modelos C3, S2 e S3 **não** funcionam com controle de PS4 (a versão do site funciona em qualquer modelo com Wi-Fi).
- ATmega328P a 16 MHz (Arduino Uno/Nano ou equivalente).
- Ponte H e dois motores DC.
- Laser, sensor LDR com divisor de tensão, 3 LEDs com resistor e 1 botão.
- Controle de PS4 (DualShock 4), se for usada a versão com controle.

### Ligações

| Função | Pino do ATmega328P | Observação |
|---|---|---|
| Recepção serial (do ESP32) | PD0 (D0, RX) | Ligado ao GPIO17 do ESP32 |
| Transmissão serial (log de depuração) | PD1 (D1, TX) | Para o monitor USB do PC |
| PWM do motor esquerdo | PD6 (D6, OC0A) | |
| Direção do motor esquerdo | PD7 (D7) e PB0 (D8) | |
| PWM do motor direito | PD5 (D5, OC0B) | |
| Direção do motor direito | PD4 (D4) e PB4 (D12) | |
| Laser | PD3 (D3) | |
| Sensor LDR | PC3 (A3, ADC3) | Com divisor de tensão |
| LEDs de vida | PC0, PC1 e PC2 (A0, A1 e A2) | Cada LED com resistor |
| Botão de reinício | PD2 (D2, INT0) | Para o GND, com pull-up interno |

| Função | Pino do ESP32 |
|---|---|
| TX da UART2 (para o ATmega) | GPIO17 |

O **GND** do ESP32, do ATmega e da ponte H deve ser comum. Se o Arduino estiver ligado ao PC pelo USB, desconecte o fio do ESP32 do RX durante a gravação, porque o RX é compartilhado com o USB.

## Como o software funciona

### Protocolo serial (ESP32 → ATmega328P)

Uma UART a 9600 bps, 8N1, com **1 caractere por comando**:

| Caractere | Ação |
|---|---|
| `F` | Frente |
| `B` | Ré |
| `L` | Giro para a esquerda |
| `R` | Giro para a direita |
| `S` | Parar |

O ATmega também **envia** pela serial (pino TX) um log de depuração a cada 200 ms, no formato `LDR: 512 | Estado: 0 | Vidas: 3 | Cmd: S`. O número do estado é: 0 = `ST_CONTROLE`, 1 = `ST_GIRO`, 2 = `ST_PAUSA`, 3 = `ST_ELIMINADO`, 4 = `ST_MANUAL`. O log pode ser visto no Serial Monitor (9600 bps) e o ESP32 não o utiliza.

### ESP32 com controle (`esp32/controle_ps4/my_platform.c`)

- O comando é lido do **D-pad** (prioridade) ou do **analógico esquerdo**.
- Enquanto algo estiver apertado, o comando é **reenviado a cada 100 ms**. Ao soltar, o ESP32 envia `S`. Se o controle desconectar, o robô para.

### ESP32 com site (`esp32/site_wifi/main.c`)

- O ESP32 cria uma rede Wi-Fi própria (modo Access Point) chamada `GigantesMDF`, com a senha `gigante123`.
- O celular conecta nessa rede e abre `http://192.168.4.1`, que mostra uma página com 4 setas.
- Enquanto uma seta está apertada, a página chama `/cmd?c=X` a cada 100 ms, e o ESP32 repassa o caractere ao ATmega. Ao soltar, a página envia `S`.
- O ESP32 só aceita os caracteres `F`, `B`, `L`, `R` e `S`.

### Firmware do ATmega328P (`atmega/main.c`)

| Periférico | Uso |
|---|---|
| Timer0 | PWM dos motores (OC0A e OC0B) |
| Timer2 | Base de tempo de 1 ms (modo CTC, interrupção por comparação); dela saem todos os tempos do programa, inclusive o do laser (1 s ligado e 1 s desligado) |
| ADC (canal 3) | Leitura do LDR a cada 1 ms |
| INT0 | Botão de reinício (borda de descida) |
| USART0 | Recebe os comandos do ESP32 e envia o log de depuração |

**Parada de segurança:** se o robô estiver em movimento e passar 300 ms sem receber comando, os motores são parados. Assim, um `S` perdido não deixa o robô sem controle.

**Máquina de estados:**

```mermaid
stateDiagram-v2
    [*] --> ST_MANUAL
    ST_MANUAL --> ST_CONTROLE: botão de reinício
    ST_CONTROLE --> ST_GIRO: atingido e ainda com vidas
    ST_CONTROLE --> ST_ELIMINADO: atingido sem vidas
    ST_GIRO --> ST_PAUSA: após 600 ms
    ST_PAUSA --> ST_CONTROLE: após 5 s
    ST_ELIMINADO --> ST_CONTROLE: botão de reinício
    ST_GIRO --> ST_CONTROLE: botão de reinício
    ST_PAUSA --> ST_CONTROLE: botão de reinício
```

| Estado | O que acontece |
|---|---|
| `ST_MANUAL` | Estado inicial: aceita comandos de movimento, sem laser e sem leitura do LDR |
| `ST_CONTROLE` | Jogo em andamento: aceita comandos, o laser pisca e o LDR é monitorado |
| `ST_GIRO` | Atingido: gira 180° (600 ms) e ignora comandos e o LDR |
| `ST_PAUSA` | Fica 5 s parado, com motores e receptor desativados |
| `ST_ELIMINADO` | Sem vidas: motores parados, laser desligado, aguarda o botão |

## Como parear o controle

Vale só para a versão com controle. Com o ESP32 ligado e rodando, segure **SHARE + PS** no controle de PS4 até a luz piscar rápido. Em alguns segundos ele conecta sozinho. Nas próximas vezes, basta apertar o botão **PS**.


## Documentação do software (Doxygen)

Todo o código-fonte está comentado no padrão **Doxygen** (`@file`, `@brief`, `@param`, `@return`). Para gerar a documentação em HTML:

```bash
doxygen Doxyfile
```

Depois abra `docs/html/index.html` no navegador. A saída em `docs/html` não é versionada (está no `.gitignore`); gere-a localmente. Para os gráficos de chamadas, o Graphviz (`dot`) precisa estar instalado.

## Dependências externas

- **[Bluepad32](https://github.com/ricardoquesada/bluepad32):** biblioteca usada **apenas** na versão com controle, para ler o controle Bluetooth. O arquivo `esp32/controle_ps4/my_platform.c` é o código do grupo que roda sobre ela. A versão `esp32/site_wifi/main.c` não usa essa biblioteca.
- **ESP-IDF** (Espressif) e **avr-libc** (`avr-gcc`): SDKs e bibliotecas padrão de cada placa.
