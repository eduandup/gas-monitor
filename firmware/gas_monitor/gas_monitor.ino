// =====================================================================
//  Gas Monitor: EyeGas Pro / Mopeka sensors -> ESP32-S3 -> Supabase + local website
//  Every 5 minutes the readings are sent to Supabase (the online dashboard reads them there).
//  The local page at http://gas.local (or the IP on the Serial Monitor) keeps working without internet.
//
//  Arduino IDE settings (Tools menu, ESP32S3 Dev Module):
//    USB CDC On Boot:   Enabled
//    Partition Scheme:  Huge APP (3MB No OTA/1MB SPIFFS)   <- needed for Wi-Fi + BLE + history storage
//
//  Wi-Fi and Supabase keys go in secrets.h (copy secrets.example.h). It is git-ignored.
// =====================================================================

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <time.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "secrets.h is missing: copy secrets.example.h to secrets.h and fill it in"
#endif
#include "ca_certs.h"
#include "page.h"

// ============================ SETTINGS ============================
const char *HOSTNAME  = "gas";            // website at http://gas.local

struct TankConfig {
  const char *name;
  const char *mac;        // sensor MAC (from the Serial Monitor)
  float capacityKg;       // gas in a full cylinder
  float fullMm;           // sensor reading on a freshly filled cylinder  <- CALIBRATE
  float emptyMm;          // sensor reading when the cylinder runs out    <- CALIBRATE
  float diameterMm;       // inside diameter, corrects for the domed bottom
};

TankConfig TANKS[] = {
  // name      sensor MAC            kg    full  empty  diameter
  {"Tank 1", "f1:81:73:69:2c:5b",  48.0,  900,  20,   365},
  {"Tank 2", "e9:e1:cf:08:46:7c",  48.0,  900,  20,   365},
};
const int NUM_TANKS = sizeof(TANKS) / sizeof(TANKS[0]);

const char *TZ_INFO     = "SAST-2";   // South Africa (UTC+2)
const uint32_t BUCKET_S = 900;        // one history point every 15 minutes
const int HIST_LEN      = 2880;       // 30 days of history per tank
const float RATE_WINDOW_H = 72;       // usage rate = trend over the last 3 days
const float REFILL_JUMP_PCT = 8;      // a rise bigger than this is treated as a refill

const uint32_t UPLOAD_S   = 300;      // one cloud row per tank every 5 minutes
const int QUEUE_LEN       = 576;      // rows kept while offline: 24 h for 2 tanks
const int UPLOAD_BATCH    = 36;       // rows per HTTPS request
const uint32_t SENSOR_LOST_MS = 900000;   // sensor counts as "not heard" after 15 minutes
// ==================================================================

// ---------- live readings (written by the BLE task) ----------
struct Live {
  bool valid = false;
  float levelMm = NAN;
  int quality = 0, tempC = 0, rssi = 0;
  float voltage = 0;
  uint32_t lastSeenMs = 0;
  float samples[64];       // good readings in the current 15-min bucket
  uint32_t sampleCount = 0;
  float upSamples[64];     // good readings in the current 5-min upload window
  uint32_t upCount = 0;
};
Live live[NUM_TANKS];
SemaphoreHandle_t lock;

// ---------- history ring buffers (persisted to flash hourly) ----------
struct Point { uint32_t t; uint16_t mm10; uint16_t pad; };
struct History { uint32_t magic; uint16_t head; uint16_t count; Point pts[HIST_LEN]; };
const uint32_t HIST_MAGIC = 0x47415332;
History hist[NUM_TANKS];

struct Stats { float rate = NAN; float used24 = NAN; };   // defined early: the Arduino IDE auto-declares functions near the top

// ---------- upload queue (rows waiting to be sent to Supabase) ----------
struct Row {               // one tank, one 5-min window; "null" markers: mm10 < 0, quality < 0, batt32 == 0
  uint32_t ts;
  int16_t mm10;
  int8_t tank, quality, tempC, rssi;
  uint8_t batt32, samples;
};
Row queue[QUEUE_LEN];
uint32_t qEnd = 0;         // sequence number of the next row; row s lives in queue[s % QUEUE_LEN]
int qCount = 0;
uint32_t qDropped = 0;     // rows lost because the queue was full
SemaphoreHandle_t qlock;
volatile int lastUploadCode = 0;
volatile uint32_t lastUploadOkMs = 0, uploadedTotal = 0;

WebServer server(80);
BLEScan *scan;

// ---------- tank geometry: mm of liquid -> % of gas ----------
float volAt(const TankConfig &c, float h) {          // 2:1 elliptical bottom + straight body
  float a = c.diameterMm / 2, hd = c.diameterMm / 4;
  if (h <= 0) return 0;
  if (h <= hd) return PI * a * a * (h * h / hd - h * h * h / (3 * hd * hd));
  return PI * a * a * (2 * hd / 3 + (h - hd));
}
float pctFor(int i, float mm) {
  const TankConfig &c = TANKS[i];
  float v0 = volAt(c, c.emptyMm), v1 = volAt(c, c.fullMm);
  return constrain((volAt(c, mm) - v0) / (v1 - v0) * 100.0f, 0.0f, 100.0f);
}

float rawToMm(uint16_t raw, uint8_t rawTemp) {        // LPG speed-of-sound compensation
  const float c0 = 0.573045f, c1 = -0.002822f, c2 = -0.00000535f;
  return raw * (c0 + c1 * rawTemp + c2 * rawTemp * rawTemp);
}

float median(float *v, int n) {
  for (int i = 1; i < n; i++) { float x = v[i]; int j = i - 1; while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; } v[j + 1] = x; }
  return (n % 2) ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}

// ---------- BLE ----------
class Callbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) {
    if (!dev.haveManufacturerData()) return;
    auto md = dev.getManufacturerData();
    if (md.length() != 12 || (uint8_t)md[0] != 0x59 || (uint8_t)md[1] != 0x00) return;

    String mac = String(dev.getAddress().toString().c_str());
    int idx = -1;
    for (int i = 0; i < NUM_TANKS; i++) if (mac.equalsIgnoreCase(TANKS[i].mac)) idx = i;
    if (idx < 0) return;

    const uint8_t *d = (const uint8_t *)md.c_str() + 2;
    uint8_t  rawTemp = d[2] & 0x7F;
    uint16_t rawLvl  = ((d[4] << 8) | d[3]) & 0x3FFF;
    uint8_t  quality = d[4] >> 6;
    float    mm      = rawToMm(rawLvl, rawTemp);

    xSemaphoreTake(lock, portMAX_DELAY);
    Live &L = live[idx];
    L.valid = true;
    L.tempC = (int)rawTemp - 40;
    L.voltage = (d[1] & 0x7F) / 32.0f;
    L.rssi = dev.getRSSI();
    L.quality = quality;
    L.lastSeenMs = millis();
    if (quality >= 1) L.levelMm = mm;
    if (quality >= 2) {
      L.samples[L.sampleCount++ % 64] = mm;
      L.upSamples[L.upCount++ % 64] = mm;
    }
    xSemaphoreGive(lock);
  }
};

void scanTask(void *) {
  for (;;) {
    scan->start(5, false);
    scan->clearResults();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ---------- history helpers ----------
const Point &histAt(int i, int k) { return hist[i].pts[(hist[i].head + k) % HIST_LEN]; }
float histPct(int i, int k) { return pctFor(i, histAt(i, k).mm10 / 10.0f); }

void histPush(int i, uint32_t t, float mm) {
  History &H = hist[i];
  uint16_t idx;
  if (H.count < HIST_LEN) { idx = (H.head + H.count) % HIST_LEN; H.count++; }
  else { idx = H.head; H.head = (H.head + 1) % HIST_LEN; }
  H.pts[idx].t = t;
  H.pts[idx].mm10 = (uint16_t)constrain(mm * 10.0f, 0.0f, 65535.0f);
}

void loadHistory() {
  for (int i = 0; i < NUM_TANKS; i++) {
    char path[16]; snprintf(path, sizeof(path), "/hist%d.bin", i);
    bool ok = false;
    File f = LittleFS.open(path, "r");
    if (f && f.size() == sizeof(History)) {
      ok = f.read((uint8_t *)&hist[i], sizeof(History)) == sizeof(History)
           && hist[i].magic == HIST_MAGIC && hist[i].count <= HIST_LEN && hist[i].head < HIST_LEN;
    }
    if (f) f.close();
    if (!ok) { hist[i].magic = HIST_MAGIC; hist[i].head = 0; hist[i].count = 0; }
    Serial.printf("%s: %u history points loaded\n", TANKS[i].name, hist[i].count);
  }
}

void saveHistory() {
  for (int i = 0; i < NUM_TANKS; i++) {
    char path[16]; snprintf(path, sizeof(path), "/hist%d.bin", i);
    File f = LittleFS.open(path, "w");
    if (!f) continue;
    f.write((const uint8_t *)&hist[i], sizeof(History));
    f.close();
  }
}

void closeBucket(uint32_t t) {
  for (int i = 0; i < NUM_TANKS; i++) {
    float buf[64]; int n;
    xSemaphoreTake(lock, portMAX_DELAY);
    n = min((uint32_t)64, live[i].sampleCount);
    memcpy(buf, live[i].samples, n * sizeof(float));
    live[i].sampleCount = 0;
    xSemaphoreGive(lock);
    if (n >= 3) histPush(i, t, median(buf, n));
  }
}

// ---------- usage statistics ----------
Stats computeStats(int i, uint32_t now) {
  Stats s;
  int n = hist[i].count;
  if (n < 2) return s;
  float cap = TANKS[i].capacityKg;

  // usage rate: linear trend of kg over the last RATE_WINDOW_H hours, stopping at a refill
  uint32_t winStart = now - (uint32_t)(RATE_WINDOW_H * 3600);
  int start = n - 1;
  while (start > 0) {
    if (histAt(i, start - 1).t < winStart) break;
    if (histPct(i, start) - histPct(i, start - 1) > REFILL_JUMP_PCT) break;
    start--;
  }
  int m = n - start;
  if (m >= 8 && histAt(i, n - 1).t - histAt(i, start).t >= 6 * 3600) {
    double sx = 0, sy = 0, sxx = 0, sxy = 0, t0 = histAt(i, start).t;
    for (int k = start; k < n; k++) {
      double x = (histAt(i, k).t - t0) / 3600.0, y = cap * histPct(i, k) / 100.0;
      sx += x; sy += y; sxx += x * x; sxy += x * y;
    }
    double den = m * sxx - sx * sx;
    if (den > 0) s.rate = max(0.0, -((m * sxy - sx * sy) / den) * 24.0);
  }

  // gas used in the last 24 h (skipped if a refill happened)
  uint32_t dayAgo = now - 86400;
  int k = n - 1; bool refill = false;
  while (k > 0 && histAt(i, k - 1).t >= dayAgo) {
    if (histPct(i, k) - histPct(i, k - 1) > REFILL_JUMP_PCT) { refill = true; break; }
    k--;
  }
  if (!refill && histAt(i, n - 1).t - histAt(i, k).t >= 20 * 3600)
    s.used24 = max(0.0f, cap * (histPct(i, k) - histPct(i, n - 1)) / 100.0f);
  return s;
}

// ---------- JSON helpers ----------
String num(double v, int dec) { return isnan(v) ? String("null") : String(v, dec); }

// ---------- web handlers ----------
// ---------- cloud upload (Supabase) ----------
bool cloudEnabled() { return strlen(SUPABASE_URL) > 12 && !strstr(SUPABASE_URL, "xxxxxxxx"); }

void enqueue(const Row &r) {
  xSemaphoreTake(qlock, portMAX_DELAY);
  queue[qEnd % QUEUE_LEN] = r;
  qEnd++;
  if (qCount < QUEUE_LEN) qCount++; else qDropped++;      // full: the oldest row is overwritten
  xSemaphoreGive(qlock);
}

// end of a 5-min window: one row per tank, even if the sensor wasn't heard (shows the hub is alive)
void closeUploadWindow(uint32_t t) {
  for (int i = 0; i < NUM_TANKS; i++) {
    xSemaphoreTake(lock, portMAX_DELAY);
    Live L = live[i];
    live[i].upCount = 0;
    xSemaphoreGive(lock);
    int n = min((uint32_t)64, L.upCount);
    bool heard = L.valid && millis() - L.lastSeenMs < SENSOR_LOST_MS;
    Row r;
    r.ts = t;
    r.tank = i + 1;
    r.samples = n;
    r.mm10 = n ? (int16_t)constrain(median(L.upSamples, n) * 10.0f, 0.0f, 32767.0f) : -1;
    r.quality = heard ? L.quality : -1;
    r.tempC = heard ? L.tempC : 0;
    r.rssi = heard ? L.rssi : 0;
    r.batt32 = heard ? (uint8_t)lroundf(L.voltage * 32) : 0;
    enqueue(r);
  }
}

void rowJson(String &b, const Row &r) {
  bool heard = r.quality >= 0;
  char ts[24]; time_t tt = r.ts; struct tm tm;
  gmtime_r(&tt, &tm);
  strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm);
  b += "{\"ts\":\""; b += ts;
  b += "\",\"tank\":"; b += String((int)r.tank);
  b += ",\"level_mm\":"; b += r.mm10 < 0 ? String("null") : String(r.mm10 / 10.0f, 1);
  b += ",\"quality\":"; b += heard ? String((int)r.quality) : String("null");
  b += ",\"temp_c\":"; b += heard ? String((int)r.tempC) : String("null");
  b += ",\"battery_v\":"; b += r.batt32 ? String(r.batt32 / 32.0f, 2) : String("null");
  b += ",\"rssi\":"; b += heard ? String((int)r.rssi) : String("null");
  b += ",\"samples\":"; b += String((int)r.samples);
  b += "}";
}

// returns how many rows Supabase stored as new, or -1 on failure
int postRows(const Row *rows, int n) {
  String body;
  body.reserve(80 + n * 150);
  body += "{\"p_key\":\""; body += DEVICE_KEY; body += "\",\"p_rows\":[";
  for (int k = 0; k < n; k++) { if (k) body += ","; rowJson(body, rows[k]); }
  body += "]}";

  String url = SUPABASE_URL;
  if (url.endsWith("/")) url.remove(url.length() - 1);
  url += "/rest/v1/rpc/ingest";

  WiFiClientSecure client;
  client.setCACert(SUPABASE_ROOT_CA);       // real certificate check, see ca_certs.h
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  if (!http.begin(client, url)) { lastUploadCode = -1; Serial.println("upload: bad SUPABASE_URL"); return -1; }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  if (strncmp(SUPABASE_ANON_KEY, "eyJ", 3) == 0)          // legacy JWT anon key also goes in Authorization
    http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  int code = http.POST(body);
  String resp = code > 0 ? http.getString() : String();
  http.end();
  lastUploadCode = code;
  if (code == 200) { lastUploadOkMs = millis(); uploadedTotal += n; return resp.toInt(); }
  if (code < 0) Serial.printf("upload: %s\n", HTTPClient::errorToString(code).c_str());
  else Serial.printf("upload: Supabase replied %d: %s\n", code, resp.substring(0, 200).c_str());
  return -1;
}

// sends queued rows in batches; on failure waits 30 s, 1, 2, 4 ... up to 10 min
void uploadTask(void *) {
  uint32_t retryAtMs = 0;
  int fails = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (WiFi.status() != WL_CONNECTED || (int32_t)(millis() - retryAtMs) < 0) continue;

    Row batch[UPLOAD_BATCH];
    xSemaphoreTake(qlock, portMAX_DELAY);
    int n = min(qCount, UPLOAD_BATCH);
    uint32_t first = qEnd - qCount;
    for (int k = 0; k < n; k++) batch[k] = queue[(first + k) % QUEUE_LEN];
    xSemaphoreGive(qlock);
    if (!n) continue;

    int added = postRows(batch, n);
    if (added >= 0) {
      xSemaphoreTake(qlock, portMAX_DELAY);
      uint32_t head = qEnd - qCount, done = first + n;     // rows may have been overwritten meanwhile
      if (done > head) qCount -= min((uint32_t)qCount, done - head);
      int left = qCount;
      xSemaphoreGive(qlock);
      Serial.printf("uploaded %d rows (%d new), %d still queued\n", n, added, left);
      fails = 0;
      retryAtMs = millis();                                  // keep draining a backlog
    } else {
      fails++;
      uint32_t wait = min(600000UL, 30000UL << min(fails - 1, 5));
      retryAtMs = millis() + wait;
      Serial.printf("upload failed, retrying in %lu s (%d rows queued)\n", (unsigned long)(wait / 1000), qCount);
    }
  }
}

// PAGE (the local website) lives in page.h

void handleRoot() { server.send(200, "text/html", PAGE); }

void handleState() {
  uint32_t now = time(nullptr);
  bool timeOk = now > 1700000000;
  String j;
  j.reserve(1400);
  j += "{\"now\":"; j += String(now);
  j += ",\"time_ok\":"; j += timeOk ? "true" : "false";
  j += ",\"tanks\":[";
  for (int i = 0; i < NUM_TANKS; i++) {
    const TankConfig &c = TANKS[i];
    Live L; float buf[64]; int n;
    xSemaphoreTake(lock, portMAX_DELAY);
    L = live[i];
    n = min((uint32_t)64, L.sampleCount);
    memcpy(buf, L.samples, n * sizeof(float));
    xSemaphoreGive(lock);

    uint32_t age = L.valid ? (millis() - L.lastSeenMs) / 1000 : 0;
    float mm = (n >= 3) ? median(buf, n) : L.levelMm;
    if (L.valid && age > 900) mm = NAN;                     // stale
    float pct = isnan(mm) ? NAN : pctFor(i, mm);
    if (isnan(pct) && hist[i].count) {                      // fall back to last stored point
      mm = histAt(i, hist[i].count - 1).mm10 / 10.0f;
      pct = pctFor(i, mm);
    }
    float kg = isnan(pct) ? NAN : c.capacityKg * pct / 100.0f;
    Stats st = timeOk ? computeStats(i, now) : Stats();
    double days = (!isnan(st.rate) && st.rate >= 0.1 && !isnan(kg)) ? kg / st.rate : NAN;
    float batt = constrain((L.voltage - 2.2f) / 0.65f * 100.0f, 0.0f, 100.0f);

    if (i) j += ",";
    j += "{\"name\":\""; j += c.name;
    j += "\",\"mac\":\""; j += c.mac;
    j += "\",\"capacity_kg\":"; j += num(c.capacityKg, 1);
    j += ",\"valid\":"; j += L.valid ? "true" : "false";
    j += ",\"age_s\":"; j += String(age);
    j += ",\"quality\":"; j += String(L.quality);
    j += ",\"level_mm\":"; j += num(mm, 0);
    j += ",\"percent\":"; j += num(pct, 1);
    j += ",\"kg\":"; j += num(kg, 2);
    j += ",\"temp_c\":"; j += L.valid ? String(L.tempC) : String("null");
    j += ",\"battery_v\":"; j += L.valid ? num(L.voltage, 2) : String("null");
    j += ",\"battery_pct\":"; j += L.valid ? num(batt, 0) : String("null");
    j += ",\"rssi\":"; j += L.valid ? String(L.rssi) : String("null");
    j += ",\"rate_kg_day\":"; j += num(st.rate, 2);
    j += ",\"used_24h\":"; j += num(st.used24, 2);
    j += ",\"days_left\":"; j += num(days, 1);
    j += ",\"empty_at\":"; j += isnan(days) ? String("null") : String((double)now + days * 86400.0, 0);
    j += "}";
  }
  j += "]}";
  server.send(200, "application/json", j);
}

void handleHistory() {
  uint32_t now = time(nullptr);
  if (now < 1700000000) { server.send(200, "application/json", "{\"t0\":0,\"tanks\":[]}"); return; }
  long hours = server.hasArg("hours") ? server.arg("hours").toInt() : 168;
  hours = constrain(hours, 1L, 24L * 31);
  uint32_t from = now - hours * 3600UL;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  String b; b.reserve(1600);
  b += "{\"t0\":"; b += String(from); b += ",\"tanks\":[";
  for (int i = 0; i < NUM_TANKS; i++) {
    b += i ? ",{\"m\":[" : "{\"m\":[";
    bool first = true;
    for (int k = 0; k < hist[i].count; k++) {
      const Point &p = histAt(i, k);
      if (p.t < from) continue;
      if (!first) b += ",";
      b += String((p.t - from) / 60); first = false;
      if (b.length() > 1200) { server.sendContent(b); b = ""; }
    }
    b += "],\"p\":["; first = true;
    for (int k = 0; k < hist[i].count; k++) {
      const Point &p = histAt(i, k);
      if (p.t < from) continue;
      if (!first) b += ",";
      b += String(pctFor(i, p.mm10 / 10.0f), 1); first = false;
      if (b.length() > 1200) { server.sendContent(b); b = ""; }
    }
    b += "]}";
  }
  b += "]}";
  server.sendContent(b);
  server.sendContent("");
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(2000);
  lock = xSemaphoreCreateMutex();
  qlock = xSemaphoreCreateMutex();

  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed: history won't survive a reboot");
  loadHistory();

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to Wi-Fi");
  for (int k = 0; k < 40 && WiFi.status() != WL_CONNECTED; k++) { delay(500); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED)
    Serial.printf("\nConnected. Open http://%s.local or http://%s\n", HOSTNAME, WiFi.localIP().toString().c_str());
  else
    Serial.println("\nWi-Fi not connected yet, will keep retrying in the background");

  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");

  MDNS.begin(HOSTNAME);
  server.on("/", handleRoot);
  server.on("/api/state", handleState);
  server.on("/api/history", handleHistory);
  server.begin();
  MDNS.addService("http", "tcp", 80);

  BLEDevice::init("");
  scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new Callbacks(), true);
  scan->setActiveScan(false);
  scan->setInterval(160);
  scan->setWindow(80);        // ~50% duty cycle leaves the radio time for Wi-Fi
  xTaskCreatePinnedToCore(scanTask, "bleScan", 8192, nullptr, 1, nullptr, 0);

  if (cloudEnabled()) {
    xTaskCreatePinnedToCore(uploadTask, "upload", 12288, nullptr, 1, nullptr, 1);   // TLS needs a big stack
    Serial.printf("Cloud upload on: %s every %lu min\n", SUPABASE_URL, (unsigned long)(UPLOAD_S / 60));
  } else {
    Serial.println("Cloud upload off: SUPABASE_URL is not set in secrets.h");
  }
}

void loop() {
  server.handleClient();

  static uint32_t lastBucket = 0, lastWindow = 0, lastSaveMs = 0, lastPrintMs = 0;
  static bool dirty = false;
  uint32_t now = time(nullptr);

  if (now > 1700000000) {
    uint32_t b = now / BUCKET_S;
    if (lastBucket == 0) lastBucket = b;
    else if (b != lastBucket) { closeBucket(b * BUCKET_S); lastBucket = b; dirty = true; }

    uint32_t w = now / UPLOAD_S;
    if (lastWindow == 0) lastWindow = w;
    else if (w != lastWindow) { if (cloudEnabled()) closeUploadWindow(w * UPLOAD_S); lastWindow = w; }
  }

  if (dirty && millis() - lastSaveMs > 3600000UL) { saveHistory(); lastSaveMs = millis(); dirty = false; }

  if (millis() - lastPrintMs > 30000) {
    lastPrintMs = millis();
    Serial.printf("[%s] ", WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "no Wi-Fi");
    for (int i = 0; i < NUM_TANKS; i++) {
      xSemaphoreTake(lock, portMAX_DELAY);
      Live L = live[i];
      xSemaphoreGive(lock);
      if (L.valid) Serial.printf("%s: %.0f mm q%d %dC %.2fV  ", TANKS[i].name, L.levelMm, L.quality, L.tempC, L.voltage);
      else Serial.printf("%s: not heard  ", TANKS[i].name);
    }
    Serial.printf("| heap %lu (min %lu)", (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());
    if (cloudEnabled()) {
      Serial.printf(" | queued %d, sent %lu", qCount, (unsigned long)uploadedTotal);
      if (qDropped) Serial.printf(", dropped %lu", (unsigned long)qDropped);
      if (lastUploadOkMs) Serial.printf(", last ok %lu s ago", (unsigned long)((millis() - lastUploadOkMs) / 1000));
    }
    Serial.println();
  }
  delay(2);
}
