#include "influx.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <math.h>

InfluxWriter::InfluxWriter(const char* url, const char* org, const char* bucket, const char* token)
    : url_(url), org_(org), bucket_(bucket), token_(token) {}

void InfluxWriter::appendField(String& out, const char* key, float v, bool& first) {
    if (isnan(v)) return;
    if (!first) out += ',';
    out += key;
    out += '=';
    out += String(v, 4);
    first = false;
}

void InfluxWriter::appendIntField(String& out, const char* key, int32_t v, bool& first) {
    if (v == INT32_MIN) return;
    if (!first) out += ',';
    out += key;
    out += '=';
    out += String(v);
    out += 'i';
    first = false;
}

void InfluxWriter::enqueue(const VictronSample& s) {
    // measurement,name=<name>,type=<type> field1=...,field2=...
    String line = "victron,name=";
    line += s.name ? s.name : "unknown";
    line += ",type=";
    line += String((int)s.type);
    line += ' ';

    bool first = true;
    appendField(line, "batt_v",   s.batteryVoltage,    first);
    appendField(line, "batt_a",   s.batteryCurrent,    first);
    appendField(line, "soc",      s.soc,               first);
    appendField(line, "cons_ah",  s.consumedAh,        first);
    appendIntField(line, "ttg_min", s.timeToGoMin,     first);
    appendField(line, "aux_v",    s.auxVoltage,        first);
    appendField(line, "temp_c",   s.temperatureC,      first);
    appendField(line, "mid_v",    s.midpointVoltage,   first);
    appendField(line, "pv_w",     s.solarPowerW,       first);
    appendField(line, "yield_wh", s.yieldTodayWh,      first);
    appendField(line, "load_a",   s.loadCurrentA,      first);
    appendField(line, "ac_va",    s.acApparentPowerVA, first);
    appendField(line, "ac_v",     s.acVoltage,         first);
    appendField(line, "ac_a",     s.acCurrent,         first);
    if (s.chargerState != 0xFF) {
        if (!first) line += ',';
        line += "state="; line += String(s.chargerState); line += 'i';
        first = false;
    }
    if (s.chargerError != 0xFF) {
        if (!first) line += ',';
        line += "err=";  line += String(s.chargerError); line += 'i';
        first = false;
    }
    if (first) return; // nothing to record

    line += '\n';
    buffer_ += line;

    // Cap buffer to avoid runaway growth if WiFi is down.
    if (buffer_.length() > 8192) {
        int cut = buffer_.indexOf('\n', buffer_.length() - 8192);
        if (cut > 0) buffer_.remove(0, cut + 1);
    }
}

bool InfluxWriter::maybeFlush(uint32_t intervalMs) {
    uint32_t now = millis();
    if (now - lastFlushMs_ < intervalMs) return false;
    if (buffer_.length() == 0) { lastFlushMs_ = now; return false; }
    if (WiFi.status() != WL_CONNECTED) return false;

    String endpoint = url_;
    endpoint += "/api/v2/write?org=";
    endpoint += org_;
    endpoint += "&bucket=";
    endpoint += bucket_;
    endpoint += "&precision=s";

    HTTPClient http;
    http.begin(endpoint);
    http.addHeader("Authorization", String("Token ") + token_);
    http.addHeader("Content-Type", "text/plain; charset=utf-8");
    int rc = http.POST((uint8_t*)buffer_.c_str(), buffer_.length());
    if (rc >= 200 && rc < 300) {
        Serial.printf("[influx] flushed %u bytes ok\n", (unsigned)buffer_.length());
        buffer_ = "";
    } else {
        Serial.printf("[influx] write failed: HTTP %d (%s)\n", rc, http.errorToString(rc).c_str());
    }
    http.end();
    lastFlushMs_ = now;
    return true;
}
