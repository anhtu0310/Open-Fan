#pragma once
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void){return (void*)1;}
#define xSemaphoreTake(a,b) pdTRUE
#define xSemaphoreGive(a) 0
