# Claude Desktop Buddy, Millennial Edition

A wrist-readable fork of
[claude-desktop-buddy](https://github.com/anthropics/claude-desktop-buddy)
for those of us who already lived through the Tamagotchi era once and would
rather not squint at one again.

The original is great. It's also a portrait-mode pet sim with tiny text
and an animated capybara. This fork rotates the display 90° clockwise so
it sits readably on your wrist, blows up the approval-prompt text to a
size you can read at a glance, and rips out everything that wasn't a
permission decision.

If you want pets, GIFs, mood, leveling, and a lovingly-crafted Tamagotchi
homage, use the [original](https://github.com/anthropics/claude-desktop-buddy).
If you just want a wrist-glance device that buzzes when Claude wants to
run a tool, this is for you.

## What it shows

A single approval screen, plus a few status fallbacks:

- **Approval prompts** — tool name at size 3 (wraps to two lines for long
  names), hint underneath, `↑ Deny` / `Approve →` arrows that physically
  point at the buttons on the device
- **Status** — `running 3 sessions`, `waiting`, or `idle`
- **Pairing passkey** when first connecting
- **No Claude connected** when the desktop bridge isn't reachable

The screen auto-sleeps after 30s on battery and stays awake while an
approval is pending or while plugged in. The red LED pulses when there's
something to approve.

## What got cut

In rough order of how attached the original was to it:

- 18 ASCII pet species and their seven animations each
- GIF character system + over-the-air folder-push transfer
- Pet stats — mood, energy, hunger, level, accumulated tokens, level-up
  confetti
- Settings menu, info pages, factory-reset menu
- Charging clock face (portrait and landscape) with weekday/Friday-mood
- Shake-to-dizzy, face-down nap
- The whole seven-state animation engine

What's left: BLE pairing, JSON intake, approval handling, screen-off /
wake, and one big-text screen at a time. About 5,700 lines went away.

## Hardware

M5StickC Plus. Worn landscape with USB-C pointing right — `B` ends up on
the top edge of the device, `A` on the right of the screen. The
firmware uses the M5StickCPlus library for display, IMU, and buttons; if
you want a different board, you'll need to swap those drivers.

## Flashing

Install
[PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/),
then:

```bash
pio run -t upload
```

If you're flashing over an existing (upstream) install, erase NVS first
so old bonds and settings don't linger:

```bash
pio run -t erase && pio run -t upload
```

## Pairing

Same flow as upstream. Enable developer mode
(**Help → Troubleshooting → Enable Developer Mode**), open
**Developer → Open Hardware Buddy…**, click **Connect**, pick
`Claude-XXXX` from the list, and approve macOS's BLE prompt. A six-digit
passkey appears on the stick — type it on the desktop. Reconnects are
automatic after that.

If discovery isn't finding the stick, make sure it's awake (any button
press) and that you forgot any prior pairing in
**System Settings → Bluetooth**.

## Controls

| Button          | Action                                    |
| --------------- | ----------------------------------------- |
| **A** (right)   | Approve the pending prompt                |
| **B** (top)     | Deny the pending prompt                   |
| Power (bottom)  | Tap: screen off — Hold 6s: hard power off |

## Project layout

```
src/
  main.cpp        — landscape UI, screen routing, button handling
  ble_bridge.cpp  — Nordic UART service, line-buffered TX/RX
  data.h          — wire protocol parser, demo mode
  xfer.h          — desktop control commands (status, name, owner, unpair)
  stats.h         — NVS-backed name persistence
```

For the wire protocol, see [REFERENCE.md](REFERENCE.md). The protocol is
identical to upstream — only the device-side rendering and behavior
changed.

## Availability

The BLE API is exposed only when the Claude desktop app is in developer
mode. It's a maker-oriented feature on both forks; not a supported
product surface.
