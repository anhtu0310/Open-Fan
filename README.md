# Open Fan firmware (ESP32-C3)

Open firmware for the **ESP32C3-Dev-Fan-V1** board. It replaces the cloud firmware
(BLE provisioning + MQTT) with a fully local one:

- joins your Wi-Fi and serves a fast single-page web UI (`http://fan.local`)
- the 4 original speeds, plus a **PWM mode** with a slider whose range (min/max) you set
- swing on/off
- **off timer**: after a duration, or at a clock time
- reads **Xiaomi thermometers running [pvvx ATC_MiThermometer](https://github.com/pvvx/ATC_MiThermometer)**
  over BLE: live readings for every room, with temperature and humidity charts (6/12/24 h)
- **auto off when cool**: turns the fan off when the selected thermometer stays below a
  threshold for N minutes, and can optionally turn it back on when it warms up again
- firmware update from the browser (OTA)

Nothing leaves your network. The page has no external dependencies, so it also works on the
setup access point without internet.

## Hardware behaviour (unchanged)

| Signal | GPIO | Notes |
|---|---|---|
| Speed signal | 7 | LEDC, 10-bit, inverted, fixed duty 110. **Speed is set by frequency** |
| Brake | 6 | driven high at boot |
| Swing | 10 | off whenever the fan is off |
| Keys 1-4 | 0, 1, 3, 4 | key 1 = off, keys 2-4 = speed 1-3 (same as before) |
| Reset key | 9 | hold 3 s: forget Wi-Fi, restart into setup mode |
| LED | 8 | active low. Blinking = setup AP, on = connecting, off = connected |

Preset speeds are 200 / 250 / 300 / 350 Hz, as in the original firmware. The PWM slider sets this
same frequency directly. The slider range defaults to 100-350 Hz and can be changed in the UI
within the hard limits set in `menuconfig` (default 50-1000 Hz).

## Build and flash

Needs ESP-IDF 5.3 or newer (tested with 5.4.1).

```sh
. ~/esp-idf/export.sh
idf.py set-target esp32c3
idf.py menuconfig          # optional: "Open Fan Configuration"
idf.py build
```

**Before the first flash, back up the original firmware**, so you can go back to it:

```sh
esptool.py --chip esp32c3 -p /dev/ttyUSB0 read_flash 0 0x400000 original_fan_backup.bin
```

Then erase and flash (the old partition layout and the old cloud credentials are removed):

```sh
idf.py -p /dev/ttyUSB0 erase-flash flash monitor
```

> **Secure Boot / Flash Encryption.** The original firmware was built with both disabled, and so
> is this one. The `encrypted` flags in the old partition table had no effect. To confirm on a
> unit before flashing:
>
> ```sh
> espefuse.py -p /dev/ttyUSB0 summary | grep -E "SPI_BOOT_CRYPT_CNT|SECURE_BOOT_EN"
> ```
>
> Both must be off (`SPI_BOOT_CRYPT_CNT = Disable`, `SECURE_BOOT_EN = False`).

Later updates can be uploaded from the web UI (Settings → Firmware) using `build/open_fan.bin`.

## First start

1. With no Wi-Fi configured the fan opens an access point **`OpenFan-XXXX`**
   (password `smartfan`, change it in `menuconfig`) and the LED blinks.
2. Join it and open `http://192.168.4.1`. Choose **Set up Wi-Fi**, pick your network,
   enter the password, then **Save & restart**.
3. Open `http://192.168.1.123` (static IP, set in menuconfig → Open Fan Configuration →
   Network) or `http://fan.local`. Android does not resolve `.local` names, so use the IP there.

If your router is unreachable for 60 s, the setup AP comes back while the fan keeps retrying.
The physical keys always work, with or without Wi-Fi.

You can also set default credentials at build time (`menuconfig` → Open Fan → Network).

## Thermometers

Flash the thermometers with pvvx firmware and set, in its
[TelinkMiFlasher](https://pvvx.github.io/ATC_MiThermometer/TelinkMiFlasher.html) page,
an **unencrypted** advertising format: `BTHome v2`, `Custom (pvvx)` or `ATC1441`.

Every thermometer heard shows up as a room card. Name them in Settings → Thermometers (names
are stored on the fan). Up to 8 are tracked. Unnamed ones can be replaced by newly heard
devices, for example a neighbour's.

History: one point every 2 minutes, 24 hours per thermometer, kept in RAM. It is lost on power
loss. Times are shown in your browser's clock, so the fan needs no internet time.

### Auto off

Choose the thermometer, the threshold and the hold time. While the fan is on and the reading
stays **below** the threshold for the whole hold time, the fan turns off. If **Turn back on**
is enabled, a fan that the rule turned off restarts, at its previous speed, once the
temperature is at or above the second value for the same hold time. The two values must
be at least 0.5 °C apart. Any manual action (web, keys, timer) cancels a pending restart. If the
thermometer stops reporting for 10 minutes the rule pauses and leaves the fan as it is.

## HTTP API

All `POST` bodies are `application/x-www-form-urlencoded`. Responses are JSON.

| Endpoint | Fields |
|---|---|
| `GET /api/state` | everything the UI shows |
| `POST /api/control` | `power=0/1/toggle`, `speed=1..4`, `mode=preset/pwm`, `pwm=<Hz>`, `swing=0/1`, `timer=<seconds, 0 cancels>` |
| `POST /api/config` | `pwm_min`, `pwm_max`, `ao=0/1`, `ao_temp`, `hold` (min), `ar=0/1`, `ar_temp`, `sensor=<MAC or empty>` |
| `POST /api/sensor` | `mac` + `name`, or `mac` + `forget=1` |
| `GET /api/history?mac=…` | `t` (0.01 °C), `h` (0.5 %), `f` (Hz/4), `interval`, `age` (s) |
| `GET /api/wifi/scan`, `POST /api/wifi` | `ssid`, `pass` or `forget=1` |
| `POST /api/ota` | raw `.bin` body |
| `POST /api/reboot` | |

Example: `curl -d speed=3 http://fan.local/api/control`

The API has no authentication. Anyone on your LAN, or on the setup AP, can control the fan
and upload firmware. Keep the device on a trusted network.

## Source layout

| File | Purpose |
|---|---|
| `main/board.c` | GPIO, LEDC and keys: the original hardware routines |
| `main/fan.c` | fan state, presets/PWM, timer, auto-off rule, NVS persistence |
| `main/sensors.c` | NimBLE passive scanner, pvvx format parsers, history ring buffer |
| `main/wifi.c` | station + setup AP, reconnect, mDNS |
| `main/web.c` | HTTP server, REST API, OTA |
| `main/web/index.html` | the UI (gzipped and embedded at build time) |
| `test/host/run.sh` | host unit tests for the BLE parsers and fan logic (`gcc`, no ESP-IDF needed) |
