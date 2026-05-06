#include <M5StickCPlus.h>
#include "ble_bridge.h"
#include "data.h"

// Landscape rewrite: USB-C points right (rotation 3). The sprite buffer is
// allocated in landscape coordinates (240×135) and pushes to the rotated
// LCD as-is. The original portrait UI with animated buddies, pet stats,
// info pages, settings, and charging clock was stripped out — this build
// is approval-prompt-first, with the transcript scroller as the default
// idle view. See REFERENCE.md for the bridge protocol.

TFT_eSprite spr = TFT_eSprite(&M5.Lcd);

const int W = 240, H = 135;
const int CX = W / 2, CY = H / 2;
const int LED_PIN = 10;          // red LED, active-low

// CL_ prefix to dodge TFT_eSPI's many color macros (CL_OK, RED, etc.)
const uint16_t CL_BG     = 0x0000;
const uint16_t CL_FG     = 0xFFFF;
const uint16_t CL_DIM    = 0x8410;
const uint16_t CL_HOT    = 0xFA20;
const uint16_t CL_OK     = 0x07E0;
const uint16_t CL_ACCENT = 0x07FF;
const uint16_t CL_M5     = 0xC180;       // rusty brick (~#C03000)

// Advertise as "Claude-XXXX" (last two BT MAC bytes) so multiple sticks in
// one room are distinguishable in the desktop picker.
static char btName[16] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

TamaState tama;
char     lastPromptId[40] = "";
uint32_t promptArrivedMs  = 0;
bool     responseSent     = false;
bool     screenOff        = false;
uint32_t lastInteractMs   = 0;
uint8_t  brightLevel      = 4;
const uint32_t SCREEN_OFF_MS = 30000;

uint8_t  msgScroll   = 0;       // lines back from newest in transcript
uint16_t lastLineGen = 0;

// Append-only ring buffer of distinct status lines we've seen on the wire.
// The desktop snapshot replaces tama.lines/tama.msg on every update; without
// this, the transcript view would only ever show whatever's in the latest
// snapshot. We dedupe against the most recent entry so the heartbeat doesn't
// fill the buffer with duplicates of the same state.
const uint8_t HIST_N = 12;
const uint8_t HIST_LEN = 80;
char     history[HIST_N][HIST_LEN];
uint8_t  historyHead  = 0;     // next write index
uint8_t  historyCount = 0;     // saturates at HIST_N

// Sentinel bytes that tag entries with a glyph kind. Stored in byte 0 so
// the rest of the row remains plain ASCII and dedupe / wrap stay simple.
// Values are < 0x20 so they can't collide with anything the desktop might
// send.
static const char TOOL_MARK    = '\x01';   // wrench   — tool call
static const char DONE_OK_MARK = '\x02';   // green ✓  — done (success)
static const char DONE_FAIL_MARK= '\x03';  // red   ✗  — done (failure)

static void historyAppend(const char* s) {
  if (!s || !s[0]) return;
  if (strcmp(s, "(no messages)") == 0) return;     // never display the placeholder

  // Pattern → sentinel rewrites. Each compresses a verbose desktop line
  // into one row with a glyph drawn at render time.
  char buf[HIST_LEN];
  size_t len = strlen(s);
  if (len > 9 && strncmp(s, "(called ", 8) == 0 && s[len - 1] == ')') {
    // "(called WebSearch)" → "\x01WebSearch"
    size_t nameLen = len - 9;
    if (nameLen > HIST_LEN - 2) nameLen = HIST_LEN - 2;
    buf[0] = TOOL_MARK;
    memcpy(buf + 1, s + 8, nameLen);
    buf[1 + nameLen] = 0;
    s = buf;
  } else if (strncmp(s, "done (success)", 14) == 0) {
    // "done (success), 8 turns" → "\x02done, 8 turns"
    snprintf(buf, sizeof(buf), "%cdone%s", DONE_OK_MARK, s + 14);
    s = buf;
  } else if (strncmp(s, "done (failure)", 14) == 0 ||
             strncmp(s, "done (error)", 12) == 0 ||
             strncmp(s, "done (fail)", 11) == 0) {
    size_t skip = (s[6] == 'f' && s[7] == 'a' && s[8] == 'i' && s[9] == 'l' && s[10] == 'u') ? 14 :
                  (s[6] == 'e') ? 12 : 11;
    snprintf(buf, sizeof(buf), "%cdone%s", DONE_FAIL_MARK, s + skip);
    s = buf;
  }

  if (historyCount > 0) {
    uint8_t prev = (historyHead + HIST_N - 1) % HIST_N;
    if (strncmp(history[prev], s, HIST_LEN - 1) == 0) return;
  }
  strncpy(history[historyHead], s, HIST_LEN - 1);
  history[historyHead][HIST_LEN - 1] = 0;
  historyHead = (historyHead + 1) % HIST_N;
  if (historyCount < HIST_N) historyCount++;
}

static void applyBrightness() { M5.Axp.ScreenBreath(20 + brightLevel * 20); }

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    M5.Axp.SetLDO2(true);
    applyBrightness();
    screenOff = false;
  }
}

static void beep(uint16_t freq, uint16_t dur) { M5.Beep.tone(freq, dur); }

// Decorative brand stripe across the top of every full screen, drawn after
// fillSprite so it survives the per-frame clear.
static void drawTopBar() {
  spr.fillRect(0, 0, W, 2, CL_M5);
}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}

// Greedy word-wrap into fixed-width rows. Word boundaries are spaces;
// when a word is too wide for one row we look for a "soft" separator
// (/ \ . - _) inside the budget and break there before falling back to
// a hard-break. That keeps file paths readable: `src/main.cpp` will
// split as `src/` + `main.cpp`, not `src/ma` + `in.cpp`. Returns rows
// written.
static uint8_t wrapInto(const char* in, char out[][32], uint8_t maxRows, uint8_t width) {
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;
    if (!*p) break;
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;

    // Doesn't fit on current line — wrap unless we're already fresh.
    uint8_t sp = (col > 0) ? 1 : 0;
    if (col + sp + wlen > width && col > 0) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      col = 0; sp = 0;
    }

    // Word still too long for one row → soft-break at a separator if we
    // can find one in the right half of the budget; otherwise hard-break.
    while (wlen > width - col - sp) {
      if (sp) { out[row][col++] = ' '; sp = 0; }
      uint8_t budget = width - col;
      uint8_t take = budget;
      for (int8_t k = (int8_t)budget - 1; k > (int8_t)budget / 2; k--) {
        char c = w[k];
        if (c == '/' || c == '\\' || c == '-' || c == '_' || c == '.' || c == ',') {
          take = k + 1;   // include the separator on this row
          break;
        }
      }
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      col = 0;
    }
    if (sp) out[row][col++] = ' ';
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

// Battery percent + USB indicator, drawn at top-right at size 1.
static void drawBatteryTopRight() {
  int vBat = (int)(M5.Axp.GetBatVoltage() * 1000);
  int pct = (vBat - 3200) / 10;
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;
  bool usb = M5.Axp.GetVBusVoltage() > 4.0f;
  char bat[10];
  snprintf(bat, sizeof(bat), "%s%d%%", usb ? "+" : "", pct);
  spr.setTextSize(1);
  spr.setTextColor(usb ? CL_OK : CL_DIM, CL_BG);
  int batW = strlen(bat) * 6;
  spr.setCursor(W - batW - 4, 4);
  spr.print(bat);
}

static void drawApproval() {
  spr.fillSprite(CL_BG); drawTopBar();

  // Top: timer. Goes CL_HOT after 10s of waiting.
  spr.setTextSize(1);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  spr.setTextColor(waited >= 10 ? CL_HOT : CL_DIM, CL_BG);
  spr.setCursor(4, 4);
  spr.printf("approve?  %lus", (unsigned long)waited);

  // Tool name dominates the screen — always size 3, wrapped to 2 lines at
  // 13 chars (240/18 ≈ 13). Long names get a second line instead of being
  // shrunk or truncated.
  spr.setTextSize(3);
  spr.setTextColor(CL_FG, CL_BG);
  char toolWrap[2][32];
  uint8_t toolRows = wrapInto(tama.promptTool, toolWrap, 2, 13);
  const int toolY = 16;
  for (uint8_t i = 0; i < toolRows; i++) {
    spr.setCursor(4, toolY + i * 26);
    spr.print(toolWrap[i]);
  }
  int toolEnd = toolY + toolRows * 26;

  // Hint underneath at size 2, wrapped to 2 lines at 19 chars. Two-line
  // tool names leave less room — when the tool wraps to two lines we
  // cap the hint at one line so nothing collides with the buttons.
  spr.setTextSize(2);
  spr.setTextColor(CL_DIM, CL_BG);
  char hintWrap[2][32];
  uint8_t hintMax = (toolRows >= 2) ? 1 : 2;
  uint8_t hintRows = wrapInto(tama.promptHint, hintWrap, hintMax, 19);
  int hintY = toolEnd + 4;
  for (uint8_t i = 0; i < hintRows; i++) {
    spr.setCursor(4, hintY + i * 18);
    spr.print(hintWrap[i]);
  }

  // Bottom row buttons. Worn on the wrist with USB-C right, the B button
  // sits on the top edge of the device and the A button sits to the right
  // of the screen — so deny gets an up-arrow on its left, approve gets a
  // right-arrow on its right. Layout follows physical positions to make
  // the action obvious without reading. "sent..." replaces both after a
  // response goes out.
  spr.setTextSize(2);
  if (responseSent) {
    spr.setTextColor(CL_DIM, CL_BG);
    spr.setCursor(4, H - 18);
    spr.print("sent...");
  } else {
    const int yText = H - 18;
    const int aTop = H - 17, aBot = H - 3, aMid = H - 10;   // 14px arrow

    // Left: ↑ Deny  (B button — top edge)
    spr.fillTriangle(4, aBot, 18, aBot, 11, aTop, CL_HOT);
    spr.setTextColor(CL_HOT, CL_BG);
    spr.setCursor(24, yText);
    spr.print("Deny");

    // Right: Approve →  (A button — right side)
    const int approveW = 7 * 12;                  // "Approve" at size 2
    const int arrowX = W - 14 - 4;                // 14px arrow + 4px right margin
    const int approveX = arrowX - 4 - approveW;
    spr.setTextColor(CL_OK, CL_BG);
    spr.setCursor(approveX, yText);
    spr.print("Approve");
    spr.fillTriangle(arrowX, aTop, arrowX, aBot, arrowX + 14, aMid, CL_OK);
  }
}

static void drawPasskey() {
  spr.fillSprite(CL_BG); drawTopBar();
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(1); spr.setTextColor(CL_DIM, CL_BG);
  spr.drawString("BLUETOOTH PAIRING", CX, 14);
  spr.setTextSize(3); spr.setTextColor(CL_FG, CL_BG);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.drawString(b, CX, 60);
  spr.setTextSize(1); spr.setTextColor(CL_DIM, CL_BG);
  spr.drawString("enter on desktop", CX, 110);
  spr.setTextDatum(TL_DATUM);
}

static void drawDisconnected() {
  spr.fillSprite(CL_BG); drawTopBar();
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(3); spr.setTextColor(CL_DIM, CL_BG);
  spr.drawString("No Claude", CX, CY - 14);
  spr.drawString("connected", CX, CY + 14);
  spr.setTextDatum(TL_DATUM);
  drawBatteryTopRight();
}

static void drawIdle() {
  spr.fillSprite(CL_BG); drawTopBar();
  drawBatteryTopRight();

  spr.setTextDatum(MC_DATUM);
  if (tama.sessionsWaiting > 0) {
    spr.setTextSize(3); spr.setTextColor(CL_HOT, CL_BG);
    spr.drawString("waiting", CX, CY - 12);
    spr.setTextSize(2); spr.setTextColor(CL_DIM, CL_BG);
    char s[24]; snprintf(s, sizeof(s), "%u session%s",
                        tama.sessionsWaiting, tama.sessionsWaiting == 1 ? "" : "s");
    spr.drawString(s, CX, CY + 22);
  } else if (tama.sessionsRunning > 0) {
    spr.setTextSize(3); spr.setTextColor(CL_FG, CL_BG);
    spr.drawString("running", CX, CY - 12);
    spr.setTextSize(2); spr.setTextColor(CL_DIM, CL_BG);
    char s[24]; snprintf(s, sizeof(s), "%u session%s",
                        tama.sessionsRunning, tama.sessionsRunning == 1 ? "" : "s");
    spr.drawString(s, CX, CY + 22);
  } else {
    spr.setTextSize(3); spr.setTextColor(CL_DIM, CL_BG);
    spr.drawString("idle", CX, CY);
  }
  spr.setTextDatum(TL_DATUM);
}

static void drawTranscript() {
  spr.fillSprite(CL_BG); drawTopBar();
  spr.setTextSize(2);
  const int LH = 16;
  const int WIDTH_CHARS = 19;
  const int SHOW = 7;             // 7 × 16 = 112px, leaves room for top margin

  if (historyCount == 0) { drawIdle(); return; }

  // Wrap each history entry oldest→newest into a flat row buffer. age=0
  // marks rows from the most recent entry (rendered in white); older ages
  // get dimmed.
  static char disp[24][32];
  static uint8_t age[24];
  uint8_t nDisp = 0;
  for (int8_t a = (int8_t)historyCount - 1; a >= 0 && nDisp < 24; a--) {
    uint8_t idx = (historyHead + HIST_N - 1 - a) % HIST_N;
    char wrap[2][32];
    uint8_t got = wrapInto(history[idx], wrap, 2, WIDTH_CHARS);
    for (uint8_t j = 0; j < got && nDisp < 24; j++) {
      strncpy(disp[nDisp], wrap[j], 31); disp[nDisp][31] = 0;
      age[nDisp] = (uint8_t)a;
      nDisp++;
    }
  }

  // Newest at bottom; scrolling moves the window backward (toward older).
  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int end = (int)nDisp - (int)msgScroll;
  int start = end - SHOW; if (start < 0) start = 0;

  for (int i = start; i < end; i++) {
    bool fresh = (age[i] == 0) && (msgScroll == 0);
    uint16_t fg = fresh ? CL_FG : CL_DIM;
    int yRow = 4 + (i - start) * LH;       // 2px gap below the brand bar
    spr.setTextColor(fg, CL_BG);
    char tag = disp[i][0];
    if (tag == TOOL_MARK || tag == DONE_OK_MARK || tag == DONE_FAIL_MARK) {
      const int ix = 4, iy = yRow;
      uint16_t mark = fresh ? (tag == DONE_FAIL_MARK ? CL_HOT
                              : tag == DONE_OK_MARK  ? CL_OK
                                                     : CL_ACCENT) : CL_DIM;
      if (tag == TOOL_MARK) {
        // Open-end wrench: ring head + handle going right.
        spr.fillCircle(ix + 4, iy + 5, 4, mark);
        spr.fillCircle(ix + 4, iy + 5, 2, CL_BG);
        spr.fillRect(ix + 7, iy + 8, 9, 3, mark);
      } else if (tag == DONE_OK_MARK) {
        // Checkmark: short stroke down-right + long stroke up-right, drawn
        // 2px thick by repeating each stroke offset by one row.
        for (int t = 0; t < 2; t++) {
          spr.drawLine(ix + 1, iy + 7 + t, ix + 5, iy + 11 + t, mark);
          spr.drawLine(ix + 5, iy + 11 + t, ix + 14, iy + 2 + t, mark);
        }
      } else {
        // X: two diagonals, 2px thick.
        for (int t = 0; t < 2; t++) {
          spr.drawLine(ix + 2, iy + 2 + t, ix + 13, iy + 13 + t, mark);
          spr.drawLine(ix + 13, iy + 2 + t, ix + 2, iy + 13 + t, mark);
        }
      }
      spr.setCursor(ix + 20, yRow);
      spr.print(disp[i] + 1);
    } else {
      spr.setCursor(4, yRow);
      spr.print(disp[i]);
    }
  }

  if (msgScroll > 0) {
    spr.setTextSize(1);
    spr.setTextColor(CL_ACCENT, CL_BG);
    spr.setCursor(W - 24, H - 10);
    spr.printf("-%u", msgScroll);
  }
}

void setup() {
  M5.begin();
  M5.Lcd.setRotation(1);          // landscape, USB-C right
  M5.Beep.begin();
  startBt();
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);    // off
  applyBrightness();
  lastInteractMs = millis();

  petNameLoad();
  spr.createSprite(W, H);

  spr.fillSprite(CL_BG); drawTopBar();
  spr.setTextDatum(MC_DATUM);
  spr.setTextSize(3); spr.setTextColor(CL_FG, CL_BG);
  if (ownerName()[0]) {
    char line[40]; snprintf(line, sizeof(line), "Hi %s", ownerName());
    spr.drawString(line, CX, CY - 12);
  } else {
    spr.drawString("Hello!", CX, CY - 12);
  }
  spr.setTextSize(1); spr.setTextColor(CL_DIM, CL_BG);
  spr.drawString("Claude Buddy", CX, CY + 16);
  spr.setTextDatum(TL_DATUM);
  spr.pushSprite(0, 0);
  delay(1200);
}

void loop() {
  M5.update();
  M5.Beep.update();
  uint32_t now = millis();

  dataPoll(&tama);

  bool inPrompt = tama.promptId[0] && !responseSent;

  // Prompt arrival: chirp, wake, reset response state.
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      beep(1200, 80);
    }
  }

  if (tama.lineGen != lastLineGen) {
    msgScroll = 0;
    lastLineGen = tama.lineGen;
    wake();
  }

  // Mirror the latest activity into our local ring buffer so the transcript
  // view accumulates history instead of getting clobbered on each snapshot.
  // Prefer the last `entries` row (richer) over the short `msg` field, and
  // skip while disconnected so the offline placeholder doesn't pile up.
  if (tama.connected) {
    const char* latest = nullptr;
    if (tama.nLines > 0)      latest = tama.lines[tama.nLines - 1];
    else if (tama.msg[0])     latest = tama.msg;
    historyAppend(latest);
  }

  if (M5.BtnA.isPressed() || M5.BtnB.isPressed()) wake();

  // AXP power button (now physically on the bottom in landscape): toggle
  // screen off. Long-press (6s) still hard-powers via AXP hardware.
  if (M5.Axp.GetBtnPress() == 0x02) {
    if (screenOff) wake();
    else { M5.Axp.SetLDO2(false); screenOff = true; }
  }

  // BtnA = approve when in prompt, else scroll transcript backward.
  if (M5.BtnA.wasReleased()) {
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd),
               "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}",
               tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      beep(2400, 60);
    } else {
      msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 3;
      beep(1800, 30);
    }
  }

  // BtnB = deny when in prompt, else reset scroll to newest.
  if (M5.BtnB.wasPressed()) {
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd),
               "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}",
               tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      beep(600, 60);
    } else if (msgScroll > 0) {
      msgScroll = 0;
      beep(1800, 30);
    }
  }

  // LED pulses while waiting for a response — the wrist-glance "look at me".
  if (inPrompt) {
    digitalWrite(LED_PIN, (now / 400) % 2 ? LOW : HIGH);
  } else {
    digitalWrite(LED_PIN, HIGH);
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;

  if (!screenOff) {
    if (pk)                          drawPasskey();
    else if (tama.promptId[0])       drawApproval();    // covers in-prompt and "sent..."
    else if (!tama.connected)        drawDisconnected();
    else if (historyCount > 0)       drawTranscript();
    else                             drawIdle();
    spr.pushSprite(0, 0);
  }

  // Auto screen-off on battery only — clock face went away with the rewrite,
  // so there's no reason to keep it lit while charging either; but keep the
  // existing "USB stays awake" rule so a desk-side stick acts like a status
  // monitor.
  bool onUsb = M5.Axp.GetVBusVoltage() > 4.0f;
  if (!screenOff && !inPrompt && !onUsb && (millis() - lastInteractMs > SCREEN_OFF_MS)) {
    M5.Axp.SetLDO2(false);
    screenOff = true;
  }

  delay(screenOff ? 100 : 16);
}
