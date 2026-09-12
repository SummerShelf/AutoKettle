#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <RotaryEncoder.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>

// ================= Network Configuration =================
IPAddress local_IP(192, 168, 0, 180); 
IPAddress gateway(192, 168, 0, 1);    
IPAddress subnet(255, 255, 255, 0);  
IPAddress primaryDNS(8, 8, 8, 8);   

WebServer server(80);

// ================= Definitions =================
#define ONE_WIRE_BUS 4   
#define PIN_IN1      25  
#define PIN_IN2      26  
#define PIN_SW       27  
#define BUZZER_PIN   19  
#define SERVO_PIN    33  
#define HEATING_LED_PIN 32
#define OFFSET 10
#define DeathZone 2

const int SERVO_IDLE_ANGLE = 90;    
const int SERVO_ON_ANGLE   = 140;   
const int SERVO_OFF_ANGLE  = 40;    
const int PULSE_HOLD_MS    = 600;   

// ================= Peripherals =================
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
U8G2_SSD1306_128X32_UNIVISION_F_HW_I2C u8g2(U8G2_R2, U8X8_PIN_NONE);
RotaryEncoder encoder(PIN_IN1, PIN_IN2, RotaryEncoder::LatchMode::FOUR0);
Servo kettleServo;

// ================= Variables =================
volatile int targetTemp = 80;
int realTargetTemp = 0;
int fail=0;
float currentTemp = 0.0;
float startingTemp = 0.0;
bool isHeating = false;
bool targetReachedBuzzed = false;
bool longpress = false;
bool mode = false; // false = Single Target, true = Hold Temp Mode
bool work = false;
bool hold = false;
int repetition = 1;

// Timing & Safety Variables
unsigned long lastBtnPress = 0;
unsigned long lastTempRequest = 0;
unsigned long pressTime = 0;
unsigned long heatingStartTime = 0;
unsigned long lastSafetyCheckTime = 0;
unsigned long heatingWaiting = 0; 
float lastSafetyTemp = 0.0;

// ================= Helpers =================
void beep(int durationMs) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(durationMs);
  digitalWrite(BUZZER_PIN, LOW);
}

void playTargetReachedAlert() {
  for (int i = 0; i < 3; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(150);
    digitalWrite(BUZZER_PIN, LOW);
    delay(100);
  }
}

void pulseKettleSwitch(bool turnOn) {
  if (!kettleServo.attached()) {
    kettleServo.attach(SERVO_PIN, 500, 2400);
  }
  kettleServo.write(turnOn ? SERVO_ON_ANGLE : SERVO_OFF_ANGLE);  
  delay(PULSE_HOLD_MS); 
  kettleServo.write(SERVO_IDLE_ANGLE); 
  delay(300); // Ensures mechanical return before code execution continues
}

void panic(int code) {
  if (kettleServo.attached()) {
    kettleServo.write(SERVO_OFF_ANGLE);
    delay(600);
    kettleServo.detach();
  }
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(0, 12, "PANIC - SYSTEM HALT");
  char codeBuf[16];
  snprintf(codeBuf, sizeof(codeBuf), "Code: %d", code);
  u8g2.drawStr(0, 30, codeBuf);
  u8g2.sendBuffer();
  
  for (int i = 0; i < 30; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(70);
    digitalWrite(BUZZER_PIN, LOW);
    delay(50);
  }
  digitalWrite(BUZZER_PIN, LOW);
  esp_deep_sleep_start();
}

void startHeating() {
  if (!isHeating) {
    isHeating = true;
    pulseKettleSwitch(true);
    targetReachedBuzzed = false; 
    heatingStartTime = millis();
    lastSafetyTemp = currentTemp;
    lastSafetyCheckTime = millis();
  }
}

void stopHeating() {
  if (isHeating) {
    isHeating = false;
    pulseKettleSwitch(false);
  }
}

void IRAM_ATTR checkPosition() {
  encoder.tick();
  static int lastPos = 0;
  int newPos = encoder.getPosition();
  
  if (newPos != lastPos) {
    int delta = newPos - lastPos;
    targetTemp -= delta;
    if (targetTemp < 20) targetTemp = 20;
    if (targetTemp > 95) targetTemp = 95;
    lastPos = newPos;
  }
}

// ================= Web Interface =================

const char HTML_INDEX[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Smart Kettle Control</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <style>
    body { font-family: Arial, sans-serif; background: #121212; color: #fff; text-align: center; margin: 0; padding: 20px; }
    .card { background: #1e1e1e; max-width: 480px; margin: auto; padding: 20px; border-radius: 12px; box-shadow: 0 4px 10px rgba(0,0,0,0.5); }
    h1 { color: #00adb5; font-size: 24px; margin-top: 0; }
    .temp-display { font-size: 48px; font-weight: bold; margin: 10px 0; color: #ff5722; }
    .status { font-size: 16px; margin-bottom: 15px; text-transform: uppercase; letter-spacing: 1px; }
    .status-on { color: #e53935; font-weight: bold; }
    .status-off { color: #757575; }
    .status-hold { color: #ffb300; font-weight: bold; }
    .chart-container { position: relative; height: 220px; width: 100%; margin-bottom: 20px; }
    .control-group { margin: 15px 0; text-align: left; }
    label { font-size: 14px; color: #aaa; display: block; margin-bottom: 5px; }
    input[type=number], select { width: 100%; padding: 10px; font-size: 16px; border-radius: 6px; border: none; background: #2c2c2c; color: #fff; box-sizing: border-box; }
    .btn { display: inline-block; width: 100%; padding: 12px; margin-top: 10px; font-size: 18px; font-weight: bold; color: #fff; border: none; border-radius: 6px; cursor: pointer; }
    .btn-on { background: #e53935; }
    .btn-off { background: #388e3c; }
  </style>
</head>
<body>
  <div class="card">
    <h1>Kettle Controller</h1>
    <div class="temp-display" id="temp">-- &deg;C</div>
    <div class="status" id="status">Status: Off</div>
    <div class="chart-container"><canvas id="tempChart"></canvas></div>

    <div class="control-group">
      <label>Target Temperature (&deg;C):</label>
      <input type="number" id="targetTemp" min="20" max="95" onchange="updateSettings()">
    </div>

    <div class="control-group">
      <label>Mode:</label>
      <select id="modeSelect" onchange="updateSettings()">
        <option value="0">Single Target Cutoff</option>
        <option value="1">Hold Temperature</option>
      </select>
    </div>

    <button id="toggleBtn" class="btn btn-off" onclick="togglePower()">Turn ON</button>
  </div>

  <script>
    const MAX_DATA_POINTS = 40; 
    const ctx = document.getElementById('tempChart').getContext('2d');
    const tempChart = new Chart(ctx, {
      type: 'line',
      data: {
        labels: [],
        datasets: [
          { label: 'Probe Temp (\u00B0C)', data: [], borderColor: '#ff5722', backgroundColor: 'rgba(255, 87, 34, 0.1)', borderWidth: 2, fill: true, tension: 0, pointRadius: 2, yAxisID: 'y' },
          { label: 'Target Temp (\u00B0C)', data: [], borderColor: '#00adb5', borderDash: [5, 5], borderWidth: 2, pointRadius: 0, fill: false, yAxisID: 'y' },
          { label: 'Heating Active', data: [], borderColor: 'rgba(229, 57, 53, 0.8)', backgroundColor: 'rgba(229, 57, 53, 0.15)', borderWidth: 1, fill: true, stepped: true, pointRadius: 0, yAxisID: 'y1' }
        ]
      },
      options: {
        responsive: true, maintainAspectRatio: false, animation: false,
        scales: { 
          x: { display: false }, 
          y: { type: 'linear', display: true, position: 'left', grid: { color: '#333' }, ticks: { color: '#aaa' }, suggestedMin: 20, suggestedMax: 100 },
          y1: { type: 'linear', display: false, position: 'right', min: 0, max: 1 } // Hidden secondary axis just for the on/off shading
        },
        plugins: { legend: { labels: { color: '#ccc', font: { size: 12 } } } }
      }
    });

    async function fetchData() {
      try {
        const response = await fetch('/data');
        const data = await response.json();
        
        document.getElementById('temp').innerHTML = data.currentTemp.toFixed(1) + " &deg;C";
        
        const statusEl = document.getElementById('status');
        const btn = document.getElementById('toggleBtn');

        if (!data.work) {
          statusEl.innerText = "Status: IDLE";
          statusEl.className = "status status-off";
          btn.innerText = "Turn ON";
          btn.className = "btn btn-off";
        } else {
          btn.innerText = "Turn OFF";
          btn.className = "btn btn-on";
          if (data.isHeating) {
            statusEl.innerText = "Status: HEATING";
            statusEl.className = "status status-on";
          } else if (data.hold) {
            statusEl.innerText = "Status: PULSING WAIT";
            statusEl.className = "status status-hold";
          } else {
            statusEl.innerText = "Status: STANDBY (HOLDING)";
            statusEl.className = "status status-hold";
          }
        }

        if (document.activeElement.id !== "targetTemp") document.getElementById('targetTemp').value = data.targetTemp;
        if (document.activeElement.id !== "modeSelect") document.getElementById('modeSelect').value = data.mode ? "1" : "0";

        const now = new Date().toLocaleTimeString();
        tempChart.data.labels.push(now);
        tempChart.data.datasets[0].data.push(data.currentTemp);
        tempChart.data.datasets[1].data.push(data.targetTemp);
        tempChart.data.datasets[2].data.push(data.isHeating ? 1 : 0); // 1 for ON, 0 for OFF

        if (tempChart.data.labels.length > MAX_DATA_POINTS) {
          tempChart.data.labels.shift();
          tempChart.data.datasets[0].data.shift();
          tempChart.data.datasets[1].data.shift();
          tempChart.data.datasets[2].data.shift();
        }
        tempChart.update();
      } catch (e) { console.error("Fetch error", e); }
    }

    async function updateSettings() {
      const target = document.getElementById('targetTemp').value;
      const mode = document.getElementById('modeSelect').value;
      await fetch(`/control?target=${target}&mode=${mode}`);
    }

    async function togglePower() {
      await fetch('/control?action=toggle');
      fetchData();
    }

    setInterval(fetchData, 3000);
    fetchData();
  </script>
</body>
</html>
)rawliteral";

void handleRoot() { server.send(200, "text/html", HTML_INDEX); }

void handleData() {
  String json = "{";
  json += "\"currentTemp\":" + String(currentTemp, 1) + ",";
  json += "\"targetTemp\":" + String(targetTemp) + ",";
  json += "\"isHeating\":" + String(isHeating ? "true" : "false") + ",";
  json += "\"mode\":" + String(mode ? "true" : "false") + ",";
  json += "\"work\":" + String(work ? "true" : "false") + ",";
  json += "\"hold\":" + String(hold ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleControl() {
  if (server.hasArg("target")) {
    int val = server.arg("target").toInt();
    if (val >= 20 && val <= 95) targetTemp = val;
  }
  if (server.hasArg("mode")) {
    mode = (server.arg("mode").toInt() == 1);
  }
  if (server.hasArg("action")) {
    String action = server.arg("action");
    if (action == "toggle") {
      work = !work;
    } else if (action == "on") {
      work = true;
    } else if (action == "off") {
      work = false;
    }
  }
  server.send(200, "text/plain", "OK");
}

// ================= Setup =================
void setup() {
  Serial.begin(115200);

  pinMode(PIN_IN1, INPUT_PULLUP);
  pinMode(PIN_IN2, INPUT_PULLUP);
  pinMode(PIN_SW, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(HEATING_LED_PIN, OUTPUT);
  digitalWrite(HEATING_LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  ESP32PWM::allocateTimer(0);
  kettleServo.setPeriodHertz(50);
  kettleServo.attach(SERVO_PIN, 500, 2400);
  kettleServo.write(SERVO_IDLE_ANGLE);

  attachInterrupt(digitalPinToInterrupt(PIN_IN1), checkPosition, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_IN2), checkPosition, CHANGE);

  u8g2.begin();
  sensors.begin();
  sensors.setWaitForConversion(false);
  sensors.requestTemperatures();

  // Initialize WiFiManager
  WiFiManager wm;
  
  // Set the custom static IP
  wm.setSTAStaticIPConfig(local_IP, gateway, subnet, primaryDNS);

  // Automatically connect to the saved network or open the AP
  if(!wm.autoConnect("Kettle_SetupAP")) {
    Serial.println("Failed to connect or hit timeout");
    delay(3000);
    ESP.restart(); // Reboot and try again
  }
  
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/control", handleControl);
  server.begin();

  beep(100);
}

// ================= Main Loop =================
void loop() {
  server.handleClient();

  // 1. Button Press Logic
  if (digitalRead(PIN_SW) == LOW) {
    if (millis() - lastBtnPress > 50) {
      if (pressTime == 0) pressTime = millis();
      
      if (millis() - pressTime >= 1000 && !longpress) {
        beep(100);
        mode = !mode;
        work = false;
        longpress = true;
      }
    }
  } else if (pressTime > 0) {
    if (!longpress && (millis() - pressTime > 50)) {
      beep(100);
      work = !work;
    }
    pressTime = 0;
    longpress = false;
    lastBtnPress = millis();
  }

  digitalWrite(HEATING_LED_PIN, isHeating ? HIGH : LOW);

  // 2. Read Temperature 
  if (millis() - lastTempRequest >= 1000) {
    float temp = sensors.getTempCByIndex(0);
    if (temp != DEVICE_DISCONNECTED_C && temp > -10.0) {
      currentTemp = temp;
      fail=0;
    } else {
      fail++;
      Serial.printf("fail: %d\n", fail);
      if(fail>=25){
      Serial.println("connection fail panic");
      panic(0);}
    }
    sensors.requestTemperatures();
    lastTempRequest = millis();
  }

  // 3. Safety Check for bulk temperature
  if (millis() - lastSafetyCheckTime >= 5000) {
    float tempDiff = currentTemp - lastSafetyTemp;
    if (isHeating && (millis() - heatingStartTime > 30000)) { 
      if (tempDiff < 0.2 && currentTemp < (targetTemp - 2.0)) {
        Serial.println("temp down");
        panic(1);}
    }
    lastSafetyTemp = currentTemp;
    lastSafetyCheckTime = millis();
  }

  // 4. Core Heating Logic

  
  // Master OFF switch enforcement
  if (!work) {
    if (isHeating) stopHeating();
    hold = false;
  }

  // Trigger Single Target Cycle
  if (!hold && work && !isHeating && !mode && currentTemp < targetTemp) {
    startingTemp = currentTemp;
    realTargetTemp = targetTemp;
    hold = true;
    repetition = 1;
  }

  // Trigger Hold Target Cycle
  if (!hold && work && mode && currentTemp < targetTemp - DeathZone) {
    hold = true;
    startingTemp = currentTemp;
    realTargetTemp = targetTemp + DeathZone;
    repetition = 1;
  }

  // Bulk Heating Phase
  if (hold && work && realTargetTemp - startingTemp > OFFSET + 3 && realTargetTemp > startingTemp&&millis() >= heatingWaiting) {
    if (!isHeating) startHeating();
    
    if (currentTemp + OFFSET >= realTargetTemp) {
      stopHeating();
      hold=false;
      heatingWaiting = millis() + 15000;//so it wont trigger pulsing phase before the temperature stabilitates
    }
  }

  // Precise Pulsing Phase
  if (hold && work && realTargetTemp - startingTemp <= OFFSET + 3 && realTargetTemp > startingTemp) {
      if (repetition >= 30) {
        Serial.println("repetition");
        panic(2);}
      if (millis() >= heatingWaiting) {
        if (repetition % 2 == 0) {
          if (!isHeating) startHeating(); 
          heatingWaiting = millis() + constrain (round((realTargetTemp-currentTemp)*333.33+2000),1500,15000);
          repetition++;
        } else {
          if (isHeating) stopHeating();   
          heatingWaiting = millis() + 25000;
          repetition++;
        }
      }
  }
  if (currentTemp >= realTargetTemp) {
    repetition = 1;
    if (isHeating) stopHeating();
    if (!mode) {
      work = false;
      hold = false;
      playTargetReachedAlert();
    } else {
      hold = false;
    }
  }

  // 5. Render UI to OLED
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_7x13_tf);
  
  char currentBuf[20];
  snprintf(currentBuf, sizeof(currentBuf), "%.1fC", currentTemp);
  u8g2.drawStr(0, 12, currentBuf);

  char targetBuf[10];
  snprintf(targetBuf, sizeof(targetBuf), "%dC", targetTemp);
  u8g2.drawStr(100, 12, targetBuf);
  
  u8g2.drawFrame(0, 13, 128, 19);

  int barWidth = constrain(round(currentTemp * 1.24), 0, 124);
  if (barWidth > 0) {
    u8g2.drawBox(2, 15, barWidth, 15); 
  }
  
  if (mode && work) {
    u8g2.setFont(u8g2_font_6x10_tf);
    uint16_t textWidth = u8g2.getStrWidth("Holding");    
    uint16_t x = (128 - textWidth) / 2;
    u8g2.drawStr(x, 10, "Holding");
  }
  if (!mode && work) {
    u8g2.setFont(u8g2_font_6x10_tf);
    uint16_t textWidth = u8g2.getStrWidth("Heating");    
    uint16_t x = (128 - textWidth) / 2;
    u8g2.drawStr(x, 10, "Heating");
  }

  auto drawDashedLine = [](int width) {
    if (width > 0) {
      for (int i = 0; i < 15; i++) {
        u8g2.setDrawColor(i % 2 == 0 ? 1 : 0);
        u8g2.drawPixel(width, i + 15);
      }
      u8g2.setDrawColor(1);
    }
  };

  if (!mode) {
    drawDashedLine(constrain(round(targetTemp * 1.24), 0, 124));
  } else {
    drawDashedLine(constrain(round((targetTemp + 1.5) * 1.24), 0, 124));
    drawDashedLine(constrain(round((targetTemp - 1.5) * 1.24), 0, 124));
  }
  
  u8g2.sendBuffer();
}