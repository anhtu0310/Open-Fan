#include <assert.h>
#include <string.h>
#include <stdlib.h>
int64_t fake_now = 1000000;
#define strlcpy(d,s,n) (snprintf(d,n,"%s",s))
#include "sensors.c"

static void adv(const uint8_t addr_be[6], const uint8_t *data, int len) {
    uint8_t le[6]; for (int i = 0; i < 6; i++) le[i] = addr_be[5 - i];
    handle_adv(le, -60, data, len);
}
static void show(void){
    sensor_info_t l[4]; int n = sensors_list(l, 4);
    for (int i=0;i<n;i++){char m[18];mac_to_str(l[i].mac,m);
      printf("  %s %-14s fmt=%-7s t=%.2f h=%.2f b=%u mv=%u\n",m,l[i].name,sensor_fmt_name(l[i].fmt),l[i].temp/100.0,l[i].hum/100.0,l[i].batt,l[i].batt_mv);}
}
int main(void){ setvbuf(stdout,NULL,_IONBF,0);
    sensors_init();
    const uint8_t A[6]={0xA4,0xC1,0x38,0x11,0x22,0x33}, B[6]={0xA4,0xC1,0x38,0x44,0x55,0x66}, C[6]={0xA4,0xC1,0x38,0x77,0x88,0x99};
    /* ATC1441: len=16 type 0x16 uuid 1a 18, mac, temp 0x00FA=25.0, hum 55, batt 100, mv 2954, cnt */
    uint8_t atc[] = {0x02,0x01,0x06, 0x10,0x16,0x1A,0x18, 0xA4,0xC1,0x38,0x11,0x22,0x33, 0x00,0xFA, 0x37, 0x64, 0x0B,0x8A, 0x05};
    adv(A, atc, sizeof atc);
    /* pvvx custom: temp -1.23 (0xFF85), hum 45.67 (0x11D7), mv 3001, batt 88 */
    uint8_t pv[] = {0x12,0x16,0x1A,0x18, 0x66,0x55,0x44,0x38,0xC1,0xA4, 0x85,0xFF, 0xD7,0x11, 0xB9,0x0B, 0x58, 0x01, 0x04,
                    0x0B,0x09,'B','e','d','r','o','o','m','_','A','T'};
    adv(B, pv, sizeof pv);
    /* BTHome v2: 40 | 00 pid | 01 batt 77 | 02 temp 2506 | 03 hum 5055 */
    uint8_t bt[] = {0x02,0x01,0x06, 0x0E,0x16,0xD2,0xFC, 0x40, 0x00,0x12, 0x01,0x4D, 0x02,0xCA,0x09, 0x03,0xBF,0x13};
    adv(C, bt, sizeof bt);
    /* BTHome second packet type from pvvx: voltage + switch, must update only mv */
    uint8_t bt2[] = {0x0B,0x16,0xD2,0xFC, 0x40, 0x00,0x13, 0x0C,0xD0,0x0B, 0x10,0x00};
    adv(C, bt2, sizeof bt2);
    /* encrypted BTHome must be ignored, and a non-temp BTHome device must not get a slot */
    uint8_t enc[] = {0x0B,0x16,0xD2,0xFC, 0x41, 1,2,3,4,5,6,7};
    const uint8_t D[6]={1,2,3,4,5,6}; adv(D, enc, sizeof enc);
    uint8_t btn[] = {0x07,0x16,0xD2,0xFC, 0x44, 0x3A,0x01};
    adv(D, btn, sizeof btn);
    /* truncated / garbage */
    uint8_t bad[] = {0x1F,0x16,0x1A}; adv(D, bad, sizeof bad);
    uint8_t bad2[] = {0x05,0x16,0xD2,0xFC,0x40,0x02}; adv(D, bad2, sizeof bad2);
    show();

    sensor_info_t s;
    assert(sensors_get(A,&s) && s.fmt==SENSOR_FMT_ATC1441 && s.temp==2500 && s.hum==5500 && s.batt==100 && s.batt_mv==2954);
    assert(sensors_get(B,&s) && s.fmt==SENSOR_FMT_PVVX && s.temp==-123 && s.hum==4567 && s.batt==88 && s.batt_mv==3001 && !strcmp(s.name,"Bedroom_AT"));
    assert(sensors_get(C,&s) && s.temp==2506 && s.hum==5055 && s.batt==77 && s.batt_mv==3024);
    assert(!sensors_get(D,&s));

    /* history: 3 samples, B goes stale before the 3rd */
    for (int i = 0; i < 3; i++) {
        if (i == 2) { adv(A, atc, sizeof atc); }
        sensors_history_tick(i == 1 ? 250 : 0);
        fake_now += 121LL * 1000000;
        if (i == 0) { adv(A, atc, sizeof atc); adv(B, pv, sizeof pv); }
    }
    history_sample_t h[40]; uint32_t age;
    int n = sensors_history(B, h, 40, &age);
    printf("history B: n=%d age=%u:", n, age); for (int i=0;i<n;i++) printf(" [%d %u %u]", h[i].temp, h[i].hum, h[i].fan); printf("\n");
    assert(n == 3 && h[0].temp == -123 && h[1].temp == -123 && h[2].temp == -123 && h[1].fan == 62 && h[0].fan == 0);
    fake_now += 400LL*1000000; sensors_history_tick(0); n = sensors_history(B, h, 40, &age); assert(n == 4 && h[3].temp == HIST_TEMP_INVALID && h[3].hum == HIST_HUM_INVALID);
    assert(h[1].hum == 91);   /* 45.67% -> 0.5% steps */

    /* eviction: table of 3 is full, a 4th sensor replaces the least recently heard */
    sensors_rename(A, "Living");
    const uint8_t E[6]={0xA4,0xC1,0x38,0xEE,0xEE,0xEE};
    fake_now += 1000000; adv(C, bt, sizeof bt);
    adv(E, atc, sizeof atc);
    assert(sensors_get(E,&s) && sensors_get(A,&s) && sensors_get(C,&s) && !sensors_get(B,&s));
    show();
    puts("ALL OK");
}
