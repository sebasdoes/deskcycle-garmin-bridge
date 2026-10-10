# DeskCycle 2 reed jack / calibration — private notes

- Belt: 23.0 ft per crank rev (7010 mm) — constant at every resistance.
- Console "mile" = 240 revs = 5520 ft (~4.6% long).
- Jack = dry reed contact, closes once per rev (crank-direct assumed,
  NOT yet verified 1:1 vs console RPM — verify before trusting cadence).
- Resistance knob is mechanical; no electrical output. Calibration
  assumes max (level 8) always.
- DeskCycle calculator API: GET https://api.deskcycle.com/v1/activity_logs/new
  (unauthenticated). Params: device, dial_position, pedal_distance,
  seconds_pedaled, weight_lbs, height_inches, age, gender.
- Valid cadence window 10–160 rpm (client-side validation).
- L8 power: P(W) = 0.000864·rpm² + 2.0473·rpm − 35.939 (50–140 rpm).
- Road-speed model: P = 1.0574·v + 0.013636·v³ (v in mph).
- Calories weight-dependent: L8/60rpm 165 lb → 483 kcal/h (94 sitting).
- ESP32 CSC build: 45% flash / 11% RAM. FTMS: ~74% flash / 9% RAM.
- FTMS build not yet hardware-tested; jack 1:1 gearing not yet verified.