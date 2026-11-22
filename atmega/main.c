/**
 * @file main.c
 * @brief Gigantes de MDF - Programação de Hardware 2026 (ATmega328P)
 *
 * Robô controlado remotamente por um ESP32 (que recebe um controle de PS4
 * por Bluetooth) ligado ao pino RX (PD0) do ATmega328P via USART.
 *
 * Periféricos utilizados:
 *  - Timer0: PWM dos motores (OC0A = PD6, OC0B = PD5)
 *  - Timer1: laser (OC1B = PB2), 1 s ligado / 1 s desligado
 *  - Timer2: base de tempo de 1 ms (interrupção)
 *  - ADC0 (PC0): receptor LDR
 *  - INT0 (PD2): botão de reinício das vidas
 *  - USART0: comandos vindos do ESP32 (9600 8N1)
 *
 * Protocolo UART (1 caractere por comando, ESP32 -> Arduino, apenas RX):
 *   'F' frente | 'B' ré | 'L' giro esquerda | 'R' giro direita | 'S' parar
 *
 * Parada de segurança: se o robô estiver em movimento e ficar
 * @ref TEMPO_SEM_CMD_MS sem receber comando, os motores são parados.
 *
 * Ao ser atingido pelo laser adversário o robô perde uma vida, gira 180 graus
 * (@ref ST_GIRO_180) e depois fica parado por @ref TEMPO_PAUSA_MS
 * (@ref ST_PAUSA). Sem vidas, fica em @ref ST_ELIMINADO até o botão de
 * reinício ser apertado.
 *
 * As vidas aparecem somente nos LEDs do robô (nada é enviado ao ESP32).
 */

#define F_CPU 16000000UL /**< Clock do ATmega328P (Hz) */
#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdint.h>

/* ---------- Pinos ---------- */
#define MOT_E_PWM   PD6  /**< PWM do motor esquerdo (OC0A) */
#define MOT_D_PWM   PD5  /**< PWM do motor direito (OC0B) */
#define MOT_E_IN1   PD7  /**< Direção do motor esquerdo, entrada 1 */
#define MOT_E_IN2   PB0  /**< Direção do motor esquerdo, entrada 2 */
#define MOT_D_IN3   PD4  /**< Direção do motor direito, entrada 3 */
#define MOT_D_IN4   PB4  /**< Direção do motor direito, entrada 4 */
#define LASER_BIT   PB2  /**< Saída do laser (OC1B) */
#define BTN_BIT     PD2  /**< Botão de reinício (INT0) */
#define LED1        PC1  /**< LED da vida 1 */
#define LED2        PC2  /**< LED da vida 2 */
#define LED3        PC3  /**< LED da vida 3 */
#define LEDS_MASK   (_BV(LED1) | _BV(LED2) | _BV(LED3)) /**< Máscara dos três LEDs de vida */

/* ---------- Parâmetros ---------- */
#define ICR1_2S              31249u   /**< Período do laser: 2 s (prescaler 1024) */
#define OCR1B_1S             15624u   /**< Laser ligado por 1 s                   */
#define OCR2A_1MS            249u     /**< Timer2: 1 ms (prescaler 64)            */
#define LDR_LIMIAR           700u     /**< Leitura do ADC (0-1023) a partir da qual há laser */
#define LDR_AMOSTRAS_OK      3u       /**< Leituras seguidas acima do limiar para contar acerto */
#define TEMPO_PAUSA_MS       5000u    /**< Parado, motores e receptor desativados (após o giro) */
#define TEMPO_GIRO_180_MS    600u     /**< Calibrar no robô real                  */
#define TEMPO_SEM_CMD_MS     300u     /**< Sem comando por este tempo: para os motores */
#define DEBOUNCE_BTN_MS      200u     /**< Tempo mínimo entre dois acionamentos do botão */
#define COMP_E               100      /**< Compensação motor esquerdo (%)         */
#define COMP_D               100      /**< Compensação motor direito (%)          */
#define VELOCIDADE_AVANCO    200      /**< Velocidade fixa: frente, ré e giro manual */
#define VELOCIDADE_GIRO      200      /**< Velocidade do giro automático de 180 graus */

#define BAUD                 9600UL                              /**< Velocidade da UART (bps)  */
#define UBRR_VAL             ((F_CPU / 16UL / BAUD) - 1UL)       /**< Valor do registrador UBRR */

/** @brief Estados do robô */
typedef enum
{
    ST_CONTROLE,    /**< Aceita comandos do ESP32                 */
    ST_GIRO_180,    /**< Atingido: gira 180 graus primeiro        */
    ST_PAUSA,       /**< Depois do giro: 5 s parado, tudo desligado */
    ST_ELIMINADO    /**< Sem vidas: aguarda botão de reinício     */
} state_t;

static volatile uint8_t g_ticks = 0;      /**< Contador de milissegundos (8 bits, incrementado no Timer2) */
static volatile uint8_t g_btn_evento = 0; /**< 1 = botão de reinício foi apertado (INT0)                   */
static volatile uint8_t g_cmd = 0;        /**< Último caractere recebido pela UART                        */
static volatile uint8_t g_cmd_novo = 0;   /**< 1 = há um comando novo em g_cmd a ser tratado              */

static state_t state = ST_CONTROLE;       /**< Estado atual da máquina de estados */
static uint8_t vidas = 3;                 /**< Vidas restantes (0 a 3)            */

/* ---------- Interrupções ---------- */

/**
 * @brief Interrupção do Timer2: incrementa a base de tempo.
 *
 * Ocorre a cada 1 ms e incrementa @ref g_ticks.
 */
ISR(TIMER2_OVF_vect)
{
    g_ticks++;
}

/**
 * @brief Interrupção externa INT0 (PD2): botão de reinício das vidas.
 *
 * Apenas sinaliza o evento em @ref g_btn_evento; o tratamento (com
 * debounce) é feito no laço principal.
 */
ISR(INT0_vect)
{
    g_btn_evento = 1;
}

/**
 * @brief Interrupção de recepção da USART0.
 *
 * Guarda o último caractere recebido do ESP32 em @ref g_cmd e sinaliza
 * em @ref g_cmd_novo.
 */
ISR(USART_RX_vect)
{
    g_cmd = UDR0;
    g_cmd_novo = 1;
}

/* ---------- UART ---------- */

/**
 * @brief Configura a USART0 em 9600 8N1, somente recepção, com interrupção.
 */
static void uart_init(void)
{
    UBRR0H = (uint8_t)(UBRR_VAL >> 8);
    UBRR0L = (uint8_t)UBRR_VAL;
    UCSR0B = _BV(RXEN0) | _BV(RXCIE0);
    UCSR0C = _BV(UCSZ01) | _BV(UCSZ00);
}

/* ---------- Laser ---------- */

/**
 * @brief Liga o laser (reativa a saída PWM do Timer1 em OC1B/PB2).
 *
 * Zera o contador para que o ciclo recomece com o laser aceso.
 */
static void laser_liga(void)
{
    TCNT1 = 0;
    TCCR1A |= _BV(COM1B1);
}

/**
 * @brief Desliga o laser (desconecta o PWM e força PB2 em nível baixo).
 */
static void laser_desliga(void)
{
    TCCR1A &= (uint8_t)~_BV(COM1B1);
    PORTB &= (uint8_t)~_BV(LASER_BIT);
}

/* ---------- Motores ---------- */

/**
 * @brief Define sentido e velocidade do motor esquerdo.
 * @param vel Velocidade de -255 a 255. Sinal positivo = frente, negativo = ré,
 *            zero = parado.
 */
static void motor_e_set(int16_t vel)
{
    uint8_t duty = (uint8_t)((vel < 0) ? -vel : vel);

    if (vel > 0)
    {
        PORTD |= _BV(MOT_E_IN1);
        PORTB &= (uint8_t)~_BV(MOT_E_IN2);
    }
    else if (vel < 0)
    {
        PORTD &= (uint8_t)~_BV(MOT_E_IN1);
        PORTB |= _BV(MOT_E_IN2);
    }
    else
    {
        PORTD &= (uint8_t)~_BV(MOT_E_IN1);
        PORTB &= (uint8_t)~_BV(MOT_E_IN2);
    }

    if (duty == 0)
    {
        TCCR0A &= (uint8_t)~_BV(COM0A1);
        PORTD &= (uint8_t)~_BV(MOT_E_PWM);
    }
    else
    {
        OCR0A = duty;
        TCCR0A |= _BV(COM0A1);
    }
}

/**
 * @brief Define sentido e velocidade do motor direito.
 * @param vel Velocidade de -255 a 255. Sinal positivo = frente, negativo = ré,
 *            zero = parado.
 */
static void motor_d_set(int16_t vel)
{
    uint8_t duty = (uint8_t)((vel < 0) ? -vel : vel);

    if (vel > 0)
    {
        PORTD |= _BV(MOT_D_IN3);
        PORTB &= (uint8_t)~_BV(MOT_D_IN4);
    }
    else if (vel < 0)
    {
        PORTD &= (uint8_t)~_BV(MOT_D_IN3);
        PORTB |= _BV(MOT_D_IN4);
    }
    else
    {
        PORTD &= (uint8_t)~_BV(MOT_D_IN3);
        PORTB &= (uint8_t)~_BV(MOT_D_IN4);
    }

    if (duty == 0)
    {
        TCCR0A &= (uint8_t)~_BV(COM0B1);
        PORTD &= (uint8_t)~_BV(MOT_D_PWM);
    }
    else
    {
        OCR0B = duty;
        TCCR0A |= _BV(COM0B1);
    }
}

/**
 * @brief Aplica a compensação percentual de um motor.
 * @param v   Velocidade original.
 * @param pct Compensação em porcentagem (100 = sem alteração).
 * @return Velocidade compensada.
 */
static int16_t compensa(int16_t v, int16_t pct)
{
    return (int16_t)(((int32_t)v * pct) / 100);
}

/**
 * @brief Para os dois motores.
 */
static void motores_parar(void)
{
    motor_e_set(0);
    motor_d_set(0);
}

/**
 * @brief Move o robô em linha reta.
 * @param v Velocidade de -255 a 255 (negativa = ré).
 */
static void motores_frente(int16_t v)
{
    motor_e_set(compensa(v, COMP_E));
    motor_d_set(compensa(v, COMP_D));
}

/**
 * @brief Gira o robô no próprio eixo.
 * @param v Velocidade. Positiva: sentido horário (direita); negativa: anti-horário.
 */
static void motores_girar(int16_t v)
{
    motor_e_set(compensa(v, COMP_E));
    motor_d_set(compensa(-v, COMP_D));
}

/* ---------- LEDs de vida ---------- */

/**
 * @brief Atualiza os três LEDs que mostram as vidas.
 * @param v Número de vidas (0 a 3); cada vida acende um LED.
 */
static void leds_vidas_atualiza(uint8_t v)
{
    uint8_t m = 0;

    if (v >= 1)
    {
        m |= _BV(LED1);
    }
    if (v >= 2)
    {
        m |= _BV(LED2);
    }
    if (v >= 3)
    {
        m |= _BV(LED3);
    }

    PORTC = (uint8_t)((PORTC & ~LEDS_MASK) | m);
}

/* ---------- ADC ---------- */

/**
 * @brief Faz uma leitura do conversor analógico-digital.
 * @param canal Canal do ADC (0 = PC0, onde está o LDR).
 * @return Valor lido, de 0 a 1023.
 */
static uint16_t adc_read(uint8_t canal)
{
    ADMUX = (uint8_t)((ADMUX & 0xF0) | (canal & 0x0F));
    ADCSRA |= _BV(ADSC);

    while (ADCSRA & _BV(ADSC))
    {
    }

    return ADC;
}

/* ---------- Comandos remotos ---------- */

/**
 * @brief Aplica um comando recebido do ESP32 (somente em ST_CONTROLE)
 * @param c Caractere do comando
 */
static void comando_aplica(uint8_t c)
{
    switch (c)
    {
    case 'F': motores_frente(VELOCIDADE_AVANCO);     break;
    case 'B': motores_frente(-VELOCIDADE_AVANCO);    break;
    case 'L': motores_girar(-VELOCIDADE_AVANCO);     break;
    case 'R': motores_girar(VELOCIDADE_AVANCO);      break;
    case 'S': motores_parar();                       break;
    default:                                         break;
    }
}

/* ---------- Programa principal ---------- */

/**
 * @brief Programa principal.
 *
 * Configura os periféricos e executa o laço que trata o botão de reinício,
 * os comandos do ESP32, a parada de segurança, a leitura do LDR e a máquina
 * de estados (@ref state_t).
 *
 * @return Não retorna.
 */
int main(void)
{
    uint32_t agora = 0;
    uint32_t t_estado = 0;
    uint32_t t_btn = 0;
    uint32_t t_adc = 0;
    uint32_t t_cmd = 0;
    uint8_t movendo = 0;
    uint8_t ult_ticks = 0;
    uint8_t rx_ativo = 1;
    uint8_t cont_ldr = 0;

    DDRC |= LEDS_MASK;
    DDRD |= _BV(MOT_E_PWM) | _BV(MOT_D_PWM) | _BV(MOT_E_IN1) | _BV(MOT_D_IN3);
    DDRB |= _BV(MOT_E_IN2) | _BV(MOT_D_IN4) | _BV(LASER_BIT);

    /* Timer0: Fast PWM 8 bits, prescaler 64 (motores) */
    TCCR0A = _BV(WGM01) | _BV(WGM00);
    TCCR0B = _BV(CS01) | _BV(CS00);

    /* Timer1: Fast PWM modo 14, prescaler 1024 (laser 1 s ON / 1 s OFF) */
    TCCR1A = _BV(COM1B1) | _BV(WGM11);
    ICR1 = ICR1_2S;
    OCR1B = OCR1B_1S;
    TCCR1B = _BV(WGM13) | _BV(WGM12) | _BV(CS12) | _BV(CS10);

    /* Timer2: base de tempo de 1 ms */
    TCCR2A = _BV(WGM21) | _BV(WGM20);
    OCR2A = OCR2A_1MS;
    TIMSK2 = _BV(TOIE2);
    TCCR2B = _BV(WGM22) | _BV(CS22);

    /* ADC0: referência AVcc, prescaler 128 */
    ADMUX = _BV(REFS0);
    ADCSRA = _BV(ADEN) | _BV(ADPS2) | _BV(ADPS1) | _BV(ADPS0);
    DIDR0 = _BV(ADC0D);

    /* Botão em INT0 (borda de descida, pull-up interno) */
    PORTD |= _BV(BTN_BIT);
    EICRA = _BV(ISC01);
    EIMSK = _BV(INT0);

    uart_init();
    leds_vidas_atualiza(vidas);
    sei();

    motores_parar();

    while (1)
    {
        uint8_t t = g_ticks;
        agora += (uint8_t)(t - ult_ticks);
        ult_ticks = t;

        /* --- Botão: reinicia vidas --- */
        if (g_btn_evento)
        {
            g_btn_evento = 0;

            if ((uint32_t)(agora - t_btn) >= DEBOUNCE_BTN_MS)
            {
                t_btn = agora;
                vidas = 3;
                leds_vidas_atualiza(vidas);
                laser_liga();
                rx_ativo = 1;
                cont_ldr = 0;
                g_cmd_novo = 0;
                movendo = 0;
                t_estado = agora;
                state = ST_CONTROLE;
                motores_parar();
            }
        }

        /* --- Comandos vindos do ESP32 --- */
        if (g_cmd_novo)
        {
            uint8_t c = g_cmd;
            g_cmd_novo = 0;

            if (state == ST_CONTROLE)
            {
                comando_aplica(c);
                t_cmd = agora;
                movendo = (c == 'F' || c == 'B' || c == 'L' || c == 'R');
            }
            /* Em GIRO_180, PAUSA e ELIMINADO os comandos são ignorados */
        }

        /* --- Parada de segurança: a página reenvia o comando enquanto
         *     a seta está apertada; sem comando, os motores param --- */
        if (state == ST_CONTROLE && movendo &&
            (uint32_t)(agora - t_cmd) >= TEMPO_SEM_CMD_MS)
        {
            motores_parar();
            movendo = 0;
        }

        /* --- Receptor LDR: detecção do laser adversário --- */
        if (rx_ativo &&
            state == ST_CONTROLE &&
            (uint32_t)(agora - t_adc) >= 1)
        {
            t_adc = agora;

            if (adc_read(0) >= LDR_LIMIAR)
            {
                cont_ldr++;
            }
            else
            {
                cont_ldr = 0;
            }

            if (cont_ldr >= LDR_AMOSTRAS_OK)
            {
                cont_ldr = 0;
                rx_ativo = 0;
                motores_parar();
                movendo = 0;
                vidas--;
                leds_vidas_atualiza(vidas);
                t_estado = agora;

                if (vidas == 0)
                {
                    state = ST_ELIMINADO;
                    laser_desliga();
                }
                else
                {
                    /* Gira 180 graus ANTES da pausa de 5 s */
                    motores_girar(VELOCIDADE_GIRO);
                    state = ST_GIRO_180;
                }
            }
        }

        /* --- Máquina de estados --- */
        switch (state)
        {
        case ST_CONTROLE:
            /* Movimento definido pelos comandos recebidos */
            break;

        case ST_GIRO_180:
            if ((uint32_t)(agora - t_estado) >= TEMPO_GIRO_180_MS)
            {
                motores_parar();
                t_estado = agora;
                state = ST_PAUSA;
            }
            break;

        case ST_PAUSA:
            if ((uint32_t)(agora - t_estado) >= TEMPO_PAUSA_MS)
            {
                rx_ativo = 1;
                cont_ldr = 0;
                g_cmd_novo = 0;
                state = ST_CONTROLE;
            }
            break;

        case ST_ELIMINADO:
            motores_parar();
            break;

        default:
            break;
        }
    }
}
