# ESP32-S3-Embedded-Audio-Player

A hardware MP3 player built on the ESP32-S3. It reads MP3 files from a microSD card, decodes them with the Helix MP3 decoder, and plays them through a PCM5102 I2S DAC. A button matrix controls volume, pause, and track skipping.

**Status: work in progress.** See [Known issues](#known-issues).

<!-- TODO: add demo video link or embedded GIF -->
<!-- TODO: add photo of the breadboard -->

## Features

- Plays `.mp3` files from the root of a FAT-formatted microSD card (SPI mode)
- Software decoding with Helix (`mp3dec.h`)
- I2S output to a PCM5102 DAC (44.1 kHz, 16-bit stereo)
- Button controls: volume up/down, previous, next, pause/resume
- Two FreeRTOS tasks: input scanning on core 0, audio on core 1
- MP3 input and PCM output buffers allocated in PSRAM

## Hardware

- ESP32-S3 board with PSRAM
- microSD card module (SPI)
- PCM5102 I2S DAC module
- Button matrix (wired as 4x4, 2 rows x 4 columns used in code)
- SSD1306-style OLED (wired, not yet used in code)

### Pin connections

**microSD**

| SD   | ESP32-S3 |
|------|----------|
| GND  | GND      |
| 3V3  | 3V3      |
| MISO | GPIO 40  |
| MOSI | GPIO 42  |
| CS   | GPIO 1   |
| CLK  | GPIO 41  |

**PCM5102 DAC**

| PCM5102 | ESP32-S3 |
|---------|----------|
| VIN     | 5V       |
| GND     | GND      |
| LRCK    | GPIO 9   |
| BCK     | GPIO 10  |
| DIN     | GPIO 11  |
| SCK     | GND      |
| XSMT    | 3V3      |
| FMT     | GND      |
| FLT     | GND      |
| DEMP    | GND      |

**Button matrix**

| Matrix | ESP32-S3 |
|--------|----------|
| R1     | GPIO 15  |
| R2     | GPIO 16  |
| C1     | GPIO 7   |
| C2     | GPIO 6   |
| C3     | GPIO 5   |
| C4     | GPIO 4   |

**OLED (wired, no code yet)**

| OLED | ESP32-S3 |
|------|----------|
| GND  | GND      |
| VCC  | 3V3      |
| SDA  | GPIO 21  |
| SCL  | GPIO 47  |

### Button map

| Key | Action      |
|-----|-------------|
| S1  | Volume up   |
| S2  | Previous    |
| S3  | Pause/resume|
| S4  | Next        |
| S5  | Volume down |

S6-S8 are unassigned.

## How it works

1. `app_main` mounts the SD card, scans the root directory for `.mp3` files (up to 120), initializes I2S, and sets up the button matrix.
2. `matrix_scanner_task` (core 0) drives each row low, reads the columns, and reacts on key-state changes. Volume is changed directly. Track and pause commands go into a single-slot `pending_system_command`.
3. `mp3_player` (core 1) opens the current track and loops: read a command, refill the input buffer, find a frame sync word, decode one frame, scale by volume, and write PCM to I2S.
4. When a track ends or the user skips, the decoder is recreated and the next track starts.

<!-- TODO: add flowchart image here -->

## Building

Built with ESP-IDF `<version>`.

```
idf.py set-target esp32s3
idf.py build flash monitor
```

Requirements:

- PSRAM enabled in `menuconfig`
- Helix MP3 decoder available as a component (`mp3dec.h`) <!-- TODO: say where it comes from -->

Put your `.mp3` files in the root of a FAT-formatted SD card.

## Known issues

- **Repeated audio on track change.** A short fragment of the previous buffer replays when switching tracks manually. Likely stale data in the I2S DMA buffers after the channel is disabled and re-enabled. Under investigation.
- **Crash on the 5th song.** Playback crashes when the fifth track plays. Not yet diagnosed. It did not happen on the original ESP32.
- **44.1 kHz stereo only.** The I2S clock is fixed. Files with other sample rates or mono audio will play at the wrong speed or pitch.
- **No ID3v2 skipping.** Tags are handled only by the sync-word search.
- **Limits.** Root directory only, 120 tracks max, filenames up to 63 characters.

## Roadmap

- [ ] Fix the track-change repeat and the 5th-song crash
- [ ] Read sample rate and channel count from the frame header and reconfigure I2S
- [ ] Split `mp3_player` into smaller helper functions
- [ ] OLED display (track name, volume, play state)
- [ ] Skip ID3v2 tags

## Debugging

I used a logic analyzer to check the I2S signals (BCLK, LRC, DOUT).

<!-- TODO: add logic analyzer screenshots with a one-line caption each -->
