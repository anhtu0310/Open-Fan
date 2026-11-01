#pragma once
#include <stdbool.h>
#include <stdint.h>
extern uint32_t out_freq; extern bool out_swing;
static inline void board_fan_output(uint32_t f, bool s){out_freq=f;out_swing=s;}
