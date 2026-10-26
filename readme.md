# ePixC LYT firmware — a modified version of WLED

**This is a Derivative Work of WLED and it has been modified.** EUPL v1.2 Article 5 requires that
notice to be prominent, and to carry the date, so it is the first thing in this file rather than a
line somewhere below.

| | |
|---|---|
| **Upstream** | [WLED](https://github.com/wled-dev/WLED) by Christian Schwinne and contributors |
| **Licence** | EUPL v1.2 or later — see [`LICENSE`](LICENSE). ePixC's changes are under the same licence, as Article 5 requires |
| **Modified by** | ePixC (ePixCTech), from 2026-04 onward; this notice added 2026-08-27 |

## What ePixC changed

ePixC's code is in `usermods/pixc_connect_blink/`, in the `wled00/pixc_*` files and in the
`PixC_V1` build environment. Where ePixC changed an upstream file, the change is mostly behind a
`PIXC_` flag (usually `PIXC_LAN_AUTH`), so the diff against upstream stays readable. Updated
2026-10-20; until then this list named only the usermod and the build environment.

- **`pixc_connect_blink`**: the usermod that talks to the ePixC cloud. The light fetches its own
  broker login over HTTPS (provisioning), reports state, health and hourly energy over MQTT, and
  takes OTA updates with an ECDSA P-256 signature check that refuses any image it cannot verify.
  It downloads updates only from `https://api.epixc.in/firmware/`.
- **`pixc_https`**: an ESP-IDF `esp_http_client` wrapper with the ISRG roots pinned (X1, X2,
  Root YE and Root YR) and no plaintext fallback, because the framework WLED pins has no
  `WiFiClientSecure`.
- **`wled00/pixc_lan*`, `wled00/pixc_device*`**: pairing and the signed LAN API. The setup
  hotspot is `ePixC-XXXX` with a password for each unit from factory NVS (printed on the label).
  `/json/pixc/pair` answers on the hotspot only, every other `/json` request must be signed with
  the light's LAN key, and DDP is taken only from an address that holds a realtime lease.
- **`wled00/pixc_mqtt_client*`**: an esp-mqtt client with AsyncMqttClient's interface, for MQTT
  over WebSocket on 443 (`wss://api.epixc.in/mqtt`) or MQTT over TLS on 8883.
- **Upstream files ePixC changed** (`wled.cpp`, `wled_server.cpp`, `json.cpp`, `mqtt.cpp`,
  `presets.cpp`, `udp.cpp`, `e131.cpp`, `cfg.cpp`, `util.cpp` and a few more): no CORS, no `/win`,
  no web UI, the UDP control inputs closed, and LAN, MQTT and preset writes filtered.
- **`tools/factory_provision.py`**: writes the factory NVS image with the hotspot password and
  prints the label.
- **`test/pixc/`**: host tests of the ePixC logic (`test/pixc/run.sh`).
- **`platformio_override.ini`**: the `PixC_V1` target for the ESP32-S3 board ePixC ships, with
  WLED's web UI, Alexa, DMX input, E1.31 and Art-Net, ESP-NOW, the GIF *Image* effect and other
  unused features turned off.
- **`ci/`**: how the box builds a release (epixc-backend `deploy/native/pipeline`). A release is a
  tag made by hand on `main`, named **`lyt-v<version>`** after `version` in `package.json` (the
  version every light reports; the box refuses a tag that names another). The box runs
  `ci/test.sh` (the host tests, the claims and brand checks, the pinned CA roots), then, only if
  it passed, `ci/build.sh`: `pio run -e PixC_V1` and every image gate in `ci/checks/` (the image
  fits the OTA slot, no plaintext HTTP client, the LAN API gated, no bench defaults,
  `verifyRollbackLater`, bootloader rollback, an OTA public key compiled in). The box signs
  `pixc.bin` with the OTA key, refusing an image whose compiled key is not its own, and the
  admin console deploys it (Deploy, Firmware). The build also keeps what a first flash over USB
  needs: `bootloader.bin`, `partitions.bin`, `boot_app0.bin` and `flash.json` (chip, flash
  settings and each file's offset, read from the build), written to a board as they are.
  GitHub Actions builds nothing here since 2026-10-26: ePixC's workflow and upstream's are gone.
  Both scripts run on a laptop too: `bash ci/test.sh`, `bash ci/build.sh <folder>`.

Upstream's own README follows, unchanged. It describes WLED, not the ePixC build: several features
it lists (Alexa, the web UI, DMX, E1.31 and Art-Net, ESP-NOW sync) are off in `PixC_V1`.

## Why this repository is public

WLED is under the EUPL, which is a copyleft licence: Article 1 counts selling a device as
Distribution, Article 5 requires a Derivative Work to go out under the same licence, and Article 3
requires the source of a distributed binary to be available. So this repository is public because
that is what shipping a controller with this firmware on it obliges — not as a courtesy, and not
optionally.

Nothing else in the ePixC stack is affected. The app, the backend, the website and the cloud
services are separate works that link none of this code.

---

<p align="center">
  <img src="/images/wled_logo_akemi.png">
  <a href="https://github.com/wled-dev/WLED/releases"><img src="https://img.shields.io/github/release/wled-dev/WLED.svg?style=flat-square"></a>
  <a href="https://raw.githubusercontent.com/wled-dev/WLED/main/LICENSE"><img src="https://img.shields.io/github/license/wled-dev/wled?color=blue&style=flat-square"></a>
  <a href="https://wled.discourse.group"><img src="https://img.shields.io/discourse/topics?colorB=blue&label=forum&server=https%3A%2F%2Fwled.discourse.group%2F&style=flat-square"></a>
  <a href="https://discord.gg/QAh7wJHrRM"><img src="https://img.shields.io/discord/473448917040758787.svg?colorB=blue&label=discord&style=flat-square"></a>
  <a href="https://kno.wled.ge"><img src="https://img.shields.io/badge/quick_start-wiki-blue.svg?style=flat-square"></a>
  <a href="https://github.com/Aircoookie/WLED-App"><img src="https://img.shields.io/badge/app-wled-blue.svg?style=flat-square"></a>
  <a href="https://gitpod.io/#https://github.com/wled-dev/WLED"><img src="https://img.shields.io/badge/Gitpod-ready--to--code-blue?style=flat-square&logo=gitpod"></a>

  </p>

# Welcome to WLED! ✨

A fast and feature-rich implementation of an ESP32 and ESP8266 webserver to control NeoPixel (WS2812B, WS2811, SK6812) LEDs or also SPI based chipsets like the WS2801 and APA102!

Originally created by [Aircoookie](https://github.com/Aircoookie)

## ⚙️ Features
- WS2812FX library with more than 100 special effects  
- FastLED noise effects and 50 palettes  
- Modern UI with color, effect and segment controls  
- Segments to set different effects and colors to user defined parts of the LED string  
- Settings page - configuration via the network  
- Access Point and station mode - automatic failsafe AP  
- [Up to 10 LED outputs](https://kno.wled.ge/features/multi-strip/#esp32) per instance
- Support for RGBW strips  
- Up to 250 user presets to save and load colors/effects easily, supports cycling through them.  
- Presets can be used to automatically execute API calls  
- Nightlight function (gradually dims down)  
- Full OTA software updateability (HTTP + ArduinoOTA), password protectable  
- Configurable analog clock (Cronixie, 7-segment and EleksTube IPS clock support via usermods) 
- Configurable Auto Brightness limit for safe operation  
- Filesystem-based config for easier backup of presets and settings  

## 💡 Supported light control interfaces
- WLED app for [Android](https://play.google.com/store/apps/details?id=ca.cgagnier.wlednativeandroid) and [iOS](https://apps.apple.com/gb/app/wled-native/id6446207239)
- JSON and HTTP request APIs  
- MQTT   
- E1.31, Art-Net, DDP and TPM2.net
- [diyHue](https://github.com/diyhue/diyHue) (Wled is supported by diyHue, including Hue Sync Entertainment under udp. Thanks to [Gregory Mallios](https://github.com/gmallios))
- [Hyperion](https://github.com/hyperion-project/hyperion.ng)
- UDP realtime  
- Alexa voice control (including dimming and color)  
- Sync to Philips hue lights  
- Adalight (PC ambilight via serial) and TPM2  
- Sync color of multiple WLED devices (UDP notifier)  
- Infrared remotes (24-key RGB, receiver required)  
- Simple timers/schedules (time from NTP, timezones/DST supported)  

## 📲 Quick start guide and documentation

See the [documentation on our official site](https://kno.wled.ge)!

[On this page](https://kno.wled.ge/basics/tutorials/) you can find excellent tutorials and tools to help you get your new project up and running!

## 🖼️ User interface
<img src="/images/macbook-pro-space-gray-on-the-wooden-table.jpg" width="50%"><img src="/images/walking-with-iphone-x.jpg" width="50%">

## 💾 Compatible hardware

See [here](https://kno.wled.ge/basics/compatible-hardware)!

## ✌️ Other

Licensed under the EUPL v1.2 license  
Credits [here](https://kno.wled.ge/about/contributors/)!
CORS proxy by [Corsfix](https://corsfix.com/)

Join the Discord server to discuss everything about WLED!

<a href="https://discord.gg/QAh7wJHrRM"><img src="https://discordapp.com/api/guilds/473448917040758787/widget.png?style=banner2" width="25%"></a>

Check out the WLED [Discourse forum](https://wled.discourse.group)!  

You can also send me mails to [dev.aircoookie@gmail.com](mailto:dev.aircoookie@gmail.com), but please, only do so if you want to talk to me privately.  

If WLED really brightens up your day, you can [![](https://img.shields.io/badge/send%20me%20a%20small%20gift-paypal-blue.svg?style=flat-square)](https://paypal.me/aircoookie)


*Disclaimer:*   

If you are prone to photosensitive epilepsy, we recommended you do **not** use this software.  
If you still want to try, don't use strobe, lighting or noise modes or high effect speed settings.

As per the EUPL license, I assume no liability for any damage to you or any other person or equipment.  

