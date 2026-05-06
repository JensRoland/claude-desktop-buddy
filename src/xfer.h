#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <M5StickCPlus.h>
#include "ble_bridge.h"
#include "stats.h"

// Ack goes to both streams — we don't track which one delivered the command,
// and writes to a clientless BLE just drop. The bridge listens on whichever
// port it opened.
static void _xAck(const char* what, bool ok) {
  char b[64];
  int len = snprintf(b, sizeof(b), "{\"ack\":\"%s\",\"ok\":%s}\n", what, ok ? "true" : "false");
  Serial.write(b, len);
  bleWrite((const uint8_t*)b, len);
}

// Called from data.h when incoming JSON has a "cmd" key. Returns true if
// the command was handled (caller skips telemetry parsing). The character
// install / GIF transfer flow was removed in the landscape rewrite — we
// only handle name/owner/unpair/status.
inline bool xferCommand(JsonDocument& doc) {
  const char* cmd = doc["cmd"];
  if (!cmd) return false;

  if (strcmp(cmd, "name") == 0) {
    const char* n = doc["name"];
    if (n) petNameSet(n);
    _xAck("name", n != nullptr);
    return true;
  }

  if (strcmp(cmd, "owner") == 0) {
    const char* n = doc["name"];
    if (n) ownerSet(n);
    _xAck("owner", n != nullptr);
    return true;
  }

  if (strcmp(cmd, "unpair") == 0) {
    bleClearBonds();
    _xAck("unpair", true);
    return true;
  }

  if (strcmp(cmd, "status") == 0) {
    int vBat = (int)(M5.Axp.GetBatVoltage() * 1000);
    int iBat = (int)M5.Axp.GetBatCurrent();
    int vBus = (int)(M5.Axp.GetVBusVoltage() * 1000);
    int pct = (vBat - 3200) / 10;
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    char b[256];
    int len = snprintf(b, sizeof(b),
      "{\"ack\":\"status\",\"ok\":true,\"data\":{"
      "\"name\":\"%s\",\"owner\":\"%s\",\"sec\":%s,"
      "\"bat\":{\"pct\":%d,\"mV\":%d,\"mA\":%d,\"usb\":%s},"
      "\"sys\":{\"up\":%lu,\"heap\":%u}"
      "}}\n",
      petName(), ownerName(), bleSecure() ? "true" : "false",
      pct, vBat, iBat, (vBus > 4000) ? "true" : "false",
      millis() / 1000, ESP.getFreeHeap()
    );
    Serial.write(b, len);
    bleWrite((const uint8_t*)b, len);
    return true;
  }

  // permission cmd is sent FROM device TO desktop, never the other way; if
  // we see one inbound it's not ours and telemetry parsing should skip it
  // anyway. Anything else unknown: leave for telemetry parsing.
  return strcmp(cmd, "permission") == 0;
}
