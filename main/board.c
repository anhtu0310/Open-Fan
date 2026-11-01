#include "board.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define TAG "board"

#define RESET_HOLD_MS   3000

static const gpio_num_t btn_pins[BTN_NUM] = { BTN_1, BTN_2, BTN_3, BTN_4 };

static TaskHandle_t btn_task_handle;
static board_switch_cb_t switch_cb;
static board_reset_cb_t reset_cb;
static uint32_t current_freq;

static void IRAM_ATTR button_isr_handler(void *arg)
{
    gpio_num_t pin = (gpio_num_t)(uintptr_t)arg;
    if (gpio_get_level(pin) != BTN_PRESS) return;

    BaseType_t woken = pdFALSE;
    xTaskNotifyFromISR(btn_task_handle, 1UL << pin, eSetBits, &woken);
    portYIELD_FROM_ISR(woken);
}

static void handle_reset_key(void)
{
    TickType_t start = xTaskGetTickCount();
    while (gpio_get_level(BTN_RESET) == BTN_PRESS) {
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(RESET_HOLD_MS)) {
            ESP_LOGW(TAG, "Reset key held");
            if (reset_cb) reset_cb();
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* Same timing as the original firmware: wait 500 ms for the mechanical
 * switch to settle, then the last pressed key decides the position */
static void button_task(void *arg)
{
    uint32_t pins;
    for (;;) {
        xTaskNotifyWait(0, UINT32_MAX, &pins, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(200));

        if (pins & (1UL << BTN_RESET)) {
            handle_reset_key();
            pins &= ~(1UL << BTN_RESET);
        }

        if (pins) {
            vTaskDelay(pdMS_TO_TICKS(300));
            uint8_t position = 0;
            for (int i = 0; i < BTN_NUM; i++) {
                if (gpio_get_level(btn_pins[i]) == BTN_PRESS) position = i;
            }
            ESP_LOGI(TAG, "Switch position: %d", position);
            if (switch_cb) switch_cb(position);
        }

        /* drop edges collected from contact bounce while we were busy */
        xTaskNotifyStateClear(NULL);
        ulTaskNotifyValueClear(NULL, UINT32_MAX);
    }
}

void board_fan_output(uint32_t freq_hz, bool swing)
{
    if (freq_hz == 0) {
        if (ledc_get_duty(FAN_PWM_MODE, FAN_PWM_CHANNEL) != 0) {
            ESP_ERROR_CHECK(ledc_set_duty(FAN_PWM_MODE, FAN_PWM_CHANNEL, 0));
            ESP_ERROR_CHECK(ledc_update_duty(FAN_PWM_MODE, FAN_PWM_CHANNEL));
        }
        gpio_set_level(FAN_SWING, 0);
        current_freq = 0;
        return;
    }

    if (ledc_get_duty(FAN_PWM_MODE, FAN_PWM_CHANNEL) != FAN_PWM_DUTY) {
        ESP_ERROR_CHECK(ledc_set_duty(FAN_PWM_MODE, FAN_PWM_CHANNEL, FAN_PWM_DUTY));
        ESP_ERROR_CHECK(ledc_update_duty(FAN_PWM_MODE, FAN_PWM_CHANNEL));
    }
    if (freq_hz != current_freq) {
        esp_err_t err = ledc_set_freq(FAN_PWM_MODE, FAN_PWM_TIMER, freq_hz);
        if (err == ESP_OK) {
            current_freq = freq_hz;
            ESP_LOGI(TAG, "Fan: %luHz", (unsigned long)freq_hz);
        } else {
            ESP_LOGE(TAG, "Cannot set %luHz: %s", (unsigned long)freq_hz, esp_err_to_name(err));
        }
    }
    gpio_set_level(FAN_SWING, swing);
}

void board_led_set(bool on)
{
    gpio_set_level(LED_STATUS, on ? LED_ON : LED_OFF);
}

static void board_led_init(void)
{
    gpio_reset_pin(LED_STATUS);
    gpio_set_direction(LED_STATUS, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_STATUS, LED_ON);
}

static void board_button_init(void)
{
    gpio_config_t conf = {
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_ANYEDGE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);

    for (int i = 0; i < BTN_NUM; i++) {
        conf.pin_bit_mask = 1ULL << btn_pins[i];
        gpio_config(&conf);
        gpio_isr_handler_add(btn_pins[i], button_isr_handler, (void *)(uintptr_t)btn_pins[i]);
    }
    conf.pin_bit_mask = 1ULL << BTN_RESET;
    gpio_config(&conf);
    gpio_isr_handler_add(BTN_RESET, button_isr_handler, (void *)(uintptr_t)BTN_RESET);
}

static void board_fan_init(void)
{
    gpio_config_t conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << FAN_BREAK,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&conf);
    gpio_set_level(FAN_BREAK, 1);

    conf.pin_bit_mask = 1ULL << FAN_SWING;
    gpio_config(&conf);
    gpio_set_level(FAN_SWING, 0);

    ledc_timer_config_t timer = {
        .speed_mode = FAN_PWM_MODE,
        .timer_num = FAN_PWM_TIMER,
        .duty_resolution = FAN_PWM_RES,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t channel = {
        .speed_mode = FAN_PWM_MODE,
        .channel = FAN_PWM_CHANNEL,
        .timer_sel = FAN_PWM_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = FAN_SPEED,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = true,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

esp_err_t board_init(board_switch_cb_t on_switch, board_reset_cb_t on_reset)
{
    switch_cb = on_switch;
    reset_cb = on_reset;

    board_led_init();
    board_fan_init();
    if (xTaskCreate(button_task, "buttons", 3072, NULL, 10, &btn_task_handle) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    board_button_init();
    return ESP_OK;
}
