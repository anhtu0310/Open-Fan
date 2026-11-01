#pragma once
#include <stdint.h>
struct ble_gap_disc_params{uint16_t itvl,window;uint8_t filter_policy,limited,passive,filter_duplicates;};
struct ble_gap_event{int type;struct{struct{uint8_t val[6];}addr;int8_t rssi;const uint8_t*data;uint8_t length_data;}disc;};
#define BLE_GAP_EVENT_DISC 7
#define BLE_GAP_EVENT_DISC_COMPLETE 8
#define BLE_HCI_SCAN_FILT_NO_WL 0
#define BLE_HS_FOREVER 0x7fffffff
static inline int ble_gap_disc(uint8_t a,int32_t d,const struct ble_gap_disc_params*p,int(*cb)(struct ble_gap_event*,void*),void*arg){return 0;}
static inline int ble_hs_util_ensure_addr(int x){return 0;}
static inline int ble_hs_id_infer_auto(int p,uint8_t*a){return 0;}
static inline esp_err_t nimble_port_init(void){return 0;}
static inline void nimble_port_run(void){}
static inline void nimble_port_freertos_deinit(void){}
static inline void nimble_port_freertos_init(void(*f)(void*)){}
static struct { void(*sync_cb)(void); void(*reset_cb)(int);} ble_hs_cfg;
