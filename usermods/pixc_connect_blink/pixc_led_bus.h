#pragma once

// -----------------------------------------------------------------------------
// How wide the LED bus is, and who is allowed to decide.
//
// ePixC sells a 12 V RGB reel and a 12 V RGBW reel. Same protocol, same wiring, same controller,
// one firmware image. The only difference that reaches this code is how many bytes each pixel
// eats: three for RGB, four for RGBW.
//
// Until this file existed that number was a BUILD-TIME constant — `-D LED_TYPES=TYPE_WS2812_RGB`
// in the PixC_V1 env (platformio_override.ini). Which means the RGBW SKU could only ever have
// worked if it shipped a different image, and it does not: `default_envs` names one env and CI
// builds one binary. An RGBW reel on a three-channel bus is not "the white die stays dark" — it
// is worse than that. The reel latches 32 bits per pixel and is fed 24, so every pixel after the
// first reads its colour out of its neighbour's bytes and the tail of the reel never latches at
// all. The strip is visibly wrong, not merely less colourful. (Predicted from the protocol; see
// the bench note at the bottom of this comment.)
//
// So the ownership boundary moves one notch:
//
//    the cloud owns the WIDTH    — it is a property of the reel in the box, and only the cloud
//                                  knows which SKU shipped to which customer
//    the device owns the WIRING  — pin, length, colour order, reversal, current limits. The cloud
//                                  has never known any of it and must not start.
//
// That is the same split already drawn between `led_chip` (cloud-set, implies voltage) and
// `led_channels` (device-reported, from strip.hasWhiteChannel()), pushed one step further: the
// cloud now *sets* the width instead of only being told what it turned out to be.
//
// Doing it this way — a two-value string rather than a whole `hw.led.ins` bus descriptor from the
// server — is what keeps a bad push from being an RMA. WLED's own deserializer treats an `ins`
// array as authoritative and replaces every bus with it (wled00/cfg.cpp:217-255, then
// FX_fcn.cpp:1176 BusManager::removeAll()). A server that sends bus descriptors therefore sends
// pin numbers, and one wrong pin number is a dark strip recoverable only with a cable. A server
// that sends "RGB" or "RGBW" cannot express a pin at all. The worst a wrong value can do here is
// put the reel on the wrong width — visibly wrong, and fixed by the next push over the same MQTT
// connection that delivered the bad one.
//
// NOT YET PROVEN ON HARDWARE. Everything above about how an SK6812-family reel behaves when
// under-fed is read off the protocol, not off a bench unit. Ticket 75.
// -----------------------------------------------------------------------------

// Same fallback the core uses, so this header can be compiled in an env that does not set the
// flag. See wled00/cfg.cpp:17-19.
#ifndef LED_TYPES
  #define LED_TYPES DEFAULT_LED_TYPE
#endif

namespace pixc_led_bus {

  // The widths ePixC sells, and what each one needs from WLED.
  //
  // TYPE_SK6812_RGBW is a WIDTH, not a part number. WLED names its bus types after the commonest
  // chip that speaks each protocol; ePixC stocks LC8808B, LC8816E and LC8824, none of which is an
  // SK6812 or a WS2812, and all of which speak the same single-wire protocol at three or four
  // bytes per pixel. Selecting by channel count rather than by chip name is also the only mapping
  // the server can supply: its strip table records `channels`, not a protocol.
  //
  // The auto-white mode is part of the width because it is meaningless without it. On an RGBW bus
  // the white byte can come from two places, and the mode decides which:
  //
  //   RGBW_MODE_MANUAL_ONLY (0)  W is whatever the app put in col[0][3] and nothing else. Every
  //                              effect that does not set W leaves the white die dark, which for
  //                              almost all of them means always — WLED's palette lookup returns
  //                              W=0 (colors.cpp:144) and only the segment's own colour slot
  //                              carries a white value through (FX_fcn.cpp:1156).
  //   RGBW_MODE_AUTO_BRIGHTER(1) W is derived from RGB and the app's white value is IGNORED. This
  //                              is what the app's LAN push has been sending, and it quietly makes
  //                              the white slider — which is plumbed end to end, all the way to
  //                              ColorDto.w in the api — do nothing.
  //   RGBW_MODE_DUAL (3)         The app's white value wins whenever it is non-zero; when it is
  //                              zero the bus derives white from RGB. Both halves of the product
  //                              are then true at once: the slider works, and an effect that never
  //                              thought about white still lights the white die.
  //
  // DUAL also keeps SEG_CAPABILITY_W set (FX_fcn.cpp:1015-1023), which is what tells the app and
  // WLED's own UI that this strip has a white channel worth offering a control for. BRIGHTER and
  // ACCURATE clear it and hide the slider.
  struct Width {
    const char* channels;   // the string the cloud sends and the device announces
    uint8_t     type;       // WLED TYPE_* bus type
    uint8_t     autoWhite;  // RGBW_MODE_* — ignored by WLED on a bus with no white (bus_manager.h:125)
  };

  static constexpr Width kWidths[] = {
    { "RGB",  TYPE_WS2812_RGB,  RGBW_MODE_MANUAL_ONLY },
    { "RGBW", TYPE_SK6812_RGBW, RGBW_MODE_DUAL        },
  };
  static constexpr size_t kWidthCount = sizeof(kWidths) / sizeof(kWidths[0]);

  // constexpr because the static_assert below has to be answerable by the compiler. Same reason
  // pixc_ota_pubkey.h rolls its own startsWith().
  constexpr bool sameText(const char* a, const char* b) {
    return *a == *b && (*a == '\0' || sameText(a + 1, b + 1));
  }

  constexpr int indexOfType(uint8_t type, size_t i = 0) {
    return i >= kWidthCount ? -1 : (kWidths[i].type == type ? (int)i : indexOfType(type, i + 1));
  }

  // -----------------------------------------------------------------------------
  // The guard this whole ticket exists because nobody had.
  //
  // The gap that shipped was not a bug in a line of code. It was two halves of one decision living
  // in places that could not see each other: the bus width was fixed in a build flag, and the
  // thing that was supposed to change it (the cloud config push) did not know the key existed. Two
  // writers, no overlap, no error — the build was green the entire time.
  //
  // So: the compiled-in default must be a width this runtime table can also produce. If someone
  // changes LED_TYPES in platformio_override.ini to a type the push path cannot express, the image
  // does not build. If someone adds a width to kWidths, nothing breaks. The asymmetry is
  // deliberate — the compiled value is the one a device boots with before the cloud has ever
  // spoken to it, so it is the one that must always be reachable afterwards.
  //
  // A static_assert rather than a CI step because CI is skippable and a compiler is not, and
  // because this must also hold for the bench env (PixC_V1_dev extends PixC_V1 and inherits the
  // flag). The matching half — that the server only ever sends a string in this table — is
  // asserted on the other side, in TheCfgPushCarriesTheStripWidthTest.
  // -----------------------------------------------------------------------------
  namespace guard {
    constexpr unsigned kCompiledTypes[] = {LED_TYPES};
    constexpr size_t kCompiledCount = sizeof(kCompiledTypes) / sizeof(kCompiledTypes[0]);

    constexpr bool everyCompiledTypeIsPushable(size_t i = 0) {
      return i >= kCompiledCount
           ? true
           : (indexOfType((uint8_t)kCompiledTypes[i]) >= 0 && everyCompiledTypeIsPushable(i + 1));
    }
  }

  static_assert(guard::everyCompiledTypeIsPushable(),
    "LED_TYPES names a bus type that pixc_led_bus::kWidths cannot produce. The compiled-in bus is "
    "what a device runs until the cloud tells it otherwise, so every value it can take has to be a "
    "value the cloud can also set — otherwise a unit can end up on a width that no config push is "
    "able to correct, which is exactly the state the RGBW SKU shipped in. Either add the width to "
    "kWidths above (and to the server's strip table, or the string will never arrive), or put "
    "LED_TYPES back to one of the types already listed.");

  // The cloud sends "RGB" or "RGBW". Anything else returns nullptr and is ignored — an unknown
  // width must leave the strip exactly as it was, because the alternative is guessing at the one
  // setting that decides whether the reel renders at all.
  //
  // Case-insensitive on the way in: the string round-trips through Postgres and a JSON encoder
  // before it gets here, and refusing a device's only chance to be configured correctly over a
  // lowercase 'w' would be an absurd way to lose a unit.
  inline const Width* find(const char* channels) {
    if (channels == nullptr || channels[0] == '\0') return nullptr;
    for (size_t i = 0; i < kWidthCount; i++) {
      if (strcasecmp(channels, kWidths[i].channels) == 0) return &kWidths[i];
    }
    return nullptr;
  }

}  // namespace pixc_led_bus
