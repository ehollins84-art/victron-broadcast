// Captive-portal setup UI.
//
// Starts a SoftAP "victron-broadcast-setup" (open). Joining triggers the
// captive-portal popup on most phones because we run a DNS server that
// answers every query with our own IP. The page lets the user enter
// WiFi/Influx/Victron-device config, then writes it to NVS and reboots.

#include "portal.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <set>

namespace {

const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_MASK(255, 255, 255, 0);
constexpr const char* AP_SSID = "victron-broadcast-setup";

WebServer server(80);
DNSServer dns;

// Devices spotted via BLE during the most recent scan, keyed by MAC.
struct SpottedDevice { String mac; int rssi; uint16_t modelId; };
std::vector<SpottedDevice> spotted;

const char* INDEX_HTML = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>victron-broadcast setup</title>
<style>
  :root{--bg:#0b1020;--card:#141a30;--ink:#e8ecff;--muted:#9aa3c7;--accent:#5ac8fa;--ok:#34c759;--err:#ff453a;}
  *{box-sizing:border-box}
  body{margin:0;font:14px/1.4 -apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--ink);padding:16px;max-width:720px;margin:auto}
  h1{font-size:18px;margin:0 0 4px}
  p.lede{color:var(--muted);margin:0 0 16px}
  fieldset{border:1px solid #2a325a;border-radius:10px;background:var(--card);margin:0 0 14px;padding:12px}
  legend{padding:0 6px;color:var(--accent);font-weight:600}
  label{display:block;margin:8px 0 4px;color:var(--muted);font-size:12px}
  input{width:100%;padding:8px 10px;background:#0e1431;color:var(--ink);border:1px solid #2a325a;border-radius:8px;font:inherit}
  input:focus{outline:none;border-color:var(--accent)}
  .row{display:grid;grid-template-columns:1fr 1fr;gap:8px}
  .device{border:1px solid #2a325a;border-radius:8px;padding:8px;margin-bottom:8px;background:#0e1431}
  .device .top{display:flex;justify-content:space-between;align-items:center;margin-bottom:6px}
  button{background:var(--accent);color:#001a2b;border:0;border-radius:8px;padding:8px 12px;font-weight:600;cursor:pointer}
  button.ghost{background:transparent;color:var(--accent);border:1px solid var(--accent)}
  button.danger{background:transparent;color:var(--err);border:1px solid var(--err);padding:4px 8px;font-size:12px}
  .actions{display:flex;gap:8px;flex-wrap:wrap;margin-top:8px}
  .save{position:sticky;bottom:0;background:var(--bg);padding:12px 0;margin-top:16px;display:flex;gap:8px}
  .save button{flex:1;padding:12px;font-size:15px}
  .scan-results{margin-top:8px;display:flex;flex-direction:column;gap:6px}
  .scan-results .hit{display:flex;justify-content:space-between;align-items:center;padding:6px 8px;background:#0b1020;border-radius:6px;cursor:pointer;border:1px solid #2a325a}
  .scan-results .hit:hover{border-color:var(--accent)}
  .scan-results .mac{font-family:ui-monospace,monospace}
  small{color:var(--muted)}
  .status{padding:8px;border-radius:8px;margin-bottom:12px;display:none}
  .status.ok{display:block;background:rgba(52,199,89,.15);color:var(--ok);border:1px solid var(--ok)}
  .status.err{display:block;background:rgba(255,69,58,.15);color:var(--err);border:1px solid var(--err)}
</style>
</head>
<body>
<h1>victron-broadcast setup</h1>
<p class="lede">Fill in WiFi, your InfluxDB Cloud info, and one row per Victron device. Tap save when done.</p>
<div id="status" class="status"></div>

<form id="f">
<fieldset>
<legend>WiFi</legend>
<label>SSID</label><input name="wifiSsid" required>
<label>Password</label><input name="wifiPassword" type="password">
</fieldset>

<fieldset>
<legend>InfluxDB Cloud</legend>
<label>Cluster URL <small>(e.g. https://us-east-1-1.aws.cloud2.influxdata.com)</small></label>
<input name="influxUrl" placeholder="https://..." required>
<div class="row">
  <div><label>Org (email or org-id)</label><input name="influxOrg" required></div>
  <div><label>Bucket</label><input name="influxBucket" value="victron" required></div>
</div>
<label>API token <small>(write access to the bucket)</small></label>
<input name="influxToken" required>
</fieldset>

<fieldset>
<legend>Victron devices</legend>
<small>From VictronConnect: Settings → Product info → Encryption data (bind key, 32 hex chars) and the BLE MAC.</small>
<div id="devices"></div>
<div class="actions">
  <button type="button" class="ghost" onclick="addDevice()">+ Add device</button>
  <button type="button" class="ghost" onclick="scan()">Scan nearby</button>
</div>
<div class="scan-results" id="scan"></div>
</fieldset>

<div class="save">
  <button type="button" class="ghost" onclick="reset()">Wipe config</button>
  <button type="submit">Save &amp; restart</button>
</div>
</form>

<script>
let cfg = __CFG_JSON__;
const devsEl = document.getElementById('devices');
function devTpl(d, i){
  return `<div class="device" data-i="${i}">
    <div class="top"><strong>Device ${i+1}</strong>
      <button type="button" class="danger" onclick="rmDevice(${i})">Remove</button></div>
    <div class="row">
      <div><label>Name (tag)</label><input value="${d.name||''}" oninput="setF(${i},'name',this.value)" placeholder="shunt"></div>
      <div><label>MAC</label><input value="${d.mac||''}" oninput="setF(${i},'mac',this.value)" placeholder="aa:bb:cc:dd:ee:ff"></div>
    </div>
    <label>Bind key (32 hex chars)</label>
    <input value="${d.key||''}" oninput="setF(${i},'key',this.value)" placeholder="00112233445566778899aabbccddeeff">
  </div>`;
}
function render(){ devsEl.innerHTML = cfg.devices.map(devTpl).join(''); }
function setF(i,k,v){ cfg.devices[i][k]=v; }
function addDevice(){ cfg.devices.push({name:'',mac:'',key:''}); render(); }
function rmDevice(i){ cfg.devices.splice(i,1); render(); }
window.addDevice=addDevice; window.rmDevice=rmDevice;

['wifiSsid','wifiPassword','influxUrl','influxOrg','influxBucket','influxToken'].forEach(k=>{
  const el=document.querySelector(`[name=${k}]`); if (cfg[k]!=null) el.value=cfg[k];
  el.addEventListener('input',e=>cfg[k]=e.target.value);
});
if (!cfg.devices.length) addDevice(); else render();

async function scan(){
  const el=document.getElementById('scan');
  el.innerHTML='<small>Scanning for 8 seconds…</small>';
  try {
    const r = await fetch('/scan');
    const hits = await r.json();
    if (!hits.length){ el.innerHTML='<small>No Victron devices seen. Make sure Instant Readout is enabled in VictronConnect.</small>'; return; }
    el.innerHTML = hits.map(h=>`<div class="hit" onclick="useMac('${h.mac}')"><span class="mac">${h.mac}</span><small>rssi ${h.rssi} dBm · model 0x${h.modelId.toString(16)}</small></div>`).join('');
  } catch(e){ el.innerHTML='<small>Scan failed.</small>'; }
}
function useMac(m){
  let target = cfg.devices.findIndex(d=>!d.mac);
  if (target<0){ cfg.devices.push({name:'',mac:m,key:''}); render(); return; }
  cfg.devices[target].mac=m; render();
}
window.useMac=useMac; window.scan=scan;

async function reset(){
  if (!confirm('Wipe all saved config and reboot?')) return;
  await fetch('/reset',{method:'POST'});
  show('ok','Config wiped, rebooting…');
}
function show(kind,msg){ const s=document.getElementById('status'); s.className='status '+kind; s.textContent=msg; }

document.getElementById('f').addEventListener('submit', async e=>{
  e.preventDefault();
  for (const d of cfg.devices){
    if (!d.name || !d.mac || !d.key){ show('err','Each device needs name, MAC, and bind key.'); return; }
    if (!/^[0-9a-f]{32}$/i.test(d.key.replace(/\s/g,''))){ show('err','Bind key for "'+d.name+'" must be 32 hex chars.'); return; }
  }
  show('ok','Saving…');
  const r = await fetch('/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(cfg)});
  if (r.ok) show('ok','Saved. Rebooting — disconnect from this WiFi network.');
  else show('err','Save failed ('+r.status+').');
});
</script>
</body></html>)HTML";

void scanBleForVictron(uint16_t scanMs) {
    spotted.clear();
    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setActiveScan(false);
    NimBLEScanResults results = scan->start(scanMs / 1000, false);
    std::set<String> seen;
    for (int i = 0; i < results.getCount(); ++i) {
        NimBLEAdvertisedDevice d = results.getDevice(i);
        if (!d.haveManufacturerData()) continue;
        std::string md = d.getManufacturerData();
        if (md.size() < 3) continue;
        uint16_t mid = (uint8_t)md[0] | ((uint16_t)(uint8_t)md[1] << 8);
        if (mid != 0x02E1) continue;
        String mac = d.getAddress().toString().c_str();
        mac.toLowerCase();
        if (seen.count(mac)) continue;
        seen.insert(mac);
        uint16_t modelId = (uint8_t)md[3] | ((uint16_t)(uint8_t)md[4] << 8);
        spotted.push_back({mac, d.getRSSI(), modelId});
    }
    scan->clearResults();
}

void handleRoot(const AppConfig& cfg) {
    JsonDocument doc;
    doc["wifiSsid"]     = cfg.wifiSsid;
    doc["wifiPassword"] = cfg.wifiPassword;
    doc["influxUrl"]    = cfg.influxUrl;
    doc["influxOrg"]    = cfg.influxOrg;
    doc["influxBucket"] = cfg.influxBucket.length() ? cfg.influxBucket : String("victron");
    doc["influxToken"]  = cfg.influxToken;
    JsonArray arr = doc["devices"].to<JsonArray>();
    for (const auto& d : cfg.devices) {
        JsonObject o = arr.add<JsonObject>();
        o["name"] = d.name; o["mac"] = d.mac; o["key"] = d.key;
    }
    String json; serializeJson(doc, json);
    String page = INDEX_HTML;
    page.replace("__CFG_JSON__", json);
    server.send(200, "text/html; charset=utf-8", page);
}

void handleScan() {
    scanBleForVictron(8000);
    JsonDocument out;
    JsonArray arr = out.to<JsonArray>();
    for (const auto& s : spotted) {
        JsonObject o = arr.add<JsonObject>();
        o["mac"] = s.mac; o["rssi"] = s.rssi; o["modelId"] = s.modelId;
    }
    String body; serializeJson(out, body);
    server.send(200, "application/json", body);
}

bool handleSave() {
    String body = server.arg("plain");
    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        server.send(400, "text/plain", "bad json");
        return false;
    }
    AppConfig cfg;
    cfg.wifiSsid     = (const char*)(doc["wifiSsid"]     | "");
    cfg.wifiPassword = (const char*)(doc["wifiPassword"] | "");
    cfg.influxUrl    = (const char*)(doc["influxUrl"]    | "");
    cfg.influxOrg    = (const char*)(doc["influxOrg"]    | "");
    cfg.influxBucket = (const char*)(doc["influxBucket"] | "victron");
    cfg.influxToken  = (const char*)(doc["influxToken"]  | "");
    for (JsonObject d : doc["devices"].as<JsonArray>()) {
        DeviceCfg dc;
        dc.name = (const char*)(d["name"] | "");
        dc.mac  = (const char*)(d["mac"]  | "");
        dc.key  = (const char*)(d["key"]  | "");
        dc.mac.toLowerCase(); dc.key.toLowerCase();
        if (dc.name.length() && dc.mac.length() && dc.key.length() == 32) {
            cfg.devices.push_back(dc);
        }
    }
    if (!ConfigStore::save(cfg)) {
        server.send(500, "text/plain", "save failed");
        return false;
    }
    server.send(200, "text/plain", "ok");
    return true;
}

} // namespace

[[noreturn]] void runSetupPortal(AppConfig& cfg) {
    Serial.println("[portal] starting AP");
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(AP_IP, AP_IP, AP_MASK);
    WiFi.softAP(AP_SSID);
    dns.start(53, "*", AP_IP);

    // NimBLE may not yet be inited if we entered portal before scan started.
    if (!NimBLEDevice::getInitialized()) NimBLEDevice::init("victron-broadcast");

    server.on("/", HTTP_GET, [&cfg]() { handleRoot(cfg); });
    server.on("/scan", HTTP_GET, handleScan);
    server.on("/save", HTTP_POST, []() {
        if (handleSave()) {
            delay(500);
            ESP.restart();
        }
    });
    server.on("/reset", HTTP_POST, []() {
        ConfigStore::clear();
        server.send(200, "text/plain", "wiped");
        delay(500);
        ESP.restart();
    });
    // Captive-portal catchalls
    server.onNotFound([&cfg]() { handleRoot(cfg); });
    server.begin();

    Serial.printf("[portal] join WiFi \"%s\", open http://%s\n", AP_SSID, AP_IP.toString().c_str());

    for (;;) {
        dns.processNextRequest();
        server.handleClient();
        delay(2);
    }
}
