/*
 * Open firmware for the ESP32C3-Dev-Fan-V1 board.
 *
 * Local Wi-Fi + web UI, 4 preset speeds and a free PWM (frequency) mode,
 * off timer, and BLE thermometers (pvvx ATC_MiThermometer) for history
 * graphs and automatic switch-off.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "board.h"
#include "fan.h"
#include "sensors.h"
#include "web.h"
#include "wifi.h"

#define TAG "main"

static volatile bool fan_ready;

static void on_switch(uint8_t position)
{
    if (fan_ready) fan_switch_position(position);
}

static void on_reset_key(void)
{
    ESP_LOGW(TAG, "Clearing Wi-Fi settings");
    for (int i = 0; i < 10; i++) {
        board_led_set(i & 1);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    wifi_forget_credentials();
    esp_restart();
}

/* LED: blinking = setup AP active, on = connecting, off = connected */
static void update_led(uint32_t tick)
{
    wifi_status_t net;
    wifi_get_status(&net);
    switch (net.state) {
    case NET_CONNECTED:  board_led_set(false); break;
    case NET_CONNECTING: board_led_set(true); break;
    default:             board_led_set(tick & 1); break;
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(board_init(on_switch, on_reset_key));
    fan_init();
    fan_ready = true;

    if (sensors_init() != ESP_OK) ESP_LOGE(TAG, "BLE thermometers unavailable");
    ESP_ERROR_CHECK(wifi_init());
    ESP_ERROR_CHECK(web_start());

    /* 2 Hz loop: LED blink; fan rules and history once per second */
    for (uint32_t tick = 0;; tick++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        update_led(tick);
        if (tick & 1) continue;

        fan_tick();
        fan_status_t st;
        fan_get_status(&st);
        sensors_history_tick(st.out_hz);
    }
}
