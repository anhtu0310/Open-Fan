/*
 * HTTP server: the single page UI (gzipped at build time) and a small REST API.
 * Request bodies are application/x-www-form-urlencoded, responses are JSON.
 *
 *   GET  /                 web UI
 *   GET  /api/state        fan, settings, thermometers, system
 *   POST /api/control      power, speed, mode, pwm, swing, timer
 *   POST /api/config       pwm_min, pwm_max, ao, ao_temp, hold, ar, ar_temp, sensor
 *   POST /api/sensor       mac, name | forget
 *   GET  /api/history      ?mac=AA:BB:CC:DD:EE:FF
 *   GET  /api/wifi/scan
 *   POST /api/wifi         ssid, pass | forget
 *   POST /api/ota          raw firmware image
 *   POST /api/reboot
 */
#include "web.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "fan.h"
#include "sensors.h"
#include "wifi.h"

#define TAG "web"

#define BODY_MAX        512
#define SCAN_MAX        20
#define OTA_BUF_SIZE    2048

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");

static const char *const auto_names[] = {
    "disabled", "nosensor", "stale", "idle", "armed", "counting", "tripped", "resuming",
};
static const char *const event_names[] = { "", "auto_off", "auto_on", "timer" };
static const char *const net_names[] = { "setup", "connecting", "connected", "fallback" };

/* ------------------------------------------------------------- JSON buffer */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    bool   oom;
} jbuf_t;

static void jb_printf(jbuf_t *jb, const char *fmt, ...)
{
    if (jb->oom) return;
    for (;;) {
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(jb->buf + jb->len, jb->cap - jb->len, fmt, ap);
        va_end(ap);
        if (n < 0) { jb->oom = true; return; }
        if (jb->len + n < jb->cap) { jb->len += n; return; }
        size_t cap = (jb->cap + n) * 2;
        char *p = realloc(jb->buf, cap);
        if (!p) { jb->oom = true; return; }
        jb->buf = p;
        jb->cap = cap;
    }
}

static void jb_str(jbuf_t *jb, const char *s)
{
    jb_printf(jb, "\"");
    for (; *s; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') jb_printf(jb, "\\%c", c);
        else if (c < 0x20) jb_printf(jb, "\\u%04x", c);
        else jb_printf(jb, "%c", c);
    }
    jb_printf(jb, "\"");
}

static esp_err_t jb_send(httpd_req_t *req, jbuf_t *jb)
{
    esp_err_t err;
    if (jb->oom) {
        err = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    } else {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        err = httpd_resp_send(req, jb->buf, jb->len);
    }
    free(jb->buf);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *msg)
{
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg);
}

/* ------------------------------------------------------------ form parsing */

static int read_body(httpd_req_t *req, char *buf, size_t size)
{
    if (req->content_len >= size) return -1;
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return -1;
        got += r;
    }
    buf[got] = '\0';
    return got;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s)
{
    char *out = s;
    for (; *s; s++) {
        if (*s == '+') {
            *out++ = ' ';
        } else if (*s == '%' && hex_val(s[1]) >= 0 && hex_val(s[2]) >= 0) {
            *out++ = (char)(hex_val(s[1]) << 4 | hex_val(s[2]));
            s += 2;
        } else {
            *out++ = *s;
        }
    }
    *out = '\0';
}

static bool form_get(const char *body, const char *key, char *out, size_t size)
{
    if (httpd_query_key_value(body, key, out, size) != ESP_OK) return false;
    url_decode(out);
    return true;
}

static bool form_int(const char *body, const char *key, long *out)
{
    char v[16];
    if (!form_get(body, key, v, sizeof(v))) return false;
    char *end;
    *out = strtol(v, &end, 10);
    return end != v;
}

/* temperature in °C ("24.5") to 0.01 °C */
static bool form_temp(const char *body, const char *key, int16_t *out)
{
    char v[16];
    if (!form_get(body, key, v, sizeof(v))) return false;
    char *end;
    float f = strtof(v, &end);
    if (end == v || f < -50 || f > 80) return false;
    *out = (int16_t)(f * 100 + (f < 0 ? -0.5f : 0.5f));
    return true;
}

static void restart_cb(void *arg)
{
    esp_restart();
}

static void schedule_restart(void)
{
    static esp_timer_handle_t t;
    esp_timer_create_args_t args = { .callback = restart_cb, .name = "restart" };
    if (!t && esp_timer_create(&args, &t) != ESP_OK) return;
    esp_timer_start_once(t, 1000 * 1000);
}

/* ------------------------------------------------------------- state JSON */

static void build_state(jbuf_t *jb)
{
    fan_status_t st;
    fan_config_t cfg;
    wifi_status_t net;
    fan_get_status(&st);
    fan_get_config(&cfg);
    wifi_get_status(&net);

    jb_printf(jb, "{\"power\":%s,\"mode\":\"%s\",\"speed\":%u,\"pwm\":%u,\"swing\":%s,"
                  "\"hz\":%lu,\"timer\":%lu,",
              st.power ? "true" : "false", st.mode == FAN_MODE_PWM ? "pwm" : "preset",
              st.speed, st.pwm_hz, st.swing ? "true" : "false",
              (unsigned long)st.out_hz, (unsigned long)st.timer_left_s);

    char sensor_mac[18] = "";
    static const uint8_t zero[6] = { 0 };
    if (memcmp(cfg.sensor, zero, 6) != 0) mac_to_str(cfg.sensor, sensor_mac);

    jb_printf(jb, "\"cfg\":{\"pwm_min\":%u,\"pwm_max\":%u,\"pwm_floor\":%d,\"pwm_limit\":%d,"
                  "\"presets\":[%lu,%lu,%lu,%lu],\"ao\":%s,\"ao_temp\":%.2f,\"hold\":%u,"
                  "\"ar\":%s,\"ar_temp\":%.2f,\"sensor\":\"%s\"},",
              cfg.pwm_min_hz, cfg.pwm_max_hz, CONFIG_FAN_PWM_HZ_FLOOR, CONFIG_FAN_PWM_HZ_LIMIT,
              (unsigned long)fan_preset_hz(1), (unsigned long)fan_preset_hz(2),
              (unsigned long)fan_preset_hz(3), (unsigned long)fan_preset_hz(4),
              cfg.auto_off ? "true" : "false", cfg.off_temp / 100.0, cfg.hold_min,
              cfg.auto_on ? "true" : "false", cfg.on_temp / 100.0, sensor_mac);

    jb_printf(jb, "\"auto\":{\"state\":\"%s\",\"wait\":%lu,\"event\":\"%s\",\"event_ago\":%lu,"
                  "\"event_temp\":%.2f},",
              auto_names[st.auto_state], (unsigned long)st.auto_wait_s, event_names[st.last_event],
              (unsigned long)st.last_event_ago_s, st.last_event_temp / 100.0);

    sensor_info_t *list = malloc(sizeof(sensor_info_t) * SENSOR_MAX);
    int n = list ? sensors_list(list, SENSOR_MAX) : 0;
    jb_printf(jb, "\"sensors\":[");
    for (int i = 0; i < n; i++) {
        const sensor_info_t *s = &list[i];
        char mac[18];
        mac_to_str(s->mac, mac);
        jb_printf(jb, "%s{\"mac\":\"%s\",\"name\":", i ? "," : "", mac);
        jb_str(jb, s->name);
        jb_printf(jb, ",\"saved\":%s,\"fmt\":\"%s\",\"rssi\":%d",
                  s->saved ? "true" : "false", sensor_fmt_name(s->fmt), s->rssi);
        if (s->has_reading) {
            jb_printf(jb, ",\"t\":%.2f,\"h\":%.1f,\"b\":%u,\"mv\":%u,\"age\":%lu",
                      s->temp / 100.0, s->hum / 100.0, s->batt, s->batt_mv, (unsigned long)s->age_s);
        }
        jb_printf(jb, "}");
    }
    free(list);
    jb_printf(jb, "],\"hist_interval\":%d,", HISTORY_INTERVAL_S);

    const esp_app_desc_t *app = esp_app_get_description();
    jb_printf(jb, "\"sys\":{\"ver\":\"%s\",\"idf\":\"%s\",\"up\":%lu,\"heap\":%lu,\"heap_min\":%lu,"
                  "\"net\":{\"state\":\"%s\",\"ap\":%s,\"ssid\":",
              app->version, app->idf_ver, (unsigned long)(esp_timer_get_time() / 1000000),
              (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size(),
              net_names[net.state], net.ap_on ? "true" : "false");
    jb_str(jb, net.ssid);
    jb_printf(jb, ",\"ap_ssid\":");
    jb_str(jb, net.ap_ssid);
    jb_printf(jb, ",\"ip\":\"%s\",\"rssi\":%d,\"host\":\"%s\"}}}",
              net.ip, net.rssi, CONFIG_FAN_HOSTNAME);
}

static esp_err_t send_state(httpd_req_t *req)
{
    jbuf_t jb = { 0 };
    build_state(&jb);
    return jb_send(req, &jb);
}

/* --------------------------------------------------------------- handlers */

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, (const char *)index_html_gz_start,
                           index_html_gz_end - index_html_gz_start);
}

static esp_err_t state_get(httpd_req_t *req)
{
    return send_state(req);
}

static esp_err_t control_post(httpd_req_t *req)
{
    char body[BODY_MAX];
    if (read_body(req, body, sizeof(body)) < 0) return send_error(req, "bad body");

    char v[16];
    long n;
    if (form_get(body, "mode", v, sizeof(v))) {
        if (fan_set_mode(strcmp(v, "pwm") == 0 ? FAN_MODE_PWM : FAN_MODE_PRESET) != ESP_OK) {
            return send_error(req, "bad mode");
        }
    }
    if (form_int(body, "speed", &n) && fan_set_speed(n) != ESP_OK) return send_error(req, "bad speed");
    if (form_int(body, "pwm", &n)) {
        if (n < 0 || n > UINT16_MAX) return send_error(req, "bad pwm");
        fan_set_pwm(n);
    }
    if (form_get(body, "power", v, sizeof(v))) {
        if (strcmp(v, "toggle") == 0) {
            fan_status_t st;
            fan_get_status(&st);
            fan_set_power(!st.power);
        } else {
            fan_set_power(atoi(v) != 0);
        }
    }
    if (form_int(body, "swing", &n)) fan_set_swing(n != 0);
    if (form_int(body, "timer", &n)) {
        if (n < 0 || fan_set_timer(n) != ESP_OK) return send_error(req, "bad timer");
    }
    return send_state(req);
}

static esp_err_t config_post(httpd_req_t *req)
{
    char body[BODY_MAX];
    if (read_body(req, body, sizeof(body)) < 0) return send_error(req, "bad body");

    fan_config_t cfg;
    fan_get_config(&cfg);
    long n;
    char v[24];
    if (form_int(body, "pwm_min", &n)) cfg.pwm_min_hz = n < 0 ? 0 : n > UINT16_MAX ? UINT16_MAX : n;
    if (form_int(body, "pwm_max", &n)) cfg.pwm_max_hz = n < 0 ? 0 : n > UINT16_MAX ? UINT16_MAX : n;
    if (form_int(body, "ao", &n)) cfg.auto_off = n != 0;
    if (form_int(body, "ar", &n)) cfg.auto_on = n != 0;
    if (form_int(body, "hold", &n)) cfg.hold_min = n < 0 ? 0 : n > 255 ? 255 : n;
    if (form_get(body, "ao_temp", v, sizeof(v)) && !form_temp(body, "ao_temp", &cfg.off_temp)) {
        return send_error(req, "bad temperature");
    }
    if (form_get(body, "ar_temp", v, sizeof(v)) && !form_temp(body, "ar_temp", &cfg.on_temp)) {
        return send_error(req, "bad temperature");
    }
    if (form_get(body, "sensor", v, sizeof(v))) {
        if (!v[0]) memset(cfg.sensor, 0, 6);
        else if (!mac_from_str(v, cfg.sensor)) return send_error(req, "bad sensor");
    }
    if (fan_set_config(&cfg) != ESP_OK) {
        return send_error(req, "invalid settings (check slider range, hold time and resume temperature)");
    }
    return send_state(req);
}

static esp_err_t sensor_post(httpd_req_t *req)
{
    char body[BODY_MAX];
    if (read_body(req, body, sizeof(body)) < 0) return send_error(req, "bad body");

    char v[SENSOR_NAME_LEN * 3];
    uint8_t mac[6];
    if (!form_get(body, "mac", v, sizeof(v)) || !mac_from_str(v, mac)) return send_error(req, "bad mac");

    esp_err_t err = ESP_OK;
    long forget;
    if (form_int(body, "forget", &forget) && forget) {
        err = sensors_forget(mac);
    } else if (form_get(body, "name", v, sizeof(v))) {
        if (!v[0] || strlen(v) >= SENSOR_NAME_LEN) return send_error(req, "name must be 1-23 bytes");
        err = sensors_rename(mac, v);
    }
    if (err != ESP_OK) return send_error(req, esp_err_to_name(err));
    return send_state(req);
}

static esp_err_t history_get(httpd_req_t *req)
{
    char query[64], v[24];
    uint8_t mac[6];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        !form_get(query, "mac", v, sizeof(v)) || !mac_from_str(v, mac)) {
        return send_error(req, "bad mac");
    }

    history_sample_t *hist = malloc(sizeof(history_sample_t) * HISTORY_LEN);
    if (!hist) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    uint32_t age = 0;
    int n = sensors_history(mac, hist, HISTORY_LEN, &age);
    if (n < 0) {
        free(hist);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown sensor");
    }

    /* temperatures in 0.01 °C, humidity in 0.5 %, fan in Hz / 4 */
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char chunk[1024];
    int len = snprintf(chunk, sizeof(chunk), "{\"interval\":%d,\"age\":%lu",
                       HISTORY_INTERVAL_S, (unsigned long)age);
    for (int field = 0; field < 3; field++) {
        len += snprintf(chunk + len, sizeof(chunk) - len, ",\"%c\":[", "thf"[field]);
        for (int i = 0; i < n; i++) {
            const char *sep = i ? "," : "";
            if (field == 0 && hist[i].temp == HIST_TEMP_INVALID) {
                len += snprintf(chunk + len, sizeof(chunk) - len, "%snull", sep);
            } else if (field == 1 && hist[i].hum == HIST_HUM_INVALID) {
                len += snprintf(chunk + len, sizeof(chunk) - len, "%snull", sep);
            } else {
                int val = field == 0 ? hist[i].temp : field == 1 ? hist[i].hum : hist[i].fan;
                len += snprintf(chunk + len, sizeof(chunk) - len, "%s%d", sep, val);
            }
            if (len > (int)sizeof(chunk) - 32) {
                if (httpd_resp_send_chunk(req, chunk, len) != ESP_OK) goto fail;
                len = 0;
            }
        }
        len += snprintf(chunk + len, sizeof(chunk) - len, "]");
    }
    len += snprintf(chunk + len, sizeof(chunk) - len, "}");
    if (httpd_resp_send_chunk(req, chunk, len) != ESP_OK) goto fail;
    free(hist);
    return httpd_resp_send_chunk(req, NULL, 0);

fail:
    free(hist);
    return ESP_FAIL;
}

static esp_err_t wifi_scan_get(httpd_req_t *req)
{
    wifi_ap_record_t *recs = malloc(sizeof(wifi_ap_record_t) * SCAN_MAX);
    if (!recs) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    int n = wifi_scan(recs, SCAN_MAX);

    jbuf_t jb = { 0 };
    jb_printf(&jb, "[");
    for (int i = 0; i < n; i++) {
        jb_printf(&jb, "%s{\"ssid\":", i ? "," : "");
        jb_str(&jb, (const char *)recs[i].ssid);
        jb_printf(&jb, ",\"rssi\":%d,\"secure\":%s}", recs[i].rssi,
                  recs[i].authmode == WIFI_AUTH_OPEN ? "false" : "true");
    }
    jb_printf(&jb, "]");
    free(recs);
    return jb_send(req, &jb);
}

static esp_err_t wifi_post(httpd_req_t *req)
{
    char body[BODY_MAX];
    if (read_body(req, body, sizeof(body)) < 0) return send_error(req, "bad body");

    long forget;
    esp_err_t err;
    if (form_int(body, "forget", &forget) && forget) {
        err = wifi_forget_credentials();
    } else {
        char ssid[33 * 3] = "", pass[65 * 3] = "";
        form_get(body, "ssid", ssid, sizeof(ssid));
        form_get(body, "pass", pass, sizeof(pass));
        err = wifi_save_credentials(ssid, pass);
    }
    if (err != ESP_OK) return send_error(req, "invalid Wi-Fi settings (password: 8-64 characters or empty)");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restart\":true}");
    schedule_restart();
    return ESP_OK;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return send_error(req, "no OTA partition");
    if (req->content_len == 0 || req->content_len > part->size) return send_error(req, "bad image size");

    esp_ota_handle_t ota;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) return send_error(req, esp_err_to_name(err));

    char *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        esp_ota_abort(ota);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    ESP_LOGI(TAG, "OTA: %u bytes to %s", (unsigned)req->content_len, part->label);

    size_t remaining = req->content_len;
    int timeouts = 0;
    while (remaining > 0) {
        int r = httpd_req_recv(req, buf, remaining < OTA_BUF_SIZE ? remaining : OTA_BUF_SIZE);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) continue;
        if (r <= 0) { err = ESP_FAIL; break; }
        timeouts = 0;
        err = esp_ota_write(ota, buf, r);
        if (err != ESP_OK) break;
        remaining -= r;
    }
    free(buf);

    if (err != ESP_OK) {
        esp_ota_abort(ota);
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        return send_error(req, err == ESP_FAIL ? "upload interrupted" : "not a valid firmware image");
    }
    err = esp_ota_end(ota);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA end: %s", esp_err_to_name(err));
        return send_error(req, "image verification failed");
    }

    ESP_LOGI(TAG, "OTA done, restarting");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restart\":true}");
    schedule_restart();
    return ESP_OK;
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restart\":true}");
    schedule_restart();
    return ESP_OK;
}

/* Unknown paths go to the UI, which helps phones on the setup AP */
static esp_err_t not_found(httpd_req_t *req, httpd_err_code_t code)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t web_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.max_uri_handlers = 12;
    config.lru_purge_enable = true;

    httpd_handle_t server;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) return err;

    static const httpd_uri_t uris[] = {
        { .uri = "/",              .method = HTTP_GET,  .handler = index_get },
        { .uri = "/api/state",     .method = HTTP_GET,  .handler = state_get },
        { .uri = "/api/control",   .method = HTTP_POST, .handler = control_post },
        { .uri = "/api/config",    .method = HTTP_POST, .handler = config_post },
        { .uri = "/api/sensor",    .method = HTTP_POST, .handler = sensor_post },
        { .uri = "/api/history",   .method = HTTP_GET,  .handler = history_get },
        { .uri = "/api/wifi/scan", .method = HTTP_GET,  .handler = wifi_scan_get },
        { .uri = "/api/wifi",      .method = HTTP_POST, .handler = wifi_post },
        { .uri = "/api/ota",       .method = HTTP_POST, .handler = ota_post },
        { .uri = "/api/reboot",    .method = HTTP_POST, .handler = reboot_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found);
    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}
