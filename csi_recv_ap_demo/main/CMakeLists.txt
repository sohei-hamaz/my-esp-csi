idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_csi_gain_ctrl
        esp_event
        esp_netif
        esp_wifi
        lwip
        mqtt
        nvs_flash
        protocol_examples_common
)
