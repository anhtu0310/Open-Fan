# Porting log: WiLe cloud firmware → Open Fan firmware

## 2026-11-01: initial port (v1.0.0)

### Source analysed
Original firmware: `/home/anhtu/Documents/esp32/WiLe-Firmware/dev_fan` (ESP-IDF 5.1.6, C/C++,
depends on the proprietary `wile_common` / `wile_config` libraries for BLE-mesh provisioning, MQTT
and the cloud). Board: `CONFIG_ESP32C3_FAN_V1`.

Hardware behaviour extracted from `main/board.c`, `main/board.h` and `main/app_device.cpp`:

| Item | Original behaviour | Kept? |
|---|---|---|
| Speed signal GPIO7 | LEDC low-speed, timer 0 / channel 0, 10-bit, `output_invert`, init at 50 Hz with `LEDC_AUTO_CLK`, duty 0 when off | yes, identical |
| Speed control | **Fixed duty 110/1024, speed set by frequency**: speed 1-4 = 200/250/300/350 Hz (`ledc_set_freq`) | yes (Kconfig `FAN_SPEEDn_HZ`, `FAN_PWM_DUTY`) |
| Brake GPIO6 | output, driven high at init, never changed | yes |
| Swing GPIO10 | output; forced low when the fan is off | yes |
| Keys GPIO0/1/3/4 | input pull-up, any-edge ISR, acts on press; after 200+300 ms the last pressed key index is the position: key 1 = off, keys 2-4 = speed 1-3 | yes |
| Reset key GPIO9 | hold 3 s → factory reset (cloud) | now: forget Wi-Fi and restart into setup AP |
| LED GPIO8 | active low; slow blink unprovisioned, off when got IP, on when disconnected | same meaning (blink = setup AP) |
| FAN_DATA GPIO5 | defined, unused | unused |
| State restore | power/speed/swing restored after power loss | yes (NVS, 3 s debounced) |
| Speed 4 (MAX) | only reachable from the app | only from the web UI / API |

Security config: verified in the original `sdkconfig` and `sdkconfig.old` that `CONFIG_SECURE_BOOT`,
`CONFIG_SECURE_FLASH_ENC_ENABLED` and `CONFIG_FLASH_ENCRYPTION_ENABLED` are not set. The
`encrypted` flags in the old partition table had no effect. The user confirmed nothing was
enabled. The eFuses on each unit can still be checked with `espefuse.py summary`.

### Bugs in the original, fixed during the port
- `xTaskCreate` was called from inside the GPIO ISR (not ISR-safe). Replaced by a persistent
  button task notified with `xTaskNotifyFromISR`. Contact-bounce notifications are cleared after
  each read.
- The reset-key hold loop busy-waited without a delay. It now polls every 50 ms.

### What was built
A new ESP-IDF 5.4 project in plain C with no WiLe dependency:
- `board.c`: hardware layer from the table above.
- `fan.c`: state (power, preset/PWM mode, speed, PWM Hz, swing), slider range, off timer,
  temperature auto-off with optional auto-resume, NVS persistence.
- `sensors.c`: NimBLE passive scanner for pvvx ATC_MiThermometer. Decodes the ATC1441, pvvx
  custom and BTHome v2 (unencrypted) formats. Tracks up to 8 sensors with user names in NVS,
  plus a 24 h RAM history (2-minute samples of temperature, humidity and fan Hz).
- `wifi.c`: station mode with NVS or Kconfig credentials. `OpenFan-XXXX` setup AP when no
  credentials are stored, or after 60 s offline. Reconnect with backoff, mDNS `fan.local`, scan.
- `web.c`: esp_http_server, gzipped single-page UI embedded at build time, form-encoded REST API
  with hand-built JSON (no cJSON), browser OTA.
- `web/index.html`: self-contained UI (no CDN). Light/dark theme, room cards, temperature and
  humidity charts as separate lanes (no dual axis) with a fan-activity lane, crosshair tooltip
  and table view.

### Decisions
- **PWM slider = frequency in Hz** (user confirmed). Default range 100-350 Hz, editable in the UI
  within the Kconfig hard limits 50-1000 Hz.
- No SNTP/RTC: history and the timer use uptime. The browser converts to wall-clock time
  ("off at 02:30" becomes seconds from now, and chart times come from `age` + `interval`).
- The auto-off rule pauses (leaves the fan alone) when the sensor is silent for more than 10 min.
  Auto-resume only happens if the rule itself turned the fan off. Any manual action cancels it.
  Resume must be at least 0.5 °C above the off threshold.
- Starting a timer turns the fan on. Turning the fan off cancels the timer.
- No authentication on the web/API. The user accepted this, for a trusted LAN.
- New partition table (default offset 0x8000): nvs 48k, otadata, phy, ota_0/ota_1 1856k each.
  The old `rogo`, `test_rd` and `nvs_key` partitions were dropped. The flash must be erased once.
- Setup AP password `smartfan` (Kconfig), because OTA is reachable through the AP.

### Problems hit
- **NimBLE in IDF 5.4.1 does not compile observer-only** (errors in `ble_gap.c`:
  `ble_gap_call_conn_event_cb` undeclared). Workaround: `CONFIG_BT_NIMBLE_ROLE_CENTRAL=y` in
  `sdkconfig.defaults`. The central role is never used.

### Verification done
- `idf.py build` with IDF v5.4.1: no warnings in project code. Image 0x103e00 bytes (44% of the
  app partition free). Static DRAM 152 KB, about 170 KB left for heap.
- Host unit tests (`test/host/run.sh`, ASan + UBSan): all BLE formats, rejection of
  encrypted/garbage/non-thermometer packets, history staleness, slot eviction; fan presets, PWM
  clamping, timer, auto-off/resume hold times, stale pause, manual cancel.
- UI rendered in headless Chromium with a mocked API, light and dark, at 360 and 420 px wide.
  No JS errors. Fixed overlapping 12 h x-axis labels and the timer chip wrapping.

### Not yet verified on hardware
- Flashing a real unit, LEDC frequencies on the scope, keys and LED.
- Wi-Fi + BLE coexistence (scan 80 ms / 320 ms), reception rate of real thermometers.
- Heap headroom at runtime (shown in Settings → Firmware, `heap_min`).
- OTA upload through the browser.

## 2026-11-01: first boot on hardware

- **Crash at boot with saved Wi-Fi credentials**: `esp_wifi_set_config(WIFI_IF_AP, …)` returned
  `ESP_ERR_WIFI_MODE` because the radio was in STA-only mode. Fix in `wifi.c`: the AP config is
  kept in a static `ap_config` and applied in `set_ap(true)` right after switching to APSTA.
  Rule: only call AP config while the mode includes AP.

## 2026-11-01: static IP for Android

- `fan.local` does not resolve on Android (Chrome has no reliable mDNS). Added Kconfig
  `FAN_STATIC_IP` (default on): 192.168.1.123 / 255.255.255.0, gateway 192.168.1.1. Applied in
  `wifi.c` on `WIFI_EVENT_STA_CONNECTED` (DHCP client stopped, `esp_netif_set_ip_info`, which
  posts `IP_EVENT_STA_GOT_IP`, so the connected path is unchanged). mDNS and the DHCP hostname
  are still set. No DNS server is configured; the firmware needs none.
- Caveat: joining a network that is not 192.168.1.x from the web UI leaves the fan "connected"
  but unreachable (no fallback AP). Hold GPIO9 for 3 s to forget Wi-Fi, or turn the option off.
- Gotcha: after editing `Kconfig.projbuild`, a plain `idf.py build` did not pick up the new
  symbols (the code compiled out silently). Run `idf.py reconfigure` first.
- Not yet verified on hardware.

### Ideas / TODO
- Persist the history to flash (e.g. hourly) so it survives power loss.
- Optional SNTP for absolute-time schedules.
- Home Assistant integration (MQTT discovery or REST).
- BTHome v1 (UUID 0x181C) and encrypted pvvx formats (bindkey).
- Captive-portal DNS on the setup AP.
