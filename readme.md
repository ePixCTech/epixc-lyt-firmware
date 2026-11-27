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
  Root YE and Root YR) and no plaintext fallback. It sets the trust decision for every TLS
  connection of the light (provisioning, the OTA download and MQTT) in one place. It was made
  because the framework WLED pinned until 2026-11 had no `WiFiClientSecure`.
- **`wled00/pixc_lan*`, `wled00/pixc_device*`**: pairing and the signed LAN API. The setup
  hotspot is `ePixC-XXXX`, WPA2/WPA3 mixed, with a password for each unit from factory NVS
  (printed on the label).
  `/json/pixc/pair` answers on the hotspot only, every other `/json` request must be signed with
  the light's LAN key, and DDP is taken only from an address that holds a realtime lease. Also the
  factory reset (from the cloud, five short power-ons, or the BOOT button held 10 to 30 s) and the
  board's identity from factory NVS (`huid`, `hw_rev`).
- **The board runcheck** (vault: 08-Codebase/Board runcheck): `pixc info`, `pixc runcheck`,
  `pixc otacheck` and `pixc download` on UART0, which the Writer runs on a board it has just
  written; accepted only in the first ten minutes after a boot or while no Wi-Fi is configured.
- **`wled00/pixc_mqtt_client*`**: an esp-mqtt client with AsyncMqttClient's interface, for MQTT
  over WebSocket on 443 (`wss://api.epixc.in/mqtt`) or MQTT over TLS on 8883.
- **Upstream files ePixC changed** (`wled.cpp`, `wled_server.cpp`, `json.cpp`, `mqtt.cpp`,
  `presets.cpp`, `udp.cpp`, `e131.cpp`, `cfg.cpp`, `util.cpp` and a few more): no CORS, no `/win`,
  no web UI, the UDP control inputs closed, and LAN, MQTT and preset writes filtered.
- **`tools/factory_provision.py`**: writes the factory NVS image with the hotspot password and
  the board's identity, and prints the label.
- **`test/pixc/`**: host tests of the ePixC logic (`test/pixc/run.sh`).
- **`platformio_override.ini`**: the `PixC_V1` target for the ESP32-S3 board ePixC ships, with
  WLED's web UI, Alexa, DMX input, E1.31 and Art-Net, ESP-NOW, the GIF *Image* effect and other
  unused features turned off.
- **`ci/`**: how the box builds a release (epixc-backend `deploy/native/pipeline`). A release is a
  tag made by hand on `master`, named **`lyt-v<version>`** after `version` in `package.json` (the
  version every light reports; the box refuses a tag that names another). The box runs
  `ci/test.sh` (the host tests, the claims and brand checks, the pinned CA roots), then, only if
  it passed, `ci/build.sh`: `pio run -e PixC_V1` and every image gate in `ci/checks/` (the image
  fits the OTA slot, no plaintext HTTP client, the LAN API gated, no bench defaults,
  `verifyRollbackLater`, bootloader rollback and the framework's TLS and hotspot settings, the
  usermods registered, an OTA public key compiled in, the image reports `package.json`'s
  version). The box signs `pixc.bin` with the OTA key, refusing an image whose compiled key is
  not its own, and the admin console deploys it (Deploy, Firmware). The build also keeps what a
  first flash over USB needs: `bootloader.bin`, `partitions.bin`, `boot_app0.bin` and
  `flash.json` (chip, flash settings and each file's offset, read from the build), written to a
  board as they are. It also writes the build's `firmware.elf` to
  `<folder>/private/<env>-<version>-<elfsha16>.elf`, where `<elfsha16>` is the first 16 hex digits
  of the ELF's SHA-256: the `elf` that a light's crash report and the console's Events show. The
  box keeps this file privately and never publishes it; a crash's addresses cannot be decoded
  without it.
  GitHub Actions builds nothing here since 2026-10-26: ePixC's workflow and upstream's are gone.
  Both scripts run on a laptop too: `bash ci/test.sh`, `bash ci/build.sh <folder>`.

Upstream's own README follows, unchanged. It describes WLED, not the ePixC build: several features
it lists (Alexa, the web UI, DMX, E1.31 and Art-Net, ESP-NOW sync) are off in `PixC_V1`.

## Re-issue an older release

A light does not install an image whose build number is lower than the build that it runs
(`otaRefusal` in `usermods/pixc_connect_blink/pixc_logic.h`). The build number is the commit time
in minutes (`pio-scripts/pixc_build_id.py`), so an older release always has a lower number. Thus
the console cannot roll the lights back to an older release. Its Roll back is disabled. It does
not make a build current when its build number is lower than the build number of the current
release. To go back to an older release, re-issue its source as a new version. The new commit has
a newer commit time, so its build number is higher.

Before you start, make sure that:

- The older release is a `lyt-v*` tag that the box built. Its tree must have `ci/test.sh` and
  `ci/build.sh`, or the box does not build it.
- Its `PIXC_SECURITY_VERSION` (`pixc_logic.h`) is the same as in the bad release. A light refuses
  an image below the highest security version that it ran. If the bad release raised that number,
  do not re-issue. The older source has the vulnerability that the bad release fixed. Fix the
  fault in a new release.
- The box's build image has the platform of the older release. The image has one platform, and a
  build cannot install a different one into it. The pins of the image are in epixc-backend
  `deploy/native/pipeline/image/Containerfile`: `PIO_PLATFORM`, `PIO_FRAMEWORK`,
  `PIO_FRAMEWORK_LIBS` and `PIO_ESPTOOL`. Compare them with `platform` and `platform_packages` of
  `PixC_V1` in the older release's `platformio_override.ini`. The image's PlatformIO must also be
  the version in the older release's `requirements.txt`.

  If one of them is different, change the image in epixc-backend to the older release's pins, and
  deploy the backend. The box uses the changed image from its next build. After the box built the
  re-issue, set the pins back and deploy the backend again.

  A release on IDF 4.4 has `platform = espressif32@6.13.0`. For it, set
  `PIO_PLATFORM=espressif32@6.13.0` and
  `PIO_FRAMEWORK=platformio/framework-arduinoespressif32@3.20017.241212` (ESP-IDF 4.4.7). Platform
  6.13.0 does not use the other two pins. Only a light with the IDF 4.4.7 bootloader starts such an
  image (`docs/pixc-idf5-bench.md`, stage 3, item e).

The example re-issues `17.0.1-pixc7` as `17.0.1-pixc9`, because `17.0.1-pixc8` is bad:

1. Start on `master`, with no changes in the working tree.
2. Restore the tree of the older release. This also deletes each file that the older release does
   not have.
   ```sh
   git switch master
   git restore --source=lyt-v17.0.1-pixc7 --staged --worktree -- :/
   ```
   If the restored `ci/build.sh` does not contain `image_home` (`17.0.1-pixc6` and older), also
   take `ci/build.sh` from commit `36dc95eb`. The box's build image has a read-only PlatformIO
   home. An older `ci/build.sh` gives that home to PlatformIO, and PlatformIO stops because it
   cannot write there. The `ci/build.sh` of `36dc95eb` is the last one for IDF 4.4. It has
   `image_home`, and it runs only the gates that an IDF 4.4 tree has. Do not take `ci/build.sh`
   from `master`. The script on `master` runs gates that an older tree does not have, so the build
   stops.
   ```sh
   git restore --source=36dc95eb --staged --worktree -- ci/build.sh
   ```
3. Set the next version: one more than the newest `pixcN` tag. A light gets only a version that is
   newer than the version that it runs. This command changes `package.json` and the two versions in
   `package-lock.json`.
   ```sh
   npm version 17.0.1-pixc9 --no-git-tag-version --ignore-scripts
   ```
4. Commit. Do not change the commit date (no `GIT_COMMITTER_DATE`, no
   `--committer-date-is-author-date`), because the box makes the build number from it.
   ```sh
   git commit -am "17.0.1-pixc9: the source of 17.0.1-pixc7 again (re-issue)"
   ```
5. When the founder says to release, Claude pushes `master`, makes the annotated tag on this
   commit, and pushes the tag. Do not move or delete a tag.
   ```sh
   git push origin master
   git tag -a lyt-v17.0.1-pixc9 -m "17.0.1-pixc9: re-issue of 17.0.1-pixc7"
   git push origin lyt-v17.0.1-pixc9
   ```
6. Build it in the console: Deploy, Builds, Build, then Firmware and the tag. The box tests, builds
   and signs it. Open the Firmware page (Deploy, Current builds, Firmware). Make sure that the
   ready build has a higher build number than the current release.
7. Deploy it on the Firmware page: Deploy at the ready build, then the target, the tag and a new
   code. It becomes the current release of that target, and the lights get it when they next update.
   Do this for each target that has the bad release. If the fault is urgent, also make the release
   critical. Then each light whose owner allows unattended updates installs it at the owner's hour.
8. To continue from the newer code, revert the re-issue commit (`git revert <commit>`). Then repair
   the fault, set the next version (`17.0.1-pixc10`) and release it as usual.

`EPIXC_BUILD_NUMBER` (`pio-scripts/pixc_build_id.py`) sets the build number by hand. A re-issue
does not need it; use it only for a special case. Never set it higher than the real time. A light
that installs such an image refuses each later release until a commit time is higher than that
number. Also, the light uses the number as its earliest possible time when it examines
certificate dates (`kTlsTimeFloor` in `pixc_https.cpp`).

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

A fast and feature-rich firmware for ESP32 microcontrollers to control addressable LEDs — from simple strips to large 2D matrices and HUB75 panels.

Originally created by [Aircoookie](https://github.com/Aircoookie), now maintained by a community of contributors.

## 🤝 Contributing

Want to help improve WLED? Awesome! Please skim [CONTRIBUTING.md](CONTRIBUTING.md) first - it covers how we like PRs and issues to look, including our take on AI-assisted contributions.
If you're an AI coding agent, [AGENTS.md](AGENTS.md) is for you - please read it before modifying any files. 😊

## ⚙️ Features

### Effects & Visuals
- [**200+ built-in effects**](https://kno.wled.ge/features/effects/) including classic animations, audio-reactive, and 2D/matrix effects
- [50+ color palettes](https://kno.wled.ge/features/palettes/) plus a built-in **custom palette editor** (PixelForge)
- [**2D LED matrix support**](https://kno.wled.ge/advanced/mapping/) with dedicated 2D effects and flexible panel mapping
- [**HUB75 RGB matrix panel support**](https://kno.wled.ge/advanced/HUB75/) (ESP32)
- [**AudioReactive**](https://kno.wled.ge/advanced/audio-reactive/) effects — included by default, responding to sound via microphone, line-in, or network audio source
- Effect blending for smooth transitions between animations
- Antialiased drawing functions for smooth graphics

### Segments & Control
- [**Segments**](https://kno.wled.ge/features/segments/) — apply different effects, colors and palettes to independent parts of your LED setup simultaneously
- Up to **250 presets** to save and recall colors, effects and segment configurations — supports [playlists](https://kno.wled.ge/features/presets/) for automated cycling
- Nightlight function with configurable dimming curve
- Configurable **Auto Brightness Limiter** (per output) for safe operation

### Hardware Support
- **ESP32** (all variants: original, S2, S3, C3)
- [**Up to 17 LED outputs**](https://kno.wled.ge/features/multi-strip/) on ESP32 using parallel I2S + RMT
- [Addressable LED support](https://kno.wled.ge/basics/compatible-led-strips/): WS2812B, WS2811, WS2815, SK6812, WS2805, TM1914, APA102, WS2801, LPD8806, and many more
- RGBW, [RGB+CCT](https://kno.wled.ge/features/cct/) and white-only strips
- PWM outputs for analog LEDs and dimmers
- [**Ethernet** support](https://kno.wled.ge/features/ethernet-lan/) for a wide range of boards (QuinLED, LILYGO, Olimex, and more)
- Filesystem-based config for easy backup and restore of presets and settings
- Full OTA firmware updates (HTTP + ArduinoOTA), password-protectable

### Connectivity & Integrations
- **WLED app** for [Android](https://play.google.com/store/apps/details?id=ca.cgagnier.wlednativeandroid) and [iOS](https://apps.apple.com/gb/app/wled-native/id6446207239)
- [JSON](https://kno.wled.ge/interfaces/json-api/) and [HTTP request](https://kno.wled.ge/interfaces/http-api/) APIs
- **Multi-WiFi** — connect to up to 3 networks with automatic AP fallback
- **ESP-NOW** wireless sync between devices (no WiFi router required)
- [**MQTT**](https://kno.wled.ge/interfaces/mqtt/) with Home Assistant discovery
- [**E1.31, Art-Net**](https://kno.wled.ge/interfaces/e1.31-dmx/), [DDP](https://kno.wled.ge/interfaces/ddp/) and [TPM2.net](https://kno.wled.ge/interfaces/udp-realtime/) for DMX/professional lighting control
- [UDP realtime sync](https://kno.wled.ge/interfaces/udp-notifier/) across multiple WLED devices
- Alexa voice control (on/off, brightness, color)
- [Philips Hue sync](https://kno.wled.ge/interfaces/philips-hue/)
- [diyHue](https://github.com/diyhue/diyHue) and [Hyperion](https://github.com/hyperion-project/hyperion.ng) integration
- [Adalight / TPM2](https://kno.wled.ge/interfaces/serial/) (PC ambilight via serial)
- [Infrared remote control](https://kno.wled.ge/interfaces/infrared/) (24-key RGB, receiver required)
- Timers and schedules (NTP time sync, full timezone and DST support)

### Developer-Friendly
- **Usermod system** — extend WLED with community or custom modules without modifying core code
- Large and active [usermod library](https://kno.wled.ge/advanced/community-usermods/) including AudioReactive, temperature sensors, rotary encoders, displays, and much more
- Well-documented [JSON API](https://kno.wled.ge/interfaces/json-api/)
- Licensed under the **EUPL v1.2**

## 📲 Quick start guide and documentation

See the [documentation at kno.wled.ge](https://kno.wled.ge)!

[Tutorials and getting-started guides](https://kno.wled.ge/basics/tutorials/) to help you get your project running quickly.

## 🖼️ User interface

<img src="/images/macbook-pro-space-gray-on-the-wooden-table.jpg" width="50%"><img src="/images/walking-with-iphone-x.jpg" width="50%">

## 💾 Compatible hardware

See the [compatible hardware list](https://kno.wled.ge/basics/compatible-hardware) on the wiki.

## ✌️ Other

Licensed under the [EUPL v1.2](https://raw.githubusercontent.com/wled-dev/WLED/main/LICENSE).  
Credits to all [contributors](https://kno.wled.ge/about/contributors/)!  
CORS proxy by [Corsfix](https://corsfix.com/).

![CodeRabbit Pull Request Reviews](https://img.shields.io/coderabbit/prs/github/wled/WLED?utm_source=oss&utm_medium=github&utm_campaign=wled%2FWLED&labelColor=171717&color=FF570A&link=https%3A%2F%2Fcoderabbit.ai&label=PR+Reviews+supported+by+CodeRabbit)

Join the Discord server to discuss everything about WLED!

<a href="https://discord.gg/QAh7wJHrRM"><img src="https://discordapp.com/api/guilds/473448917040758787/widget.png?style=banner2" width="25%"></a>

Check out the WLED [Discourse forum](https://wled.discourse.group)!

If you'd like to reach the original creator privately: [dev.aircoookie@gmail.com](mailto:dev.aircoookie@gmail.com).

If WLED brightens up your day, you can [send a gift to Aircoookie via PayPal](https://paypal.me/aircoookie).

---

*Disclaimer:*

If you are prone to photosensitive epilepsy, we recommend you do **not** use this software.  
If you still want to try, avoid strobe, lightning or noise modes and high effect speed settings.

As per the EUPL license, no liability is assumed for any damage to you or any other person or equipment.
