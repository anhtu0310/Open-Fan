#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_FAN_BOARD_ESP32C3_FAN_V1
/* Pin map of the ESP32C3-Dev-Fan-V1 board, unchanged from the original firmware */
#define BTN_NUM         4
#define BTN_1           GPIO_NUM_0
#define BTN_2           GPIO_NUM_1
#define BTN_3           GPIO_NUM_3
#define BTN_4           GPIO_NUM_4
#define BTN_RESET       GPIO_NUM_9
#define LED_STATUS      GPIO_NUM_8
#define FAN_BREAK       GPIO_NUM_6
#define FAN_SPEED       GPIO_NUM_7
#define FAN_SWING       GPIO_NUM_10
#define FAN_DATA        GPIO_NUM_5      /* not used */

#define FAN_PWM_RES     LEDC_TIMER_10_BIT
#define FAN_PWM_CHANNEL LEDC_CHANNEL_0
#define FAN_PWM_TIMER   LEDC_TIMER_0
#define FAN_PWM_MODE    LEDC_LOW_SPEED_MODE
#endif

#define FAN_PWM_DUTY    CONFIG_FAN_PWM_DUTY

#define BTN_PRESS       0
#define LED_ON          0
#define LED_OFF         1

/* Called from the button task with the position of the 4-key switch:
 * 0 = off (key 1), 1..3 = speed (keys 2..4) */
typedef void (*board_switch_cb_t)(uint8_t position);
/* Called when the reset key is held for 3 seconds */
typedef void (*board_reset_cb_t)(void);

esp_err_t board_init(board_switch_cb_t switch_cb, board_reset_cb_t reset_cb);

/* Drive the fan: freq_hz == 0 stops the fan and the swing motor */
void board_fan_output(uint32_t freq_hz, bool swing);

void board_led_set(bool on);

#ifdef __cplusplus
}
#endif
