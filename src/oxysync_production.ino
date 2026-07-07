// =====================================================================
// OxySync — PRODUCTION VERSION
// Megasoft | Client: Fred Faiz
//
// For real hardware: ESP32 Devkit V1 + Atlas Scientific ENO-02 (I2C)
// Flash this version to the actual device.
//
// Partition scheme: "Default 4MB with spiffs" (needed for LittleFS log)
// =====================================================================

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <esp_task_wdt.h>
#include <esp_idf_version.h>
#include <esp_system.h>

// =====================
// SENSOR CONFIG
// =====================
#define EZO_O2_ADDRESS 0x6C
#define SDA_PIN 21
#define SCL_PIN 22

// =====================
// RELAY CONFIG (ACTIVE HIGH — DFRobot MOSFET DFR0457)
// =====================
#define RELAY_PIN 27

// =====================
// WIFI AP CONFIG
// =====================
const char* ap_ssid     = "O2_Controller";
const char* ap_password = "techonly123";

// =====================
// BASIC AUTH
// =====================
const char* www_user = "tech";
const char* www_pass = "oxygen";

// =====================
// GLOBAL OBJECTS
// =====================
WebServer   server(80);
Preferences prefs;
bool        fsReady    = false;
int32_t     timeOffset = 0; // unix_time - (millis()/1000), updated by browser sync

// =====================
// SYSTEM STATE
// =====================
struct SystemState {
  float   o2Value        = 0.0;
  float   setpoint       = 20.9;
  bool    relayOn        = false;
  bool    sensorSleeping = false;

  bool    lockoutActive  = false;
  unsigned long lockoutStart = 0;

  bool    manualOverride    = false;
  bool    manualRelayState  = false;

  // True only when AUTO logic turned the relay on.
  // Lockout must never trigger from a manual relay action.
  bool    relayAutoActive = false;
};

SystemState state;

// =====================
// TIMING
// =====================
unsigned long lastReadTime = 0;
const unsigned long readInterval  = 10000UL;   // 10 seconds
const unsigned long lockoutTime   = 1800000UL; // 30 minutes

unsigned long manualStartTime = 0;
const unsigned long manualTimeout = 300000UL;  // 5 minutes

const uint32_t WDT_TIMEOUT_SEC = 30;

bool pendingReboot = false;
unsigned long rebootAt = 0;

unsigned long lastTimeSave = 0;
const unsigned long timeSaveInterval = 600000UL; // save time base every 10 min

// =====================
// SENSOR BUFFER
// =====================
char o2_concentration[32];

// =====================
// SENSOR DRIFT TRACKING
// =====================
const float DRIFT_CAUTION_PCT = 0.30f;
const float DRIFT_REPLACE_PCT = 0.60f;

// Sensor status codes — live issues take priority over drift status
#define SENSOR_COLLECTING   0  // not enough days of data yet
#define SENSOR_OK           1  // drift within limits
#define SENSOR_CAUTION      2  // drift ±0.3% — recalibrate soon
#define SENSOR_REPLACE      3  // drift ±0.6% — replace sensor
#define SENSOR_DISCONNECTED 4  // no I2C response
#define SENSOR_STUCK        5  // returning identical value for 1+ min
#define SENSOR_SLEEPING     6  // sleep command was sent

// Only completed daily averages and reference are persisted to NVS.
// Mid-day accumulators live in RAM and reset naturally on reboot.
struct DriftData {
  float   daily[30];  // circular buffer of daily O2 averages
  uint8_t count;      // how many daily slots are filled (0–30)
  uint8_t dayIndex;   // next write position
  float   refAvg;     // first day's average — baseline after calibration
  bool    refSet;     // whether the reference has been established
};

DriftData driftData;

float    driftDaySum     = 0.0f;
uint16_t driftDaySamples = 0;
unsigned long lastDayMs    = 0;
float         currentDrift = 0.0f;
uint8_t       sensorStatus = SENSOR_COLLECTING;

const unsigned long DAY_MS = 86400000UL;

void loadDrift() {
  prefs.begin("oxysync", false);
  size_t len = prefs.getBytesLength("drift");
  if (len == sizeof(DriftData)) {
    prefs.getBytes("drift", &driftData, sizeof(DriftData));
  } else {
    memset(&driftData, 0, sizeof(DriftData));
    driftData.refSet = false;
  }
  prefs.end();
}

void saveDrift() {
  prefs.begin("oxysync", false);
  prefs.putBytes("drift", &driftData, sizeof(DriftData));
  prefs.end();
}

void resetDrift() {
  memset(&driftData, 0, sizeof(DriftData));
  driftData.refSet = false;
  driftDaySum      = 0.0f;
  driftDaySamples  = 0;
  currentDrift     = 0.0f;
  saveDrift();
  Serial.println("[DRIFT] History reset after calibration.");
}

void recalcDriftStatus() {
  if (!driftData.refSet || driftData.count < 2) {
    // Only reset to COLLECTING if no live error is currently active
    if (sensorStatus < SENSOR_DISCONNECTED) {
      sensorStatus = SENSOR_COLLECTING;
    }
    currentDrift = 0.0f;
    return;
  }

  int recentDays = min((int)driftData.count, 3);
  float recentSum = 0.0f;
  for (int i = 0; i < recentDays; i++) {
    int idx = ((int)driftData.dayIndex - 1 - i + 30) % 30;
    recentSum += driftData.daily[idx];
  }
  float recentAvg = recentSum / recentDays;
  currentDrift = recentAvg - driftData.refAvg;
  float absDrift = fabs(currentDrift);

  if      (absDrift >= DRIFT_REPLACE_PCT) sensorStatus = SENSOR_REPLACE;
  else if (absDrift >= DRIFT_CAUTION_PCT) sensorStatus = SENSOR_CAUTION;
  else                                    sensorStatus = SENSOR_OK;
}

void updateDriftTracking(float reading) {
  driftDaySum += reading;
  driftDaySamples++;

  if (millis() - lastDayMs >= DAY_MS) {
    lastDayMs = millis();

    if (driftDaySamples > 0) {
      float dayAvg = driftDaySum / driftDaySamples;

      if (!driftData.refSet) {
        driftData.refAvg = dayAvg;
        driftData.refSet = true;
      }

      driftData.daily[driftData.dayIndex] = dayAvg;
      driftData.dayIndex = (driftData.dayIndex + 1) % 30;
      if (driftData.count < 30) driftData.count++;

      recalcDriftStatus();
      saveDrift();

      Serial.println("[DRIFT] Day avg: " + String(dayAvg, 2) +
                     "% | Drift: " + String(currentDrift, 2) +
                     "% | Days: " + String(driftData.count));
    }

    driftDaySum     = 0.0f;
    driftDaySamples = 0;
  }
}

// =====================
// LIVE SENSOR TRACKING
// =====================
float   lastValidO2          = -1.0f;
uint8_t stuckCount           = 0;
bool    sensorWasDisconnected = false;
bool    sensorWasStuck        = false;
const uint8_t STUCK_THRESHOLD = 6; // 6 consecutive identical reads ≈ 1 minute

// =====================
// TIME SYNC (browser sends Unix epoch on every dashboard load)
// =====================
uint32_t getUnixTime() {
  return (uint32_t)(millis() / 1000) + (uint32_t)(timeOffset > 0 ? timeOffset : 0);
}

void loadTimeBase() {
  prefs.begin("oxysync", false);
  uint32_t saved = prefs.getUInt("timebase", 0);
  prefs.end();
  if (saved > 1700000000UL) {
    // Approximate: assume device has just booted (millis()≈0)
    timeOffset = (int32_t)saved;
  }
}

void saveTimeBase() {
  uint32_t now = getUnixTime();
  if (now > 1700000000UL) {
    prefs.begin("oxysync", false);
    prefs.putUInt("timebase", now);
    prefs.end();
  }
}

// =====================
// EVENT LOG (LittleFS, /log.txt)
// Max 300 lines; trimmed to 200 when exceeded.
// Timestamps from browser sync — fallback shows uptime seconds.
// =====================
const int MAX_LOG_LINES = 300;
const int TRIM_LOG_TO   = 200;

void trimLog() {
  File f = LittleFS.open("/log.txt", "r");
  if (!f) return;

  // Count lines
  int total = 0;
  while (f.available()) {
    if (f.read() == '\n') total++;
  }
  f.close();

  if (total <= MAX_LOG_LINES) return;

  // Skip oldest (total - TRIM_LOG_TO) lines; copy rest to temp file
  f = LittleFS.open("/log.txt", "r");
  if (!f) return;
  int toSkip = total - TRIM_LOG_TO;
  int skipped = 0;
  while (f.available() && skipped < toSkip) {
    if (f.read() == '\n') skipped++;
  }

  File tmp = LittleFS.open("/log_tmp.txt", "w");
  if (!tmp) { f.close(); return; }
  uint8_t buf[64];
  while (f.available()) {
    int n = f.read(buf, sizeof(buf));
    tmp.write(buf, n);
  }
  f.close();
  tmp.close();

  LittleFS.remove("/log.txt");
  LittleFS.rename("/log_tmp.txt", "/log.txt");
}

void logEvent(const String& msg) {
  Serial.println("[LOG] " + msg);
  if (!fsReady) return;

  File f = LittleFS.open("/log.txt", "a");
  if (!f) return;
  f.println(String(getUnixTime()) + "|" + msg);
  f.close();

  // Periodic trim
  static uint16_t writeCount = 0;
  if (++writeCount >= 20) {
    writeCount = 0;
    trimLog();
  }
}

// =====================
// AUTH CHECK
// =====================
bool checkAuth() {
  if (!server.authenticate(www_user, www_pass)) {
    server.requestAuthentication(BASIC_AUTH, "OxySync");
    return false;
  }
  return true;
}

// =====================
// REAL I2C SENSOR READ (Atlas Scientific ENO-02)
// =====================
String sendToSensor(const String& cmd, int delayAfter = 900) {
  Wire.beginTransmission(EZO_O2_ADDRESS);
  Wire.write((const uint8_t*)cmd.c_str(), cmd.length());
  uint8_t err = Wire.endTransmission();

  if (err != 0) {
    Serial.println("[SENSOR] I2C error: " + String(err));
    return "";
  }

  delay(delayAfter);

  int bytesRead = Wire.requestFrom(EZO_O2_ADDRESS, sizeof(o2_concentration) - 1);
  if (bytesRead == 0) {
    Serial.println("[SENSOR] No bytes received");
    return "";
  }

  int index = 0;
  while (Wire.available() && index < (int)sizeof(o2_concentration) - 1) {
    char c = Wire.read();
    if (isPrintable(c)) {
      o2_concentration[index++] = c;
    }
  }
  o2_concentration[index] = '\0';

  String result = String(o2_concentration);
  result.trim();
  return result;
}

// =====================
// RELAY CONTROL (ACTIVE HIGH — DFRobot MOSFET DFR0457)
// Logs ON/OFF only when state actually changes.
// =====================
void setRelay(bool on) {
  if (on == state.relayOn) return; // no state change

  if (on) {
    digitalWrite(RELAY_PIN, HIGH);
    state.relayOn       = true;
    state.lockoutActive = false;
    state.lockoutStart  = 0;
    logEvent(state.manualOverride ? "RELAY ON (MANUAL)" : "RELAY ON (AUTO)");
  } else {
    digitalWrite(RELAY_PIN, LOW);
    state.relayOn = false;
    logEvent(state.manualOverride ? "RELAY OFF (MANUAL)" : "RELAY OFF (AUTO)");
  }
}

// =====================
// LOCKOUT
// =====================
void startLockout() {
  state.lockoutActive = true;
  state.lockoutStart  = millis();
  logEvent("LOCKOUT START");
}

bool lockoutExpired() {
  if (!state.lockoutActive) return true;
  if (millis() - state.lockoutStart >= lockoutTime) {
    state.lockoutActive = false;
    logEvent("LOCKOUT END");
    return true;
  }
  return false;
}

unsigned long lockoutRemaining() {
  if (!state.lockoutActive) return 0;
  if (lockoutExpired())     return 0;
  return lockoutTime - (millis() - state.lockoutStart);
}

// =====================
// CONTROL LOGIC
// =====================
void evaluateRelay() {
  if (state.manualOverride) {
    setRelay(state.manualRelayState);
    return;
  }

  if (state.o2Value < state.setpoint) {
    if (lockoutExpired()) {
      setRelay(true);
      state.relayAutoActive = true;
    }
  } else {
    if (state.relayOn && state.relayAutoActive) {
      startLockout();
    }
    state.relayAutoActive = false;
    setRelay(false);
  }
}

// =====================
// REAL SENSOR READ
// =====================
void readSensor() {
  if (state.sensorSleeping) {
    sensorStatus = SENSOR_SLEEPING;
    return;
  }

  String raw = sendToSensor("R", 900);
  Serial.println("RAW: " + raw);

  // --- Disconnection check ---
  if (raw.length() == 0) {
    if (!sensorWasDisconnected) {
      logEvent("SENSOR DISCONNECTED");
      sensorWasDisconnected = true;
    }
    sensorStatus = SENSOR_DISCONNECTED;
    stuckCount   = 0;
    return;
  }

  if (sensorWasDisconnected) {
    logEvent("SENSOR RECONNECTED");
    sensorWasDisconnected = false;
  }

  // --- Parse value ---
  float val = 0.0;
  bool parsed = false;
  for (int i = 0; i < (int)raw.length(); i++) {
    if (isDigit(raw[i]) || raw[i] == '.') {
      val = raw.substring(i).toFloat();
      parsed = true;
      break;
    }
  }

  if (!parsed || val < 0.1f || val > 50.0f) {
    sensorStatus = SENSOR_DISCONNECTED;
    return;
  }

  // --- Stuck detection ---
  if (lastValidO2 >= 0.0f && fabs(val - lastValidO2) < 0.001f) {
    if (stuckCount < 255) stuckCount++;
    if (stuckCount == STUCK_THRESHOLD) {
      logEvent("SENSOR STUCK");
      sensorWasStuck = true;
    }
  } else {
    if (sensorWasStuck) {
      logEvent("SENSOR RECOVERED");
      sensorWasStuck = false;
    }
    stuckCount   = 0;
    lastValidO2  = val;
  }

  if (stuckCount >= STUCK_THRESHOLD) {
    sensorStatus  = SENSOR_STUCK;
    state.o2Value = val;
    return;
  }

  state.o2Value = val;
  Serial.println("O2: " + String(state.o2Value, 2) + "%");

  evaluateRelay();
  updateDriftTracking(val); // may update sensorStatus via recalcDriftStatus()
  lastValidO2 = val;
}

// =====================
// LOG PAGE HTML
// =====================
const char log_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>OxySync — Event Log</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
*, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }
body {
  font-family: 'Segoe UI', system-ui, Arial, sans-serif;
  background: #eef2f7; color: #1c3557; min-height: 100vh;
}
header {
  background: #1c3557; color: #fff;
  padding: 0 20px; height: 52px;
  display: flex; align-items: center; justify-content: space-between;
  border-bottom: 2px solid #152a45; flex-shrink: 0;
}
.logo { font-size: 19px; font-weight: 800; }
.logo em { font-style: normal; color: #7dd3fc; }
.back-link {
  font-size: 11px; color: #93b8d8; text-decoration: none;
  font-weight: 600; letter-spacing: 0.3px;
}
.back-link:hover { color: #fff; }
.container { max-width: 900px; margin: 0 auto; padding: 16px; }
.toolbar {
  display: flex; align-items: center;
  justify-content: space-between; margin-bottom: 14px;
}
.toolbar h2 { font-size: 14px; font-weight: 700; color: #1c3557; }
.btn-row { display: flex; gap: 8px; }
button {
  padding: 8px 14px; font-size: 11px; font-weight: 700;
  border: none; border-radius: 6px; cursor: pointer; color: #fff;
  transition: filter 0.12s;
}
.b-red   { background: #c0392b; }
.b-ghost { background: transparent; color: #5b718a; border: 1px solid #cfd9e6; }
.b-red:hover   { filter: brightness(1.12); }
.b-ghost:hover { color: #1c3557; border-color: #5b718a; }
.log-card {
  background: #fff; border: 1px solid #cfd9e6;
  border-radius: 10px; overflow: hidden;
}
.log-head {
  display: grid; grid-template-columns: 180px 1fr;
  padding: 8px 16px; background: #eef2f7;
  border-bottom: 1px solid #cfd9e6;
  font-size: 9px; font-weight: 700;
  letter-spacing: 1.5px; text-transform: uppercase; color: #5b718a;
}
.log-row {
  display: grid; grid-template-columns: 180px 1fr;
  padding: 10px 16px; border-bottom: 1px solid #eef2f7;
  align-items: center;
}
.log-row:last-child { border-bottom: none; }
.log-time { font-size: 11px; color: #5b718a; }
.log-msg  { font-size: 12px; font-weight: 700; }
.c-green  { color: #166534; }
.c-red    { color: #b91c1c; }
.c-purple { color: #5b21b6; }
.c-amber  { color: #92400e; }
.c-blue   { color: #1e3a8a; }
.c-gray   { color: #64748b; }
.empty { padding: 32px 16px; text-align: center; color: #5b718a; font-size: 13px; }
.count-note { font-size: 11px; color: #5b718a; margin-bottom: 10px; }
</style>
</head>
<body>
<header>
  <div style="display:flex;align-items:baseline;gap:12px;">
    <span class="logo"><em>Oxy</em>Sync</span>
    <span style="font-size:11px;color:#93b8d8;">Event Log</span>
  </div>
  <a href="/" class="back-link">&#8592; Dashboard</a>
</header>

<div class="container">
  <div class="toolbar">
    <h2 id="logTitle">Event History</h2>
    <div class="btn-row">
      <button class="b-ghost" onclick="loadLog()">&#8635; Refresh</button>
      <button class="b-red"   onclick="clearLog()">&#128465; Clear Log</button>
    </div>
  </div>
  <div class="log-card" id="logCard">
    <div class="empty">Loading&hellip;</div>
  </div>
</div>

<script>
function msgColor(msg) {
  if (msg.indexOf('RELAY ON')   >= 0) return 'c-green';
  if (msg.indexOf('RELAY OFF')  >= 0) return 'c-red';
  if (msg.indexOf('LOCKOUT')    >= 0) return 'c-purple';
  if (msg.indexOf('SENSOR')     >= 0) return 'c-amber';
  if (msg.indexOf('CALIBRAT')   >= 0) return 'c-amber';
  if (msg.indexOf('REBOOT')     >= 0) return 'c-blue';
  if (msg.indexOf('BOOT')       >= 0) return 'c-blue';
  if (msg.indexOf('MANUAL')     >= 0) return 'c-gray';
  if (msg.indexOf('MODE')       >= 0) return 'c-gray';
  return 'c-blue';
}

function fmtTime(ts) {
  if (ts > 1700000000) {
    return new Date(ts * 1000).toLocaleString();
  }
  var h = Math.floor(ts / 3600);
  var m = Math.floor((ts % 3600) / 60);
  var s = ts % 60;
  return 'Uptime ' + h + 'h ' + (m<10?'0':'') + m + 'm ' + (s<10?'0':'') + s + 's';
}

function loadLog() {
  fetch('/log')
    .then(function(r) { return r.json(); })
    .then(function(events) {
      document.getElementById('logTitle').innerText =
        'Event History (' + events.length + ' entries)';
      if (events.length === 0) {
        document.getElementById('logCard').innerHTML =
          '<div class="empty">No events logged yet.</div>';
        return;
      }
      var html = '<div class="log-head"><span>Timestamp</span><span>Event</span></div>';
      for (var i = events.length - 1; i >= 0; i--) {
        var e = events[i];
        html += '<div class="log-row">';
        html += '<span class="log-time">' + fmtTime(e.ts) + '</span>';
        html += '<span class="log-msg ' + msgColor(e.msg) + '">' + e.msg + '</span>';
        html += '</div>';
      }
      document.getElementById('logCard').innerHTML = html;
    })
    .catch(function() {
      document.getElementById('logCard').innerHTML =
        '<div class="empty">Failed to load log.</div>';
    });
}

function clearLog() {
  if (!confirm('Clear all log entries? This cannot be undone.')) return;
  fetch('/clearlog').then(function() { loadLog(); });
}

loadLog();
setInterval(loadLog, 15000);
</script>
</body>
</html>
)rawliteral";

// =====================
// MAIN DASHBOARD HTML
// =====================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>OxySync</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
*, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

:root {
  --bg:       #eef2f7;
  --surface:  #ffffff;
  --header:   #1c3557;
  --border:   #cfd9e6;
  --text:     #1c3557;
  --sub:      #5b718a;
  --divider:  #e4eaf2;

  --c-red:    #b91c1c;
  --c-green:  #166534;
  --c-amber:  #92400e;
  --c-blue:   #1e3a8a;
  --c-purple: #5b21b6;
  --c-gray:   #64748b;
}

html, body { height: 100%; overflow: hidden; }

body {
  font-family: 'Segoe UI', system-ui, Arial, sans-serif;
  background: var(--bg);
  color: var(--text);
  display: flex;
  flex-direction: column;
  height: 100vh;
}

header {
  background: var(--header);
  color: #fff;
  padding: 0 20px;
  height: 52px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  flex-shrink: 0;
  border-bottom: 2px solid #152a45;
}
.header-left { display: flex; align-items: baseline; gap: 10px; }
.logo { font-size: 19px; font-weight: 800; letter-spacing: 0.3px; color: #fff; }
.logo em { font-style: normal; color: #7dd3fc; }
.header-sub { font-size: 11px; color: #93b8d8; font-weight: 400; }
.status-pill {
  font-size: 11px; font-weight: 600; letter-spacing: 0.8px;
  text-transform: uppercase; color: #86efac;
  background: rgba(134,239,172,0.12);
  border: 1px solid rgba(134,239,172,0.25);
  padding: 4px 12px; border-radius: 999px;
}

main {
  flex: 1; display: grid;
  grid-template-columns: 3fr 2fr;
  gap: 14px; padding: 14px;
  overflow: hidden; min-height: 0;
}

.panel { display: flex; flex-direction: column; gap: 12px; min-height: 0; }

.card {
  background: var(--surface);
  border: 1px solid var(--border);
  border-radius: 10px; padding: 16px 18px;
}
.card-fill { flex: 1; min-height: 0; display: flex; flex-direction: column; }
.card-label {
  font-size: 9px; font-weight: 700;
  letter-spacing: 2px; text-transform: uppercase; color: var(--sub);
}
.divider { border: none; border-top: 1px solid var(--divider); margin: 12px 0; }

.o2-section {
  flex: 1; display: flex;
  flex-direction: column; align-items: center; justify-content: center;
}
.o2-number {
  font-size: clamp(72px, 10vw, 108px);
  font-weight: 800; line-height: 1;
  letter-spacing: -3px; color: var(--text);
}
.o2-suffix {
  font-size: clamp(22px, 3vw, 32px); font-weight: 300;
  color: var(--sub); margin-left: 4px;
  vertical-align: bottom; position: relative; bottom: 10px;
}
.o2-caption {
  font-size: 10px; letter-spacing: 2px;
  text-transform: uppercase; color: var(--sub); margin-top: 6px;
}

.sp-row {
  display: flex; align-items: center; justify-content: space-between;
  background: var(--bg); border: 1px solid var(--border);
  border-radius: 7px; padding: 9px 14px; margin-top: 10px;
}
.sp-lbl { font-size: 11px; font-weight: 600; color: var(--sub); }
.sp-val { font-size: 16px; font-weight: 700; color: var(--text); }

.status-grid {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 8px; margin-top: 10px;
}
.stat {
  background: var(--bg); border: 1px solid var(--border);
  border-radius: 7px; padding: 10px 8px; text-align: center;
}
.stat-lbl {
  font-size: 8px; font-weight: 700;
  letter-spacing: 1.5px; text-transform: uppercase;
  color: var(--sub); margin-bottom: 5px;
}
.stat-val { font-size: 13px; font-weight: 800; letter-spacing: 0.2px; }
.stat-sub { font-size: 9px; color: var(--sub); margin-top: 3px; }

.cv-red    { color: var(--c-red); }
.cv-green  { color: var(--c-green); }
.cv-amber  { color: var(--c-amber); }
.cv-blue   { color: var(--c-blue); }
.cv-purple { color: var(--c-purple); }
.cv-gray   { color: var(--c-gray); }

.btn-group { display: grid; gap: 8px; }
.btn-group-3 { grid-template-columns: 1fr 1fr 1fr; }
.btn-group-2 { grid-template-columns: 1fr 1fr; }
.btn-group-1 { grid-template-columns: 1fr; }

button {
  padding: 11px 6px; font-size: 12px; font-weight: 700;
  letter-spacing: 0.4px; border: none; border-radius: 7px;
  cursor: pointer; color: #fff;
  transition: filter 0.12s, transform 0.08s; white-space: nowrap;
}
button:active { transform: scale(0.97); }

.b-red    { background: #c0392b; }
.b-green  { background: #1e6b38; }
.b-blue   { background: #1e3a8a; }
.b-amber  { background: #92400e; }
.b-slate  { background: #374151; }
.b-teal   { background: #0f766e; }
.b-ghost  { background: transparent; color: var(--sub); border: 1px solid var(--border); }

.b-red:hover    { filter: brightness(1.12); }
.b-green:hover  { filter: brightness(1.12); }
.b-blue:hover   { filter: brightness(1.15); }
.b-amber:hover  { filter: brightness(1.12); }
.b-slate:hover  { filter: brightness(1.15); }
.b-teal:hover   { filter: brightness(1.12); }
.b-ghost:hover  { color: var(--text); border-color: var(--sub); }
</style>
</head>
<body>

<header>
  <div class="header-left">
    <span class="logo"><em>Oxy</em>Sync</span>
    <span class="header-sub">Oxygen Safety Controller</span>
  </div>
  <div class="status-pill">Online</div>
</header>

<main>

  <!-- LEFT — O2 reading + status -->
  <div class="panel">
    <div class="card card-fill">
      <div class="card-label">Oxygen Level</div>

      <div class="o2-section">
        <div>
          <span class="o2-number" id="o2">--</span><span class="o2-suffix">%</span>
        </div>
        <div class="o2-caption">Ambient O&#8322; Concentration</div>
      </div>

      <div class="sp-row">
        <span class="sp-lbl">Cutoff Setpoint</span>
        <span class="sp-val"><span id="sp">--</span>%</span>
      </div>

      <div class="status-grid">
        <div class="stat">
          <div class="stat-lbl">Relay</div>
          <div class="stat-val" id="relay">--</div>
        </div>
        <div class="stat">
          <div class="stat-lbl">Mode</div>
          <div class="stat-val" id="mode">--</div>
        </div>
        <div class="stat">
          <div class="stat-lbl">Lockout</div>
          <div class="stat-val" id="lock">--</div>
        </div>
        <div class="stat">
          <div class="stat-lbl">Sensor</div>
          <div class="stat-val" id="sensorSt">--</div>
          <div class="stat-sub" id="sensorSub">--</div>
        </div>
      </div>
    </div>
  </div>

  <!-- RIGHT — Controls -->
  <div class="panel">

    <div class="card">
      <div class="card-label">Relay Control</div>
      <hr class="divider">
      <div class="btn-group btn-group-3">
        <button class="b-green" onclick="relayOn()">&#9650; ON</button>
        <button class="b-red"   onclick="relayOff()">&#9660; OFF</button>
        <button class="b-blue"  onclick="relayAuto()">&#8635; AUTO</button>
      </div>
    </div>

    <div class="card">
      <div class="card-label">Sensor</div>
      <hr class="divider">
      <div class="btn-group btn-group-1">
        <button class="b-amber" onclick="calibrate()">&#9672; Calibrate &amp; Reset Drift</button>
      </div>
    </div>

    <div class="card">
      <div class="card-label">System</div>
      <hr class="divider">
      <div class="btn-group btn-group-2" style="margin-bottom:8px;">
        <button class="b-slate" onclick="rebootController()">&#8635; Reboot</button>
        <button class="b-ghost" onclick="logout()">&#8594; Logout</button>
      </div>
      <div class="btn-group btn-group-1">
        <button class="b-teal" onclick="window.location.href='/logpage'">&#9776; View Event Log</button>
      </div>
    </div>

  </div>

</main>

<script>
// Sync browser time to device on every load
fetch('/synctime?ts=' + Math.floor(Date.now() / 1000)).catch(function(){});

function update() {
  fetch('/status')
    .then(function(r) { return r.json(); })
    .then(function(d) {
      document.getElementById('o2').innerText = d.o2.toFixed(2);
      document.getElementById('sp').innerText = d.setpoint.toFixed(2);

      var rel = document.getElementById('relay');
      rel.innerText = d.relay ? 'ON' : 'OFF';
      rel.className = 'stat-val ' + (d.relay ? 'cv-red' : 'cv-green');

      var mod = document.getElementById('mode');
      mod.innerText = d.manual ? 'MANUAL' : 'AUTO';
      mod.className = 'stat-val ' + (d.manual ? 'cv-amber' : 'cv-blue');

      var lck = document.getElementById('lock');
      if (d.lockout > 0) {
        var m = Math.floor(d.lockout / 60);
        var s = d.lockout % 60;
        lck.innerText = m + 'm ' + (s < 10 ? '0' : '') + s + 's';
        lck.className = 'stat-val cv-purple';
      } else {
        lck.innerText = 'None';
        lck.className = 'stat-val cv-gray';
      }

      var sns = document.getElementById('sensorSt');
      var sub = document.getElementById('sensorSub');
      var st  = d.sensorSt;
      if (st === 0) {
        sns.innerText = 'COLLECT';
        sns.className = 'stat-val cv-gray';
        sub.innerText = d.driftDays + 'd data';
      } else if (st === 1) {
        sns.innerText = 'OK';
        sns.className = 'stat-val cv-green';
        sub.innerText = drift(d.driftPct) + ' / ' + d.driftDays + 'd';
      } else if (st === 2) {
        sns.innerText = 'CAUTION';
        sns.className = 'stat-val cv-amber';
        sub.innerText = drift(d.driftPct) + ' — recal.';
      } else if (st === 3) {
        sns.innerText = 'REPLACE';
        sns.className = 'stat-val cv-red';
        sub.innerText = drift(d.driftPct) + ' — replace';
      } else if (st === 4) {
        sns.innerText = 'NO SENSOR';
        sns.className = 'stat-val cv-red';
        sub.innerText = 'check wiring';
      } else if (st === 5) {
        sns.innerText = 'STUCK';
        sns.className = 'stat-val cv-red';
        sub.innerText = 'same reading 1+ min';
      } else if (st === 6) {
        sns.innerText = 'SLEEPING';
        sns.className = 'stat-val cv-gray';
        sub.innerText = 'sensor in sleep';
      }
    })
    .catch(function() {});
}

function drift(v) {
  return (v >= 0 ? '+' : '') + parseFloat(v).toFixed(2) + '%';
}

function calibrate() {
  if (!confirm('Calibrate sensor? This will also reset drift history.')) return;
  fetch('/calibrate')
    .then(function(r) { return r.text(); })
    .then(function(t) { alert(t); update(); });
}

function relayOn()   { fetch('/relay?state=on').then(update); }
function relayOff()  { fetch('/relay?state=off').then(update); }
function relayAuto() { fetch('/relay?state=auto').then(update); }

function rebootController() {
  if (!confirm('Reboot the controller?')) return;
  document.body.style.cssText = 'display:flex;align-items:center;justify-content:center;height:100vh;background:#eef2f7;font-family:Segoe UI,Arial,sans-serif;color:#1c3557;flex-direction:column;gap:12px;';
  document.body.innerHTML = '<div style="font-size:36px;">&#8635;</div><h2 style="font-size:18px;font-weight:700;">Rebooting&#8230;</h2><p style="color:#5b718a;font-size:13px;">Please wait. Page will reload automatically.</p>';
  fetch('/reboot').catch(function(){});
  var attempts = 0;
  function pollBack() {
    attempts++;
    if (attempts > 30) {
      document.body.innerHTML = '<h2 style="color:#b91c1c;">Device Not Responding</h2><p style="color:#5b718a;margin-top:8px;font-size:13px;">Please check the device and <a href="/" style="color:#1e3a8a;">try again</a>.</p>';
      return;
    }
    fetch('/status').then(function(r) {
      if (r.ok) { window.location.href = '/'; }
      else { setTimeout(pollBack, 3000); }
    }).catch(function() { setTimeout(pollBack, 3000); });
  }
  setTimeout(pollBack, 5000);
}

function logout() { window.location.href = '/logout'; }

setInterval(update, 2000);
update();
</script>

</body>
</html>
)rawliteral";

// =====================
// WEB HANDLERS
// =====================
void handleRoot() {
  if (!checkAuth()) return;
  server.send_P(200, "text/html", index_html);
}

void handleStatus() {
  if (!checkAuth()) return;

  String json = "{";
  json += "\"o2\":"        + String(state.o2Value, 2)                         + ",";
  json += "\"setpoint\":"  + String(state.setpoint, 2)                        + ",";
  json += "\"relay\":"     + String(state.relayOn ? "true" : "false")         + ",";
  json += "\"lockout\":"   + String(lockoutRemaining() / 1000)                + ",";
  json += "\"manual\":"    + String(state.manualOverride ? "true" : "false")  + ",";
  json += "\"sensorSt\":"  + String(sensorStatus)                             + ",";
  json += "\"driftPct\":"  + String(currentDrift, 2)                          + ",";
  json += "\"driftDays\":" + String(driftData.count);
  json += "}";

  server.send(200, "application/json", json);
}

void handleCalibrate() {
  if (!checkAuth()) return;
  sendToSensor("CAL,20.9", 2500);
  delay(1500);
  String verify = sendToSensor("R", 900);
  resetDrift();
  logEvent("SENSOR CALIBRATED");
  server.send(200, "text/plain", "Calibration sent. Drift history reset. Reading: " + verify);
}

void handleRelay() {
  if (!checkAuth()) return;

  String cmd = server.arg("state");

  if (cmd == "on") {
    state.manualOverride   = true;
    state.manualRelayState = true;
    state.relayAutoActive  = false;
    manualStartTime        = millis();
    setRelay(true);
    server.send(200, "text/plain", "Relay ON");
  }
  else if (cmd == "off") {
    state.manualOverride   = true;
    state.manualRelayState = false;
    state.relayAutoActive  = false;
    manualStartTime        = millis();
    setRelay(false);
    server.send(200, "text/plain", "Relay OFF");
  }
  else if (cmd == "auto") {
    state.manualOverride = false;
    logEvent("MODE: AUTO");
    evaluateRelay();
    server.send(200, "text/plain", "AUTO MODE");
  }
  else {
    server.send(400, "text/plain", "Bad Command");
  }
}

void handleReboot() {
  if (!checkAuth()) return;
  logEvent("REBOOT CMD");
  saveTimeBase();
  pendingReboot = true;
  rebootAt      = millis() + 500;
  server.send(200, "text/plain", "Rebooting...");
}

void handleLogout() {
  server.sendHeader("WWW-Authenticate", "Basic realm=\"OxySync\"");
  server.send(401, "text/html",
    "<!DOCTYPE html><html><head><title>Logged Out</title></head><body>"
    "<h2>Logged Out</h2>"
    "<p>You have been logged out of OxySync.</p>"
    "<p><a href='/'>Click here to log in again</a></p>"
    "</body></html>"
  );
}

// Browser syncs Unix time on every dashboard load
void handleSyncTime() {
  if (!checkAuth()) return;
  if (server.hasArg("ts")) {
    int32_t clientTs = (int32_t)server.arg("ts").toInt();
    timeOffset = clientTs - (int32_t)(millis() / 1000);
    saveTimeBase();
    server.send(200, "text/plain", "OK");
  } else {
    server.send(400, "text/plain", "Missing ?ts=");
  }
}

// Returns last 50 log entries as JSON array [{ts, msg}, ...]
void handleLog() {
  if (!checkAuth()) return;

  if (!fsReady) {
    server.send(200, "application/json", "[]");
    return;
  }

  File f = LittleFS.open("/log.txt", "r");
  if (!f) {
    server.send(200, "application/json", "[]");
    return;
  }

  const int RETURN_MAX = 50;
  String lines[RETURN_MAX];
  int total = 0;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      lines[total % RETURN_MAX] = line;
      total++;
    }
  }
  f.close();

  int count = min(total, RETURN_MAX);
  int start = (total > RETURN_MAX) ? (total % RETURN_MAX) : 0;

  String json = "[";
  bool first = true;
  for (int i = 0; i < count; i++) {
    int idx = (start + i) % RETURN_MAX;
    if (lines[idx].length() == 0) continue;
    int sep = lines[idx].indexOf('|');
    if (sep < 0) continue;
    String ts  = lines[idx].substring(0, sep);
    String msg = lines[idx].substring(sep + 1);
    msg.replace("\\", "\\\\");
    msg.replace("\"", "\\\"");
    if (!first) json += ",";
    json += "{\"ts\":" + ts + ",\"msg\":\"" + msg + "\"}";
    first = false;
  }
  json += "]";

  server.send(200, "application/json", json);
}

void handleLogPage() {
  if (!checkAuth()) return;
  server.send_P(200, "text/html", log_html);
}

void handleClearLog() {
  if (!checkAuth()) return;
  if (fsReady) LittleFS.remove("/log.txt");
  logEvent("LOG CLEARED");
  server.send(200, "text/plain", "Log cleared.");
}

// =====================
// SETUP
// =====================
void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);

  // Watchdog — API changed in ESP-IDF v5 (Arduino core v3)
#if ESP_IDF_VERSION_MAJOR >= 5
  {
    esp_task_wdt_config_t wdt_cfg = {
      .timeout_ms     = WDT_TIMEOUT_SEC * 1000,
      .idle_core_mask = 0,
      .trigger_panic  = true
    };
    esp_task_wdt_reconfigure(&wdt_cfg);
  }
#else
  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
#endif
  esp_task_wdt_add(NULL);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // OFF (active-high)

  // Filesystem — needed for event log
  fsReady = LittleFS.begin(true);
  if (!fsReady) Serial.println("[FS] LittleFS mount failed — log disabled");

  loadDrift();
  loadTimeBase();
  lastDayMs = millis();
  recalcDriftStatus();

  WiFi.softAP(ap_ssid, ap_password);
  Serial.println("WiFi AP: " + String(ap_ssid));
  Serial.println("Dashboard: http://" + WiFi.softAPIP().toString());
  Serial.println("Drift days loaded: " + String(driftData.count));

  // Log boot reason
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  logEvent("BOOT (POWER ON)");    break;
    case ESP_RST_SW:       logEvent("REBOOT (SOFTWARE)");  break;
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      logEvent("REBOOT (WATCHDOG)");  break;
    case ESP_RST_BROWNOUT: logEvent("REBOOT (BROWNOUT)");  break;
    default:               logEvent("BOOT");                break;
  }

  server.on("/",          handleRoot);
  server.on("/status",    handleStatus);
  server.on("/calibrate", handleCalibrate);
  server.on("/relay",     handleRelay);
  server.on("/reboot",    handleReboot);
  server.on("/logout",    handleLogout);
  server.on("/synctime",  handleSyncTime);
  server.on("/log",       handleLog);
  server.on("/logpage",   handleLogPage);
  server.on("/clearlog",  handleClearLog);

  server.begin();
  Serial.println("Web server started.");
}

// =====================
// LOOP
// =====================
void loop() {
  esp_task_wdt_reset();
  server.handleClient();

  // Deferred reboot — ensures HTTP response is fully sent before restart
  if (pendingReboot && millis() >= rebootAt) {
    ESP.restart();
  }

  // Save approximate time to NVS every 10 minutes so post-reboot
  // log entries have reasonable timestamps before next browser sync
  if (millis() - lastTimeSave >= timeSaveInterval) {
    saveTimeBase();
    lastTimeSave = millis();
  }

  // Manual override timeout
  if (state.manualOverride) {
    if (millis() - manualStartTime >= manualTimeout) {
      Serial.println("Manual override timeout — returning to AUTO");
      state.manualOverride = false;
      manualStartTime = 0;
      logEvent("MANUAL TIMEOUT -> AUTO");
      evaluateRelay();
    }
  }

  // Periodic sensor read
  if (millis() - lastReadTime >= readInterval) {
    readSensor();
    lastReadTime = millis();
  }
}
