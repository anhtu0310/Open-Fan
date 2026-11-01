#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
int64_t fake_now = 1000000;
uint32_t out_freq; bool out_swing;
#include "sensors.h"
static int16_t s_temp; static uint32_t s_age; static bool s_ok = true;
bool sensors_get(const uint8_t mac[6], sensor_info_t *o){ if(!s_ok) return false; memset(o,0,sizeof *o); o->has_reading=true; o->temp=s_temp; o->age_s=s_age; return true; }
#include "fan.c"   /* copied next to stub board.h by run.sh */
static void run(int secs){ for(int i=0;i<secs;i++){ fake_now+=1000000; fan_tick(); } }
int main(void){
    fan_init();
    fan_status_t st; fan_config_t c;
    assert(fan_set_speed(3)==0); assert(out_freq==300);
    fan_set_swing(true); assert(out_swing);
    fan_set_power(false); assert(out_freq==0);                 /* swing pin follows board rule */
    assert(fan_set_pwm(5000)==0); fan_get_status(&st); assert(st.mode==FAN_MODE_PWM && st.pwm_hz==350 && out_freq==350); /* clamped to max */
    fan_get_config(&c); c.pwm_max_hz=600; assert(fan_set_config(&c)==0); fan_set_pwm(555); assert(out_freq==555);
    c.pwm_max_hz=400; assert(fan_set_config(&c)==0); assert(out_freq==400);  /* range shrink clamps live output */
    c.pwm_max_hz=2000; assert(fan_set_config(&c)==ESP_ERR_INVALID_ARG);
    fan_set_mode(FAN_MODE_PRESET); assert(out_freq==300);

    /* timer */
    fan_set_timer(10); run(9); assert(out_freq==300); run(2); assert(out_freq==0);
    fan_get_status(&st); assert(st.last_event==FAN_EVT_TIMER && st.timer_left_s==0);
    fan_set_timer(60); fan_get_status(&st); assert(st.power && st.timer_left_s==60);  /* timer turns on */
    fan_set_power(false); fan_get_status(&st); assert(st.timer_left_s==0);              /* off cancels */

    /* auto off: 25.00 for 2 min, resume at 26.00 */
    fan_get_config(&c); c.auto_off=true; c.off_temp=2500; c.hold_min=2; c.auto_on=true; c.on_temp=2550;
    memset(c.sensor,0xAA,6); assert(fan_set_config(&c)==0);
    c.on_temp=2520; assert(fan_set_config(&c)==ESP_ERR_INVALID_ARG);              /* <0.5 C gap rejected */
    fan_switch_position(2); assert(out_freq==250);
    s_temp=2600; run(5); fan_get_status(&st); assert(st.auto_state==AUTO_ARMED);
    s_temp=2490; run(60); fan_get_status(&st); assert(st.auto_state==AUTO_COUNTING && st.auto_wait_s==60 && st.power);
    s_temp=2510; run(1); fan_get_status(&st); assert(st.auto_state==AUTO_ARMED);   /* blip above resets */
    s_temp=2490; run(119); assert(out_freq==250); run(1); assert(out_freq==0);
    fan_get_status(&st); assert(st.auto_state==AUTO_TRIPPED && st.last_event==FAN_EVT_AUTO_OFF && st.last_event_temp==2490);
    s_temp=2549; run(300); assert(out_freq==0);
    s_temp=2550; run(119); assert(out_freq==0); fan_get_status(&st); assert(st.auto_state==AUTO_RESUMING);
    run(1); assert(out_freq==250); fan_get_status(&st); assert(st.last_event==FAN_EVT_AUTO_ON);
    /* stale sensor pauses rule, fan untouched */
    s_temp=2000; s_age=700; run(300); assert(out_freq==250); fan_get_status(&st); assert(st.auto_state==AUTO_STALE);
    s_age=0; run(120); assert(out_freq==0);
    /* manual off after trip: no auto resume */
    fan_switch_position(0); s_temp=3000; run(300); assert(out_freq==0); fan_get_status(&st); assert(st.auto_state==AUTO_IDLE);
    puts("FAN OK");
}
