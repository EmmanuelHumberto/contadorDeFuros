#include "wifi_manager.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_netif_ip_addr.h"

static const char *TAG = "wifi_mgr";

static bool s_wifi_initialized = false;
static bool s_wifi_started = false;
static wifi_status_info_t s_status = {0};
static wifi_scan_result_cb_t s_scan_callback = NULL;
static wifi_ap_record_t s_scan_results[WIFI_MANAGER_MAX_SCAN_RESULTS];

static void wifi_update_status_ip(const ip_event_got_ip_t *event);
static void wifi_handle_scan_done(void);

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "Wi-Fi STA iniciado");
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            const wifi_event_sta_connected_t *connected = (const wifi_event_sta_connected_t *)event_data;
            memset(&s_status, 0, sizeof(s_status));
            s_status.initialized = true;
            s_status.connecting = true;
            strlcpy(s_status.ssid, (const char *)connected->ssid, sizeof(s_status.ssid));
            ESP_LOGI(TAG, "Conectado ao AP: %s", s_status.ssid);
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *disconnected = (const wifi_event_sta_disconnected_t *)event_data;
            s_status.connecting = false;
            s_status.connected = false;
            s_status.last_disconnect_reason = disconnected->reason;
            s_status.ip[0] = '\0';
            ESP_LOGW(TAG, "Desconectado (motivo=%d)", disconnected->reason);
            break;
        }
        case WIFI_EVENT_SCAN_DONE:
            wifi_handle_scan_done();
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_update_status_ip((const ip_event_got_ip_t *)event_data);
    }
}

static void wifi_update_status_ip(const ip_event_got_ip_t *event)
{
    s_status.connected = true;
    s_status.connecting = false;
    snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "Endereco IP obtido: %s", s_status.ip);
}

static void wifi_handle_scan_done(void)
{
    if (!s_scan_callback) {
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > WIFI_MANAGER_MAX_SCAN_RESULTS) {
        ap_count = WIFI_MANAGER_MAX_SCAN_RESULTS;
    }
    if (ap_count > 0) {
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, s_scan_results));
    }
    s_scan_callback(s_scan_results, ap_count);
    s_scan_callback = NULL;
}

static esp_err_t wifi_manager_ensure_started(void)
{
    if (!s_wifi_initialized) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        s_wifi_initialized = true;
    }

    if (!s_wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifi_started = true;
    }

    return ESP_OK;
}

esp_err_t wifi_manager_init(void)
{
    return wifi_manager_ensure_started();
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    esp_err_t err = wifi_manager_ensure_started();
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t wifi_config = {0};
    if (ssid) {
        strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    }
    if (password) {
        strlcpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    }
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_LOGI(TAG, "Conectando ao SSID: %s", wifi_config.sta.ssid);

    s_status.connecting = true;
    s_status.connected = false;
    s_status.last_disconnect_reason = WIFI_REASON_UNSPECIFIED;
    strlcpy(s_status.ssid, (const char *)wifi_config.sta.ssid, sizeof(s_status.ssid));

    ESP_ERROR_CHECK(esp_wifi_disconnect());
    return esp_wifi_connect();
}

esp_err_t wifi_manager_disconnect(void)
{
    if (!s_wifi_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    s_status.connecting = false;
    s_status.connected = false;
    s_status.last_disconnect_reason = WIFI_REASON_UNSPECIFIED;
    s_status.ip[0] = '\0';
    return esp_wifi_disconnect();
}

esp_err_t wifi_manager_get_status(wifi_status_info_t *info)
{
    if (!info) {
        return ESP_ERR_INVALID_ARG;
    }
    *info = s_status;
    return ESP_OK;
}

esp_err_t wifi_manager_start_scan(wifi_scan_result_cb_t cb)
{
    esp_err_t err = wifi_manager_ensure_started();
    if (err != ESP_OK) {
        return err;
    }
    wifi_scan_config_t scan_cfg = {
        .ssid = 0,
        .bssid = 0,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    s_scan_callback = cb;
    return esp_wifi_scan_start(&scan_cfg, false);
}
