# PicoCarPod firmware

Bluetooth A2DP receiver for Raspberry Pi Pico W that will emulate an Apple iPod on a
car's 30-pin dock cable (target: Volvo XC70 2008, "USB and iPod Music Interface").
Audio from the phone goes out over I2S (Waveshare Pico-Audio), metadata and
steering-wheel control will be bridged between AVRCP and the iPod Accessory Protocol
(iAP1, serial on dock pins 12/13).

## Status

Stage 1 (bench, source <-> Pico): **done, verified 2026-11-08** with a Linux PC (bluez/PipeWire)
as the A2DP source: auto-reconnect after reboot, 48 kHz SBC stream, metadata incl. UTF-8,
track-change notifications, next/prev/play/pause from the console. Phone test still pending.

- A2DP sink with SBC decoding, I2S output (`pico_audio_i2s`)
- AVRCP controller: title / artist / album / track numbers, play status, position
- AVRCP target: accepts absolute volume from the phone and ignores it (car controls volume)
- Remembers the last phone in flash (BTstack TLV) and **reconnects on its own** after
  boot and after a dropped link; auto-sends PLAY once AVRCP is up
- Pairing mode (SSP "just works") when no phone is remembered or on request
- USB serial console with status and single-key transport commands

Stage 1b: AVRCP Browsing client (`src/bt_browse.c`) - browsing channel, media player list, Now Playing
queue, folder navigation, PlayItem. **Finding (2026-11-08, Galaxy S24 + Spotify):** the phone exposes one
player "Media Player" that is *not browsable* (status 0x12) and a Now Playing queue of exactly one item
(the current track). So the playlist cannot be read over Bluetooth from this phone/app combination; a
companion app (Spotify App Remote SDK) would be needed for that. Console keys `l`, `P`, `r`, `u`, `0-9`, `<`, `>`
stay available to test other players/phones.

Stage 2 (car <-> Pico): iAP1 emulator on UART0 - not started, see `docs/iap1-notes.md`.

## Layout

```
src/main.c              init + BTstack run loop
src/bt_audio.c          A2DP sink, SBC decode, playback (from BTstack a2dp_sink_demo)
src/bt_avrcp.c          AVRCP controller/target, now_playing model, media control API
src/bt_browse.c         AVRCP Browsing controller (players, Now Playing queue, folders, PlayItem)
src/bt_conn.c           GAP, bonding storage, pairing mode, reconnect loop, autoplay, LED
src/console.c           USB serial console
src/now_playing.h       shared metadata model (consumer of the future iAP module)
src/btstack_audio_pico.c  I2S audio sink glue (pico-examples, BSD-3)
src/btstack_config.h    BTstack configuration (Classic only)
src/diag.c              watchdog, hard-fault/panic capture across reboots, BTstack log routing
tools/cockpit.html      Volvo display mock + car buttons; demo player or live Pico via Web Serial
lib/btstack_patches/    patched copy of BTstack a2dp.c (see "SDK bugs" below)
lib/pico-extras/        clone of raspberrypi/pico-extras (I2S driver), not tracked
```

## Known pico-sdk 2.2.0 / BTstack 1.6 bugs worked around

- `a2dp.c` stores remote stream endpoints in a 10-entry array without a bounds check. A Linux
  PipeWire source exposes 14+ source SEPs, which corrupts memory and hangs the Pico on a
  sink-initiated connection. We compile `lib/btstack_patches/a2dp.c` (limit 24 + bounds check)
  instead of the SDK file. Phones expose a handful of SEPs, so this only bit on the bench.
- The AVRCP controller never emits `NOW_PLAYING_INFO_DONE` for a single-packet
  GetElementAttributes reply, and the title/artist/album events are emitted with a size that
  excludes the text. `bt_avrcp.c` therefore collects attributes and commits them after a 150 ms
  quiet period instead of waiting for the done event.

Both exist in upstream BTstack as shipped with the SDK; re-check when bumping the SDK.

## Diagnostics

`src/diag.c` arms an 8 s hardware watchdog fed from the BTstack run loop, catches hard faults
and panics, stores PC/LR in watchdog scratch registers and prints them after the reboot
(`DIAG : previous run HARD FAULT at PC=...`, decode with `arm-none-eabi-addr2line -e build/picocarpod.elf`).
Build options: `-DPICOCARPOD_BT_LOG_INFO=ON` prints BTstack `log_info` messages (no packet dumps),
`-DPICOCARPOD_AVRCP_DEBUG=ON` prints every AVRCP controller subevent.

## Build

Requirements: Pico SDK 2.2.0 with BTstack (the VS Code Pico extension install in
`~/.pico-sdk` is picked up automatically), ARM GCC, CMake, Ninja, and pico-extras:

```sh
git clone --depth 1 https://github.com/raspberrypi/pico-extras lib/pico-extras
./build.sh              # -> build/picocarpod.uf2
./flash.sh              # 1200-baud reboot into BOOTSEL, picotool or UF2 copy
```

`build.sh` honours `PICO_SDK_PATH`, `PICO_TOOLCHAIN_PATH`, `PICO_EXTRAS_PATH` if set.

### I2S pins

Default is the Waveshare Pico-Audio **PCM5101A** revision: DIN=GP26, BCK=GP27, LRCK=GP28
(`pico_audio_i2s` needs LRCK = BCK + 1). Override with
`./build.sh -DPICOCARPOD_I2S_DATA_PIN=.. -DPICOCARPOD_I2S_CLOCK_PIN_BASE=..`.
The **Rev2.1 (CS4344)** board uses DIN=GP22, MCLK=GP26, LRCK=GP27, SCLK=GP28; it has the
clock order reversed and needs MCLK, so it requires a modified PIO program (not done).

## Serial console

115200 baud on the USB CDC port (`/dev/ttyACM0`). Without a terminal program:

```sh
stty -F /dev/ttyACM0 115200 raw -echo; cat /dev/ttyACM0 &
printf 's' > /dev/ttyACM0
```

| key | action |
|---|---|
| `h` | help |
| `s` | status (state, remembered phone, A2DP/AVRCP, now playing) |
| `p` | pairing mode (discoverable; 3 min when a phone is already remembered) |
| `f` | forget all paired phones |
| `c` | connect to the remembered phone now |
| `d` | disconnect and pause auto-reconnect until `c` |
| space | play / pause |
| `n` / `b` | next / previous track |
| `i` | re-query now playing info and play status |
| `a` | toggle autoplay after connect |
| `l` | AVRCP browsing: list the Now Playing queue |
| `P` | AVRCP browsing: re-list media players and pick one |
| `r` / `u` | AVRCP browsing: root folder / up one level (needs a browsable player) |
| `0`-`9`, `<`, `>` | open folder or PlayItem n of the last list, page by 10 |

LED: solid = connected, fast blink = pairing, short blip = reconnecting.

Besides the human-readable lines the console mirrors state as JSON, one object per line prefixed
with `@J `: `{"np":{status,title,artist,album,track,total,pos,len}}` on every metadata, status or
position change and `{"conn":{state,phone,a2dp,stream,avrcp,autoplay}}` on connection changes
(`s` prints both). Tools parse these instead of the text lines.

## Cockpit page (tools/cockpit.html)

A single HTML page that mimics the pre-facelift XC70 radio: the integrated monochrome dot-matrix
display drawn as a 1-bit 256 x 64 framebuffer (resolution estimated from a photo: 4:1 glass, 5x7 small
font, same font at 2x for the main line; `LCD_W`/`LCD_H` in the script), the climate row under the
rule, the radio keys (eject/AM/FM, CD/Mode, Phone/Menu, 4-way, Enter/Exit, Volume and Tuning knobs) and
the steering-wheel keypad. Menu shows the fake iPod lists the Lingo 0x04 emulator will expose. By default it runs a
demo player. Open it in Chrome or Edge straight from the repo, press "Connect Pico (Web Serial)",
pick the Pico's USB serial port, and the display follows the phone while the buttons send the console
keys (`n`, `b`, space). Keyboard: arrows, space, `m`, Esc, Enter.

Log example after pairing and starting Spotify:

```
CONN : state -> connected
AVRCP: connected to 11:22:33:44:55:66, cid 0x01
NOW  : [PLAYING] Artist - Title (Album) #3/12 0:00/3:45
```

## Bench test without a phone (Linux PC as source)

Pair from the PC (`bluetoothctl`: `scan on`, `pair`, `trust`, `connect 88:A2:...`), play anything
to the `bluez_output.*` sink. For metadata without a real player, register a test MPRIS player
directly with bluez over D-Bus (`org.bluez.Media1.RegisterPlayer`, MPRIS-style properties) - the
script used for the first verification lives outside the repo for now. Note bluez's
`mpris-proxy` did not pick up session-bus players on this machine.

## Bench test procedure (phone)

0. If the phone was paired with the earlier "A2DP Sink Demo" firmware, **forget that entry on the
   phone first**: it has the same Bluetooth address as PicoCarPod and hides the new name.

1. Flash, open the console, confirm `state : pairing`.
2. On the phone pair with "PicoCarPod". Expect `CONN : remembered phone -> ...`,
   `A2DP : signaling connected`, `AVRCP: connected`, then `NOW : ...` lines as tracks change.
3. Start Spotify; metadata should print on every track change, `n`/`b`/space must control it.
4. Reboot the Pico (unplug / replug USB). It must reconnect by itself within a few seconds
   and send PLAY (`CONN : autoplay - sending PLAY`).
5. Turn phone Bluetooth off and on: the Pico retries every few seconds and reconnects.
