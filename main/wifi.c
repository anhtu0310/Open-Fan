#include "wifi.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs.h"

#define TAG "wifi"

#define NVS_NS          "wifi"
#define RETRY_MIN_MS    1000
#define RETRY_MAX_MS    30000

static portMUX_TYPE status_mux = portMUX_INITIALIZER_UNLOCKED;
static wifi_status_t status;

static esp_netif_t *sta_netif;
static wifi_config_t ap_config;     /* can only be applied while in APSTA mode */
static esp_timer_handle_t retry_timer;
static esp_timer_handle_t fallback_timer;
static uint32_t retry_ms = RETRY_MIN_MS;
static bool have_credentials;

static void set_state(net_state_t s)
{
    portENTER_CRITICAL(&status_mux);
    status.state = s;
    portEXIT_CRITICAL(&status_mux);
}

static void set_ap(bool on)
{
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    wifi_mode_t want = on ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    if (mode != want) {
        ESP_LOGI(TAG, "Setup AP %s", on ? "on" : "off");
        esp_wifi_set_mode(want);
        if (on) esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    }
    portENTER_CRITICAL(&status_mux);
    status.ap_on = on;
    portEXIT_CRITICAL(&status_mux);
}

static void retry_cb(void *arg)
{
    esp_wifi_connect();
}

static void fallback_cb(void *arg)
{
    if (status.state != NET_CONNECTED) {
        ESP_LOGW(TAG, "No connection, enabling setup AP");
        set_state(NET_FALLBACK);
        set_ap(true);
    }
}

#if CONFIG_FAN_STATIC_IP
/* set_ip_info posts IP_EVENT_STA_GOT_IP, so the connected path below stays the same */
static void set_static_ip(void)
{
    esp_netif_ip_info_t info = { 0 };
    if (esp_netif_str_to_ip4(CONFIG_FAN_STATIC_IP_ADDR, &info.ip) != ESP_OK ||
        esp_netif_str_to_ip4(CONFIG_FAN_STATIC_NETMASK, &info.netmask) != ESP_OK ||
        esp_netif_str_to_ip4(CONFIG_FAN_STATIC_GW, &info.gw) != ESP_OK) {
        ESP_LOGE(TAG, "Invalid static IP config, using DHCP");
        return;
    }
    esp_err_t err = esp_netif_dhcpc_stop(sta_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "DHCP stop failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_netif_set_ip_info(sta_netif, &info);
    if (err != ESP_OK) ESP_LOGE(TAG, "Static IP failed: %s", esp_err_to_name(err));
}
#endif

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (have_credentials) esp_wifi_connect();
#if CONFIG_FAN_STATIC_IP
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        set_static_ip();
#endif
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = data;
        if (status.state == NET_CONNECTED) {
            ESP_LOGW(TAG, "Disconnected, reason %d", e->reason);
            set_state(NET_CONNECTING);
            esp_timer_stop(fallback_timer);
            esp_timer_start_once(fallback_timer, (uint64_t)CONFIG_FAN_STA_FALLBACK_S * 1000000);
        }
        portENTER_CRITICAL(&status_mux);
        status.ip[0] = '\0';
        portEXIT_CRITICAL(&status_mux);
        if (have_credentials) {
            /* back off; scanning for the router disturbs clients of the setup AP */
            uint32_t delay = status.ap_on ? RETRY_MAX_MS : retry_ms;
            esp_timer_stop(retry_timer);
            esp_timer_start_once(retry_timer, (uint64_t)delay * 1000);
            retry_ms = retry_ms * 2 > RETRY_MAX_MS ? RETRY_MAX_MS : retry_ms * 2;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        portENTER_CRITICAL(&status_mux);
        esp_ip4addr_ntoa(&e->ip_info.ip, status.ip, sizeof(status.ip));
        portEXIT_CRITICAL(&status_mux);
        ESP_LOGI(TAG, "Connected, IP %s", status.ip);
        retry_ms = RETRY_MIN_MS;
        esp_timer_stop(fallback_timer);
        set_state(NET_CONNECTED);
        set_ap(false);
    }
}

static bool load_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_str(h, "ssid", ssid, &ssid_len) == ESP_OK && ssid[0];
        if (ok && nvs_get_str(h, "pass", pass, &pass_len) != ESP_OK) pass[0] = '\0';
        nvs_close(h);
    }
    if (!ok && CONFIG_FAN_WIFI_SSID[0]) {
        strlcpy(ssid, CONFIG_FAN_WIFI_SSID, ssid_len);
        strlcpy(pass, CONFIG_FAN_WIFI_PASSWORD, pass_len);
        ok = true;
    }
    return ok;
}

static void start_mdns(void)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed");
        return;
    }
    mdns_hostname_set(CONFIG_FAN_HOSTNAME);
    mdns_instance_name_set("Open Fan");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

esp_err_t wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(sta_netif, CONFIG_FAN_HOSTNAME);

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));

    esp_timer_create_args_t targs = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &retry_timer));
    targs.callback = fallback_cb;
    targs.name = "wifi_fallback";
    ESP_ERROR_CHECK(esp_timer_create(&targs, &fallback_timer));

    /* setup access point */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t *ap = &ap_config;
    snprintf((char *)ap->ap.ssid, sizeof(ap->ap.ssid), "OpenFan-%02X%02X", mac[4], mac[5]);
    ap->ap.ssid_len = strlen((char *)ap->ap.ssid);
    strlcpy((char *)ap->ap.password, CONFIG_FAN_AP_PASSWORD, sizeof(ap->ap.password));
    ap->ap.authmode = strlen(CONFIG_FAN_AP_PASSWORD) >= 8 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ap->ap.channel = 1;
    ap->ap.max_connection = 4;
    strlcpy(status.ap_ssid, (char *)ap->ap.ssid, sizeof(status.ap_ssid));

    wifi_config_t sta = { 0 };
    have_credentials = load_credentials((char *)sta.sta.ssid, sizeof(sta.sta.ssid),
                                        (char *)sta.sta.password, sizeof(sta.sta.password));
    strlcpy(status.ssid, (char *)sta.sta.ssid, sizeof(status.ssid));

    if (have_credentials) {
        ESP_LOGI(TAG, "Connecting to \"%s\"", status.ssid);
        sta.sta.threshold.authmode = sta.sta.password[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
        status.state = NET_CONNECTING;
        esp_timer_start_once(fallback_timer, (uint64_t)CONFIG_FAN_STA_FALLBACK_S * 1000000);
    } else {
        ESP_LOGW(TAG, "No Wi-Fi credentials, starting setup AP \"%s\"", status.ap_ssid);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, ap));
        status.state = NET_SETUP;
        status.ap_on = true;
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    start_mdns();
    return ESP_OK;
}

void wifi_get_status(wifi_status_t *out)
{
    portENTER_CRITICAL(&status_mux);
    *out = status;
    portEXIT_CRITICAL(&status_mux);

    wifi_ap_record_t ap;
    out->rssi = (out->state == NET_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}

esp_err_t wifi_save_credentials(const char *ssid, const char *password)
{
    if (!ssid[0] || strlen(ssid) > 32 || strlen(password) > 64) return ESP_ERR_INVALID_ARG;
    if (password[0] && strlen(password) < 8) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", password);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t wifi_forget_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

int wifi_scan(wifi_ap_record_t *out, int max)
{
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Scan failed: %s", esp_err_to_name(err));
        return 0;
    }
    uint16_t n = max;
    if (esp_wifi_scan_get_ap_records(&n, out) != ESP_OK) return 0;
    return n;
}
