# Pocket Atmosphere — M5Stack CoreS3

A handheld that holds six atmospheric fields and lets whoever is holding it
decide how much reduction they want, and what it is they are looking at.

Three design decisions, all deliberate:

1. **The device never names its own fields.** They are I–VI. Putting "harbour
   before sunrise" underneath would close the reading before it started — the
   beholder's share is the subject, not a side effect.
2. **The reduction depth lives under the thumb**, not in the firmware. You drag
   vertically on the field itself and it moves live.
3. **What gets stored is data about the viewer, not the device**: the words
   people give each field (with the depth they gave them at) and how long each
   person spends at each depth. `DUMP` over USB at 115200 baud.

---

## Install it from the browser

Open the GitHub Pages URL for this repo in **Chrome or Edge on a computer**,
plug the CoreS3 in over USB-C, and press Install. Two firmwares are offered:

| build | what it shows |
|---|---|
| **Noise fields v0.1** | six fields built from fractal noise in authored palettes |
| **Photograph fields v0.3** | six photographs, reduced by a local histogram-perfect permutation |

The CoreS3 has native USB, so it should appear as a serial port with no driver
and without holding any button. If it doesn't appear, hold the **left button**
for about six seconds to power-cycle, then retry.

Installing *without* erasing keeps the words and dwell log already collected.

## Holding it

| gesture | what happens |
|---|---|
| drag up / down | reduction depth, live under the thumb |
| swipe sideways | next field — each remembers its own depth |
| tap | show or hide the readout |
| hold about a second | name what you're looking at |
| left button, short press | next screen |
| left button, hold ~1.5 s | power off properly |

Over USB at 115200 baud: `DUMP` for the words as CSV, `BAND` for the dwell
histogram, `SOURCES` if you really want to know where the fields came from,
`RESET` to clear the words.

---

## What's in here

    index.html                    the web installer
    manifest-cores3-*.json        ESP Web Tools manifests, one per build
    firmware/                     prebuilt binaries + bootloader + partition table
    src/                          the Arduino sketch
    tools/import_images.py        turns photographs into src/images.h
    tools/src/                    the six source photographs

## Building from source

Arduino IDE or arduino-cli, board **M5Stack CoreS3**.
Libraries: **M5Unified** and **M5GFX** (Library Manager).

    arduino-cli compile --fqbn esp32:esp32:m5stack_cores3 src                    # noise, 588 kB
    arduino-cli compile --fqbn esp32:esp32:m5stack_cores3 \
      --build-property "compiler.cpp.extra_flags=-DIMAGE_FIELDS=1" src           # photographs, 2.43 MB

To use your own photographs:

    python3 tools/import_images.py tools/src/*.png > src/images.h

Six images at 320×240 in RGB565 is 900 kB of flash. The importer stores the
photographs whole and derives everything else at run time.

## Flashing from the command line instead

    esptool.py --chip esp32s3 write_flash \
      0x0     firmware/bootloader.bin \
      0x8000  firmware/partitions.bin \
      0xe000  firmware/boot_app0.bin \
      0x10000 firmware/cores3-noise-v0.1.bin

## Adding the light ring and the haptics

Both Grove ports on the unit are free:

    PORT.B   GPIO 8    SK6812 ring data — put a 1N4001 in series with its +5 V
    PORT.C   GPIO 18   vibration motor PWM  (akita11 LightVibratorUnit, AKITA-060)
    PORT.A             left free (I2C)

The vibration unit carries its control signal on **Grove pin 2** (pin 1 is not
connected) and runs happily at either 5 V or 3.3 V. Which of a port's two GPIOs
is "pin 1" is not stated unambiguously anywhere I could find, so if the motor
stays silent, change `HAPTIC_PIN` in `src/board_cores3.h` from `PORT_C_PIN2` to
`PORT_C_PIN1` and rebuild. That is the only thing to touch.

The haptics are not a notification layer. Every event in the field is played
through the speaker **and** the motor with the same envelope — a felt pulse and
an audible one are the same curve at different frequencies. Deeper reduction
makes events longer and weaker, so at the far end the device barely announces
itself. Each pulse starts with an 18 ms kick at full duty, because a small
eccentric-mass motor will not break inertia below about 60% and would otherwise
just hum without turning.

Power the ring from the port's 5 V, cap its brightness in firmware, and expect
about 100–150 mA at the level an ambient glow actually needs.

---

## Notes on the port from the Waveshare 1.69

Everything hardware-specific goes through `src/board_cores3.h`, which keeps the
names the original used and calls M5Unified underneath. Removed outright:

* Arduino_GFX + Arduino_ST7789 → `M5.Display` / `M5Canvas`
* SensorLib's CST816, QMI8658, PCF85063 drivers → `M5.Touch` / `M5.Imu` / `M5.Rtc`
* the SYS_EN / SYS_OUT soft-latch → `M5.Power` (the AXP2101 handles it)
* the resistive battery divider and ADC → `M5.Power.getBatteryLevel()`
* the LEDC piezo buzzer → `M5.Speaker`, real I²S audio

The UI was relaid out from 240×280 portrait to 320×240 landscape.

A research instrument for a PhD on reduced perceptual cues.
