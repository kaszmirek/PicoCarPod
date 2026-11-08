# PicoCarPod

Bluetooth-to-iPod adapter for cars with a 30-pin iPod dock cable. A Raspberry Pi Pico W
receives music from the phone over Bluetooth (A2DP), plays it through an I2S DAC
(Waveshare Pico-Audio) and will emulate an Apple iPod on the car's dock connector (iAP1),
so the head unit shows track metadata and the steering-wheel buttons control the phone.

Target car: Volvo XC70 2008 ("USB and iPod Music Interface", pre-facelift P3 radio).

## Status

| part | status |
|---|---|
| Firmware, stage 1: Bluetooth A2DP/AVRCP sink, auto-reconnect | done (2026-11-08) |
| Firmware, stage 2: iAP1 emulator (car side) | not started |
| Hardware (PCB) | not started, bench setup is Pico W + Pico-Audio |
| Mechanical (enclosure) | not started |

## Repository layout

```
firmware/     Pico W firmware (C, Pico SDK 2.2.0 + BTstack), build/flash scripts, tools
hardware/     PCB design (KiCad): schematic, layout, own libraries, production files
mechanical/   enclosure: CAD sources and STEP/STL exports
docs/         project-wide notes (car interface, iAP1 protocol research)
```

Each part has its own README with details. Start with [firmware/README.md](firmware/README.md).

## Versioning

Tags cover the whole project (e.g. `v0.1-proto`, `v1.0`), so one tag pins a firmware,
PCB revision and enclosure that fit together. Generated files (Gerbers, BOM, STL) are
attached to GitHub Releases or committed only at tagged versions.

## Large files

CAD exports and binary CAD files (`*.stl`, `*.step`, `*.3mf`, `*.f3d`, ...) go through
Git LFS (see `.gitattributes`). Install it once before adding such files:

```sh
sudo pacman -S git-lfs   # or your distro's package
git lfs install
```

KiCad files are plain text and are stored in regular git.
