# victron-broadcast

ESP32-S3 firmware that listens to Victron "Instant Readout" BLE
advertisements, decrypts them, and pushes the readings to InfluxDB Cloud
so you can view live + historical charts on a public Grafana Cloud
dashboard from anywhere in the world.

```
 Victron device(s)                ESP32-S3                Cloud (free)
 ┌──────────────┐    BLE adv     ┌──────────────┐  HTTPS  ┌─────────────┐
 │ SmartShunt   │ ───────────▶   │ scan + decode│ ──────▶ │ InfluxDB    │
 │ MPPT         │ ───────────▶   │ AES-CTR      │         │ Cloud       │
 │ Inverter     │ ───────────▶   │ WiFi push    │         └──────┬──────┘
 └──────────────┘                └──────────────┘                │
                                                                 ▼
                                                          ┌─────────────┐
                                                          │  Grafana    │
                                                          │  Cloud      │
                                                          │  (public)   │
                                                          └─────────────┘
```

## What you need

- ESP32-S3 board (DevKitC-1, Feather S3, etc. — anything with BLE + WiFi).
- USB-C cable.
- VictronConnect app on your phone (to read each device's bind key once).
- Free accounts at:
  - [InfluxDB Cloud Serverless](https://cloud2.influxdata.com/signup) — time-series store.
  - [Grafana Cloud](https://grafana.com/auth/sign-up/create-user) — dashboards with public sharing.
- [PlatformIO](https://platformio.org/install) (VS Code extension or the CLI) to build/flash.

## 1. Grab the Victron bind keys

For each Victron device you want to monitor:

1. Open VictronConnect, connect to the device.
2. Settings (gear icon) → Product info.
3. Scroll to **Instant readout via Bluetooth** → toggle it **on**.
4. Tap **Show** under "Encryption data" → copy the 32-hex-character key.
5. Also note the device's **BLE MAC address** (also shown on the product
   info screen, format `AA:BB:CC:DD:EE:FF`).

Repeat for each device.

## 2. Set up InfluxDB Cloud

1. Sign up at https://cloud2.influxdata.com/signup (pick the free
   Serverless plan — 5 GB ingest/month, 30-day default retention).
2. After signup, note your **Cluster URL** (e.g.
   `https://us-east-1-1.aws.cloud2.influxdata.com`) — visible in the
   account menu.
3. Create a bucket named `victron` (Load Data → Buckets → Create Bucket).
4. Create an API token with **Write** access to that bucket
   (Load Data → API Tokens → Generate → Custom API Token). Copy it —
   it's only shown once.
5. Note your **Org ID** (Settings → About; or use your signup email).

## 3. Configure the firmware

```bash
cd firmware
cp include/config.example.h include/config.h
$EDITOR include/config.h
```

Fill in:
- `WIFI_SSID`, `WIFI_PASSWORD`
- `INFLUX_URL`, `INFLUX_ORG`, `INFLUX_BUCKET`, `INFLUX_TOKEN`
- The `VICTRON_DEVICES` array — one entry per device. Lowercase the MAC,
  paste the 32-hex bind key (no spaces).
- Update `VICTRON_DEVICE_COUNT` to match the array length.

`config.h` is gitignored, so your secrets won't be committed.

## 4. Build and flash

With PlatformIO CLI:

```bash
cd firmware
pio run -t upload    # flash
pio device monitor   # open serial @ 115200
```

You should see:

```
victron-broadcast booting
[wifi] connected, ip=192.168.x.x rssi=-52
[ble] scanning
[victron] shunt type=2 V=13.45 I=-1.230 SoC=87.5%
[victron] mppt  type=1 V=13.45 PV=42W
[influx] flushed 312 bytes ok
```

If you see Victron lines but no InfluxDB flush, check your token + bucket
+ cluster URL. If you see nothing, double-check the bind key and that
Instant Readout is enabled on the Victron device.

## 5. Build the public dashboard

1. In **Grafana Cloud**, add an **InfluxDB** data source:
   - Query language: **Flux**
   - URL: your InfluxDB Cluster URL
   - Auth → custom HTTP header `Authorization: Token <YOUR_TOKEN>`
   - Default org/bucket: your values from step 2.
2. Create a new dashboard. Example Flux query for the SmartShunt:
   ```flux
   from(bucket: "victron")
     |> range(start: -24h)
     |> filter(fn: (r) => r._measurement == "victron" and r.name == "shunt")
     |> filter(fn: (r) => r._field == "batt_v" or r._field == "soc")
   ```
3. Add panels for whatever you care about: battery V, SoC, PV W, AC V,
   etc. The field names are listed below.
4. **Share publicly:** dashboard settings → **Public dashboards** →
   enable. Grafana gives you a URL anyone can open from anywhere. No
   login, no port-forwarding, no router setup.

## Field reference

Measurement: `victron`. Tags: `name` (your device label), `type`
(record type id). Fields written when the device reports them:

| field      | unit | source records           |
|------------|------|--------------------------|
| `batt_v`   | V    | all                      |
| `batt_a`   | A    | shunt, MPPT              |
| `soc`      | %    | shunt                    |
| `cons_ah`  | Ah   | shunt (negative = drawn) |
| `ttg_min`  | min  | shunt                    |
| `aux_v`    | V    | shunt (starter aux)      |
| `temp_c`   | °C   | shunt (temp aux)         |
| `mid_v`    | V    | shunt (midpoint aux)     |
| `pv_w`     | W    | MPPT                     |
| `yield_wh` | Wh   | MPPT (today's yield)     |
| `load_a`   | A    | MPPT (load output)       |
| `ac_va`    | VA   | inverter                 |
| `ac_v`     | V    | inverter                 |
| `ac_a`     | A    | inverter                 |
| `state`    | enum | MPPT, inverter, DC-DC    |
| `err`      | enum | MPPT, DC-DC              |

## Supported record types

Decoded today: Battery Monitor (SmartShunt/BMV), Solar Charger (MPPT),
Inverter, DC-DC Converter. Other record types (AC Charger, Smart
Lithium, Lynx BMS, Multi RS, etc.) are recognized and logged but not
parsed — open an issue or extend `firmware/src/victron_ble.cpp`.

## Layout

```
firmware/
  platformio.ini
  include/
    config.example.h   # template — copy to config.h
  src/
    main.cpp           # WiFi + BLE scan + dispatch
    victron_ble.{h,cpp} # AES-CTR decrypt + bitfield parsers
    influx.{h,cpp}      # batched line-protocol writer
```

## Troubleshooting

- **`[victron]` lines never appear**: bind key wrong, MAC wrong, or
  Instant Readout disabled in VictronConnect. Verify with `idf.py monitor`
  or `pio device monitor` — every Victron BLE advert seen will be
  decrypted and logged.
- **`HTTP -1` from InfluxDB**: usually WiFi DNS or TLS root cert. Make
  sure the ESP32-S3 has time sync if your TLS chain requires it (most
  Cloud endpoints do — the Arduino `WiFiClientSecure` uses the bundled
  root CA store by default).
- **BLE + WiFi instability**: keep `WiFi.setSleep(false)` (already set
  in `main.cpp`); the BLE radio and WiFi share the same antenna on
  ESP32-S3 and modem sleep can cause stalls.
