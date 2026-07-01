// =====================================================================
// OxySync — PRODUCTION VERSION
// Megasoft | Client: Fred Faiz
//
// For real hardware: ESP32 Devkit V1 + Atlas Scientific ENO-02 (I2C)
// Flash this version to the actual device.
// =====================================================================

#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_task_wdt.h>

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

// Watchdog: reboot if loop stalls longer than this (sensor I2C hang, etc.)
const uint32_t WDT_TIMEOUT_SEC = 30;

bool pendingReboot = false;
unsigned long rebootAt = 0;

// =====================
// SENSOR BUFFER
// =====================
char o2_concentration[32];

// =====================
// SENSOR DRIFT TRACKING
// =====================
const float DRIFT_CAUTION_PCT = 0.30f; // ±0.3% — recalibrate soon
const float DRIFT_REPLACE_PCT = 0.60f; // ±0.6% — replace sensor

#define SENSOR_COLLECTING 0
#define SENSOR_OK         1
#define SENSOR_CAUTION    2
#define SENSOR_REPLACE    3

// Only completed daily averages and the reference are persisted to NVS.
// Mid-day accumulators live in RAM and reset naturally on reboot.
struct DriftData {
  float   daily[30];  // circular buffer of daily O2 averages
  uint8_t count;      // how many daily slots are filled (0–30)
  uint8_t dayIndex;   // next write position
  float   refAvg;     // first day's average — baseline after calibration
  bool    refSet;     // whether the reference has been established
};

DriftData driftData;

float    driftDaySum     = 0.0f;  // accumulator for current partial day
uint16_t driftDaySamples = 0;     // samples counted today

unsigned long lastDayMs    = 0;
float         currentDrift = 0.0f;
uint8_t       sensorStatus = SENSOR_COLLECTING;

const unsigned long DAY_MS = 86400000UL; // 24 hours

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
  sensorStatus     = SENSOR_COLLECTING;
  saveDrift();
  Serial.println("[DRIFT] History reset after calibration.");
}

void recalcDriftStatus() {
  if (!driftData.refSet || driftData.count < 2) {
    sensorStatus = SENSOR_COLLECTING;
    currentDrift = 0.0f;
    return;
  }

  // Average of the most recent 3 completed days (or fewer if not enough data)
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

      // First completed day becomes the reference baseline
      if (!driftData.refSet) {
        driftData.refAvg = dayAvg;
        driftData.refSet = true;
      }

      driftData.daily[driftData.dayIndex] = dayAvg;
      driftData.dayIndex = (driftData.dayIndex + 1) % 30;
      if (driftData.count < 30) driftData.count++;

      recalcDriftStatus();
      saveDrift();

      Serial.println("[DRIFT] Day average: " + String(dayAvg, 2) +
                     "% | Drift: " + String(currentDrift, 2) +
                     "% | Days: " + String(driftData.count));
    }

    driftDaySum     = 0.0f;
    driftDaySamples = 0;
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
// Based on client's reference: Sensor script_new.txt
// =====================
String sendToSensor(const String& cmd, int delayAfter = 900) {
  Wire.beginTransmission(EZO_O2_ADDRESS);
  Wire.write((const uint8_t*)cmd.c_str(), cmd.length());
  Wire.endTransmission();

  delay(delayAfter);

  Wire.requestFrom(EZO_O2_ADDRESS, sizeof(o2_concentration) - 1);
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
// =====================
void setRelay(bool on) {
  if (on) {
    digitalWrite(RELAY_PIN, HIGH);
    state.relayOn       = true;
    state.lockoutActive = false;
    state.lockoutStart  = 0;
  } else {
    digitalWrite(RELAY_PIN, LOW);
    state.relayOn = false;
  }
}

// =====================
// LOCKOUT
// =====================
void startLockout() {
  state.lockoutActive = true;
  state.lockoutStart  = millis();
}

bool lockoutExpired() {
  if (!state.lockoutActive) return true;
  if (millis() - state.lockoutStart >= lockoutTime) {
    state.lockoutActive = false;
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
    // Only start lockout if AUTO was the one that turned the relay on
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
  if (state.sensorSleeping) return;

  String raw = sendToSensor("R", 900);
  Serial.println("RAW: " + raw);

  float val = 0.0;
  for (int i = 0; i < (int)raw.length(); i++) {
    if (isDigit(raw[i]) || raw[i] == '.') {
      val = raw.substring(i).toFloat();
      break;
    }
  }

  state.o2Value = val;
  Serial.println("O2: " + String(state.o2Value, 2) + "%");

  evaluateRelay();
  updateDriftTracking(val);
}

// =====================
// HTML DASHBOARD
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

/* ── HEADER ── */
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

/* ── MAIN GRID ── */
main {
  flex: 1; display: grid;
  grid-template-columns: 3fr 2fr;
  gap: 14px; padding: 14px;
  overflow: hidden; min-height: 0;
}

/* ── PANELS ── */
.panel { display: flex; flex-direction: column; gap: 12px; min-height: 0; }

/* ── CARDS ── */
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

/* ── O2 DISPLAY ── */
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

/* ── SETPOINT ROW ── */
.sp-row {
  display: flex; align-items: center; justify-content: space-between;
  background: var(--bg); border: 1px solid var(--border);
  border-radius: 7px; padding: 9px 14px; margin-top: 10px;
}
.sp-lbl { font-size: 11px; font-weight: 600; color: var(--sub); }
.sp-val { font-size: 16px; font-weight: 700; color: var(--text); }

/* ── STATUS GRID (2x2) ── */
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

/* Status colors */
.cv-red    { color: var(--c-red); }
.cv-green  { color: var(--c-green); }
.cv-amber  { color: var(--c-amber); }
.cv-blue   { color: var(--c-blue); }
.cv-purple { color: var(--c-purple); }
.cv-gray   { color: var(--c-gray); }

/* ── BUTTONS ── */
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
.b-ghost  { background: transparent; color: var(--sub); border: 1px solid var(--border); }

.b-red:hover    { filter: brightness(1.12); }
.b-green:hover  { filter: brightness(1.12); }
.b-blue:hover   { filter: brightness(1.15); }
.b-amber:hover  { filter: brightness(1.12); }
.b-slate:hover  { filter: brightness(1.15); }
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
      <div class="btn-group btn-group-2">
        <button class="b-slate" onclick="rebootController()">&#8635; Reboot</button>
        <button class="b-ghost" onclick="logout()">&#8594; Logout</button>
      </div>
    </div>

  </div>

</main>

<script>
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
      } else {
        sns.innerText = 'REPLACE';
        sns.className = 'stat-val cv-red';
        sub.innerText = drift(d.driftPct) + ' — replace';
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
    evaluateRelay();
    server.send(200, "text/plain", "AUTO MODE");
  }
  else {
    server.send(400, "text/plain", "Bad Command");
  }
}

void handleReboot() {
  if (!checkAuth()) return;
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

// =====================
// SETUP
// =====================
void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);

  esp_task_wdt_init(WDT_TIMEOUT_SEC, true);
  esp_task_wdt_add(NULL);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // OFF (active-high)

  loadDrift();
  lastDayMs = millis();
  recalcDriftStatus();

  WiFi.softAP(ap_ssid, ap_password);
  Serial.println("WiFi AP: " + String(ap_ssid));
  Serial.println("Dashboard: http://" + WiFi.softAPIP().toString());
  Serial.println("Drift days loaded: " + String(driftData.count));

  server.on("/",          handleRoot);
  server.on("/status",    handleStatus);
  server.on("/calibrate", handleCalibrate);
  server.on("/relay",     handleRelay);
  server.on("/reboot",    handleReboot);
  server.on("/logout",    handleLogout);

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

  // Manual override timeout
  if (state.manualOverride) {
    if (millis() - manualStartTime >= manualTimeout) {
      Serial.println("Manual override timeout — returning to AUTO");
      state.manualOverride = false;
      manualStartTime = 0;
      evaluateRelay();
    }
  }

  // Periodic sensor read
  if (millis() - lastReadTime >= readInterval) {
    readSensor();
    lastReadTime = millis();
  }
}
