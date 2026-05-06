#pragma once
#include <Arduino.h>
#include <Preferences.h>

// Header-only with file-static state: include from exactly one translation
// unit (main.cpp). The file is kept under its old name to minimize churn —
// the pet stats / level / mood system was removed in the landscape rewrite,
// leaving only the persistent-name plumbing.

static Preferences _prefs;

static char _petName[24]   = "Buddy";
static char _ownerName[32] = "";

// Strip JSON-breaking chars — these names go into a printf'd JSON string
// unescaped (xfer.h status response). A quote persists to NVS and breaks
// the status endpoint until the name is re-set.
static void _safeCopy(char* dst, size_t dstLen, const char* src) {
  size_t j = 0;
  for (size_t i = 0; src[i] && j < dstLen - 1; i++) {
    char c = src[i];
    if (c != '"' && c != '\\' && c >= 0x20) dst[j++] = c;
  }
  dst[j] = 0;
}

inline void petNameLoad() {
  _prefs.begin("buddy", true);
  _prefs.getString("petname", _petName, sizeof(_petName));
  _prefs.getString("owner",   _ownerName, sizeof(_ownerName));
  _prefs.end();
}

inline void petNameSet(const char* name) {
  _safeCopy(_petName, sizeof(_petName), name);
  _prefs.begin("buddy", false);
  _prefs.putString("petname", _petName);
  _prefs.end();
}

inline void ownerSet(const char* name) {
  _safeCopy(_ownerName, sizeof(_ownerName), name);
  _prefs.begin("buddy", false);
  _prefs.putString("owner", _ownerName);
  _prefs.end();
}

inline const char* petName()   { return _petName; }
inline const char* ownerName() { return _ownerName; }
