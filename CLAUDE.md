# Open Fan firmware

Open, local-only firmware for the user's own ESP32-C3 fan board (ESP32C3-Dev-Fan-V1). It replaces
a proprietary cloud firmware: Wi-Fi + web UI, 4 preset speeds + PWM (frequency) slider, off
timer, BLE thermometers (pvvx ATC_MiThermometer) with history charts and auto-off.
History of the port and its decisions: [PORTING_LOG.md](PORTING_LOG.md). Keep it updated with
each session's work.

## Build / test

```sh
. ~/esp-idf/export.sh            # ESP-IDF v5.4.1 lives in ~/esp-idf
idf.py build                     # target esp32c3 is already set
idf.py -p /dev/ttyUSB0 flash monitor
test/host/run.sh                 # host unit tests (gcc + ASan), no IDF needed
```

After changing `sdkconfig.defaults`, delete `sdkconfig` before building, or the change is ignored.
After adding symbols to `main/Kconfig.projbuild`, run `idf.py reconfigure`, or the code that uses them silently compiles out.
The original cloud firmware is kept for reference at
`/home/anhtu/Documents/esp32/WiLe-Firmware/dev_fan`. Read it, never modify it.

## Layout

| File | Role |
|---|---|
| `main/board.c/.h` | GPIO/LEDC/keys/LED. Same behaviour as the original hardware routines |
| `main/fan.c/.h` | fan state, presets/PWM, timer, auto-off rule, NVS persistence (`fan` namespace) |
| `main/sensors.c/.h` | NimBLE passive scan, pvvx format parsers, sensor table (`sensors` NVS), RAM history ring |
| `main/wifi.c/.h` | STA + fallback setup AP `OpenFan-XXXX`, reconnect backoff, mDNS, scan (`wifi` NVS) |
| `main/web.c/.h` | esp_http_server, REST API (form-encoded in, hand-built JSON out), OTA |
| `main/web/index.html` | the whole UI: inline CSS/JS, no external assets. Gzipped into the image by `tools/gzip_file.py` |
| `main/main.c` | init order + 1 Hz loop (`fan_tick`, `sensors_history_tick`, LED) |
| `main/Kconfig.projbuild` | "Open Fan Configuration": hostname, default Wi-Fi, AP password, speed Hz, limits, history |
| `test/host/` | stub headers + unit tests; `run.sh` copies `fan.c` next to a stub `board.h` |

## Hardware facts (do not change without the user)

- **Fan speed is set by FREQUENCY, not duty**: GPIO7, LEDC 10-bit, inverted output, fixed
  duty 110. Presets 200/250/300/350 Hz. The PWM slider is in Hz (hard limits 50-1000 Hz in Kconfig).
- The LEDC timer must be initialised at 50 Hz with `LEDC_AUTO_CLK`, like the original. This picks a
  clock source that also allows low frequencies.
- Brake GPIO6 high at init. Swing GPIO10 low whenever the fan is off.
- Keys GPIO0/1/3/4: key 1 = off, keys 2-4 = speed 1-3, read 500 ms after a press edge.
  GPIO9 held 3 s = forget Wi-Fi. LED GPIO8 is active low.
- Pin map is in `board.h` under `CONFIG_FAN_BOARD_ESP32C3_FAN_V1`. The user has the schematic if needed.

## Conventions

- Plain C, ESP-IDF APIs only, no C++ and no extra libraries (only `espressif/mdns`). JSON is built
  with `jb_printf`/`jb_str` in `web.c`, so don't add cJSON.
- Each module owns one FreeRTOS mutex. Never hold two at once: `fan_tick` reads the sensor
  *before* taking the fan lock.
- Temperatures are int16 in 0.01 °C everywhere. History humidity is uint8 in 0.5 % steps, fan is Hz/4.
- NVS blobs are plain structs. A size mismatch on load means "keep the defaults", so changing a
  struct resets that blob.
- No wall-clock time on the device. The browser converts times (`age` + `interval`, timer seconds).
- UI: chart colours and tokens follow the dataviz palette (temp orange, humidity blue, fan
  neutral). Separate lanes, never a dual axis. Test any UI change by rendering it.

## Gotchas

- NimBLE in IDF 5.4.1 fails to compile with only the observer role, so `CONFIG_BT_NIMBLE_ROLE_CENTRAL=y`
  must stay.
- Wi-Fi power save cannot be disabled while BLE is enabled (coexistence). Expect about 100 ms latency.
- In APSTA fallback the STA retries every 30 s. Each retry scans channels and briefly disturbs AP clients.
- The API has no auth. The user accepted this (trusted LAN).
- Previewing the UI: `~/.cache/ms-playwright/chromium_headless_shell-*/chrome-headless-shell/chrome-headless-shell`
  with `--screenshot`. Inject a `fetch` mock before the main script (see PORTING_LOG).
  Node is not installed.

## Status

v1.0.0 builds and passes the host tests, but has **not yet been run on hardware**. See the
"Not yet verified" list in PORTING_LOG.md.
