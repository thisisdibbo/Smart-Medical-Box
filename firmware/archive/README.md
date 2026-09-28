# Archive

Earlier iterations, kept because they document how the project got here. Neither is what runs on
the built unit — use [`../esp1_controller`](../esp1_controller) and
[`../esp2_display`](../esp2_display) for that.

## `esp1_controller_early/`

An earlier controller build with the same motor, pump and schedule logic but before the Wi-Fi
stability work. It has no power-save disable, no reconnect loop and no Wi-Fi event logging, so
it drops off the network after a while and `/ingest` starts failing — which reads as "the sensor
node stopped sending" even though the sensor node is fine.

Kept for the dashboard HTML, which is close to the final version and easier to read in isolation.

## `vitals_oled_test/`

The MAX30102 bring-up sketch: heart rate, SpO₂ and the `0x7F` temperature sensor on a 128×64
SH1106 OLED, with no Wi-Fi and nothing else attached. This is the quickest way to prove a
sensor board works before wiring it into the box.

Needs `Adafruit_GFX`, `Adafruit_SH110X` and `SparkFun MAX3010x`.
