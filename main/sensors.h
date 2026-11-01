#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SENSOR_MAX          CONFIG_FAN_SENSOR_MAX
#define SENSOR_NAME_LEN     24
#define SENSOR_STALE_S      CONFIG_FAN_SENSOR_STALE_S
#define HISTORY_INTERVAL_S  CONFIG_FAN_HISTORY_INTERVAL_S
#define HISTORY_LEN         (CONFIG_FAN_HISTORY_HOURS * 3600 / CONFIG_FAN_HISTORY_INTERVAL_S)

#define HIST_TEMP_INVALID   INT16_MIN
#define HIST_HUM_INVALID    0xFF

typedef enum {
    SENSOR_FMT_NONE = 0,
    SENSOR_FMT_ATC1441,
    SENSOR_FMT_PVVX,
    SENSOR_FMT_BTHOME,
} sensor_fmt_t;

typedef struct {
    uint8_t  mac[6];                /* display order, MSB first */
    char     name[SENSOR_NAME_LEN];
    bool     saved;                 /* named by the user, kept in NVS */
    bool     has_reading;
    int16_t  temp;                  /* 0.01 °C */
    uint16_t hum;                   /* 0.01 % */
    uint8_t  batt;                  /* % */
    uint16_t batt_mv;
    int8_t   rssi;
    uint8_t  fmt;                   /* sensor_fmt_t */
    uint32_t age_s;                 /* seconds since the last reading */
} sensor_info_t;

/* One history sample: temperature in 0.01 °C, humidity in 0.5 % steps,
 * fan output frequency / 4 (0 = off) */
typedef struct {
    int16_t temp;
    uint8_t hum;
    uint8_t fan;
} history_sample_t;

esp_err_t sensors_init(void);

int  sensors_list(sensor_info_t *out, int max);
bool sensors_get(const uint8_t mac[6], sensor_info_t *out);
esp_err_t sensors_rename(const uint8_t mac[6], const char *name);
esp_err_t sensors_forget(const uint8_t mac[6]);

/* Call once per second; records a sample every HISTORY_INTERVAL_S */
void sensors_history_tick(uint32_t fan_hz);

/* Copies up to max samples, oldest first. *age_s is the age of the newest one.
 * Returns the number of samples or -1 when the sensor is unknown. */
int sensors_history(const uint8_t mac[6], history_sample_t *out, int max, uint32_t *age_s);

const char *sensor_fmt_name(uint8_t fmt);
void mac_to_str(const uint8_t mac[6], char out[18]);
bool mac_from_str(const char *str, uint8_t mac[6]);

#ifdef __cplusplus
}
#endif
