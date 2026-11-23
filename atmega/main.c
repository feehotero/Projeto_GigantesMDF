/**
 * @file main.c
 * @brief Controle do carrinho com ATmega328P a 16 MHz.
 *
 * Motor esquerdo: PWM D6, direção D4/D5.
 * Motor direito:  PWM D10, direção D7/D8.
 * Botão: D2. LEDs de vida: A0, A1 e A2. LDR: A3.
 * UART: D0/RX e D1/TX, 9600 baud.
 */

#define F_CPU 16000000UL

#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdint.h>

#define PWM_MOTORES        200u
#define LDR_LIMIAR         800u
#define LDR_AMOSTRAS_OK    3u

#define TEMPO_SEM_CMD_MS   300UL
#define TEMPO_GIRO_MS      1250UL
#define TEMPO_PAUSA_MS     5000UL
#define DEBOUNCE_MS        200UL
#define INTERVALO_LOG_MS   200UL

#define LEDS_VIDAS  (_BV(PC0) | _BV(PC1) | _BV(PC2))
#define LED_BANCADA PB5
#define UBRR_VAL    ((F_CPU / (16UL * 9600UL)) - 1UL)

typedef enum
{
    CONTROLE  = 0,
    PAUSA     = 1,
    GIRO      = 2,
    ELIMINADO = 3
} estado_t;

static volatile uint32_t tempo_ms = 0;
static volatile uint8_t evento_botao = 0;
static volatile uint8_t ciclo_ativo = 0;
static volatile uint16_t fase_ms = 0;

/** @brief Conta o tempo e sinaliza o ciclo de 1 s ligado e 1 s desligado. */
ISR(TIMER2_COMPA_vect)
{
    tempo_ms++;

    if (ciclo_ativo)
    {
        fase_ms++;

        if (fase_ms == 1000u)
        {
            PORTB &= (uint8_t)~_BV(LED_BANCADA);
        }
        else if (fase_ms >= 2000u)
        {
            fase_ms = 0;
            PORTB |= _BV(LED_BANCADA);
        }
    }
}

/** @brief Registra a borda de descida do botão em D2. */
ISR(INT0_vect)
{
    evento_botao = 1;
}

static uint32_t le_eventos(uint8_t *botao)
{
    uint8_t sreg = SREG;
    cli();

    uint32_t agora = tempo_ms;
    *botao = evento_botao;
    evento_botao = 0;

    SREG = sreg;
    return agora;
}

static void ciclo_bancada(uint8_t ligar)
{
    uint8_t sreg = SREG;
    cli();

    fase_ms = 0;
    ciclo_ativo = ligar;

    if (ligar)
        PORTB |= _BV(LED_BANCADA);
    else
        PORTB &= (uint8_t)~_BV(LED_BANCADA);

    SREG = sreg;
}

/** @brief Mostra as vidas restantes nos LEDs A0, A1 e A2. */
static void mostra_vidas(uint8_t vidas)
{
    uint8_t leds = 0;

    if (vidas >= 1) leds |= _BV(PC0);
    if (vidas >= 2) leds |= _BV(PC1);
    if (vidas >= 3) leds |= _BV(PC2);

    PORTC = (PORTC & (uint8_t)~LEDS_VIDAS) | leds;
}

/**
 * @brief Define o sentido e habilita o PWM de cada motor.
 * @param esquerdo Sentido do canal esquerdo: -1, 0 ou 1.
 * @param direito Sentido do canal direito: -1, 0 ou 1.
 */
static void motores(int8_t esquerdo, int8_t direito)
{
    uint8_t direcao_d = PORTD &
        (uint8_t)~(_BV(PD4) | _BV(PD5) | _BV(PD7));

    if (esquerdo > 0) direcao_d |= _BV(PD4);
    if (esquerdo < 0) direcao_d |= _BV(PD5);
    if (direito > 0)  direcao_d |= _BV(PD7);

    PORTD = direcao_d;

    if (direito < 0)
        PORTB |= _BV(PB0);          // D8
    else
        PORTB &= (uint8_t)~_BV(PB0);

    if (esquerdo != 0)
        TCCR0A |= _BV(COM0A1);      // PWM em D6
    else
        TCCR0A &= (uint8_t)~_BV(COM0A1);

    if (direito != 0)
        TCCR1A |= _BV(COM1B1);      // PWM em D10
    else
        TCCR1A &= (uint8_t)~_BV(COM1B1);
}

/** @brief Aplica o mapeamento da montagem atual do carrinho. */
static uint8_t aplica_comando(uint8_t comando)
{
    switch (comando)
    {
        case 'L': motores( 1,  1); break;
        case 'R': motores(-1, -1); break;
        case 'F': motores(-1,  1); break;
        case 'B': motores( 1, -1); break;
        case 'S': motores( 0,  0); break;
        default: return 0;
    }

    return 1;
}

/** @brief Lê o LDR ligado ao ADC3 (A3). */
static uint16_t le_ldr(void)
{
    ADCSRA |= _BV(ADSC);

    while (ADCSRA & _BV(ADSC))
    {
    }

    return ADC;
}

/** @brief Lê um byte válido do ESP32, ou retorna zero. */
static uint8_t le_comando(void)
{
    uint8_t status = UCSR0A;

    if (!(status & _BV(RXC0)))
        return 0;

    uint8_t dado = UDR0;

    if (status & (_BV(FE0) | _BV(DOR0) | _BV(UPE0)))
        return 0;

    return dado;
}

/** @brief Envia o diagnóstico pela UART, um caractere por vez. */
static void atualiza_log(uint32_t agora, uint16_t ldr, estado_t estado,
                         uint8_t vidas, char comando)
{
    static char linha[] = "LDR=0000 Estado=0 Vidas=0 Cmd=S\r\n";
    static uint8_t pos = sizeof(linha) - 1u;
    static uint32_t anterior = 0;

    if (linha[pos] == '\0' && agora - anterior >= INTERVALO_LOG_MS)
    {
        anterior = agora;

        for (uint8_t i = 0; i < 4; i++)
        {
            linha[7u - i] = (char)('0' + ldr % 10u);
            ldr /= 10u;
        }

        linha[16] = (char)('0' + estado);
        linha[24] = (char)('0' + vidas);
        linha[30] = comando;
        pos = 0;
    }

    if (linha[pos] != '\0' && (UCSR0A & _BV(UDRE0)))
        UDR0 = (uint8_t)linha[pos++];
}

static void configura_hardware(void)
{
    // Ponte H: canais de direção e PWM.
    DDRD |= _BV(PD4) | _BV(PD5) | _BV(PD6) | _BV(PD7);
    DDRB |= _BV(PB0) | _BV(PB2) | _BV(LED_BANCADA);
    PORTD &= (uint8_t)~_BV(PD6);
    PORTB &= (uint8_t)~_BV(PB2);

    // Timer0: Fast PWM de 8 bits em D6 (OC0A).
    TCCR0A = _BV(WGM01) | _BV(WGM00);
    TCCR0B = _BV(CS01) | _BV(CS00);
    OCR0A = PWM_MOTORES;

    // Timer1: PWM de 8 bits com correção de fase em D10 (OC1B).
    TCCR1A = _BV(WGM10);
    TCCR1B = _BV(CS11) | _BV(CS10);
    OCR1B = PWM_MOTORES;

    motores(0, 0);

    // LEDs em A0–A2 e entrada analógica em A3.
    DDRC |= LEDS_VIDAS;
    DDRC &= (uint8_t)~_BV(PC3);
    PORTC &= (uint8_t)~_BV(PC3);
    ADMUX = _BV(REFS0) | _BV(MUX1) | _BV(MUX0);
    ADCSRA = _BV(ADEN) | _BV(ADPS2) | _BV(ADPS1) | _BV(ADPS0);
    DIDR0 = _BV(ADC3D);

    // Botão em D2, com pull-up e interrupção na borda de descida.
    DDRD &= (uint8_t)~_BV(PD2);
    PORTD |= _BV(PD2);
    EICRA = _BV(ISC01);
    EIFR = _BV(INTF0);
    EIMSK = _BV(INT0);

    // Na bancada, o ciclo é mostrado no LED D13.
    PORTD &= (uint8_t)~_BV(PD3);
    DDRD |= _BV(PD3);

    // UART a 9600 baud, 8N1.
    UCSR0A = 0;
    UBRR0H = (uint8_t)(UBRR_VAL >> 8);
    UBRR0L = (uint8_t)UBRR_VAL;
    UCSR0C = _BV(UCSZ01) | _BV(UCSZ00);
    UCSR0B = _BV(RXEN0) | _BV(TXEN0);

    // Timer2 em CTC: interrupção a cada 1 ms.
    TCCR2A = _BV(WGM21);
    TCNT2 = 0;
    OCR2A = 249;
    TIFR2 = _BV(OCF2A);
    TIMSK2 = _BV(OCIE2A);
    TCCR2B = _BV(CS22);
}

int main(void)
{
    estado_t estado = CONTROLE;
    uint8_t vidas = 3;
    uint8_t amostras_altas = 0;
    uint8_t ldr_armado = 1;
    uint16_t leitura = 0;
    char comando_atual = 'S';

    uint32_t ultimo_comando = 0;
    uint32_t ultima_amostra = 0;
    uint32_t inicio_estado = 0;
    uint32_t ultimo_botao = 0UL - DEBOUNCE_MS;

    configura_hardware();
    mostra_vidas(vidas);
    ciclo_bancada(1);
    sei();

    for (;;)
    {
        uint8_t botao;
        uint32_t agora = le_eventos(&botao);
        uint8_t comando = le_comando();

        // O botão pode reiniciar a partida em qualquer estado.
        if (botao && agora - ultimo_botao >= DEBOUNCE_MS)
        {
            ultimo_botao = agora;
            vidas = 3;
            estado = CONTROLE;
            amostras_altas = 0;
            ldr_armado = 1;
            comando_atual = 'S';
            comando = 0;

            ADCSRA |= _BV(ADEN);
            motores(0, 0);
            mostra_vidas(vidas);
            ciclo_bancada(1);
        }

        switch (estado)
        {
            case CONTROLE:
                if (aplica_comando(comando))
                {
                    ultimo_comando = agora;
                    comando_atual = (char)comando;
                }

                if (comando_atual != 'S' &&
                    agora - ultimo_comando >= TEMPO_SEM_CMD_MS)
                {
                    motores(0, 0);
                    comando_atual = 'S';
                }

                if (agora - ultima_amostra >= 1UL)
                {
                    ultima_amostra = agora;
                    leitura = le_ldr();

                    if (leitura < LDR_LIMIAR)
                    {
                        amostras_altas = 0;
                        ldr_armado = 1;
                    }
                    else if (ldr_armado)
                    {
                        amostras_altas++;

                        if (amostras_altas >= LDR_AMOSTRAS_OK)
                        {
                            // Acerto: bloqueia o receptor e apaga uma vida.
                            ADCSRA &= (uint8_t)~_BV(ADEN);
                            ldr_armado = 0;
                            amostras_altas = 0;
                            vidas--;
                            mostra_vidas(vidas);

                            // Cada acerto gera um giro, inclusive o último.
                            aplica_comando('R');
                            comando_atual = 'R';
                            inicio_estado = agora;
                            estado = GIRO;

                            if (vidas == 0)
                                ciclo_bancada(0);
                        }
                    }
                }
                break;

            case GIRO:
                if (agora - inicio_estado >= TEMPO_GIRO_MS)
                {
                    motores(0, 0);
                    comando_atual = 'S';

                    if (vidas == 0)
                    {
                        // Última vida: motores parados e ADC desligado.
                        estado = ELIMINADO;
                    }
                    else
                    {
                        // Os 5 segundos começam após parar os motores.
                        inicio_estado = agora;
                        estado = PAUSA;
                    }
                }
                break;

            case PAUSA:
                if (agora - inicio_estado >= TEMPO_PAUSA_MS)
                {
                    ADCSRA |= _BV(ADEN);
                    estado = CONTROLE;
                }
                break;

            case ELIMINADO:
                // Somente o botão de reinício libera uma nova partida.
                break;
        }
        // Replica no D3 o ciclo de 1 s ligado e 1 s desligado do Timer2.
        if (PORTB & _BV(LED_BANCADA))
        {
            PORTD |= _BV(PD3);
        }
        else
        {
            PORTD &= (uint8_t)~_BV(PD3);
        }
        atualiza_log(agora, leitura, estado, vidas, comando_atual);
    }
}