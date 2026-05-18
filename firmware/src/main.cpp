// victron-broadcast firmware
//   - Scans BLE for Victron "Instant Readout" advertisements
//   - Decrypts/parses per configured device
//   - Pushes samples to InfluxDB Cloud over WiFi
//
// Configure secrets and device list in `include/config.h` (copy from
// config.example.h). config.h is gitignored.

#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <time.h>
#include "victron_ble.h"
#include "influx.h"
#include "config.h"

static InfluxWriter influx(INFLUX_URL, INFLUX_ORG, INFLUX_BUCKET, INFLUX_TOKEN);

// Victron BLE manufacturer ID (Apple's range; Victron registered 0x02E1).
static constexpr uint16_t VICTRON_MANUF_ID = 0x02E1;

class AdvCallbacks : public NimBLEAdvertisedDeviceCallbacks {
    void onResult(NimBLEAdvertisedDevice* dev) override {
        if (!dev->haveManufacturerData()) return;
        std::string md = dev->getManufacturerData();
        if (md.size() < 3) return;
        uint16_t mid = (uint8_t)md[0] | ((uint16_t)(uint8_t)md[1] << 8);
        if (mid != VICTRON_MANUF_ID) return;

        const uint8_t* payload = (const uint8_t*)md.data() + 2;
        size_t payloadLen = md.size() - 2;

        String mac = dev->getAddress().toString().c_str();
        mac.toLowerCase();

        VictronSample s{};
        if (!VictronDecoder::decode(payload, payloadLen, mac.c_str(),
                                    VICTRON_DEVICES, VICTRON_DEVICE_COUNT, s)) {
            return;
        }

        Serial.printf("[victron] %s type=%d", s.name, (int)s.type);
        if (!isnan(s.batteryVoltage)) Serial.printf(" V=%.2f", s.batteryVoltage);
        if (!isnan(s.batteryCurrent)) Serial.printf(" I=%.3f", s.batteryCurrent);
        if (!isnan(s.soc))            Serial.printf(" SoC=%.1f%%", s.soc);
        if (!isnan(s.solarPowerW))    Serial.printf(" PV=%.0fW", s.solarPowerW);
        Serial.println();

        influx.enqueue(s);
    }
};

static void connectWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false); // BLE + WiFi coexistence: keep WiFi awake
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("[wifi] connecting to %s", WIFI_SSID);
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
    delay(500);
    Serial.println("\nvictron-broadcast booting");

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
    // Reconnect WiFi if dropped.
    static uint32_t lastWifiCheck = 0;
    if (millis() - lastWifiCheck > 5000) {
        lastWifiCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[wifi] reconnecting");
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        }
    }

    influx.maybeFlush(INFLUX_FLUSH_INTERVAL_MS);
    delay(50);
}
