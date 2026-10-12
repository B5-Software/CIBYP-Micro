idf_component_register(SRCS "main.c" "../../../src/json.c" "../../../src/micro.c" "../../../src/fragments.c"
  INCLUDE_DIRS "../../../include" REQUIRES esp_wifi esp_netif nvs_flash esp_event esp_timer esp_driver_gpio mbedtls)
