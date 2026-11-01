/*
 * Passive BLE scanner for thermometers running the pvvx ATC_MiThermometer
 * firmware (https://github.com/pvvx/ATC_MiThermometer). Understands the
 * unencrypted "ATC1441", "pvvx custom" and "BTHome v2" advertising formats.
 */
#include "sensors.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

#define TAG "sensors"

#define NVS_NS          "sensors"
#define NVS_KEY_LIST    "list"

#define UUID_ENV_SENSING    0x181A  /* ATC1441 and pvvx custom formats */
#define UUID_BTHOME         0xFCD2

/* Scan 80 ms out of every 320 ms to leave air time for Wi-Fi */
#define SCAN_ITVL       512     /* 0.625 ms units */
#define SCAN_WINDOW     128

typedef struct {
    bool     used;
    bool     saved;
    bool     has_reading;
    uint8_t  mac[6];
    char     name[SENSOR_NAME_LEN];
    int16_t  temp;
    uint16_t hum;
    uint8_t  batt;
    uint16_t batt_mv;
    int8_t   rssi;
    uint8_t  fmt;
    int64_t  last_us;
} slot_t;

typedef struct {
    uint8_t mac[6];
    char    name[SENSOR_NAME_LEN];
} saved_entry_t;

typedef struct {
    bool     has_temp, has_hum, has_batt, has_mv;
    int16_t  temp;
    uint16_t hum;
    uint8_t  batt;
    uint16_t mv;
    uint8_t  fmt;
} reading_t;

static SemaphoreHandle_t lock;
static slot_t slots[SENSOR_MAX];

static int16_t hist_temp[SENSOR_MAX][HISTORY_LEN];
static uint8_t hist_hum[SENSOR_MAX][HISTORY_LEN];
static uint8_t hist_fan[HISTORY_LEN];
static uint16_t hist_head;      /* next write position */
static uint16_t hist_count;
static int64_t hist_last_us;

/* ------------------------------------------------------------------ helpers */

void mac_to_str(const uint8_t mac[6], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool mac_from_str(const char *str, uint8_t mac[6])
{
    unsigned int b[6];
    if (sscanf(str, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) mac[i] = b[i];
    return true;
}

const char *sensor_fmt_name(uint8_t fmt)
{
    switch (fmt) {
    case SENSOR_FMT_ATC1441: return "atc1441";
    case SENSOR_FMT_PVVX:    return "pvvx";
    case SENSOR_FMT_BTHOME:  return "bthome";
    default:                 return "";
    }
}

static void hist_clear_slot(int idx)
{
    for (int i = 0; i < HISTORY_LEN; i++) hist_temp[idx][i] = HIST_TEMP_INVALID;
    memset(hist_hum[idx], HIST_HUM_INVALID, HISTORY_LEN);
}

static int slot_find(const uint8_t mac[6])
{
    for (int i = 0; i < SENSOR_MAX; i++) {
        if (slots[i].used && memcmp(slots[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

/* Finds a free slot, or evicts the unsaved sensor heard from least recently */
static int slot_alloc(const uint8_t mac[6])
{
    int idx = -1;
    for (int i = 0; i < SENSOR_MAX && idx < 0; i++) {
        if (!slots[i].used) idx = i;
    }
    for (int i = 0; i < SENSOR_MAX && idx < 0; i++) {
        if (!slots[i].saved && (idx < 0 || slots[i].last_us < slots[idx].last_us)) idx = i;
    }
    if (idx < 0) return -1;

    memset(&slots[idx], 0, sizeof(slots[idx]));
    slots[idx].used = true;
    memcpy(slots[idx].mac, mac, 6);
    snprintf(slots[idx].name, SENSOR_NAME_LEN, "ATC_%02X%02X%02X", mac[3], mac[4], mac[5]);
    hist_clear_slot(idx);
    return idx;
}

static esp_err_t save_list(void)
{
    saved_entry_t list[SENSOR_MAX];
    int n = 0;
    for (int i = 0; i < SENSOR_MAX; i++) {
        if (slots[i].used && slots[i].saved) {
            memcpy(list[n].mac, slots[i].mac, 6);
            memcpy(list[n].name, slots[i].name, SENSOR_NAME_LEN);
            n++;
        }
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = n ? nvs_set_blob(h, NVS_KEY_LIST, list, n * sizeof(saved_entry_t))
            : nvs_erase_key(h, NVS_KEY_LIST);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void load_list(void)
{
    saved_entry_t list[SENSOR_MAX];
    size_t len = sizeof(list);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, NVS_KEY_LIST, list, &len) == ESP_OK) {
        for (int i = 0; i < (int)(len / sizeof(saved_entry_t)); i++) {
            int idx = slot_alloc(list[i].mac);
            if (idx < 0) break;
            slots[idx].saved = true;
            memcpy(slots[idx].name, list[i].name, SENSOR_NAME_LEN);
            slots[idx].name[SENSOR_NAME_LEN - 1] = '\0';
        }
    }
    nvs_close(h);
}

/* ---------------------------------------------------------------- parsing */

static int16_t rd_s16le(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }
static uint16_t rd_u16le(const uint8_t *p) { return p[0] | p[1] << 8; }
static int16_t rd_s16be(const uint8_t *p) { return (int16_t)(p[0] << 8 | p[1]); }
static uint16_t rd_u16be(const uint8_t *p) { return p[0] << 8 | p[1]; }

/* Data length of BTHome v2 object ids 0x00..0x61, 0 = unknown */
static const uint8_t bthome_len[] = {
    1, 1, 2, 2, 3, 3, 2, 2, 2, 1, 3, 3, 2, 2, 2, 1,     /* 0x00 */
    1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,     /* 0x10 */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,     /* 0x20 */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 2, 2, 4, 2,     /* 0x30 */
    2, 2, 3, 2, 2, 2, 1, 2, 2, 2, 2, 3, 4, 4, 4, 4,     /* 0x40 */
    4, 2, 2, 0, 0, 4, 2, 1, 1, 1, 2, 4, 4, 2, 2, 2,     /* 0x50 */
    1, 2,                                               /* 0x60 */
};

static bool parse_bthome(const uint8_t *d, int len, reading_t *r)
{
    if (len < 1) return false;
    uint8_t info = d[0];
    if ((info & 0x01) || (info >> 5) != 2) return false;   /* encrypted or not v2 */

    r->fmt = SENSOR_FMT_BTHOME;
    for (int i = 1; i < len;) {
        uint8_t id = d[i++];
        int n;
        if (id == 0x53 || id == 0x54) {         /* text / raw: length prefixed */
            if (i >= len) break;
            n = 1 + d[i];
        } else if (id >= 0xF0 && id <= 0xF2) {
            n = id == 0xF0 ? 2 : id == 0xF1 ? 4 : 3;
        } else if (id < sizeof(bthome_len) && bthome_len[id]) {
            n = bthome_len[id];
        } else {
            break;                              /* unknown object: stop */
        }
        if (i + n > len) break;

        const uint8_t *v = &d[i];
        switch (id) {
        case 0x01: r->has_batt = true; r->batt = v[0]; break;
        case 0x02: r->has_temp = true; r->temp = rd_s16le(v); break;
        case 0x45: r->has_temp = true; r->temp = rd_s16le(v) * 10; break;
        case 0x03: r->has_hum = true; r->hum = rd_u16le(v); break;
        case 0x2E: r->has_hum = true; r->hum = v[0] * 100; break;
        case 0x0C: r->has_mv = true; r->mv = rd_u16le(v); break;
        default: break;
        }
        i += n;
    }
    return r->has_temp || r->has_hum || r->has_batt || r->has_mv;
}

static bool parse_service_data(uint16_t uuid, const uint8_t *d, int len, reading_t *r)
{
    if (uuid == UUID_ENV_SENSING && len == 13) {
        /* ATC1441: mac[6] temp(BE, 0.1) hum(%) batt(%) mv(BE) cnt */
        r->fmt = SENSOR_FMT_ATC1441;
        r->has_temp = r->has_hum = r->has_batt = r->has_mv = true;
        r->temp = rd_s16be(&d[6]) * 10;
        r->hum = d[8] * 100;
        r->batt = d[9];
        r->mv = rd_u16be(&d[10]);
        return true;
    }
    if (uuid == UUID_ENV_SENSING && len == 15) {
        /* pvvx custom: mac[6] temp(0.01) hum(0.01) mv batt cnt flags, little endian */
        r->fmt = SENSOR_FMT_PVVX;
        r->has_temp = r->has_hum = r->has_batt = r->has_mv = true;
        r->temp = rd_s16le(&d[6]);
        r->hum = rd_u16le(&d[8]);
        r->mv = rd_u16le(&d[10]);
        r->batt = d[12];
        return true;
    }
    if (uuid == UUID_BTHOME) return parse_bthome(d, len, r);
    return false;
}

static void handle_adv(const uint8_t *addr_le, int8_t rssi, const uint8_t *data, int len)
{
    reading_t r = { 0 };
    const uint8_t *name = NULL;
    int name_len = 0;

    for (int i = 0; i + 1 < len;) {
        int ad_len = data[i];
        if (ad_len == 0 || i + 1 + ad_len > len) break;
        uint8_t type = data[i + 1];
        const uint8_t *p = &data[i + 2];
        int plen = ad_len - 1;

        if (type == 0x16 && plen >= 2) {
            parse_service_data(p[0] | p[1] << 8, p + 2, plen - 2, &r);
        } else if (type == 0x09 || type == 0x08) {
            name = p;
            name_len = plen;
        }
        i += 1 + ad_len;
    }
    if (!r.fmt) return;

    uint8_t mac[6];
    for (int i = 0; i < 6; i++) mac[i] = addr_le[5 - i];

    if (xSemaphoreTake(lock, pdMS_TO_TICKS(20)) != pdTRUE) return;
    int idx = slot_find(mac);
    /* Only temperature packets may claim a slot; BTHome also has buttons etc. */
    if (idx < 0 && r.has_temp) idx = slot_alloc(mac);
    if (idx >= 0) {
        slot_t *s = &slots[idx];
        if (r.has_temp) { s->temp = r.temp; s->has_reading = true; s->last_us = esp_timer_get_time(); }
        if (r.has_hum)  s->hum = r.hum;
        if (r.has_batt) s->batt = r.batt;
        if (r.has_mv)   s->batt_mv = r.mv;
        s->rssi = rssi;
        s->fmt = r.fmt;
        if (name && !s->saved) {
            int n = name_len < SENSOR_NAME_LEN - 1 ? name_len : SENSOR_NAME_LEN - 1;
            memcpy(s->name, name, n);
            s->name[n] = '\0';
        }
    }
    xSemaphoreGive(lock);
}

/* -------------------------------------------------------------------- BLE */

static uint8_t own_addr_type;
static void start_scan(void);

static int gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        handle_adv(event->disc.addr.val, event->disc.rssi, event->disc.data, event->disc.length_data);
        break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        start_scan();
        break;
    default:
        break;
    }
    return 0;
}

static void start_scan(void)
{
    struct ble_gap_disc_params params = {
        .itvl = SCAN_ITVL,
        .window = SCAN_WINDOW,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
        .passive = 1,
        .filter_duplicates = 0,
    };
    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    ESP_LOGI(TAG, "BLE scanning started");
    start_scan();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset: %d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---------------------------------------------------------------- public */

esp_err_t sensors_init(void)
{
    lock = xSemaphoreCreateMutex();
    if (!lock) return ESP_ERR_NO_MEM;

    memset(hist_fan, 0, sizeof(hist_fan));
    load_list();

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

static void slot_to_info(const slot_t *s, sensor_info_t *out, int64_t now)
{
    memcpy(out->mac, s->mac, 6);
    memcpy(out->name, s->name, SENSOR_NAME_LEN);
    out->saved = s->saved;
    out->has_reading = s->has_reading;
    out->temp = s->temp;
    out->hum = s->hum;
    out->batt = s->batt;
    out->batt_mv = s->batt_mv;
    out->rssi = s->rssi;
    out->fmt = s->fmt;
    out->age_s = s->has_reading ? (uint32_t)((now - s->last_us) / 1000000) : UINT32_MAX;
}

int sensors_list(sensor_info_t *out, int max)
{
    int n = 0;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < SENSOR_MAX && n < max; i++) {
        if (slots[i].used) slot_to_info(&slots[i], &out[n++], now);
    }
    xSemaphoreGive(lock);
    return n;
}

bool sensors_get(const uint8_t mac[6], sensor_info_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    int idx = slot_find(mac);
    if (idx >= 0) slot_to_info(&slots[idx], out, esp_timer_get_time());
    xSemaphoreGive(lock);
    return idx >= 0;
}

esp_err_t sensors_rename(const uint8_t mac[6], const char *name)
{
    esp_err_t err = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(lock, portMAX_DELAY);
    int idx = slot_find(mac);
    if (idx >= 0) {
        strlcpy(slots[idx].name, name, SENSOR_NAME_LEN);
        slots[idx].saved = true;
        err = save_list();
    }
    xSemaphoreGive(lock);
    return err;
}

esp_err_t sensors_forget(const uint8_t mac[6])
{
    esp_err_t err = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(lock, portMAX_DELAY);
    int idx = slot_find(mac);
    if (idx >= 0) {
        bool was_saved = slots[idx].saved;
        memset(&slots[idx], 0, sizeof(slots[idx]));
        err = was_saved ? save_list() : ESP_OK;
    }
    xSemaphoreGive(lock);
    return err;
}

void sensors_history_tick(uint32_t fan_hz)
{
    int64_t now = esp_timer_get_time();
    if (hist_last_us && now - hist_last_us < (int64_t)HISTORY_INTERVAL_S * 1000000) return;

    xSemaphoreTake(lock, portMAX_DELAY);
    for (int i = 0; i < SENSOR_MAX; i++) {
        const slot_t *s = &slots[i];
        bool fresh = s->used && s->has_reading &&
                     now - s->last_us < (int64_t)HISTORY_INTERVAL_S * 2 * 1000000;
        hist_temp[i][hist_head] = fresh ? s->temp : HIST_TEMP_INVALID;
        hist_hum[i][hist_head] = fresh ? (uint8_t)((s->hum + 25) / 50) : HIST_HUM_INVALID;
    }
    hist_fan[hist_head] = fan_hz ? (fan_hz / 4 > 254 ? 254 : (fan_hz / 4 ?: 1)) : 0;
    hist_head = (hist_head + 1) % HISTORY_LEN;
    if (hist_count < HISTORY_LEN) hist_count++;
    /* keep a fixed cadence so the browser can rebuild timestamps from it */
    int64_t step = (int64_t)HISTORY_INTERVAL_S * 1000000;
    hist_last_us = (hist_last_us && now - hist_last_us < 2 * step) ? hist_last_us + step : now;
    xSemaphoreGive(lock);
}

int sensors_history(const uint8_t mac[6], history_sample_t *out, int max, uint32_t *age_s)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    int idx = slot_find(mac);
    int n = -1;
    if (idx >= 0) {
        n = hist_count < max ? hist_count : max;
        int start = (hist_head - n + HISTORY_LEN) % HISTORY_LEN;
        for (int i = 0; i < n; i++) {
            int p = (start + i) % HISTORY_LEN;
            out[i].temp = hist_temp[idx][p];
            out[i].hum = hist_hum[idx][p];
            out[i].fan = hist_fan[p];
        }
        *age_s = hist_last_us ? (uint32_t)((esp_timer_get_time() - hist_last_us) / 1000000) : 0;
    }
    xSemaphoreGive(lock);
    return n;
}
