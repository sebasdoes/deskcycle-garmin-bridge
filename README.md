# DeskCycle 2 → Garmin bridge (ESP32)

Read the DeskCycle 2 under-desk bike's console jack with an ESP32 and
broadcast it as standard Bluetooth fitness sensors that any Garmin
Forerunner (and most watches/apps) pairs with natively.

Two firmware variants:

| Firmware | Status | What the watch sees |
|---|---|---|
| [`firmware/DeskCycleCSC/`](firmware/DeskCycleCSC/DeskCycleCSC.ino) | **working, hardware-validated build** | BLE Cycling Speed & Cadence (0x1816): cadence + speed/distance |
| [`firmware/DeskCycleFTMS/`](firmware/DeskCycleFTMS/DeskCycleFTMS.ino) | **compiles, not yet hardware-tested** | BLE FTMS Fitness Machine (0x1826): cadence + **power (watts)** + calibrated energy |

Why two? A CSC sensor cannot carry watts. If you want power on the
watch, the ESP32 has to speak FTMS and the watch pairs with it as a
trainer. The FTMS firmware bakes in a power curve reverse-engineered
from DeskCycle's own official online calculator (see below), so the
watts match what the console's calorie math is based on.

```
DeskCycle 2 console ──3.5mm──► ESP32 ──BLE CSC or FTMS──► Garmin watch
   (dry reed contact             GPIO25, internal
    closes 1× per                pull-up, falling edge
    crank revolution)            = 1 revolution)
```

## How the DeskCycle 2 jack works

Verified against three open-source projects that read this exact signal
([PartlyAtomic/deskcycle](https://github.com/PartlyAtomic/deskcycle),
[kneave/dcspeedo](https://github.com/kneave/dcspeedo),
[Veraxus/DeskCycle-Rasperry-Pi-Pico-Game-Controller](https://github.com/Veraxus/DeskCycle-Rasperry-Pi-Pico-Game-Controller)):

- The 3.5 mm "bike computer" jack is a **dry reed-switch contact**: it
  closes once per crank revolution. It is *not* audio, *not* serial,
  and carries **no voltage**.
- Sleeve (longest segment) = GND; tip and/or ring = the switch contact.
- Belt travel is **23.0 ft (7010 mm) per crank revolution**, the same at
  every resistance setting — the magnetic knob changes effort per rev,
  not gearing. There is no freewheel.
- The resistance knob is purely mechanical (magnetic detents); no
  electronics can read it without adding hardware. All firmware here
  assumes you ride at your fixed setting.

### Wiring (3 wires, no components)

| DC2 3.5mm plug | ESP32 |
|---|---|
| Sleeve (GND) | GND |
| Tip **or** ring | GPIO 25 (`PULSE_PIN`) |
| (other of tip/ring) | leave unconnected |

The firmware enables the GPIO's internal pull-up, so the pin idles HIGH
and the reed switch pulls it LOW once per revolution. If counts are
erratic on a long cable, add a 10 kΩ pull-up to 3V3 and a 100 nF cap to
GND.

Practical notes: use a 3.5 mm extension cable (bike side is male;
cut/strip the extension's male end). Copper strands are varnish-coated —
burn/scrape before trusting continuity beeps. No stereo-to-mono
adapters (they short tip+ring to sleeve). Multimeter sanity test:
continuity between tip and sleeve, spin a pedal slowly → one beep per
revolution. Must be a real ESP32 (ESP8266 and plain Pico boards have no
BLE).

**Pre-flight for the FTMS build:** this repo's own jack-vs-console test
has not yet confirmed that one reed pulse = one console RPM. The DC2
hall sensor sits on the belt drive; if the jack is flywheel-geared
instead of crank-direct, cadence and power will read k× high/low —
check the watch cadence against the console RPM readout once, pedal at
a steady rate, and if they differ by a constant factor, change `JACK_REV_PER_CRANK`
in the FTMS sketch.

## Power calibration (reverse-engineered from DeskCycle's official calculator)

DeskCycle's official calculator at [app.deskcycle.com](https://app.deskcycle.com/)
computes results server-side. Its endpoint
`https://api.deskcycle.com/v1/activity_logs/new` answers unauthenticated
GET requests, so the model was mapped by probing ~25 parameter
combinations (levels 1–8, cadence 50–140 rpm, weights 110–220 lb,
ages, genders, sexes, session lengths). Full probe table:
[`docs/calibration.md`](docs/calibration.md).

Findings:

- **Power depends only on device model, resistance level and cadence.**
  Weight, age, height and sex have zero effect on watts (they only
  scale the calorie outputs).
- **Level 8 (max resistance) power curve, valid 50–140 rpm, fit error
  ≤ 0.05 W:**

  ```
  P(W) = 0.000864 · rpm² + 2.0473 · rpm − 35.939
  ```

  | rpm | 50 | 60 | 70 | 80 | 90 | 100 | 110 | 120 | 140 |
  |---|---|---|---|---|---|---|---|---|---|
  | W | 68.6 | 90.0 | 111.6 | 133.4 | 155.3 | 177.4 | 199.7 | 222.2 | 267.6 |

- Other levels at 60 rpm: L1 10.0 / L2 19.4 / L3 25.5 / L4 35.2 /
  L5 48.2 / L6 59.9 / L7 72.4 / L8 90.0 W. Level→power is jagged (a
  per-level factory table, not a smooth formula).
- Calories *do* scale with body weight: at L8/60 rpm, 165 lb →
  483 kcal/h, of which ~94 kcal/h is the sitting baseline.
- The road-bike "speed/distance" the calculator shows is **derived from
  watts by inverting a real road-bike physics model** (rolling +
  aero drag), recovered exactly as:

  ```
  P(W) = 1.0574 · v(mph) + 0.013636 · v³(mph)
  ```

  Same 60 rpm at L1/L4/L8 maps to road-equivalent 6.3 / 11.8 / 17.4 mph.
  That is why "distance" depends on level in their app but never in the
  console display: the console's own mile is just 240 revs (= 5520 ft
  at 23 ft/rev, ~4.6% longer than a statute mile).
- The FTMS firmware implements the L8 curve (the calibration target:
  resistance knob always maxed). Riding at other settings will read
  proportionally high — rerun the probe for levels 1–7 if you need them.
- Caveat: watts here are the *calculator's* model of level-8 effort,
  i.e. the ground truth behind the console calorie readout, not a
  crank-meter measurement. Expected on-bike agreement with console
  calories: good when your console weight setting matches the weight
  you tell Garmin.

## Firmware

### CSC build (`firmware/DeskCycleCSC`)

- GPIO25 input, internal pull-up, falling-edge interrupt, 15 ms
  reed-chatter debounce
- Advertises as **"DC2 Cadence"**, CSC service 0x1816
- CSC Measurement (0x2A5B) notifies at ~4 Hz while pedalling with
  cumulative wheel + crank revolutions and event times
- One final measurement ~2.5 s after you stop, so the watch logs the
  last revolution instead of freezing a bogus cadence
- Re-advertises automatically after the watch disconnects

### FTMS build (`firmware/DeskCycleFTMS`)

- Same pulse capture path as the CSC build
- Advertises as **"DC2 Bike"**, FTMS service 0x1826 with the Fitness
  Machine Feature + Indoor Bike Data + Training Status characteristics
- Indoor Bike Data notification at ~2 Hz while pedalling carrying:
  cadence (rpm), **instantaneous power (W)** from the calibration
  curve, and total energy (kcal) accumulated with a 20% metabolic
  efficiency factor to match the console's gross-calorie convention
- Power curve uses a float quadratic evaluated per report; cadence is
  computed over a sliding 3-rev window so it settles fast
- Re-advertises automatically after the watch disconnects

## Building

GitHub Actions builds both sketches on every push and attaches `.bin`
artifacts — see [`.github/workflows/build.yml`](.github/workflows/build.yml).

Locally:

```bash
arduino-cli core install esp32:esp32
arduino-cli lib install "NimBLE-Arduino"
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/DeskCycleCSC
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/DeskCycleFTMS
arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 firmware/DeskCycleCSC
```

Reference build results (classic ESP32, esp32 core 3.3.x,
NimBLE-Arduino 2.5.1 by h2zero):

| Sketch | Flash | RAM |
|---|---|---|
| DeskCycleCSC | 590,088 B (45%) | 36.5 KB (11%) |
| DeskCycleFTMS | 977,678 B (74%) | 29.6 KB (9%) |

Serial console at 115200 baud prints one line every 5 s (revs, cadence,
watts) for bench testing without a watch.

## Garmin setup

### CSC firmware

1. Watch → Settings → Sensors & Accessories → Add New → discover
   "DC2 Cadence"
2. After pairing, open the sensor's settings and set **wheel
   circumference = 7010 mm** (23 ft/rev). Without this the watch
   assumes a ~2100 mm road wheel and speed reads ~3.3× high.
3. Use the **Indoor Bike** activity; cadence comes from the crank
   counter, speed/distance from the wheel counter at 7010 mm.

### FTMS firmware

1. Watch → Settings → Sensors & Accessories → Add New → discover
   "DC2 Bike" — it pairs as a *trainer*, not a sensor
2. No circumference setting needed: the firmware reports power and
   cadence directly
3. Use the **Indoor Bike** activity; watch distance/calories derive
   from the broadcast watts

### Which firmware for which goal?

- Want cadence + physical speed/distance of the belt (23 ft truth):
  **CSC**
- Want cadence + watts that match DeskCycle's calorie math at max
  resistance: **FTMS**

## Tuning & interpretation notes

- CSC: cadence = pulse rate, matches the console RPM readout.
  At ~60 rpm belt speed is ~15.7 mph (25 km/h) — the DC2 console's own
  "speed" number folds in resistance/weight and will read
  differently; that's a virtual workload number, not physical speed.
- Garmin sometimes pairs a CSC sensor as "cadence only" if it sees the
  crank fields first. If speed stays 0, delete and re-add the sensor
  (wheel fields then bind first).
- Belt-true distance: revolutions × 23 ft. Console-"mile" distance:
  revolutions ÷ 240. They differ ~4.6%.
- FTMS: if your watch shows cadence but power 0 during the first
  seconds, that's the 3-rev cadence window filling; it settles within
  ~2 s of pedalling.

## Roadmap

- [ ] Hardware-test the FTMS build against a real Forerunner
- [ ] Verify jack pulse = 1 console RPM (1:1) vs flywheel gearing
- [ ] Garmin-vs-console calorie agreement check at max resistance
- [ ] Bench fixture: drive the reed input with a signal generator to
      test cadence edge cases (stop/restart, dropouts) without pedalling

## License

MIT — see [LICENSE](LICENSE).