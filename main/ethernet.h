#pragma once
#include "wizchip_conf.h"

// Socket cứng số 0 dùng cho TCP server
#define TCP_SOCKET_NUM  0
#define TCP_PORT        3333

/**
 * @brief Khởi tạo SPI/GPIO (wiznet_port_init) + gán MAC/IP tĩnh cho W5500.
 */
void ethernet_init(void);

/**
 * @brief Task chạy TCP server bằng socket cứng của W5500 (không dùng lwIP).
 */
void tcp_server_task(void *pvParameters);
