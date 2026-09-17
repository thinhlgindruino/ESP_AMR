idf_component_register(
    SRCS
        "main.c"
        "uart.c"
        "bms.c"
        "udp_api.c"
        "crc16.c"
        # "ethernet.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_driver_gpio
        esp_driver_uart
        esp_event
        esp_eth
        esp_netif
        nvs_flash
)
# idf_component_register(SRCS "main.c"
#                     INCLUDE_DIRS "."
#                     REQUIRES esp_driver_gpio esp_driver_spi
#                     PRIV_REQUIRES esp_eth lwip esp_netif w5500)






