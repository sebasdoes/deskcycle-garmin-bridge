// DeskCycle 2 -> BLE FTMS Fitness Machine bridge for Garmin Forerunner
// =====================================================================
// Reads the DC2 console's 3.5mm "bike computer" jack: a dry reed-switch
// contact that CLOSES once per revolution (sleeve = GND, tip/ring =
// switch). GPIO input with internal pull-up; falling edge = 1 rev.
//
// Broadcasts the standard Bluetooth FTMS service (0x1826) so the watch
// pairs with it as a *trainer* and displays cadence AND power:
//   - Indoor Bike Data characteristic (0x2AD2), notifies every 500 ms
//     while pedalling: cadence (rpm), instantaneous power (W), total
//     energy (kcal)
//   - Power comes from DeskCycle's own calibration model for level 8
//     (max resistance), reverse-engineered from their official online
//     calculator (see docs/calibration.md in this repo):
//
//       P(W) = 0.000864 * rpm^2 + 2.0473 * rpm - 35.939
//
//     (60 rpm = 90 W, 80 = 133 W, 100 = 177 W; fit error <= 0.05 W)
//
// Assumption: 1 jack pulse = 1 crank revolution, i.e. jack RPM equals
// the console RPM readout. VERIFY once against the console: if cadence
// differs by a constant factor k, set JACK_REV_PER_CRANK below (some
// units may gear the jack to the flywheel instead of the crank).
//
// Byte layout follows the Bluetooth FTMS spec, cross-checked against
// eiffelpeter/esp32_ftms_indoor_bike:
//   Indoor Bike Data flags: bit3 instantaneous cadence (value 4),
//     bit7 instantaneous power (value 64), bit9 total energy (value 256)
//   Cadence field resolution = 1/2 rpm (value = rpm * 2)
//   Speed field is ALWAYS present (uint16, 0.01 km/h) — sent as 0 and
//     the watch derives speed from watts
//
// Library: NimBLE-Arduino by h2zero (v2.x, Library Manager)
// Board:   ESP32 Dev Module (classic WROOM-32; S3/C3 work too)

#include <NimBLEDevice.h>

// ----------------- configuration -----------------
constexpr int      PULSE_PIN     = 25;      // 3.5mm tip/ring; sleeve to GND
constexpr uint32_t DEBOUNCE_US   = 15000;   // 15 ms reed-chatter guard
constexpr const char* DEVICE_NAME = "DC2 Bike";

// Jack-to-crank gearing correction. 1.0 = jack is crank-direct (1 pulse
// = 1 console RPM). If watch cadence reads k x console RPM, set this to
// 1/k.
constexpr float    JACK_REV_PER_CRANK = 1.0f;

// Power curve, DeskCycle level 8 (max resistance). See docs/calibration.md.
constexpr float    PWR_A2 = 0.000864f;
constexpr float    PWR_B1 = 2.0473f;
constexpr float    PWR_C0 = -35.939f;

// Calories: gross kcal from mechanical work at ~20% human efficiency.
// Validated against the DeskCycle calculator: 90 W for 1 h -> 387 kcal
// computed vs 389 kcal/h reported (pedaling term, excludes sitting
// baseline the watch adds itself).
constexpr float    WH_TO_KCAL_GROSS = 3.6f * 1.193f;   // Wh -> kJ -> kcal / 0.20
// --------------------------------------------------

// BLE UUIDs (16-bit, standardized)
static const NimBLEUUID FTMS_SERVICE_UUID  ((uint16_t)0x1826);
static const NimBLEUUID FTMS_MT_FEATURE    ((uint16_t)0x2ACC);
static const NimBLEUUID FTMS_IBD_CHAR_UUID ((uint16_t)0x2AD2);  // Indoor Bike Data
static const NimBLEUUID FTMS_TRAIN_STATUS  ((uint16_t)0x2AD3);
static const NimBLEUUID FTMS_CP_CHAR_UUID  ((uint16_t)0x2AD9);

// ---------------- pulse state (ISR) ----------------
volatile uint32_t s_pulseCount = 0;      // total revolutions
volatile uint32_t s_lastEdgeUs = 0;
volatile uint32_t s_revUs[4]   = {0, 0, 0, 0};  // ring buffer of last rev timestamps
volatile size_t   s_revIdx     = 0;

void IRAM_ATTR onPulse() {
    const uint32_t now = micros();
    const uint32_t interval = now - s_lastEdgeUs;
    if (interval && interval < DEBOUNCE_US) return;   // contact bounce
    s_lastEdgeUs = now;
    s_pulseCount++;
    s_revUs[s_revIdx] = now;
    s_revIdx = (s_revIdx + 1) % 4;
}

// ---------------- cadence ----------------
// Cadence from the newest pulse interval (snappy); idle-damped to zero.
// Returns rpm * 2 (FTMS resolution 1/2 rpm), 0 = idle.
uint16_t cadenceRpm2(float crankRpm) {
    if (crankRpm <= 0) return 0;
    const int v = (int)lroundf(crankRpm * 2.0f);
    return (uint16_t)(v > 0xFFFF ? 0xFFFF : v);
}

// Crank rpm over the last pulse interval, gated for staleness.
float currentCrankRpm(uint32_t nowUs) {
    const size_t newest = (s_revIdx + 3) % 4;    // last written slot
    const uint32_t tNew = s_revUs[newest];
    if (tNew == 0 || (nowUs - tNew) > 3000000ULL) return 0.0f;   // idle > 3 s
    const size_t prev = (s_revIdx + 2) % 4;
    const uint32_t tPrev = s_revUs[prev];
    if (tPrev == 0) return 0.0f;                 // only one rev so far
    const uint32_t span = tNew - tPrev;          // one full rev
    if (span == 0) return 0.0f;
    return 60000000.0f / (float)span;            // us per rev -> rpm
}

// ---------------- BLE handles ----------------
NimBLEServer*         g_server = nullptr;
NimBLECharacteristic* g_ibd    = nullptr;

static volatile bool g_restartAdv = false;

class ServerCallbacks : public NimBLEServerCallbacks {
    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        (void)pServer; (void)connInfo; (void)reason;
        g_restartAdv = true;
    }
};

// ---------------- FTMS Indoor Bike Data 0x2AD2 ----------------
// Field order fixed by spec regardless of flags:
//   flags u16 | speed u16 | [more-data fields...] |
//   cadence u16 | [avg cadence] | [distance] | [resistance] | power s16 |
//   [expended energy] ...
// With flags = instCadence | instPower | totalEnergy the layout is:
//   flags, speed(0), cadence, power, energy  → 10 bytes
static void sendIndoorBikeData(uint16_t cad2, int16_t watts, uint16_t kcalTotal, bool notify) {
    uint8_t buf[10];
    const uint16_t flags = 0x0004 | 0x0040 | 0x0100;   // bits 3, 7, 9
    size_t i = 0;
    const uint16_t speedCkph2 = 0;                     // field always present
    memcpy(&buf[i], &flags, 2);       i += 2;
    memcpy(&buf[i], &speedCkph2, 2);  i += 2;
    memcpy(&buf[i], &cad2, 2);        i += 2;
    memcpy(&buf[i], &watts, 2);       i += 2;
    memcpy(&buf[i], &kcalTotal, 2);   i += 2;
    g_ibd->setValue((uint8_t*)buf, i);
    if (notify) g_ibd->notify();
}

void setup() {
    Serial.begin(115200);
    pinMode(PULSE_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PULSE_PIN), onPulse, FALLING);

    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);            // max EIRP

    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(new ServerCallbacks());

    NimBLEService* ftms = g_server->createService(FTMS_SERVICE_UUID);

    NimBLECharacteristic* feat = ftms->createCharacteristic(
        FTMS_MT_FEATURE, NIMBLE_PROPERTY::READ);
    // Fitness Machine Feature u32 — only claim what we actually report:
    //   bit1 cadence supported, bit7 power measurement supported,
    //   bit14 energy measurement supported
    const uint32_t ff = (1u << 1) | (1u << 7) | (1u << 14);
    feat->setValue((uint8_t*)&ff, 4);

    g_ibd = ftms->createCharacteristic(
        FTMS_IBD_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    ftms->createCharacteristic(FTMS_TRAIN_STATUS, NIMBLE_PROPERTY::NOTIFY);

    ftms->createCharacteristic(FTMS_CP_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::INDICATE);

    ftms->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(FTMS_SERVICE_UUID);
    adv->setName(DEVICE_NAME);
    adv->start();

    Serial.println("Advertising as FTMS service 0x1826 (DC2 Bike)");
}

void loop() {
    if (g_restartAdv) {
        g_restartAdv = false;
        NimBLEDevice::startAdvertising();
        Serial.println("Client disconnected - readvertising");
    }

    static uint32_t s_lastSeenCount = 0;   // pulse count at previous loop pass
    static uint32_t s_lastNotifyMs  = 0;
    static float    s_kcalTotal     = 0.0f;

    const uint32_t nowMs = millis();
    const uint32_t nowUs = micros();
    const uint32_t count = s_pulseCount;
    const uint32_t newRevs = count - s_lastSeenCount;

    const float crankRpm = currentCrankRpm(nowUs) * JACK_REV_PER_CRANK;

    // notify every 500 ms while pedalling
    if (count != s_lastSeenCount && (nowMs - s_lastNotifyMs) > 500) {
        float watts = 0.0f;
        if (crankRpm > 17.0f) {            // curve goes negative below ~17 rpm
            watts = PWR_A2 * crankRpm * crankRpm + PWR_B1 * crankRpm + PWR_C0;
            if (watts < 0.0f) watts = 0.0f;
        }
        // integrate energy ONLY over revolutions performed since last pass:
        //   revs / rpm = minutes; Wh -> gross kcal at ~20% efficiency
        if (crankRpm > 0.0f) {
            const float minutes = (float)newRevs / crankRpm;
            s_kcalTotal += watts * (minutes / 60.0f) * WH_TO_KCAL_GROSS;
        }
        sendIndoorBikeData(cadenceRpm2(crankRpm), (int16_t)(watts + 0.5f),
                           (uint16_t)(s_kcalTotal + 0.5f), true);

        Serial.printf("revs=%u cad=%.1f rpm %0.1f W  kcal=%.1f\n",
                      count, crankRpm, watts, s_kcalTotal);

        s_lastSeenCount = count;
        s_lastNotifyMs  = nowMs;
    }
    // idle stop: one zeroed read-value update 3 s after last pulse
    else if (count == s_lastSeenCount && s_lastSeenCount != 0 &&
             (nowMs - s_lastNotifyMs) > 3000 && currentCrankRpm(nowUs) == 0.0f) {
        sendIndoorBikeData(0, 0, (uint16_t)(s_kcalTotal + 0.5f), false);
        s_lastNotifyMs = nowMs;
    }

    delay(20);
}