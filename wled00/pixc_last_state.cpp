#include "wled.h"

#ifdef PIXC_LAN_AUTH

#include "pixc_last_state.h"
#include "pixc_device.h"
#include "pixc_lan.h"

namespace {
constexpr const char* kFile = "/last.json";
constexpr const char* kTemp = "/last.tmp";
// A state with every segment in use is a few kB; anything far larger is not one we wrote.
constexpr size_t kMaxBytes = 16 * 1024;
// Saved once the state has been still this long (a slider being dragged is one save), and no more
// often than every kSpacingMs, so a routine or a stream of changes cannot wear the flash. A change
// inside the spacing is still saved, at its end.
constexpr unsigned long kQuietMs = 3000;
constexpr unsigned long kSpacingMs = 20000;

enum class Mode : uint8_t { Last, On, Off };
Mode mode = Mode::Last;

bool dirty = false;
unsigned long changedAt = 0;
unsigned long savedAt = 0;
bool savedOnce = false;
uint32_t savedSum = 0;   // of the bytes in /last.json, so an unchanged state is not written again

uint32_t fnv1a(const uint8_t* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
  return h;
}

// Write under a temporary name, then rename over the old file: a power cut mid-write leaves the
// previous state whole, never half of the new one.
void save() {
  if (!requestJSONBufferLock(JSON_LOCK_PRESET_SAVE)) return;   // busy: try again next pass
  JsonObject root = pDoc->to<JsonObject>();
  serializeState(root, true, true, true);
  const size_t n = measureJson(*pDoc);
  char* buf = (n > 0 && n <= kMaxBytes) ? (char*)malloc(n + 1) : nullptr;
  if (buf != nullptr) serializeJson(*pDoc, buf, n + 1);
  releaseJSONBufferLock();
  dirty = false;
  if (buf == nullptr) return;

  const uint32_t sum = fnv1a((const uint8_t*)buf, n);
  if (sum != savedSum || !WLED_FS.exists(kFile)) {
    File f = WLED_FS.open(kTemp, "w");
    const size_t wrote = f ? f.write((const uint8_t*)buf, n) : 0;
    if (f) f.close();
    if (wrote == n && (WLED_FS.rename(kTemp, kFile) || (WLED_FS.remove(kFile) && WLED_FS.rename(kTemp, kFile)))) {
      savedSum = sum;
    } else {
      WLED_FS.remove(kTemp);
      DEBUG_PRINTLN(F("[ePixC] last state not saved"));
    }
  }
  free(buf);
  savedAt = millis();
  savedOnce = true;
}

}  // namespace

void pixcFirstLook() {
  strip.getMainSegment().setMode(FX_MODE_AURORA, true);
}

bool pixcRestoreLastState() {
  bool applied = false;
  bool full = false;
  File f = WLED_FS.open(kFile, "r");
  if (f && f.size() > 0 && f.size() <= kMaxBytes && requestJSONBufferLock(JSON_LOCK_PRESET_LOAD)) {
    const size_t n = f.size();
    char* buf = (char*)malloc(n + 1);
    if (buf != nullptr && f.read((uint8_t*)buf, n) == n) {
      buf[n] = 0;
      if (!deserializeJson(*pDoc, buf, n) && pDoc->is<JsonObject>()) {
        JsonObject root = pDoc->as<JsonObject>();
        // The same rules as a stored preset: no "win", no reboot (pixc_lan_logic.h).
        pixcSanitizeState(root, true);
        root.remove(F("transition"));
        full = mode == Mode::Last && root["on"].is<bool>();
        if (!full) { root.remove("on"); root.remove("bri"); }
        deserializeState(root, CALL_MODE_INIT);
        savedSum = fnv1a((const uint8_t*)buf, n);
        applied = true;
      }
    }
    free(buf);
    releaseJSONBufferLock();
  }
  if (f) f.close();
  if (!applied) pixcFirstLook();
  return full;
}

void pixcLastStateChanged() {
  dirty = true;
  changedAt = millis();
}

void pixcLastStateLoop(bool hold) {
  if (!dirty || hold || pixcFactoryResetPending()) return;
  const unsigned long now = millis();
  if (now - changedAt < kQuietMs) return;
  if (savedOnce && now - savedAt < kSpacingMs) return;
  save();
}

bool pixcSetRestoreMode(const char* m) {
  if (m == nullptr) return false;
  Mode next;
  if (strcmp(m, "last") == 0) next = Mode::Last;
  else if (strcmp(m, "on") == 0) next = Mode::On;
  else if (strcmp(m, "off") == 0) next = Mode::Off;
  else return false;
  if (next == mode) return false;
  mode = next;
  return true;
}

const char* pixcRestoreMode() {
  switch (mode) {
    case Mode::On: return "on";
    case Mode::Off: return "off";
    default: return "last";
  }
}

#endif  // PIXC_LAN_AUTH
