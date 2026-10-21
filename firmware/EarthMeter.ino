#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <math.h>
#include <string.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
// Self-contained types: no extra header file is required.
struct Phasor { float re; float im; };
struct Win { Phasor ep; Phasor cur; float epNoise; float curNoise; bool clipped; };
static inline float mag(Phasor p) { return sqrtf(p.re * p.re + p.im * p.im); }


// ============================================================
// WIFI
// ============================================================

WebServer server(80);

// Saved Wi-Fi settings (separate from measurement history)
Preferences wifiPreferences;
String configuredSSID;
String configuredPassword;
const char* SETUP_AP_SSID = "EarthMeter_Setup";
const char* SETUP_AP_PASSWORD = "EarthMeter@123";


// ============================================================
// LOGIN
// ============================================================

const char* LOGIN_USER = "Moseta";
const char* LOGIN_PASS = "Moseta";

bool loggedIn = false;

// ============================================================
// PINS
// ============================================================

const int EP_PIN = 34;
const int CURRENT_PIN = 35;
const int SDA_PIN = 21;
const int SCL_PIN = 22;
LiquidCrystal_I2C lcd(0x27, 16, 2);

void lcdLine(int row, const char *msg) {
  char buf[17];
  snprintf(buf, sizeof(buf), "%-16.16s", msg);
  lcd.setCursor(0, row);
  lcd.print(buf);
}

// ============================================================
// AMPLIFIER / CURRENT SENSE
// ============================================================
const float VOLTAGE_GAIN = 6.4f;
const float CURRENT_GAIN = 6.4f;
const float R_SENSE = 100.0f;

// ============================================================
// 50 Hz PHASOR SAMPLING
// ============================================================
const int FS_HZ = 1000;
const int N_SAMPLES = 1000;
const int K_SIGNAL = 50;
const int K_NOISE_A = 40;
const int K_NOISE_B = 60;
const unsigned long TEST_TIME = 60000UL;
const float MIN_SNR = 3.0f;
const float MAX_PHASE_DEG = 15.0f;
const float MIN_CURRENT_mA = 0.1463f;
const float MAX_RESISTANCE = 10000.0f;
const int MAX_RESULTS = 70;

float hannW[N_SAMPLES];
float cosT[3][N_SAMPLES];
float sinT[3][N_SAMPLES];
float wSum = 0.0f;
float epBuf[N_SAMPLES];
float curBuf[N_SAMPLES];
float resistanceValues[MAX_RESULTS];
int validResistanceCount = 0;
int totalWindows = 0;

void initTables() {
  const int ks[3] = { K_SIGNAL, K_NOISE_A, K_NOISE_B };
  wSum = 0.0f;
  for (int n = 0; n < N_SAMPLES; n++) {
    hannW[n] = 0.5f - 0.5f * cosf(2.0f * PI * n / (N_SAMPLES - 1));
    wSum += hannW[n];
    for (int b = 0; b < 3; b++) {
      float a = 2.0f * PI * ks[b] * n / N_SAMPLES;
      cosT[b][n] = cosf(a);
      sinT[b][n] = sinf(a);
    }
  }
}

Win measurePhasorWindow() {
  const uint32_t per = 1000000UL / FS_HZ;
  uint32_t t0 = micros();
  double epMean = 0.0, curMean = 0.0;
  for (int n = 0; n < N_SAMPLES; n++) {
    uint32_t due = t0 + (uint32_t)n * per;
    while ((int32_t)(micros() - due) < 0) { }
    curBuf[n] = analogReadMilliVolts(CURRENT_PIN) / 1000.0f;
    epBuf[n] = analogReadMilliVolts(EP_PIN) / 1000.0f;
    curMean += curBuf[n]; epMean += epBuf[n];
  }
  float cm = curMean / N_SAMPLES, em = epMean / N_SAMPLES;
  double eR[3] = {0}, eI[3] = {0}, cR[3] = {0}, cI[3] = {0};
  for (int n = 0; n < N_SAMPLES; n++) {
    float xe = (epBuf[n] - em) * hannW[n];
    float xc = (curBuf[n] - cm) * hannW[n];
    for (int b = 0; b < 3; b++) {
      eR[b] += xe * cosT[b][n]; eI[b] -= xe * sinT[b][n];
      cR[b] += xc * cosT[b][n]; cI[b] -= xc * sinT[b][n];
    }
  }
  float scale = 2.0f / wSum;
  Win w;
  w.ep = {(float)eR[0] * scale, (float)eI[0] * scale};
  w.cur = {(float)cR[0] * scale, (float)cI[0] * scale};
  Phasor eA = {(float)eR[1] * scale, (float)eI[1] * scale};
  Phasor eB = {(float)eR[2] * scale, (float)eI[2] * scale};
  Phasor cA = {(float)cR[1] * scale, (float)cI[1] * scale};
  Phasor cB = {(float)cR[2] * scale, (float)cI[2] * scale};
  w.epNoise = 0.5f * (mag(eA) + mag(eB));
  w.curNoise = 0.5f * (mag(cA) + mag(cB));
  w.clipped = false; // clipping rejection intentionally disabled
  return w;
}

float calculateMedian(float values[], int count) {
  if (count <= 0) return 0.0f;
  float temp[MAX_RESULTS];
  for (int i = 0; i < count; i++) temp[i] = values[i];
  for (int i = 1; i < count; i++) {
    float key = temp[i]; int j = i - 1;
    while (j >= 0 && temp[j] > key) { temp[j + 1] = temp[j]; j--; }
    temp[j + 1] = key;
  }
  if (count % 2) return temp[count / 2];
  return 0.5f * (temp[count / 2 - 1] + temp[count / 2]);
}

// ============================================================
// TEST VARIABLES
// ============================================================

bool currentState = false;
bool testRunning = false;
bool testFinished = false;

int consecutiveOnWindows = 0;

unsigned long testStart = 0;
int testSecond = 0;

// Automatic cycle: start a 60-second test every 5 minutes, independent of the website.
const unsigned long AUTO_TEST_INTERVAL_MS = 5UL * 60UL * 1000UL;
unsigned long nextAutoTestAt = 0;
bool autoTestSchedulerReady = false;

// ============================================================
// LIVE VALUES
// ============================================================

float liveCurrentRMS = 0.0f;
float liveEPRMS = 0.0f;
float liveResistance = 0.0f;

bool liveResistanceValid = false;

float lastValidResistance = 0.0f;

// ============================================================
// RESISTANCE BUFFER
// ============================================================

// Phasor resistance buffer declared above

// ============================================================
// HISTORY
// ============================================================

Preferences preferences;

const uint16_t HISTORY_MAGIC = 0xEA25;
const int MAX_HISTORY = 20;

struct TestRecord {
  uint16_t magic;
  uint32_t testNumber;
  float currentRMS;
  float epRMS;
  float resistance;
  uint8_t valid;
  uint8_t reserved[3];
};

TestRecord history[MAX_HISTORY];

int historyCount = 0;
uint32_t nextTestNumber = 1;

// ============================================================
// CORS
// ============================================================

void addCORS() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader(
    "Access-Control-Allow-Methods",
    "GET,POST,OPTIONS"
  );
  server.sendHeader(
    "Access-Control-Allow-Headers",
    "Content-Type, Authorization"
  );
}

// ============================================================
// HISTORY LOAD
// ============================================================

void loadHistory() {

  preferences.begin("earthmeter", false);

  historyCount = preferences.getInt("count", 0);
  nextTestNumber = preferences.getUInt("next", 1);

  if (historyCount < 0 || historyCount > MAX_HISTORY) {
    historyCount = 0;
  }

  size_t bytes = sizeof(history);

  if (preferences.getBytesLength("records") == bytes) {

    preferences.getBytes(
      "records",
      history,
      bytes
    );

  } else {

    memset(history, 0, bytes);
    historyCount = 0;
    nextTestNumber = 1;
  }

  Serial.print("Stored history = ");
  Serial.println(historyCount);
}

// ============================================================
// SAVE HISTORY
// ============================================================

void saveHistory() {

  preferences.putInt(
    "count",
    historyCount
  );

  preferences.putUInt(
    "next",
    nextTestNumber
  );

  preferences.putBytes(
    "records",
    history,
    sizeof(history)
  );
}

// ============================================================
// ADD HISTORY
// ============================================================

void addHistoryRecord(bool valid) {

  TestRecord rec;

  rec.magic = HISTORY_MAGIC;
  rec.testNumber = nextTestNumber++;
  rec.currentRMS = liveCurrentRMS;
  rec.epRMS = liveEPRMS;

  rec.resistance =
    valid ? lastValidResistance : 0.0f;

  rec.valid = valid ? 1 : 0;

  memset(
    rec.reserved,
    0,
    sizeof(rec.reserved)
  );

  if (historyCount < MAX_HISTORY) {

    history[historyCount++] = rec;

  } else {

    for (int i = 1; i < MAX_HISTORY; i++) {
      history[i - 1] = history[i];
    }

    history[MAX_HISTORY - 1] = rec;
  }

  saveHistory();
}

// ============================================================
// CLEAR HISTORY
// ============================================================

void clearHistory() {

  historyCount = 0;
  nextTestNumber = 1;

  memset(
    history,
    0,
    sizeof(history)
  );

  saveHistory();
}

// ============================================================
// CALIBRATION NOTE
// Phasor method removes each window's DC mean before analysis.
// Keep test current OFF during startup.
// ============================================================
void calibrateOffset() {
  lcdLine(0, "Earth Resistance");
  lcdLine(1, "Sensor ready");
  Serial.println("Phasor method: per-window DC mean removal enabled.");
}

// ============================================================
// START TEST
// ============================================================

void startTest() {

  currentState = false;
  consecutiveOnWindows = 0;

  lastValidResistance = 0.0f;

  liveCurrentRMS = 0.0f;
  liveEPRMS = 0.0f;
  liveResistance = 0.0f;

  liveResistanceValid = false;

  testSecond = 0;
  testFinished = false;

  validResistanceCount = 0;

  memset(
    resistanceValues,
    0,
    sizeof(resistanceValues)
  );

  totalWindows = 0;
  testRunning = true;
  testStart = millis();
  lcdLine(0, "Earth Resistance");
  lcdLine(1, "Testing 60 sec");

  Serial.println();
  Serial.println("========================================");
  Serial.println("       STARTING 60 SECOND TEST");
  Serial.println("========================================");
}

// ============================================================
// FINISH TEST
// ============================================================
void finishTest() {
  if (!testRunning) return;
  testRunning = false;
  testFinished = true;
  testSecond = 60;
  if (validResistanceCount >= 5) {
    float med = calculateMedian(resistanceValues, validResistanceCount);
    if (med > 0.0f && med <= MAX_RESISTANCE) {
      lastValidResistance = med;
      liveResistance = med / 10.0f;
      liveResistanceValid = true;
    } else { liveResistance = 0.0f; liveResistanceValid = false; }
  } else { liveResistance = 0.0f; liveResistanceValid = false; }
  addHistoryRecord(liveResistanceValid);
  Serial.println("========== TEST FINISHED ==========");
  Serial.printf("Valid windows = %d / %d\n", validResistanceCount, totalWindows);
  Serial.printf("Current RMS = %.4f mA\n", liveCurrentRMS);
  Serial.printf("E-P RMS = %.5f V\n", liveEPRMS);
  if (liveResistanceValid) {
    Serial.printf("Final Resistance = %.1f ohm\n", liveResistance / 10.0f);
    char b[17]; snprintf(b, sizeof(b), "R:%.1f Ohm", liveResistance / 10.0f);
    lcdLine(0, "Final Resistance"); lcdLine(1, b);
  } else {
    Serial.println("Final Resistance = NO VALID RESULT");
    lcdLine(0, "Final Resistance"); lcdLine(1, "No valid result");
  }
}

// ============================================================
// PROCESS TEST: 50 Hz PHASOR + SNR + PHASE VALIDATION
// ============================================================
void processTest() {
  if (!testRunning) return;
  if (millis() - testStart >= TEST_TIME) { finishTest(); return; }

  Win w = measurePhasorWindow();
  totalWindows++;
  Phasor V = {w.ep.re / VOLTAGE_GAIN, w.ep.im / VOLTAGE_GAIN};
  float curScale = 1.0f / (CURRENT_GAIN * R_SENSE);
  Phasor I = {w.cur.re * curScale, w.cur.im * curScale};
  float Imag = mag(I), Vmag = mag(V);
  float I_mA_rms = Imag * 1000.0f / 1.41421356f;
  float V_rms = Vmag / 1.41421356f;
  float R = 0.0f, phase = 0.0f;
  if (Imag > 1e-9f) {
    float dot = V.re * I.re + V.im * I.im;
    float cross = V.im * I.re - V.re * I.im;
    R = dot / (Imag * Imag);
    phase = atan2f(cross, dot) * 180.0f / PI;
  }
  float snrI = (w.curNoise > 1e-9f) ? mag(w.cur) / w.curNoise : 999.0f;
  float snrV = (w.epNoise > 1e-9f) ? mag(w.ep) / w.epNoise : 999.0f;
  bool currentPresent = (snrI > MIN_SNR) && (I_mA_rms >= MIN_CURRENT_mA);
  bool good = currentPresent && snrV > MIN_SNR && fabsf(phase) < MAX_PHASE_DEG && isfinite(R) && R > 0.0f && R <= MAX_RESISTANCE;

  liveCurrentRMS = I_mA_rms;
  liveEPRMS = V_rms;
  testSecond = (millis() - testStart) / 1000UL;
  if (testSecond > 60) testSecond = 60;
  currentState = currentPresent;

  if (good && validResistanceCount < MAX_RESULTS) {
    resistanceValues[validResistanceCount++] = R;
    liveResistance = calculateMedian(resistanceValues, validResistanceCount) / 10.0f;
    liveResistanceValid = true;
    lastValidResistance = liveResistance;
  } else if (!currentPresent) {
    liveResistanceValid = false;
  }

  Serial.printf("t=%lu s | I=%.4f mA RMS | VEP=%.3f mV RMS | Rraw=%.1f | Rdisplay=%.1f ohm | phase=%.2f deg | SNR-I=%.2f | SNR-V=%.2f | %s | valid=%d/%d\n",
    (millis() - testStart) / 1000UL, I_mA_rms, V_rms * 1000.0f, R, R / 10.0f, phase, snrI, snrV, good ? "VALID" : (!currentPresent ? "NO CURRENT" : "REJECT"), validResistanceCount, totalWindows);

  char line[17];
  snprintf(line, sizeof(line), "I:%.3fmA", I_mA_rms);
  lcdLine(0, line);
  if (liveResistanceValid) snprintf(line, sizeof(line), "R:%.1f Ohm", liveResistance / 10.0f);
  else snprintf(line, sizeof(line), "T:%02d/60 No R", testSecond);
  lcdLine(1, line);
}

// ============================================================
// WIFI CONNECT
// ============================================================

// ============================================================
// WI-FI SETUP PAGE
// ============================================================

void handleWifiPage() {
  String html = R"rawliteral(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>EarthMeter Wi-Fi Setup</title><style>
*{box-sizing:border-box}body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;background:#0f172a;color:#f8fafc;font-family:Arial;padding:18px}
.card{width:100%;max-width:440px;background:#1e293b;padding:25px;border-radius:18px}h1{font-size:24px;margin-top:0}p{color:#cbd5e1;line-height:1.5}
label{display:block;margin-top:14px;margin-bottom:6px}input{width:100%;padding:13px;border-radius:8px;border:1px solid #475569;background:#0f172a;color:white;font-size:16px}
button{width:100%;padding:14px;border:0;border-radius:9px;background:#22c55e;color:#052e16;font-weight:bold;font-size:16px;margin-top:18px}.status{padding:12px;border-radius:8px;background:#0f172a;overflow-wrap:anywhere}.small{font-size:13px;color:#94a3b8}a{color:#7dd3fc}
</style></head><body><div class="card"><h1>EarthMeter Wi-Fi Setup</h1>
<p>Enter the 2.4 GHz Wi-Fi or mobile-hotspot details that the ESP32 should connect to.</p>
<form id="wifiForm"><label for="ssid">Wi-Fi name (SSID)</label><input id="ssid" name="ssid" maxlength="32" autocomplete="off" required>
<label for="password">Wi-Fi password</label><input id="password" name="password" type="password" maxlength="63" autocomplete="new-password">
<button id="connectBtn" type="submit">Connect to Wi-Fi</button></form>
<p id="message" class="status">Setup hotspot is active.</p><div id="ipArea" hidden><p><b>ESP32 Wi-Fi IP address:</b><br><span id="ip"></span></p>
<p><a id="dashboardLink" href="/">Open EarthMeter dashboard</a></p><p class="small">Connect your phone/laptop to the same Wi-Fi network before opening this IP.</p></div>
<p class="small">Setup hotspot: EarthMeter_Setup · Open http://192.168.4.1</p>
<script>
const form=document.getElementById('wifiForm'),message=document.getElementById('message'),btn=document.getElementById('connectBtn');let polling=false;
form.addEventListener('submit',async e=>{e.preventDefault();const ssid=document.getElementById('ssid').value.trim(),password=document.getElementById('password').value;if(!ssid){message.textContent='Please enter the Wi-Fi name.';return;}btn.disabled=true;btn.textContent='Sending credentials...';message.textContent='Sending Wi-Fi details to ESP32...';try{const body=new URLSearchParams({ssid,password});const r=await fetch('/wifi-save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body});const d=await r.json();if(!r.ok||!d.ok)throw new Error(d.error||'Could not save Wi-Fi settings');message.textContent='Credentials saved. ESP32 is connecting...';document.getElementById('password').value='';polling=true;pollStatus();}catch(err){message.textContent='Error: '+err.message;btn.disabled=false;btn.textContent='Connect to Wi-Fi';}});
async function pollStatus(){if(!polling)return;try{const r=await fetch('/api/status',{cache:'no-store'}),d=await r.json();if(d.status==='connected'&&d.ip&&d.ip!=='0.0.0.0'){message.textContent='Wi-Fi connected successfully!';document.getElementById('ip').textContent=d.ip;document.getElementById('dashboardLink').href='http://'+d.ip+'/';document.getElementById('ipArea').hidden=false;btn.disabled=false;btn.textContent='Connect to another Wi-Fi';polling=false;return;}message.textContent='Connecting to '+(d.targetSsid||'Wi-Fi')+'... Please wait.';}catch(e){message.textContent='Still connecting. Keep this page open and wait...';}setTimeout(pollStatus,1500);}
</script></div></body></html>)rawliteral";
  server.send(200, "text/html", html);
}

void handleWifiSave() {
  addCORS();
  if (!server.hasArg("ssid") || !server.arg("ssid").length()) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Wi-Fi name is required\"}");
    return;
  }
  configuredSSID = server.arg("ssid");
  configuredPassword = server.arg("password");
  wifiPreferences.begin("wifi-config", false);
  wifiPreferences.putString("ssid", configuredSSID);
  wifiPreferences.putString("password", configuredPassword);
  wifiPreferences.end();
  // Keep setup AP running while ESP32 joins the router/hotspot.
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(configuredSSID.c_str(), configuredPassword.c_str());
  Serial.print("Connecting to configured Wi-Fi: ");
  Serial.println(configuredSSID);
  server.send(200, "application/json", "{\"ok\":true,\"message\":\"Credentials saved; connecting\"}");
}

// ============================================================
// STATUS API
// ============================================================

void handleStatus() {

  addCORS();

  String status =
    WiFi.status() == WL_CONNECTED
    ? "connected"
    : "disconnected";

  String ssid =
    WiFi.status() == WL_CONNECTED
    ? WiFi.SSID()
    : "";

  String ip =
    WiFi.status() == WL_CONNECTED
    ? WiFi.localIP().toString()
    : "0.0.0.0";

  int rssi =
    WiFi.status() == WL_CONNECTED
    ? WiFi.RSSI()
    : 0;

  String json = "{";

  json += "\"device\":\"ESP32\",";
  json += "\"status\":\"" + status + "\",";
  json += "\"ssid\":\"" + ssid + "\",";
  json += "\"ip\":\"" + ip + "\",";
  json += "\"rssi\":" + String(rssi) + ",";
  json += "\"targetSsid\":\"" + configuredSSID + "\",";
  json += "\"setupApIp\":\"" + WiFi.softAPIP().toString() + "\"";

  json += "}";

  server.send(
    200,
    "application/json",
    json
  );
}

// ============================================================
// API LOGIN
// ============================================================

void handleApiLogin() {

  addCORS();

  if (
    !server.hasArg("username") ||
    !server.hasArg("password")
  ) {

    server.send(
      400,
      "application/json",
      "{\"ok\":false,\"error\":\"Missing credentials\"}"
    );

    return;
  }

  String user =
    server.arg("username");

  String pass =
    server.arg("password");

  if (
    user == LOGIN_USER &&
    pass == LOGIN_PASS
  ) {

    loggedIn = true;

    server.send(
      200,
      "application/json",
      "{\"ok\":true,\"loggedIn\":true}"
    );

  } else {

    loggedIn = false;

    server.send(
      401,
      "application/json",
      "{\"ok\":false,\"error\":\"Invalid login\"}"
    );
  }
}

// ============================================================
// LOGIN PAGE
// ============================================================

void handleLoginPage() {

  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport"
content="width=device-width,initial-scale=1">
<title>Earth Meter Login</title>
<style>
body{
margin:0;
background:#0f172a;
color:white;
font-family:Arial;
display:flex;
justify-content:center;
align-items:center;
min-height:100vh;
}
.box{
width:90%;
max-width:400px;
background:#1e293b;
padding:30px;
border-radius:18px;
}
input{
width:100%;
box-sizing:border-box;
padding:13px;
margin:8px 0;
border-radius:8px;
border:1px solid #475569;
background:#0f172a;
color:white;
}
button{
width:100%;
padding:13px;
margin-top:12px;
border:0;
border-radius:8px;
background:#22c55e;
color:white;
font-weight:bold;
}
</style>
</head>
<body>
<div class="box">
<h2>Earth Resistance Meter</h2>
<form method="POST" action="/login">
<input name="username"
placeholder="Username"
required>
<input name="password"
type="password"
placeholder="Password"
required>
<button type="submit">LOGIN</button>
</form>
</div>
</body>
</html>
)rawliteral";

  server.send(
    200,
    "text/html",
    html
  );
}

// ============================================================
// NORMAL LOGIN
// ============================================================

void handleLogin() {

  String user =
    server.arg("username");

  String pass =
    server.arg("password");

  if (
    user == LOGIN_USER &&
    pass == LOGIN_PASS
  ) {

    loggedIn = true;

    server.sendHeader(
      "Location",
      "/"
    );

    server.send(
      302,
      "text/plain",
      ""
    );

  } else {

    server.send(
      401,
      "text/plain",
      "Invalid username or password"
    );
  }
}

// ============================================================
// LOGOUT
// ============================================================

void handleLogout() {

  loggedIn = false;

  server.sendHeader(
    "Location",
    "/login"
  );

  server.send(
    302,
    "text/plain",
    ""
  );
}

// ============================================================
// AUTH CHECK
// ============================================================

bool requireLogin() {

  if (loggedIn) {
    return true;
  }

  addCORS();

  server.send(
    401,
    "application/json",
    "{\"ok\":false,\"error\":\"Unauthorized\"}"
  );

  return false;
}

// ============================================================
// DATA API
// ============================================================

void handleData() {

  addCORS();

  if (!requireLogin()) {
    return;
  }

  String json = "{";

  json += "\"current\":";
  json += String(
    liveCurrentRMS,
    4
  );

  json += ",\"ep\":";
  json += String(
    liveEPRMS,
    5
  );

  json += ",\"resistance\":";
  json += String(
    liveResistance,
    2
  );

  json += ",\"time\":";
  json += String(
    testSecond
  );

  json += ",\"running\":";
  json += testRunning
    ? "true"
    : "false";

  json += ",\"finished\":";
  json += testFinished
    ? "true"
    : "false";

  json += ",\"currentOn\":";
  json += currentState
    ? "true"
    : "false";

  json += ",\"resistanceValid\":";
  json += liveResistanceValid
    ? "true"
    : "false";

  json += ",\"finalValid\":";
  json += (
    testFinished &&
    liveResistanceValid
  )
    ? "true"
    : "false";

  json += ",\"testSecond\":";
  json += String(testSecond);

  json += "}";

  server.send(
    200,
    "application/json",
    json
  );
}

// ============================================================
// START API
// ============================================================

void handleStart() {

  addCORS();

  if (!requireLogin()) {
    return;
  }

  if (testRunning) {

    server.send(
      200,
      "text/plain",
      "TEST ALREADY RUNNING"
    );

    return;
  }

  startTest();

  server.send(
    200,
    "text/plain",
    "TEST STARTED"
  );
}

// ============================================================
// STOP API
// ============================================================

void handleStop() {

  addCORS();

  if (!requireLogin()) {
    return;
  }

  if (testRunning) {

    testRunning = false;
    testFinished = true;

    if (validResistanceCount > 0) {

      float median =
        calculateMedian(
          resistanceValues,
          validResistanceCount
        );

      if (
        median > 0 &&
        median <= MAX_RESISTANCE
      ) {

        liveResistance =
          median / 10.0f;

        lastValidResistance =
          liveResistance;

        liveResistanceValid =
          true;
      }
    }

    addHistoryRecord(
      liveResistanceValid
    );
  }

  server.send(
    200,
    "text/plain",
    "TEST STOPPED"
  );
}

// ============================================================
// HISTORY API
// ============================================================

void handleHistory() {

  addCORS();

  if (!requireLogin()) {
    return;
  }

  String json = "[";

  for (
    int i = historyCount - 1;
    i >= 0;
    i--
  ) {

    if (i != historyCount - 1) {
      json += ",";
    }

    TestRecord &r =
      history[i];

    json += "{";

    json += "\"test\":";
    json += String(
      r.testNumber
    );

    json += ",\"current\":";
    json += String(
      r.currentRMS,
      4
    );

    json += ",\"ep\":";
    json += String(
      r.epRMS,
      5
    );

    json += ",\"resistance\":";
    json += String(
      r.resistance,
      2
    );

    json += ",\"valid\":";
    json += r.valid
      ? "true"
      : "false";

    json += "}";
  }

  json += "]";

  server.send(
    200,
    "application/json",
    json
  );
}

// ============================================================
// CLEAR HISTORY API
// ============================================================

void handleClearHistory() {

  addCORS();

  if (!requireLogin()) {
    return;
  }

  clearHistory();

  server.send(
    200,
    "text/plain",
    "HISTORY CLEARED"
  );
}

// ============================================================
// DOWNLOAD ALL CSV
// ============================================================

void handleDownloadAll() {

  addCORS();

  if (!requireLogin()) {
    return;
  }

  String csv = String("\xEF\xBB\xBF") +
    "Test,Current RMS (mA),E-P RMS (V),Resistance (Ohm),Status\r\n";

  for (int i = 0; i < historyCount; i++) {

    TestRecord &r =
      history[i];

    csv += String(
      r.testNumber
    );

    csv += ",";

    csv += String(
      r.currentRMS,
      4
    );

    csv += ",";

    csv += String(
      r.epRMS,
      5
    );

    csv += ",";

    if (r.valid) {
      csv += String(
        r.resistance,
        2
      );
    } else {
      csv += "NOT MEASURED";
    }

    csv += ",";

    csv += r.valid
      ? "VALID"
      : "NO TEST CURRENT";

    csv += "\r\n";
  }

  server.sendHeader("Content-Disposition", "attachment; filename=\"earth-meter-history.csv\"");
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("X-Content-Type-Options", "nosniff");
  server.send(200, "text/csv; charset=utf-8", csv);
}

// ============================================================
// DOWNLOAD SINGLE CSV
// ============================================================

void handleDownloadSingle() {

  addCORS();

  if (!requireLogin()) {
    return;
  }

  if (!server.hasArg("test")) {

    server.send(
      400,
      "text/plain",
      "Missing test number"
    );

    return;
  }

  uint32_t number =
    server.arg("test").toInt();

  for (int i = 0; i < historyCount; i++) {

    TestRecord &r =
      history[i];

    if (r.testNumber == number) {

      String csv = String("\xEF\xBB\xBF") +
        "Test,Current RMS (mA),E-P RMS (V),Resistance (Ohm),Status\r\n";

      csv += String(
        r.testNumber
      );

      csv += ",";

      csv += String(
        r.currentRMS,
        4
      );

      csv += ",";

      csv += String(
        r.epRMS,
        5
      );

      csv += ",";

      if (r.valid) {

        csv += String(
          r.resistance,
          2
        );

      } else {

        csv += "NOT MEASURED";
      }

      csv += ",";

      csv += r.valid
        ? "VALID"
        : "NO TEST CURRENT";

      csv += "\r\n";

      String filename =
        "earth_meter_test_" +
        String(r.testNumber) +
        ".csv";

      server.sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
      server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
      server.sendHeader("Pragma", "no-cache");
      server.sendHeader("X-Content-Type-Options", "nosniff");
      server.send(200, "text/csv; charset=utf-8", csv);

      return;
    }
  }

  server.send(
    404,
    "text/plain",
    "Test not found"
  );
}

// ============================================================
// ROOT
// ============================================================

void handleRoot() {

  if (WiFi.status() != WL_CONNECTED) {
    server.sendHeader("Location", "/wifi");
    server.send(302, "text/plain", "");
    return;
  }

  if (!loggedIn) {

    server.sendHeader(
      "Location",
      "/login"
    );

    server.send(
      302,
      "text/plain",
      ""
    );

    return;
  }

  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport"
content="width=device-width,initial-scale=1">
<title>Earth Meter</title>

<style>

body{
margin:0;
padding:20px;
background:#0f172a;
color:white;
font-family:Arial;
}

.container{
max-width:1000px;
margin:auto;
}

.card{
background:#1e293b;
padding:20px;
margin-bottom:15px;
border-radius:15px;
}

.grid{
display:grid;
grid-template-columns:
repeat(auto-fit,minmax(200px,1fr));
gap:15px;
}

.value{
font-size:30px;
font-weight:bold;
margin-top:10px;
}

.label{
color:#94a3b8;
}

button{
width:100%;
padding:13px;
border:0;
border-radius:9px;
background:#22c55e;
color:white;
font-weight:bold;
margin-top:10px;
cursor:pointer;
}

.red{
background:#dc2626;
}

.blue{
background:#2563eb;
}

table{
width:100%;
border-collapse:collapse;
}

th,td{
padding:10px;
border-bottom:
1px solid #334155;
text-align:center;
}

.wrap{
overflow:auto;
}

</style>
</head>

<body>

<div class="container">

<div class="card">

<h1>
EARTH RESISTANCE METER
</h1>

<p>
ESP32 Measurement System
</p>

</div>

<div class="grid">

<div class="card">

<div class="label">
CURRENT RMS
</div>

<div
class="value"
id="current">
0.0000
</div>

<p>mA</p>

</div>

<div class="card">

<div class="label">
E-P RMS
</div>

<div
class="value"
id="ep">
0.00000
</div>

<p>V</p>

</div>

<div class="card">

<div class="label">
RESISTANCE
</div>

<div
class="value"
id="resistance">
--
</div>

<p>Ohm</p>

</div>

<div class="card">

<div class="label">
TEST TIME
</div>

<div
class="value"
id="time">
0
</div>

<p>Seconds</p>

</div>

</div>

<div
class="card"
id="status">
NO TEST CURRENT
</div>

<button
onclick="startTest()">
START 60 SECOND TEST
</button>

<button
class="red"
onclick="stopTest()">
STOP TEST
</button>

<div class="card">

<h2>
Stored Data
</h2>

<div class="wrap">

<table>

<thead>

<tr>

<th>Test</th>
<th>Current</th>
<th>E-P</th>
<th>Resistance</th>
<th>Status</th>
<th>Download</th>

</tr>

</thead>

<tbody
id="history">
</tbody>

</table>

</div>

<button
class="blue"
onclick="downloadAll()">
DOWNLOAD ALL DATA
</button>

<button
class="red"
onclick="clearHistory()">
CLEAR ALL DATA
</button>

</div>

<button
class="red"
onclick="location='/logout'">
LOGOUT
</button>

</div>

<script>

async function updateData(){

try{

let r =
await fetch('/data');

if(r.status === 401){

location='/login';
return;

}

let d =
await r.json();

document.getElementById(
'current'
).innerText =
Number(d.current).toFixed(4);

document.getElementById(
'ep'
).innerText =
Number(d.ep).toFixed(5);

document.getElementById(
'time'
).innerText =
d.time;

let status =
document.getElementById(
'status'
);

let resistance =
document.getElementById(
'resistance'
);

if(d.running){

if(d.currentOn){

status.innerText =
'TEST CURRENT ON';

status.style.background =
'#14532d';

if(d.resistanceValid){

resistance.innerText =
Number(
d.resistance
).toFixed(2);

}else{

resistance.innerText =
'NOT MEASURED';

}

}else{

status.innerText =
'NO TEST CURRENT';

status.style.background =
'#7f1d1d';

resistance.innerText =
'NOT MEASURED';

}

}else if(d.finished){

if(d.finalValid){

status.innerText =
'FINAL RESULT: ' +
Number(
d.resistance
).toFixed(2) +
' Ohm';

status.style.background =
'#164e63';

resistance.innerText =
Number(
d.resistance
).toFixed(2);

}else{

status.innerText =
'FINAL RESULT: NO TEST CURRENT';

status.style.background =
'#7f1d1d';

resistance.innerText =
'0.00';

}

}

}catch(e){

console.log(e);

}

}

async function startTest(){

await fetch('/start');

}

async function stopTest(){

await fetch('/stop');

}

async function loadHistory(){

try{

let r =
await fetch('/history');

let data =
await r.json();

let html='';

data.reverse().forEach(
x => {

html += '<tr>';

html +=
'<td>#' +
x.test +
'</td>';

html +=
'<td>' +
Number(
x.current
).toFixed(4) +
' mA</td>';

html +=
'<td>' +
Number(
x.ep
).toFixed(5) +
' V</td>';

html +=
'<td>' +
(
x.valid
?
Number(
x.resistance
).toFixed(2)
+
' Ohm'
:
'NOT MEASURED'
) +
'</td>';

html +=
'<td>' +
(
x.valid
?
'VALID'
:
'INVALID'
) +
'</td>';

html +=
'<td>' +

'<button onclick="downloadSingle(' +
x.test +
')">' +

'DOWNLOAD' +

'</button>' +

'</td>';

html += '</tr>';

});

document.getElementById(
'history'
).innerHTML =
html;

}catch(e){

console.log(e);

}

}

async function downloadCsv(url, fallbackName){
  try {
    const response = await fetch(url, {cache: 'no-store'});
    if (!response.ok) {
      const detail = await response.text();
      alert('CSV download failed (' + response.status + '): ' + detail);
      return;
    }
    const blob = await response.blob();
    const objectUrl = URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href = objectUrl;
    link.download = fallbackName;
    document.body.appendChild(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(objectUrl), 1500);
  } catch (err) {
    console.error('CSV download error:', err);
    alert('CSV download failed. Check Wi-Fi connection and reload the page.');
  }
}

function downloadAll(){
  downloadCsv('/download-all', 'earth-meter-history.csv');
}

function downloadSingle(test){
  downloadCsv('/download?test=' + encodeURIComponent(test),
              'earth_meter_test_' + test + '.csv');
}

async function clearHistory(){

if(
confirm(
'Delete all stored data?'
)
){

await fetch('/clear');

loadHistory();

}

}

setInterval(
updateData,
1000
);

setInterval(
loadHistory,
5000
);

updateData();
loadHistory();

</script>

</body>
</html>
)rawliteral";

  server.send(
    200,
    "text/html",
    html
  );
}

// ===== Firebase Realtime Database (temporary public-rules testing) =====
// Do not use public read/write rules in production. Secure the database before real deployment.
const char* FIREBASE_DB_URL = "https://earth-meter-project-default-rtdb.asia-southeast1.firebasedatabase.app";
unsigned long lastCloudPublish = 0;
unsigned long lastCommandPoll = 0;
String lastCommandId = "";
String cloudStatus = "starting";
uint32_t lastAutoRecordUploadNumber = 0;
unsigned long lastAutoRecordUploadAt = 0;

String cloudRequest(const String& path, const String& method, const String& payload = "") {
  if (WiFi.status() != WL_CONNECTED) return "";
  WiFiClientSecure client;
  client.setInsecure(); // Testing only; use certificate validation for production.
  HTTPClient http;
  String url = String(FIREBASE_DB_URL) + path;
  if (!http.begin(client, url)) return "";
  http.addHeader("Content-Type", "application/json");
  int code = -1;
  if (method == "GET") code = http.GET();
  else if (method == "PUT") code = http.PUT(payload);
  else if (method == "PATCH") code = http.PATCH(payload);
  String response = (code > 0) ? http.getString() : "";
  if (code < 200 || code >= 300) {
    Serial.printf("Firebase request failed: HTTP %d\n", code);
    cloudStatus = "error";
  } else cloudStatus = "connected";
  http.end();
  return response;
}

String cloudTelemetryJson() {
  String j = "{";
  j += "\"device\":\"EarthMeter\",";
  j += "\"current\":" + String(liveCurrentRMS, 4) + ",";
  j += "\"ep\":" + String(liveEPRMS, 5) + ",";
  j += "\"resistance\":" + String(liveResistance, 2) + ",";
  j += "\"time\":" + String(testSecond) + ",";
  j += "\"running\":" + String(testRunning ? "true" : "false") + ",";
  j += "\"finished\":" + String(testFinished ? "true" : "false") + ",";
  j += "\"currentOn\":" + String(currentState ? "true" : "false") + ",";
  j += "\"resistanceValid\":" + String(liveResistanceValid ? "true" : "false") + ",";
  j += "\"finalValid\":" + String((testFinished && liveResistanceValid) ? "true" : "false") + ",";
  j += "\"wifiConnected\":true,";
  j += "\"updatedAt\":" + String((uint32_t)millis());
  j += "}";
  return j;
}

String cloudHistoryJson() {
  String j = "[";
  for (int i = historyCount - 1; i >= 0; --i) {
    if (i != historyCount - 1) j += ",";
    TestRecord &r = history[i];
    j += "{\"test\":" + String(r.testNumber);
    j += ",\"current\":" + String(r.currentRMS, 4);
    j += ",\"ep\":" + String(r.epRMS, 5);
    j += ",\"resistance\":" + String(r.resistance, 2);
    j += ",\"valid\":" + String(r.valid ? "true" : "false") + "}";
  }
  j += "]";
  return j;
}

String autoTestRecordJson(const TestRecord &r) {
  String j = "{";
  j += "\"testNumber\":" + String(r.testNumber) + ",";
  j += "\"test\":" + String(r.testNumber) + ",";
  j += "\"current\":" + String(r.currentRMS, 4) + ",";
  j += "\"ep\":" + String(r.epRMS, 5) + ",";
  j += "\"resistance\":" + String(r.resistance, 2) + ",";
  j += "\"valid\":" + String(r.valid ? "true" : "false") + ",";
  j += "\"source\":\"ESP32-auto\"";
  j += "}";
  return j;
}

void serviceAutoTestUploads(unsigned long now) {
  // Local Preferences retains the latest 20 records. Firebase PUT is idempotent by test number.
  if (now - lastAutoRecordUploadAt < 1500UL || historyCount <= 0) return;
  for (int i = 0; i < historyCount; ++i) {
    const TestRecord &r = history[i]; // oldest to newest
    if (r.magic != HISTORY_MAGIC || r.testNumber <= lastAutoRecordUploadNumber) continue;
    lastAutoRecordUploadAt = now;
    cloudRequest("/autoTests/" + String(r.testNumber) + ".json", "PUT", autoTestRecordJson(r));
    if (cloudStatus == "connected") lastAutoRecordUploadNumber = r.testNumber;
    return; // at most one record per interval
  }
}

void serviceFirebase() {
  if (WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();
  if (now - lastCloudPublish >= 1500UL) {
    lastCloudPublish = now;
    cloudRequest("/telemetry.json", "PUT", cloudTelemetryJson());
    cloudRequest("/history.json", "PUT", cloudHistoryJson());
  }
  serviceAutoTestUploads(now);
  if (now - lastCommandPoll >= 1200UL) {
    lastCommandPoll = now;
    String cmd = cloudRequest("/command.json", "GET");
    if (cmd.length() && cmd != "null") {
      int idPos = cmd.indexOf("\"id\":\"");
      int actPos = cmd.indexOf("\"action\":\"");
      if (idPos >= 0 && actPos >= 0) {
        int idStart = idPos + 6;
        int idEnd = cmd.indexOf('\"', idStart);
        int actStart = actPos + 10;
        int actEnd = cmd.indexOf('\"', actStart);
        if (idEnd > idStart && actEnd > actStart) {
          String id = cmd.substring(idStart, idEnd);
          String action = cmd.substring(actStart, actEnd);
          if (id != lastCommandId) {
            lastCommandId = id;
            if (action == "start" && !testRunning) startTest();
            else if (action == "stop" && testRunning) {
              testRunning = false;
              testFinished = true;
              if (validResistanceCount >= 5) {
                float med = calculateMedian(resistanceValues, validResistanceCount);
                if (med > 0 && med <= MAX_RESISTANCE) {
                  liveResistance = med / 10.0f;
                  lastValidResistance = liveResistance;
                  liveResistanceValid = true;
                }
              }
              addHistoryRecord(liveResistanceValid);
            }
            cloudRequest("/commandResult.json", "PUT", "{\"id\":\"" + id + "\",\"action\":\"" + action + "\",\"status\":\"received\"}");
          }
        }
      }
    }
  }
}



// ============================================================
// SETUP
// ============================================================

void setup() {

  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  initTables();

  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println("       EARTH METER STARTING");
  Serial.println("========================================");

  // ADC

  pinMode(
    EP_PIN,
    INPUT
  );

  pinMode(
    CURRENT_PIN,
    INPUT
  );

  analogSetPinAttenuation(
    EP_PIN,
    ADC_11db
  );

  analogSetPinAttenuation(
    CURRENT_PIN,
    ADC_11db
  );

  // History

  loadHistory();

  // Start a local setup hotspot and keep it available during Wi-Fi connection.
  WiFi.mode(WIFI_AP_STA);
  WiFi.setHostname("EarthMeter");
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  // Explicit channel and visible SSID improve setup-hotspot reliability.
  bool apStarted = WiFi.softAP(SETUP_AP_SSID, SETUP_AP_PASSWORD, 1, false, 4);
  Serial.print("Setup hotspot: ");
  Serial.println(apStarted ? "started" : "FAILED");
  Serial.print("Setup page: http://");
  Serial.println(WiFi.softAPIP());
  Serial.print("Setup SSID: "); Serial.println(SETUP_AP_SSID);
  Serial.print("Setup password: "); Serial.println(SETUP_AP_PASSWORD);

  wifiPreferences.begin("wifi-config", true);
  configuredSSID = wifiPreferences.getString("ssid", "");
  configuredPassword = wifiPreferences.getString("password", "");
  wifiPreferences.end();

  if (configuredSSID.length() > 0) {
    WiFi.begin(configuredSSID.c_str(), configuredPassword.c_str());
    Serial.print("Trying saved Wi-Fi: ");
    Serial.println(configuredSSID);
  } else {
    Serial.println("No saved Wi-Fi. Connect to EarthMeter_Setup and open http://192.168.4.1");
  }

  // ==========================================================
  // ROUTES
  // ==========================================================

  server.on("/wifi", HTTP_GET, handleWifiPage);
  server.on("/wifi-save", HTTP_POST, handleWifiSave);
  server.on("/wifi-save", HTTP_OPTIONS, []() {
    addCORS();
    server.send(204, "text/plain", "");
  });

  server.on(
    "/login",
    HTTP_GET,
    handleLoginPage
  );

  server.on(
    "/login",
    HTTP_POST,
    handleLogin
  );

  server.on(
    "/api/login",
    HTTP_POST,
    handleApiLogin
  );

  server.on(
    "/api/login",
    HTTP_OPTIONS,
    []() {
      addCORS();
      server.send(
        204,
        "text/plain",
        ""
      );
    }
  );

  server.on(
    "/logout",
    HTTP_GET,
    handleLogout
  );

  server.on("/", HTTP_GET, []() {
    // Setup AP root opens Wi-Fi setup; router IP opens normal dashboard.
    IPAddress localAddress = server.client().localIP();
    if (localAddress == WiFi.softAPIP()) {
      handleWifiPage();
    } else {
      handleRoot();
    }
  });

  server.on(
    "/api/status",
    HTTP_GET,
    handleStatus
  );

  server.on(
    "/api/status",
    HTTP_OPTIONS,
    []() {
      addCORS();
      server.send(
        204,
        "text/plain",
        ""
      );
    }
  );

  server.on(
    "/data",
    HTTP_GET,
    handleData
  );

  server.on(
    "/api/data",
    HTTP_GET,
    handleData
  );

  server.on(
    "/api/data",
    HTTP_OPTIONS,
    []() {
      addCORS();
      server.send(
        204,
        "text/plain",
        ""
      );
    }
  );

  server.on(
    "/start",
    HTTP_GET,
    handleStart
  );

  server.on(
    "/api/start",
    HTTP_GET,
    handleStart
  );

  server.on(
    "/stop",
    HTTP_GET,
    handleStop
  );

  server.on(
    "/api/stop",
    HTTP_GET,
    handleStop
  );

  server.on(
    "/history",
    HTTP_GET,
    handleHistory
  );

  server.on(
    "/api/history",
    HTTP_GET,
    handleHistory
  );

  server.on(
    "/clear",
    HTTP_GET,
    handleClearHistory
  );

  server.on(
    "/api/history/clear",
    HTTP_GET,
    handleClearHistory
  );

  server.on(
    "/download-all",
    HTTP_GET,
    handleDownloadAll
  );

  server.on(
    "/api/history/download-all",
    HTTP_GET,
    handleDownloadAll
  );

  server.on(
    "/download",
    HTTP_GET,
    handleDownloadSingle
  );

  server.onNotFound([]() {
    addCORS();
    IPAddress localAddress = server.client().localIP();
    if (localAddress == WiFi.softAPIP()) {
      server.sendHeader("Location", "/wifi", true);
      server.send(302, "text/plain", "Open /wifi for EarthMeter setup");
    } else {
      server.send(404, "text/plain", "Not Found");
    }
  });

  server.begin();

  Serial.println();
  Serial.println("========================================");
  Serial.println("       EARTH METER SERVER READY");
  Serial.println("========================================");

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    Serial.print(
      "Website: http://"
    );

    Serial.println(
      WiFi.localIP()
    );

    Serial.print(
      "API: http://"
    );

    Serial.print(
      WiFi.localIP()
    );

    Serial.println(
      "/api/status"
    );

  } else {
    Serial.println("Wi-Fi is not connected.");
  }

  // Zero calibration
  // Keep test current OFF

  calibrateOffset();

  nextAutoTestAt = millis() + AUTO_TEST_INTERVAL_MS;
  autoTestSchedulerReady = true;
  Serial.println("Automatic test scheduler enabled: 60-second test every 5 minutes.");

  Serial.println();
  Serial.println(
    "SYSTEM READY"
  );
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  server.handleClient();

  processTest();
  serviceFirebase();

  // Website-independent auto-run: first test after 5 minutes, then every 5 minutes.
  if (autoTestSchedulerReady && !testRunning &&
      (int32_t)(millis() - nextAutoTestAt) >= 0) {
    startTest();
    do { nextAutoTestAt += AUTO_TEST_INTERVAL_MS; }
    while ((int32_t)(millis() - nextAutoTestAt) >= 0);
  }

  // ==========================================================
  // AUTO WIFI RECONNECT
  // ==========================================================
  static unsigned long lastReconnect = 0;
  if (WiFi.status() != WL_CONNECTED && configuredSSID.length() > 0 &&
      millis() - lastReconnect >= 20000UL) {
    lastReconnect = millis();
    // Retry slowly; repeated WiFi.begin calls can interrupt association.
    Serial.println("Wi-Fi disconnected; retrying saved network...");
    WiFi.setAutoReconnect(true);
    WiFi.begin(configuredSSID.c_str(), configuredPassword.c_str());
  }

  delay(1);
}
