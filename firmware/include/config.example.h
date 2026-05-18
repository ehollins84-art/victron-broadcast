// Copy this file to `config.h` and fill in your values.
// config.h is gitignored so your secrets never get committed.
#pragma once

// ------------------------ WiFi ------------------------
#define WIFI_SSID      "your-wifi-ssid"
#define WIFI_PASSWORD  "your-wifi-password"

// ------------------------ InfluxDB Cloud Serverless ------------------------
// Create a free account at https://cloud2.influxdata.com/signup
// Then create a bucket (e.g. "victron") and an API token with write access to it.
// INFLUX_URL is the "Cluster URL" shown in your account, e.g. https://us-east-1-1.aws.cloud2.influxdata.com
#define INFLUX_URL    "https://us-east-1-1.aws.cloud2.influxdata.com"
#define INFLUX_ORG    "your-org-id-or-email"
#define INFLUX_BUCKET "victron"
#define INFLUX_TOKEN  "your-influxdb-api-token"

// How often to flush buffered samples to InfluxDB (ms).
#define INFLUX_FLUSH_INTERVAL_MS 5000

// ------------------------ Victron devices ------------------------
// Add one entry per Victron device you want to monitor.
//   name : friendly tag used in the dashboard
//   mac  : BLE MAC address, lowercase, colon-separated (find in VictronConnect)
//   key  : 32-char hex "encryption key" from VictronConnect:
//          Device -> Settings -> Product info -> Encryption data
//          (also called "advertisement key" / "bindkey")
#define VICTRON_DEVICE_COUNT 2
static const VictronDevice VICTRON_DEVICES[VICTRON_DEVICE_COUNT] = {
    { "shunt",   "ab:cd:ef:01:02:03", "00112233445566778899aabbccddeeff" },
    { "mppt",    "ab:cd:ef:04:05:06", "ffeeddccbbaa99887766554433221100" },
};
