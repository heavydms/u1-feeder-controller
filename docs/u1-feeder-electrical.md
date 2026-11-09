# Snapmaker U1 feeder module: electrical spec (measured)

> **Disclaimer:** independent project, not affiliated with Snapmaker or Bambu Lab. Provided as is, with no
> warranty; use at your own risk. Check your own hardware before applying power. See the README's full disclaimer.

The Snapmaker U1's automatic filament feeder module ([store page](https://us.snapmaker.com/products/automatic-filament-feeder-module-for-snapmaker-u1))
is a two-lane push feeder sold as a $17 spare part. These notes cover what you need to drive one from your own
controller. They come from bench measurements on two modules (November 2026), checked against Snapmaker's
open-source Klipper code
([`klippy/extras/filament_feed.py`](https://github.com/Snapmaker/u1-klipper/blob/HEAD/klippy/extras/filament_feed.py)).

**The module has no microcontroller.** It is a motor, five Hall-type tach sensors, two inlet sensors and two
bi-colour LEDs, all wired straight to one 15-pin connector. The printer's board does everything, including the
motor H-bridge.

## Pinout

Orientation for "left" and "right": inlets pointing down, cable at the back (away from you), LEDs facing you.

| Pin | Signal | Type | Notes |
|---|---|---|---|
| 1 | V+ | Supply | Sensor and LED supply. **3.3 V works for everything here.** What the U1 itself supplies is not measured |
| 2 | GND | Supply | |
| 3 | Motor tach | Open-drain | 2 pulses per motor rev (per Snapmaker's code). Needs a pull-up |
| 4 | Motor A | Motor | Bare motor wire. Isolated from everything else |
| 5 | Motor B | Motor | Bare motor wire |
| 6 | Left LED, white | Logic input | High = on. Drives an on-board transistor; the LED runs from V+ |
| 7 | Left LED, red | Logic input | High = on |
| 8 | Left wheel tach A | Open-drain | Needs a pull-up |
| 9 | Left wheel tach B | Open-drain | Needs a pull-up. A and B are a quadrature-style pair on the same wheel |
| 10 | Left lane inlet sensor | Analog | See "Inlet sensors" |
| 11 | Right lane inlet sensor | Analog | |
| 12 | Right LED, red | Logic input | High = on |
| 13 | Right LED, white | Logic input | High = on |
| 14 | Right wheel tach A | Open-drain | Needs a pull-up |
| 15 | Right wheel tach B | Open-drain | Needs a pull-up |

The connector type is not confirmed yet (likely a fine-pitch latching wire-to-board connector). The bench rig
uses adapter boards from the U1's own extension cables. **Extension cables can mirror the pin order:** trace
pin 1 end to end before trusting any numbering.

## Motor

- **Winding:** about 5.9 Ω. That's a low-voltage motor; a 24 V motor of this size would read tens of ohms.
- **Runs on 3 V and 6 V** (bench, AA cells). 6 V is the working voltage here. Stall current is roughly 1 A at 6 V.
- **You need your own H-bridge.** A DRV8833 (1.5 A per channel, VM ≤ 10.8 V) works well.
- **One motor, two lanes.** The motor's direction picks the lane through a swing-arm gear selector. Reversing it
  gives an audible click as the arm swaps lanes, and then that lane feeds.

  | Pin 4 / pin 5 | Feeds |
  |---|---|
  | + / − | Right lane (inlet pin 11, tachs 14 + 15, LEDs 12 / 13) |
  | − / + | Left lane (inlet pin 10, tachs 8 + 9, LEDs 6 / 7) |

- **Lock:** the lane that was fed last stays locked (the worm drive can't be back-driven), both directions. The
  other lane moves freely.
- **Neutral:** a short reverse pulse, cut before the arm finishes swinging, leaves **both** lanes free.
  Snapmaker uses a 40 ms pulse at the U1's voltage. The right length depends on motor voltage, so calibrate it
  with the motor tach count rather than a fixed time.
- **Push only.** The module can't pull filament back. On the U1, the toolhead extruder does the pulling.

## Inlet sensors (pins 10 and 11)

Each inlet sensor reads like a resistance to GND that drops when filament is present.

With a **10 kΩ pull-up to 3.3 V**:

| State | Voltage |
|---|---|
| Module present, no filament | ~0.8–0.9 V |
| Filament present | ~0.06–0.11 V |
| No module (pin floating on the pull-up) | ~3.3 V |

These match the thresholds in Snapmaker's code ("module present ~0.9", "filament below 0.18"), so the U1 most
likely uses the same pull-up. Read them with an ADC, and treat anything below ~0.5 V as filament present.
They report presence only, not motion.

**Watch the lane pairing:** pin 10 is the **left** lane's sensor (the lane with tachs 8 + 9) and pin 11 the right
lane's. An early pass of this sweep had these swapped. Pairing was confirmed by feeding each lane with the motor
and pulling filament from each inlet.

## Tach sensors (pins 3, 8, 9, 14, 15)

- **Open-drain** (they only pull low). Add a pull-up to your logic supply; 10 kΩ to 3.3 V works.
- **Wheel tachs:** two per lane, on a 10 mm wheel (31.4 mm circumference), 6 pulses per rev per Snapmaker's code.
  Only the fed lane's wheel counts. At 6 V we saw about 24–37 edges/s on tach A.
- **Motor tach:** steady pulse train while the motor runs; useful for stall detection and for timing the neutral pulse.
- On the bench, the tachs counted cleanly even without pull-ups. Don't rely on that; fit the pull-ups.

## LEDs (pins 6, 7, 12, 13)

- Each pin drives a transistor inside the module (they read ~1.77 V on a diode meter, like a transistor base
  through a resistor). The LED itself is powered from V+ (pin 1).
- **Drive them from 3.3 V logic through 330 Ω.** High = on.
- Two physical LEDs, one per lane, each with a white and a red die.

## How these were measured

1. Unpowered diode-mode sweep of every ordered pin pair (210 readings) to find GND, V+ and the motor pins.
2. Resistance on the motor pair.
3. 3.3 V on pin 1, one signal at a time: LEDs through a resistor, inlet sensors with a 10 kΩ pull-up, tachs by
   pushing filament by hand.
4. Motor on AA cells (3 V, then 6 V) for direction, lock and neutral.
5. Final lane pairing from an ESP32-C6 controller feeding each lane for 1 s and watching every sensor.
