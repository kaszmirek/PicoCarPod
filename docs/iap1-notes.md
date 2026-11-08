# iAP1 / Volvo research notes

Collected 2026-11-08 (web research, no bus sniffing yet). Confirmed facts vs. guesses are marked.

## Target interface in the car

- Volvo P3 (S80 2007+, V70/XC70 2008+) accessory "USB and iPod Music Interface":
  MOST module, software/part 31210410 (USB+iPod variant), cable 31316394 (split: USB-A socket + 30-pin plug),
  kit 30758238/30758239, harness 31201152. Controlled from steering wheel / radio, shows text on the radio display.
- **Strong evidence it is serial iAP over the 30-pin (not USB):** the PodEmu Android iPod emulator was
  developed by its author on a 2008 Volvo S80 (same platform and interface), in serial mode, with
  steering-wheel control and track info working. A Volvo service bulletin says the iPod cable only provides
  12 V charging, which matches dock-connector serial/analog mode. Not verified on the XC70 itself.
- Volvo quirk reported by the PodEmu author: the head unit **repeatedly sends PLAY while paused**, which can
  block other commands. PodEmu has a "mimicAlwaysPlay" option (report PLAYING to the car even when paused).
- Pin 18 must be pulled up to 3.3 V by the "iPod" for some cars to detect it. Do not tie audio GND (pin 2)
  to power GND at the device; audio from pins 3/4 (line out), serial on 12 (iPod TX) / 13 (iPod RX), 3.3 V TTL.
- Baud: default 19200 8N1. PodEmu used 57600 with its serial modules; BMWPyPod starts at 19200 and switches to
  57600 after Lingo 4 cmd 0x12. The Volvo rate is unknown: implement autobaud on the 0xFF sync byte or try both.

## Protocol essentials

Frame: `FF 55 <len> <lingo> <cmd...> <payload> <checksum>`, checksum makes sum(len..checksum) == 0 mod 256,
big-endian fields, Lingo 4 uses 2-byte command IDs, len 0x00 + 2-byte length for payloads > 255 bytes.

### Lingo 0x00 (General) seen from head units (PodEmu / BMWPyPod handlers)

| cmd | name | reply |
|---|---|---|
| 0x01 | Identify (legacy, lingo byte follows: 0x04 = Extended) | none |
| 0x03 | RequestRemoteUIMode | 0x04 + (1 if extended else 0) |
| 0x05 / 0x06 | Enter / ExitRemoteUIMode | ACK 0x02 00 cmd |
| 0x07 | RequestiPodName | 0x08 + string |
| 0x09 | RequestiPodSoftwareVersion | 0x0A + major minor rev |
| 0x0B | RequestiPodSerialNum | 0x0C + string |
| 0x0D | RequestiPodModelNum | 0x0E + u32 model + string (PodEmu: "MB754") |
| 0x0F | RequestLingoProtocolVersion (lingo) | 0x10 + lingo major minor |
| 0x13 | IdentifyDeviceLingoes (u32 lingo mask, u32 options, u32 device id) | ACK 0x02 00 13; bit 4 = Extended Interface -> switch to AiR mode. PodEmu then sends Lingo0 `27 01` (GetAccessoryInfo); real iPod repeats it after 5 s |
| 0x24 | GetiPodOptions | 0x25 + 8 zero bytes |
| 0x28 | RetAccessoryInfo | none |

Authentication (0x14-0x1F) is accessory->iPod; emulators simply never challenge and cars accept that.
ACK format: `0x02 <status> <cmd>`; status 0x00 OK, 0x04 bad parameter.

### Lingo 0x04 (Extended Interface) - what head units use

| cmd | name | payload / reply |
|---|---|---|
| 0x12 | RequestProtocolVersion | 0x13 major minor (1.x) |
| 0x14 | RequestiPodName | 0x15 string |
| 0x16 | ResetDBSelection | ACK |
| 0x17 | SelectDBRecord | cat(1) idx(4); -1 = up one level |
| 0x18 | GetNumberCategorizedDBRecords | cat(1) -> 0x19 count(4) |
| 0x1A | RetrieveCategorizedDatabaseRecords | cat(1) start(4) count(4) -> one 0x1B idx(4)+string per record |
| 0x1C | GetPlayStatus | -> 0x1D length_ms(4) position_ms(4) state(1: 0 stop,1 play,2 pause) |
| 0x1E | GetCurrentPlayingTrackIndex | -> 0x1F idx(4) |
| 0x20/0x22/0x24 | GetIndexedPlayingTrackTitle/Artist/Album | idx(4) -> 0x21/0x23/0x25 string |
| 0x0C | GetIndexedPlayingTrackInfo | type(1) idx(4) chapter(2) -> 0x0D type + data (type 0: caps(4) length_ms(4) chapters(2)) |
| 0x26 | SetPlayStatusChangeNotification | 1 byte (old) or 4-byte mask (new) -> ACK |
| 0x27 | PlayStatusChangeNotification (iPod -> car) | 0x00 stopped, 0x01 track changed + idx(4), 0x04 position + ms(4), 0x02/0x03 FF/REW stop |
| 0x28 | PlayCurrentSelection | idx(4) -> ACK |
| 0x29 | PlayControl | 01 toggle, 02 stop, 03 next, 04 prev, 05 FF, 06 REW, 07 end FF/REW, 08/09 next/prev chapter |
| 0x2C/0x2E | Get/SetShuffle | 0x2D value |
| 0x2F/0x31 | Get/SetRepeat | 0x30 value |
| 0x32 | SetDisplayImage | ACK (ignore data) |
| 0x33 / 0x39 | GetMono/ColorDisplayImageLimits | 0x34 / 0x3A width height format |
| 0x35 | GetNumPlayingTracks | 0x36 count(4) |
| 0x37 | SetCurrentPlayingTrack | idx(4) -> ACK (some HUs use this for next/prev) |
| 0x38 | SelectSortDBRecord | cat(1) idx(4) sort(1) |

Categories: 1 playlist, 2 artist, 3 album, 4 genre, 5 track, 6 composer, 7 audiobook, 8 podcast.

Metadata trick used by all emulators: present a fake "now playing" list (N tracks, current index),
serve title/artist/album for the current index from AVRCP, and on an AVRCP track change send
`0x27 01 <new index>` so the head unit re-queries 0x20/0x22/0x24. Send `0x27 04 <ms>` ~every 500 ms.

## References

- PodEmu (Android iPod emulator, Volvo S80 2008 dev car): https://github.com/xtensa/PodEmu
  (`app/src/main/java/com/rp/podemu/OAPMessenger.java`), author notes: https://www.drive2.ru/c/523991233865973985/
- BMWPyPod-CircuitPython (RN52 + MCU bridge, same idea): https://github.com/00b/BMWPyPod-CircuitPython (`PyPod.py`)
- Rockbox iPod-side iAP implementation with spec quotes: https://github.com/Rockbox/rockbox/tree/master/apps/iap
- oandrew/ipod (Go, USB iAP, has lingo-extremote command definitions): https://github.com/oandrew/ipod
- ipodlinux wiki "Apple Accessory Protocol": https://web.archive.org/web/2012/http://ipodlinux.org/wiki/Apple_Accessory_Protocol
- 30-pin pinout: https://pinoutguide.com/PortableDevices/ipod_pinout.shtml
- Volvo bulletin (iPod charging 12 V via dock cable): https://manuals.autorepairshq.com/Volvo/2007/S80%20AWD%20V8-4.4L%20VIN%2085%20B8444S/Repair%20and%20Diagnosis/Technical%20Service%20Bulletins/Customer%20Interest/Audio%20System%20-%20IPod%28R%29%20Does%20Not%20Charge
- Commercial equivalents: Bovee Tune2Air WMA3000 (30-pin BT iPod emulator), reseller claims Volvo 2006 V70R works.
