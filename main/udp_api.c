#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "esp_log.h"
#include "udp_api.h"
#include "crc16.h"
#include "uart.h"
#include "bms_cmd.h"  

static const char *TAG = "udp_api";
extern QueueHandle_t bms_cmd_queue;
// ---- Ma TYPE theo dung doc API v2.0 ----
#define TYPE_CMD_CONTROL          0x01
#define TYPE_MSG_HELLO            0x81
#define TYPE_MSG_STATUS           0x82
#define TYPE_MSG_SHUTDOWN_REQUEST 0x84
#define PROTOCOL_VERSION          2
#define IPC_TIMEOUT_MS  10000 

static int sock = -1;
static struct sockaddr_in ipc_addr;
static uint16_t local_port_g;
static uint8_t tx_seq = 0;
static TickType_t last_rx_tick = 0;

typedef struct {
    uint16_t voltage_x100;
    int16_t  current_x100;
    uint8_t  soc;
    uint8_t  button_pressed;
    uint8_t  turn_signal;
    uint8_t  np_left_mode, np_right_mode, np_front_mode;
    uint8_t  led_mask;
    uint8_t  warning_code;   
    uint8_t  error_code;
    uint8_t  conn_error;
    uint8_t  charge_state;     
    uint8_t  discharge_state; 
} system_state_t;



static system_state_t state = {0};
static SemaphoreHandle_t state_mutex;
static bool api_network_ready = false;
void udp_api_update_diagnostics(uint8_t warning_code, uint8_t error_code)
{
    if (state_mutex == NULL) return;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    state.warning_code = warning_code;
    state.error_code = error_code;
    xSemaphoreGive(state_mutex);
}

static void send_frame(uint8_t type, const uint8_t *payload, uint8_t len)
{
    uint8_t buf[5 + 255 + 2];
    buf[0] = 0xAA;
    buf[1] = 0x55;
    buf[2] = tx_seq++;
    buf[3] = type;
    buf[4] = len;
    if (len > 0) memcpy(&buf[5], payload, len);

    uint16_t crc = crc16_modbus(&buf[2], 3 + len); // tinh tren SEQ+TYPE+LEN+PAYLOAD
    buf[5 + len]     = crc & 0xFF;        // byte thap truoc
    buf[5 + len + 1] = (crc >> 8) & 0xFF;

    int total_len = 5 + len + 2;
    sendto(sock, buf, total_len, 0, (struct sockaddr *)&ipc_addr, sizeof(ipc_addr));
}

static void send_hello(void)
{
    uint8_t payload = PROTOCOL_VERSION;
    send_frame(TYPE_MSG_HELLO, &payload, 1);
}

void udp_api_state_init(void)
{
    state_mutex = xSemaphoreCreateMutex();
}

static void send_status(void)
{
    uint8_t payload[15];
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    payload[0] = (state.voltage_x100 >> 8) & 0xFF;
    payload[1] = state.voltage_x100 & 0xFF;
    payload[2] = (state.current_x100 >> 8) & 0xFF;
    payload[3] = state.current_x100 & 0xFF;
    payload[4] = state.soc;
    payload[5] = state.button_pressed;
    payload[6] = state.turn_signal;
    payload[7] = state.np_left_mode;
    payload[8] = state.np_right_mode;
    payload[9] = state.np_front_mode;
    payload[10] = state.led_mask;
    payload[11] = state.warning_code;
    payload[12] = state.error_code | state.conn_error;
    payload[13] = state.charge_state;     
    payload[14] = state.discharge_state; 
    xSemaphoreGive(state_mutex);

    send_frame(TYPE_MSG_STATUS, payload, sizeof(payload));
}

void udp_api_send_shutdown_request(uint8_t reason_code, uint16_t voltage_x100, uint8_t soc)
{
    uint8_t payload[4];
    payload[0] = reason_code;
    payload[1] = (voltage_x100 >> 8) & 0xFF;
    payload[2] = voltage_x100 & 0xFF;
    payload[3] = soc;
    send_frame(TYPE_MSG_SHUTDOWN_REQUEST, payload, sizeof(payload));
    ESP_LOGW(TAG, "Da gui MSG_SHUTDOWN_REQUEST (reason=0x%02X)", reason_code);
}

// ---- Xu ly khi nhan duoc CMD_CONTROL tu IPC ----
// Payload 8 byte: [led_mask][turn_signal][np_left_mode][np_left_color]
//                 [np_right_mode][np_right_color][np_front_mode][np_front_color]
static void handle_cmd_control(const uint8_t *p, uint8_t len)
{
    if (len < 8) {
        ESP_LOGW(TAG, "CMD_CONTROL thieu byte (len=%d)", len);
        return;
    }
    uint8_t led_mask   = p[0];
    uint8_t turn_sig   = p[1];
    uint8_t left_mode  = p[2], left_color  = p[3];
    uint8_t right_mode = p[4], right_color = p[5];
    uint8_t front_mode = p[6], front_color = p[7];
    uint8_t charge_cmd    = p[8];  
    uint8_t discharge_cmd = p[9]; 

    ESP_LOGI(TAG, ">>> Forward xuong STM32: led_mask=0x%02X", led_mask);  
    uart_stm32_send_led_mask(led_mask);
    uart_stm32_send_turn_signal(turn_sig);
    uart_stm32_send_neopixel(0, left_mode,  left_color);
    uart_stm32_send_neopixel(1, right_mode, right_color);
    uart_stm32_send_neopixel(2, front_mode, front_color);

    bms_cmd_t cmd = { .charge_on = charge_cmd != 0, .discharge_on = discharge_cmd != 0 };
    xQueueSend(bms_cmd_queue, &cmd, 0);

    xSemaphoreTake(state_mutex, portMAX_DELAY);
    state.led_mask = led_mask;
    state.turn_signal = turn_sig;
    state.np_left_mode = left_mode;
    state.np_right_mode = right_mode;
    state.np_front_mode = front_mode;
    xSemaphoreGive(state_mutex);
}

static void udp_rx_task(void *arg)
{
    uint8_t buf[128];
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);

    while (1) {
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                            (struct sockaddr *)&from_addr, &from_len);
        if (len < 7) continue; // frame toi thieu 5+0+2=7 byte

        if (buf[0] != 0xAA || buf[1] != 0x55) continue; // sai sync

        uint8_t type    = buf[3];
        uint8_t pld_len = buf[4];
        if (len != 5 + pld_len + 2) continue; // LEN khong khop so byte thuc nhan

        uint16_t crc_recv = buf[5 + pld_len] | (buf[5 + pld_len + 1] << 8);
        uint16_t crc_calc = crc16_modbus(&buf[2], 3 + pld_len);
        if (crc_recv != crc_calc) {
            ESP_LOGW(TAG, "CRC sai, bo qua frame");
            continue;
        }
        last_rx_tick = xTaskGetTickCount();  
        ipc_addr.sin_addr = from_addr.sin_addr;

        if (type == TYPE_CMD_CONTROL) {
            handle_cmd_control(&buf[5], pld_len);
        }
    }
}

static void udp_tx_task(void *arg)
{
    uint32_t tick = 0;
    last_rx_tick = xTaskGetTickCount();   // khoi tao gia tri ban dau, tranh bao loi gia luc moi boot
    send_hello();
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(2000));

        // --- Kiem tra timeout ket noi IPC ---
        TickType_t elapsed_ms = (xTaskGetTickCount() - last_rx_tick) * portTICK_PERIOD_MS;
        xSemaphoreTake(state_mutex, portMAX_DELAY);
        if (elapsed_ms > IPC_TIMEOUT_MS) {
            if (!(state.conn_error & ERR_IPC_TIMEOUT)) {
                ESP_LOGW(TAG, "Mat ket noi IPC (khong nhan goi tin nao > %d ms)", IPC_TIMEOUT_MS);
            }
            state.conn_error |= ERR_IPC_TIMEOUT;
        } else {
            state.conn_error &= ~ERR_IPC_TIMEOUT;
        }
        xSemaphoreGive(state_mutex);

        send_status();
        tick += 2000;
        if (tick >= 30000) { send_hello(); tick = 0; }
    }
}

void udp_api_init(const char *ipc_ip, uint16_t ipc_port, uint16_t local_port)
{
    local_port_g = local_port;

    memset(&ipc_addr, 0, sizeof(ipc_addr));
    ipc_addr.sin_family = AF_INET;
    ipc_addr.sin_port = htons(ipc_port);
    ipc_addr.sin_addr.s_addr = inet_addr(ipc_ip);

    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Khong tao duoc socket UDP");
        return;
    }

    struct sockaddr_in local_addr = {0};
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(local_port);
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr));

    xTaskCreate(udp_rx_task, "udp_rx_task", 4096, NULL, 5, NULL);
    xTaskCreate(udp_tx_task, "udp_tx_task", 4096, NULL, 5, NULL);
    api_network_ready = true;
    ESP_LOGI(TAG, "UDP API san sang: IPC=%s:%d, lang nghe port %d", ipc_ip, ipc_port, local_port);
}

// ---- Ham cap nhat trang thai tu cac module khac goi vao ----
void udp_api_update_battery(uint16_t voltage_x100, int16_t current_x100, uint8_t soc)
{
    if (state_mutex == NULL) return; 
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    state.voltage_x100 = voltage_x100;
    state.current_x100 = current_x100;
    state.soc = soc;
    xSemaphoreGive(state_mutex);
}

void udp_api_update_inputs(uint8_t button_pressed, uint8_t turn_signal)
{
    if (state_mutex == NULL) return; 
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    state.button_pressed = button_pressed;
    state.turn_signal = turn_signal;
    xSemaphoreGive(state_mutex);
}

void udp_api_update_neopixel_status(uint8_t left_mode, uint8_t right_mode, uint8_t front_mode)
{
    if (state_mutex == NULL) return; 
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    state.np_left_mode = left_mode;
    state.np_right_mode = right_mode;
    state.np_front_mode = front_mode;
    xSemaphoreGive(state_mutex);
}

void udp_api_update_led_status(uint8_t led_mask)
{
    if (state_mutex == NULL) return; 
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    state.led_mask = led_mask;
    xSemaphoreGive(state_mutex);
}

void udp_api_update_turn_signal_status(uint8_t confirmed_state)
{
    if (state_mutex == NULL) return;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    state.turn_signal = confirmed_state;
    xSemaphoreGive(state_mutex);
}

void udp_api_update_charge_status(uint8_t charge_on, uint8_t discharge_on)
{
    if (state_mutex == NULL) return;
    xSemaphoreTake(state_mutex, portMAX_DELAY);
    state.charge_state = charge_on;
    state.discharge_state = discharge_on;
    xSemaphoreGive(state_mutex);
}
