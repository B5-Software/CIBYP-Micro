if(NOT IDF_TARGET MATCHES "^(esp32s3|esp32c6|esp32s31)$")
  message(FATAL_ERROR "CIBYP Micro ESP port supports esp32s3, esp32c6 and esp32s31; select one with idf.py set-target")
endif()
if(IDF_TARGET STREQUAL "esp32s31" AND (IDF_VERSION_MAJOR LESS 6 OR (IDF_VERSION_MAJOR EQUAL 6 AND IDF_VERSION_MINOR LESS 1)))
  message(FATAL_ERROR "ESP32-S31 requires ESP-IDF master/6.1 or newer with preview target support")
endif()
idf_component_register(SRCS "main.c" "../../../src/json.c" "../../../src/micro.c" "../../../src/fragments.c"
  INCLUDE_DIRS "../../../include" REQUIRES esp_wifi esp_netif nvs_flash esp_partition esp_event esp_timer esp_driver_gpio mbedtls)
