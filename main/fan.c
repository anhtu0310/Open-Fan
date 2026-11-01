#include "fan.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "board.h"
#include "sensors.h"

#define TAG "fan"

#define NVS_NS          "fan"
#define NVS_KEY_STATE   "state"
#define NVS_KEY_CONFIG  "config"
#define SAVE_DELAY_US   (3 * 1000000LL)

typedef struct {
    bool     power;
    uint8_t  mode;
    uint8_t  speed;
    uint16_t pwm_hz;
    bool     swing;
} fan_state_t;

static const uint32_t preset_hz[FAN_SPEED_COUNT] = {
    CONFIG_FAN_SPEED1_HZ, CONFIG_FAN_SPEED2_HZ, CONFIG_FAN_SPEED3_HZ, CONFIG_FAN_SPEED4_HZ,
};

static SemaphoreHandle_t lock;
static fan_state_t state = {
    .power = false,
    .mode = FAN_MODE_PRESET,
    .speed = 1,
    .pwm_hz = CONFIG_FAN_SPEED2_HZ,
    .swing = false,
};
static fan_config_t config = {
    .pwm_min_hz = 100,
    .pwm_max_hz = CONFIG_FAN_SPEED4_HZ,
    .auto_off = false,
    .off_temp = 2500,
    .hold_min = 10,
    .auto_on = false,
    .on_temp = 2700,
};

static uint32_t out_hz;
static int64_t timer_deadline_us;
static int64_t state_dirty_us;

static bool auto_tripped;
static uint32_t auto_count_s;
static uint8_t auto_state;
static uint8_t last_event;
static int64_t last_event_us;
static int16_t last_event_temp;

uint32_t fan_preset_hz(uint8_t speed)
{
    if (speed < 1 || speed > FAN_SPEED_COUNT) return 0;
    return preset_hz[speed - 1];
}

static void load_blob(const char *key, void *dst, size_t size)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = 0;
    /* a size mismatch means an older layout: keep the defaults */
    if (nvs_get_blob(h, key, NULL, &len) == ESP_OK && len == size) {
        nvs_get_blob(h, key, dst, &len);
    }
    nvs_close(h);
}

static esp_err_t save_blob(const char *key, const void *src, size_t size)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, key, src, size);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "save %s: %s", key, esp_err_to_name(err));
    return err;
}

/* Must be called with the lock held */
static void apply_locked(void)
{
    uint32_t hz = 0;
    if (state.power) {
        hz = state.mode == FAN_MODE_PWM ? state.pwm_hz : fan_preset_hz(state.speed);
    }
    out_hz = hz;
    board_fan_output(hz, state.swing);
    state_dirty_us = esp_timer_get_time();
}

static void set_power_locked(bool on)
{
    state.power = on;
    if (!on) timer_deadline_us = 0;
}

/* Any manual action cancels a pending auto-resume */
static void manual_locked(void)
{
    auto_tripped = false;
    auto_count_s = 0;
}

static void event_locked(uint8_t evt, int16_t temp)
{
    last_event = evt;
    last_event_us = esp_timer_get_time();
    last_event_temp = temp;
}

static uint16_t clamp_pwm(uint16_t hz)
{
    if (hz < config.pwm_min_hz) return config.pwm_min_hz;
    if (hz > config.pwm_max_hz) return config.pwm_max_hz;
    return hz;
}

void fan_init(void)
{
    lock = xSemaphoreCreateMutex();
    load_blob(NVS_KEY_CONFIG, &config, sizeof(config));
    load_blob(NVS_KEY_STATE, &state, sizeof(state));

    if (state.speed < 1 || state.speed > FAN_SPEED_COUNT) state.speed = 1;
    if (state.mode > FAN_MODE_PWM) state.mode = FAN_MODE_PRESET;
    state.pwm_hz = clamp_pwm(state.pwm_hz);

    xSemaphoreTake(lock, portMAX_DELAY);
    apply_locked();
    state_dirty_us = 0;
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "Restored: power=%d mode=%d speed=%d pwm=%u swing=%d",
             state.power, state.mode, state.speed, state.pwm_hz, state.swing);
}

static bool sensor_selected(const fan_config_t *cfg)
{
    static const uint8_t zero[6] = { 0 };
    return memcmp(cfg->sensor, zero, 6) != 0;
}

/* Temperature rule. The sensor is read before taking the fan lock. */
static void auto_tick(const fan_config_t *cfg)
{
    sensor_info_t info;
    bool fresh = false;
    if (cfg->auto_off && sensor_selected(cfg)) {
        fresh = sensors_get(cfg->sensor, &info) && info.has_reading && info.age_s < SENSOR_STALE_S;
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    uint32_t hold_s = config.hold_min * 60;

    if (!config.auto_off) {
        auto_state = AUTO_DISABLED;
        manual_locked();
    } else if (!sensor_selected(&config)) {
        auto_state = AUTO_NO_SENSOR;
        auto_count_s = 0;
    } else if (!fresh) {
        auto_state = AUTO_STALE;
        auto_count_s = 0;
    } else if (state.power) {
        auto_tripped = false;
        auto_count_s = info.temp < config.off_temp ? auto_count_s + 1 : 0;
        auto_state = auto_count_s ? AUTO_COUNTING : AUTO_ARMED;
        if (auto_count_s >= hold_s) {
            ESP_LOGI(TAG, "Auto off: %d < %d", info.temp, config.off_temp);
            set_power_locked(false);
            apply_locked();
            auto_tripped = true;
            auto_count_s = 0;
            auto_state = AUTO_TRIPPED;
            event_locked(FAN_EVT_AUTO_OFF, info.temp);
        }
    } else if (auto_tripped && config.auto_on) {
        auto_count_s = info.temp >= config.on_temp ? auto_count_s + 1 : 0;
        auto_state = auto_count_s ? AUTO_RESUMING : AUTO_TRIPPED;
        if (auto_count_s >= hold_s) {
            ESP_LOGI(TAG, "Auto on: %d >= %d", info.temp, config.on_temp);
            set_power_locked(true);
            apply_locked();
            auto_tripped = false;
            auto_count_s = 0;
            auto_state = AUTO_ARMED;
            event_locked(FAN_EVT_AUTO_ON, info.temp);
        }
    } else {
        auto_count_s = 0;
        auto_state = auto_tripped ? AUTO_TRIPPED : AUTO_IDLE;
    }
    xSemaphoreGive(lock);
}

void fan_tick(void)
{
    fan_config_t cfg;
    fan_get_config(&cfg);
    auto_tick(&cfg);

    int64_t now = esp_timer_get_time();
    fan_state_t to_save;
    bool save = false;

    xSemaphoreTake(lock, portMAX_DELAY);
    if (timer_deadline_us && now >= timer_deadline_us) {
        ESP_LOGI(TAG, "Timer expired");
        set_power_locked(false);
        manual_locked();
        apply_locked();
        event_locked(FAN_EVT_TIMER, 0);
    }
    if (state_dirty_us && now - state_dirty_us >= SAVE_DELAY_US) {
        to_save = state;
        state_dirty_us = 0;
        save = true;
    }
    xSemaphoreGive(lock);

    if (save) save_blob(NVS_KEY_STATE, &to_save, sizeof(to_save));
}

void fan_get_status(fan_status_t *out)
{
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(lock, portMAX_DELAY);
    out->power = state.power;
    out->mode = state.mode;
    out->speed = state.speed;
    out->pwm_hz = state.pwm_hz;
    out->swing = state.swing;
    out->out_hz = out_hz;
    out->timer_left_s = timer_deadline_us ? (uint32_t)((timer_deadline_us - now + 999999) / 1000000) : 0;
    out->auto_state = auto_state;
    uint32_t hold_s = config.hold_min * 60;
    out->auto_wait_s = auto_count_s && auto_count_s < hold_s ? hold_s - auto_count_s : 0;
    out->last_event = last_event;
    out->last_event_ago_s = last_event ? (uint32_t)((now - last_event_us) / 1000000) : 0;
    out->last_event_temp = last_event_temp;
    xSemaphoreGive(lock);
}

void fan_get_config(fan_config_t *out)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    *out = config;
    xSemaphoreGive(lock);
}

esp_err_t fan_set_config(const fan_config_t *cfg)
{
    if (cfg->pwm_min_hz < CONFIG_FAN_PWM_HZ_FLOOR || cfg->pwm_max_hz > CONFIG_FAN_PWM_HZ_LIMIT ||
        cfg->pwm_max_hz < cfg->pwm_min_hz + 10) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg->hold_min < 1 || cfg->hold_min > 120) return ESP_ERR_INVALID_ARG;
    if (cfg->off_temp < -2000 || cfg->off_temp > 6000 || cfg->on_temp < -2000 || cfg->on_temp > 6000) {
        return ESP_ERR_INVALID_ARG;
    }
    /* resume must sit above the off threshold or the fan would oscillate */
    if (cfg->auto_on && cfg->on_temp < cfg->off_temp + 50) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(lock, portMAX_DELAY);
    if (memcmp(cfg->sensor, config.sensor, 6) != 0 || cfg->off_temp != config.off_temp ||
        cfg->on_temp != config.on_temp) {
        auto_count_s = 0;
    }
    config = *cfg;
    uint16_t hz = clamp_pwm(state.pwm_hz);
    if (hz != state.pwm_hz) {
        state.pwm_hz = hz;
        apply_locked();
    }
    fan_config_t to_save = config;
    xSemaphoreGive(lock);

    return save_blob(NVS_KEY_CONFIG, &to_save, sizeof(to_save));
}

void fan_set_power(bool on)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    manual_locked();
    set_power_locked(on);
    apply_locked();
    xSemaphoreGive(lock);
}

esp_err_t fan_set_speed(uint8_t speed)
{
    if (speed < 1 || speed > FAN_SPEED_COUNT) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(lock, portMAX_DELAY);
    manual_locked();
    state.speed = speed;
    state.mode = FAN_MODE_PRESET;
    set_power_locked(true);
    apply_locked();
    xSemaphoreGive(lock);
    return ESP_OK;
}

esp_err_t fan_set_pwm(uint16_t hz)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    manual_locked();
    state.pwm_hz = clamp_pwm(hz);
    state.mode = FAN_MODE_PWM;
    set_power_locked(true);
    apply_locked();
    xSemaphoreGive(lock);
    return ESP_OK;
}

esp_err_t fan_set_mode(uint8_t mode)
{
    if (mode > FAN_MODE_PWM) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(lock, portMAX_DELAY);
    state.mode = mode;
    apply_locked();
    xSemaphoreGive(lock);
    return ESP_OK;
}

void fan_set_swing(bool on)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    state.swing = on;
    apply_locked();
    xSemaphoreGive(lock);
}

esp_err_t fan_set_timer(uint32_t seconds)
{
    if (seconds > FAN_TIMER_MAX_S) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (seconds) {
        if (!state.power) {
            manual_locked();
            set_power_locked(true);
            apply_locked();
        }
        timer_deadline_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
    } else {
        timer_deadline_us = 0;
    }
    xSemaphoreGive(lock);
    return ESP_OK;
}

void fan_switch_position(uint8_t position)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    manual_locked();
    if (position == 0) {
        set_power_locked(false);
    } else {
        state.mode = FAN_MODE_PRESET;
        state.speed = position;
        set_power_locked(true);
    }
    apply_locked();
    xSemaphoreGive(lock);
}
