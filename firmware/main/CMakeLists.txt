idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_wifi esp_event esp_netif nvs_flash esp_http_client mbedtls json
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE RELAY_ACTIVE_LOW=${RELAY_ACTIVE_LOW})
