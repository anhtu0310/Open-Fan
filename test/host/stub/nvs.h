#pragma once
#include <stddef.h>
typedef int nvs_handle_t;
#define NVS_READWRITE 1
#define NVS_READONLY 0
static inline esp_err_t nvs_open(const char*n,int m,nvs_handle_t*h){return 0;}
static inline esp_err_t nvs_set_blob(nvs_handle_t h,const char*k,const void*v,size_t l){return 0;}
static inline esp_err_t nvs_get_blob(nvs_handle_t h,const char*k,void*v,size_t*l){return 1;}
static inline esp_err_t nvs_erase_key(nvs_handle_t h,const char*k){return 0;}
static inline esp_err_t nvs_commit(nvs_handle_t h){return 0;}
static inline void nvs_close(nvs_handle_t h){}
