# victron-broadcast

ESP32-S3 firmware that listens to Victron "Instant Readout" BLE adverts,
decrypts them, and pushes the readings to InfluxDB Cloud — so you can
view live + historical charts on a public Grafana dashboard from
anywhere in the world.

```
 Victron device(s)               ESP32-S3                Cloud (free)
 ┌──────────────┐    BLE         ┌──────────────┐  HTTPS  ┌────────────┐
 │ SmartShunt   │ ─────────────▶ │ scan, decrypt│ ──────▶ │ InfluxDB   │
 │ MPPT         │ ─────────────▶ │ AES-CTR      │         │ Cloud      │
 │ Inverter     │ ─────────────▶ │ WiFi push    │         └─────┬──────┘
 └──────────────┘                └──────────────┘               │
                                                                ▼
                                                          ┌────────────┐
                                                          │ Grafana    │
                                                          │ Cloud      │
                                                          │ (public)   │
                                                          └────────────┘
```

There is no code editing. First boot, the ESP32 hosts its own WiFi setup
portal. You join it from your phone, fill in a form, save. Done.

## Supported hardware

This firmware is built **specifically for the Seeed Studio Xiao ESP32-S3**
(ESP32-S3R8 with 8 MB QIO flash + 8 MB OPI PSRAM). The prebuilt release
**will not boot on other ESP32-S3 boards** — the bootloader's flash/PSRAM
config is wrong for them and will trigger a bootloop. (A sustained
bootloop can physically damage the chip's USB peripheral; ask me how I
know.)

If you have a different ESP32-S3 board (DevKitC-1, Feather S3,
LilyGo T-Display S3, etc.), don't use the prebuilt release. Clone the
repo, edit `firmware/platformio.ini` to match your board, and build
locally. The firmware halts at boot with a clear serial message if it
detects it's running on the wrong silicon, so you won't brick it by
accident — but you also won't get a working device until you build with
the right config.

## What you need before flashing

- A Seeed Xiao ESP32-S3 (~$8 — [Seeed direct][xiao] / Amazon / Mouser /
  DigiKey) + a **data-capable** USB-C cable (not charge-only).

[xiao]: https://www.seeedstudio.com/XIAO-ESP32S3-p-5627.html
- Your VictronConnect app — to grab the **bind key** and **BLE MAC** for
  each Victron device you want to monitor.
- A free [InfluxDB Cloud Serverless](https://cloud2.influxdata.com/signup)
  account (5 GB ingest/month, 30-day retention).
- A free [Grafana Cloud](https://grafana.com/auth/sign-up/create-user)
  account (publicly shareable dashboards).
- Either Python 3 + `esptool` (`pip install esptool`) **or** Chrome/Edge
  for web-flashing.

## 1. Flash the firmware

**Easiest (Chromebook, Mac, Windows — no installs):** open
[**the web flasher**](https://ehollins84-art.github.io/victron-broadcast/)
in Chrome or Edge, plug the ESP32-S3 in via USB-C, click
**Connect & install**. ~30 seconds.

If it can't connect, hold the **BOOT** button while pressing **RESET**,
then release RESET (release BOOT once the install starts).

**Or via esptool** (Linux/Mac/Windows):

```bash
pip install esptool
esptool.py --chip esp32s3 --port <YOUR_PORT> --baud 921600 \
    write_flash 0x0 victron-broadcast-merged.bin
```

Download `victron-broadcast-merged.bin` from the
[`latest` release](https://github.com/ehollins84-art/victron-broadcast/releases/tag/latest).

## 2. Configure on the device (no code, no edits)

1. On your phone, join the WiFi network **`victron-broadcast-setup`**
   (open, no password). Most phones will pop a captive-portal page
   automatically. If not, open `http://192.168.4.1` in a browser.
2. Fill in:
   - **WiFi** — the network the ESP32 should join afterwards.
   - **InfluxDB** — cluster URL, org, bucket (default `victron`), token
     (see step 3 below for where these come from).
   - **Victron devices** — name + MAC + bind key, one row per device.
     Tap **Scan nearby** and the page will list every Victron device in
     BLE range; tap a row to fill in the MAC. Paste the 32-hex bind key
     from VictronConnect.
3. Tap **Save & restart**. The chip leaves AP mode, joins your WiFi, and
   starts publishing.

Forgot something? Hold BOOT for ~3 s while running, or hold BOOT at
power-on, to re-enter the portal.

### Getting the bind key + MAC from VictronConnect

For each Victron device:

1. Open VictronConnect, tap your device.
2. Gear icon → **Product info**.
3. Toggle **Instant readout via Bluetooth** ON.
4. Tap **Show** under "Encryption data" → copy the 32-hex key.
5. The BLE MAC is on the same screen (`AA:BB:CC:DD:EE:FF`).

## 3. Set up InfluxDB Cloud (free)

1. Sign up at https://cloud2.influxdata.com/signup (Serverless plan).
2. Note the **Cluster URL** shown in the top-right account menu, e.g.
   `https://us-east-1-1.aws.cloud2.influxdata.com`.
3. **Load Data → Buckets → Create Bucket** → name it `victron`.
4. **Load Data → API Tokens → Generate API Token → Custom API Token** →
   give it **Write** access to the `victron` bucket. Copy it once.
5. Your **Org** is your signup email (or the org slug in Settings → About).

Put the URL, org, bucket, and token into the device portal.

## 4. Set up the public Grafana dashboard

1. Sign up at https://grafana.com/auth/sign-up/create-user.
2. In your Grafana stack, **Connections → Add new connection → InfluxDB**:
   - Query language: **Flux**
   - URL: your InfluxDB cluster URL
   - Auth: under "Custom HTTP Headers", add
     `Authorization` → `Token <YOUR_INFLUX_TOKEN>`
   - Default org / bucket: your values
   - Save & test → should say "datasource is working".
3. Import the dashboard from `dashboards/victron.json` in this repo
   (Dashboards → New → Import → Upload JSON file).
4. Dashboard settings → **Public dashboards** → enable → copy the URL.
   That URL is your public dashboard.

## Field reference (what shows up in InfluxDB)

Measurement: `victron`. Tags: `name` (your device label), `type`
(record type id). Fields written only when the device reports them:

| field      | unit | source                            |
|------------|------|-----------------------------------|
| `batt_v`   | V    | all                               |
| `batt_a`   | A    | SmartShunt, MPPT                  |
| `soc`      | %    | SmartShunt                        |
| `cons_ah`  | Ah   | SmartShunt (negative when drawn)  |
| `ttg_min`  | min  | SmartShunt                        |
| `aux_v`    | V    | SmartShunt (starter aux)          |
| `temp_c`   | °C   | SmartShunt (temp aux)             |
| `mid_v`    | V    | SmartShunt (midpoint aux)         |
| `pv_w`     | W    | MPPT                              |
| `yield_wh` | Wh   | MPPT (today's yield)              |
| `load_a`   | A    | MPPT (load output)                |
| `ac_va`    | VA   | inverter                          |
| `ac_v`     | V    | inverter                          |
| `ac_a`     | A    | inverter                          |
| `state`    | enum | MPPT, inverter, DC-DC             |
| `err`      | enum | MPPT, DC-DC                       |

## Building from source (optional)

```bash
cd firmware
pio run -e esp32-s3 -t upload
pio device monitor
```

## Layout

```
firmware/
  platformio.ini
  src/
    main.cpp                # boot flow, BLE callback, WiFi reconnect
    config_store.{h,cpp}    # NVS-backed config (JSON in Preferences)
    portal.{h,cpp}          # SoftAP + DNS + setup webpage
    victron_ble.{h,cpp}     # AES-CTR decrypt + bitfield parsers
    influx.{h,cpp}          # batched line-protocol writer
.github/workflows/build.yml # CI: builds firmware.bin, publishes release
```

## Troubleshooting

- **No `[victron]` lines** in serial output: bind key wrong, MAC wrong,
  or Instant Readout not enabled on the Victron device.
- **`HTTP -1` from InfluxDB**: WiFi DNS hiccup or TLS chain issue.
  Reboot and confirm `[wifi] connected` appears before the flush.
- **Can't see the setup AP**: hold BOOT for 3 s during normal run, or
  power-cycle with BOOT held — both wipe stored config and re-enter the
  portal.
- **BLE + WiFi instability**: keep `WiFi.setSleep(false)` (default in
  this firmware) — the S3 radio is shared and modem sleep stalls BLE.
