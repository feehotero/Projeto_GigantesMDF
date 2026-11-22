/**
 * @file main.c
 * @brief ESP32 (C puro, ESP-IDF): cria um Wi-Fi, mostra uma página com
 *        setas e repassa os comandos ao ATmega328P pela UART.
 *
 * Wi-Fi: GigantesMDF / senha gigante123  ->  http://192.168.4.1
 * Ligação: GPIO17 (TX2) do ESP32 -> RX (PD0) do Arduino, e GND com GND.
 * Comandos (1 caractere): F frente, B ré, L esquerda, R direita, S parar.
 */

#include <string.h>
#include "esp_idf_version.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "driver/uart.h"

#define UART_PORTA  UART_NUM_2 /**< UART usada para falar com o ATmega     */
#define UART_TX_PIN 17         /**< Pino TX da UART (liga no RX do ATmega) */

/**
 * @brief Página HTML de controle (4 setas).
 *
 * Enquanto a seta está apertada, a página reenvia o comando a cada 100 ms;
 * ao soltar, envia 'S'.
 */
static const char PAGINA[] =
"<!DOCTYPE html><html><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1,user-scalable=no'>"
"<style>"
"body{background:#111;display:flex;justify-content:center;align-items:center;"
"height:100vh;margin:0;touch-action:none;user-select:none}"
".pad{display:grid;grid-template-columns:repeat(3,96px);grid-template-rows:repeat(3,96px);gap:10px}"
"button{font-size:30px;border:0;border-radius:16px;background:#2a2a2a;color:#eee;touch-action:none}"
"button:active{background:#e8590c}"
"</style></head><body><div class='pad'>"
"<button style='grid-area:1/2' data-c='F'>&#9650;</button>"
"<button style='grid-area:2/1' data-c='L'>&#9664;</button>"
"<button style='grid-area:2/3' data-c='R'>&#9654;</button>"
"<button style='grid-area:3/2' data-c='B'>&#9660;</button>"
"</div><script>"
"document.querySelectorAll('button').forEach(b=>{let t=null;"
"const go=()=>fetch('/cmd?c='+b.dataset.c);"
"const st=()=>{if(t){clearInterval(t);t=null;fetch('/cmd?c=S')}};"
"b.onpointerdown=e=>{e.preventDefault();if(!t){go();t=setInterval(go,100)}};"
"b.onpointerup=b.onpointerleave=b.onpointercancel=st;"
"b.oncontextmenu=e=>e.preventDefault()})"
"</script></body></html>";

/**
 * @brief Handler de GET /: devolve a página de controle.
 * @param req Requisição HTTP.
 * @return ESP_OK em caso de sucesso.
 */
static esp_err_t raiz_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGINA, HTTPD_RESP_USE_STRLEN);
}

/**
 * @brief Handler de GET /cmd?c=X: repassa o caractere X ao ATmega pela UART.
 *
 * Só aceita um dos caracteres 'F', 'B', 'L', 'R' ou 'S'.
 *
 * @param req Requisição HTTP.
 * @return ESP_OK em caso de sucesso.
 */
static esp_err_t cmd_handler(httpd_req_t *req)
{
    char query[16], val[4];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "c", val, sizeof(val)) == ESP_OK &&
        val[0] != '\0' && val[1] == '\0' && strchr("FBLRS", val[0]) != NULL)
    {
        uart_write_bytes(UART_PORTA, val, 1);
    }

    return httpd_resp_sendstr(req, "ok");
}

/**
 * @brief Ponto de entrada: configura a UART2, o Wi-Fi (Access Point) e o servidor HTTP.
 */
void app_main(void)
{
    /* UART2: 9600 8N1, só TX */
    const uart_config_t uart_cfg = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
#if ESP_IDF_VERSION_MAJOR >= 5
        .source_clk = UART_SCLK_DEFAULT,
#else
        .source_clk = UART_SCLK_APB,
#endif
    };
    uart_driver_install(UART_PORTA, 256, 0, 0, NULL, 0);
    uart_param_config(UART_PORTA, &uart_cfg);
    uart_set_pin(UART_PORTA, UART_TX_PIN, UART_PIN_NO_CHANGE,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    /* Wi-Fi em modo Access Point */
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wifi_init);

    wifi_config_t wifi_cfg = { 0 };
    strcpy((char *)wifi_cfg.ap.ssid, "GigantesMDF");
    strcpy((char *)wifi_cfg.ap.password, "gigante123");
    wifi_cfg.ap.ssid_len = strlen("GigantesMDF");
    wifi_cfg.ap.max_connection = 4;
    wifi_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg);
    esp_wifi_start();

    /* Servidor HTTP */
    httpd_handle_t srv = NULL;
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    const httpd_uri_t raiz = { .uri = "/",    .method = HTTP_GET, .handler = raiz_handler };
    const httpd_uri_t cmd  = { .uri = "/cmd", .method = HTTP_GET, .handler = cmd_handler };

    if (httpd_start(&srv, &http_cfg) == ESP_OK)
    {
        httpd_register_uri_handler(srv, &raiz);
        httpd_register_uri_handler(srv, &cmd);
    }
}
