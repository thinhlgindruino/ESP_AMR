#include "ethernet.h"
#include "wiznet_port.h"
#include "socket.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "Ethernet";

void ethernet_init(void)
{
    ESP_ERROR_CHECK(wiznet_port_init());

    // --- Cấu hình IP tĩnh (giữ nguyên dải mạng bạn đã dùng) ---
    wiz_NetInfo net_info = {
        .mac = {0x02, 0xAA, 0xBB, 0xCC, 0xDD, 0x01},
        .ip  = {192, 168, 1, 178},
        .sn  = {255, 255, 255, 0},
        .gw  = {192, 168, 1, 1},
        .dns = {8, 8, 8, 8},
        .dhcp = NETINFO_STATIC,
    };
    wizchip_setnetinfo(&net_info);

    wiz_NetInfo check_info;
    wizchip_getnetinfo(&check_info);
    ESP_LOGI(TAG, "IP tinh da gan: %d.%d.%d.%d",
             check_info.ip[0], check_info.ip[1], check_info.ip[2], check_info.ip[3]);

    // --- Kiểm tra link vật lý (đèn LINK trên module có sáng không) ---
    if (wizphy_getphylink() == PHY_LINK_ON) {
        ESP_LOGI(TAG, "PHY Link: ON");
    } else {
        ESP_LOGW(TAG, "PHY Link: OFF - kiem tra day mang RJ45");
    }
}

/*
 * TCP server dùng trực tiếp socket cứng của W5500 — không qua BSD socket / lwIP.
 * Vòng đời 1 socket TCP server kiểu WIZnet:
 *   CLOSED -> socket() -> listen() -> (chờ client) -> ESTABLISHED -> recv/send
 *          -> client đóng kết nối -> CLOSE_WAIT -> disconnect()/close() -> lặp lại
 */
void tcp_server_task(void *pvParameters)
{
    uint8_t sn = TCP_SOCKET_NUM;
    uint8_t rx_buf[128];
    bool client_was_connected = false;

    while (1) {
        uint8_t status = getSn_SR(sn);

        switch (status) {
        case SOCK_CLOSED:
            // Mở lại socket ở chế độ TCP và bắt đầu lắng nghe
            if (socket(sn, Sn_MR_TCP, TCP_PORT, 0x00) != sn) {
                ESP_LOGE(TAG, "socket() that bai, thu lai sau 1s");
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            }
            if (listen(sn) != SOCK_OK) {
                ESP_LOGE(TAG, "listen() that bai");
            } else {
                ESP_LOGI(TAG, "TCP server dang lang nghe tren port %d", TCP_PORT);
            }
            client_was_connected = false;
            break;

        case SOCK_ESTABLISHED:
            if (!client_was_connected) {
                ESP_LOGI(TAG, "Client da ket noi");
                client_was_connected = true;
            }
            {
                int32_t len = getSn_RX_RSR(sn);   // số byte đang chờ trong buffer RX
                if (len > 0) {
                    if (len > (int32_t)sizeof(rx_buf) - 1) {
                        len = sizeof(rx_buf) - 1;
                    }
                    int32_t recv_len = recv(sn, rx_buf, (uint16_t)len);
                    if (recv_len > 0) {
                        rx_buf[recv_len] = 0;
                        while (recv_len > 0 &&
                               (rx_buf[recv_len - 1] == '\r' || rx_buf[recv_len - 1] == '\n')) {
                            rx_buf[--recv_len] = 0;
                        }
                        if (recv_len > 0) {
                            ESP_LOGI(TAG, "Nhan %ld byte: %s", (long)recv_len, rx_buf);
                            send(sn, rx_buf, (uint16_t)recv_len);
                        }
                    }
                }
            }
            break;

        case SOCK_CLOSE_WAIT:
            ESP_LOGI(TAG, "Client ngat ket noi, dong socket");
            disconnect(sn);
            break;

        default:
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(10));   // tần suất poll socket, có thể tinh chỉnh
    }
}
