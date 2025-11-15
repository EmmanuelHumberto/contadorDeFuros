#pragma once

#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_MANAGER_MAX_SSID_LEN 32
#define WIFI_MANAGER_MAX_PASS_LEN 64
#define WIFI_MANAGER_MAX_SCAN_RESULTS 20

typedef struct {
    bool initialized;
    bool connecting;
    bool connected;
    wifi_err_reason_t last_disconnect_reason;
    char ssid[WIFI_MANAGER_MAX_SSID_LEN + 1];
    char ip[16];
} wifi_status_info_t;

typedef void (*wifi_scan_result_cb_t)(const wifi_ap_record_t *records, uint16_t count);

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
esp_err_t wifi_manager_disconnect(void);
esp_err_t wifi_manager_get_status(wifi_status_info_t *info);
esp_err_t wifi_manager_start_scan(wifi_scan_result_cb_t cb);

#ifdef __cplusplus
}
#endif
