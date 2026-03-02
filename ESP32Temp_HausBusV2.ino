/*
  ESP32Temp_HausBusV2 — ESP32 + DS18B20 + RS485 HausBus + Web-UI (AP)
  ------------------------------------------------------------------
  Telegram framing: 0xFD <PAYLOAD> 0xFE

  Temperature auto-send (non-blocking scheduler):
    <DeviceID>.TMP.<inst>.STATUS.P1.P2

  Temperature on-demand:
    RX: <DeviceID>.TMP.<inst>.gSTATUS
    TX: <DeviceID>.TMP.<inst>.STATUS.P1.P2

  Ping responder (templates, DeviceID token replacement):
    RX template default: DeviceID.OUT.210.gSTATUS
    TX template default: DeviceID.OUT.210.rSTATUS

  SYS config set (NO auto confirmation):
    RX: <DeviceID>.SYS.1.SET_CFG.<P1>.<P2>
      P1=1 => WiFi/Web enable, P2=1 ON, P2=0 OFF

  SYS config query:
    RX: <DeviceID>.SYS.1.gCONFIG.<P1>
    TX: <DeviceID>.SYS.1.rCONFIG.<P1>.<VALUE>
      For P1=1 => VALUE is ACTUAL runtime state (1 if AP+Web running, else 0)

  Web-UI:
    AP SSID: ESP32-TMP-SETUP (open by default)
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
static const char* AP_PASS = ""; // open AP; set password (>=8 chars) if you want

// ---------------- Preferences ----------------
Preferences prefs;
static const char* PREF_NS     = "tmpcfg";
static const char* KEY_DEV_ID  = "dev_id";
static const char* KEY_TX_MS   = "tx_ms";
static const char* KEY_AUTO_EN = "auto_en";
static const char* KEY_GAP_MS  = "gap_ms";
static const char* KEY_WIFI_EN = "wifi_en";
static const char* KEY_PINGTX  = "ping_tx";
static const char* KEY_PINGRX  = "ping_rx";

static const uint32_t DEFAULT_DEVICE_ID = 1;
static const uint32_t DEFAULT_TX_MS     = 5000;
static const uint32_t DEFAULT_GAP_MS    = 100;
static const bool     DEFAULT_AUTO_EN   = true;
static const bool     DEFAULT_WIFI_EN   = true;

static const char* DEFAULT_PING_TX = "DeviceID.OUT.210.rSTATUS";
static const char* DEFAULT_PING_RX = "DeviceID.OUT.210.gSTATUS";

// ---------------- DS18B20 ----------------
OneWire oneWire(PIN_ONEWIRE);
DallasTemperature sensors(&oneWire);

// ---------------- Web ----------------
WebServer server(80);
static bool g_webStarted = false;

// ---------------- Runtime State ----------------
static uint32_t g_deviceId = DEFAULT_DEVICE_ID;
static uint32_t g_txMs     = DEFAULT_TX_MS;     // 0 => disabled
static uint32_t g_gapMs    = DEFAULT_GAP_MS;
static bool     g_autoEn   = DEFAULT_AUTO_EN;
static uint32_t g_lastCycleStartMs = 0;

static String g_pingTx = DEFAULT_PING_TX;
static String g_pingRx = DEFAULT_PING_RX;

// ---------------- WiFi/Web desired vs actual ----------------
static bool g_wifiDesired = DEFAULT_WIFI_EN;
static uint32_t g_wifiNextActionMs = 0;
static const uint32_t WIFI_TRANSITION_COOLDOWN_MS = 600;

// ---------------- RS485 RX/TX arbitration ----------------
static volatile uint32_t g_lastRxByteMicros = 0;
static volatile uint32_t g_tempHoldoffUntilMs = 0;

static bool   g_inFrame = false;
static String g_rxBuf;

// ---------------- Timing ----------------
static const uint32_t RS485_BAUD = 57600;

static inline uint32_t charTimeMicros() { return (uint32_t)(10000000UL / RS485_BAUD); }
static const uint32_t BUS_IDLE_CHARS = 6;
static inline uint32_t busIdleMicros() { return BUS_IDLE_CHARS * charTimeMicros(); }

static const uint32_t BACKOFF_MIN_MS = 2;
static const uint32_t BACKOFF_MAX_MS = 20;
static const uint32_t BUS_WAIT_MAX_MS = 250;

// ---------------- Non-blocking temperature TX scheduler ----------------
enum TxState : uint8_t { TX_IDLE = 0, TX_PREPARE_CYCLE, TX_SEND_ONE, TX_WAIT_GAP };
static TxState g_txState = TX_IDLE;

static const uint8_t MAX_SENSORS = 16;
static DeviceAddress g_cycleAddrs[MAX_SENSORS];
static float         g_cycleTemps[MAX_SENSORS];
static uint8_t       g_cycleCount = 0;
static uint8_t       g_cycleIndex = 0;
static uint32_t      g_nextActionMs = 0;

// ---------- Forward decl ----------
void rs485Poll();
void setupWeb();
String htmlPage();
void wifiRequest(bool en);
void wifiServiceTick();
void handleRxPayload(const String& payload);
void prepareTempCycleSnapshot();
void tempTxSchedulerTick();

// ---------- Helpers: ROM <-> hex ----------
String romToHexNoSep(const DeviceAddress rom) {
  char buf[17];
  for (int i = 0; i < 8; i++) sprintf(buf + (i * 2), "%02X", rom[i]);
  buf[16] = '\0';
  return String(buf);
}

uint32_t fnv1a32(const String& s) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < s.length(); i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
  return h;
}

String mappingKeyForRom(const String& romHex) {
  uint32_t h = fnv1a32(romHex);
  char buf[10];
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
  if (v > 3600000) v = 3600000;
  return v;
}

bool loadAutoEn() {
  prefs.begin(PREF_NS, true);
  uint32_t v = prefs.getUInt(KEY_AUTO_EN, DEFAULT_AUTO_EN ? 1 : 0);
  prefs.end();
  return v != 0;
}

uint32_t loadGapMs() {
  prefs.begin(PREF_NS, true);
  uint32_t v = prefs.getUInt(KEY_GAP_MS, DEFAULT_GAP_MS);
  prefs.end();
  if (v > 2000) v = 2000;
  return v;
}

bool loadWifiEn() {
  prefs.begin(PREF_NS, true);
  uint32_t v = prefs.getUInt(KEY_WIFI_EN, DEFAULT_WIFI_EN ? 1 : 0);
  prefs.end();
  return v != 0;
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
  if (v > 3600000) v = 3600000;
  prefs.begin(PREF_NS, false);
  prefs.putUInt(KEY_TX_MS, v);
  prefs.end();
  g_txMs = v;
}

void saveAutoEn(bool en) {
  prefs.begin(PREF_NS, false);
  prefs.putUInt(KEY_AUTO_EN, en ? 1 : 0);
  prefs.end();
  g_autoEn = en;
}

void saveGapMs(uint32_t v) {
  if (v > 2000) v = 2000;
  prefs.begin(PREF_NS, false);
  prefs.putUInt(KEY_GAP_MS, v);
  prefs.end();
  g_gapMs = v;
}

void saveWifiEn(bool en) {
  prefs.begin(PREF_NS, false);
  prefs.putUInt(KEY_WIFI_EN, en ? 1 : 0);
  prefs.end();
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

// ---------- Bus idle check ----------
static inline bool isBusIdleNow() {
  return (uint32_t)(micros() - g_lastRxByteMicros) >= busIdleMicros();
}

// ---------- Telegram send helpers ----------
void sendFramedPayload(const String& payload) {
  Serial2.write(0xFD);
  Serial2.print(payload);
  Serial2.write(0xFE);

  Serial.print("[TX] 0xFD ");
  Serial.print(payload);
  Serial.println(" 0xFE");
}

void sendTempTelegram(uint32_t deviceId, uint32_t instance, int16_t p1, int16_t p2) {
  String payload =
    String(deviceId) + ".TMP." + String(instance) + ".STATUS." +
    String(p1) + "." + String(p2);
  sendFramedPayload(payload);
}

bool sendRawTelegramSoftBusFree(const String& payload, uint32_t maxWaitMs) {
  uint32_t start = millis();
  while ((millis() - start) < maxWaitMs) {
    rs485Poll();
    if (isBusIdleNow()) break;
    delay(1);
  }
  if (!isBusIdleNow()) return false;

  uint32_t backoff = random(BACKOFF_MIN_MS, BACKOFF_MAX_MS + 1);
  uint32_t t0 = millis();
  while (millis() - t0 < backoff) {
    rs485Poll();
    if (!isBusIdleNow()) return false;
    delay(1);
  }
  if (!isBusIdleNow()) return false;

  sendFramedPayload(payload);
  Serial2.flush();
  return true;
}

// ---------- Temperature encode helper ----------
static inline void tempToP1P2(float t, int16_t& p1, int16_t& p2) {
  if (t < -100 || t > 150) { p1 = -127; p2 = 0; return; }
  int32_t scaled = (int32_t) lroundf(t * 100.0f);
  p1 = (int16_t)(scaled / 100);
  p2 = (int16_t)(abs(scaled % 100));
}

// ---------- Parse helpers ----------
bool parseTempGetStatus(const String& payload, uint32_t& devOut, uint32_t& instOut) {
  int pTmp = payload.indexOf(".TMP.");
  if (pTmp < 0) return false;

  int p0 = payload.indexOf('.');
  if (p0 <= 0) return false;

  String devStr = payload.substring(0, p0);
  for (size_t i = 0; i < devStr.length(); i++) if (!isDigit(devStr[i])) return false;
  devOut = (uint32_t)devStr.toInt();

  int instStart = pTmp + 5;
  int instDot = payload.indexOf('.', instStart);
  if (instDot < 0) return false;

  String instStr = payload.substring(instStart, instDot);
  for (size_t i = 0; i < instStr.length(); i++) if (!isDigit(instStr[i])) return false;
  instOut = (uint32_t)instStr.toInt();

  String suffix = payload.substring(instDot + 1);
  return suffix == "gSTATUS";
}

// parse <dev>.SYS.<sysInst>.SET_CFG.<p1>.<p2>
bool parseSysSetCfg2(const String& payload, uint32_t& devOut, uint32_t& sysInstOut, uint32_t& p1Out, String& p2StrOut) {
  int p[6];
  int start = 0;
  for (int i = 0; i < 5; i++) {
    p[i] = payload.indexOf('.', start);
    if (p[i] < 0) return false;
    start = p[i] + 1;
  }

  String part0 = payload.substring(0, p[0]);
  String part1 = payload.substring(p[0] + 1, p[1]);
  String part2 = payload.substring(p[1] + 1, p[2]);
  String part3 = payload.substring(p[2] + 1, p[3]);
  String part4 = payload.substring(p[3] + 1, p[4]);
  String part5 = payload.substring(p[4] + 1);

  if (part1 != "SYS") return false;
  if (part3 != "SET_CFG") return false;

  if (part0.length() == 0) return false;
  for (size_t i = 0; i < part0.length(); i++) if (!isDigit(part0[i])) return false;
  devOut = (uint32_t)part0.toInt();

  if (part2.length() == 0) return false;
  for (size_t i = 0; i < part2.length(); i++) if (!isDigit(part2[i])) return false;
  sysInstOut = (uint32_t)part2.toInt();

  if (part4.length() == 0) return false;
  for (size_t i = 0; i < part4.length(); i++) if (!isDigit(part4[i])) return false;
  p1Out = (uint32_t)part4.toInt();

  p2StrOut = part5;
  return true;
}

// parse <dev>.SYS.1.gCONFIG.<p1>
bool parseSysGetConfig(const String& payload, uint32_t& devOut, uint32_t& sysInstOut, uint32_t& p1Out) {
  int p0 = payload.indexOf('.');
  if (p0 <= 0) return false;
  int p1 = payload.indexOf('.', p0 + 1);
  if (p1 < 0) return false;
  int p2 = payload.indexOf('.', p1 + 1);
  if (p2 < 0) return false;
  int p3 = payload.indexOf('.', p2 + 1);
  if (p3 < 0) return false;

  String part0 = payload.substring(0, p0);
  String part1 = payload.substring(p0 + 1, p1);
  String part2 = payload.substring(p1 + 1, p2);
  String part3 = payload.substring(p2 + 1, p3);
  String part4 = payload.substring(p3 + 1);

  if (part1 != "SYS") return false;
  if (part3 != "gCONFIG") return false;

  for (size_t i = 0; i < part0.length(); i++) if (!isDigit(part0[i])) return false;
  devOut = (uint32_t)part0.toInt();

  for (size_t i = 0; i < part2.length(); i++) if (!isDigit(part2[i])) return false;
  sysInstOut = (uint32_t)part2.toInt();

  for (size_t i = 0; i < part4.length(); i++) if (!isDigit(part4[i])) return false;
  p1Out = (uint32_t)part4.toInt();

  return true;
}

bool parseUIntStrict(const String& s, uint32_t& out) {
  if (s.length() == 0) return false;
  for (size_t i = 0; i < s.length(); i++) if (!isDigit(s[i])) return false;
  out = (uint32_t)s.toInt();
  return true;
}

bool findSensorByInstance(uint32_t inst, DeviceAddress& addrOut, float& tempOut) {
  int count = sensors.getDeviceCount();
  for (int i = 0; i < count; i++) {
    DeviceAddress addr;
    if (!sensors.getAddress(addr, i)) continue;

    String romHex = romToHexNoSep(addr);
    uint32_t mapped = getMappedInstanceForRom(romHex, 1);
    if (mapped == inst) {
      for (int k = 0; k < 8; k++) addrOut[k] = addr[k];
      tempOut = sensors.getTempC(addr);
      return true;
    }
  }
  return false;
}

// ---------- WiFi/Web control ----------
static void wifiWebStartNow() {
  if (g_webStarted) {
    Serial.println("[WEB] already started");
    return;
  }

  Serial.println("[WIFI] starting AP...");

  WiFi.mode(WIFI_OFF);
  delay(80);

  WiFi.mode(WIFI_AP);
  delay(80);

  bool ok;
  if (strlen(AP_PASS) >= 8) ok = WiFi.softAP(AP_SSID, AP_PASS);
  else ok = WiFi.softAP(AP_SSID);

  delay(120);

  IPAddress ip = WiFi.softAPIP();
  Serial.print("[WIFI] AP start ");
  Serial.print(ok ? "OK" : "FAIL");
  Serial.print(" | mode=");
  Serial.print((int)WiFi.getMode());
  Serial.print(" | IP: ");
  Serial.println(ip);

  setupWeb();
  server.begin();
  g_webStarted = true;
  Serial.println("[WEB] started");
}

static void wifiWebStopNow() {
  Serial.println("[WIFI] stopping AP...");

  if (g_webStarted) {
    server.stop();
    delay(80);
    g_webStarted = false;
    Serial.println("[WEB] stopped");
  }

  WiFi.softAPdisconnect(true);
  delay(120);

  WiFi.mode(WIFI_OFF);
  delay(120);

  Serial.print("[WIFI] off | mode=");
  Serial.println((int)WiFi.getMode());
}

void wifiRequest(bool en) {
  g_wifiDesired = en;
  uint32_t now = millis();
  if (g_wifiNextActionMs < now) g_wifiNextActionMs = now;
}

void wifiServiceTick() {
  uint32_t now = millis();
  bool actualOn = (g_webStarted && (WiFi.getMode() == WIFI_AP));

  if (actualOn == g_wifiDesired) return;
  if (now < g_wifiNextActionMs) return;

  if (g_wifiDesired) {
    Serial.println("[WIFI] transition -> ON");
    wifiWebStartNow();
  } else {
    Serial.println("[WIFI] transition -> OFF");
    wifiWebStopNow();
  }

  g_wifiNextActionMs = now + WIFI_TRANSITION_COOLDOWN_MS;
}

// ---------- RX handling ----------
void handleRxPayload(const String& payload) {
  Serial.print("[RX] ");
  Serial.println(payload);

  // 1) SYS SET_CFG
  uint32_t devReq = 0, sysInst = 0, p1 = 0;
  String p2Str;
  if (parseSysSetCfg2(payload, devReq, sysInst, p1, p2Str)) {
    if (devReq != g_deviceId) return;
    if (sysInst != 1) return;

    if (p1 == 1) {
      uint32_t p2;
      if (!parseUIntStrict(p2Str, p2) || (p2 != 0 && p2 != 1)) {
        Serial.println("[SYS] SET_CFG P1=1 requires P2 0/1");
        return;
      }

      bool en = (p2 == 1);
      Serial.printf("[SYS] SET_CFG WiFi/Web -> %s\n", en ? "ON" : "OFF");

      saveWifiEn(en);
      wifiRequest(en);

      g_tempHoldoffUntilMs = millis() + 1000;
      return;
    }

    Serial.printf("[SYS] SET_CFG unknown P1=%u\n", (unsigned)p1);
    g_tempHoldoffUntilMs = millis() + 200;
    return;
  }

  // 2) SYS gCONFIG -> rCONFIG (ACTUAL runtime)
  uint32_t qP1 = 0;
  if (parseSysGetConfig(payload, devReq, sysInst, qP1)) {
    if (devReq != g_deviceId) return;
    if (sysInst != 1) return;

    if (qP1 == 1) {
      uint32_t value = (g_webStarted && (WiFi.getMode() == WIFI_AP)) ? 1 : 0;
      String reply = String(g_deviceId) + ".SYS.1.rCONFIG.1." + String(value);

      uint32_t start = millis();
      bool sent = false;
      while ((millis() - start) < 250 && !sent) {
        sent = sendRawTelegramSoftBusFree(reply, BUS_WAIT_MAX_MS);
        if (!sent) delay(5);
      }

      g_tempHoldoffUntilMs = millis() + 500;
      if (!sent) Serial.println("[SYS] rCONFIG reply not sent (bus busy)");
      else       Serial.println("[SYS] rCONFIG reply sent");
      return;
    }

    Serial.printf("[SYS] gCONFIG unknown P1=%u\n", (unsigned)qP1);
    return;
  }

  // 3) Ping responder
  String expectedReq = applyDeviceIdToken(g_pingRx);
  if (payload == expectedReq) {
    String reply = applyDeviceIdToken(g_pingTx);

    uint32_t start = millis();
    bool sent = false;
    while ((millis() - start) < 200 && !sent) {
      sent = sendRawTelegramSoftBusFree(reply, BUS_WAIT_MAX_MS);
      if (!sent) delay(5);
    }

    g_tempHoldoffUntilMs = millis() + 1000;
    if (!sent) Serial.println("[PING] reply not sent (bus busy)");
    else       Serial.println("[PING] reply sent");
    return;
  }

  // 4) Temperature on-demand request
  uint32_t instReq = 0;
  if (parseTempGetStatus(payload, devReq, instReq)) {
    if (devReq != g_deviceId) return;
    if (instReq < 1 || instReq > 255) return;

    sensors.requestTemperatures();

    DeviceAddress addr;
    float t;
    if (!findSensorByInstance(instReq, addr, t)) {
      Serial.println("[TMP-REQ] instance not found -> ignore");
      return;
    }

    int16_t p1t, p2t;
    tempToP1P2(t, p1t, p2t);

    String replyPayload =
      String(g_deviceId) + ".TMP." + String(instReq) + ".STATUS." +
      String(p1t) + "." + String(p2t);

    uint32_t start = millis();
    bool sent = false;
    while ((millis() - start) < 250 && !sent) {
      sent = sendRawTelegramSoftBusFree(replyPayload, BUS_WAIT_MAX_MS);
      if (!sent) delay(5);
    }

    g_tempHoldoffUntilMs = millis() + 1000;
    if (!sent) Serial.println("[TMP-REQ] reply not sent (bus busy)");
    else       Serial.println("[TMP-REQ] reply sent");
    return;
  }
}

// ---------- RS485 RX polling ----------
void rs485Poll() {
  while (Serial2.available() > 0) {
    int b = Serial2.read();
    if (b < 0) break;

    g_lastRxByteMicros = micros();

    if (!g_inFrame) {
      if (b == 0xFD) { g_inFrame = true; g_rxBuf = ""; }
      continue;
    }

    if (b == 0xFE) {
      g_inFrame = false;
      handleRxPayload(g_rxBuf);
      g_rxBuf = "";
      continue;
    }

    if (g_rxBuf.length() < 220) g_rxBuf += (char)b;
    else { g_inFrame = false; g_rxBuf = ""; }
  }
}

// ---------- Cycle snapshot ----------
void prepareTempCycleSnapshot() {
  sensors.requestTemperatures();

  int count = sensors.getDeviceCount();
  if (count < 0) count = 0;

  g_cycleCount = 0;
  for (int i = 0; i < count && g_cycleCount < MAX_SENSORS; i++) {
    DeviceAddress addr;
    if (!sensors.getAddress(addr, i)) continue;

    for (int k = 0; k < 8; k++) g_cycleAddrs[g_cycleCount][k] = addr[k];
    g_cycleTemps[g_cycleCount] = sensors.getTempC(addr);
    g_cycleCount++;
  }

  g_cycleIndex = 0;
  Serial.printf("[CYCLE] prepared %u sensor(s)\n", (unsigned)g_cycleCount);
}

// ---------- Non-blocking temperature TX scheduler tick ----------
void tempTxSchedulerTick() {
  uint32_t now = millis();

  if (!g_autoEn || g_txMs == 0) { g_txState = TX_IDLE; return; }
  if (now < g_tempHoldoffUntilMs) { g_txState = TX_IDLE; return; }

  switch (g_txState) {
    case TX_IDLE: {
      if ((uint32_t)(now - g_lastCycleStartMs) >= g_txMs) {
        if (!isBusIdleNow()) { g_nextActionMs = now + 50; g_txState = TX_PREPARE_CYCLE; return; }
        g_txState = TX_PREPARE_CYCLE;
      }
      break;
    }
    case TX_PREPARE_CYCLE: {
      if (g_nextActionMs != 0 && now < g_nextActionMs) break;
      if (!isBusIdleNow()) { g_nextActionMs = now + 50; break; }

      g_nextActionMs = 0;
      g_lastCycleStartMs = now;

      prepareTempCycleSnapshot();
      g_txState = (g_cycleCount == 0) ? TX_IDLE : TX_SEND_ONE;
      break;
    }
    case TX_SEND_ONE: {
      if (g_cycleIndex >= g_cycleCount) { g_txState = TX_IDLE; break; }
      if (!isBusIdleNow()) break;

      String romHex = romToHexNoSep(g_cycleAddrs[g_cycleIndex]);
      uint32_t inst = getMappedInstanceForRom(romHex, 1);
      float t = g_cycleTemps[g_cycleIndex];

      int16_t p1, p2;
      tempToP1P2(t, p1, p2);

      Serial.printf("[SENS] #%u ROM %s -> instance %u | T=%.2f | key=%s\n",
                    (unsigned)g_cycleIndex, romHex.c_str(), (unsigned)inst, (double)t,
                    mappingKeyForRom(romHex).c_str());

      sendTempTelegram(g_deviceId, inst, p1, p2);

      g_cycleIndex++;
      if (g_cycleIndex >= g_cycleCount) g_txState = TX_IDLE;
      else { g_nextActionMs = now + g_gapMs; g_txState = TX_WAIT_GAP; }
      break;
    }
    case TX_WAIT_GAP: {
      if (now >= g_nextActionMs) g_txState = TX_SEND_ONE;
      break;
    }
    default:
      g_txState = TX_IDLE;
      break;
  }
}

// ---------- Web UI HTML ----------
String htmlPage() {
  String s;
  s.reserve(9800);

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
         "<div class='col'><label>Send interval (ms) (0 = off)</label><input id='txMs' type='number' min='0' max='3600000' step='250'></div>"
         "</div>"
         "<div class='row'>"
         "<div class='col'><label>Auto send enabled</label>"
         "<input id='autoEn' type='checkbox' style='width:auto;transform:scale(1.3);margin-right:8px'>"
         "<span style='font-size:14px;color:#444'>Enable periodic temperature sending</span>"
         "</div>"
         "<div class='col'></div>"
         "</div>"
         "<div class='row'>"
         "<div class='col'><label>Gap between sensor telegrams (ms)</label>"
         "<input id='gapMs' type='number' min='0' max='2000' step='10'></div>"
         "<div class='col'></div>"
         "</div>"
         "<div class='row'>"
         "<div class='col'><label>Ping response template</label>"
         "<input id='pingTx' type='text' placeholder='DeviceID.OUT.210.rSTATUS'></div>"
         "<div class='col'><label>Ping request template</label>"
         "<input id='pingRx' type='text' placeholder='DeviceID.OUT.210.gSTATUS'></div>"
         "</div>"
         "<button onclick='saveBus()'>Save bus settings</button>"
         "<div id='busMsg'></div>"
         "<p style='font-size:13px;color:#555'>On-demand Temp: <code>&lt;DeviceID&gt;.TMP.&lt;inst&gt;.gSTATUS</code></p>"
         "<p style='font-size:13px;color:#555'>SYS SET: <code>&lt;DeviceID&gt;.SYS.1.SET_CFG.1.1</code> (WiFi/Web ON), "
         "<code>&lt;DeviceID&gt;.SYS.1.SET_CFG.1.0</code> (WiFi/Web OFF)</p>"
         "<p style='font-size:13px;color:#555'>SYS GET: <code>&lt;DeviceID&gt;.SYS.1.gCONFIG.1</code> → "
         "<code>&lt;DeviceID&gt;.SYS.1.rCONFIG.1.&lt;0|1&gt;</code></p>"
         "</div>");

  s += F("<div class='card'><h3>Sensors</h3>"
         "<button onclick='loadSensors()'>Refresh sensor list</button>"
         "<div id='sens'></div></div>");

  s += F("</body></html>");

  s += F("<script>"
         "async function loadBus(){"
         " const r=await fetch('/api/buscfg'); const j=await r.json();"
         " deviceId.value=j.deviceId;"
         " txMs.value=j.txIntervalMs;"
         " gapMs.value=j.sensorGapMs;"
         " autoEn.checked=!!j.autoSendEnabled;"
         " pingTx.value=j.pingTx||'';"
         " pingRx.value=j.pingRx||'';"
         "}"
         "async function saveBus(){"
         " const payload={"
         "  deviceId:parseInt(deviceId.value,10),"
         "  txIntervalMs:parseInt(txMs.value,10),"
         "  sensorGapMs:parseInt(gapMs.value,10),"
         "  autoSendEnabled:!!autoEn.checked,"
         "  pingTx:pingTx.value,"
         "  pingRx:pingRx.value"
         " };"
         " const r=await fetch('/api/buscfg',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)});"
         " const j=await r.json();"
         " busMsg.innerText = j.ok ? 'Saved.' : ('Error: '+(j.error||'unknown'));"
         " if(j.ok) setTimeout(()=>busMsg.innerText='',2000);"
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
         " sens.innerHTML=h;"
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

// ---------- Minimal JSON parsing ----------
bool jsonGetUInt(const String& body, const String& key, uint32_t& out) {
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

bool jsonGetBool(const String& body, const String& key, bool& out) {
  int k = body.indexOf("\"" + key + "\"");
  if (k < 0) return false;
  int c = body.indexOf(":", k);
  if (c < 0) return false;
  int i = c + 1;
  while (i < (int)body.length() && (body[i] == ' ' || body[i] == '\n' || body[i] == '\r' || body[i] == '\t')) i++;
  if (body.startsWith("true", i))  { out = true;  return true; }
  if (body.startsWith("false", i)) { out = false; return true; }
  return false;
}

bool jsonGetString(const String& body, const String& key, String& out) {
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
    out += "\"autoSendEnabled\":" + String(g_autoEn ? "true" : "false") + ",";
    out += "\"sensorGapMs\":" + String(g_gapMs) + ",";
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

    uint32_t devId, txMs, gapMs;
    bool autoEn;
    String pingTx, pingRx;

    bool hasDev  = jsonGetUInt(body, "deviceId", devId);
    bool hasTx   = jsonGetUInt(body, "txIntervalMs", txMs);
    bool hasGap  = jsonGetUInt(body, "sensorGapMs", gapMs);
    bool hasAuto = jsonGetBool(body, "autoSendEnabled", autoEn);
    bool hasPTx  = jsonGetString(body, "pingTx", pingTx);
    bool hasPRx  = jsonGetString(body, "pingRx", pingRx);

    if (!hasDev && !hasTx && !hasGap && !hasAuto && !hasPTx && !hasPRx) {
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
      if (txMs > 3600000) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"txIntervalMs range\"}");
        return;
      }
      saveTxMs(txMs);
    }
    if (hasAuto) saveAutoEn(autoEn);

    if (hasGap) {
      if (gapMs > 2000) {
        server.send(400, "application/json", "{\"ok\":false,\"error\":\"sensorGapMs range\"}");
        return;
      }
      saveGapMs(gapMs);
    }
    if (hasPTx) { if (pingTx.length() == 0) pingTx = DEFAULT_PING_TX; savePingTx(pingTx); }
    if (hasPRx) { if (pingRx.length() == 0) pingRx = DEFAULT_PING_RX; savePingRx(pingRx); }

    Serial.printf("[CFG] deviceId=%u, txMs=%u, autoEn=%u, gapMs=%u\n",
                  (unsigned)g_deviceId, (unsigned)g_txMs, (unsigned)(g_autoEn ? 1 : 0), (unsigned)g_gapMs);

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

  // server.begin() is called in wifiWebStartNow()
}

// ---------- Setup/Loop ----------
void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println("\n--- ESP32 TMP SETUP ---");

  g_deviceId    = loadDeviceId();
  g_txMs        = loadTxMs();
  g_autoEn      = loadAutoEn();
  g_gapMs       = loadGapMs();
  g_wifiDesired = loadWifiEn();
  g_pingTx      = loadPingTx();
  g_pingRx      = loadPingRx();

  Serial.printf("[CFG] deviceId=%u, txMs=%u, autoEn=%u, gapMs=%u, wifiDesired=%u\n",
                (unsigned)g_deviceId, (unsigned)g_txMs, (unsigned)(g_autoEn ? 1 : 0),
                (unsigned)g_gapMs, (unsigned)(g_wifiDesired ? 1 : 0));

  Serial2.begin(RS485_BAUD, SERIAL_8E1, RS485_RX, RS485_TX);
  Serial.println("[RS485] Serial2 started");

  randomSeed((uint32_t)esp_random());

  sensors.begin();
  Serial.printf("[DS18B20] found devices: %d\n", sensors.getDeviceCount());

  g_wifiNextActionMs = millis();
  wifiServiceTick(); // start AP if desired

  g_lastCycleStartMs = millis();
  g_txState = TX_IDLE;
}

void loop() {
  rs485Poll();
  wifiServiceTick();

  if (g_webStarted) server.handleClient();

  tempTxSchedulerTick();
}
