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
| 0 | Founder | One OTA on today's IDF 4.4 line (`17.0.1-pixc7`, the tree of `9af1bf20`), before any IDF 5 image. |
| 1 | Agent, done | Merge of WLED main at `01f74928`. `PixC_V1` stays on IDF 4.4 (`espressif32@6.13.0`). |
| 1 bench | Founder | The merged build on IDF 4.4 (the tree of `36dc95eb`). This finds faults from the merge alone. |
| 2 | Agent, done | `espressif32@8.0.0`. Every ePixC env builds on IDF 5, and the box's image gates read the IDF 5 build. |
| 2, rest | Agent, done | mbedTLS 3 in the ePixC TLS paths (the pinned roots, the OTA signature check), the WPA2/WPA3 hotspot, the readers of `deviceId` in `/json/info`, and two more box gates. |
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
- Not changed in this part: the setup hotspot and the TLS code (the next section).

Measured on the stage 2 build (`PixC_V1`, IDF 5.5.5, 2026-11-13):

| Item | Value |
|---|---|
| App image | 1,664,384 bytes of the 3,145,728-byte OTA slot (52.9 %) |
| Static RAM | 62,852 bytes (19.2 %) |
| `bootloader.bin` | 19,968 bytes (IDF 5.5.5, prebuilt) |

## What the rest of stage 2 changed

TLS on mbedTLS 3.6.6 (commit `fw-idf5-3`):

- OTA image check: mbedTLS 3 returns an error code from each SHA-256 call. `doOta` checks each
  one. If a call fails, the light stops the update and reports "image digest failed"
  (`OTA_FAILED`). It does not compare or verify a digest that it did not calculate.
- OTA signature check: not changed. `mbedtls_pk_parse_public_key` and `mbedtls_pk_verify` work as
  in mbedTLS 2.28. The framework does not use PSA crypto for them (`MBEDTLS_USE_PSA_CRYPTO` off).
- Pinned roots and certificate dates (D411), for provisioning, the OTA download and MQTT over WSS:
  not changed. esp-tls in IDF 5.5.5 sets verification REQUIRED and then calls `pixcTlsAttach`
  (read from its `libesp-tls.a`). It does not read the hook's return value: if the roots do not
  parse, the hook sets no CA chain, and mbedTLS refuses the handshake. The certificate date fields
  are still public in mbedTLS 3.6. Two framework settings must stay as they are, and the box gate
  `ci/checks/bootloader-rollback.py` now checks both:
  - `CONFIG_MBEDTLS_HAVE_TIME_DATE` off. If it is on, mbedTLS checks the dates against the clock,
    which shows 1970 until SNTP answers, and refuses each handshake before SNTP.
  - `CONFIG_MBEDTLS_DYNAMIC_FREE_CA_CERT` off (stage 2 added this check).
- The low-memory TLS guard is not added. It is the work-around for CVE-2025-27810 on mbedTLS 2.28,
  and mbedTLS 3.6.3 and later have the fix. Add it only for the IDF 4.4 fallback.

Setup hotspot:

- The hotspot is WPA2/WPA3 mixed (transition mode): `WIFI_AUTH_WPA2_WPA3_PSK`, cipher CCMP, PMF not
  required. The name and the label password do not change. The label QR (`WIFI:T:WPA;...`) does not
  change: `T:WPA` is correct for WPA2 and WPA3.
- A phone that joins with WPA3 (SAE) is safe from a passive listener who knows the label password.
  A phone that joins with WPA2 is not safe from that listener, as before.
- The app's join paths:
  - iOS: `NEHotspotConfiguration` with the passphrase. It is correct for WPA2 and WPA3 networks.
  - Android: `wifi_iot` asks for a WPA2 network (`WifiNetworkSpecifier.setWpa2Passphrase`). A
    transition-mode hotspot accepts it. The bench must find which mode Android uses. To make
    Android use WPA3, the app must ask for WPA3 (`setWpa3Passphrase`). That is a change in the app,
    not in this build.
- The IDF 4.4 framework has no WPA3 for its hotspot. The code uses WPA2 there, so the fallback
  build still opens a hotspot. The box gate requires `CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT` for a
  release.

`deviceId` in `/json/info`:

- WLED's `deviceId` (`getDeviceId`, `wled00/util.cpp`) is a hash of the MAC and chip data. On the
  ESP32-S3 the chip data changes on IDF 5, so a light shows a different `deviceId` after it
  crosses.
- No ePixC code reads it. Checked on 2026-11-13: epixc-app (it reads `mac`, `ip`, `wifi`, `leds`,
  `live`, `lip` and `lm` from `/json/info`; the identity of a light is its MAC), epixc-sync,
  epixc-backend, epixc-web, epixc-status and epixc-writer. Only WLED's web UI reads it (its usage
  report), and no ePixC image has that UI. No change is necessary.

Box gates (`ci/build.sh`):

- New: `ci/checks/build-version.py`. The version in the image's WLED build description must be
  the version in `package.json`. Each light reports this version as `fw_version`, and the box
  names the release with the `package.json` version. If `pio-scripts/set_metadata.py` does not
  set it, upstream's placeholder `17.0.0-devV5` goes into the image with only a warning.
- `ci/checks/bootloader-rollback.py`: the two TLS settings above, and WPA3 for the hotspot.

## Stage 0: one OTA on IDF 4.4

The source of `17.0.1-pixc7` is the tree of commit `9af1bf20`. It is `17.0.1-pixc6` with the fixes
of review 2026-11-13, on IDF 4.4, before the upstream merge. Do not make it from a `master` that
has the IDF 5 commits: that is an IDF 5 image.

The box's build image must have `espressif32@6.13.0` when the box builds stages 0 and 1. The
epixc-backend commit fw-idf5 changes the image to `espressif32@8.0.0`. Deploy that commit only
after the box built stages 0 and 1. If it is already deployed, first change the image to the
IDF 4.4 pins. The readme tells how ("Re-issue an older release", "Before you start").

- [ ] Make `17.0.1-pixc7` on `master`. If `master` is at `17.0.1-pixc6`, fast-forward it to
      `9af1bf20` (`git merge --ff-only 9af1bf20`). Then do steps 3 to 6 of the readme's "Re-issue
      an older release". If `master` has the IDF 5 commits, do steps 2 to 6 with
      `--source=9af1bf20`. After the release, revert the re-issue commit (step 8).
- [ ] The first board (`80b54ecf4940`) runs `17.0.1-pixc6`. Deploy `17.0.1-pixc7`, and let the
      board update by OTA. Make sure that it reaches the broker after the update.

Do this before any IDF 5 image. Then a failure in stage 3 has one cause, not two.

## Stage 1 bench: the merged build on IDF 4.4

The source of the merged build is the tree of commit `36dc95eb`. The box's build image must have
`espressif32@6.13.0` (stage 0). Do not write the first board by cable: its cable writes are over.
Compare each result with `17.0.1-pixc6`.

- [ ] After stage 0, release the merged build with the next version. Merge `36dc95eb` into
      `master` (`git merge 36dc95eb`), then do readme steps 3 to 7. If `master` has the IDF 5
      commits, do readme steps 2 to 7 with `--source=36dc95eb`. After the release, revert the
      re-issue commit (step 8). You can also write the build by cable to a spare board.

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

Use a spare board. Do not use the first board: its cable writes are over.

- [ ] Build the IDF 5 release on the box (a new `package.json` version, tag `lyt-v<version>`). Do
      not deploy it in the console yet: the first board would update.
- [ ] Write the spare board by cable with the Writer, as a new board. Write the bootloader, the
      partition table, `boot_app0.bin` and the app (`flash.json`). Then write the factory data
      (the hotspot password) and print the label. The board gets the IDF 5 bootloader. A release
      image with no factory password opens no hotspot. The Writer must first accept the IDF 5
      bootloader (19,968 bytes, `esp-idf: v5.5.5`; the Writer's part of fw-idf5).
- [ ] Pair the spare board in the app. Then do the tests below.

- [ ] 1. Cold boot ten times. Look for "CONFLICT! driver_ng" aborts, watchdog and interrupt
      watchdog resets, and 8 MB octal PSRAM not found. On `PixC_V1_dev`, the serial log shows
      that the ePixC usermod and the mic usermod start.
- [ ] 2. 300 LEDs on GPIO16 under Wi-Fi load. Run fast effects and a Sync DDP stream at 60 fps.
      At the same time, read live power every 2 s and cause a TLS reconnect. Look for flicker (a
      scope if you can).
      The high-priority RMT interrupt (NeoEsp32RmtHI) is not in the IDF 5 build. The effects run as
      smoothly as on the IDF 4.4 build.
- [ ] 3. No orange flash at boot. The blue run, the green run and Aurora still play.
- [ ] 4. Limiter and energy: full white on 5 m. The published amps are the same as the IDF 4.4
      build.
- [ ] 5. Mic: the ICS-43434 on the new I2S driver. Compare with the IDF 4.4 build: the same sound,
      default gain and squelch, LytSync with the ePixC Mic. Upstream tested this driver on ESP32
      and ESP32-C3 only.
- [ ] 6. Pairing v2 on iOS and on Android. Test the hotspot `ePixC-XXXX` with the label password,
      and the retry after a wrong Wi-Fi password. Test 409 for a second phone, and `_pixc._tcp`
      found by mDNS.
- [ ] 7. Hotspot, WPA2/WPA3 mixed (do it again after a factory reset, item 12):
  - [ ] On a Mac, hold Option and click the Wi-Fi menu. The hotspot shows "WPA2/WPA3 Personal".
        Join it by hand with the label password: the Mac shows "WPA3 Personal" for the connection.
  - [ ] iPhone: pair in the app, once by the label QR and once by the password typed by hand.
  - [ ] Android: pair in the app, once by the label QR and once by the password typed by hand. In
        the phone's Wi-Fi details, write down the mode it shows for the hotspot (WPA2 or WPA3).
        The app asks for WPA2, so WPA2 is the expected result. Write it in the vault.
  - [ ] A phone or a laptop with no WPA3 joins the hotspot (WPA2).
  - [ ] Each pairing completes: the blue run, then the green run.
- [ ] 8. TLS:
  - [ ] Provisioning, and MQTT over `wss://api.epixc.in/mqtt`.
  - [ ] Certificate dates (D411): the Let's Encrypt expired, valid and revoked test hosts.
  - [ ] 50 forced MQTT reconnects and provisioning pulls. The pinned roots stay, and the heap
        does not leak.
  - [ ] The stack high-water mark of the esp-mqtt task during the handshake.
- [ ] 9. Signed LAN API: a replay, `STALE_BOOT`, the body gate, the 429 limits. DDP only with a
      lease.
- [ ] 10. 24 h soak. The framework has the lwIP thread-safety asserts on
      (`LWIP_CHECK_THREAD_SAFETY`): a call from the wrong task stops the light, and the crash
      report shows the assert. For 24 h, run SNTP, the LAN API, a Sync DDP stream and MQTT
      together. For the LAN API, keep the app open on the light. Record `heap_min`, `heap_largest`
      and `mqtt_dropped` from the light's health reports at the start, after 1 h, and every few
      hours.
  - [ ] No restart in the 24 h (the uptime in the health report increases all the time).
  - [ ] `heap_min` and `heap_largest` do not decrease from hour 1 to hour 24.
  - [ ] `mqtt_dropped` does not increase without a cause that you know.
- [ ] 11. Watchdog: 20 s never trips in normal use. A hang that you cause trips it after 20 s,
      not after 5 s. Arduino 3.x starts the watchdog at 5 s, and WLED sets it to 20 s.
- [ ] 12. Factory reset: five power-ons, and the reset from the cloud.
- [ ] 13. Cause a panic on the dev build. The core dump fits in 64 KB, and its summary reads back.
      The dump has 9 hex digits of the ELF SHA-256 (`CONFIG_APP_RETRIEVE_LEN_ELF_SHA` is 9 on
      IDF 5.5.5). The light finds the image with those digits and sends 16.
  - [ ] The `panic` of the next boot event has `"reverted":false` and an `elf` of 16 hex
        digits. `sha256sum` of the build's `firmware.elf` starts with these digits.
  - [ ] Cause a panic on an image that is on probation (after an OTA, before it gets to the
        broker). The bootloader starts the previous image. Its boot event has `"reverted":true`,
        and `panic` has the `elf`, `fw_version` and `fw_build` of the image that crashed. The
        `fw_version` and `fw_build` of the event are those of the image that runs.
  - [ ] In both cases, write down the time in the `crash dump:` line on the serial port
        ("found in N ms"). After a revert, the light reads the other slot to find the build
        number of the image that crashed.
  - The console does not show `elf`; the edge keeps the whole event in `device_events`.
- [ ] 14. Wi-Fi starts on the DC jack with no brownout.

Measure and record in the vault:

| Item | IDF 4.4 (stage 1) | IDF 5 |
|---|---|---|
| App image (slot 3,145,728 bytes) | 1,471,456 bytes | 1,664,464 bytes (`fw-idf5-3`) |
| Static RAM | 60,172 bytes | 62,852 bytes |
| Free internal heap after boot | | |
| Largest free internal block after boot | | |
| `bootloader.bin` | 15,104 bytes | 19,968 bytes |

## Stage 3: the first board crosses by OTA

The first board keeps its IDF 4.4.7 bootloader. Espressif: always test with the bootloader that
is in use. An IDF 4.4.7 bootloader starts an app from a newer IDF.

Do b first: it puts the board back on IDF 4.4. Then a crosses it for good. Use the IDF 5 release
that passed the stage 2 bench.

- [ ] Before: the first board runs the IDF 4.4 release of stage 0 and reaches the broker. Write
      down its version, its colour, one preset and its energy history in the app.
- [ ] b. Forced revert. A new image is on probation until it reaches the broker. A restart before
      that, or 60 minutes of Wi-Fi with no broker, puts the previous image back.
  - [ ] Deploy the IDF 5 release in the console. When the console shows "installing" (95 %), cut
        the internet at the router. Pull its internet cable, or pause the light's internet in the
        router's app. Keep the Wi-Fi on. The light restarts on the IDF 5 image and cannot reach the
        broker. The console may not show "done", because the internet is cut.
  - [ ] b1. Remove the power for 10 s, then apply it. A restart on probation is the same for the
        bootloader as a crash at boot: the light starts the IDF 4.4 image.
  - [ ] Connect the internet again. In the console, the light's boot event shows the IDF 4.4
        version. The light reaches the broker with no new pairing: the keys and the Wi-Fi stay.
        The backend then offers the IDF 5 release again, and the light starts the update.
  - [ ] b2. When the console shows "installing" again, cut the internet again, and keep the
        power on. Set "After a power cut" to Last state. During the first minutes, set a new
        colour from the app on the home Wi-Fi (the LAN route). Wait 5 s: the IDF 5 image writes
        `/last.json`. After 60 minutes of Wi-Fi with no broker (`kRollbackWifiUpMs`), the light
        puts the IDF 4.4 image back by itself and restarts. It starts with the new colour: the
        IDF 4.4 image mounts the file system that the IDF 5 image wrote.
  - The third rule (no broker 15 minutes after the API confirmed a login) needs the API up and
    the broker down. Do not cause that on the production server.
- [ ] a. Connect the internet, and let the light update to the IDF 5 release. It reaches the
      broker and confirms the image. The console shows the IDF 5 version for the light (the
      `fw_version` that `ci/checks/build-version.py` checks). These stay: `cfg.json`, `wsec.json`,
      the presets, the energy log, `pixc_fac` (the label password) and the `pixc_ota` floor.
      WLED's `deviceId` in `/json/info` changes; no ePixC code reads it.
- [ ] Remove the power after it confirmed. It starts on the IDF 5 image again: a confirmed image
      is not put back.
- [ ] c. OTA from IDF 5 to IDF 5, on both bootloaders (IDF 4.4.7 and IDF 5).
- [ ] d. The light refuses a downgrade and a bad signature. It survives a power cut during the
      download.
- [ ] e. A forward re-issue back to the IDF 4.4 source works. Follow the readme's "Re-issue an
      older release". First change the box's image to the IDF 4.4 pins ("Before you start"). If
      the source has no `image_home`, take `ci/build.sh` from `36dc95eb` (step 2). This works
      only on a light with the IDF 4.4.7 bootloader. An IDF 5 bootloader does not start an app
      from an older IDF.

## Stage 4: the secure bootloader (Writer ticket 09)

Use a spare board. Never the first board.

- [ ] Build the bootloader from ESP-IDF v5.5.5 (`b774170ff46`), not 4.4.7. Use secure boot v2
      (RSA-3072), flash encryption in release mode and log level ERROR or NONE.
- [ ] Measure its size against the 0x8000 limit, with padding and the 4 KB signature sector. If it
      does not fit, the partition table offset must move before batch 1. Then the bench boards need
      one more cable write, and the Writer, the CSV, `ci/flash-files.py` and the factory NVS at
      0x9000 change too.
- [ ] A board with flash encryption burned runs the prebuilt app.

## If a blocker cannot be fixed before batch 1

Ship IDF 4.4.7 with the low-memory guard before each TLS connection, and cross by OTA later.
This stays possible on secured units too: a 4.4.7 bootloader starts IDF 5 apps. The guard is not
in the code yet (fw-small-fixes left it for this case): add it before such a release. On that
build the setup hotspot is WPA2 only, because the IDF 4.4 framework has no WPA3 hotspot. Also
change `ci/checks/bootloader-rollback.py` with it: it reads the IDF 5 package layout. The box's
image must have the IDF 4.4 pins (readme, "Re-issue an older release", "Before you start").
