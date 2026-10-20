# PicoTape ZX

A tape deck for the ZX Spectrum 48K, built on a Raspberry Pi RP2040.

A tape deck replacement built on a Waveshare RP2040-Zero. It plays `.TAP` files into the
Spectrum's EAR socket and records `SAVE`s from its MIC socket into new `.TAP` files. Files
live in the Pico's flash and you copy them on and off over USB.

## Hardware

| Part | Connection |
|---|---|
| GM009605 OLED (SSD1306, I²C) | VCC→3V3, GND, SDA→GP4, SCL→GP5 |
| HW-040 rotary encoder | **+ → 3V3** (not 5V), GND, CLK→GP6, DT→GP7, SW→GP8 |
| Optional Back/Stop button | GP14 → button → GND |
| Mono 3.5 mm jack | see below |
| Status LED | on-board WS2812 (GP16) |

### Audio jack network

```
 PLAY:  GP2 ──[1k]─────────────────────┬── JACK TIP
                                      │
 REC:   3V3 ─[10k]─┬─[1k]── GP26     │
        GND ─[10k]─┴──────┤├─────────┘
                          1µF
        JACK SLEEVE ── GND
```

To play, plug the cable into the Spectrum's **EAR** socket. To record, plug it into **MIC**.
The full 3.3 V swing through 1k loads reliably on a 48K. A divider down to ~0.8 V was too quiet.

**Status:** playback and recording both work on a real 48K using the interim wiring below,
without the 1µF capacitor and the two 10k resistors.

**Interim REC wiring, until the 1µF capacitor arrives:** connect `JACK TIP ──[1k]── GP26` only,
and leave out the two 10k resistors as well. Without the capacitor they would pull the
playback signal toward 1.65 V. The Spectrum's MIC output is a small positive signal, and the
ADC can read it directly. The decoder's threshold follows the signal level, so it works with
or without the capacitor. When the capacitor arrives, add it together with the two 10k bias
resistors as drawn above.

If loading suddenly fails, check the wiring before suspecting anything else. A loose
connection on the tip or sleeve gives red/cyan border flicker with only occasional stripes.

## Build and flash

1. Install PlatformIO (the VS Code extension, or `pip install platformio`).
2. Hold BOOT on the RP2040-Zero while plugging in USB, then run:

```bash
pio run -t upload
```

On first boot the 1.5 MB storage area is formatted automatically.

## Controls

| Where | Turn | Click | Hold / Back button |
|---|---|---|---|
| File list | move | play file / `[Record]` / `[USB drive]` | – |
| Playing | – | pause / resume (back to list when done) | stop, back to list |
| Recording | – | stop and **save** | stop and **discard** |
| USB drive | – | – | cancel (before the PC mounts it) |

**USB drive:** pick `[USB drive]`, wait for the drive to appear on your PC, and copy `.TAP`
files to its root. Then **eject** the drive on the PC to get back to the deck. The drive is
only offered while nothing is playing, so the PC and the deck never write to flash at the
same time.

**Recording:** plug the cable into MIC, pick `[Record]`, then type `SAVE "name"` on the
Spectrum and press a key when it asks you to start the tape. The screen shows:
- what's happening right now (waiting, pilot tone, or bytes being received);
- each finished block, with its checksum result (`OK` or `ERR`);
- an input level bar.

When the Spectrum is done, click to save. The file is named from the first header, e.g.
`SAVE "boulder"` gives `boulder.TAP`; if that name exists, `boulder_2.TAP`. A block marked
`ERR` is still saved, but you should record it again.

LED: green = playing, amber = paused, red = recording, blue = USB.

## How playback works

- Core 0 handles the UI and FatFS, and streams the file into a 4 KB ring buffer.
- Core 1 converts bytes into half-wave durations using the standard ROM timings: pilot 2168 T
  (8063 pulses for a header, 3223 for data), sync 667/735 T, bits 855/1710 T, and 1 s pauses.
- A 4-instruction PIO program clocked at 3.5 MHz (1 cycle = 1 Z80 T-state) toggles GP2 with
  exact timing.

## How recording works

- The ADC samples GP26 at 50 kHz (8-bit), and DMA writes the samples into a 32 KB ring buffer.
  DMA keeps running even while a flash write briefly pauses the CPU cores, so no samples are lost.
- Core 1 runs `TapDecoder`. A slicer finds the signal's edges, and its threshold follows the
  signal's peaks, so DC offset and level don't matter. A state machine then finds the pilot
  (at least 256 pulses), the two sync pulses and the bits: a full period over 2565 T is a 1.
  More than 4000 T without an edge ends the block.
- Core 0 collects each block in RAM and appends it to `REC.TMP`, which is renamed when you save.
- GP2 is switched to high impedance while recording, so it doesn't load the shared jack.

### Decoder tests on a PC

`tools/decoder_test.cpp` runs the same decoder code against WAV files, and against generated
signals: various levels, DC offsets, noise, ±8% speed, timing jitter, and noise with no signal.

```bash
g++ -O2 -std=c++17 -Isrc tools/decoder_test.cpp src/tap_decoder.cpp -o decoder_test
```

Then `decoder_test --synth`, or `decoder_test file.wav [volts_pp] [offset_v] [out.tap]`.

## Roadmap

1. ✅ Display, encoder, file list, USB drive
2. ✅ TAP playback (PIO)
3. ✅ Recording: ADC + DMA on GP26, pulse decoder, checksum check, naming from the header
4. ⬜ Polish: block skip/seek, a settings screen, deleting files on the device
5. ⬜ Maybe: TZX support (turbo and custom loaders), subfolders

## Troubleshooting

- **Cursor moves the wrong way, or 2 places per click:** change `REVERSE` or
  `COUNTS_PER_DETENT` in `src/input.cpp`.
- **LED shows the wrong colour:** change `NEO_GRB` to `NEO_RGB` in `src/main.cpp`.
- **Recording shows "Waiting for SAVE..." and the level bar stays near the tick mark:**
  the MIC signal isn't reaching GP26. Check the cable is in **MIC**, and check the
  tip → 1k → GP26 connection.
- **Blocks come in as ERR:** send me a WAV of the Spectrum's MIC output, or try recording
  from the Spectrum's EAR socket instead, which also outputs the signal during `SAVE`.
- **Checking the output:** record GP2 (through the divider) with a PC's line-in and open the
  WAV in an emulator, or use `audio2tape` from fuse-utils to compare it with the original TAP.

## License

GPL-3.0. See [LICENSE](LICENSE).
