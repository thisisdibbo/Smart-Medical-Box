# Pinout and electrical notes

Everything here is read straight from the firmware. If you change a pin in a sketch, change it
here too.

---

## ESP32 #1 — controller

`firmware/esp1_controller/esp1_controller.ino`

### Motors

Three DC gear motors across two dual H-bridge drivers. The drivers have a `STBY` line, which
points at TB6612FNG-type boards — an L298N has no standby pin and would need the code changed.

| Signal | GPIO | Driver pin | Drives |
|--------|:----:|------------|--------|
| `M1_IN1` | 16 | A-IN1, driver A | Motor 0 — silo 1 |
| `M1_IN2` | 17 | A-IN2, driver A | Motor 0 — silo 1 |
| `M2_IN1` | 18 | B-IN1, driver A | Motor 1 — silo 2 |
| `M2_IN2` | 19 | B-IN2, driver A | Motor 1 — silo 2 |
| `M3_IN1` | 21 | A-IN1, driver B | Motor 2 — silo 3 |
| `M3_IN2` | 22 | A-IN2, driver B | Motor 2 — silo 3 |
| `STBY1`  | 23 | STBY, driver A | held HIGH in `setup()` |
| `STBY2`  | 5  | STBY, driver B | held HIGH in `setup()` |

Motors 1 and 2 are driven in reverse (`IN1` LOW, `IN2` HIGH) in `driveMotorForward()` because
their gearboxes are mounted mirrored. Motor 0 runs the other way round. If you rebuild the
mechanism, that function is the single place to fix rotation direction.

### Pumps

| Signal | GPIO | Notes |
|--------|:----:|-------|
| `PUMP1_PIN` | 26 | **Active LOW** — driven HIGH in `setup()` and whenever idle |
| `PUMP2_PIN` | 27 | same |

Run times, in milliseconds, indexed by the dose preset (0 short, 1 medium, 2 long):

```cpp
unsigned long pumpRunTime[2][3] = {
  {1000, 2000, 5000},   // pump 0
  {1500, 2500, 4500}    // pump 1
};
```

### Sensing and controls

| Signal | GPIO | Mode | Notes |
|--------|:----:|------|-------|
| `PIEZO` | 34 | analog in | Pill-drop detector. `PIEZO_THRESHOLD 400` |
| `DHTPIN` | 4 | 1-wire | DHT22, cabinet temperature and humidity |
| `BUTTON` | 32 | `INPUT_PULLUP` | Start. Active LOW, edge-detected |
| `EMERGENCY_SWITCH` | 33 | `INPUT_PULLUP` | Active LOW. Checked before the state machine |
| `BUZZER_PIN` | 13 | out | Siren, toggled every 200 ms while emergency is held |

### Status LEDs

| Signal | GPIO | Lights when |
|--------|:----:|-------------|
| `LED_SEQ_START` | 14 | A dispensing sequence is running |
| `LED_SEQ_DONE` | 12 | All silos have delivered — clears after 10 s |
| `LED_PUMP_START` | 25 | A pump is running |
| `LED_PUMP_DONE` | 2 | Pumping finished — clears after 10 s |

---

## ESP32 #2 — sensor node and display

`firmware/esp2_display/esp2_display.ino`

| Signal | GPIO | Notes |
|--------|:----:|-------|
| I²C `SDA` | 21 | `Wire.begin(21, 22)` |
| I²C `SCL` | 22 | |
| `WATER_ADC_PIN` | 35 | 12-bit, `ADC_11db` |
| TFT | SPI | Set in `TFT_eSPI/User_Setup.h`, not in the sketch |

### I²C devices

| Address | Device | Reads |
|:-------:|--------|-------|
| `0x57` | MAX30102 | IR + red → heart rate and SpO₂ |
| `0x7F` | Temperature sensor | 24-bit signed, command register `0x30`, data from `0x10` |

The temperature sensor is read as a 24-bit two's-complement value divided by 2¹⁴ (16384.0), with
`BODY_TEMP_OFFSET_C 1.5f` added and the result clamped to 34–41 °C.

### MAX30102 configuration

```cpp
particleSensor.setup(10, 4, 2, 100, 411, 4096);
//                   │   │  │   │    │    └ ADC range
//                   │   │  │   │    └ pulse width
//                   │   │  │   └ sample rate
//                   │   │  └ LED mode (red + IR)
//                   │   └ sample averaging
//                   └ LED brightness
```

Finger presence is a moving comparison: the IR baseline is captured over 50 samples at boot, and
a finger is considered present when the average of the next 100 samples exceeds that baseline by
`IR_THRESHOLD` (8000). BPM is only accepted inside 55–110.

### Water level

```cpp
WATER_EMPTY_RAW = 800    // raw ADC with the tank empty
WATER_FULL_RAW  = 3200   // raw ADC with the tank full
```

Linear map to 0–100 %, then smoothed 70 % previous / 30 % new each loop.

---

## Electrical notes

**Input-only pins.** GPIO 34 and 35 have no output driver and no internal pull-up or pull-down.
Both are used as analog inputs here, which is the correct use. Do not try to drive anything from
them.

**Strapping pins.** GPIO 2, 5 and 12 are read at boot to decide the boot mode.

- GPIO 2 drives an LED — keep it low-current and make sure nothing holds it high during reset.
- GPIO 12 (MTDI) selects flash voltage. A strong pull-up here can stop the board booting. Use a
  series resistor and a low-current LED, or move this LED to a spare pin if you see boot loops.
- GPIO 5 drives a driver `STBY` line; the driver's own input impedance is high enough that this
  is normally fine.

**ADC2 and Wi-Fi.** ADC2 pins cannot be read while Wi-Fi is active. Both analog inputs used here
(34 and 35) are on ADC1, so this project is unaffected — but keep it in mind if you add sensors.

**Power.** Motors, pumps and the ESP32s share a ground, but the motors and pumps must be fed
from their own supply, never from the ESP32's 3V3 regulator. A stall on a gear motor will brown
out the board and reset it mid-sequence. Add a bulk capacitor (470 µF+) across the motor supply
close to the drivers, and flyback protection on the pump relays if your relay board doesn't have
it already.

**Wi-Fi stability.** The controller disables power save explicitly:

```cpp
WiFi.setSleep(false);
esp_wifi_set_ps(WIFI_PS_NONE);
```

Without it the board drops off the network while idle, `/ingest` starts failing, and it looks
like the sensor node has stopped sending. The loop also calls `delay(1)` so the Wi-Fi stack gets
scheduling time.

---

## Persistence

Saved to NVS (namespace `appcfg`) and reloaded on every boot:

- default motor and pump sequences, and their lengths
- per-silo pill targets and remaining counts
- pump dose presets
- all three schedule windows

The remaining count is written the instant a pill is detected, so a power cut mid-sequence
cannot cause a double dose on the next run.
