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

// =====================
// SENSOR CONFIG
// =====================
#define EZO_O2_ADDRESS 0x6C
#define SDA_PIN 21
#define SCL_PIN 22

// =====================
// RELAY CONFIG (ACTIVE LOW)
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
WebServer server(80);

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

// =====================
// SENSOR BUFFER
// =====================
char o2_concentration[32];

// =====================
// AUTH CHECK
// =====================
bool checkAuth() {
  if (!server.authenticate(www_user, www_pass)) {
    server.requestAuthentication();
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

  int len = Wire.requestFrom(EZO_O2_ADDRESS, sizeof(o2_concentration) - 1);
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
// RELAY CONTROL (ACTIVE LOW)
// =====================
void setRelay(bool on) {
  if (on) {
    digitalWrite(RELAY_PIN, LOW);
    state.relayOn       = true;
    state.lockoutActive = false;
    state.lockoutStart  = 0;
  } else {
    digitalWrite(RELAY_PIN, HIGH);
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
}

// =====================
// HTML DASHBOARD
// =====================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<title>OxySync</title>
<meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { font-family: Arial; margin: 20px; background: #f4f4f4; }
  .card { background: white; border-radius: 8px; padding: 20px; margin-bottom: 16px; box-shadow: 0 1px 4px rgba(0,0,0,0.1); }
  h1 { margin-bottom: 4px; }
  h3 { margin-top: 0; color: #555; }
  button { padding: 12px 18px; margin: 4px; font-size: 15px; border: none; border-radius: 6px; cursor: pointer; }
  .btn-primary  { background: #007bff; color: white; }
  .btn-danger   { background: #dc3545; color: white; }
  .btn-success  { background: #28a745; color: white; }
  .btn-warning  { background: #ffc107; color: black; }
  .btn-secondary{ background: #6c757d; color: white; }
  hr { margin: 16px 0; border: none; border-top: 1px solid #ddd; }
</style>
</head>
<body>

<div class="card" style="text-align:center;">
  <h1>OxySync</h1>
  <h3>Technician Interface</h3>
</div>

<div class="card">
  <p><b>O2 Reading:</b> <span id="o2">--</span> %</p>
  <p><b>Setpoint:</b> <span id="sp">--</span> %</p>
  <p><b>Relay Status:</b> <span id="relay">--</span></p>
  <p><b>Mode:</b> <span id="mode">--</span></p>
  <p><b>Lockout Remaining:</b> <span id="lock">--</span></p>
</div>

<div class="card">
  <b>Relay Control</b><hr>
  <button class="btn-success" onclick="relayOn()">Relay ON</button>
  <button class="btn-danger"  onclick="relayOff()">Relay OFF</button>
  <button class="btn-primary" onclick="relayAuto()">AUTO</button>
</div>

<div class="card">
  <b>Sensor</b><hr>
  <button class="btn-warning" onclick="calibrate()">Calibrate Sensor</button>
</div>

<div class="card">
  <b>System</b><hr>
  <button class="btn-secondary" onclick="rebootController()">Reboot Controller</button>
  <button class="btn-secondary" onclick="logout()">Logout</button>
</div>

<script>
function update() {
  fetch('/status')
    .then(r => r.json())
    .then(d => {
      document.getElementById('o2').innerText   = d.o2.toFixed(2);
      document.getElementById('sp').innerText   = d.setpoint.toFixed(2);
      document.getElementById('relay').innerText = d.relay ? "ON" : "OFF";
      document.getElementById('mode').innerText  = d.manual ? "MANUAL" : "AUTO";

      let mins = Math.floor(d.lockout / 60);
      let secs = d.lockout % 60;
      document.getElementById('lock').innerText =
        d.lockout > 0 ? mins + "m " + secs + "s" : "None";
    });
}

function calibrate() {
  fetch('/calibrate')
    .then(r => r.text())
    .then(t => alert(t));
}

function relayOn()   { fetch('/relay?state=on').then(update); }
function relayOff()  { fetch('/relay?state=off').then(update); }
function relayAuto() { fetch('/relay?state=auto').then(update); }

function rebootController() {
  if (confirm('Reboot controller?')) {
    fetch('/reboot');
  }
}

function logout() {
  fetch('/logout', { credentials: 'include' })
    .then(() => {
      window.location.href = '/';
    });
}

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
  json += "\"o2\":"       + String(state.o2Value, 2)   + ",";
  json += "\"setpoint\":" + String(state.setpoint, 2)  + ",";
  json += "\"relay\":"    + String(state.relayOn ? "true" : "false") + ",";
  json += "\"lockout\":"  + String(lockoutRemaining() / 1000)        + ",";
  json += "\"manual\":"   + String(state.manualOverride ? "true" : "false");
  json += "}";

  server.send(200, "application/json", json);
}

void handleCalibrate() {
  if (!checkAuth()) return;
  sendToSensor("CAL,20.9", 2500);
  delay(1500);
  String verify = sendToSensor("R", 900);
  server.send(200, "text/plain", "Calibration sent. Reading: " + verify);
}

void handleRelay() {
  if (!checkAuth()) return;

  String cmd = server.arg("state");

  if (cmd == "on") {
    state.manualOverride   = true;
    state.manualRelayState = true;
    state.relayAutoActive  = false; // manual action — auto tracking cleared
    manualStartTime        = millis();
    setRelay(true);
    server.send(200, "text/plain", "Relay ON");
  }
  else if (cmd == "off") {
    state.manualOverride   = true;
    state.manualRelayState = false;
    state.relayAutoActive  = false; // manual action — auto tracking cleared
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
  server.send(200, "text/plain", "Rebooting...");
  delay(500);
  ESP.restart();
}

void handleLogout() {
  server.sendHeader("WWW-Authenticate", "Basic realm=\"logout\"");
  server.send(401, "text/html",
    "<html><body><p>Logged out.</p>"
    "<p><a href='/'>Click here to log back in</a></p>"
    "</body></html>"
  );
}

// =====================
// SETUP
// =====================
void setup() {
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // OFF (active-low)

  WiFi.softAP(ap_ssid, ap_password);
  Serial.println("WiFi AP: " + String(ap_ssid));
  Serial.println("Dashboard: http://" + WiFi.softAPIP().toString());

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
  server.handleClient();

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
