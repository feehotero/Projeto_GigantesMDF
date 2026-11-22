/**
 * @file my_platform.c
 * @brief ESP32 (ESP-IDF + Bluepad32): controle de PS4 (DualShock 4) -> ATmega328P
 *
 * Este arquivo substitui o main/my_platform.c do exemplo examples/esp32 da
 * Bluepad32. O ESP32 pareia com o controle por Bluetooth e repassa comandos de
 * 1 caractere pela UART2 (GPIO17 -> RX/PD0 do Arduino):
 *   'F' frente | 'B' ré | 'L' esquerda | 'R' direita | 'S' parar
 *
 * Controles: direcional (D-pad) ou analógico esquerdo.
 * Pareamento do DS4: segure SHARE + PS até a luz piscar rápido.
 *
 * O comando é reenviado a cada `PERIODO_MS` enquanto algo está apertado; o
 * ATmega para sozinho se ficar 300 ms sem receber nada (parada de segurança).
 */

#include <uni.h>
#include "sdkconfig.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define UART_PORTA   UART_NUM_2 /**< UART usada para falar com o ATmega        */
#define UART_TX_PIN  17         /**< Pino TX da UART (liga no RX do ATmega)    */
#define LIMIAR_STICK 250        /**< Deslocamento mínimo do analógico (-512 a 511) */
#define PERIODO_MS   100        /**< Intervalo de reenvio do comando (ms)      */

static volatile char g_cmd = 'S'; /**< Comando atual (atualizado pelo controle) */

/**
 * @brief Tarefa que reenvia o comando atual ao ATmega periodicamente.
 *
 * Envia o comando enquanto for diferente de 'S' e envia um 'S' final quando
 * o controle é solto.
 *
 * @param arg Não utilizado.
 */
static void tx_task(void *arg)
{
    char ultimo = 'S';

    (void)arg;
    for (;;)
    {
        char c = g_cmd;

        if (c != 'S' || ultimo != 'S')
        {
            uart_write_bytes(UART_PORTA, &c, 1);
        }
        ultimo = c;
        vTaskDelay(pdMS_TO_TICKS(PERIODO_MS));
    }
}

/**
 * @brief Converte o estado do controle em um comando para o robô.
 *
 * O D-pad tem prioridade; se não estiver apertado, vale o eixo do analógico
 * esquerdo com maior deslocamento acima de `LIMIAR_STICK`.
 *
 * @param gp Estado do gamepad.
 * @return 'F', 'B', 'L', 'R' ou 'S'.
 */
static char gamepad_para_comando(const uni_gamepad_t *gp)
{
    /* D-pad tem prioridade */
    if (gp->dpad & DPAD_UP)    return 'F';
    if (gp->dpad & DPAD_DOWN)  return 'B';
    if (gp->dpad & DPAD_LEFT)  return 'L';
    if (gp->dpad & DPAD_RIGHT) return 'R';

    /* Analógico esquerdo: vale o eixo com maior deslocamento */
    int32_t x = gp->axis_x;
    int32_t y = gp->axis_y;          /* para cima = valor negativo */
    int32_t ax = (x < 0) ? -x : x;
    int32_t ay = (y < 0) ? -y : y;

    if (ay >= ax && ay > LIMIAR_STICK)
    {
        return (y < 0) ? 'F' : 'B';
    }
    if (ax > LIMIAR_STICK)
    {
        return (x < 0) ? 'L' : 'R';
    }
    return 'S';
}

/**
 * @brief Inicialização da plataforma: configura a UART e cria a tarefa de envio.
 * @param argc Quantidade de argumentos (não utilizado).
 * @param argv Argumentos (não utilizado).
 */
static void my_platform_init(int argc, const char **argv)
{
    (void)argc;
    (void)argv;

    const uart_config_t cfg = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(UART_PORTA, 256, 0, 0, NULL, 0);
    uart_param_config(UART_PORTA, &cfg);
    uart_set_pin(UART_PORTA, UART_TX_PIN, UART_PIN_NO_CHANGE,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    xTaskCreate(tx_task, "tx_task", 2048, NULL, 5, NULL);
}

/**
 * @brief Chamada quando o Bluetooth está pronto: libera novas conexões.
 */
static void my_platform_on_init_complete(void)
{
    logi("robo: pronto, aguardando controle (SHARE + PS)\n");
    uni_bt_start_scanning_and_autoconnect_unsafe();
    uni_bt_allow_incoming_connections(true);
}

/**
 * @brief Chamada quando um controle conecta.
 * @param d Dispositivo conectado.
 */
static void my_platform_on_device_connected(uni_hid_device_t *d)
{
    (void)d;
    logi("robo: controle conectado\n");
}

/**
 * @brief Chamada quando o controle desconecta: para o robô por segurança.
 * @param d Dispositivo desconectado.
 */
static void my_platform_on_device_disconnected(uni_hid_device_t *d)
{
    (void)d;
    g_cmd = 'S';
    logi("robo: controle desconectado\n");
}

/**
 * @brief Chamada quando o controle está pronto para uso.
 * @param d Dispositivo pronto.
 * @return UNI_ERROR_SUCCESS para aceitar o dispositivo.
 */
static uni_error_t my_platform_on_device_ready(uni_hid_device_t *d)
{
    (void)d;
    return UNI_ERROR_SUCCESS;
}

/**
 * @brief Chamada a cada pacote de dados do controle.
 *
 * Atualiza `g_cmd` conforme o estado do gamepad.
 *
 * @param d   Dispositivo de origem.
 * @param ctl Dados do controle.
 */
static void my_platform_on_controller_data(uni_hid_device_t *d, uni_controller_t *ctl)
{
    (void)d;

    if (ctl->klass == UNI_CONTROLLER_CLASS_GAMEPAD)
    {
        g_cmd = gamepad_para_comando(&ctl->gamepad);
    }
}

/**
 * @brief Propriedades da plataforma (nenhuma é utilizada).
 * @param idx Índice da propriedade.
 * @return Sempre NULL.
 */
static const uni_property_t *my_platform_get_property(uni_property_idx_t idx)
{
    (void)idx;
    return NULL;
}

/**
 * @brief Eventos fora de banda (não utilizados).
 * @param event Evento.
 * @param data  Dados do evento.
 */
static void my_platform_on_oob_event(uni_platform_oob_event_t event, void *data)
{
    (void)event;
    (void)data;
}

/**
 * @brief Devolve a plataforma personalizada para a Bluepad32.
 * @return Ponteiro para a estrutura da plataforma.
 */
struct uni_platform *get_my_platform(void)
{
    static struct uni_platform plat = {
        .name = "robo-ps4",
        .init = my_platform_init,
        .on_init_complete = my_platform_on_init_complete,
        .on_device_connected = my_platform_on_device_connected,
        .on_device_disconnected = my_platform_on_device_disconnected,
        .on_device_ready = my_platform_on_device_ready,
        .on_oob_event = my_platform_on_oob_event,
        .on_controller_data = my_platform_on_controller_data,
        .get_property = my_platform_get_property,
    };

    return &plat;
}
