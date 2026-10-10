# Calibration notes — reverse-engineering the DeskCycle calculator

Probed 2026-10 by scripted GET requests against the endpoint used by
the official calculator app at https://app.deskcycle.com/:

```
GET https://api.deskcycle.com/v1/activity_logs/new
    ?device=deskcycle 2
    &dial_position=<1..8>
    &pedal_distance=<display miles>
    &seconds_pedaled=<sec>
    &weight_lbs=<90..500>
    &height_inches=<48..83>
    &age=<18..80>
    &gender=male|female
    &user_id=&source=web&source_version=probe
    &started_at=...
```

Response fields include `average_power` (Watts), `average_torque`
(in-lb), `average_tangential_force` (lbs), `pedal_speed_rpm`,
`total_calories`, `calories_sitting`, `additional_calories_pedaling`,
`total_calories_per_hour`, `miles_cycled`, `steps`, `pedal_strokes`.

No authentication, no rate limiting observed at research pace
(~0.7 s between requests).

Client-side validation found in the app bundle (main.4abc197d.js):
pedal RPM is computed as `240 × distance_miles ÷ minutes` for
DeskCycle 1/2 (`revolutions ÷ minutes` for the Ellipse), and requests
with rpm < 10 or > 160 are rejected before hitting the API.
→ **1 console "mile" = 240 pedal revolutions**; at 23.0 ft/rev that's
5520 ft ≈ 1.045 statute miles (console miles run ~4.6% long).

## Probe results

### Level sweep (60 rpm, 165 lb male, 45 y, 60 min)

Consistent inputs: pedal_distance = 15 display miles (= 60 rpm by the
240-revs/mile rule).

| Level | Watts | Torque (in-lb) | Tangential force (lbs) | kcal/h total | kcal/h sitting | kcal/h pedaling |
|---|---|---|---|---|---|---|
| 1 | 10.0 | 14.0 | 4.0 | 149 | 94 | 55 |
| 2 | 19.4 | 27.4 | 7.8 | 201 | 94 | 107 |
| 3 | 25.5 | 35.9 | 10.3 | 234 | 94 | 140 |
| 4 | 35.2 | 49.5 | 14.2 | 287 | 94 | 193 |
| 5 | 48.2 | 67.9 | 19.4 | 342 | 94 | 248 |
| 6 | 59.9 | 84.3 | 24.1 | 381 | 94 | 287 |
| 7 | 72.4 | 102.1 | 29.2 | 423 | 94 | 329 |
| 8 | 90.0 | 126.8 | 36.2 | 483 | 94 | 389 |

Power is a per-level factory table (level→W is jagged, no smooth
formula). Torque is exactly `in-lb = hp × 63025 ÷ rpm`, and tangential
force = torque ÷ 3.5 in (crank arm) — all derived from watts.

### Cadence sweep (level 8, 165 lb male)

pedal_distance swept so that rpm = 240·distance/60 (valid range only).

| rpm | Watts | kcal/h |
|---|---|---|
| 50 | 68.6 | 411 |
| 55 | 79.3 | 447 |
| 60 | 90.0 | 483 |
| 65 | 100.8 | 519 |
| 70 | 111.6 | 556 |
| 75 | 122.5 | 592 |
| 80 | 133.4 | 629 |
| 85 | 144.3 | 666 |
| 90 | 155.3 | 703 |
| 95 | 166.3 | 740 |
| 100 | 177.4 | 778 |
| 105 | 188.5 | 815 |
| 110 | 199.7 | 853 |
| 115 | 210.9 | 891 |
| 120 | 222.2 | 929 |
| 140 | 267.6 | 1082 |

Least-squares quadratic over the 60–140 rpm subset:

```
P(W) = 0.000864·rpm² + 2.0473·rpm − 35.939
```

Fit residuals ≤ 0.03 W. Spot-checks at 50/55/65/…/115 rpm agree with
the curve within 0.05 W.

### Weight / age / height / sex sweeps (60 rpm, level 8)

| Input change | Watts | total kcal/h |
|---|---|---|
| weight 120 lb | 90.0 | 405 |
| weight 165 lb | 90.0 | 483 |
| weight 220 lb | 90.0 | 578 |
| female | 90.0 | — |
| age 70 / 63 in / 110 lb | 90.0 | — |

→ **Watts are weight/age/height/sex-blind.** Calories scale with
weight (sitting baseline ~94→113 kcal/h across 120→220 lb).

### Duration check

30 min @ 60 rpm (pedal_distance 7.5) → total 241 kcal,
`total_calories_per_hour` 482, watts 90.0 → per-hour rates are
duration-independent, totals scale linearly.

### Distance / speed model

`display_miles_cycled` echoes the input regardless of level (15.0 at
L1/L4/L8). But the **road-bike** outputs move with level: at 60 rpm,
L1/L4/L8 → 6.3 / 11.8 / 17.4 mph road-equivalent and 2243 / 7476 /
11501 walking steps.

Fitting all collected (power, road-speed) pairs — level sweep and L8
cadence sweep (17.4/20.2/22.4 mph at 90/133/177 W) — to a physical
road-bike model recovers it exactly:

```
P(W) = 1.0574 · v(mph) + 0.013636 · v³(mph)
```

(residuals ≤ 0.5 W over 11 points). The calculator derives road speed
by inverting this with the computed watts, which is why road-equivalent
speed/distance respond to level exactly like real biking.

Belt-true speed for contrast: 23 ft × rpm. 60 rpm = 15.7 mph regardless
of level — 10% below the L8 road-equivalent at the same cadence.

## Reuse

The probe script shape (stdlib Python):

```python
import json, urllib.request, urllib.parse

def calc(**kw):
    params = {
        "device": "deskcycle 2", "gender": "male", "age": 45, "revolutions": 0,
        "height_inches": 71, "weight_lbs": 165, "dial_position": 8,
        "seconds_pedaled": 3600, "pedal_distance": 15.0, "user_id": "", "source": "web",
        "source_version": "probe", "started_at": "Fri Oct 09 2026 10:00:00 GMT+0000",
    }
    params.update(kw)
    url = "https://api.deskcycle.com/v1/activity_logs/new?" + urllib.parse.urlencode(params)
    with urllib.request.urlopen(urllib.request.Request(
            url, headers={"Accept": "application/json"}), timeout=30) as r:
        return json.load(r)

d = calc(dial_position=8, pedal_distance=20.0)   # 80 rpm
print(d["average_power"], d["total_calories_per_hour"])
```

To calibrate levels 1–7: repeat the cadence sweep per level, fit the
quadratic, swap the coefficients into `DeskCycleFTMS.ino`
(`PWR_A2`, `PWR_B1`, `PWR_C0`).