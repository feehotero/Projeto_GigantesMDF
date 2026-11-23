/**
 * @file main.c
 * @brief Gigantes de MDF - Programação de Hardware 2026 (ATmega328P)
 *
 * Firmware do robô de combate "Gigante de MDF". O robô recebe comandos de
 * movimento por UART, dispara um laser que pisca 1 s ligado e 1 s desligado e
 * usa um sensor LDR para detectar o laser do adversário.
 *
 * Periféricos utilizados:
 *  - Timer0: PWM dos motores (OC0A = PD6, OC0B = PD5)
 *  - Timer2: base de tempo de 1 ms (interrupção por comparação)
 *  - ADC3 (PC3): receptor LDR
 *  - INT0 (PD2): botão de reinício das vidas
 *  - USART0: recebe comandos (D0) e envia um log de depuração (D1)
 *
 * Protocolo UART (1 caractere por comando, apenas recepção):
 *   'F' frente | 'B' ré | 'L' giro à esquerda | 'R' giro à direita | 'S' parar
 *
 * Regras do jogo:
 *  - Ao detectar o laser adversário o robô perde uma vida (LED apagado),
 *    desativa motores e receptor, gira 180 graus no próprio eixo e só volta a
 *    jogar depois da pausa.
 *  - Com as 3 vidas perdidas o robô fica eliminado até o botão de reinício
 *    ser apertado.
 *
 * Parada de segurança: se o robô estiver em movimento e passar
 * @ref TEMPO_SEM_COMANDO_MS sem receber comando, os motores são parados.
 */

#define F_CPU 16000000UL /**< Clock do ATmega328P (Hz) */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdint.h>
#include <stdio.h>

/* ---------- Parâmetros e pinos ---------- */

#define MODO_PWM (_BV(WGM01) | _BV(WGM00))   /**< Timer0 em Fast PWM de 8 bits           */
#define PWM_MOTORES 200                      /**< Razão cíclica dos motores (0 a 255)    */
#define TEMPO_SEM_COMANDO_MS 300UL           /**< Tempo sem comando até parar (ms)       */
#define UBRR_VAL ((F_CPU / (16UL * 9600UL)) - 1UL) /**< Valor do UBRR para 9600 bps      */

#define BOTAO_BIT PD2                        /**< Botão de reinício (D2, INT0)           */
#define LASER_BIT PD3                        /**< Saída do laser (D3)                    */
#define LEDS_MASK (_BV(PC0) | _BV(PC1) | _BV(PC2)) /**< Máscara dos LEDs de vida (A0-A2) */

#define USAR_LASER 1                         /**< 1 = laser habilitado, 0 = desabilitado */
#define LASER_LIGADO_MS 1000UL               /**< Tempo do laser ligado (ms)             */
#define LASER_PERIODO_MS 2000UL              /**< Período do laser: ligado + desligado   */

#define LDR_LIMIAR 800u                      /**< Leitura do ADC (0-1023) que indica laser */
#define LDR_AMOSTRAS_OK 3u                   /**< Leituras seguidas acima do limiar      */
#define PRINT_LDR_MS 200UL                   /**< Intervalo entre os logs de depuração   */

#define TEMPO_GIRO_MS 600UL                  /**< Duração do giro de 180 graus (calibrar) */
#define TEMPO_PAUSA_MS 5000UL                /**< Tempo parado após o giro (ms)          */
#define DEBOUNCE_BTN_MS 200UL                /**< Tempo mínimo entre acionamentos do botão */

/** @brief Estados do robô */
typedef enum
{
    ST_CONTROLE = 0, /**< Jogo em andamento: aceita comandos e monitora o LDR  */
    ST_GIRO,         /**< Atingido: gira 180 graus no próprio eixo             */
    ST_PAUSA,        /**< Depois do giro: parado, motores e receptor desativados */
    ST_ELIMINADO,    /**< Sem vidas: aguarda o botão de reinício               */
    ST_MANUAL        /**< Estado inicial: só movimento manual, sem laser nem LDR */
} estado_t;

static volatile uint8_t ticks = 0;        /**< Contador de milissegundos (8 bits, incrementado no Timer2) */
static volatile uint8_t botao_evento = 0; /**< 1 = botão de reinício foi apertado (INT0)                  */

/**
 * @brief Interrupção de comparação do Timer2: base de tempo de 1 ms.
 *
 * Incrementa @ref ticks a cada 1 ms.
 */
ISR(TIMER2_COMPA_vect)
{
    ticks++;
}

/**
 * @brief Interrupção externa INT0 (PD2): botão de reinício das vidas.
 *
 * Apenas sinaliza o evento em @ref botao_evento; o tratamento (com debounce)
 * é feito no laço principal.
 */
ISR(INT0_vect)
{
    botao_evento = 1;
}

/**
 * @brief Liga ou desliga a direção dos dois motores e habilita o PWM.
 *
 * Os pinos de direção são PD7 e PB0 (motor esquerdo) e PD4 e PB4 (motor
 * direito). O PWM sai em OC0A (D6, esquerdo) e OC0B (D5, direito).
 *
 * @param esquerdo Motor esquerdo: 1 = frente, -1 = trás, 0 = parar.
 * @param direito  Motor direito: 1 = frente, -1 = trás, 0 = parar.
 */
static void motores(int8_t esquerdo, int8_t direito)
{
    uint8_t saidas_d = PORTD & (uint8_t)~(_BV(PD7) | _BV(PD4));
    uint8_t saidas_b = PORTB & (uint8_t)~(_BV(PB0) | _BV(PB4));

    if (esquerdo > 0) saidas_d |= _BV(PD7);  // D7
    if (esquerdo < 0) saidas_b |= _BV(PB0);  // D8
    if (direito > 0)  saidas_d |= _BV(PD4);  // D4
    if (direito < 0)  saidas_b |= _BV(PB4);  // D12

    PORTD = saidas_d;
    PORTB = saidas_b;

    // OC0A = D6, esquerdo. OC0B = D5, direito.
    TCCR0A = MODO_PWM |
             (esquerdo != 0 ? _BV(COM0A1) : 0) |
             (direito != 0 ? _BV(COM0B1) : 0);
}

/**
 * @brief Executa um comando de movimento recebido pela UART.
 *
 * @param comando Caractere recebido: 'F', 'B', 'L', 'R' ou 'S'.
 * @return 1 se o comando é válido e foi aplicado, 0 caso contrário.
 */
static uint8_t aplica_comando(uint8_t comando)
{
    switch (comando)
    {
        case 'F': motores( 1,  1); break;
        case 'B': motores(-1, -1); break;
        case 'L': motores(-1,  1); break;
        case 'R': motores( 1, -1); break;
        case 'S': motores( 0,  0); break;
        default: return 0;
    }

    return 1;
}

/**
 * @brief Atualiza os três LEDs que indicam as vidas.
 *
 * Cada vida acende um LED: A0 (1 vida), A1 (2 vidas) e A2 (3 vidas).
 *
 * @param vidas Número de vidas restantes (0 a 3).
 */
static void leds_atualiza(uint8_t vidas)
{
    uint8_t leds = 0;

    if (vidas >= 1) leds |= _BV(PC0);  // A0
    if (vidas >= 2) leds |= _BV(PC1);  // A1
    if (vidas >= 3) leds |= _BV(PC2);  // A2

    PORTC = (PORTC & (uint8_t)~LEDS_MASK) | leds;
}

/**
 * @brief Faz uma leitura do sensor LDR no canal ADC3.
 *
 * Inicia a conversão e aguarda o término.
 *
 * @return Valor lido, de 0 a 1023.
 */
static uint16_t ldr_le(void)
{
    ADCSRA |= _BV(ADSC);

    while (ADCSRA & _BV(ADSC))
    {
    }

    return ADC;
}

/**
 * @brief Programa principal.
 *
 * Configura os periféricos (Timer0, Timer2, UART, ADC, INT0 e GPIO) e executa
 * o laço que trata, nesta ordem: recepção de comandos, parada de segurança,
 * botão de reinício, leitura do LDR, máquina de estados, laser e log serial.
 *
 * @return Não retorna.
 */
int main(void)
{
    uint32_t agora = 0;            /* tempo atual em ms                       */
    uint32_t ultimo_comando = 0;   /* instante do último comando válido       */
    uint32_t t_btn = 0;            /* instante do último acionamento do botão */
    uint32_t t_estado = 0;         /* instante em que o estado atual começou  */
    uint32_t t_adc = 0;            /* instante da última leitura do LDR       */
    uint32_t t_print = 0;          /* instante do último log                  */
    uint32_t t_laser = 0;          /* início do ciclo atual do laser          */

    uint8_t tick_anterior = 0;     /* valor anterior de ticks                 */
    uint8_t movendo = 0;           /* 1 = último comando foi de movimento     */
    uint8_t vidas = 3;             /* vidas restantes                         */
    uint8_t cont_ldr = 0;          /* leituras seguidas acima do limiar       */
    uint8_t ldr_armado = 1;        /* 1 = pronto para contar um novo acerto   */
    uint16_t leitura_ldr = 0;      /* última leitura do LDR                   */
    char comando_atual = 'S';      /* último comando aplicado (para o log)    */

    estado_t estado = ST_MANUAL;   /* estado atual da máquina de estados      */
    char linha[56] = {0};          /* linha de log em transmissão             */
    uint8_t pos_tx = 0;            /* posição do próximo caractere a enviar   */

    // Timer0 e saídas dos motores, iguais ao teste que funcionou.
    DDRD |= _BV(PD6) | _BV(PD5) | _BV(PD7) | _BV(PD4);
    DDRB |= _BV(PB0) | _BV(PB4);
    PORTD &= (uint8_t)~(_BV(PD6) | _BV(PD5));

    motores(0, 0);
    TCCR0B = _BV(CS01) | _BV(CS00);
    OCR0A = PWM_MOTORES;
    OCR0B = PWM_MOTORES;

    // UART a 9600 baud, 8N1. D0 recebe e D1 envia ao monitor USB.
    UCSR0A = 0;
    UBRR0H = (uint8_t)(UBRR_VAL >> 8);
    UBRR0L = (uint8_t)UBRR_VAL;
    UCSR0C = _BV(UCSZ01) | _BV(UCSZ00);
    UCSR0B = _BV(RXEN0) | _BV(TXEN0);

    // Timer2 em CTC, com interrupção a cada 1 ms.
    TCCR2A = _BV(WGM21);
    OCR2A = 249;
    TIFR2 = _BV(OCF2A);
    TIMSK2 = _BV(OCIE2A);
    TCCR2B = _BV(CS22);

    // LEDs em A0, A1 e A2. LDR em A3 (ADC3).
    DDRC |= LEDS_MASK;
    DDRC &= (uint8_t)~_BV(PC3);
    PORTC &= (uint8_t)~_BV(PC3);
    ADMUX = _BV(REFS0) | _BV(MUX1) | _BV(MUX0);
    ADCSRA = _BV(ADEN) | _BV(ADPS2) | _BV(ADPS1) | _BV(ADPS0);
    DIDR0 = _BV(ADC3D);
    leds_atualiza(vidas);

    // Botão em D2 com pull-up interno.
    DDRD &= (uint8_t)~_BV(BOTAO_BIT);
    PORTD |= _BV(BOTAO_BIT);
    EICRA = _BV(ISC01);
    EIFR = _BV(INTF0);
    EIMSK = _BV(INT0);

    // Laser em D3, inicialmente desligado.
    PORTD &= (uint8_t)~_BV(LASER_BIT);
    DDRD |= _BV(LASER_BIT);

    sei();

    /*
     * Laço principal. Em cada volta, nesta ordem:
     *  1. atualiza o tempo (ms) a partir do Timer2;
     *  2. recebe e aplica comandos da UART;
     *  3. para os motores se ficar sem comando (segurança);
     *  4. trata o botão de reinício;
     *  5. lê o LDR e detecta acertos do laser adversário;
     *  6. avança a máquina de estados (giro, pausa);
     *  7. controla o laser (1 s ligado, 1 s desligado);
     *  8. envia o log de depuração pela UART.
     */
    for (;;)
    {
        /* 1. Tempo: soma ao total os milissegundos contados pelo Timer2 */
        uint8_t tick_atual = ticks;
        agora += (uint8_t)(tick_atual - tick_anterior);
        tick_anterior = tick_atual;

        /* 2. Comandos: só são aceitos sem erro de UART e nos estados
         *    ST_MANUAL e ST_CONTROLE; nos demais o byte é descartado */
        uint8_t status = UCSR0A;

        if (status & _BV(RXC0))
        {
            uint8_t comando = UDR0;

            if (!(status & (_BV(FE0) | _BV(DOR0) | _BV(UPE0))) &&
                (estado == ST_MANUAL || estado == ST_CONTROLE) &&
                aplica_comando(comando))
            {
                ultimo_comando = agora;
                movendo = (comando != 'S');
                comando_atual = (char)comando;
            }
        }

        /* 3. Parada de segurança: sem comando por TEMPO_SEM_COMANDO_MS,
         *    o robô para (evita ficar andando se um 'S' for perdido) */
        if (movendo && agora - ultimo_comando >= TEMPO_SEM_COMANDO_MS)
        {
            motores(0, 0);
            movendo = 0;
            comando_atual = 'S';
        }

        /* 4. Botão de reinício: com debounce, restaura as 3 vidas, reinicia
         *    o ciclo do laser e volta ao estado ST_CONTROLE */
        if (botao_evento)
        {
            botao_evento = 0;

            if (agora - t_btn >= DEBOUNCE_BTN_MS)
            {
                t_btn = agora;
                t_laser = agora;
                vidas = 3;
                cont_ldr = 0;
                ldr_armado = 1;
                movendo = 0;
                comando_atual = 'S';
                estado = ST_CONTROLE;

                motores(0, 0);
                leds_atualiza(vidas);
            }
        }

        /* 5. Receptor LDR: lê a cada 1 ms. Em ST_CONTROLE, após
         *    LDR_AMOSTRAS_OK leituras seguidas acima de LDR_LIMIAR conta um
         *    acerto: apaga um LED, desativa o receptor (ldr_armado = 0) e
         *    inicia o giro de 180 graus; sem vidas, o robô é eliminado.
         *    O receptor só rearma quando a leitura cai abaixo do limiar. */
        if (agora - t_adc >= 1UL)
        {
            t_adc = agora;
            leitura_ldr = ldr_le();

            if (estado == ST_CONTROLE)
            {
                if (leitura_ldr < LDR_LIMIAR)
                {
                    ldr_armado = 1;
                    cont_ldr = 0;
                }
                else if (ldr_armado && ++cont_ldr >= LDR_AMOSTRAS_OK)
                {
                    ldr_armado = 0;
                    cont_ldr = 0;
                    vidas--;
                    movendo = 0;
                    comando_atual = 'S';
                    t_estado = agora;
                    leds_atualiza(vidas);

                    if (vidas == 0)
                    {
                        motores(0, 0);
                        estado = ST_ELIMINADO;
                    }
                    else
                    {
                        motores(1, -1);
                        estado = ST_GIRO;
                    }
                }
            }
        }

        /* 6. Máquina de estados: ST_GIRO -> ST_PAUSA (motores parados)
         *    -> ST_CONTROLE (motores e receptor reativados) */
        if (estado == ST_GIRO && agora - t_estado >= TEMPO_GIRO_MS)
        {
            motores(0, 0);
            t_estado = agora;
            estado = ST_PAUSA;
        }
        else if (estado == ST_PAUSA && agora - t_estado >= TEMPO_PAUSA_MS)
        {
            estado = ST_CONTROLE;
        }

        /* 7. Laser: pisca durante o jogo (LASER_LIGADO_MS ligado dentro de
         *    cada LASER_PERIODO_MS) e fica apagado em ST_MANUAL e
         *    ST_ELIMINADO */
        if (USAR_LASER && estado != ST_MANUAL && estado != ST_ELIMINADO)
        {
            if (agora - t_laser >= LASER_PERIODO_MS)
            {
                t_laser += LASER_PERIODO_MS;
            }

            if (agora - t_laser < LASER_LIGADO_MS)
            {
                PORTD |= _BV(LASER_BIT);
            }
            else
            {
                PORTD &= (uint8_t)~_BV(LASER_BIT);
            }
        }
        else
        {
            PORTD &= (uint8_t)~_BV(LASER_BIT);
        }

        /* 8. Log: monta uma linha a cada PRINT_LDR_MS e a envia um
         *    caractere por volta, sem bloquear o laço, para continuar
         *    lendo os comandos recebidos */
        if (linha[pos_tx] == '\0' && agora - t_print >= PRINT_LDR_MS)
        {
            t_print = agora;
            pos_tx = 0;

            snprintf(linha, sizeof linha,
                     "LDR: %u | Estado: %u | Vidas: %u | Cmd: %c\r\n",
                     (unsigned int)leitura_ldr, (unsigned int)estado,
                     (unsigned int)vidas, comando_atual);
        }

        if (linha[pos_tx] != '\0' && (UCSR0A & _BV(UDRE0)))
        {
            UDR0 = (uint8_t)linha[pos_tx++];
        }
    }
}
