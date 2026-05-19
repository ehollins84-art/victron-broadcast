// victron-broadcast firmware
//
//   - Reads config from NVS. If incomplete (or the BOOT button is held
//     at power-on), launches the on-device setup portal and waits there.
//   - Otherwise: connects to WiFi, scans BLE for Victron Instant Readout
//     adverts, decrypts/parses, and pushes samples to InfluxDB Cloud.

#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include "victron_ble.h"
#include "influx.h"
#include "config_store.h"
#include "portal.h"

// Held LOW at boot => force portal mode (BOOT button on most S3 DevKits).
static constexpr int RESET_PIN = 0;
// Xiao ESP32-S3 user LED (active LOW). Used as a heartbeat so a healthy chip
// is visually obvious even without a serial console.
static constexpr int STATUS_LED = 21;

static AppConfig g_cfg;
static InfluxWriter g_influx;
static constexpr uint32_t INFLUX_FLUSH_INTERVAL_MS = 5000;

// Print boot env and halt (don't crash/loop) if we're on the wrong silicon.
// This firmware is built for the Seeed Xiao ESP32-S3 (ESP32-S3R8, 8MB QIO
// flash + 8MB OPI PSRAM). Running it on a different ESP32-S3 variant with
// PSRAM enabled in the bootloader can hammer the chip's SPI/PSRAM pins in
// a bootloop and physically damage USB peripherals.
static void verifyHardwareOrHalt() {
    Serial.printf("[boot] chip=%s rev=%d cores=%d\n",
                  ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores());
    Serial.printf("[boot] flash=%uMB @%uMHz mode=%d\n",
                  ESP.getFlashChipSize() / (1024u * 1024u),
                  ESP.getFlashChipSpeed() / 1000000u,
                  (int)ESP.getFlashChipMode());
    Serial.printf("[boot] psram=%uMB\n", ESP.getPsramSize() / (1024u * 1024u));

    if (String(ESP.getChipModel()) != "ESP32-S3") {
        Serial.println("[boot] FATAL: this firmware is for the Seeed Xiao ESP32-S3 only.");
        Serial.println("[boot] Halting instead of bootlooping. Power off the board.");
        // Slow heartbeat so the user can tell the chip is alive but parked.
        pinMode(STATUS_LED, OUTPUT);
        for (;;) {
            digitalWrite(STATUS_LED, LOW);  delay(100);
            digitalWrite(STATUS_LED, HIGH); delay(900);
        }
    }
}

class AdvCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        if (!dev->haveManufacturerData()) return;
        std::string md = dev->getManufacturerData();
        if (md.size() < 3) return;
        uint16_t mid = (uint8_t)md[0] | ((uint16_t)(uint8_t)md[1] << 8);
        if (mid != 0x02E1) return;

        const uint8_t* payload = (const uint8_t*)md.data() + 2;
        size_t payloadLen = md.size() - 2;

        String mac = dev->getAddress().toString().c_str();
        mac.toLowerCase();

        VictronSample s{};
        if (!VictronDecoder::decode(payload, payloadLen, mac.c_str(),
                                    g_cfg.devices, s)) {
            return;
        }

        Serial.printf("[victron] %s type=%d", s.name, (int)s.type);
        if (!isnan(s.batteryVoltage)) Serial.printf(" V=%.2f", s.batteryVoltage);
        if (!isnan(s.batteryCurrent)) Serial.printf(" I=%.3f", s.batteryCurrent);
        if (!isnan(s.soc))            Serial.printf(" SoC=%.1f%%", s.soc);
        if (!isnan(s.solarPowerW))    Serial.printf(" PV=%.0fW", s.solarPowerW);
        Serial.println();

        g_influx.enqueue(s);
    }
};

static void connectWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    WiFi.begin(g_cfg.wifiSsid.c_str(), g_cfg.wifiPassword.c_str());
    Serial.printf("[wifi] connecting to %s", g_cfg.wifiSsid.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
        delay(500);
        Serial.print('.');
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[wifi] connected, ip=%s rssi=%d\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
        Serial.println("[wifi] connect failed, will retry in background");
    }
}

void setup() {
    Serial.begin(115200);
    delay(300);
    pinMode(RESET_PIN, INPUT_PULLUP);
    pinMode(STATUS_LED, OUTPUT);
    // Three quick blinks: "I'm alive, made it to setup()".
    for (int i = 0; i < 3; ++i) {
        digitalWrite(STATUS_LED, LOW);  delay(80);
        digitalWrite(STATUS_LED, HIGH); delay(80);
    }
    Serial.println("\nvictron-broadcast booting");

    verifyHardwareOrHalt();

    bool forcePortal = (digitalRead(RESET_PIN) == LOW);
    if (forcePortal) {
        Serial.println("[boot] BOOT button held -> portal mode");
    }

    bool loaded = ConfigStore::load(g_cfg);
    if (!loaded || !g_cfg.isComplete() || forcePortal) {
        Serial.println("[boot] no/incomplete config -> entering setup portal");
        runSetupPortal(g_cfg); // never returns
    }

    Serial.printf("[boot] loaded config: %u device(s)\n", (unsigned)g_cfg.devices.size());
    for (const auto& d : g_cfg.devices) {
        Serial.printf("  - %s @ %s\n", d.name.c_str(), d.mac.c_str());
    }

    g_influx.configure(g_cfg.influxUrl, g_cfg.influxOrg, g_cfg.influxBucket, g_cfg.influxToken);

    connectWifi();

    NimBLEDevice::init("victron-broadcast");
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(new AdvCallbacks(), /*wantDuplicates=*/true);
    scan->setActiveScan(false);
    scan->setInterval(160);  // * 0.625 ms = 100 ms
    scan->setWindow(160);
    scan->start(0, nullptr, false); // continuous
    Serial.println("[ble] scanning");
}

void loop() {
    static uint32_t lastWifiCheck = 0;
    if (millis() - lastWifiCheck > 5000) {
        lastWifiCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[wifi] reconnecting");
            WiFi.disconnect();
            WiFi.begin(g_cfg.wifiSsid.c_str(), g_cfg.wifiPassword.c_str());
        }
    }

    // 50ms-on / 1950ms-off heartbeat: app is in normal scan/upload loop.
    static uint32_t lastBeat = 0;
    uint32_t now = millis();
    digitalWrite(STATUS_LED, (now - lastBeat < 50) ? LOW : HIGH);
    if (now - lastBeat > 2000) lastBeat = now;

    g_influx.maybeFlush(INFLUX_FLUSH_INTERVAL_MS);

    // Hold BOOT for ~3s at runtime to wipe config and re-enter portal.
    static uint32_t bootDownSince = 0;
    if (digitalRead(RESET_PIN) == LOW) {
        if (bootDownSince == 0) bootDownSince = millis();
        if (millis() - bootDownSince > 3000) {
            Serial.println("[boot] long-press detected, wiping config");
            ConfigStore::clear();
            delay(200);
            ESP.restart();
        }
    } else {
        bootDownSince = 0;
    }
    delay(50);
}
