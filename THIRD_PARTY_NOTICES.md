# Third-party notices

The code in this repository is MIT-licensed (see [LICENSE](LICENSE)), except for the parts below, which keep their
own licenses. Firmware images built from this repository contain all of them.

## Included in this repository

| Component | Where | License |
|---|---|---|
| Montserrat font (Julieta Ulanovsky and the Montserrat Project Authors) | `main/montserrat.ttf` | SIL Open Font License 1.1, see [`main/montserrat-OFL.txt`](main/montserrat-OFL.txt) |
| Captive-portal DNS server (from ESP-IDF's `captive_portal` example, Espressif Systems) | `components/dns_server/` | Unlicense OR CC0-1.0 |

## Fetched at build time

| Component | Version | License |
|---|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) (FreeRTOS, lwIP, mbedTLS, Wi-Fi drivers…) | 5.5.4 | Apache-2.0, with components under their own compatible licenses (see ESP-IDF's `COPYRIGHT.rst`) |
| [LVGL](https://github.com/lvgl/lvgl) (including its TinyTTF / stb_truetype and QR code modules) | 9.2.2 | MIT |
| [miniz](https://github.com/richgel999/miniz) inflate (`tinfl`), in the ESP32-S3 ROM, used by `components/forge_core/png_rows.c` | ROM | MIT |

## Tools used by the web flasher

| Component | License |
|---|---|
| [ESP Web Tools](https://github.com/esphome/esp-web-tools) (loaded by the flasher page) | Apache-2.0 |
| [esptool](https://github.com/espressif/esptool) (flash helper; not in the repository) | GPL-2.0-or-later |

The framework code (components/, boards/, tools/) is adapted from [esp32-s3-weather](https://github.com/TheMonkeyz/esp32-s3-weather), MIT, same author.
