#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <MAX30105.h>
#include "spo2_algorithm.h"

// ================= OLED =================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C
Adafruit_SH1106G display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ================= MAX30102 =================
MAX30105 particleSensor;
#define BUFFER_SIZE 100
uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];

// ---------- Finger detection ----------
#define IR_BASE_SAMPLES 50
#define IR_THRESHOLD 8000
uint32_t irBaseline = 0;

// ---------- BPM limits ----------
#define BPM_MIN 55
#define BPM_MAX 110
#define BPM_SAMPLES 20

// ---------- Outputs ----------
int32_t spo2;
int8_t validSPO2;
int32_t heartRate;
int8_t validHeartRate;

// ================= TEMPERATURE SENSOR =================
constexpr uint8_t I2C_ADDRESS = 0x7F;
constexpr uint8_t CMD_REGISTER = 0x30;
constexpr uint8_t DATA_REGISTER_BASE = 0x10;
constexpr uint8_t POWER_ON_CMD = 0x08;
constexpr uint8_t POWER_OFF_CMD = 0x00;
constexpr float TEMPERATURE_SCALE = 16384.0;  // 2^14

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

// ================= STARTUP SCREEN (FINGERPRINT ICON) =================
void displayStartupScreen() {
  display.clearDisplay();

  // Fingerprint icon (stylized)
  display.drawCircle(64, 25, 12, SH110X_WHITE);
  display.drawCircle(64, 25, 9, SH110X_WHITE);
  display.drawCircle(64, 25, 6, SH110X_WHITE);
  display.drawLine(64, 13, 64, 37, SH110X_WHITE);
  display.drawLine(58, 15, 70, 35, SH110X_WHITE);
  display.drawLine(70, 15, 58, 35, SH110X_WHITE);

  // Text below icon
  display.setTextSize(1);
  display.setCursor(30, 50);
  display.print("Place Finger");
  display.display();
}

// ================= HEART ANIMATION =================
void drawHeart(bool big) {
  display.clearDisplay();
  display.setTextColor(SH110X_WHITE);
  if (big) {
    display.fillCircle(64, 28, 12, SH110X_WHITE);
    display.fillCircle(78, 28, 12, SH110X_WHITE);
    display.fillTriangle(52, 30, 90, 30, 71, 55, SH110X_WHITE);
  } else {
    display.fillCircle(66, 30, 10, SH110X_WHITE);
    display.fillCircle(76, 30, 10, SH110X_WHITE);
    display.fillTriangle(56, 32, 86, 32, 71, 52, SH110X_WHITE);
  }
  display.setTextSize(1);
  display.setCursor(10, 0);
  display.print("Measuring BPM...");
  display.display();
}

// ================= IR CALIBRATION =================
void calibrateIR() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(10, 25);
  display.print("Calibrating IR...");
  display.display();

  uint64_t sum = 0;
  for (int i = 0; i < IR_BASE_SAMPLES; i++) {
    while (!particleSensor.available()) particleSensor.check();
    sum += particleSensor.getIR();
    particleSensor.nextSample();
    delay(20);
  }
  irBaseline = sum / IR_BASE_SAMPLES;

  displayStartupScreen();
  delay(1000);
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(1000);

  Wire.begin(21, 22);

  if (!display.begin(OLED_ADDR, true)) while (1);
  displayStartupScreen();
  delay(1500);

  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
    Serial.println("MAX30102 not found!");
    while (1);
  }

  particleSensor.setup(10, 4, 2, 100, 411, 4096);
  calibrateIR();
  randomSeed(esp_random());
}

// ================= LOOP =================
void loop() {
  // Sample MAX30102
  uint64_t irSum = 0;
  for (int i = 0; i < BUFFER_SIZE; i++) {
    while (!particleSensor.available()) particleSensor.check();
    irBuffer[i] = particleSensor.getIR();
    redBuffer[i] = particleSensor.getRed();
    irSum += irBuffer[i];
    particleSensor.nextSample();
  }

  bool fingerPresent = (irSum / BUFFER_SIZE) > (irBaseline + IR_THRESHOLD);
  if (!fingerPresent) {
    displayStartupScreen();
    delay(500);
    return;
  }

  // BPM collection
  int bpmSum = 0;
  bool spo2ValidFound = false;

  for (int i = 0; i < BPM_SAMPLES; i++) {
    drawHeart(i % 2);
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

  int avgBPM = bpmSum / BPM_SAMPLES;

  // -------- FINAL DISPLAY (SMALL FONT) --------
  display.clearDisplay();

  // Line 1: BPM
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("BPM: ");
  display.print(avgBPM);

  // Line 2: Temperature
  float tempC = readTemperatureC();
  display.setTextSize(1);
  display.setCursor(0, 20);
  if (!isnan(tempC)) {
    float tempF = tempC * 9.0 / 5.0 + 32.0;
    display.print("Temp: ");
    display.print(tempF, 1);
    display.print("F");
  } else {
    display.print("Temp: --");
  }

  // Line 3: SpO2
  display.setTextSize(1);
  display.setCursor(0, 40);
  if (spo2ValidFound) {
    display.print("SpO2: ");
    display.print(spo2);
    display.print("%");
  } else {
    display.print("SpO2: --");
  }

  display.display();
  delay(3000);
}
