#include <DHT.h>
#include <esp_now.h>
#include <WiFi.h>
#include <WebServer.h>
#include <time.h>
#include <Preferences.h>

// ---------------- PINS ----------------
#define M1_IN1 16
#define M1_IN2 17
#define M2_IN1 18
#define M2_IN2 19
#define M3_IN1 21
#define M3_IN2 22
#define STBY1 23
#define STBY2 5

#define PIEZO 34
#define PIEZO_THRESHOLD 800

#define PUMP1_PIN 26
#define PUMP2_PIN 27

#define BUTTON 32
#define EMERGENCY_SWITCH 33
#define BUZZER_PIN 13

// LEDs
#define LED_SEQ_START 14
#define LED_SEQ_DONE 12
#define LED_PUMP_START 25
#define LED_PUMP_DONE 2

#define DHTPIN 4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// ---------------- NVS ----------------
Preferences prefs;
static const char* PREF_NS = "appcfg";

// ---------------- MOTOR STRUCT ----------------
struct Motor {
  int in1;
  int in2;
  bool active;
};
Motor motors[3];

// ---------------- LIMITS ----------------
static const int MAX_MOTORS = 3;
static const int MAX_PUMPS = 2;

// ---------------- DEFAULT ACTIVE SETTINGS ----------------
int motorSequence[MAX_MOTORS] = {0, 1, 2};
int pumpSequence[MAX_PUMPS]   = {1, 0};
int motorSeqLen = 3;
int pumpSeqLen  = 2;

int pillTarget[MAX_MOTORS]    = {5, 5, 5};
int pillRemaining[MAX_MOTORS] = {5, 5, 5};

// Pump levels: 0 short, 1 medium, 2 long
unsigned long pumpRunTime[MAX_PUMPS][3] = {
  {1000, 2000, 5000},   // Pump0
  {1500, 2500, 4500}    // Pump1
};
int pumpLevelSelection[MAX_PUMPS] = {1, 0};

// ---------------- SCHEDULES ----------------
enum PartId { PART_MORNING = 0, PART_DAY = 1, PART_NIGHT = 2 };

struct PartConfig {
  bool enabled;
  int startMin; // [0..1439]
  int endMin;   // [0..1439]

  int motSeq[MAX_MOTORS];
  int motLen;

  int pumSeq[MAX_PUMPS];
  int pumLen;

  int pumpLevel[MAX_PUMPS]; // per pump 0..2
};
PartConfig parts[3];

// runtime picked at button press
int runtimeMotSeq[MAX_MOTORS];
int runtimeMotLen = 0;
int runtimePumSeq[MAX_PUMPS];
int runtimePumLen = 0;
int runtimePumpLevel[MAX_PUMPS] = {0, 0};
String runtimeSource = "DEFAULT";

// ---------------- STATE MACHINE ----------------
enum SequenceState { IDLE, RUN_MOTOR, WAIT_AFTER_PILL, RUN_PUMP };
SequenceState seqState = IDLE;

int motorIndex = 0;
int pumpIndex = 0;
bool pillDetected = false;

unsigned long pillDelayStart = 0;
const unsigned long pillDelay = 2000;
unsigned long pumpStartMillis = 0;
bool lastButtonState = HIGH;

// LED timers
unsigned long seqDoneLEDTimer = 0;
unsigned long pumpDoneLEDTimer = 0;
const unsigned long LED_TIMEOUT = 10000;

// Emergency
unsigned long previousSirenMillis = 0;
bool sirenState = false;

// ---------------- ESP-NOW ----------------
typedef struct struct_message { char msg[200]; } struct_message;
struct_message espMessage;
uint8_t broadcastAddress[] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

void sendMessage(const char* text) {
  strncpy(espMessage.msg, text, sizeof(espMessage.msg));
  espMessage.msg[sizeof(espMessage.msg) - 1] = '\0';
  esp_now_send(broadcastAddress, (uint8_t*)&espMessage, sizeof(espMessage));
  Serial.println(text);
}

// ---------------- WIFI & Web ----------------
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
WebServer server(80);
String uiMessage = "Ready";

// ---------------- NTP TIME ----------------
// CHANGE this to your timezone seconds:
// IST: 19800, Bangladesh: 21600, Pakistan: 18000, UAE: 14400
const long gmtOffset_sec = 21600;
const int daylightOffset_sec = 0;
const char* ntpServer = "pool.ntp.org";

bool timeReady() {
  time_t now = time(nullptr);
  return now > 1700000000;
}
bool getTimeInfo(struct tm &timeinfo) { return getLocalTime(&timeinfo, 100); }
int minutesOfDayNow() {
  struct tm t;
  if (!getTimeInfo(t)) return -1;
  return t.tm_hour * 60 + t.tm_min;
}
String nowHHMM() {
  struct tm t;
  if (!getTimeInfo(t)) return "--:--";
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  return String(buf);
}
bool windowActive(int nowMin, int startMin, int endMin) {
  if (nowMin < 0) return false;
  if (startMin == endMin) return true;
  if (startMin < endMin) return (nowMin >= startMin && nowMin < endMin);
  return (nowMin >= startMin) || (nowMin < endMin);
}
const char* partName(PartId id) {
  switch (id) {
    case PART_MORNING: return "MORNING";
    case PART_DAY: return "DAY";
    case PART_NIGHT: return "NIGHT";
    default: return "UNKNOWN";
  }
}
PartId activePart() {
  if (!timeReady()) return (PartId)-1;
  int nowMin = minutesOfDayNow();
  for (int i = 0; i < 3; i++) {
    if (!parts[i].enabled) continue;
    if (windowActive(nowMin, parts[i].startMin, parts[i].endMin)) return (PartId)i;
  }
  return (PartId)-1;
}

// ---------------- HELPERS ----------------
String arrayToCsvLen(const int* arr, int len) {
  String s;
  for (int i = 0; i < len; i++) { if (i) s += ","; s += String(arr[i]); }
  return s;
}
String jsonEscape(const String& in) {
  String out; out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '\\') out += "\\\\";
    else if (c == '"') out += "\\\"";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else out += c;
  }
  return out;
}
bool parseCsvToArrayVarLen(const String& csv, int* out, int maxLen, int& outLen, int minVal, int maxVal) {
  String s = csv; s.trim();
  if (s.length() == 0) return false;
  int count = 0, start = 0;
  for (int i = 0; i <= (int)s.length(); i++) {
    if (i == (int)s.length() || s[i] == ',') {
      String token = s.substring(start, i); token.trim();
      if (token.length() == 0) return false;
      if (count >= maxLen) return false;
      int v = token.toInt();
      if (!(token == "0" || v != 0)) return false;
      if (v < minVal || v > maxVal) return false;
      out[count++] = v;
      start = i + 1;
    }
  }
  if (count <= 0) return false;
  outLen = count;
  return true;
}
bool hasDuplicates(const int* arr, int len) {
  for (int i = 0; i < len; i++) for (int j = i + 1; j < len; j++) if (arr[i] == arr[j]) return true;
  return false;
}
const char* seqStateToString(SequenceState st) {
  switch (st) {
    case IDLE: return "IDLE";
    case RUN_MOTOR: return "RUN_MOTOR";
    case WAIT_AFTER_PILL: return "WAIT_AFTER_PILL";
    case RUN_PUMP: return "RUN_PUMP";
    default: return "UNKNOWN";
  }
}

// ---------------- DEFAULTS ----------------
void initPartsDefaults() {
  parts[PART_MORNING].enabled = false;
  parts[PART_MORNING].startMin = 6 * 60;
  parts[PART_MORNING].endMin   = 10 * 60;
  parts[PART_MORNING].motLen = 3;
  parts[PART_MORNING].motSeq[0]=0; parts[PART_MORNING].motSeq[1]=1; parts[PART_MORNING].motSeq[2]=2;
  parts[PART_MORNING].pumLen = 2;
  parts[PART_MORNING].pumSeq[0]=1; parts[PART_MORNING].pumSeq[1]=0;
  parts[PART_MORNING].pumpLevel[0]=1; parts[PART_MORNING].pumpLevel[1]=0;

  parts[PART_DAY].enabled = false;
  parts[PART_DAY].startMin = 10 * 60;
  parts[PART_DAY].endMin   = 18 * 60;
  parts[PART_DAY].motLen = 2;
  parts[PART_DAY].motSeq[0]=0; parts[PART_DAY].motSeq[1]=2;
  parts[PART_DAY].pumLen = 1;
  parts[PART_DAY].pumSeq[0]=1;
  parts[PART_DAY].pumpLevel[0]=0; parts[PART_DAY].pumpLevel[1]=1;

  parts[PART_NIGHT].enabled = false;
  parts[PART_NIGHT].startMin = 18 * 60;
  parts[PART_NIGHT].endMin   = 6 * 60; // crosses midnight
  parts[PART_NIGHT].motLen = 1;
  parts[PART_NIGHT].motSeq[0]=1;
  parts[PART_NIGHT].pumLen = 1;
  parts[PART_NIGHT].pumSeq[0]=0;
  parts[PART_NIGHT].pumpLevel[0]=2; parts[PART_NIGHT].pumpLevel[1]=2;
}

// ---------------- NVS: save/load ALL SETTINGS ----------------
void nvsSaveDefaults() {
  prefs.begin(PREF_NS, false);

  prefs.putInt("motLen", motorSeqLen);
  prefs.putInt("mot0", motorSequence[0]);
  prefs.putInt("mot1", motorSequence[1]);
  prefs.putInt("mot2", motorSequence[2]);

  prefs.putInt("pumLen", pumpSeqLen);
  prefs.putInt("pum0", pumpSequence[0]);
  prefs.putInt("pum1", pumpSequence[1]);

  prefs.putInt("pl0", pumpLevelSelection[0]);
  prefs.putInt("pl1", pumpLevelSelection[1]);

  prefs.putInt("t0", pillTarget[0]);
  prefs.putInt("t1", pillTarget[1]);
  prefs.putInt("t2", pillTarget[2]);
  prefs.putInt("r0", pillRemaining[0]);
  prefs.putInt("r1", pillRemaining[1]);
  prefs.putInt("r2", pillRemaining[2]);

  prefs.putBool("hasAll", true);
  prefs.end();
}

void nvsLoadDefaults() {
  prefs.begin(PREF_NS, true);
  bool has = prefs.getBool("hasAll", false);
  if (has) {
    motorSeqLen = prefs.getInt("motLen", motorSeqLen);
    if (motorSeqLen < 1) motorSeqLen = 1;
    if (motorSeqLen > MAX_MOTORS) motorSeqLen = MAX_MOTORS;
    motorSequence[0] = prefs.getInt("mot0", motorSequence[0]);
    motorSequence[1] = prefs.getInt("mot1", motorSequence[1]);
    motorSequence[2] = prefs.getInt("mot2", motorSequence[2]);

    pumpSeqLen = prefs.getInt("pumLen", pumpSeqLen);
    if (pumpSeqLen < 1) pumpSeqLen = 1;
    if (pumpSeqLen > MAX_PUMPS) pumpSeqLen = MAX_PUMPS;
    pumpSequence[0] = prefs.getInt("pum0", pumpSequence[0]);
    pumpSequence[1] = prefs.getInt("pum1", pumpSequence[1]);

    pumpLevelSelection[0] = prefs.getInt("pl0", pumpLevelSelection[0]);
    pumpLevelSelection[1] = prefs.getInt("pl1", pumpLevelSelection[1]);

    pillTarget[0] = prefs.getInt("t0", pillTarget[0]);
    pillTarget[1] = prefs.getInt("t1", pillTarget[1]);
    pillTarget[2] = prefs.getInt("t2", pillTarget[2]);
    pillRemaining[0] = prefs.getInt("r0", pillRemaining[0]);
    pillRemaining[1] = prefs.getInt("r1", pillRemaining[1]);
    pillRemaining[2] = prefs.getInt("r2", pillRemaining[2]);
  }
  prefs.end();
}

void nvsSaveParts() {
  prefs.begin(PREF_NS, false);
  prefs.putBool("hasParts", true);

  for (int i = 0; i < 3; i++) {
    String p = "p" + String(i) + "_";
    prefs.putBool((p + "en").c_str(), parts[i].enabled);
    prefs.putInt((p + "s").c_str(), parts[i].startMin);
    prefs.putInt((p + "e").c_str(), parts[i].endMin);

    prefs.putInt((p + "ml").c_str(), parts[i].motLen);
    for (int k = 0; k < parts[i].motLen; k++) prefs.putInt((p + "m" + String(k)).c_str(), parts[i].motSeq[k]);

    prefs.putInt((p + "pl").c_str(), parts[i].pumLen);
    for (int k = 0; k < parts[i].pumLen; k++) prefs.putInt((p + "pu" + String(k)).c_str(), parts[i].pumSeq[k]);

    prefs.putInt((p + "l0").c_str(), parts[i].pumpLevel[0]);
    prefs.putInt((p + "l1").c_str(), parts[i].pumpLevel[1]);
  }
  prefs.end();
}

void nvsLoadParts() {
  prefs.begin(PREF_NS, true);
  bool has = prefs.getBool("hasParts", false);
  if (has) {
    for (int i = 0; i < 3; i++) {
      String p = "p" + String(i) + "_";
      parts[i].enabled = prefs.getBool((p + "en").c_str(), parts[i].enabled);
      parts[i].startMin = prefs.getInt((p + "s").c_str(), parts[i].startMin);
      parts[i].endMin   = prefs.getInt((p + "e").c_str(), parts[i].endMin);

      parts[i].motLen = prefs.getInt((p + "ml").c_str(), parts[i].motLen);
      if (parts[i].motLen < 1) parts[i].motLen = 1;
      if (parts[i].motLen > MAX_MOTORS) parts[i].motLen = MAX_MOTORS;
      for (int k = 0; k < parts[i].motLen; k++) parts[i].motSeq[k] = prefs.getInt((p + "m" + String(k)).c_str(), parts[i].motSeq[k]);

      parts[i].pumLen = prefs.getInt((p + "pl").c_str(), parts[i].pumLen);
      if (parts[i].pumLen < 1) parts[i].pumLen = 1;
      if (parts[i].pumLen > MAX_PUMPS) parts[i].pumLen = MAX_PUMPS;
      for (int k = 0; k < parts[i].pumLen; k++) parts[i].pumSeq[k] = prefs.getInt((p + "pu" + String(k)).c_str(), parts[i].pumSeq[k]);

      parts[i].pumpLevel[0] = prefs.getInt((p + "l0").c_str(), parts[i].pumpLevel[0]);
      parts[i].pumpLevel[1] = prefs.getInt((p + "l1").c_str(), parts[i].pumpLevel[1]);
    }
  }
  prefs.end();
}

// ---------------- Runtime picking ----------------
void loadRuntimeFromDefault() {
  runtimeMotLen = motorSeqLen;
  runtimePumLen = pumpSeqLen;
  for (int i = 0; i < runtimeMotLen; i++) runtimeMotSeq[i] = motorSequence[i];
  for (int i = 0; i < runtimePumLen; i++) runtimePumSeq[i] = pumpSequence[i];
  runtimePumpLevel[0] = pumpLevelSelection[0];
  runtimePumpLevel[1] = pumpLevelSelection[1];
  runtimeSource = "DEFAULT";
}
void loadRuntimeFromPart(PartId id) {
  PartConfig &p = parts[(int)id];
  runtimeMotLen = p.motLen;
  runtimePumLen = p.pumLen;
  for (int i = 0; i < runtimeMotLen; i++) runtimeMotSeq[i] = p.motSeq[i];
  for (int i = 0; i < runtimePumLen; i++) runtimePumSeq[i] = p.pumSeq[i];
  runtimePumpLevel[0] = p.pumpLevel[0];
  runtimePumpLevel[1] = p.pumpLevel[1];
  runtimeSource = partName(id);
}
void pickRuntimeForButton() {
  loadRuntimeFromDefault();
  if (!timeReady()) return;
  PartId ap = activePart();
  if ((int)ap >= 0) loadRuntimeFromPart(ap);
}

// ---------------- FULL HTML (your dashboard) ----------------
String buildRootPage() {
  // This is the full dashboard HTML you already built in previous messages,
  // including: schedules + pills progress + pump levels + builder.
  // (Kept as-is here.)
  String page = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>ESP32 Dashboard</title>
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <style>
    :root{
      --bg:#0b1220; --card:#111a2e; --muted:#7e8aa7; --text:#e7ecff;
      --accent:#5b8cff; --ok:#20d17a; --warn:#ffb020; --bad:#ff4d4d;
      --border:rgba(255,255,255,0.08); --shadow: 0 10px 30px rgba(0,0,0,.35);
      --radius:16px;
    }
    body{
      margin:0; font-family: system-ui, Segoe UI, Roboto, Arial;
      background: radial-gradient(1200px 500px at 20% -10%, rgba(91,140,255,.35), transparent 60%),
                  radial-gradient(900px 500px at 90% 0%, rgba(32,209,122,.25), transparent 55%),
                  var(--bg);
      color:var(--text); padding:16px;
    }
    .wrap{max-width: 1180px; margin: 0 auto;}
    .topbar{
      display:flex; gap:12px; align-items:center; justify-content:space-between;
      padding:14px 16px; border:1px solid var(--border); background: rgba(17,26,46,.75);
      backdrop-filter: blur(10px); border-radius: var(--radius);
      box-shadow: var(--shadow); margin-bottom: 14px;
    }
    .title{display:flex; flex-direction:column; line-height:1.1;}
    .title h1{font-size:18px; margin:0;}
    .title small{color:var(--muted);}
    .pill{font-size:12px; padding:6px 10px; border-radius:999px; border:1px solid var(--border); color:var(--muted);}

    .grid{display:grid; grid-template-columns: repeat(12, 1fr); gap: 14px;}
    .card{
      grid-column: span 6; padding:14px 16px; border:1px solid var(--border);
      background: rgba(17,26,46,.78); border-radius: var(--radius);
      box-shadow: var(--shadow); backdrop-filter: blur(10px);
    }
    .card.full{grid-column: span 12;}
    .card h2{font-size:14px; margin:0 0 10px 0; color:#dbe4ff;}
    .row{display:flex; justify-content:space-between; align-items:center; padding:10px 0; border-top:1px solid var(--border);}
    .row:first-of-type{border-top:0;}
    .label{color:var(--muted); font-size:13px;}
    .badge{padding:6px 10px; border-radius:999px; font-size:12px; font-weight:800; border:1px solid var(--border); user-select:none;}
    .idle{background: rgba(126,138,167,.12); color:#cbd3ee;}
    .running{background: rgba(32,209,122,.15); color: var(--ok); border-color: rgba(32,209,122,.35);}
    .on{background: rgba(91,140,255,.15); color: #9db8ff; border-color: rgba(91,140,255,.35);}
    .warn{background: rgba(255,176,32,.12); color: var(--warn); border-color: rgba(255,176,32,.35);}
    .sub{color:var(--muted); font-size:12px;}

    input, select{
      width: 100%;
      padding:10px 12px;
      border-radius: 12px;
      border:1px solid var(--border);
      background: rgba(0,0,0,.18);
      color: var(--text);
      outline:none;
    }

    .btnrow{display:flex; flex-wrap:wrap; gap:10px; align-items:center; margin-top: 10px;}
    button{appearance:none; border:0; padding:10px 12px; border-radius: 12px; background: linear-gradient(180deg, rgba(91,140,255,.95), rgba(91,140,255,.70)); color:white; font-weight:900; cursor:pointer; box-shadow: 0 10px 20px rgba(91,140,255,.18);}
    button.secondary{background: rgba(255,255,255,.08); border:1px solid var(--border); box-shadow:none;}
    button:disabled{opacity:.55; cursor:not-allowed;}

    /* Progress bars */
    .motorCards{display:grid; grid-template-columns: repeat(3, 1fr); gap: 10px;}
    .mcard{padding:12px; border-radius: 14px; border:1px solid var(--border); background: rgba(255,255,255,.05);}
    .mhead{display:flex; justify-content:space-between; align-items:center; margin-bottom:8px;}
    .mname{font-weight:900; font-size:13px;}
    .bar{height:10px; border-radius:999px; background: rgba(255,255,255,.08); overflow:hidden; border:1px solid rgba(255,255,255,.08);}
    .bar > div{height:100%; width:0%; background: linear-gradient(90deg, rgba(32,209,122,.95), rgba(91,140,255,.95)); transition: width .35s ease;}
    .mmeta{display:flex; justify-content:space-between; margin-top:8px; font-size:12px; color: var(--muted);}

    /* Stepper */
    .stepperRow{display:flex; gap:8px; align-items:center; margin-top:10px;}
    .stepper{
      display:grid; grid-template-columns: 36px 1fr 36px; align-items:center;
      border-radius: 12px; border:1px solid var(--border); overflow:hidden;
      background: rgba(0,0,0,.16); max-width: 240px;
    }
    .stepper button{height:36px; width:36px; border:0; background: rgba(255,255,255,.08); color: var(--text); font-weight:900; cursor:pointer; box-shadow:none;}
    .stepper input{height:36px; border:0; outline:none; text-align:center; background: transparent; color: var(--text); font-weight:900;}

    /* Builder */
    .twoCol{display:grid; grid-template-columns: 1fr 1fr; gap: 14px;}
    .field label{display:block; font-size:12px; color:var(--muted); margin-bottom:6px;}
    .field input.csv{width:100%; padding:10px 12px; border-radius: 12px; border:1px solid var(--border); background: rgba(0,0,0,.18); color: var(--text); outline:none;}
    .tiles{display:flex; gap:10px; flex-wrap:wrap; margin-top:10px;}
    .tile{width:130px; padding:12px; border-radius: 14px; border:1px solid var(--border); background: rgba(255,255,255,.06); cursor:pointer; user-select:none;}
    .tile:hover{border-color: rgba(91,140,255,.55);}
    .tile strong{display:block; font-size:13px;}
    .tile small{color:var(--muted);}
    .list{margin-top:10px; display:flex; gap:8px; flex-wrap:wrap; min-height:44px; padding:10px; border-radius: 14px; border: 1px dashed rgba(255,255,255,.18); background: rgba(0,0,0,.10);}
    .pillItem{padding:8px 10px; border-radius:999px; border:1px solid var(--border); background: rgba(255,255,255,.07); cursor:grab; user-select:none; font-size:12px; font-weight:800;}
    .pillItem.dragging{opacity:.55; cursor:grabbing;}
    .hint{color:var(--muted); font-size:12px; margin-top:6px;}

    /* Schedules */
    .schedGrid{
      display:grid;
      grid-template-columns: 110px 170px 1fr 1fr 210px 90px;
      gap:10px;
      align-items:start;
      margin-top:10px;
    }
    .hdr{color:var(--muted); font-size:12px;}
    .tag{font-weight:900; font-size:13px;}
    .timeCol{display:grid; grid-template-columns: 1fr; gap:6px;}
    .lvlRow{display:grid; grid-template-columns: 1fr 1fr; gap:8px;}
    .toggleWrap{display:flex; justify-content:center; align-items:center; height:100%;}
    .toggle{width:46px;height:26px;border-radius:999px;border:1px solid var(--border);background:rgba(255,255,255,.08);position:relative;cursor:pointer;}
    .knob{position:absolute;top:3px;left:3px;width:20px;height:20px;border-radius:999px;background:rgba(255,255,255,.85);transition:left .2s;}
    .toggle.on{background:rgba(32,209,122,.18);border-color:rgba(32,209,122,.35);}
    .toggle.on .knob{left:23px;background:rgba(32,209,122,.95);}

    #toast{
      position: fixed; left: 16px; bottom: 16px;
      max-width: 560px; padding: 12px 14px; border-radius: 14px;
      border: 1px solid var(--border); background: rgba(17,26,46,.92);
      color: var(--text); box-shadow: var(--shadow); display:none; z-index: 1000;
    }
    #toast.ok{border-color: rgba(32,209,122,.35);}
    #toast.bad{border-color: rgba(255,77,77,.35);}
    #toast small{color: var(--muted); display:block; margin-top:4px;}

    @media (max-width: 980px){
      .card{grid-column: span 12;}
      .twoCol{grid-template-columns: 1fr;}
      .motorCards{grid-template-columns: 1fr;}
      .schedGrid{grid-template-columns: 1fr;}
    }
  </style>
</head>
<body>
<div class="wrap">
  <div class="topbar">
    <div class="title">
      <h1>ESP32 Motor & Pump Dashboard</h1>
      <small id="subtitle">Connecting...</small>
    </div>
    <div class="pill" id="ipPill">IP: --</div>
  </div>

  <div class="grid">

    <div class="card full">
      <h2>Schedules (Morning / Day / Night)</h2>
      <div class="sub">If a schedule is enabled and current time is inside its window, pressing the device BUTTON runs that schedule. Otherwise it runs DEFAULT.</div>

      <div class="row"><div class="label">Current time</div><div class="badge on" id="nowTime">--:--</div></div>
      <div class="row"><div class="label">Active schedule</div><div class="badge warn" id="activePart">NONE</div></div>
      <div class="row"><div class="label">Runtime source (used on last/next BUTTON press)</div><div class="badge on" id="runtimeSource">DEFAULT</div></div>

      <div class="schedGrid">
        <div class="hdr">Part</div><div class="hdr">Start / End</div><div class="hdr">Motor seq</div><div class="hdr">Pump seq</div><div class="hdr">Pump level (P0,P1)</div><div class="hdr" style="text-align:center;">Enable</div>

        <div class="tag">Morning</div>
        <div class="timeCol"><input id="mStart" type="time"><input id="mEnd" type="time"></div>
        <div><input id="mMot" placeholder="0,1,2"></div>
        <div><input id="mPump" placeholder="1,0"></div>
        <div class="lvlRow"><select id="mL0"><option value="0">L1</option><option value="1">L2</option><option value="2">L3</option></select><select id="mL1"><option value="0">L1</option><option value="1">L2</option><option value="2">L3</option></select></div>
        <div class="toggleWrap"><div id="mEn" class="toggle" onclick="toggle('mEn')"><div class="knob"></div></div></div>

        <div class="tag">Day</div>
        <div class="timeCol"><input id="dStart" type="time"><input id="dEnd" type="time"></div>
        <div><input id="dMot" placeholder="0,2"></div>
        <div><input id="dPump" placeholder="1"></div>
        <div class="lvlRow"><select id="dL0"><option value="0">L1</option><option value="1">L2</option><option value="2">L3</option></select><select id="dL1"><option value="0">L1</option><option value="1">L2</option><option value="2">L3</option></select></div>
        <div class="toggleWrap"><div id="dEn" class="toggle" onclick="toggle('dEn')"><div class="knob"></div></div></div>

        <div class="tag">Night</div>
        <div class="timeCol"><input id="nStart" type="time"><input id="nEnd" type="time"></div>
        <div><input id="nMot" placeholder="1"></div>
        <div><input id="nPump" placeholder="0"></div>
        <div class="lvlRow"><select id="nL0"><option value="0">L1</option><option value="1">L2</option><option value="2">L3</option></select><select id="nL1"><option value="0">L1</option><option value="1">L2</option><option value="2">L3</option></select></div>
        <div class="toggleWrap"><div id="nEn" class="toggle" onclick="toggle('nEn')"><div class="knob"></div></div></div>
      </div>

      <div class="btnrow">
        <button type="button" onclick="saveParts()">Save Schedules</button>
        <button type="button" class="secondary" onclick="loadParts()">Reload from ESP</button>
      </div>
    </div>

    <div class="card full">
      <h2>Pills Remaining (progress)</h2>
      <div class="motorCards">
        <div class="mcard"><div class="mhead"><div class="mname">Motor 0</div><div id="m0Badge" class="badge idle">Idle</div></div><div class="bar"><div id="m0Bar"></div></div><div class="mmeta"><span id="m0Txt">--/--</span><span class="sub">Remaining/Target</span></div></div>
        <div class="mcard"><div class="mhead"><div class="mname">Motor 1</div><div id="m1Badge" class="badge idle">Idle</div></div><div class="bar"><div id="m1Bar"></div></div><div class="mmeta"><span id="m1Txt">--/--</span><span class="sub">Remaining/Target</span></div></div>
        <div class="mcard"><div class="mhead"><div class="mname">Motor 2</div><div id="m2Badge" class="badge idle">Idle</div></div><div class="bar"><div id="m2Bar"></div></div><div class="mmeta"><span id="m2Txt">--/--</span><span class="sub">Remaining/Target</span></div></div>
      </div>
      <div class="sub" style="margin-top:10px;">Bars show Remaining/Target. Remaining decreases by 1 on pill detect.</div>
    </div>

    <div class="card">
      <h2>Motors</h2>
      <div class="row"><div class="label">Motor 0</div><div id="motor0" class="badge idle">Idle</div></div>
      <div class="row"><div class="label">Motor 1</div><div id="motor1" class="badge idle">Idle</div></div>
      <div class="row"><div class="label">Motor 2</div><div id="motor2" class="badge idle">Idle</div></div>
      <div class="row"><div class="label">Configured sequence</div><div id="curMotSeq" class="badge on">--</div></div>
    </div>

    <div class="card">
      <h2>Pumps</h2>
      <div class="row"><div class="label">Pump 0</div><div id="pump0" class="badge idle">Idle</div></div>
      <div class="row"><div class="label">Pump 1</div><div id="pump1" class="badge idle">Idle</div></div>
      <div class="row"><div class="label">Configured sequence</div><div id="curPumpSeq" class="badge on">--</div></div>
      <div class="row"><div class="label">Pump 0 Level</div><div id="pump0LevelBadge" class="badge warn">--</div></div>
      <div class="row"><div class="label">Pump 1 Level</div><div id="pump1LevelBadge" class="badge warn">--</div></div>
    </div>

    <div class="card">
      <h2>Environment</h2>
      <div class="row"><div class="label">State</div><div id="stateBadge" class="badge warn">--</div></div>
      <div style="display:flex; gap:12px; align-items:baseline; margin-top:8px;">
        <div style="font-size:28px; font-weight:900;" id="temp">--</div><div class="sub">°C</div>
      </div>
      <div class="sub">Humidity: <span id="hum">--</span>%</div>
      <div class="sub" id="uiMsgInline">--</div>
    </div>

    <div class="card">
      <h2>Pill Targets (Stepper Controls)</h2>

      <div class="stepperRow">
        <div class="label" style="width:70px;">Motor 0</div>
        <div class="stepper">
          <button type="button" onclick="step('pillM0',-1)">-</button>
          <input id="pillM0" type="number" min="0" step="1" value="0">
          <button type="button" onclick="step('pillM0',+1)">+</button>
        </div>
      </div>

      <div class="stepperRow">
        <div class="label" style="width:70px;">Motor 1</div>
        <div class="stepper">
          <button type="button" onclick="step('pillM1',-1)">-</button>
          <input id="pillM1" type="number" min="0" step="1" value="0">
          <button type="button" onclick="step('pillM1',+1)">+</button>
        </div>
      </div>

      <div class="stepperRow">
        <div class="label" style="width:70px;">Motor 2</div>
        <div class="stepper">
          <button type="button" onclick="step('pillM2',-1)">-</button>
          <input id="pillM2" type="number" min="0" step="1" value="0">
          <button type="button" onclick="step('pillM2',+1)">+</button>
        </div>
      </div>

      <div class="btnrow">
        <button type="button" onclick="setPillTargets()">Set Targets</button>
        <button type="button" class="secondary" onclick="resetRemainingToTargets()">Reset Remaining</button>
      </div>

      <div class="sub">Setting targets also resets remaining to the same values.</div>
    </div>

    <div class="card">
      <h2>Pump Levels</h2>
      <div class="sub">Choose level for each pump: L1 short, L2 medium, L3 long.</div>

      <div class="field" style="margin-top:10px;">
        <label>Pump 0 Level</label>
        <select id="pump0Level">
          <option value="0">Level 1 (Short)</option>
          <option value="1">Level 2 (Medium)</option>
          <option value="2">Level 3 (Long)</option>
        </select>
      </div>

      <div class="field" style="margin-top:10px;">
        <label>Pump 1 Level</label>
        <select id="pump1Level">
          <option value="0">Level 1 (Short)</option>
          <option value="1">Level 2 (Medium)</option>
          <option value="2">Level 3 (Long)</option>
        </select>
      </div>

      <div class="btnrow">
        <button type="button" onclick="setPumpLevels()">Set Pump Levels</button>
      </div>
      <div class="sub">Levels affect pump runtime during the pump sequence.</div>
    </div>

    <div class="card full">
      <h2>Control (Builder + Drag & Drop + CSV)</h2>

      <div class="twoCol">
        <div>
          <div class="field">
            <label>Motor Sequence (CSV, 1..3 items 0-2)</label>
            <input id="motorsInput" class="csv" type="text" placeholder="e.g. 0,1 or 2,1,0">
          </div>

          <div class="tiles">
            <div class="tile" onclick="addMot(0)"><strong>Add Motor 0</strong><small>Tap to append</small></div>
            <div class="tile" onclick="addMot(1)"><strong>Add Motor 1</strong><small>Tap to append</small></div>
            <div class="tile" onclick="addMot(2)"><strong>Add Motor 2</strong><small>Tap to append</small></div>
          </div>

          <div class="hint">Drag to reorder. Click a pill to remove.</div>
          <div id="motList" class="list"></div>

          <div class="btnrow">
            <button type="button" class="secondary" onclick="motClear()">Clear</button>
            <button type="button" class="secondary" onclick="motUndo()">Undo</button>
          </div>
        </div>

        <div>
          <div class="field">
            <label>Pump Sequence (CSV, 1..2 items 0-1)</label>
            <input id="pumpsInput" class="csv" type="text" placeholder="e.g. 1 or 1,0">
          </div>

          <div class="tiles">
            <div class="tile" onclick="addPump(0)"><strong>Add Pump 0</strong><small>Tap to append</small></div>
            <div class="tile" onclick="addPump(1)"><strong>Add Pump 1</strong><small>Tap to append</small></div>
          </div>

          <div class="hint">Drag to reorder. Click a pill to remove.</div>
          <div id="pumpList" class="list"></div>

          <div class="btnrow">
            <button type="button" class="secondary" onclick="pumpClear()">Clear</button>
            <button type="button" class="secondary" onclick="pumpUndo()">Undo</button>
          </div>
        </div>
      </div>

      <div class="btnrow">
        <button id="btnApply" type="button" onclick="applyFromBuilder()">Apply Sequence</button>
        <button id="btnStart" type="button" onclick="startSequence()">Start Sequence</button>
        <button type="button" class="secondary" onclick="refreshNow()">Refresh</button>
      </div>

      <div class="sub">Tip: Apply Sequence first, then Start.</div>
    </div>

  </div>
</div>

<div id="toast"></div>

<script>
let lastSeqMot = null;
let lastSeqPump = null;

let motOrder = [];
let pumpOrder = [];
let motHistory = [];
let pumpHistory = [];

function toast(msg, ok=true){
  const t = document.getElementById('toast');
  t.className = ok ? 'ok' : 'bad';
  t.innerHTML = msg + '<small>' + new Date().toLocaleTimeString() + '</small>';
  t.style.display = 'block';
  clearTimeout(window.__toastTimer);
  window.__toastTimer = setTimeout(()=> t.style.display='none', 2600);
}
function refreshNow(){ updateStatus(true); }

function step(id, delta){
  const el = document.getElementById(id);
  let v = parseInt(el.value || '0', 10);
  if(Number.isNaN(v)) v = 0;
  v += delta;
  if(v < 0) v = 0;
  el.value = v;
}

function setBadge(id, text, state){
  const el = document.getElementById(id);
  el.textContent = text;
  el.className = 'badge ' + state;
}

function setBar(barId, remain, target){
  const el = document.getElementById(barId);
  const t = Math.max(0, target);
  const r = Math.max(0, remain);
  let pct = 0;
  if(t > 0) pct = Math.max(0, Math.min(100, (r / t) * 100));
  el.style.width = pct.toFixed(1) + '%';
}

function levelLabel(lv){
  if(lv === 0) return "L1 (Short)";
  if(lv === 1) return "L2 (Medium)";
  if(lv === 2) return "L3 (Long)";
  return "--";
}

// ---------------- Builder logic ----------------
function csvToArr(s){
  s = (s || '').trim();
  if(!s) return [];
  return s.split(',').map(x=>x.trim()).filter(x=>x.length).map(x=>parseInt(x,10)).filter(x=>!Number.isNaN(x));
}
function arrToCsv(a){ return a.join(','); }

function pushHistory(){
  motHistory.push(motOrder.slice());
  pumpHistory.push(pumpOrder.slice());
  if(motHistory.length > 20) motHistory.shift();
  if(pumpHistory.length > 20) pumpHistory.shift();
}

function addMot(n){
  if(motOrder.length >= 3) return toast('Motor list max length is 3', false);
  pushHistory(); motOrder.push(n); renderMotList();
}
function addPump(n){
  if(pumpOrder.length >= 2) return toast('Pump list max length is 2', false);
  pushHistory(); pumpOrder.push(n); renderPumpList();
}

function motClear(){ pushHistory(); motOrder=[]; renderMotList(); }
function pumpClear(){ pushHistory(); pumpOrder=[]; renderPumpList(); }
function motUndo(){ if(motHistory.length){ motOrder = motHistory.pop(); renderMotList(); } }
function pumpUndo(){ if(pumpHistory.length){ pumpOrder = pumpHistory.pop(); renderPumpList(); } }

function addDragHandlers(node, arrRef, rerender){
  node.addEventListener('dragstart', (e)=>{
    node.classList.add('dragging');
    e.dataTransfer.setData('text/plain', node.dataset.index);
  });
  node.addEventListener('dragend', ()=> node.classList.remove('dragging'));
  node.addEventListener('dragover', (e)=> e.preventDefault());
  node.addEventListener('drop', (e)=>{
    e.preventDefault();
    const from = parseInt(e.dataTransfer.getData('text/plain'), 10);
    const to = parseInt(node.dataset.index, 10);
    if(Number.isNaN(from) || Number.isNaN(to) || from === to) return;
    const item = arrRef.splice(from,1)[0];
    arrRef.splice(to,0,item);
    rerender();
  });
}

function renderMotList(){
  const el = document.getElementById('motList');
  el.innerHTML = '';
  if(motOrder.length === 0){
    el.innerHTML = '<span class="sub">Empty. Tap motor tiles to add.</span>';
  } else {
    motOrder.forEach((v, idx)=>{
      const p = document.createElement('div');
      p.className = 'pillItem';
      p.draggable = true;
      p.textContent = 'M' + v;
      p.dataset.index = idx;
      p.addEventListener('click', ()=>{
        pushHistory(); motOrder.splice(idx,1); renderMotList();
      });
      addDragHandlers(p, motOrder, renderMotList);
      el.appendChild(p);
    });
  }
  document.getElementById('motorsInput').value = arrToCsv(motOrder);
}

function renderPumpList(){
  const el = document.getElementById('pumpList');
  el.innerHTML = '';
  if(pumpOrder.length === 0){
    el.innerHTML = '<span class="sub">Empty. Tap pump tiles to add.</span>';
  } else {
    pumpOrder.forEach((v, idx)=>{
      const p = document.createElement('div');
      p.className = 'pillItem';
      p.draggable = true;
      p.textContent = 'P' + v;
      p.dataset.index = idx;
      p.addEventListener('click', ()=>{
        pushHistory(); pumpOrder.splice(idx,1); renderPumpList();
      });
      addDragHandlers(p, pumpOrder, renderPumpList);
      el.appendChild(p);
    });
  }
  document.getElementById('pumpsInput').value = arrToCsv(pumpOrder);
}

function validateMot(a){
  if(a.length < 1 || a.length > 3) return 'Motor length must be 1..3';
  for(const v of a) if(v < 0 || v > 2) return 'Motor values must be 0..2';
  if(new Set(a).size !== a.length) return 'Motor sequence cannot contain duplicates';
  return null;
}
function validatePump(a){
  if(a.length < 1 || a.length > 2) return 'Pump length must be 1..2';
  for(const v of a) if(v < 0 || v > 1) return 'Pump values must be 0..1';
  if(new Set(a).size !== a.length) return 'Pump sequence cannot contain duplicates';
  return null;
}

function applyFromBuilder(){
  const errM = validateMot(motOrder);
  if(errM) return toast(errM, false);
  const errP = validatePump(pumpOrder);
  if(errP) return toast(errP, false);

  const motors = arrToCsv(motOrder);
  const pumps  = arrToCsv(pumpOrder);

  fetch(`/setSequence?motors=${encodeURIComponent(motors)}&pumps=${encodeURIComponent(pumps)}`)
    .then(async r => {
      const data = await r.json().catch(()=>({ok:false,msg:"Bad JSON"}));
      if(!r.ok) throw data;
      return data;
    })
    .then(data => { toast(data.msg || "Sequence updated", true); updateStatus(true); })
    .catch(err => toast(err.msg || "Set sequence failed", false));
}

function startSequence(){
  fetch(`/startSequence`)
    .then(async r => {
      const data = await r.json().catch(()=>({ok:false,msg:"Bad JSON"}));
      if(!r.ok) throw data;
      return data;
    })
    .then(data => { toast(data.msg || "Sequence started", true); updateStatus(true); })
    .catch(err => toast(err.msg || "Start failed", false));
}

function setPillTargets(){
  const m0 = parseInt(document.getElementById('pillM0').value, 10);
  const m1 = parseInt(document.getElementById('pillM1').value, 10);
  const m2 = parseInt(document.getElementById('pillM2').value, 10);
  if([m0,m1,m2].some(v => Number.isNaN(v) || v < 0)) return toast('Targets must be >= 0', false);

  fetch(`/setPills?m0=${m0}&m1=${m1}&m2=${m2}`)
    .then(async r => {
      const data = await r.json().catch(()=>({ok:false,msg:"Bad JSON"}));
      if(!r.ok) throw data;
      return data;
    })
    .then(data => { toast(data.msg || "Pill targets updated", true); updateStatus(true); })
    .catch(err => toast(err.msg || "Set pills failed", false));
}

function resetRemainingToTargets(){
  fetch(`/resetPills`)
    .then(async r => {
      const data = await r.json().catch(()=>({ok:false,msg:"Bad JSON"}));
      if(!r.ok) throw data;
      return data;
    })
    .then(data => { toast(data.msg || "Remaining reset", true); updateStatus(true); })
    .catch(err => toast(err.msg || "Reset failed", false));
}

function setPumpLevels(){
  const p0 = parseInt(document.getElementById('pump0Level').value, 10);
  const p1 = parseInt(document.getElementById('pump1Level').value, 10);
  if([p0,p1].some(v => Number.isNaN(v) || v < 0 || v > 2)) return toast('Pump levels must be 0..2', false);

  fetch(`/setPumpLevels?p0=${p0}&p1=${p1}`)
    .then(async r => {
      const data = await r.json().catch(()=>({ok:false,msg:"Bad JSON"}));
      if(!r.ok) throw data;
      return data;
    })
    .then(data => { toast(data.msg || "Pump levels updated", true); updateStatus(true); })
    .catch(err => toast(err.msg || "Set pump levels failed", false));
}

// schedules toggles
function toggle(id){
  const el = document.getElementById(id);
  el.classList.toggle('on');
}
function isOn(id){ return document.getElementById(id).classList.contains('on'); }

function timeToMin(v){
  if(!v || v.indexOf(':')<0) return 0;
  const [h,m] = v.split(':').map(x=>parseInt(x,10));
  if(Number.isNaN(h)||Number.isNaN(m)) return 0;
  return h*60+m;
}
function minToTime(min){
  min = ((min%1440)+1440)%1440;
  const h = Math.floor(min/60);
  const m = min%60;
  return String(h).padStart(2,'0') + ':' + String(m).padStart(2,'0');
}

function fillPart(prefix, p){
  document.getElementById(prefix+'Start').value = minToTime(p.startMin);
  document.getElementById(prefix+'End').value   = minToTime(p.endMin);
  document.getElementById(prefix+'Mot').value   = p.motors;
  document.getElementById(prefix+'Pump').value  = p.pumps;
  document.getElementById(prefix+'L0').value    = p.levels[0];
  document.getElementById(prefix+'L1').value    = p.levels[1];
  document.getElementById(prefix+'En').classList.toggle('on', !!p.enabled);
}

function loadParts(){
  fetch('/getParts')
    .then(r=>r.json())
    .then(data=>{
      fillPart('m', data.morning);
      fillPart('d', data.day);
      fillPart('n', data.night);
    })
    .catch(_=>toast('Failed to load schedules', false));
}

function saveParts(){
  const qs = new URLSearchParams();

  qs.set('m_en', isOn('mEn') ? '1':'0');
  qs.set('m_s', String(timeToMin(document.getElementById('mStart').value)));
  qs.set('m_e', String(timeToMin(document.getElementById('mEnd').value)));
  qs.set('m_m', document.getElementById('mMot').value);
  qs.set('m_p', document.getElementById('mPump').value);
  qs.set('m_l0', document.getElementById('mL0').value);
  qs.set('m_l1', document.getElementById('mL1').value);

  qs.set('d_en', isOn('dEn') ? '1':'0');
  qs.set('d_s', String(timeToMin(document.getElementById('dStart').value)));
  qs.set('d_e', String(timeToMin(document.getElementById('dEnd').value)));
  qs.set('d_m', document.getElementById('dMot').value);
  qs.set('d_p', document.getElementById('dPump').value);
  qs.set('d_l0', document.getElementById('dL0').value);
  qs.set('d_l1', document.getElementById('dL1').value);

  qs.set('n_en', isOn('nEn') ? '1':'0');
  qs.set('n_s', String(timeToMin(document.getElementById('nStart').value)));
  qs.set('n_e', String(timeToMin(document.getElementById('nEnd').value)));
  qs.set('n_m', document.getElementById('nMot').value);
  qs.set('n_p', document.getElementById('nPump').value);
  qs.set('n_l0', document.getElementById('nL0').value);
  qs.set('n_l1', document.getElementById('nL1').value);

  fetch('/setParts?' + qs.toString())
    .then(async r=>{
      const data = await r.json().catch(()=>({ok:false,msg:"Bad JSON"}));
      if(!r.ok) throw data;
      return data;
    })
    .then(data=>toast(data.msg || "Schedules saved", true))
    .catch(err=>toast(err.msg || "Save schedules failed", false));
}

function updateStatus(forceFill=false){
  fetch('/status')
    .then(r => r.json())
    .then(data => {
      document.getElementById('subtitle').textContent = data.timeReady ? 'Time synced (NTP)' : 'Time not ready (NTP syncing...)';
      document.getElementById('ipPill').textContent = 'IP: ' + (data.ip || '--');

      document.getElementById('nowTime').textContent = data.nowHHMM || '--:--';
      document.getElementById('activePart').textContent = data.activePart || 'NONE';
      document.getElementById('runtimeSource').textContent = data.runtimeSource || 'DEFAULT';

      for(let i=0;i<3;i++){
        const st = data.motors[i].status;
        setBadge('motor'+i, st, st === 'Running' ? 'running' : 'idle');
        setBadge('m'+i+'Badge', st, st === 'Running' ? 'running' : 'idle');
      }
      for(let i=0;i<2;i++){
        const st = data.pumps[i].status;
        setBadge('pump'+i, st, st === 'Running' ? 'running' : 'idle');
      }

      document.getElementById('curMotSeq').textContent  = data.sequence.motors;
      document.getElementById('curPumpSeq').textContent = data.sequence.pumps;
      setBadge('stateBadge', data.seqState || '--', (data.seqState === 'IDLE') ? 'idle' : 'warn');

      document.getElementById('temp').textContent = data.dht.temp;
      document.getElementById('hum').textContent  = data.dht.hum;
      document.getElementById('uiMsgInline').textContent = data.uiMsg || '--';

      const tgt = data.pills.target || [0,0,0];
      const rem = data.pills.remaining || [0,0,0];
      for(let i=0;i<3;i++){
        document.getElementById('m'+i+'Txt').textContent = rem[i] + '/' + tgt[i];
        setBar('m'+i+'Bar', rem[i], tgt[i]);
      }
      if(forceFill){
        document.getElementById('pillM0').value = tgt[0];
        document.getElementById('pillM1').value = tgt[1];
        document.getElementById('pillM2').value = tgt[2];
      }

      const pls = data.pumpLevels || [0,0];
      document.getElementById('pump0LevelBadge').textContent = (pls[0]===0?"L1 (Short)":pls[0]===1?"L2 (Medium)":"L3 (Long)");
      document.getElementById('pump1LevelBadge').textContent = (pls[1]===0?"L1 (Short)":pls[1]===1?"L2 (Medium)":"L3 (Long)");
      if(forceFill){
        document.getElementById('pump0Level').value = pls[0];
        document.getElementById('pump1Level').value = pls[1];
      }

      const running = (data.seqState && data.seqState !== 'IDLE');
      document.getElementById('btnStart').disabled = running;
      document.getElementById('btnApply').disabled = running;

      if(forceFill || lastSeqMot !== data.sequence.motors){
        lastSeqMot = data.sequence.motors;
        motOrder = csvToArr(data.sequence.motors);
        renderMotList();
      }
      if(forceFill || lastSeqPump !== data.sequence.pumps){
        lastSeqPump = data.sequence.pumps;
        pumpOrder = csvToArr(data.sequence.pumps);
        renderPumpList();
      }
    })
    .catch(_ => { document.getElementById('subtitle').textContent = 'Disconnected (retrying...)'; });
}

renderMotList();
renderPumpList();
loadParts();
updateStatus(true);
setInterval(()=>updateStatus(false), 1000);
</script>
</body>
</html>
)rawliteral";
  return page;
}

// ---------------- WEB HANDLERS ----------------
void handleRoot() { server.send(200, "text/html", buildRootPage()); }

void handleSetSequence() {
  if (seqState != IDLE) {
    server.send(409, "application/json", "{\"ok\":false,\"msg\":\"Cannot change sequence while running\"}");
    return;
  }
  if (!server.hasArg("motors") || !server.hasArg("pumps")) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"Missing motors/pumps\"}");
    return;
  }

  int newMotors[MAX_MOTORS];
  int newPumps[MAX_PUMPS];
  int newMotorLen = 0;
  int newPumpLen = 0;

  bool ok = parseCsvToArrayVarLen(server.arg("motors"), newMotors, MAX_MOTORS, newMotorLen, 0, 2)
            && !hasDuplicates(newMotors, newMotorLen)
            && parseCsvToArrayVarLen(server.arg("pumps"), newPumps, MAX_PUMPS, newPumpLen, 0, 1)
            && !hasDuplicates(newPumps, newPumpLen);

  if (!ok) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid sequence\"}");
    return;
  }

  motorSeqLen = newMotorLen;
  pumpSeqLen  = newPumpLen;
  for (int i = 0; i < motorSeqLen; i++) motorSequence[i] = newMotors[i];
  for (int i = 0; i < pumpSeqLen; i++)  pumpSequence[i]  = newPumps[i];

  nvsSaveDefaults(); // save

  uiMessage = "Sequence updated (saved).";
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Sequence updated (saved).\"}");
}

void handleSetPills() {
  if (seqState != IDLE) {
    server.send(409, "application/json", "{\"ok\":false,\"msg\":\"Cannot change pills while running\"}");
    return;
  }
  if (!server.hasArg("m0") || !server.hasArg("m1") || !server.hasArg("m2")) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"Missing m0/m1/m2\"}");
    return;
  }
  int m0 = server.arg("m0").toInt();
  int m1 = server.arg("m1").toInt();
  int m2 = server.arg("m2").toInt();
  if (m0 < 0 || m1 < 0 || m2 < 0) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"Targets must be >= 0\"}");
    return;
  }
  pillTarget[0]=m0; pillTarget[1]=m1; pillTarget[2]=m2;
  pillRemaining[0]=m0; pillRemaining[1]=m1; pillRemaining[2]=m2;

  nvsSaveDefaults();

  uiMessage = "Pills updated (saved).";
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Pills updated (saved).\"}");
}

void handleResetPills() {
  if (seqState != IDLE) {
    server.send(409, "application/json", "{\"ok\":false,\"msg\":\"Cannot reset while running\"}");
    return;
  }
  for (int i=0;i<MAX_MOTORS;i++) pillRemaining[i]=pillTarget[i];

  nvsSaveDefaults();

  uiMessage = "Remaining reset (saved).";
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Remaining reset (saved).\"}");
}

void handleSetPumpLevels() {
  if (seqState != IDLE) {
    server.send(409, "application/json", "{\"ok\":false,\"msg\":\"Cannot change pump levels while running\"}");
    return;
  }
  if (!server.hasArg("p0") || !server.hasArg("p1")) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"Missing p0/p1\"}");
    return;
  }
  int p0 = server.arg("p0").toInt();
  int p1 = server.arg("p1").toInt();
  if (p0 < 0 || p0 > 2 || p1 < 0 || p1 > 2) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"Levels must be 0..2\"}");
    return;
  }
  pumpLevelSelection[0]=p0;
  pumpLevelSelection[1]=p1;

  nvsSaveDefaults();

  uiMessage = "Pump levels updated (saved).";
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Pump levels updated (saved).\"}");
}

String partToJson(const PartConfig &p) {
  String json = "{";
  json += "\"enabled\":" + String(p.enabled ? "true":"false") + ",";
  json += "\"startMin\":" + String(p.startMin) + ",";
  json += "\"endMin\":" + String(p.endMin) + ",";
  json += "\"motors\":\"" + arrayToCsvLen(p.motSeq, p.motLen) + "\",";
  json += "\"pumps\":\"" + arrayToCsvLen(p.pumSeq, p.pumLen) + "\",";
  json += "\"levels\":[" + String(p.pumpLevel[0]) + "," + String(p.pumpLevel[1]) + "]";
  json += "}";
  return json;
}

void handleGetParts() {
  String json = "{";
  json += "\"morning\":" + partToJson(parts[PART_MORNING]) + ",";
  json += "\"day\":" + partToJson(parts[PART_DAY]) + ",";
  json += "\"night\":" + partToJson(parts[PART_NIGHT]);
  json += "}";
  server.send(200, "application/json", json);
}

bool applyPartFromArgs(PartId id,
                       const char* enKey, const char* sKey, const char* eKey,
                       const char* mKey, const char* pKey,
                       const char* l0Key, const char* l1Key,
                       String &err) {
  if (!server.hasArg(enKey) || !server.hasArg(sKey) || !server.hasArg(eKey) ||
      !server.hasArg(mKey) || !server.hasArg(pKey) || !server.hasArg(l0Key) || !server.hasArg(l1Key)) {
    err = "Missing schedule args";
    return false;
  }

  PartConfig &pc = parts[(int)id];
  pc.enabled = server.arg(enKey).toInt() == 1;

  pc.startMin = server.arg(sKey).toInt();
  pc.endMin   = server.arg(eKey).toInt();
  if (pc.startMin < 0 || pc.startMin > 1439 || pc.endMin < 0 || pc.endMin > 1439) {
    err = "Invalid start/end";
    return false;
  }

  int newMot[MAX_MOTORS]; int newMotLen=0;
  int newPum[MAX_PUMPS];  int newPumLen=0;

  if (!parseCsvToArrayVarLen(server.arg(mKey), newMot, MAX_MOTORS, newMotLen, 0, 2) || hasDuplicates(newMot, newMotLen)) {
    err = "Invalid motor seq";
    return false;
  }
  if (!parseCsvToArrayVarLen(server.arg(pKey), newPum, MAX_PUMPS, newPumLen, 0, 1) || hasDuplicates(newPum, newPumLen)) {
    err = "Invalid pump seq";
    return false;
  }

  int l0 = server.arg(l0Key).toInt();
  int l1 = server.arg(l1Key).toInt();
  if (l0<0||l0>2||l1<0||l1>2) { err="Invalid pump levels"; return false; }

  pc.motLen = newMotLen;
  for (int i=0;i<newMotLen;i++) pc.motSeq[i]=newMot[i];
  pc.pumLen = newPumLen;
  for (int i=0;i<newPumLen;i++) pc.pumSeq[i]=newPum[i];
  pc.pumpLevel[0]=l0;
  pc.pumpLevel[1]=l1;

  return true;
}

void handleSetParts() {
  if (seqState != IDLE) {
    server.send(409, "application/json", "{\"ok\":false,\"msg\":\"Cannot change schedules while running\"}");
    return;
  }

  String err;
  bool ok =
    applyPartFromArgs(PART_MORNING,"m_en","m_s","m_e","m_m","m_p","m_l0","m_l1",err) &&
    applyPartFromArgs(PART_DAY,"d_en","d_s","d_e","d_m","d_p","d_l0","d_l1",err) &&
    applyPartFromArgs(PART_NIGHT,"n_en","n_s","n_e","n_m","n_p","n_l0","n_l1",err);

  if (!ok) {
    String json = "{\"ok\":false,\"msg\":\"" + jsonEscape(err) + "\"}";
    server.send(400, "application/json", json);
    return;
  }

  nvsSaveParts();

  uiMessage = "Schedules updated (saved).";
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Schedules updated (saved).\"}");
}

void handleStartSequence() {
  if (seqState != IDLE) {
    server.send(409, "application/json", "{\"ok\":false,\"msg\":\"Sequence already running\"}");
    return;
  }

  motorIndex = 0;
  pumpIndex = 0;

  while (motorIndex < motorSeqLen && pillRemaining[motorSequence[motorIndex]] <= 0) motorIndex++;
  if (motorIndex >= motorSeqLen) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"All motors have 0 remaining pills\"}");
    return;
  }

  seqState = RUN_MOTOR;
  motors[motorSequence[motorIndex]].active = true;
  digitalWrite(LED_SEQ_START, HIGH);

  runtimeSource = "DEFAULT";
  uiMessage = "Started (Web).";
  server.send(200, "application/json", "{\"ok\":true,\"msg\":\"Sequence started\"}");
}

void handleStatus() {
  String json = "{\"motors\":[";
  for (int i=0;i<3;i++){
    json += "{\"status\":\"";
    json += motors[i].active ? "Running" : "Idle";
    json += "\"}";
    if (i<2) json += ",";
  }
  json += "],\"pumps\":[";
  for (int i=0;i<2;i++){
    json += "{\"status\":\"";
    if (pumpIndex == i && seqState == RUN_PUMP) json += "Running"; else json += "Idle";
    json += "\"}";
    if (i<1) json += ",";
  }
  json += "],\"dht\":{";
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t)) t=0;
  if (isnan(h)) h=0;
  json += "\"temp\":" + String(t,1) + ",\"hum\":" + String(h,1);
  json += "}";

  json += ",\"sequence\":{";
  json += "\"motors\":\"" + arrayToCsvLen(motorSequence, motorSeqLen) + "\",";
  json += "\"pumps\":\"" + arrayToCsvLen(pumpSequence, pumpSeqLen) + "\"}";
  json += ",\"pumpLevels\":[" + String(pumpLevelSelection[0]) + "," + String(pumpLevelSelection[1]) + "]";

  json += ",\"pills\":{";
  json += "\"target\":[" + String(pillTarget[0]) + "," + String(pillTarget[1]) + "," + String(pillTarget[2]) + "],";
  json += "\"remaining\":[" + String(pillRemaining[0]) + "," + String(pillRemaining[1]) + "," + String(pillRemaining[2]) + "]}";

  json += ",\"uiMsg\":\"" + jsonEscape(uiMessage) + "\"";
  json += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
  json += ",\"seqState\":\"" + String(seqStateToString(seqState)) + "\"";
  json += ",\"timeReady\":" + String(timeReady() ? "true":"false");
  json += ",\"nowHHMM\":\"" + nowHHMM() + "\"";

  String ap = "NONE";
  if (timeReady()) {
    PartId a = activePart();
    if ((int)a >= 0) ap = partName(a);
  } else ap = "TIME_NOT_READY";
  json += ",\"activePart\":\"" + ap + "\"";
  json += ",\"runtimeSource\":\"" + runtimeSource + "\"";

  json += "}";
  server.send(200, "application/json", json);
}

// ---------------- EMERGENCY ----------------
void handleEmergency(unsigned long currentMillis) {
  bool emergency = (digitalRead(EMERGENCY_SWITCH) == LOW);
  if (emergency) {
    if (currentMillis - previousSirenMillis >= 200) {
      previousSirenMillis = currentMillis;
      sirenState = !sirenState;
      digitalWrite(BUZZER_PIN, sirenState);
    }
  } else {
    digitalWrite(BUZZER_PIN, LOW);
    sirenState = false;
  }
}

// ---------------- MOTOR CONTROL ----------------
void stopMotor(int motorNum) {
  digitalWrite(motors[motorNum].in1, LOW);
  digitalWrite(motors[motorNum].in2, LOW);
  motors[motorNum].active = false;
}

// Reverse motor 1 and motor 2 (indexes 1 and 2)
void driveMotorForward(int motorNum) {
  if (motorNum == 1 || motorNum == 2) {
    digitalWrite(motors[motorNum].in1, LOW);
    digitalWrite(motors[motorNum].in2, HIGH);
  } else {
    digitalWrite(motors[motorNum].in1, HIGH);
    digitalWrite(motors[motorNum].in2, LOW);
  }
}

void runMotorUntilPill(int motorNum, unsigned long currentMillis) {
  if (!motors[motorNum].active) return;

  driveMotorForward(motorNum);

  int sensorVal = analogRead(PIEZO);
  if (sensorVal > PIEZO_THRESHOLD && !pillDetected) {
    pillDetected = true;

    stopMotor(motorNum);

    if (pillRemaining[motorNum] > 0) pillRemaining[motorNum]--;

    nvsSaveDefaults(); // save remaining

    uiMessage = "Pill detected on motor " + String(motorNum) + ". Remaining=" + String(pillRemaining[motorNum]);

    pillDelayStart = currentMillis;
    seqState = WAIT_AFTER_PILL;
  }
  if (sensorVal < PIEZO_THRESHOLD) pillDetected = false;
}

// ---------------- PUMP ----------------
void runPumpSequence(unsigned long currentMillis) {
  if (pumpIndex >= pumpSeqLen) {
    digitalWrite(PUMP1_PIN, HIGH);
    digitalWrite(PUMP2_PIN, HIGH);

    digitalWrite(LED_PUMP_START, LOW);
    digitalWrite(LED_PUMP_DONE, HIGH);
    pumpDoneLEDTimer = millis();

    seqState = IDLE;
    uiMessage = "Pump sequence ended.";
    return;
  }

  int pumpNum = pumpSequence[pumpIndex];
  int level = pumpLevelSelection[pumpNum];
  unsigned long runTime = pumpRunTime[pumpNum][level];

  if (currentMillis - pumpStartMillis < runTime) {
    if (pumpNum == 0) digitalWrite(PUMP1_PIN, LOW);
    else digitalWrite(PUMP2_PIN, LOW);
    digitalWrite(LED_PUMP_START, HIGH);
  } else {
    digitalWrite(PUMP1_PIN, HIGH);
    digitalWrite(PUMP2_PIN, HIGH);
    pumpIndex++;
    pumpStartMillis = currentMillis;
  }
}

// ---------------- SETUP ----------------
void setup() {
  Serial.begin(115200);

  initPartsDefaults();
  nvsLoadDefaults();
  nvsLoadParts();

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi Connected. IP: " + WiFi.localIP().toString());

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/setSequence", handleSetSequence);
  server.on("/startSequence", handleStartSequence);
  server.on("/setPills", handleSetPills);
  server.on("/resetPills", handleResetPills);
  server.on("/setPumpLevels", handleSetPumpLevels);
  server.on("/getParts", handleGetParts);
  server.on("/setParts", handleSetParts);
  server.begin();

  esp_now_init();
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  esp_now_add_peer(&peerInfo);

  dht.begin();

  motors[0] = { M1_IN1, M1_IN2, false };
  motors[1] = { M2_IN1, M2_IN2, false };
  motors[2] = { M3_IN1, M3_IN2, false };
  for (int i = 0; i < 3; i++) {
    pinMode(motors[i].in1, OUTPUT);
    pinMode(motors[i].in2, OUTPUT);
    digitalWrite(motors[i].in1, LOW);
    digitalWrite(motors[i].in2, LOW);
  }

  pinMode(STBY1, OUTPUT); digitalWrite(STBY1, HIGH);
  pinMode(STBY2, OUTPUT); digitalWrite(STBY2, HIGH);

  pinMode(PIEZO, INPUT);

  pinMode(PUMP1_PIN, OUTPUT); digitalWrite(PUMP1_PIN, HIGH);
  pinMode(PUMP2_PIN, OUTPUT); digitalWrite(PUMP2_PIN, HIGH);

  pinMode(BUTTON, INPUT_PULLUP);
  pinMode(EMERGENCY_SWITCH, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);

  pinMode(LED_SEQ_START, OUTPUT);
  pinMode(LED_SEQ_DONE, OUTPUT);
  pinMode(LED_PUMP_START, OUTPUT);
  pinMode(LED_PUMP_DONE, OUTPUT);
  digitalWrite(LED_SEQ_START, LOW);
  digitalWrite(LED_SEQ_DONE, LOW);
  digitalWrite(LED_PUMP_START, LOW);
  digitalWrite(LED_PUMP_DONE, LOW);

  uiMessage = "Ready (settings loaded from NVS if available).";
}

// ---------------- LOOP ----------------
void loop() {
  server.handleClient();

  unsigned long currentMillis = millis();
  handleEmergency(currentMillis);

  // BUTTON selects schedule at press time
  bool currentButtonState = digitalRead(BUTTON);
  if (lastButtonState == HIGH && currentButtonState == LOW && seqState == IDLE) {
    pickRuntimeForButton();

    // Run using runtime sequences/levels (copy into globals for state machine)
    motorSeqLen = runtimeMotLen;
    pumpSeqLen = runtimePumLen;
    for (int i=0;i<motorSeqLen;i++) motorSequence[i]=runtimeMotSeq[i];
    for (int i=0;i<pumpSeqLen;i++)  pumpSequence[i]=runtimePumSeq[i];
    pumpLevelSelection[0]=runtimePumpLevel[0];
    pumpLevelSelection[1]=runtimePumpLevel[1];

    motorIndex = 0;
    pumpIndex = 0;

    while (motorIndex < motorSeqLen && pillRemaining[motorSequence[motorIndex]] <= 0) motorIndex++;

    if (motorIndex < motorSeqLen) {
      motors[motorSequence[motorIndex]].active = true;
      seqState = RUN_MOTOR;
      digitalWrite(LED_SEQ_START, HIGH);
      uiMessage = "Started via Button (" + runtimeSource + ").";
    } else {
      uiMessage = "All motors have 0 remaining pills.";
    }
  }
  lastButtonState = currentButtonState;

  switch (seqState) {
    case IDLE: break;

    case RUN_MOTOR:
      runMotorUntilPill(motorSequence[motorIndex], currentMillis);
      break;

    case WAIT_AFTER_PILL:
      if (currentMillis - pillDelayStart >= pillDelay) {
        motorIndex++;
        while (motorIndex < motorSeqLen && pillRemaining[motorSequence[motorIndex]] <= 0) motorIndex++;
        if (motorIndex < motorSeqLen) {
          motors[motorSequence[motorIndex]].active = true;
          seqState = RUN_MOTOR;
        } else {
          pumpStartMillis = currentMillis;
          pumpIndex = 0;
          seqState = RUN_PUMP;
          digitalWrite(LED_SEQ_START, LOW);
          digitalWrite(LED_SEQ_DONE, HIGH);
          seqDoneLEDTimer = currentMillis;
          uiMessage = "Motors done. Starting pumps.";
        }
      }
      break;

    case RUN_PUMP:
      runPumpSequence(currentMillis);
      break;
  }

  if (seqDoneLEDTimer > 0 && currentMillis - seqDoneLEDTimer >= LED_TIMEOUT) {
    digitalWrite(LED_SEQ_DONE, LOW);
    seqDoneLEDTimer = 0;
  }
  if (pumpDoneLEDTimer > 0 && currentMillis - pumpDoneLEDTimer >= LED_TIMEOUT) {
    digitalWrite(LED_PUMP_DONE, LOW);
    pumpDoneLEDTimer = 0;
  }
}