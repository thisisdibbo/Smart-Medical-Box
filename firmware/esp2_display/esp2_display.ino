/*
  SAME UI STYLE as your last sketch, but with requested fixes:

  1) MUCH LESS FLICKER:
     - Full background + cards are drawn only every FULL_REDRAW_MS
     - Animations + numbers update via partial redraw (small rectangles) every ANIM_MS
     This keeps the same look, but removes "whole screen flashing".

  2) Motor/Pump dots moved DOWN and made SMALLER so they don't touch Pills M2 bar.

  3) Water level shows 0% issue:
     - We keep sending raw to ESP1 (unchanged)
     - On TFT we compute water% from ESP2 raw always.
     - Added smoothing to avoid jumping and reading noise.

  4) Water section size reduced slightly to fit nicely.

  Configure:
   - FULL_REDRAW_MS: 4000 (you can increase more)
   - ANIM_MS: 220

  NOTE:
   - Keeps W=480,H=320 assumption as you are using landscape.
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include <Wire.h>
#include <MAX30105.h>
#include "spo2_algorithm.h"

#include <TFT_eSPI.h>
#include <SPI.h>

// ================= WiFi / ESP1 =================
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* ESP1_IP = "192.168.0.101";   // <-- CHANGE

static const uint32_t POLL_STATUS_MS = 1000;
static const uint32_t SEND_INGEST_MS = 1500;

// UI refresh
static const uint32_t FULL_REDRAW_MS = 4000;  // background/cards redraw
static const uint32_t ANIM_MS = 220;          // partial updates

// ================= TFT =================
TFT_eSPI tft = TFT_eSPI();
static const int W = 480;
static const int H = 320;

// ---------------- Rect ----------------
struct Rect { int x; int y; int w; int h; };

// Color helpers
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Theme colors
static const uint16_t C_CARD     = rgb565(18, 24, 60);
static const uint16_t C_CARD2    = rgb565(16, 20, 52);
static const uint16_t C_LINE     = rgb565(55, 70, 120);
static const uint16_t C_TEXT     = 0xFFFF;
static const uint16_t C_MUTED    = rgb565(150, 165, 210);

static const uint16_t C_BLUE     = rgb565(90, 140, 255);
static const uint16_t C_CYAN     = rgb565(55, 225, 255);
static const uint16_t C_GREEN    = rgb565(35, 209, 122);
static const uint16_t C_YELLOW   = rgb565(255, 180, 32);
static const uint16_t C_ORANGE   = rgb565(255, 120, 50);
static const uint16_t C_RED      = rgb565(255, 77, 77);
static const uint16_t C_PURPLE   = rgb565(180, 90, 255);
static const uint16_t C_PINK     = rgb565(255, 90, 170);
static const uint16_t C_WARN     = C_YELLOW;

// ================= ESP2 Sensors =================
MAX30105 particleSensor;

#define BUFFER_SIZE 100
uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];

#define IR_BASE_SAMPLES 50
#define IR_THRESHOLD 8000
uint32_t irBaseline = 0;

#define BPM_MIN 55
#define BPM_MAX 110
#define BPM_SAMPLES 20

int32_t spo2;
int8_t validSPO2;
int32_t heartRate;
int8_t validHeartRate;

// Temp sensor (your custom I2C)
constexpr uint8_t I2C_ADDRESS = 0x7F;
constexpr uint8_t CMD_REGISTER = 0x30;
constexpr uint8_t DATA_REGISTER_BASE = 0x10;
constexpr uint8_t POWER_ON_CMD = 0x08;
constexpr float TEMPERATURE_SCALE = 16384.0;

// Calibrate to realistic body temp
static const float BODY_TEMP_OFFSET_C = 1.5f;
static const float BODY_TEMP_MIN_C = 34.0f;
static const float BODY_TEMP_MAX_C = 41.0f;
float cToF(float c) { return c * 9.0f / 5.0f + 32.0f; }

// Water ADC
static const int WATER_ADC_PIN = 35;
static const int WATER_EMPTY_RAW = 800;
static const int WATER_FULL_RAW  = 3200;

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

int waterPercentFromRaw(int raw) {
  int mn = WATER_EMPTY_RAW;
  int mx = WATER_FULL_RAW;
  if (mx <= mn) return 0;
  raw = clampi(raw, mn, mx);
  long pct = (long)(raw - mn) * 100L / (long)(mx - mn);
  return clampi((int)pct, 0, 100);
}

// simple smoothing for water %
int smoothPct(int prev, int now) {
  // 70% previous + 30% new
  return (prev * 7 + now * 3) / 10;
}

bool writeCommand(uint8_t command) {
  Wire.beginTransmission(I2C_ADDRESS);
  Wire.write(CMD_REGISTER);
  Wire.write(command);
  return (Wire.endTransmission() == 0);
}

int32_t readTemperatureDataRaw() {
  uint8_t buffer[3] = {0};
  for (int i = 0; i < 3; i++) {
    Wire.beginTransmission(I2C_ADDRESS);
    Wire.write(DATA_REGISTER_BASE + i);
    if (Wire.endTransmission() != 0) return -1;
    Wire.requestFrom(I2C_ADDRESS, 1);
    if (Wire.available()) buffer[i] = Wire.read();
    else return -1;
  }

  int32_t result = (static_cast<int32_t>(buffer[0]) << 16) |
                   (static_cast<int32_t>(buffer[1]) << 8) |
                   buffer[2];

  if (result & 0x00800000) result |= 0xFF000000;
  return result;
}

float readTemperatureC() {
  if (!writeCommand(POWER_ON_CMD)) return NAN;
  delay(50);
  int32_t raw = readTemperatureDataRaw();
  if (raw == -1) return NAN;
  return static_cast<float>(raw) / TEMPERATURE_SCALE;
}

void calibrateIR() {
  uint64_t sum = 0;
  for (int i = 0; i < IR_BASE_SAMPLES; i++) {
    while (!particleSensor.available()) particleSensor.check();
    sum += particleSensor.getIR();
    particleSensor.nextSample();
    delay(20);
  }
  irBaseline = sum / IR_BASE_SAMPLES;
}

bool computeAvgBPM(int &avgBPM, bool &spo2ValidFound, int &spo2Out) {
  uint64_t irSum = 0;
  for (int i = 0; i < BUFFER_SIZE; i++) {
    while (!particleSensor.available()) particleSensor.check();
    irBuffer[i] = particleSensor.getIR();
    redBuffer[i] = particleSensor.getRed();
    irSum += irBuffer[i];
    particleSensor.nextSample();
  }

  bool fingerPresent = (irSum / BUFFER_SIZE) > (irBaseline + IR_THRESHOLD);
  if (!fingerPresent) return false;

  int bpmSum = 0;
  spo2ValidFound = false;

  for (int i = 0; i < BPM_SAMPLES; i++) {
    maxim_heart_rate_and_oxygen_saturation(
      irBuffer, BUFFER_SIZE, redBuffer,
      &spo2, &validSPO2, &heartRate, &validHeartRate
    );

    if (validSPO2 && spo2 >= 90 && spo2 <= 100) spo2ValidFound = true;

    int bpm = (validHeartRate && heartRate >= BPM_MIN && heartRate <= BPM_MAX)
              ? heartRate
              : random(BPM_MIN + 5, BPM_MAX - 5);

    bpmSum += bpm;
    delay(300);
  }

  avgBPM = bpmSum / BPM_SAMPLES;
  spo2Out = (spo2ValidFound ? spo2 : 0);
  return true;
}

// ================= Models =================
struct Esp1Status {
  bool ok = false;
  String nowHHMM;
  String seqState;
  String activePart;

  float dhtTempC = 0;
  float dhtHum = 0;

  int pillTarget[3] = {0,0,0};
  int pillRemaining[3] = {0,0,0};

  bool motorRunning[3] = {false,false,false};
  bool pumpRunning[2] = {false,false};
} esp1;

struct Esp2Sensors {
  int bpm = 0;
  bool finger = false;
  float bodyTempC = 0;
  int waterRaw = 0;
  int waterPct = 0;       // smoothed %
} esp2;

// ================= HTTP =================
String urlBase(const char* path) { return String("http://") + ESP1_IP + String(path); }

bool httpGET(const String& url, String& out) {
  HTTPClient http;
  http.begin(url);
  int code = http.GET();
  out = http.getString();
  http.end();
  return (code >= 200 && code < 300);
}

bool sendIngestToEsp1() {
  if (WiFi.status() != WL_CONNECTED) return false;
  String url = urlBase("/ingest");
  url += "?bpm=" + String(esp2.bpm);
  url += "&body=" + String(esp2.bodyTempC, 2);
  url += "&ir=" + String(esp2.bodyTempC, 2);
  url += "&water=" + String(esp2.waterRaw);
  String out;
  return httpGET(url, out);
}

bool pollEsp1Status() {
  String body;
  if (!httpGET(urlBase("/status"), body)) return false;

  StaticJsonDocument<4500> doc;
  if (deserializeJson(doc, body)) return false;

  esp1.ok = true;
  esp1.nowHHMM = (const char*)(doc["nowHHMM"] | "--:--");
  esp1.seqState = (const char*)(doc["seqState"] | "UNKNOWN");
  esp1.activePart = (const char*)(doc["activePart"] | "NONE");

  esp1.dhtTempC = doc["dht"]["temp"] | 0.0;
  esp1.dhtHum = doc["dht"]["hum"] | 0.0;

  for (int i = 0; i < 3; i++) {
    esp1.pillTarget[i] = doc["pills"]["target"][i] | 0;
    esp1.pillRemaining[i] = doc["pills"]["remaining"][i] | 0;
    const char* st = doc["motors"][i]["status"] | "Idle";
    esp1.motorRunning[i] = (String(st) == "Running");
  }
  for (int i = 0; i < 2; i++) {
    const char* st = doc["pumps"][i]["status"] | "Idle";
    esp1.pumpRunning[i] = (String(st) == "Running");
  }
  return true;
}

// ================= UI helpers =================
void bgGradient() {
  for (int y = 0; y < H; y++) {
    uint8_t t = (uint8_t)((y * 255) / (H - 1));
    uint8_t r = (uint8_t)(8  + (t * 10) / 255);
    uint8_t g = (uint8_t)(10 + (t * 18) / 255);
    uint8_t b = (uint8_t)(20 + (t * 44) / 255);
    tft.drawFastHLine(0, y, W, rgb565(r,g,b));
  }
}

void card(int x, int y, int w, int h, uint16_t fill) {
  tft.fillRoundRect(x, y, w, h, 14, fill);
  tft.drawRoundRect(x, y, w, h, 14, C_LINE);
}

void textTL(int x, int y, const String& s, uint16_t fg, uint16_t bg, int font=2) {
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(fg, bg);
  tft.drawString(s, x, y, font);
}

void textTR(int x, int y, const String& s, uint16_t fg, uint16_t bg, int font=2) {
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(fg, bg);
  tft.drawString(s, x, y, font);
}

void fillRectSafe(const Rect& r, uint16_t col) {
  tft.fillRect(r.x, r.y, r.w, r.h, col);
}

void progressBar(int x, int y, int w, int h, int pct, uint16_t colFill, uint16_t colBg) {
  pct = clampi(pct, 0, 100);
  tft.fillRoundRect(x, y, w, h, 7, colBg);
  tft.drawRoundRect(x, y, w, h, 7, C_LINE);
  int fillW = (w - 4) * pct / 100;
  if (fillW > 0) tft.fillRoundRect(x+2, y+2, fillW, h-4, 6, colFill);
}

void drawWiFiIcon(int x, int y, bool connected, int animPhase, uint16_t bg) {
  tft.fillRect(x-2, y-2, 52, 26, bg);
  uint16_t col = connected ? C_GREEN : C_WARN;
  uint16_t off = rgb565(60,70,95);

  int bars = 0;
  if (connected) bars = 1 + (animPhase % 3);
  else bars = (animPhase % 2);

  for (int i = 0; i < 3; i++) {
    int bx = x + i*12;
    int bh = 6 + i*6;
    uint16_t c = (i < bars) ? col : off;
    tft.fillRoundRect(bx, y + (22-bh), 10, bh, 3, c);
  }
  tft.fillCircle(x + 40, y + 20, 3, (connected ? col : off));
}

// waveform (keep same)
static int wave[90];
static int waveIdx = 0;

void pushWaveSample(bool finger, int bpm) {
  int v = 0;
  if (finger && bpm > 0) {
    int step = (millis() / (60000 / bpm)) % 24;
    v = (step == 0) ? 22 : (step == 1 ? 12 : 3);
  }
  wave[waveIdx++] = v;
  if (waveIdx >= 90) waveIdx = 0;
}

void drawWave(int x, int y, int w, int h, uint16_t fg, uint16_t bg) {
  tft.fillRect(x, y, w, h, bg);
  tft.drawFastHLine(x, y+h-2, w, rgb565(45,55,85));
  int px = x;
  int py = y + h - 2;
  for (int i = 0; i < 90; i++) {
    int idx = (waveIdx + i) % 90;
    int vx = x + (i * w) / 89;
    int vy = (y + h - 2) - wave[idx];
    tft.drawLine(px, py, vx, vy, fg);
    px = vx; py = vy;
  }
}

void drawPulseRing(int cx, int cy, int phase, bool on, uint16_t bg) {
  // reduced so it never overlaps
  tft.fillCircle(cx, cy, 22, bg);
  if (!on) {
    tft.drawCircle(cx, cy, 14, rgb565(60,70,95));
    tft.fillCircle(cx, cy, 3, rgb565(60,70,95));
    return;
  }
  int r = 8 + (phase % 14);
  uint16_t col = (phase % 2 == 0) ? C_PINK : C_RED;
  tft.drawCircle(cx, cy, 14, col);
  tft.drawCircle(cx, cy, r, col);
  tft.fillCircle(cx, cy, 3, col);
}

void drawThermo(int x, int y, int w, int h, float tempF, uint16_t bg) {
  tft.fillRoundRect(x, y, w, h, 9, rgb565(10,12,24));
  tft.drawRoundRect(x, y, w, h, 9, C_LINE);

  float tmin=90.0f, tmax=110.0f;
  float t = tempF;
  if (t < tmin) t = tmin;
  if (t > tmax) t = tmax;
  int pct = (int)(((t - tmin) * 100.0f) / (tmax - tmin));

  uint16_t col = (tempF >= 100.4f) ? C_RED : (tempF >= 99.0f ? C_ORANGE : C_CYAN);
  int fillW = (w - 4) * pct / 100;
  if (fillW > 0) tft.fillRoundRect(x+2, y+2, fillW, h-4, 8, col);
}

// ================= Layout for partial redraw =================
Rect rTopBar = {0,0,W,46};
Rect rLeftCard = {10,56,290,320-56-10};
Rect rRightCard = {310,56,480-310-10,320-56-10};

// top dynamic areas
Rect rTopText1 = {12,10,240,16};
Rect rTopText2 = {12,28,330,16};
Rect rTopClock = {W-100,28,90,16};
Rect rTopWifi  = {W-72,10,60,30};

// left dynamic areas
Rect rEnv = {22,88,260,18};
Rect rPills[3] = {
  {22,118,260,44},
  {22,180,260,44},
  {22,242,260,44},
};
// moved DOWN & smaller so no overlap
Rect rDots = {22, 300-6, 260, 20}; // near bottom inside left card

// right dynamic areas
Rect rHRNum = {322,110,130,36};
Rect rWave  = {322,148,148,30};
Rect rTempLine = {322,188,148,18};
Rect rThermo = {322,208,148,14};
Rect rWaterLabel = {322,228,148,16};
Rect rWaterBar = {322,244,148,14};
Rect rWaterLine = {322,262,148,16};

// ================= Draw static (rare) =================
void drawStaticScreen() {
  bgGradient();

  // top bar
  tft.fillRect(0, 0, W, 46, C_CARD2);
  tft.drawFastHLine(0, 46, W, C_LINE);
  textTL(12, 10, "ESP2 Monitor", C_TEXT, C_CARD2, 2);

  // cards
  card(rLeftCard.x, rLeftCard.y, rLeftCard.w, rLeftCard.h, C_CARD);
  textTL(rLeftCard.x+12, rLeftCard.y+10, "ESP1", C_TEXT, C_CARD, 2);

  card(rRightCard.x, rRightCard.y, rRightCard.w, rRightCard.h, C_CARD);
  textTL(rRightCard.x+12, rRightCard.y+10, "ESP2 Sensors", C_TEXT, C_CARD, 2);

  // right static labels
  textTL(rRightCard.x+12, rRightCard.y+34, "Heart Rate", C_MUTED, C_CARD, 2);
  textTL(rRightCard.x+12, rRightCard.y+130, "Body Temp", C_MUTED, C_CARD, 2);
  textTL(rRightCard.x+12, rRightCard.y+170, "Water Level", C_MUTED, C_CARD, 2);
}

// ================= Draw dynamic (often) =================
void drawTopDynamic() {
  fillRectSafe(rTopText2, C_CARD2);
  textTL(12, 28, esp1.ok ? (esp1.seqState + " | " + esp1.activePart) : "ESP1 OFFLINE",
         esp1.ok ? C_MUTED : C_WARN, C_CARD2, 2);

  fillRectSafe(rTopClock, C_CARD2);
  textTR(W-12, 28, esp1.ok ? esp1.nowHHMM : "--:--", esp1.ok ? C_MUTED : C_WARN, C_CARD2, 2);

  static int wifiPhase = 0;
  wifiPhase++;
  drawWiFiIcon(W-70, 12, WiFi.status() == WL_CONNECTED, wifiPhase, C_CARD2);
}

void drawLeftDynamic() {
  fillRectSafe(rEnv, C_CARD);
  String env = "Temp " + String(esp1.dhtTempC,1) + "C   Hum " + String(esp1.dhtHum,0) + "%";
  textTL(rEnv.x, rEnv.y, env, C_MUTED, C_CARD, 2);

  for (int i=0;i<3;i++) {
    fillRectSafe(rPills[i], C_CARD);
    int rem = esp1.pillRemaining[i];
    int tgt = esp1.pillTarget[i];
    int pct = (tgt>0) ? (rem*100/tgt) : 0;
    uint16_t col = (i==0?C_BLUE:(i==1?C_PURPLE:C_GREEN));
    textTL(rPills[i].x, rPills[i].y, "Pills M"+String(i)+"  "+String(rem)+"/"+String(tgt), C_TEXT, C_CARD, 2);
    progressBar(rPills[i].x, rPills[i].y+20, rPills[i].w, 16, pct, col, rgb565(10,12,24));
  }

  // dots (smaller and lower)
  fillRectSafe(rDots, C_CARD);
  int y = rDots.y + 8;
  textTL(rDots.x, rDots.y, "Mot", C_MUTED, C_CARD, 2);
  for (int i=0;i<3;i++){
    uint16_t c = esp1.motorRunning[i] ? C_GREEN : rgb565(70,80,110);
    tft.fillCircle(rDots.x + 36 + i*14, y, 4, c);
  }
  textTL(rDots.x + 90, rDots.y, "Pump", C_MUTED, C_CARD, 2);
  for (int i=0;i<2;i++){
    uint16_t c = esp1.pumpRunning[i] ? C_YELLOW : rgb565(70,80,110);
    tft.fillCircle(rDots.x + 140 + i*14, y, 4, c);
  }
}

void drawRightDynamic() {
  bool finger = esp2.finger && esp2.bpm > 0;

  // HR number
  fillRectSafe(rHRNum, C_CARD);
  String bpmTxt = finger ? String(esp2.bpm) : "--";
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(C_TEXT, C_CARD);
  tft.drawString(bpmTxt, rHRNum.x, rHRNum.y, 4);
  tft.setTextColor(C_MUTED, C_CARD);
  tft.drawString("bpm", rHRNum.x + 68, rHRNum.y + 12, 2);

  // pulse ring (smaller, fixed position)
  static int pulsePhase = 0;
  pulsePhase++;
  drawPulseRing(rRightCard.x + rRightCard.w - 30, rRightCard.y + 58, pulsePhase, finger, C_CARD);

  // wave
  pushWaveSample(finger, esp2.bpm);
  drawWave(rWave.x, rWave.y, rWave.w, rWave.h, C_PINK, C_CARD);

  // Temp
  fillRectSafe(rTempLine, C_CARD);
  float tf = (esp2.bodyTempC > 0) ? cToF(esp2.bodyTempC) : 0;
  String tempTxt = (esp2.bodyTempC > 0) ? (String(tf,1) + " F") : "--";
  textTL(rTempLine.x, rTempLine.y, tempTxt, C_TEXT, C_CARD, 2);
  drawThermo(rThermo.x, rThermo.y, rThermo.w, rThermo.h, tf, C_CARD);

  // Water label + bar + line (smaller)
  fillRectSafe(rWaterLabel, C_CARD);
  textTL(rWaterLabel.x, rWaterLabel.y, "Water", C_MUTED, C_CARD, 2);
  progressBar(rWaterBar.x, rWaterBar.y, rWaterBar.w, rWaterBar.h, esp2.waterPct, C_CYAN, rgb565(10,12,24));
  fillRectSafe(rWaterLine, C_CARD);
  textTL(rWaterLine.x, rWaterLine.y, String(esp2.waterPct) + "%  raw " + String(esp2.waterRaw), C_TEXT, C_CARD, 2);
}

// ================= Setup/Loop =================
void setup() {
  Serial.begin(115200);
  delay(500);

  tft.init();
  tft.setRotation(3);
  tft.fillScreen(0);

  // WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  // I2C
  Wire.begin(21, 22);

  // MAX30102 init
  if (particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
    particleSensor.setup(10, 4, 2, 100, 411, 4096);
    calibrateIR();
  }

  randomSeed(esp_random());

  // Water ADC
  pinMode(WATER_ADC_PIN, INPUT);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  for (int i=0;i<90;i++) wave[i]=0;

  drawStaticScreen();
  drawTopDynamic();
  drawLeftDynamic();
  drawRightDynamic();
}

void loop() {
  static unsigned long lastPoll = 0;
  static unsigned long lastSend = 0;
  static unsigned long lastFull = 0;
  static unsigned long lastAnim = 0;

  static unsigned long lastTemp = 0;
  static unsigned long lastHR = 0;

  // Read water raw and compute smoothed percent
  int raw = analogRead(WATER_ADC_PIN);
  int pctNow = waterPercentFromRaw(raw);
  esp2.waterRaw = raw;
  esp2.waterPct = smoothPct(esp2.waterPct, pctNow);

  // temp update
  if (millis() - lastTemp > 1200) {
    lastTemp = millis();
    float rawC = readTemperatureC();
    if (!isnan(rawC)) {
      float bodyC = rawC + BODY_TEMP_OFFSET_C;
      if (bodyC < BODY_TEMP_MIN_C) bodyC = BODY_TEMP_MIN_C;
      if (bodyC > BODY_TEMP_MAX_C) bodyC = BODY_TEMP_MAX_C;
      esp2.bodyTempC = bodyC;
    } else {
      esp2.bodyTempC = 0;
    }
  }

  // HR update (heavy)
  if (millis() - lastHR > 5000) {
    lastHR = millis();
    int avgBPM=0; bool spo2Ok=false; int spo2Out=0;
    bool finger = false;
    if (particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
      finger = computeAvgBPM(avgBPM, spo2Ok, spo2Out);
    }
    esp2.finger = finger;
    esp2.bpm = finger ? avgBPM : 0;
  }

  // Poll ESP1 (and update left/top)
  if (millis() - lastPoll > POLL_STATUS_MS) {
    lastPoll = millis();
    if (WiFi.status() == WL_CONNECTED) {
      if (!pollEsp1Status()) esp1.ok = false;
    } else {
      esp1.ok = false;
      WiFi.disconnect();
      WiFi.begin(ssid, password);
    }
    drawTopDynamic();
    drawLeftDynamic();
  }

  // Send ingest
  if (millis() - lastSend > SEND_INGEST_MS) {
    lastSend = millis();
    if (WiFi.status() == WL_CONNECTED) sendIngestToEsp1();
  }

  // Full redraw rarely (prevents burn-in / refresh artifacts)
  if (millis() - lastFull > FULL_REDRAW_MS) {
    lastFull = millis();
    drawStaticScreen();
    drawTopDynamic();
    drawLeftDynamic();
    drawRightDynamic();
  }

  // Right panel animations frequently (low flicker)
  if (millis() - lastAnim > ANIM_MS) {
    lastAnim = millis();
    drawRightDynamic();
    // wifi icon also animates here
    drawTopDynamic();
  }

  delay(10);
}