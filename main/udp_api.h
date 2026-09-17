#ifndef UDP_API_H
#define UDP_API_H
#include <stdint.h>

#define WARN_LOW_BATTERY     0x01   // SOC thap (nhung chua toi nguong critical)
#define ERR_BMS_READ_FAIL    0x02   // Doc BMS that bai
#define ERR_BATTERY_CRITICAL 0x03   // Pin qua yeu, sap shutdown
#define ERR_IPC_TIMEOUT      0x04 
void udp_api_update_diagnostics(uint8_t warning_code, uint8_t error_code);
void udp_api_init(const char *ipc_ip, uint16_t ipc_port, uint16_t local_port);
void udp_api_update_battery(uint16_t voltage_x100, int16_t current_x100, uint8_t soc);
void udp_api_update_inputs(uint8_t button_pressed, uint8_t turn_signal);
void udp_api_update_neopixel_status(uint8_t left_mode, uint8_t right_mode, uint8_t front_mode);
void udp_api_update_led_status(uint8_t led_mask);
void udp_api_state_init(void);
void udp_api_send_shutdown_request(uint8_t reason_code, uint16_t voltage_x100, uint8_t soc);
void udp_api_update_turn_signal_status(uint8_t confirmed_state);
void udp_api_update_charge_status(uint8_t charge_on, uint8_t discharge_on);
#endif