#ifndef UART_H
#define UART_H

#include <stdint.h>

// Khởi tạo UART giao tiếp với STM32F103
void uart_stm32_init(void);

// Gửi lệnh bật/tắt LED (1 = bật, 0 = tắt)
void uart_stm32_send_led(uint8_t state);
int uart_stm32_read_byte(void);
void uart_stm32_send_neopixel(uint8_t strip_id, uint8_t mode, uint8_t color);
void uart_stm32_send_led_mask(uint8_t mask);
void uart_stm32_send_turn_signal(uint8_t state);
void uart_stm32_start_rx_task(void); 
#endif // UART_H