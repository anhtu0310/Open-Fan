#pragma once
#include <stdio.h>
#define ESP_LOGI(t,f,...) printf("I %s: " f "\n",t,##__VA_ARGS__)
#define ESP_LOGW ESP_LOGI
#define ESP_LOGE ESP_LOGI
