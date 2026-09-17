#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "driver/spi_master.h"
#include "esp_eth_mac_w5500.h"
#include "esp_eth_phy_w5500.h"
#include "uart.h"
#include "bms.h"
#include "udp_api.h"
#include "lwip/ip4_addr.h"
#include "esp_eth_com.h"
#include "freertos/queue.h"
#include "bms_cmd.h"  


static const char *TAG = "W5500";
QueueHandle_t bms_cmd_queue;

#define IPC_IP        "192.168.1.100"
#define IPC_PORT      5000
#define LOCAL_PORT    5001

static void bms_command_task(void *arg)
{
    bms_cmd_t cmd;
    while (1) {
        if (xQueueReceive(bms_cmd_queue, &cmd, portMAX_DELAY)) {
            bms_setChargeSwitch(cmd.charge_on);      
            bms_setDischargeSwitch(cmd.discharge_on);
        }
    }
}

static void bms_read_task(void *pvParameters)
{
    while (1)
    {
        if (bms_update())
        {
            float v = bms_getTotalVoltage();
            float i = bms_getCurrent();
            uint8_t soc = bms_getSOC();

            ESP_LOGI(TAG, "Dien ap: %.2f V | Dong dien: %.2f A | SOC: %d%% | FET Sac[%s] Xa[%s]",
                     v, i, soc,
                     bms_isChargeOn() ? "ON" : "OFF",
                     bms_isDischargeOn() ? "ON" : "OFF");

            uint16_t voltage_x100 = (uint16_t)(v * 100);
            int16_t  current_x100 = (int16_t)(i * 100);
            udp_api_update_battery(voltage_x100, current_x100, soc);
            udp_api_update_charge_status(bms_isChargeOn() ? 1 : 0, bms_isDischargeOn() ? 1 : 0);
            uint8_t warning_code = 0;
            uint8_t error_code = 0;

            if (soc <= 20 && soc > 5) {
                warning_code |= WARN_LOW_BATTERY;
                ESP_LOGW(TAG, "Canh bao: SOC thap (%d%%)", soc);
            }
            if (soc <= 70) {
                error_code |= ERR_BATTERY_CRITICAL;
                udp_api_send_shutdown_request(0x01, voltage_x100, soc);
            }
            udp_api_update_diagnostics(warning_code, error_code);
        }
        else
        {
            ESP_LOGE(TAG, "Loi doc du lieu tu BMS!");
            udp_api_update_diagnostics(0, ERR_BMS_READ_FAIL); 
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void console_task(void *pvParameters)
{
    char line[32];
    while (1)                                            
    {
        if (fgets(line, sizeof(line), stdin) != NULL)
        {
            line[strcspn(line, "\r\n")] = 0;
            if (strcmp(line, "on") == 0) bms_setPower(true);
            else if (strcmp(line, "off") == 0) bms_setPower(false);
            else if (strcmp(line, "sacon") == 0) bms_setChargeSwitch(true);
            else if (strcmp(line, "sacoff") == 0) bms_setChargeSwitch(false);
            else if (strcmp(line, "xaon") == 0) bms_setDischargeSwitch(true);
            else if (strcmp(line, "xaoff") == 0) bms_setDischargeSwitch(false);
            else if (strlen(line) > 0)
                ESP_LOGW(TAG, "Lenh khong hop le: '%s'", line);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}



static void on_eth_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Ethernet Link UP -- cap mang da ket noi vat ly");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Ethernet Link DOWN -- kiem tra day cap RJ45");
            break;
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet Started");
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet Stopped");
            break;
        default:
            break;
    }
}

static void phy_monitor_task(void *arg)
{
    esp_eth_handle_t eth_handle = (esp_eth_handle_t)arg;
    esp_eth_mediator_t *eth = (esp_eth_mediator_t *)eth_handle;

    for (int i = 0; i < 15; i++) {   
        uint32_t phycfgr = 0;
        eth->phy_reg_read(eth, 0, 0x00, &phycfgr);
        ESP_LOGI(TAG, "[%ds] PHYCFGR = 0x%02" PRIx32 " (LNK: %s)",
                 i, phycfgr, (phycfgr & 0x01) ? "CO LINK" : "MAT LINK");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelete(NULL);
}

static void check_phy_status(esp_eth_handle_t eth_handle)
{
    esp_eth_mediator_t *eth = (esp_eth_mediator_t *)eth_handle;
    uint32_t phycfgr = 0;
    eth->phy_reg_read(eth, 0, 0x00, &phycfgr);  
    ESP_LOGI(TAG, "PHYCFGR = 0x%02" PRIx32 " (bit0 LNK: %s)",
             phycfgr, (phycfgr & 0x01) ? "CO LINK" : "MAT LINK");
}

static esp_eth_handle_t eth_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &on_eth_event, NULL));
    spi_bus_config_t buscfg = {
        .mosi_io_num = 23,
        .miso_io_num = 19,
        .sclk_io_num = 18,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t spi_devcfg = {
        .command_bits = 16,          
        .address_bits = 8,
        .mode = 0,
        .clock_speed_hz = 20 * 1000 * 1000,
        .queue_size = 20,
        .spics_io_num = 5,
    };

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &spi_devcfg);
    w5500_config.base.int_gpio_num = -1;     
    w5500_config.base.poll_period_ms = 10;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096;
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;         
    phy_config.reset_gpio_num = 4;    
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    uint8_t mac_addr[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};  
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(eth_netif));

    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    IP4_ADDR(&ip_info.ip,      192, 168, 1, 50);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(eth_netif, &ip_info));

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
    ESP_LOGI(TAG, "W5500 da khoi dong voi IP tinh: 192.168.1.50");

    udp_api_init(IPC_IP, IPC_PORT, LOCAL_PORT);

    return eth_handle;
}



void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    udp_api_state_init(); 
    eth_init();

    bms_init();
    xTaskCreate(bms_read_task, "bms_read_task", 4096, NULL, 5, NULL);
    xTaskCreate(console_task, "console_task", 4096, NULL, 5, NULL);

    bms_cmd_queue = xQueueCreate(4, sizeof(bms_cmd_t));
    xTaskCreate(bms_command_task, "bms_cmd_task", 4096, NULL, 5, NULL);

    uart_stm32_init();
    uart_stm32_start_rx_task(); 
    // uart_stm32_send_neopixel(0, 1, 1);
    // uart_stm32_send_neopixel(1, 2, 2);
    // uart_stm32_send_neopixel(2, 3, 3);

    ESP_LOGI(TAG, "San sang. Lenh console: on / off / sacon / sacoff / xaon / xaoff");
}

