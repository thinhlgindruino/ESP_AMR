#include "uart.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "udp_api.h"

#define STM32_UART_PORT   UART_NUM_1
#define STM32_TX_PIN      22
#define STM32_RX_PIN      21
#define STM32_BAUD_RATE   115200
#define STM32_BUF_SIZE    256

static const char *TAG = "uart_stm32";

void uart_stm32_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = STM32_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(STM32_UART_PORT, STM32_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(STM32_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(STM32_UART_PORT, STM32_TX_PIN, STM32_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART toi STM32 da khoi tao (TX=%d, RX=%d, baud=%d)",
             STM32_TX_PIN, STM32_RX_PIN, STM32_BAUD_RATE);
}

void uart_stm32_send_led(uint8_t state)
{
    uint8_t cmd = state ? 0x01 : 0x00;
    uart_write_bytes(STM32_UART_PORT, (const char *)&cmd, 1);
    ESP_LOGI(TAG, "Da gui lenh LED: %s", state ? "BAT" : "TAT");
}

int uart_stm32_read_byte(void)
{
    uint8_t data;
    int len = uart_read_bytes(STM32_UART_PORT, &data, 1, 0); 
    if (len > 0) {
        return data;
    }
    return -1;
}

void uart_stm32_send_neopixel(uint8_t strip_id, uint8_t mode, uint8_t color)
{
    uint8_t frame[6] = { 0xAA, 0x02, 3, strip_id, mode, color };
    uint8_t checksum = frame[1] + frame[2] + frame[3] + frame[4] + frame[5];
    uart_write_bytes(STM32_UART_PORT, (const char *)frame, 6);
    uart_write_bytes(STM32_UART_PORT, (const char *)&checksum, 1);
}

// CMD=0x01: LED mask -- DATA = [mask]
void uart_stm32_send_led_mask(uint8_t mask)
{
    uint8_t frame[4] = { 0xAA, 0x01, 1, mask };
    uint8_t checksum = frame[1] + frame[2] + frame[3];
    uart_write_bytes(STM32_UART_PORT, (const char *)frame, 4);
    uart_write_bytes(STM32_UART_PORT, (const char *)&checksum, 1);
}

// CMD=0x03: Turn signal -- DATA = [state] (0=tat, 1=bat)
void uart_stm32_send_turn_signal(uint8_t state)
{
    uint8_t frame[4] = { 0xAA, 0x03, 1, state };
    uint8_t checksum = frame[1] + frame[2] + frame[3];
    uart_write_bytes(STM32_UART_PORT, (const char *)frame, 4);
    uart_write_bytes(STM32_UART_PORT, (const char *)&checksum, 1);
}

static void stm32_rx_task(void *arg)
{
    enum { WAIT_START, WAIT_CMD, WAIT_LEN, WAIT_DATA, WAIT_CHECKSUM } pstate = WAIT_START;
    uint8_t rxCmd = 0, rxLen = 0, rxData[16], rxIdx = 0;
    uint8_t byte;

    while (1) {
        int len = uart_read_bytes(STM32_UART_PORT, &byte, 1, pdMS_TO_TICKS(100));
        if (len <= 0) continue;

        switch (pstate) {
            case WAIT_START:  if (byte == 0xAA) pstate = WAIT_CMD; break;
            case WAIT_CMD:    rxCmd = byte; pstate = WAIT_LEN; break;
            case WAIT_LEN:    rxLen = byte; rxIdx = 0; pstate = rxLen ? WAIT_DATA : WAIT_CHECKSUM; break;
            case WAIT_DATA:   rxData[rxIdx++] = byte; if (rxIdx >= rxLen) pstate = WAIT_CHECKSUM; break;
            case WAIT_CHECKSUM: {
                uint8_t sum = rxCmd + rxLen;
                for (int i = 0; i < rxLen; i++) sum += rxData[i];
                if (sum == byte && rxCmd == 0x10 && rxLen >= 9) {
                    // --- CMD=0x10: bao cao trang thai that tu STM32 ---
                    udp_api_update_led_status(rxData[0]);
                    // udp_api_update_turn_signal_status(rxData[1]);
                    udp_api_update_neopixel_status(rxData[2], rxData[4], rxData[6]);
                    udp_api_update_inputs(rxData[8], rxData[1]);
                }
                pstate = WAIT_START;
                break;
            }
        }
    }
}

void uart_stm32_start_rx_task(void)
{
    xTaskCreate(stm32_rx_task, "stm32_rx_task", 3072, NULL, 5, NULL);
}