#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_SETUP = 0,      /* no credentials: setup access point only */
    NET_CONNECTING,
    NET_CONNECTED,
    NET_FALLBACK,       /* credentials set but unreachable: AP on, still retrying */
} net_state_t;

typedef struct {
    uint8_t state;      /* net_state_t */
    bool    ap_on;
    char    ssid[33];
    char    ap_ssid[33];
    char    ip[16];
    int8_t  rssi;
} wifi_status_t;

esp_err_t wifi_init(void);
void wifi_get_status(wifi_status_t *out);

/* Stores new credentials; they are used after the next restart */
esp_err_t wifi_save_credentials(const char *ssid, const char *password);
esp_err_t wifi_forget_credentials(void);

/* Blocking scan, returns the number of records written */
int wifi_scan(wifi_ap_record_t *out, int max);

#ifdef __cplusplus
}
#endif
