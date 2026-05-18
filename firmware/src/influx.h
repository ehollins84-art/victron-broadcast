#pragma once
#include <Arduino.h>
#include "victron_ble.h"

class InfluxWriter {
public:
    InfluxWriter(const char* url, const char* org, const char* bucket, const char* token);

    // Append a sample to the in-memory line-protocol buffer. The newest
    // sample for each device replaces older buffered ones to keep the
    // payload small if WiFi/Influx is slow.
    void enqueue(const VictronSample& s);

    // Send the buffer if non-empty and the interval has elapsed.
    // Returns true if a flush happened (success or failure).
    bool maybeFlush(uint32_t intervalMs);

private:
    String url_;
    String org_;
    String bucket_;
    String token_;
    String buffer_;
    uint32_t lastFlushMs_ = 0;

    static void appendField(String& out, const char* key, float v, bool& first);
    static void appendIntField(String& out, const char* key, int32_t v, bool& first);
};
