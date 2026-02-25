/*
  ESP32 TMP Setup + RS485 Sender/Receiver (Ping responder)
  - DS18B20 on GPIO5
  - RS485 on Serial2: RX=22, TX=23 (TTL-RS485 adapter, RX/TX crossed)
  - AP Web UI: ESP32-TMP-SETUP
  - Persistent config via Preferences:
      * dev_id (1..9999)
      * tx_ms (250..3600000)
      * ping_tx (string template, e.g. "DeviceID.OUT.210.rSTATUS")
      * ping_rx (string template, e.g. "DeviceID.OUT.210.gSTATUS")
      * ROM -> instance mapping stored using short hash keys (<= 15 chars)
  - Telegram format:
      0xFD <PAYLOAD> 0xFE
    Temperature payload:
      <DEVICE>.TMP.<INST>.STATUS.P1.P2
    Ping request/response payload (configurable):
      <DEVICE>.OUT.210.gSTATUS  -> respond with <DEVICE>.OUT.210.rSTATUS
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

#include <OneWire.h>
#include <DallasTemperature.h>

// ---------------- Pins ----------------
static const int PIN_ONEWIRE = 5;
static const int RS485_RX = 22;
static const int RS485_TX = 23;

// ---------------- AP Config ----------------
static const char* AP_SSID = "ESP32-TMP-SETUP";
static const char* AP_PASS = ""; // open AP; set a password if you want (>=8 chars)

// ---------------- Preferences ----------------
Preferences prefs;
static const char* PREF_NS    = "tmpcfg";   // namespace
static const char* KEY_DEV_ID = "dev_id";   // <= 15 chars
static const char* KEY_TX_MS  = "tx_ms";    // <= 15 chars
static const char* KEY_PINGTX = "ping_tx";  // <= 15 chars
static const char* KEY_PINGRX = "ping_rx";  // <= 15 chars

static const uint32_t DEFAULT_DEVICE_ID = 1;
static const uint32_t DEFAULT_TX_MS     = 5000;

static const char* DEFAULT_PING_TX = "DeviceID.OUT.210.rSTATUS";
static const char* DEFAULT_PING_RX = "DeviceID.OUT.210.gSTATUS";

// ---------------- DS18B20 ----------------
OneWire oneWire(PIN_ONEWIRE);
DallasTemperature sensors(&oneWire);

// ---------------- Web ----------------
WebServer server(80);

// ---------------- Runtime State ----------------
static uint32_t g_deviceId = DEFAULT_DEVICE_ID;
static uint32_t g_txMs     = DEFAULT_TX_MS;
static uint32_t g_lastSend = 0;

static String g_pingTx = DEFAULT_PING_TX;
static String g_pingRx = DEFAULT_PING_RX;

// ---------------- RS485 RX/TX arbitration (soft bus-free) ----------------
// We don't have DE/RE control, so we reduce collisions by:
// - parsing incoming frames
// - tracking last RX byte time
// - only transmitting when bus has been idle for a guard time
// - random backoff before transmit
static volatile uint32_t g_lastRxByteMicros = 0;

// If we just answered a ping, delay temperature telegrams by >= 1s
static volatile uint32_t g_tempHoldoffUntilMs = 0;

// RX frame parser state
static bool   g_inFrame = false;
static String g_rxBuf;

// ---------------- Timing knobs ----------------
static const uint32_t RS485_BAUD = 56700;

// Approx char time (10 bits per byte: start + 8 data + stop)
static inline uint32_t charTimeMicros() {
  // 10 bits / baud seconds -> micros
  // 10e6 / baud
  return (uint32_t)(10000000UL / RS485_BAUD);
}

// Guard time: require bus idle for this many char times
static const uint32_t BUS_IDLE_CHARS = 6; // conservative
static inline uint32_t busIdleMicros() {
  return BUS_IDLE_CHARS * charTimeMicros();
}

// Backoff range (ms) when bus was busy
static const uint32_t BACKOFF_MIN_MS = 2;
static const uint32_t BACKOFF_MAX_MS = 20;

// Max time we are willing to wait for bus idle before giving up (ms)
static const uint32_t BUS_WAIT_MAX_MS = 250;

// ---------- Helpers: ROM <-> hex ----------
String romToHexNoSep(const DeviceAddress rom) {
  char buf[17];
  for (int i = 0; i < 8; i++) {
    sprintf(buf + (i * 2), "%02X", rom[i]);
  }
  buf[16] = '\0';
  return String(buf);
}

// FNV-1a 32-bit hash -> 8 hex chars. Key example: "mA1B2C3D4" (9 chars)
uint32_t fnv1a32(const String& s) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < s.length(); i++) {
    h ^= (uint8_t)s[i];
    h *= 16777619u;
  }
  return h;
}

String mappingKeyForRom(const String& romHex) {
  uint32_t h = fnv1a32(romHex);
  char buf[10];
  // 'm' + 8 hex = 9 chars total (safe for Preferences key limit)
  sprintf(buf, "m%08X", (unsigned)h);
  return String(buf);
}

// ---------- Preferences get/set ----------
uint32_t loadDeviceId() {
  prefs.begin(PREF_NS, true);
  uint32_t v = prefs.getUInt(KEY_DEV_ID, DEFAULT_DEVICE_ID);
  prefs.end();
  if (v < 1) v = 1;
  if (v > 9999) v = 9999;
  return v;
}

uint32_t loadTxMs() {
  prefs.begin(PREF_NS, true);
  uint32_t v = prefs.getUInt(KEY_TX_MS, DEFAULT_TX_MS);
  prefs.end();
  if (v < 250) v = 250;
  if (v > 3600000) v = 3600000;
  return v;
}

void saveDeviceId(uint32_t v) {
  if (v < 1) v = 1;
  if (v > 9999) v = 9999;
  prefs.begin(PREF_NS, false);
  prefs.putUInt(KEY_DEV_ID, v);
  prefs.end();
  g_deviceId = v;
}

void saveTxMs(uint32_t v) {
  if (v < 250) v = 250;
  if (v > 3600000) v = 3600000;
  prefs.begin(PREF_NS, false);
  prefs.putUInt(KEY_TX_MS, v);
  prefs.end();
  g_txMs = v;
}

String loadPingTx() {
  prefs.begin(PREF_NS, true);
  String v = prefs.getString(KEY_PINGTX, DEFAULT_PING_TX);
  prefs.end();
  if (v.length() == 0) v = DEFAULT_PING_TX;
  return v;
}

String loadPingRx() {
  prefs.begin(PREF_NS, true);
  String v = prefs.getString(KEY_PINGRX, DEFAULT_PING_RX);
  prefs.end();
  if (v.length() == 0) v = DEFAULT_PING_RX;
  return v;
}

void savePingTx(const String& v) {
  prefs.begin(PREF_NS, false);
  prefs.putString(KEY_PINGTX, v);
  prefs.end();
  g_pingTx = v;
}

void savePingRx(const String& v) {
  prefs.begin(PREF_NS, false);
  prefs.putString(KEY_PINGRX, v);
  prefs.end();
  g_pingRx = v;
}

uint32_t getMappedInstanceForRom(const String& romHex, uint32_t defaultInst = 1) {
  String key = mappingKeyForRom(romHex);
  prefs.begin(PREF_NS, true);
  uint32_t inst = prefs.getUInt(key.c_str(), defaultInst);
  prefs.end();
  if (inst < 1) inst = 1;
  if (inst > 255) inst = 255;
  return inst;
}

void setMappedInstanceForRom(const String& romHex, uint32_t inst) {
  if (inst < 1) inst = 1;
  if (inst > 255) inst = 255;
  String key = mappingKeyForRom(romHex);
  prefs.begin(PREF_NS, false);
  prefs.putUInt(key.c_str(), inst);
  prefs.end();
}

// ---------- DeviceID token replacement ----------
String applyDeviceIdToken(const String& templ) {
  String out = templ;
  out.replace("DeviceID", String(g_deviceId));
  return out;
}

// ---------- Soft bus-free wait ----------
bool waitForBusIdle(uint32_t maxWaitMs) {
  uint32_t start = millis();
  while ((millis() - start) < maxWaitMs) {
    // Keep parsing RX while waiting (important!)
    // (rs485Poll() will update g_lastRxByteMicros)
    // We'll call rs485Poll() from loop frequently; but also here:
    // to avoid dead time.
    // NOTE: rs485Poll() is defined later.
    extern void rs485Poll();
    rs485Poll();

    uint32_t nowUs = micros();
    uint32_t lastUs = g_lastRxByteMicros;
    if ((uint32_t)(nowUs - lastUs) >= busIdleMicros()) {
      return true;
    }
    delay(1);
  }
  return false;
}

// ---------- Telegram send (temperature) ----------
void sendTelegram(uint32_t deviceId, uint32_t instance, int16_t p1, int16_t p2) {
  String payload =
    String(deviceId) + ".TMP." + String(instance) + ".STATUS." +
    String(p1) + "." + String(p2);

  Serial2.write(0xFD);
  Serial2.print(payload);
  Serial2.write(0xFE);

  Serial.print("[TX] 0xFD ");
  Serial.print(payload);
  Serial.println(" 0xFE");
}

// ---------- Telegram send (raw payload) ----------
bool sendRawTelegramSoftBusFree(const String& payload, uint32_t maxWaitMs) {
  // Wait for idle
  if (!waitForBusIdle(maxWaitMs)) return false;

  // Random backoff to reduce simultaneous talkers
  uint32_t backoff = random(BACKOFF_MIN_MS, BACKOFF_MAX_MS + 1);
  uint32_t t0 = millis();
  while (millis() - t0 < backoff) {
    extern void rs485Poll();
    rs485Poll();
    // If bus became busy again, restart the whole attempt
    uint32_t nowUs = micros();
    if ((uint32_t)(nowUs - g_lastRxByteMicros) < busIdleMicros()) {
      // busy again
      return false;
    }
    delay(1);
  }

  // Final idle check
  if ((uint32_t)(micros() - g_lastRxByteMicros) < busIdleMicros()) return false;

  // Send
  Serial2.write(0xFD);
  Serial2.print(payload);
  Serial2.write(0xFE);
  Serial2.flush(); // wait until bytes are pushed out

  Serial.print("[TX] 0xFD ");
  Serial.print(payload);
  Serial.println(" 0xFE");

  return true;
}

// ---------- RS485 RX handling ----------
void handleRxPayload(const String& payload) {
  Serial.print("[RX] ");
  Serial.println(payload);

  // Only react to our own DeviceID (because token is replaced)
  String expectedReq = applyDeviceIdToken(g_pingRx);
  if (payload == expectedReq) {
    String reply = applyDeviceIdToken(g_pingTx);

    // Try to respond quickly; if bus busy, we'll retry a bit
    uint32_t start = millis();
    bool sent = false;
    while ((millis() - start) < 200 && !sent) {
      sent = sendRawTelegramSoftBusFree(reply, BUS_WAIT_MAX_MS);
      if (!sent) delay(5);
    }

    // Hold off temperature telegrams for at least 1s after ping activity
    g_tempHoldoffUntilMs = millis() + 1000;

    if (!sent) {
      Serial.println("[PING] reply not sent (bus busy)");
    } else {
      Serial.println("[PING] reply sent");
    }
  }
}

void rs485Poll() {
  while (Serial2.available() > 0) {
    int b = Serial2.read();
    if (b < 0) break;

    g_lastRxByteMicros = micros();

    if (!g_inFrame) {
      if (b == 0xFD) {
        g_inFrame = true;
        g_rxBuf = "";
      }
      continue;
    }

    // in frame
    if (b == 0xFE) {
      g_inFrame = false;
      handleRxPayload(g_rxBuf);
      g_rxBuf = "";
      continue;
    }

    // guard against runaway frames
    if (g_rxBuf.length() < 220) {
      g_rxBuf += (char)b;
    } else {
      g_inFrame = false;
      g_rxBuf = "";
    }
  }
}

// ---------- Web UI HTML ----------
String htmlPage() {
  String s;
  s.reserve(7500);
  s += F("<!doctype html><html><head><meta charset='utf-8'>"
         "<meta name='viewport' content='width=device-width, initial-scale=1'>"
         "<title>ESP32 TMP Setup</title>"
         "<style>body{font-family:Arial;margin:16px} .card{border:1px solid #ddd;padding:12px;margin:12px 0;border-radius:8px}"
         "input,select,button{font-size:16px;padding:8px;margin:6px 0;width:100%} table{width:100%;border-collapse:collapse} "
         "td,th{border-bottom:1px solid #eee;padding:8px;text-align:left} .row{display:flex;gap:12px;flex-wrap:wrap} .col{flex:1;min-width:240px}"
         "code{background:#f6f6f6;padding:2px 4px;border-radius:4px}"
         "</style></head><body>");

  s += F("<h2>ESP32 TMP Setup</h2>");

  s += F("<div class='card'><h3>Bus Settings</h3>"
         "<div class='row'>"
         "<div class='col'><label>Device ID (1..9999)</label><input id='deviceId' type='number' min='1' max='9999'></div>"
         "<div class='col'><label>Send interval (ms)</label><input id='txMs' type='number' min='250' max='3600000' step='250'></div>"
         "</div>"
         "<div class='row'>"
         "<div class='col'><label>Ping response template</label>"
         "<input id='pingTx' type='text' placeholder='DeviceID.OUT.210.rSTATUS'></div>"
         "<div class='col'><label>Ping request template</label>"
         "<input id='pingRx' type='text' placeholder='DeviceID.OUT.210.gSTATUS'></div>"
         "</div>"
         "<button onclick='saveBus()'>Save bus settings</button>"
         "<div id='busMsg'></div>"
         "<p style='font-size:13px;color:#555'>Hinweis: <code>DeviceID</code> wird automatisch durch die konfigurierte Device ID ersetzt.</p>"
         "</div>");

  s += F("<div class='card'><h3>Sensors</h3>"
         "<button onclick='loadSensors()'>Refresh sensor list</button>"
         "<div id='sens'></div></div>");

  s += F("</body></html>");

  // Script appended separately to keep reserve predictable
  s += F("<script>"
         "async function loadBus(){"
         " const r=await fetch('/api/buscfg'); const j=await r.json();"
         " document.getElementById('deviceId').value=j.deviceId;"
         " document.getElementById('txMs').value=j.txIntervalMs;"
         " document.getElementById('pingTx').value=j.pingTx||'';"
         " document.getElementById('pingRx').value=j.pingRx||'';"
         "}"
         "async function saveBus(){"
         " const payload={"
         "  deviceId:parseInt(deviceId.value,10),"
         "  txIntervalMs:parseInt(txMs.value,10),"
         "  pingTx:pingTx.value,"
         "  pingRx:pingRx.value"
         " };"
         " const r=await fetch('/api/buscfg',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});"
         " const j=await r.json();"
         " document.getElementById('busMsg').innerText = j.ok ? 'Saved.' : ('Error: '+(j.error||'unknown'));"
         " if(j.ok) setTimeout(()=>document.getElementById('busMsg').innerText='',2000);"
         "}"
         "function esc(s){return (''+s).replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('>','&gt;')}"
         "async function loadSensors(){"
         " const r=await fetch('/api/sensors'); const j=await r.json();"
         " let h='<table><tr><th>ROM</th><th>Temp (°C)</th><th>Instance</th><th></th></tr>';"
         " for(const it of j.sensors){"
         "  h+='<tr>';"
         "  h+='<td><code>'+esc(it.rom)+'</code></td>';"
         "  h+='<td>'+esc(it.tempC)+'</td>';"
         "  h+='<td><input type=number min=1 max=255 value='+esc(it.instance)+' id=\"i_'+it.rom+'\"></td>';"
         "  h+='<td><button onclick=\"saveMap(\\''+it.rom+'\\')\">Save</button></td>';"
         "  h+='</tr>';"
         " }"
         " h+='</table>';"
         " document.getElementById('sens').innerHTML=h;"
         "}"
         "async function saveMap(rom){"
         " const inst=parseInt(document.getElementById('i_'+rom).value,10);"
         " const payload={rom:rom,instance:inst};"
         " const r=await fetch('/api/map',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});"
         " const j=await r.json();"
         " if(!j.ok) alert('Error: '+(j.error||'unknown'));"
         "}"
         "window.addEventListener('load',()=>{loadBus();loadSensors();});"
         "</script>");

  return s;
}

// ---------- Minimal JSON parsing (no ArduinoJson dependency) ----------
bool jsonGetUInt(const String& body, const String& key, uint32_t& out) {
  // expects: "key":123
  int k = body.indexOf("\"" + key + "\"");
  if (k < 0) return false;
  int c = body.indexOf(":", k);
  if (c < 0) return false;
  int i = c + 1;
  while (i < (int)body.length() && (body[i] == ' ' || body[i] == '\n' || body[i] == '\r' || body[i] == '\t')) i++;
  int j = i;
  while (j < (int)body.length() && isDigit(body[j])) j++;
  if (j == i) return false;
  out = (uint32_t) body.substring(i, j).toInt();
  return true;
}

bool jsonGetString(const String& body, const String& key, String& out) {
  // expects: "key":"VALUE"
  int k = body.indexOf("\"" + key + "\"");
  if (k < 0) return false;
  int c = body.indexOf(":", k);
  if (c < 0) return false;
  int q1 = body.indexOf("\"", c + 1);
  if (q1 < 0) return false;
  int q2 = body.indexOf("\"", q1 + 1);
  if (q2 < 0) return false;
  out = body.substring(q1 + 1, q2);
  return true;
}

// ---------- Routes ----------
void setupWeb() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html", htmlPage());
  });

  server.on("/api/buscfg", HTTP_GET, []() {
    String out = "{";
    out += "\"deviceId\":" + String(g_deviceId) + ",";
    out += "\"txIntervalMs\":" + String(g_txMs) + ",";
    out += "\"pingTx\":\"" + g_pingTx + "\",";
    out += "\"pingRx\":\"" + g_pingRx + "\"";
    out += "}";
    server.send(200, "application/json", out);
  });

  server.on("/api/buscfg", HTTP_POST, []() {
    if (!server.hasArg("plain")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing body\"}");
      return;
    }
    String body = server.arg("plain");

    // Allow partial updates, but keep validation
    uint32_t devId, txMs;
    String pingTx, pingRx;

    bool hasDev = jsonGetUInt(body, "deviceId", devId);
    bool hasTx  = jsonGetUInt(body, "txIntervalMs", txMs);
    bool hasPTx = jsonGetString(body, "pingTx", pingTx);
    bool hasPRx = jsonGetString(body, "pingRx", pingRx);

    if (!hasDev && !hasTx && !hasPTx && !hasPRx) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
      return;
    }

    if (hasDev) {
      if (devId < 1 || devId > 9999) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"deviceId range\"}");
        return;
      }
      saveDeviceId(devId);
    }
    if (hasTx) {
      saveTxMs(txMs);
    }
    if (hasPTx) {
      if (pingTx.length() == 0) pingTx = DEFAULT_PING_TX;
      savePingTx(pingTx);
    }
    if (hasPRx) {
      if (pingRx.length() == 0) pingRx = DEFAULT_PING_RX;
      savePingRx(pingRx);
    }

    Serial.printf("[CFG] deviceId=%u, txMs=%u, pingRx=%s, pingTx=%s\n",
                  (unsigned)g_deviceId, (unsigned)g_txMs,
                  g_pingRx.c_str(), g_pingTx.c_str());

    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/sensors", HTTP_GET, []() {
    sensors.requestTemperatures();

    int count = sensors.getDeviceCount();
    String out = "{\"sensors\":[";
    bool first = true;

    for (int i = 0; i < count; i++) {
      DeviceAddress addr;
      if (!sensors.getAddress(addr, i)) continue;

      String romHex = romToHexNoSep(addr);
      float t = sensors.getTempC(addr);

      String tStr;
      if (t < -100 || t > 150) tStr = "NaN";
      else tStr = String(t, 2);

      uint32_t inst = getMappedInstanceForRom(romHex, 1);

      if (!first) out += ",";
      first = false;

      out += "{";
      out += "\"rom\":\"" + romHex + "\",";
      out += "\"tempC\":\"" + tStr + "\",";
      out += "\"instance\":" + String(inst);
      out += "}";
    }

    out += "]}";
    server.send(200, "application/json", out);
  });

  server.on("/api/map", HTTP_POST, []() {
    if (!server.hasArg("plain")) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing body\"}");
      return;
    }
    String body = server.arg("plain");

    String rom;
    uint32_t inst;
    if (!jsonGetString(body, "rom", rom) || !jsonGetUInt(body, "instance", inst)) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}");
      return;
    }
    if (rom.length() != 16) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"rom length\"}");
      return;
    }
    if (inst < 1 || inst > 255) {
      server.send(400, "application/json", "{\"ok\":false,\"error\":\"instance range\"}");
      return;
    }

    setMappedInstanceForRom(rom, inst);
    Serial.printf("[MAP] %s -> instance %u (key=%s)\n",
                  rom.c_str(), (unsigned)inst, mappingKeyForRom(rom).c_str());

    server.send(200, "application/json", "{\"ok\":true}");
  });

  server.begin();
  Serial.println("[WEB] server started");
}

// ---------- WiFi AP ----------
void setupWiFiAP() {
  WiFi.mode(WIFI_AP);
  if (strlen(AP_PASS) >= 8) WiFi.softAP(AP_SSID, AP_PASS);
  else WiFi.softAP(AP_SSID);

  IPAddress ip = WiFi.softAPIP();
  Serial.print("[WIFI] AP IP: ");
  Serial.println(ip);
}

// ---------- Setup/Loop ----------
void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println("\n--- ESP32 TMP SETUP ---");

  // Load persistent bus settings
  g_deviceId = loadDeviceId();
  g_txMs     = loadTxMs();
  g_pingTx   = loadPingTx();
  g_pingRx   = loadPingRx();

  Serial.printf("[CFG] deviceId=%u, txMs=%u\n", (unsigned)g_deviceId, (unsigned)g_txMs);
  Serial.printf("[CFG] pingRx=%s\n", g_pingRx.c_str());
  Serial.printf("[CFG] pingTx=%s\n", g_pingTx.c_str());

  // RS485 UART
  Serial2.begin(RS485_BAUD, SERIAL_8N1, RS485_RX, RS485_TX);
  Serial.println("[RS485] Serial2 started");

  // Seed random for backoff
  randomSeed((uint32_t)esp_random());

  // DS18B20
  sensors.begin();
  Serial.printf("[DS18B20] found devices: %d\n", sensors.getDeviceCount());

  // WiFi + Web
  setupWiFiAP();
  setupWeb();
}

void loop() {
  server.handleClient();

  // Always poll RS485 RX
  rs485Poll();

  uint32_t now = millis();

  // Temperature send scheduling:
  // - normal interval g_txMs
  // - but if ping happened recently, hold off at least 1s
  if (now < g_tempHoldoffUntilMs) {
    return;
  }

  if (now - g_lastSend >= g_txMs) {
    // Before sending temps, if bus is busy, postpone by 1s (your requirement)
    // This avoids fighting with other talkers and gives ping priority.
    if ((uint32_t)(micros() - g_lastRxByteMicros) < busIdleMicros()) {
      g_tempHoldoffUntilMs = now + 1000;
      return;
    }

    // Also require bus idle (soft) before sending temps; if not possible quickly, postpone 1s
    if (!waitForBusIdle(100)) {
      g_tempHoldoffUntilMs = now + 1000;
      return;
    }

    g_lastSend = now;

    // Read all sensors & send
    sensors.requestTemperatures();
    int count = sensors.getDeviceCount();

    for (int i = 0; i < count; i++) {
      DeviceAddress addr;
      if (!sensors.getAddress(addr, i)) continue;

      String romHex = romToHexNoSep(addr);
      uint32_t inst = getMappedInstanceForRom(romHex, 1);
      float t = sensors.getTempC(addr);

      int16_t p1 = 0;
      int16_t p2 = 0;

      if (t < -100 || t > 150) {
        p1 = -127;
        p2 = 0;
      } else {
        int32_t scaled = (int32_t) lroundf(t * 100.0f);
        p1 = (int16_t)(scaled / 100);
        p2 = (int16_t)(abs(scaled % 100));
      }

      Serial.printf("[SENS] ROM %s -> instance %u | T=%.2f | key=%s\n",
                    romHex.c_str(), (unsigned)inst, (double)t, mappingKeyForRom(romHex).c_str());

      // Temperature telegram uses existing format
      sendTelegram(g_deviceId, inst, p1, p2);

      // While sending multiple sensors, keep RX serviced a bit
      rs485Poll();
      delay(2);
    }
  }
}
