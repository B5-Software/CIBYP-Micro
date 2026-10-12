/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "cibyp_micro.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_idf_version.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_partition.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static cm_runtime runtime;
static nvs_handle_t storage;
static esp_timer_handle_t fail_safe;
static cm_fragments incoming;
static char pending[CM_FRAME], outgoing[CM_FRAME];
typedef struct {size_t length; unsigned epoch;} frame_job;
static QueueHandle_t jobs;
static atomic_uint epoch;
static unsigned active_job_epoch;
static atomic_bool connected, faulted, pending_busy, needs_hello;
static bool gpio_ready;
static esp_websocket_client_handle_t websocket;
static EventGroupHandle_t network;
static esp_err_t setup_gpio(int pin) {
  if (pin < 0) return ESP_OK;
  if (!GPIO_IS_VALID_OUTPUT_GPIO(pin)) return ESP_ERR_INVALID_ARG;
  esp_err_t error = gpio_set_level(pin, 0);
  if (error != ESP_OK) return error;
  return gpio_config(&(gpio_config_t){.pin_bit_mask = 1ULL << (unsigned)pin, .mode = GPIO_MODE_INPUT_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE});
}
static int load(void *ctx, cm_journal *j) {(void)ctx; size_t bytes = sizeof *j; esp_err_t e = nvs_get_blob(storage, "journal", j, &bytes); return e == ESP_ERR_NVS_NOT_FOUND ? 0 : e != ESP_OK || bytes != sizeof *j;}
static int store(void *ctx, const cm_journal *j) {(void)ctx; return nvs_set_blob(storage, "journal", j, sizeof *j) != ESP_OK || nvs_commit(storage) != ESP_OK;}
static void low(void *ctx) {(void)ctx; if (gpio_ready) (void)gpio_set_level(CONFIG_CIBYP_GPIO, 0);}
static int read_gpio(void *ctx, int *value) {(void)ctx; if (!gpio_ready) return -1; *value = gpio_get_level(CONFIG_CIBYP_GPIO); return 0;}
static int write_gpio(void *ctx, int value, uint32_t ms) {
  (void)ctx; if (!atomic_load(&connected) || active_job_epoch != atomic_load(&epoch) || !gpio_ready) return -1;
  (void)esp_timer_stop(fail_safe);
  if (value && esp_timer_start_once(fail_safe, (uint64_t)ms * 1000) != ESP_OK) return -1;
  int result = gpio_set_level(CONFIG_CIBYP_GPIO, value) == ESP_OK ? 0 : -1;
  if (!atomic_load(&connected) || active_job_epoch != atomic_load(&epoch)) {low(NULL); return -1;}
  return result;
}
static void wifi_event(void *ctx, esp_event_base_t base, int32_t id, void *data) {
  (void)ctx; (void)data;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) (void)esp_wifi_connect();
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {atomic_store(&connected, false); low(NULL); xEventGroupClearBits(network, 1); (void)esp_wifi_connect();}
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(network, 1);
}
static void ws_event(void *ctx, esp_event_base_t base, int32_t id, void *payload) {
  (void)base; (void)ctx;
  esp_websocket_event_data_t *data = payload;
  if (id == WEBSOCKET_EVENT_CONNECTED) {
    memset(&incoming, 0, sizeof incoming); atomic_fetch_add(&epoch, 1); atomic_store(&faulted, false); atomic_store(&connected, true); atomic_store(&needs_hello, true);
  } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_ERROR) {atomic_store(&connected, false); atomic_fetch_add(&epoch, 1); memset(&incoming, 0, sizeof incoming); low(NULL);}
  else if (id == WEBSOCKET_EVENT_DATA) {
    if (data->op_code != 1 && data->op_code != 0) return;
    if (data->payload_len < 0 || data->data_len < 0 || data->payload_offset < 0) {atomic_store(&faulted, true); low(NULL); return;}
    int complete = cm_fragment(&incoming, data->data_ptr, (size_t)data->data_len, (size_t)data->payload_len, (size_t)data->payload_offset, data->op_code, data->fin);
    if (complete < 0) {atomic_store(&faulted, true); low(NULL); return;}
    if (complete) {
      if (atomic_exchange(&pending_busy, true)) {atomic_store(&faulted, true); low(NULL); return;}
      memcpy(pending, incoming.bytes, incoming.length);
      frame_job job = {incoming.length, atomic_load(&epoch)};
      if (xQueueSend(jobs, &job, 0) != pdTRUE) {atomic_store(&pending_busy, false); atomic_store(&faulted, true); low(NULL);}
    }
  }
}
static void worker(void *ctx) {
  (void)ctx; TickType_t telemetry_at = 0;
  for (;;) {
    if (atomic_exchange(&faulted, false)) {
      atomic_store(&connected, false); low(NULL);
      /* Stop/start is deliberately outside the WebSocket event callback. */
      esp_websocket_client_stop(websocket); esp_websocket_client_start(websocket);
    }
    if (atomic_load(&connected) && atomic_exchange(&needs_hello, false)) {
      char boot[40]; snprintf(boot, sizeof boot, "%08lx-%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
      int n = cm_hello(&runtime, boot, outgoing, sizeof outgoing);
      if (n < 0 || esp_websocket_client_send_text(websocket, outgoing, n, pdMS_TO_TICKS(2000)) != n) atomic_store(&faulted, true);
    }
    frame_job job;
    if (xQueueReceive(jobs, &job, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (atomic_load(&connected) && job.epoch == atomic_load(&epoch)) {
        active_job_epoch = job.epoch;
        int n = cm_handle(&runtime, pending, job.length, outgoing, sizeof outgoing);
        if (n < 0) {low(NULL); atomic_store(&faulted, true);}
        else if (n > 0 && atomic_load(&connected) && job.epoch == atomic_load(&epoch) && esp_websocket_client_send_text(websocket, outgoing, n, pdMS_TO_TICKS(2000)) != n) atomic_store(&faulted, true);
      }
      atomic_store(&pending_busy, false);
    }
    if (atomic_load(&connected) && (xTaskGetTickCount() - telemetry_at) >= pdMS_TO_TICKS(30000)) {
      telemetry_at = xTaskGetTickCount();
      int n = snprintf(outgoing, sizeof outgoing, "{\"type\":\"telemetry\",\"payload\":{\"target\":\"%s\",\"freeHeap\":%lu,\"minimumFreeHeap\":%lu,\"workerStackWatermark\":%lu,\"restartReason\":%d,\"journalCapacity\":%d}}", CONFIG_IDF_TARGET, (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size(), (unsigned long)uxTaskGetStackHighWaterMark(NULL), (int)esp_reset_reason(), CM_OPERATIONS);
      if (n > 0 && esp_websocket_client_send_text(websocket, outgoing, n, pdMS_TO_TICKS(2000)) != n) atomic_store(&faulted, true);
    }
  }
}
void app_main(void) {
  printf("CIBYP Micro: %s, ESP-IDF %d.%d.%d, GPIO %d\n", CONFIG_IDF_TARGET, ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH, CONFIG_CIBYP_GPIO);
  /* Never erase a failed NVS partition automatically: that could repeat writes. */
  const esp_partition_t *journal_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
  if (!journal_partition || journal_partition->size < 2 * sizeof(cm_journal) + 4 * 4096) {printf("CIBYP Micro: NVS partition must have at least 64 KiB for durable journal updates; use the matching partition table\n"); return;}
  ESP_ERROR_CHECK(nvs_flash_init()); ESP_ERROR_CHECK(nvs_open("cibyp", NVS_READWRITE, &storage));
  ESP_ERROR_CHECK(esp_timer_create(&(esp_timer_create_args_t){.callback = low, .name = "gpio-safe"}, &fail_safe));
  esp_err_t gpio_error = setup_gpio(CONFIG_CIBYP_GPIO);
  if (gpio_error != ESP_OK) {printf("CIBYP Micro: cannot configure GPIO %d on %s (%s); check this board's pinout\n", CONFIG_CIBYP_GPIO, CONFIG_IDF_TARGET, esp_err_to_name(gpio_error)); return;}
  gpio_ready = CONFIG_CIBYP_GPIO >= 0;
  low(NULL);
  if (strncmp(CONFIG_CIBYP_GATEWAY, "wss://", 6) || strlen(CONFIG_CIBYP_DEVICE_TOKEN) != 64 || !strlen(CONFIG_CIBYP_WIFI_SSID)) {printf("CIBYP Micro: configure Wi-Fi, TLS gateway and device credentials using idf.py menuconfig\n"); return;}
  char resource[40]; snprintf(resource, sizeof resource, "gpio:%d", CONFIG_CIBYP_GPIO);
  cm_platform platform = {NULL, load, store, read_gpio, write_gpio}; if (cm_init(&runtime, &platform, CONFIG_CIBYP_DEVICE_ID, resource)) {low(NULL); printf("CIBYP Micro: journal unavailable; writes disabled\n"); return;}
  network = xEventGroupCreate(); ESP_ERROR_CHECK(esp_netif_init()); ESP_ERROR_CHECK(esp_event_loop_create_default()); esp_netif_create_default_wifi_sta();
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT(); ESP_ERROR_CHECK(esp_wifi_init(&init));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL)); ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
  wifi_config_t wifi = {0}; snprintf((char *)wifi.sta.ssid, sizeof wifi.sta.ssid, "%s", CONFIG_CIBYP_WIFI_SSID); snprintf((char *)wifi.sta.password, sizeof wifi.sta.password, "%s", CONFIG_CIBYP_WIFI_PASSWORD);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi)); ESP_ERROR_CHECK(esp_wifi_start());
  xEventGroupWaitBits(network, 1, pdFALSE, pdTRUE, portMAX_DELAY);
  esp_sntp_config_t clock = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  ESP_ERROR_CHECK(esp_netif_sntp_init(&clock));
  while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(30000)) != ESP_OK) {printf("CIBYP Micro: waiting for a trusted clock before TLS\n"); low(NULL);}
  char headers[120]; snprintf(headers, sizeof headers, "Authorization: Bearer %s\r\n", CONFIG_CIBYP_DEVICE_TOKEN);
  esp_websocket_client_config_t config = {.uri = CONFIG_CIBYP_GATEWAY, .headers = headers, .crt_bundle_attach = esp_crt_bundle_attach, .buffer_size = CM_FRAME, .task_stack = 16384, .reconnect_timeout_ms = 5000, .network_timeout_ms = 10000};
  websocket = esp_websocket_client_init(&config); jobs = xQueueCreate(1, sizeof(frame_job)); if (!websocket || !jobs) abort();
  ESP_ERROR_CHECK(esp_websocket_register_events(websocket, WEBSOCKET_EVENT_ANY, ws_event, NULL));
  if (xTaskCreate(worker, "cibyp-device", 16384, NULL, 5, NULL) != pdPASS) abort();
  ESP_ERROR_CHECK(esp_websocket_client_start(websocket));
}
