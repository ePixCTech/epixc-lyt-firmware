#pragma once

// ePixC: after a power cut the light comes back as it was left (the app's "After a power cut").
// Compiled with PIXC_LAN_AUTH.
//
// The state (on or off, brightness, every segment's effect, palette and colours) is saved to
// /last.json a few seconds after it settles, and read back in beginStrip(), before the first
// frame. With no saved state (a new or factory-reset light) the look is Aurora, not WLED's orange.
//
// After a power cut (`pixc.restore` on /cfg, kept in cfg.json):
//   last  everything as it was, on or off and the brightness too (the default)
//   on    the saved look, turned on at the default brightness (def.bri)
//   off   the saved look, off until someone turns it on

#include <stdint.h>

// beginStrip(): apply the saved state, or Aurora when there is none. True when on/off and the
// brightness came with it (restore is `last` and a state was saved), so the boot defaults are skipped.
bool pixcRestoreLastState();

// Any state change: saved once it has settled.
void pixcLastStateChanged();

// Every loop pass. `hold`: a temporary look is showing (the connect blink), which is not saved.
void pixcLastStateLoop(bool hold);

// "last", "on" or "off". False for anything else, or when it is already the mode.
bool pixcSetRestoreMode(const char* mode);
const char* pixcRestoreMode();
