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

Four builds, two boards. **Pick the one that matches your board** — both are
ESP32-S3, so flashing the wrong one just fails to come up; re-flash and it
recovers.

| board | build | screen |
|---|---|---|
| **Waveshare ESP32-S3-Touch-LCD-1.46** | Noise fields v0.1 / Photograph fields v0.3 | 412×412 **round** |
| **M5Stack CoreS3** | Noise fields v0.1 / Photograph fields v0.3 | 320×240 rectangular, plus haptics and LED ring |

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
| top-left corner | ≡ on the field opens the screens; ← anywhere else returns to the field (and cancels naming) |
| top-right corner | → on any screen continues the cycle: names, band, fields, settings |
| left button, hold ~1.5 s | power off properly |

Over USB at 115200 baud:

| command | what it does |
|---|---|
| `DUMP` | every word given, with the depth it was given at, as CSV |
| `BAND` | the dwell histogram |
| `SOURCES` | where the fields came from (deliberately not on screen) |
| `RESET` | clear the words |
| `TEST` | speaker sweep, motor ramp, ring white — says what it finds |
| `TIME` | per-stage milliseconds of a cold reduction at both levels, and free PSRAM |
| `RING <gpio> [count]` | re-wire the ring to another pin and flash it white, without recompiling |

`TEST` and `RING` exist because "it doesn't work" is not a diagnosis. The
speaker sweep in particular walks 220 Hz to 2 kHz: if only the upper tones are
audible, that is the speaker's physics, not the firmware.

---

## What's in here

    index.html                    the web installer
    manifest-cores3-*.json        ESP Web Tools manifests, one per build
    firmware/                     prebuilt binaries + bootloader + partition table
    src/                          the CoreS3 sketch
    src146/                       the Waveshare 1.46 sketch (round UI)
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
    PORT.C   GPIO 17   vibration motor PWM  (akita11 LightVibratorUnit, AKITA-060)
    PORT.A             left free (I2C)

The vibration unit carries its control signal on **Grove pin 2** (pin 1 is not
connected) and runs happily at either 5 V or 3.3 V. Which of a port's two GPIOs
is "pin 1" is not stated unambiguously anywhere I could find, so if the motor
stays silent, change `HAPTIC_PIN` in `src/board_cores3.h` between `PORT_C_PIN1`
and `PORT_C_PIN2` and rebuild. That is the only thing to touch.

The haptics are not a notification layer. Every event in the field is played
through the speaker **and** the motor with the same envelope — a felt pulse and
an audible one are the same curve at different frequencies. Deeper reduction
makes events longer and weaker, so at the far end the device barely announces
itself. Each pulse starts with an 18 ms kick at full duty, because a small
eccentric-mass motor will not break inertia below about 60% and would otherwise
just hum without turning.

The ring is driven from `src/ring.cpp` (Adafruit_NeoPixel, PORT.B / GPIO 8).
Data goes to the ring's **`I` / `DI`** pad — `O` / `DO` is the chaining output
and stays unconnected. Brightness is hard-capped at 16% in `RING_CEIL`. Set the
LED count in `ringBegin()` — it defaults to 12.

The ring is not a status light. `fieldRingColors()` in `field.cpp` samples the
**same value noise and the same palette as the screen**, around a circle, so
the light is the same weather leaking out of the edge of the device. It drifts
with the field's own clock, and each LED slews toward its target at about one
second — an instant change reads as an alert, a slow one reads as light. Fewer
colours are available as reduction deepens, exactly as on screen.

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


---

## The Waveshare 1.46 build

412×412 round, SPD2010 over QSPI. `src146/board_ws146.h` holds everything
board-specific; the field renderer and the store are shared with the CoreS3.

    arduino-cli compile --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=huge_app,CDCOnBoot=cdc src146

Add `--build-property "compiler.cpp.extra_flags=-DIMAGE_FIELDS=1"` for the
photographs. Libraries: **GFX Library for Arduino** (has `Arduino_SPD2010` and
`Arduino_ESP32QSPI`) and **SensorLib**. The SPD2010 touch driver, the TCA9554
expander driver and the I²C helper come from Waveshare's own example and are
included in `src146/`.

Three things this board does differently, all of them load-bearing:

* **The power latch is back.** GPIO 7 must be raised in the first lines of
  `setup()` or the board dies the moment you release the button. That omission
  is what killed the first 1.69.
* **The panel reset is not a GPIO** — it hangs off the TCA9554 expander (EXIO2),
  so it is pulsed by hand before `gfx->begin()`.
* **Arduino_GFX stores RGB565 unswapped**, unlike LovyanGFX on the CoreS3. The
  byte-swap in `field.h` is therefore disabled here. Getting that backwards is
  what turned the CoreS3's colours into a rainbow.

The UI is laid out for a **disc**, not a rectangle. There are no corners: the
back and next arrows sit at the far left and right of the horizontal centre
line, where a circle gives the most room, and the field's menu sits at bottom
centre. Touch targets are circles, not rectangles. Everything textual lives
inside the inscribed square (291 px). `fieldMaskCircle()` blacks out everything
outside the disc — left unpainted, it leaks as a bright rim through the glass.

Speaker and microphone are on **separate I²S buses** here, so unlike the CoreS3
they can both run at once. Recording atmospheres stops being mutually exclusive
with making sound. The voice is synthesised directly to the PCM5101 in
`src146/buzz.cpp` — a sine with a real amplitude envelope, no library.


---

## v0.4 — the thesis reduction ladder, and the breath

The 1.46 build no longer reduces photographs with the local cell permutation.
It runs **the Chapter 5 stimulus generator itself** (`sketch_rpc_depth3_texture.pde`),
constant for constant, in `src146/rpc.cpp`:

    destination key   blurred luminance, sigma(d) = 3.5 + 31.5*d^1.3 (at 900 px, scaled with W)
    source key        luminance band + hue within the band, Q(d) = 2^(6 - 5d)
    then              inside each band's destination slice, positions are reordered
                      by a blurred, chroma-weighted, vector-averaged hue field
    TEXTURE           band = floor((L + TEXTURE*(noise - 0.5)) * Q)

The output is an exact permutation of the source in every frame — verified on the
compiled firmware code, not just asserted: `np.sort(out) == np.sort(src)` at
d = 0.30 / 0.50 / 0.75.

Two deliberate departures from the sketch, both measured:

**TEXTURE is 0.10, not 0.25.** The stimuli are generated at 900 px and *seen*
downscaled, and TEXTURE is per-pixel white noise, so viewing averages it away.
Fine-scale tonal energy (std of luminance minus its 3x3 mean) against a
900 -> 412 reference of 5.9 / 6.4 / 8.3: TEXTURE 0.25 gives 14.3 / 15.2 / 19.2,
TEXTURE 0.10 gives 7.3 / 9.2 / 15.7. At d = 0.8 nothing matches, because with
Q = 4 the sort itself makes fine structure. That gap is the panel size, not the knob.

**It renders at 206x206 and doubles by nearest neighbour.** Half resolution is
indistinguishable (sigma is already >= 7 px) and costs a quarter as much. The
crude doubling turns out to be the *correct* one: 206 + nearest measures 5.99
against the reference's 5.89, while 206 + bilinear collapses to 1.92 — smooth
interpolation eats exactly the grain the reference has.

### The breath

Put the device down and leave it for a second and a half, and the field starts
breathing: reduction depth oscillates +-0.22 around wherever your thumb left it.
Touch it and it stops. `SETTINGS -> breathing` turns it off.

It cannot be made smooth, and it is worth knowing why before trying. Q is an
integer and so are the blur's box widths. Measured at 206 px: changing sigma by
less than 5% changes nothing at all, and moving Q by 1 — or by 0.125, using a
fractional Q — reshuffles the whole image (mean |diff| 16.9 for 5.000 -> 5.125,
20.4 for 5 -> 6). The operation is not continuous in depth.

But it is a *dense* change, not a sparkle: at that step 67% of pixels move a
little and only 1.7% move a lot — the same distribution as a rigid 1.5 px shift.
So instead of faking smoothness, the animation gives **one frame per quantum**:
the distinct (Q, box widths) states along the range are enumerated and each is a
frame. No dead frames, no double jumps — step-to-step deviation 2.0 against 6.1
for spacing the frames evenly in depth.

That is about 27 states over a +-0.22 range, computed once and cached
(27 x 206 x 206 x 2 = 2.3 MB of PSRAM), then ping-ponged: a 52-frame cycle at
330 ms, about 17 seconds. Playback is a blit, so the tempo is free.

The cycle is built **one state per frame, from the middle outwards**, and the
middle state is held until the cycle is complete. So putting the device down
shows essentially the image that was already under your thumb, the breath starts
from there a few seconds later, and the device never stalls waiting to compute.
If PSRAM runs out mid-build it breathes over the contiguous stretch it managed,
which by construction is centred on your depth.

    src146/rpc.h     the reasoning and the constants
    src146/rpc.cpp   the operation, the cache and the schedule

### Why the field was laggy, and what fixed it

The first cut recomputed the whole reduction every frame, even when nothing had
changed. The menus were fine because menus compute nothing — the symptom pointed
straight at the cause. Three changes, in order of what they were worth:

**Cache by state, not by depth.** Q and the blur's box widths are integers, so
two depths that land on the same state give the same output *bit for bit*.
Dragging half a millimetre almost always stays inside one state. The cache key
is (atmos, Q, box widths); a hit is a blit. Standing still now costs nothing at
all — before, it cost a full reduction 24 times a second.

**A coarse level for the drag.** While the thumb is down the field renders at
103x103 and quadruples — a quarter of the work, and against 206 you can barely
tell (the field is already blurred). The whole depth range is only 63 states at
that size, so a 40-slot LRU cache means a second pass over the range computes
nothing. On release it does one fine reduction and stops.

**16-bit blur buffers, and a blocked vertical pass.** The three box blurs are
the dominant cost and they are pure PSRAM traffic, so int32 -> int16 nearly
halves it. The vertical pass now walks 16 columns at once instead of one: a
column walk strides 412 bytes and throws away most of every PSRAM burst.

Whether that is enough is a question about the board, not about the code, so the
board answers it: **`TIME` over USB** reports measured milliseconds per stage
(blur / keys / sort / scatter) at both levels, plus free PSRAM. "It feels slow"
is not a diagnosis.

### The six-fields screen

It hung. `renderIndex` was calling the live field renderer **six times per
frame** — six whole reductions, 24 times a second — so the screen locked up and
took the touch poll with it, which is why it was hard to get back out.

A contact sheet does not need to be the exact frame; it needs to appear. So the
six thumbnails are **baked into flash by the importer**, 88x68, and drawing one
is a memcpy.

They are baked as the **reduction**, not the photograph. A thumbnail showing the
real picture would give away precisely what the device refuses to show — the
whole point is that nothing on the device names or reveals the source.

Three rungs per field (d = 0.25 / 0.50 / 0.75) and the grid picks the one
nearest that field's remembered depth, so the contact sheet also says where each
person left each field. Cost: 6 x 3 x 88 x 68 x 2 = 210 kB of flash.

    python3 tools/import_images.py tools/src/*.png > src146/images.h

`tools/rpc_reduce.py` is the reduction operation in numpy — the same constants
as `src146/rpc.cpp` — so the baked thumbnails and the live field come from one
definition.

Build: 2,782,230 bytes, 88% of the huge_app partition.
