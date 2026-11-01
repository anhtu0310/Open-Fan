#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAN_SPEED_COUNT     4
#define FAN_TIMER_MAX_S     (24 * 3600)

typedef enum {
    FAN_MODE_PRESET = 0,    /* the 4 original speeds */
    FAN_MODE_PWM,           /* free frequency from the slider */
} fan_mode_t;

typedef enum {
    AUTO_DISABLED = 0,
    AUTO_NO_SENSOR,         /* enabled but no thermometer selected */
    AUTO_STALE,             /* no recent reading: do nothing */
    AUTO_IDLE,              /* fan is off, nothing to do */
    AUTO_ARMED,             /* fan on, temperature above the threshold */
    AUTO_COUNTING,          /* below the threshold, waiting for the hold time */
    AUTO_TRIPPED,           /* fan was turned off by the rule */
    AUTO_RESUMING,          /* above the resume temperature, waiting for the hold time */
} fan_auto_state_t;

typedef enum {
    FAN_EVT_NONE = 0,
    FAN_EVT_AUTO_OFF,
    FAN_EVT_AUTO_ON,
    FAN_EVT_TIMER,
} fan_event_t;

typedef struct {
    uint16_t pwm_min_hz;    /* slider range */
    uint16_t pwm_max_hz;
    bool     auto_off;
    int16_t  off_temp;      /* 0.01 °C: turn off below this */
    uint8_t  hold_min;      /* condition must hold this long */
    bool     auto_on;
    int16_t  on_temp;       /* 0.01 °C: turn back on at/above this */
    uint8_t  sensor[6];     /* thermometer MAC, all zero = none */
} fan_config_t;

typedef struct {
    bool     power;
    uint8_t  mode;          /* fan_mode_t */
    uint8_t  speed;         /* 1..4 */
    uint16_t pwm_hz;
    bool     swing;
    uint32_t out_hz;        /* frequency driven right now, 0 = stopped */
    uint32_t timer_left_s;  /* 0 = no timer */
    uint8_t  auto_state;    /* fan_auto_state_t */
    uint32_t auto_wait_s;   /* time left before the rule acts */
    uint8_t  last_event;    /* fan_event_t */
    uint32_t last_event_ago_s;
    int16_t  last_event_temp;
} fan_status_t;

void fan_init(void);
void fan_tick(void);                    /* call once per second */

void fan_get_status(fan_status_t *out);
void fan_get_config(fan_config_t *out);
esp_err_t fan_set_config(const fan_config_t *cfg);

void fan_set_power(bool on);
esp_err_t fan_set_speed(uint8_t speed); /* also selects preset mode, turns on */
esp_err_t fan_set_pwm(uint16_t hz);     /* also selects PWM mode, turns on */
esp_err_t fan_set_mode(uint8_t mode);
void fan_set_swing(bool on);
esp_err_t fan_set_timer(uint32_t seconds);  /* 0 cancels, turns the fan on */

void fan_switch_position(uint8_t position); /* physical 4-key switch */

uint32_t fan_preset_hz(uint8_t speed);

#ifdef __cplusplus
}
#endif
