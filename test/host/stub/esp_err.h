#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NVS_NOT_FOUND 0x1102
static inline const char *esp_err_to_name(esp_err_t e){return "err";}
#define ESP_ERR_INVALID_ARG 0x102
