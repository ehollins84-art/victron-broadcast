#pragma once
#include <Arduino.h>
#include <vector>

struct DeviceCfg {
    String name;
    String mac;   // lowercase aa:bb:cc:dd:ee:ff
    String key;   // 32 hex chars (the Victron bind key)
};

struct AppConfig {
    String wifiSsid;
    String wifiPassword;
    String influxUrl;
    String influxOrg;
    String influxBucket = "victron";
    String influxToken;
    std::vector<DeviceCfg> devices;

    bool isComplete() const {
        return wifiSsid.length() && influxUrl.length() && influxOrg.length()
            && influxBucket.length() && influxToken.length() && !devices.empty();
    }
};

namespace ConfigStore {
    // Load saved config from NVS. Returns false if no config has been saved yet.
    bool load(AppConfig& out);
    // Persist config to NVS.
    bool save(const AppConfig& in);
    // Wipe NVS (so next boot enters the portal again).
    void clear();
}
