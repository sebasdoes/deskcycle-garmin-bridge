# DeskCycle 2 → Garmin bridge (ESP32)

ESP32 firmware that turns a DeskCycle 2 under-desk bike into standard
Bluetooth fitness equipment: pair it with a Garmin Forerunner (or any
BLE CSC / FTMS app) and get cadence, speed and — with the FTMS build —
power in Watts calibrated against DeskCycle's own calculator model.

- Firmware 1 — **CSC** (Cycling Speed & Cadence, 0x1816): cadence +
  belt-true speed/distance. Working, build-verified.
- Firmware 2 — **FTMS** (Fitness Machine Service, 0x1826): cadence +
  **watts** + kcal. The watch pairs with it as a trainer. Power uses the
  level-8 (max resistance) curve reverse-engineered from DeskCycle's
  official online calculator.

Full details, wiring, Garmin setup and the complete calibration story
are in the **[main README](../README.md)** and
**[docs/calibration.md](calibration.md)**.

This directory holds private implementation notes.