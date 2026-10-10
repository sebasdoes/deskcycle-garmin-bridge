// DeskCycle 2 -> BLE Cycling Speed & Cadence bridge for Garmin Forerunner
// =====================================================================
// Reads the DC2 console's 3.5mm "bike computer" jack: a dry reed-switch
// contact that CLOSES once per crank revolution (sleeve = GND,
// tip/ring = switch). GPIO input with internal pull-up; falling edge
// = one revolution. ~23.0 ft of belt travel per revolution.
//
// Broadcasts the standard Bluetooth CSC service (0x1816):
//   - crank revolutions + event times  -> cadence on the watch
//   - wheel revolutions + event times  -> speed/distance
//     (set wheel circumference 7010 mm = 23 ft in the watch's sensor
//      settings, otherwise Garmin assumes a ~2100 mm road wheel)
//
// Library: NimBLE-Arduino by h2zero (v2.x, Library Manager)
// Board:   ESP32 Dev Module (classic WROOM-32; S3/C3 work too, pick a
//          GPIO with an internal pull-up)

#include <NimBLEDevice.h>

// ----------------- configuration -----------------
constexpr int      PULSE_PIN     = 25;      // 3.5mm tip/ring; sleeve to GND
constexpr uint32_t DEBOUNCE_US   = 15000;   // 15 ms reed-chatter guard
constexpr uint32_t IDLE_FINAL_MS = 2500;    // one last meas. after stopping
constexpr const char* DEVICE_NAME = "DC2 Cadence";
// --------------------------------------------------

// BLE UUIDs (16-bit, standardized)
static const NimBLEUUID CSC_SERVICE_UUID   ((uint16_t)0x1816);
static const NimBLEUUID CSC_MEAS_CHAR_UUID ((uint16_t)0x2A5B);
static const NimBLEUUID CSC_FEAT_CHAR_UUID ((uint16_t)0x2A5C);
static const NimBLEUUID CSC_LOC_CHAR_UUID  ((uint16_t)0x2A5D);

// CSC spec event time = 1/1024 s, 16-bit wrap (every 64 s)
static inline uint16_t eventTime(uint32_t nowUs) {
    return (uint16_t)((nowUs / 977ULL) & 0xFFFF);   // 1e6/1024 ≈ 976.6 us
}

// ---------------- pulse state ----------------
volatile uint32_t s_pulseCount = 0;    // total revolutions
volatile uint32_t s_lastEdgeUs = 0;

void IRAM_ATTR onPulse() {
    const uint32_t now = micros();
    const uint32_t interval = now - s_lastEdgeUs;
    if (interval && interval < DEBOUNCE_US) return;  // contact bounce
    s_lastEdgeUs = now;
    s_pulseCount++;
}

// ---------------- BLE handles ----------------
NimBLEServer*         g_server = nullptr;
NimBLECharacteristic* g_meas   = nullptr;

static volatile bool g_restartAdv = false;

class ServerCallbacks : public NimBLEServerCallbacks {
    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
        // NimBLE 2.x signature; restart advertising from the main loop
        (void)pServer; (void)connInfo; (void)reason;
        g_restartAdv = true;
    }
};

// CSC Measurement 0x2A5B payload (little-endian):
//   flags: bit0 = wheel-rev data present, bit1 = crank-rev data present
//   [cumulative wheel revs u32][wheel event time u16]
//   [cumulative crank revs u16][crank event time u16]
static void sendMeasurement(uint32_t count, bool notify) {
    const uint32_t nowUs = micros();

    uint8_t buf[11];
    buf[0] = 0x03;                       // wheel + crank data present
    size_t idx = 1;

    const uint32_t wheelRevs = count;    // 1 crank rev = 1 "wheel" rev
    const uint16_t wTime = eventTime(nowUs);
    memcpy(&buf[idx], &wheelRevs, 4); idx += 4;
    memcpy(&buf[idx], &wTime, 2);     idx += 2;

    const uint16_t crankRevs = (uint16_t)(count & 0xFFFF);
    const uint16_t cTime = eventTime(nowUs);
    memcpy(&buf[idx], &crankRevs, 2); idx += 2;
    memcpy(&buf[idx], &cTime, 2);     idx += 2;

    g_meas->setValue((uint8_t*)buf, idx);
    if (notify) g_meas->notify();
}

void setup() {
    Serial.begin(115200);
    pinMode(PULSE_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PULSE_PIN), onPulse, FALLING);

    NimBLEDevice::init(DEVICE_NAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);            // max EIRP

    g_server = NimBLEDevice::createServer();
    g_server->setCallbacks(new ServerCallbacks());

    NimBLEService* csc = g_server->createService(CSC_SERVICE_UUID);

    g_meas = csc->createCharacteristic(
        CSC_MEAS_CHAR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);

    NimBLECharacteristic* feat = csc->createCharacteristic(
        CSC_FEAT_CHAR_UUID, NIMBLE_PROPERTY::READ);
    const uint16_t features = 0x0003;   // wheel-rev + crank-rev supported
    feat->setValue((uint8_t*)&features, 2);

    NimBLECharacteristic* loc = csc->createCharacteristic(
        CSC_LOC_CHAR_UUID, NIMBLE_PROPERTY::READ);
    const uint8_t sensorLocation = 5;   // rear hub
    loc->setValue(&sensorLocation, 1);

    csc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(CSC_SERVICE_UUID);
    adv->setName(DEVICE_NAME);
    adv->start();

    Serial.println("Advertising as CSC service 0x1816");
}

void loop() {
    if (g_restartAdv) {
        g_restartAdv = false;
        NimBLEDevice::startAdvertising();
        Serial.println("Client disconnected - readvertising");
    }
    static uint32_t s_lastSentCount  = 0;
    static uint32_t s_lastActivityMs = 0;
    static bool     s_idleSent       = true;

    const uint32_t nowMs = millis();
    const uint32_t count = s_pulseCount;
    const bool changed = (count != s_lastSentCount);

    // While pedalling: notify at most every 250 ms.
    if (changed && (nowMs - s_lastActivityMs) > 250) {
        sendMeasurement(count, true);
        s_lastSentCount  = count;
        s_lastActivityMs = nowMs;
        s_idleSent       = false;
    }
    // After stopping: one final measurement ~2.5 s after the last pulse
    // so the watch records the final revolution instead of freezing
    // mid-interval with a bogus cadence.
    else if (!changed && !s_idleSent && s_lastSentCount != 0 &&
             (nowMs - s_lastActivityMs) > IDLE_FINAL_MS) {
        sendMeasurement(count, false);   // update read value, no notify
        s_idleSent = true;
    }

    delay(20);
}