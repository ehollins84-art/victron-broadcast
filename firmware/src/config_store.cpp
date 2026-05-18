#include "config_store.h"
#include <Preferences.h>
#include <ArduinoJson.h>

namespace {
constexpr const char* NS = "victron";
constexpr const char* KEY = "cfg";
}

bool ConfigStore::load(AppConfig& out) {
    Preferences p;
    if (!p.begin(NS, /*readOnly=*/true)) return false;
    String blob = p.getString(KEY, "");
    p.end();
    if (blob.length() == 0) return false;

    JsonDocument doc;
    if (deserializeJson(doc, blob) != DeserializationError::Ok) return false;

    out.wifiSsid     = doc["wifi"]["ssid"]   | "";
    out.wifiPassword = doc["wifi"]["password"] | "";
    out.influxUrl    = doc["influx"]["url"]    | "";
    out.influxOrg    = doc["influx"]["org"]    | "";
    out.influxBucket = doc["influx"]["bucket"] | "victron";
    out.influxToken  = doc["influx"]["token"]  | "";
    out.devices.clear();
    for (JsonObject d : doc["devices"].as<JsonArray>()) {
        DeviceCfg dc;
        dc.name = (const char*)(d["name"] | "");
        dc.mac  = (const char*)(d["mac"]  | "");
        dc.key  = (const char*)(d["key"]  | "");
        dc.mac.toLowerCase();
        dc.key.toLowerCase();
        if (dc.name.length() && dc.mac.length() && dc.key.length() == 32) {
            out.devices.push_back(dc);
        }
    }
    return true;
}

bool ConfigStore::save(const AppConfig& in) {
    JsonDocument doc;
    doc["wifi"]["ssid"]     = in.wifiSsid;
    doc["wifi"]["password"] = in.wifiPassword;
    doc["influx"]["url"]    = in.influxUrl;
    doc["influx"]["org"]    = in.influxOrg;
    doc["influx"]["bucket"] = in.influxBucket;
    doc["influx"]["token"]  = in.influxToken;
    JsonArray arr = doc["devices"].to<JsonArray>();
    for (const auto& d : in.devices) {
        JsonObject o = arr.add<JsonObject>();
        o["name"] = d.name;
        o["mac"]  = d.mac;
        o["key"]  = d.key;
    }
    String blob;
    serializeJson(doc, blob);

    Preferences p;
    if (!p.begin(NS, /*readOnly=*/false)) return false;
    bool ok = p.putString(KEY, blob) > 0;
    p.end();
    return ok;
}

void ConfigStore::clear() {
    Preferences p;
    if (p.begin(NS, false)) {
        p.clear();
        p.end();
    }
}
