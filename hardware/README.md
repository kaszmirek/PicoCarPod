# PicoCarPod hardware

PCB for [PicoCarPod](../README.md), designed in KiCad. Not started yet; the bench setup is a
Raspberry Pi Pico W with a Waveshare Pico-Audio (PCM5101A) board.

```
PicoCarPod.kicad_pro / .kicad_sch / .kicad_pcb   project (create it in this directory)
lib/          project-specific symbols (*.kicad_sym) and footprints (*.pretty/)
production/   Gerbers, drill files, BOM, CPL - generated, commit only for tagged releases
```

Pin assignments must match the firmware (I2S pins in [firmware/README.md](../firmware/README.md),
iAP1 serial on UART0, dock pins 12/13, see [docs/iap1-notes.md](../docs/iap1-notes.md)).
Note the PCB revision and the matching firmware version here when the first board is made.
