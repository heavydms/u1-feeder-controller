# Controller: two U1 feeder modules on one ESP32-C6

> **Disclaimer:** independent project, not affiliated with Snapmaker or Bambu Lab. Provided as is, with no
> warranty; use at your own risk. Check your own hardware before applying power. See the README's full disclaimer.

The controller drives two U1 feeder modules (4 lanes) and presses the Feed, Adjust and Unload buttons on a Bambu Lab TPU
Feed Assist Module. Status: **bench rig working** (November 2026). All four lanes feed, every sensor reads, and
all eight LEDs light. The three assist button optos are wired; their bench test is next.

## Parts

| Part | Job |
|---|---|
| ESP32-C6 DevKitC (WROOM-1) | Main controller |
| MCP23017 (I2C, address 0x20) | 8 LEDs, 4 "B" wheel tachs, 2 assist button outputs |
| DRV8833 module | H-bridge for both feeder motors |
| LM2596 buck module | 12 V → 6.0 V motor rail |
| 3× PC817 optocoupler | Press the assist's Feed, Adjust and Unload buttons |
| 14× 10 kΩ | Pull-ups on every sensor pin |
| 8× 330 Ω | LED inputs; 3 more for the PC817 LEDs |
| 12 V adapter | Motor power |

## Power

```
12 V adapter (+) ──► LM2596 IN+      LM2596 OUT+ (set to 6.0 V first) ──► DRV8833 VM
12 V adapter (−) ──► LM2596 IN−      LM2596 OUT− ──► DRV8833 GND ──► C6 GND ──► both U1 pin 2
C6 from USB. U1 pin 1 (V+) and the MCP23017 run from the C6's 3V3.
```

- **Set the LM2596 to 6.0 V with a meter before connecting the DRV8833.** The DRV8833's VM limit is 10.8 V; never
  put 12 V on it.
- All grounds join.

## Pin map

**Feeder 1:**

| U1 pin | Signal | Goes to |
|---|---|---|
| 3 | Motor tach | C6 GPIO10 |
| 8 | Left wheel tach A | C6 GPIO18 |
| 9 | Left wheel tach B | MCP GPB0 |
| 10 | Left lane inlet | C6 GPIO2 (ADC) |
| 11 | Right lane inlet | C6 GPIO3 (ADC) |
| 14 | Right wheel tach A | C6 GPIO20 |
| 15 | Right wheel tach B | MCP GPB1 |
| 4 / 5 | Motor | DRV8833 OUT1 / OUT2 (IN1 = GPIO22, IN2 = GPIO23) |
| 6, 7, 12, 13 | LEDs | MCP GPA0, GPA1, GPA2, GPA3 (330 Ω each) |

**Feeder 2:**

| U1 pin | Signal | Goes to |
|---|---|---|
| 3 | Motor tach | C6 GPIO11 |
| 8 | Left wheel tach A | C6 GPIO6 |
| 9 | Left wheel tach B | MCP GPB2 |
| 10 | Left lane inlet | C6 GPIO0 (ADC) |
| 11 | Right lane inlet | C6 GPIO1 (ADC) |
| 14 | Right wheel tach A | C6 GPIO7 |
| 15 | Right wheel tach B | MCP GPB3 |
| 4 / 5 | Motor | DRV8833 OUT3 / OUT4 (IN3 = GPIO4, IN4 = GPIO5) |
| 6, 7, 12, 13 | LEDs | MCP GPA4, GPA5, GPA6, GPA7 (330 Ω each) |

**Shared:**

| Signal | Pin |
|---|---|
| I2C SDA / SCL | C6 GPIO19 / GPIO21 |
| Assist Feed / Adjust / Unload (PC817) | MCP GPB4 / GPB6 / GPB5 |
| Spare | C6 GPIO15, MCP GPB7 |

**Notes:**
- Every sensor pin gets its own 10 kΩ pull-up to 3V3, including the four on the MCP (14 in all).
- GPIO4 and 5 are strapping pins, but they only set debug options. The DRV8833's internal pull-downs keep them low
  at boot, so the motor stays stopped.
- Avoided: GPIO8 (RGB LED), GPIO9 (BOOT), GPIO12/13 (USB), GPIO16/17 (UART console).
- Why the MCP23017: two modules need 20 pins (14 sensors, 4 motor, 2 I2C), and the C6 has about 17 safe ones. The
  "B" wheel tachs pulse only ~10 times a second, so polling them over I2C at ~1 kHz keeps up easily.

## Lanes

Lanes are named by the inlet stickers: feeder 1 = L1, L2; feeder 2 = R3, R4.

| Lane | Feeder | Motor | U1 lane | Wheel tach A | Inlet | LED white / red |
|---|---|---|---|---|---|---|
| L1 | 1 | IN2 high | left | GPIO18 | GPIO2 | GPA0 / GPA1 |
| L2 | 1 | IN1 high | right | GPIO20 | GPIO3 | GPA3 / GPA2 |
| R3 | 2 | IN3 high | right | GPIO7 | GPIO1 | GPA7 / GPA6 |
| R4 | 2 | IN4 high | left | GPIO6 | GPIO0 | GPA4 / GPA5 |

## Assist button optos (PC817)

One opto per button. The transistor goes **across** the button, so the button still works by hand, and unplugging
the harness returns the assist to stock.

```
        PC817 (top view, dot = pin 1)
            ┌───────┐
 GPB4 ─330Ω─┤1 ●   4├──── button contact, higher-voltage side ─┐
            │       │                                          ├─ Feed button
 C6 GND ────┤2     3├──── button contact, ground side ─────────┘
            └───────┘
```

Adjust (GPB6) and Unload (GPB5) are the same. With Adjust, the controller can also change the assist's force (hold Adjust + Feed or Unload). The two sides share no ground. Find each button's ground side by metering both
contacts to the assist's ground with the assist powered: the one at 0 V gets pin 3.

## Firmware

[`firmware/pin_checker`](../firmware/pin_checker/pin_checker.ino) is the bench test sketch for this pin map: a live
sensor view, a short and pull-up test, an LED walk, motor and per-lane feed tests, and the assist button test.

```
arduino-cli compile --fqbn esp32:esp32:esp32c6 firmware/pin_checker
arduino-cli upload  --fqbn esp32:esp32:esp32c6 -p <port> firmware/pin_checker
arduino-cli monitor -p <port> -c baudrate=115200
```

Send a letter + Enter: `l` live view (runs at boot), `s` short test (modules and 12 V unplugged), `e` LED walk,
`m` motor test, `f` feed each lane, `1`–`4` feed one lane, `b` assist buttons: Feed, Adjust, Unload in turn (asks first), `i` I2C scan.
