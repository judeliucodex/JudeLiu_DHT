/*
 * ROV DHT22 Mini Project — Phase 2: cloud-connected sensor node
 *
 * Board:   ESP32 DevKitC (ESP32-WROOM-32UE)
 * Sensor:  DHT22 (3-pin module = built-in 10k pull-up)
 * Display: 0.96" SSD1306 OLED (I2C)
 *
 * What it does:
 *   1. reads the DHT22 every 3 s (fixed interval; DHT22 spec minimum is 2 s)
 *   2. pushes every reading to the Supabase `readings` table via HTTPS POST,
 *      authenticated with the x-device-key header (checked by an RLS policy).
 *      SECURITY: the device key and the Wi-Fi credentials live in secrets.h,
 *      which is git-ignored. Never commit that file.
 *   3. offline buffering: failed pushes are queued in LittleFS flash
 *      (max ~16 KB, oldest dropped) and backfilled on reconnect, paced to
 *      stay under the server's rate limit. Backfilled rows carry their
 *      reconnect timestamp (the ESP32 has no battery-backed clock).
 *   4. Wi-Fi credentials live in NVS flash, seeded from the constants below.
 *      If joining fails for 60 s at boot, the device hosts its own access
 *      point "ROV-DHT22-Setup" — connect and open http://192.168.4.1 to
 *      choose a network; no re-flashing needed.
 *   5. local fallback dashboard (HTTP :80 + WebSocket :81) + OLED always work,
 *      even with no internet. The OLED is driven by the ESP32's own readings.
 *
 * Wiring:
 *   DHT22  VCC  -> 3V3      OLED  SDA -> GPIO21
 *   DHT22  DATA -> GPIO4    OLED  SCL -> GPIO22
 *   DHT22  GND  -> GND      OLED  VCC -> 3V3 / GND -> GND
 *
 * Arduino IDE: Board "ESP32 Dev Module", 115200 baud.
 * Libraries: DHT sensor library (+Unified Sensor), WebSockets by links2004,
 *            Adafruit SSD1306 (+GFX, BusIO).
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebSocketsServer.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include <LittleFS.h>
#include "secrets.h"        // private: Wi-Fi credentials + device key (git-ignored)

// ---------- settings ----------
const char* HOSTNAME          = "rov-dht22";          // http://rov-dht22.local

const char* DEVICE_ID     = "esp32-01";
const char* SUPABASE_BASE = "https://iohgczqztyckdejsretk.supabase.co";
const char* SUPABASE_KEY  = "sb_publishable_9r5-uHvlpGkAcTwKZiqtJA_xC7MTWFy";  // publishable key: public by design

#define DHTPIN  4
#define DHTTYPE DHT22
#define LED_PIN 2                    // DevKitC onboard LED, active-HIGH

#define OLED_SDA 21
#define OLED_SCL 22
#define OLED_W   128
#define OLED_H   64
#define OLED_ADDR 0x3C

const unsigned long intervalMs   = 3000;   // fixed logging interval
const unsigned long WIFI_TIMEOUT = 60000;  // before falling back to the setup AP
const char* AP_SSID              = "ROV-DHT22-Setup";
const char* BUF_PATH             = "/buf.jsonl";
const size_t  BUF_MAX_BYTES      = 16384;  // ~470 readings
// --------------------------------

DHT dht(DHTPIN, DHTTYPE);
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);   // -1 = no reset pin
WebSocketsServer webSocket(81);
WiFiServer httpServer(80);
WebServer portal(80);                // setup-mode web server
WiFiClientSecure secureClient;
Preferences prefs;

char readingsUrl[96];

bool servicesUp  = false;            // normal-mode servers started?
bool portalActive = false;
bool retryRequested = false;
unsigned long lastPortalRetry = 0;

uint32_t totalReads = 0, goodReads = 0;
uint32_t bufCount = 0;
unsigned long lastRead = 0, lastDrain = 0;

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ROV DHT22 Local</title>
<style>
  :root { color-scheme: dark; }
  body { font-family: system-ui, sans-serif; background: #0f172a; color: #e2e8f0;
         display: flex; flex-direction: column; align-items: center;
         justify-content: center; min-height: 100vh; margin: 0; }
  h1 { font-weight: 600; letter-spacing: .05em; }
  .cards { display: flex; gap: 2rem; flex-wrap: wrap; justify-content: center; }
  .card { background: #1e293b; border-radius: 16px; padding: 2rem 3rem; text-align: center;
          min-width: 200px; box-shadow: 0 8px 24px rgba(0,0,0,.35); }
  .label { text-transform: uppercase; font-size: .8rem; letter-spacing: .15em; color: #94a3b8; }
  .value { font-size: 3.2rem; font-weight: 700; margin-top: .4rem; }
  .unit  { font-size: 1.2rem; font-weight: 400; color: #94a3b8; }
  #status { margin-top: 2rem; font-size: .9rem; color: #94a3b8; }
  .ok   { color: #4ade80; }
  .err  { color: #f87171; }
</style>
</head>
<body>
  <h1>ROV DHT22 · local fallback</h1>
  <div class="cards">
    <div class="card">
      <div class="label">Temperature</div>
      <div class="value"><span id="temp">--</span><span class="unit"> &deg;C</span></div>
    </div>
    <div class="card">
      <div class="label">Humidity</div>
      <div class="value"><span id="hum">--</span><span class="unit"> %RH</span></div>
    </div>
  </div>
  <div id="status">connecting…</div>
<script>
let ws, retryTimer;
function connect() {
  ws = new WebSocket("ws://" + location.hostname + ":81");
  ws.onopen = () => document.getElementById("status").innerText = "connected";
  ws.onmessage = (e) => {
    const d = JSON.parse(e.data);
    const status = document.getElementById("status");
    if (d.ok) {
      document.getElementById("temp").innerText = d.t.toFixed(1);
      document.getElementById("hum").innerText  = d.h.toFixed(1);
      status.className = "ok";
      status.innerText = "connected · logging every 3 s · success " + d.rate + "%";
    } else {
      document.getElementById("temp").innerText = "--";
      document.getElementById("hum").innerText  = "--";
      status.className = "err";
      status.innerText = "sensor read error (check wiring)";
    }
  };
  ws.onclose = () => {
    document.getElementById("status").className = "err";
    document.getElementById("status").innerText = "disconnected — retrying every 3 s";
    clearTimeout(retryTimer);
    retryTimer = setTimeout(connect, 3000);
  };
}
connect();
</script>
</body>
</html>
)HTML";

static const char PORTAL_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ROV DHT22 Setup</title>
<style>
  body { font-family: system-ui, sans-serif; background: #0f172a; color: #e2e8f0;
         display: flex; flex-direction: column; align-items: center;
         justify-content: center; min-height: 100vh; margin: 0; }
  form { background: #1e293b; padding: 2rem; border-radius: 16px; width: 300px; }
  h1 { font-size: 1.1rem; }
  label { display: block; font-size: .8rem; color: #94a3b8; margin: .8rem 0 .2rem; }
  input { width: 100%; box-sizing: border-box; padding: .6rem; border-radius: 8px;
          border: 1px solid #334155; background: #0f172a; color: #e2e8f0; }
  button { margin-top: 1.2rem; width: 100%; padding: .7rem; border: 0; border-radius: 8px;
           background: #38bdf8; color: #0f172a; font-weight: 700; }
</style>
</head>
<body>
  <form action="/save" method="get">
    <h1>ROV DHT22 Wi-Fi setup</h1>
    <label>Wi-Fi name (SSID)</label>
    <input name="ssid" maxlength="32" required>
    <label>Wi-Fi password</label>
    <input name="pass" type="password" maxlength="64">
    <button>Save &amp; connect</button>
  </form>
</body>
</html>
)HTML";

// ---------- OLED ----------
void updateDisplay(float t, float h, bool ok) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.cp437(true);                       // enables the degree glyph

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(DEVICE_ID);
  display.setCursor(66, 0);
  if (portalActive)          display.print("setup");
  else if (WiFi.status() == WL_CONNECTED) display.print("cloud");
  else                       display.print("local");

  display.setTextSize(2);
  display.setCursor(0, 16);
  display.print(ok ? String(t, 1) : "--.-");
  display.print((char)247);                  // degree sign
  display.print("C  ");
  display.setCursor(0, 34);
  display.print(ok ? String(h, 1) : "--.-");
  display.print("%RH");

  display.setTextSize(1);
  display.setCursor(0, 54);
  if (portalActive)         display.print("SETUP  192.168.4.1");
  else if (bufCount > 0)    display.printf("readings %lu q%lu",
                              (unsigned long)totalReads, (unsigned long)bufCount);
  else                      display.printf("readings %lu", (unsigned long)totalReads);
  display.display();
}

// ---------- cloud ----------
bool pushToCloud(float t, float h) {
  HTTPClient http;
  http.begin(secureClient, readingsUrl);
  http.addHeader("apikey", SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("x-device-key", DEVICE_KEY);          // RLS checks this
  http.addHeader("Prefer", "return=minimal");

  String body = String("{\"device\":\"") + DEVICE_ID +
                "\",\"temperature\":" + String(t, 1) +
                ",\"humidity\":" + String(h, 1) + "}";
  int code = http.POST(body);
  http.end();
  if (code == 201) return true;
  Serial.printf("[cloud] push failed: HTTP %d\n", code);
  return false;
}

// ---------- offline buffer (LittleFS JSONL queue) ----------
uint32_t countBuf() {
  if (!LittleFS.exists(BUF_PATH)) return 0;
  File f = LittleFS.open(BUF_PATH, "r");
  if (!f) return 0;
  uint32_t n = 0;
  while (f.available()) { f.readStringUntil('\n'); n++; }
  f.close();
  return n;
}

void bufferAppend(float t, float h) {
  if (LittleFS.totalBytes() - LittleFS.usedBytes() < BUF_MAX_BYTES) return;  // keep flash headroom
  File f = LittleFS.open(BUF_PATH, "a");
  if (!f) return;
  f.printf("{\"t\":%.1f,\"h\":%.1f}\n", t, h);
  f.close();
  bufCount++;
  File c = LittleFS.open(BUF_PATH, "r");
  size_t sz = c.size();
  c.close();
  if (sz > BUF_MAX_BYTES && bufCount > 0) bufCount--;   // oldest row dropped on rewrite below
  if (sz > BUF_MAX_BYTES) {
    File r = LittleFS.open(BUF_PATH, "r");
    if (r) { r.readStringUntil('\n'); String rest = r.readString(); r.close();
             File w = LittleFS.open(BUF_PATH, "w"); w.print(rest); w.close(); }
  }
}

bool popBuf(float &t, float &h) {
  if (bufCount == 0) return false;
  File f = LittleFS.open(BUF_PATH, "r");
  if (!f) { bufCount = 0; return false; }
  String first = f.readStringUntil('\n');
  String rest  = f.readString();
  f.close();
  bufCount--;
  int ti = first.indexOf("\"t\":");
  int hi = first.indexOf("\"h\":");
  if (ti < 0 || hi < 0) return false;
  t = first.substring(ti + 4).toFloat();
  h = first.substring(hi + 4).toFloat();
  if (rest.length() == 0) LittleFS.remove(BUF_PATH);
  else {
    File w = LittleFS.open(BUF_PATH, "w");
    w.print(rest);
    w.close();
  }
  return true;
}

void drainOne() {
  float t, h;
  if (!popBuf(t, h)) return;
  if (!pushToCloud(t, h)) {          // still offline or rate-limited → put it back
    bufferAppend(t, h);
    lastDrain += 3000;               // ease off before the next attempt
  }
}

void pushOrBuffer(float t, float h) {
  if (WiFi.status() == WL_CONNECTED && pushToCloud(t, h)) return;
  bufferAppend(t, h);
}

// ---------- local fallback page ----------
void handleHttp() {
  WiFiClient client = httpServer.accept();
  if (!client) return;
  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) break;
  }
  client.print("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
               "Connection: close\r\n\r\n");
  client.print(INDEX_HTML);
  client.stop();
}

void broadcastReading(float t, float h, bool ok) {
  if (portalActive) return;                      // no local servers in setup mode
  char buf[160];
  if (ok) {
    uint32_t rate = (goodReads * 100) / totalReads;
    snprintf(buf, sizeof(buf),
             "{\"t\":%.1f,\"h\":%.1f,\"ok\":true,\"rate\":%lu,\"uptime\":%lu,\"iv\":%lu}",
             t, h, (unsigned long)rate,
             (unsigned long)(millis() / 1000), (unsigned long)(intervalMs / 1000));
  } else {
    snprintf(buf, sizeof(buf), "{\"ok\":false}");
  }
  webSocket.broadcastTXT(buf);
}

// ---------- setup portal ----------
void handlePortalSave() {
  String ssid = portal.arg("ssid");
  String pass = portal.arg("pass");
  if (ssid.length()) {
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    Serial.println("[portal] credentials saved to NVS");
  }
  portal.send(200, "text/html",
    "<html><body style='font-family:sans-serif;background:#0f172a;color:#e2e8f0;"
    "text-align:center;padding-top:4rem'><h2>Saved ✔</h2><p>Retrying the connection…"
    " you can close this page. If it fails, the setup access point comes back.</p></body></html>");
  retryRequested = true;
}

void startPortal() {
  portalActive = true;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID);
  portal.on("/", []() { portal.send(200, "text/html", PORTAL_HTML); });
  portal.on("/save", handlePortalSave);
  portal.onNotFound([]() { portal.send(200, "text/html", PORTAL_HTML); });
  portal.begin();
  Serial.println("[portal] no Wi-Fi — setup access point started");
  Serial.println("[portal] join 'ROV-DHT22-Setup' and open http://192.168.4.1");
  updateDisplay(NAN, NAN, false);
}

void tryPortalRetry() {
  Serial.println("[portal] retrying Wi-Fi with stored credentials…");
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  WiFi.begin(ssid.c_str(), pass.c_str());        // AP stays up meanwhile (AP_STA)
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(250);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[portal] connected — leaving setup mode");
    portal.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    portalActive = false;
    normalInit();
  } else {
    Serial.println("[portal] still failing; AP stays up");
  }
}

// ---------- normal mode ----------
bool connectSTA(unsigned long timeoutMs) {
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

void normalInit() {
  if (servicesUp) return;
  servicesUp = true;

  Serial.print("Local dashboard: http://");
  Serial.println(WiFi.localIP());
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("Also: http://" + String(HOSTNAME) + ".local");
  }

  snprintf(readingsUrl, sizeof(readingsUrl),
           "%s/rest/v1/readings", SUPABASE_BASE);
  secureClient.setInsecure();   // TLS without cert validation — documented limitation

  webSocket.begin();
  httpServer.begin();
  digitalWrite(LED_PIN, HIGH);
  Serial.println("logging every 3 s");
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.begin(115200);
  for (int i = 0; i < 30 && !Serial; i++) delay(100);

  Wire.begin(OLED_SDA, OLED_SCL);
  if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    updateDisplay(NAN, NAN, false);
  } else {
    Serial.println("[oled] not found (check D21/D22)");
  }

  dht.begin();
  LittleFS.begin(true);                        // true = format on mount failure
  bufCount = countBuf();
  Serial.printf("[fs] %lu buffered readings on flash\n", (unsigned long)bufCount);

  prefs.begin("rov-dht22", false);
  if (prefs.getString("ssid", "").length() == 0) {
    prefs.putString("ssid", WIFI_DEFAULT_SSID);
    prefs.putString("pass", WIFI_DEFAULT_PASS);
  }

  snprintf(readingsUrl, sizeof(readingsUrl),
           "%s/rest/v1/readings", SUPABASE_BASE);
  secureClient.setInsecure();   // TLS without cert validation — documented limitation

  Serial.print("Connecting to Wi-Fi");
  if (connectSTA(WIFI_TIMEOUT)) {
    normalInit();
  } else {
    startPortal();
  }
}

void loop() {
  // shared reading cadence — runs in both normal and setup mode
  if (millis() - lastRead >= intervalMs) {
    lastRead = millis();

    float h = dht.readHumidity();
    float t = dht.readTemperature();
    totalReads++;
    bool ok = !isnan(h) && !isnan(t);
    if (ok) goodReads++;
    Serial.printf("%lu,%s,%.1f,%.1f\n", (unsigned long)totalReads,
                  ok ? "ok" : "ERR", t, h);

    broadcastReading(t, h, ok);
    if (ok) pushOrBuffer(t, h);              // online: push; offline: buffer in flash
    updateDisplay(t, h, ok);
  }

  if (portalActive) {
    portal.handleClient();
    if (retryRequested) { retryRequested = false; tryPortalRetry(); }
    if (millis() - lastPortalRetry >= 30000) {   // auto-retry in case the router came back
      lastPortalRetry = millis();
      tryPortalRetry();
    }
  } else {
    handleHttp();                     // accepts + serves one page request per pass, if any
    webSocket.loop();

    // backfill: drain the flash queue, paced to stay under the rate limit
    if (WiFi.status() == WL_CONNECTED && bufCount > 0 && millis() - lastDrain >= 700) {
      lastDrain = millis();
      drainOne();
    }
  }
}
