# ePixC: the move to ESP-IDF 5, bench checklist

The founder does the bench work in this file. Agents do not write boards and do not run OTA tests.
Tick each box when the test passes. Write each measurement and each failure in the vault
(WLED Fork, and the production review decision row for "IDF 5 path").

## The target and the stages

Founder decision, 2026-11-13 (Production review 2026-11-10, decisions):

- Upstream WLED main at `01f74928`, the last commit before the "WLED Pixel Bus" LED driver.
- Arduino-ESP32 3.3.12 on ESP-IDF v5.5.5 (mbedTLS 3.6.6), from `platform = espressif32@8.0.0`.
  Fallback platform: pioarduino `55.03.312-1` (the same Espressif 3.3.12 libraries).
- Setup hotspot: WPA2/WPA3 mixed for batch 1, when the app's join path and the label QR support it.
- Fallback for batch 1: IDF 4.4.7 with the low-memory TLS guard. Cross to IDF 5 by OTA later.

| Stage | Who | What |
|---|---|---|
| 0 | Founder | One OTA on today's IDF 4.4 line (`17.0.1-pixc7`), before any IDF 5 image. |
| 1 | Agent, done | Merge of WLED main at `01f74928`. `PixC_V1` stays on IDF 4.4 (`espressif32@6.13.0`). |
| 1 bench | Founder | The merged build on IDF 4.4. This finds faults from the merge alone. |
| 2 | Agent, done | `espressif32@8.0.0`. Every ePixC env builds on IDF 5, and the box's image gates read the IDF 5 build. |
| 2, rest | Agent | mbedTLS 3 in the ePixC TLS paths (the pinned roots, the OTA signature check), the WPA2/WPA3 hotspot, the readers of `deviceId` in `/json/info`. |
| 2 bench | Founder | The IDF 5 build on a spare board, written by cable. |
| 3 | Founder | The first board crosses from IDF 4.4 to IDF 5 by OTA, with a forced revert. |
| 4 | Agent, then founder | Writer ticket 09: the secure bootloader from IDF v5.5.5, on a spare board. |

## What stage 1 changed

- The merge brings 585 upstream commits. Upstream builds its own S3 targets on IDF 5 now.
  Upstream also keeps an IDF 4.4 build (`esp32dev_V4`), so the merged tree still builds on IDF 4.4.
- `PixC_V1` takes upstream's IDF 4.4 flags and libraries (`[pixc_esp32s3_idf_V4]` and `lib_deps`
  in `platformio_override.ini`). Stage 2 removes that section.
- Libraries: ESPAsyncWebServer `ac44e32a` (was `39b830c8`, v2.4.2) and NeoPixelBus `1d7ff38f`
  (was `a0919d1c`), as upstream pins them. QuickESPNow is gone (upstream has `lib/wled_espnow`).
- Upstream fixes after `01f74928` that the merge also takes: the Gravimeter effect (`499e2ceb`)
  and the scrolling-text timer rollover (`a0858f71`).
- Boot (`WLED::beginStrip`): upstream blanks the strip first, then sets the boot brightness.
  The ePixC restore of the last state is unchanged. WLED's orange default is not used: the look
  comes from the saved state, or Aurora on a new light.
- New effect from upstream: Analog Clock 2D, id 220 (2D only). A strip shows no change. The app's
  effect catalogue (`tool/gen_wled_catalog.py`, `wled_catalog_test.dart`) must change too.
- `bash ci/test.sh` passes. The claims row `dmx-output-absent` reads the override too now: upstream
  names `WLED_ENABLE_DMX` once, to remove it from its ESP32-P4 builds.

Measured on the stage 1 build (`PixC_V1`, IDF 4.4.7, 2026-11-13):

| Item | Value |
|---|---|
| App image | 1,471,456 bytes of the 3,145,728-byte OTA slot (46.8 %) |
| Static RAM | 60,172 bytes (18.4 %) |
| `bootloader.bin` | 15,104 bytes (IDF 4.4.7, not changed) |

The study measured 1,462,544 bytes for the app image before the merge.

## What stage 2 changed

- `PixC_V1` uses `platform = espressif32@8.0.0`: Arduino-ESP32 3.3.12 on ESP-IDF v5.5.5
  (`b774170ff46`), mbedTLS 3.6.6, GCC 14.2. The fallback platform (pioarduino `55.03.312-1`) was
  not necessary. The platform names the Arduino core, the ESP32-S3 libraries and esptool by
  range; `platform_packages` pins the exact versions of this build.
- `PixC_V1` takes upstream's IDF 5 flags and libraries (`${esp32s3.build_flags}`):
  `WLED_USE_SHARED_RMT`, `ESP32_ARDUINO_NO_RGB_BUILTIN`, NeoPixelBus `76afe832` (the CORE3
  branch, IDF 5's RMT driver), and no NeoEsp32RmtHI. The section `[pixc_esp32s3_idf_V4]` is gone.
- Libraries that no ePixC image compiles are not in `lib_deps`: esp_dmx, IRremoteESP8266,
  AsyncMqttClient and the two GIF libraries.
- Faults of the new platform. The build showed no sign of the first two:
  - Its Arduino build script sets no `-O` flag. Every file compiled with `-O0`: the image was
    2,730,144 bytes (87 % of the slot), with deeper stacks and slower effects. `PixC_V1` sets
    `-Os` now, as Arduino 2.0.17 did.
  - `pio-scripts/dynarray.py` did not find the linker script, so the linker dropped the usermods'
    entries. The image linked and booted with no ePixC usermod and no mic usermod. The script
    finds it now, and a new gate (`ci/checks/usermods-registered.py`) stops a build without them.
  - The build stopped in two scripts: `pio-scripts/output_bins.py` on an empty define of the
    platform, and `pio-scripts/strip-floats.py` on its linker flags in lists.
- Code: the task watchdog is set to 20 s with IDF 5's call (`esp_task_wdt_reconfigure`; Arduino
  3.x has already started it at 5 s). The MQTT client uses IDF 5's nested esp-mqtt config, and
  registers its event handler on the client. The partition table is the same, byte for byte.
- CI: `nm.sh` reads the new toolchain (`toolchain-xtensa-esp-elf`). `bootloader-rollback.py` reads
  the sdkconfig of `framework-arduinoespressif32-esp32s3-libs`: bootloader and app rollback on,
  the certificate-bundle hook on, `MBEDTLS_DYNAMIC_FREE_CA_CERT` off, ESP-IDF v5.5.5.
  `rollback-later.sh` accepts only the strong `verifyRollbackLater`, not the framework's weak one.
- Not changed: the setup hotspot is WPA2 still, and the TLS code has the same logic.

Measured on the stage 2 build (`PixC_V1`, IDF 5.5.5, 2026-11-13):

| Item | Value |
|---|---|
| App image | 1,664,384 bytes of the 3,145,728-byte OTA slot (52.9 %) |
| Static RAM | 62,852 bytes (19.2 %) |
| `bootloader.bin` | 19,968 bytes (IDF 5.5.5, prebuilt) |

## Stage 0: one OTA on IDF 4.4

- [ ] The first board (`80b54ecf4940`) runs `17.0.1-pixc6`. Release `17.0.1-pixc7` and let the
      board update by OTA. Make sure that it reaches the broker after the update.

Do this before any IDF 5 image. Then a failure in stage 3 has one cause, not two.

## Stage 1 bench: the merged build on IDF 4.4

Release the merged build with a new version, or write it by cable to a spare board. Do not write
the first board by cable: its cable writes are over. Compare each result with `17.0.1-pixc6`.

- [ ] Boot. No orange or yellow flash at power-on.
- [ ] Saved state. Set a colour, wait 5 s, then remove the power. The light comes back with that
      colour. A new or reset light starts on Aurora.
- [ ] After a power cut. Set each of Last state, On and Off in the app. Each one does what it says.
- [ ] Setup. The blue run plays during setup, then the green run when the light reaches ePixC,
      then Aurora.
- [ ] Brightness cap. Set a power plan with a cap (`pixc.bri_max`). The cap holds from the app,
      from a preset and from the LAN.
- [ ] Limiter. Full white on the 5 m strip dims as on `17.0.1-pixc6`. The published amps are the
      same (`ABL_MILLIAMPS_DEFAULT` 4500).
- [ ] Mic. LytSync with the ePixC Mic reacts as on `17.0.1-pixc6`: the same sound, default gain
      and squelch. Upstream changed the audioreactive usermod. Try Gravimeter too.
- [ ] LAN. The app controls the light. An unsigned request gets 401. ePixC Sync streams DDP only
      while it holds a lease.
- [ ] Cloud. The light reaches `wss://api.epixc.in/mqtt` and reports its state and energy.
- [ ] OTA. The light installs a later build, and confirms it only when it reaches the broker.
- [ ] Factory reset. Five power-ons of 2 s to 5 s erase the light.
- [ ] Effects. The app's effect list on a strip has no new or missing effects.

## Stage 2 bench: the IDF 5 build on a spare board

Write the spare board by cable. It gets the IDF 5 bootloader. Do not use the first board.

- [ ] 1. Cold boot ten times. Look for "CONFLICT! driver_ng" aborts, watchdog and interrupt
      watchdog resets, and 8 MB octal PSRAM not found. On `PixC_V1_dev`, the serial log shows
      that the ePixC usermod and the mic usermod start.
- [ ] 2. 300 LEDs on GPIO16 under Wi-Fi load. Run fast effects, a Sync DDP stream at 60 fps, live
      power every 2 s and a TLS reconnect at the same time. Look for flicker (a scope if you can).
      The high-priority RMT interrupt (NeoEsp32RmtHI) is not in the IDF 5 build. The effects run as
      smoothly as on the IDF 4.4 build.
- [ ] 3. No orange flash at boot. The blue run, the green run and Aurora still play.
- [ ] 4. Limiter and energy: full white on 5 m. The published amps are the same as the IDF 4.4
      build.
- [ ] 5. Mic: the ICS-43434 on the new I2S driver. Compare with the IDF 4.4 build: the same sound,
      default gain and squelch, LytSync with the ePixC Mic. Upstream tested this driver on ESP32
      and ESP32-C3 only.
- [ ] 6. Pairing v2 on iOS and on Android: hotspot `ePixC-XXXX` with the label password, the retry
      after a wrong Wi-Fi password, 409 for a second phone, `_pixc._tcp` found by mDNS.
- [ ] 7. Hotspot WPA2/WPA3 mixed (when the rest of stage 2 sets it): join from iOS and from
      Android, by the label QR and by hand.
- [ ] 8. TLS:
  - [ ] Provisioning, and MQTT over `wss://api.epixc.in/mqtt`.
  - [ ] Certificate dates (D411): the Let's Encrypt expired, valid and revoked test hosts.
  - [ ] 50 forced MQTT reconnects and provisioning pulls. The pinned roots stay, and the heap
        does not leak.
  - [ ] The stack high-water mark of the esp-mqtt task during the handshake.
- [ ] 9. Signed LAN API: a replay, `STALE_BOOT`, the body gate, the 429 limits. DDP only with a
      lease.
- [ ] 10. 24 h soak with the lwIP thread-safety asserts on (`LWIP_CHECK_THREAD_SAFETY`): SNTP,
      LAN API, DDP and MQTT together. Record `heap_min`, `heap_largest` and `mqtt_dropped`.
- [ ] 11. Watchdog: 20 s never trips in normal use. A hang that you cause trips it after 20 s,
      not after 5 s (Arduino 3.x starts the watchdog at 5 s, and WLED sets it to 20 s).
- [ ] 12. Factory reset: five power-ons, and the reset from the cloud.
- [ ] 13. A panic that you cause on the dev build: the core dump fits in 64 KB, and its summary
      reads back.
- [ ] 14. Wi-Fi starts on the DC jack with no brownout.

Measure and record in the vault:

| Item | IDF 4.4 (stage 1) | IDF 5 |
|---|---|---|
| App image (slot 3,145,728 bytes) | 1,471,456 bytes | 1,664,384 bytes |
| Static RAM | 60,172 bytes | 62,852 bytes |
| Free internal heap after boot | | |
| Largest free internal block after boot | | |
| `bootloader.bin` | 15,104 bytes | 19,968 bytes |

## Stage 3: the first board crosses by OTA

The first board keeps its IDF 4.4.7 bootloader. Espressif: always test with the bootloader that
is in use. An IDF 4.4.7 bootloader starts an app from a newer IDF.

- [ ] a. OTA from the IDF 4.4 build to the IDF 5 build. These stay: `cfg.json`, `wsec.json`, the
      presets, the energy log, `pixc_fac` (the label password) and the `pixc_ota` floor. After a
      revert, the IDF 4.4 image still mounts the file system that the IDF 5 image wrote.
- [ ] b. Forced revert:
  - [ ] An IDF 5 image with a wrong broker host goes back to the IDF 4.4 image. The keys and the
        Wi-Fi stay.
  - [ ] An IDF 5 image that crashes at boot goes back to the IDF 4.4 image.
- [ ] c. OTA from IDF 5 to IDF 5, on both bootloaders (IDF 4.4.7 and IDF 5).
- [ ] d. The light refuses a downgrade and a bad signature. It survives a power cut during the
      download.
- [ ] e. A forward re-issue back to the IDF 4.4 source works (readme, "Re-issue an older
      release"). This works only on a light with the IDF 4.4.7 bootloader: an IDF 5 bootloader
      does not start an app from an older IDF.

## Stage 4: the secure bootloader (Writer ticket 09)

Use a spare board. Never the first board.

- [ ] Build the bootloader from ESP-IDF v5.5.5 (`b774170ff46`), not 4.4.7: secure boot v2
      (RSA-3072), flash encryption in release mode, log level ERROR or NONE.
- [ ] Measure its size against the 0x8000 limit, with padding and the 4 KB signature sector. If it
      does not fit, the partition table offset must move before batch 1. Then the bench boards need
      one more cable write, and the Writer, the CSV, `ci/flash-files.py` and the factory NVS at
      0x9000 change too.
- [ ] A board with flash encryption burned runs the prebuilt app.

## If a blocker cannot be fixed before batch 1

Ship IDF 4.4.7 with the low-memory guard before each TLS connection, and cross by OTA later.
This stays possible on secured units too: a 4.4.7 bootloader starts IDF 5 apps. The guard is not
in the code yet (fw-small-fixes left it for this case): add it before such a release.
