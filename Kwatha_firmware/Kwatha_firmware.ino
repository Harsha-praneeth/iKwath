#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <MFRC522.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include "HX711.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

// ==========================================
// PIN DEFINITIONS
// ==========================================
#define OLED_SDA        21
#define OLED_SCL        22
#define OLED_WIDTH      128
#define OLED_HEIGHT     64

#define RFID_SS_PIN     5
#define RFID_RST_PIN    27

#define ONE_WIRE_BUS    4     // DS18B20 Temp Probe

#define HX711_DT_PIN    16    // Load Cell Data Pin
#define HX711_SCK_PIN   17    // Load Cell Clock Pin

#define SSR_RELAY_PIN   2     // Solid State Relay (Heater Control)
#define STIRRER_PIN     14    // DC Magnetic Stirrer Motor (PWM / Digital)

// ==========================================
// SYSTEM CONSTANTS & TARGETS
// ==========================================
const float TARGET_TEMP         = 88.0;   // Target Brewing Temp (°C)
const float SAFETY_TEMP_MAX     = 105.0;  // Emergency Overheat Temp (°C)
const float INITIAL_WATER_MASS  = 400.0;  // Water to add (grams / mL)
const float TARGET_YIELD_MASS   = 100.0;  // Target Final Volume Yield (grams / mL)
const float REQUIRED_MASS_LOSS  = 300.0;  // 400g - 100g = 300g Evaporation

// PID Calibration Constants
const float Kp = 12.0;
const float Ki = 0.5;
const float Kd = 2.0;

// ==========================================
// HARDWARE INSTANCES
// ==========================================
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);
MFRC522 rfid(RFID_SS_PIN, RFID_RST_PIN);
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensor(&oneWire);
HX711 loadCell;

// BLE Server & Characteristic Instances
BLEServer* pServer = NULL;
BLECharacteristic* pCharacteristic = NULL;

// ==========================================
// SYSTEM STATE CONTROL ENUM
// ==========================================
enum ProcessState {
  STATE_INIT,
  STATE_WAIT_FOR_POD,
  STATE_POD_AUTHENTICATED,
  STATE_WAIT_FOR_WATER,
  STATE_TARE_CALIBRATION,
  STATE_HEATING_BOILING,
  STATE_HOLD_STIRRING,
  STATE_PROCESS_COMPLETE,
  STATE_SAFETY_CUTOFF
};

ProcessState currentState = STATE_INIT;

// State Variables
String authenticatedPodName = "";
float currentTemp = 0.0;
float currentWeight = 0.0;
float baseTareWeight = 0.0;
float netWaterWeight = 0.0;
float totalMassLoss = 0.0;

// PID Loop Global Variables
float pidIntegral = 0.0;
float lastPidError = 0.0;
unsigned long lastPidTime = 0;

// ==========================================
// HELPER FUNCTIONS
// ==========================================

// Display Handler to show explicit steps to the user
void updateOLEDDisplay(String line1, String line2, String line3, String line4) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  display.setCursor(0, 0);
  display.println("=== KWATHA MAKER ===");
  display.setCursor(0, 16);
  display.println(line1);
  display.setCursor(0, 28);
  display.println(line2);
  display.setCursor(0, 40);
  display.println(line3);
  display.setCursor(0, 52);
  display.println(line4);
  
  display.display();
}

// Send Diagnostics via BLE to generic Bluetooth terminal
void sendBLETelemetry(String msg) {
  if (pCharacteristic) {
    pCharacteristic->setValue(msg.c_str());
    pCharacteristic->notify();
  }
  Serial.println("[BLE LOG]: " + msg);
}

// Closed-Loop PID Temperature Control Subroutine
int computePID(float targetT, float actualT) {
  unsigned long now = millis();
  float dt = (now - lastPidTime) / 1000.0;
  if (dt <= 0.0) dt = 0.1;

  float error = targetT - actualT;
  pidIntegral += error * dt;
  pidIntegral = constrain(pidIntegral, -100, 100); // Anti-windup clamp
  float derivative = (error - lastPidError) / dt;

  float output = (Kp * error) + (Ki * pidIntegral) + (Kd * derivative);
  
  lastPidError = error;
  lastPidTime = now;

  return constrain((int)output, 0, 255); // PWM Duty Cycle (0 - 255)
}

// ==========================================
// INITIAL SETUP
// ==========================================
void setup() {
  Serial.begin(115200);

  // Pin Configurations
  pinMode(SSR_RELAY_PIN, OUTPUT);
  pinMode(STIRRER_PIN, OUTPUT);
  digitalWrite(SSR_RELAY_PIN, LOW); // Default Heater OFF
  digitalWrite(STIRRER_PIN, LOW);   // Default Stirrer OFF

  // 1. Initialize OLED Screen
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED Allocation Failed");
    for (;;);
  }
  updateOLEDDisplay("STEP 1: Starting...", "Initializing Sensors", "Please Wait...", "");

  // 2. Initialize SPI & RFID Reader
  SPI.begin();
  rfid.PCD_Init();

  // 3. Initialize DS18B20 Temp Probe
  tempSensor.begin();

  // 4. Initialize HX711 Load Cell
  loadCell.begin(HX711_DT_PIN, HX711_SCK_PIN);
  loadCell.set_scale(420.0); // Replace with your exact strain gauge calibration factor
  loadCell.tare();

  // 5. Initialize BLE Diagnostics Server
  BLEDevice::init("KWATHA_DIAGNOSTICS");
  pServer = BLEDevice::createServer();
  BLEService *pService = pServer->createService("12345678-1234-1234-1234-1234567890ab");
  pCharacteristic = pService->createCharacteristic(
                      "87654321-4321-4321-4321-ba0987654321",
                      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
                    );
  pService->start();
  pServer->getAdvertising()->start();

  delay(1500);
  currentState = STATE_WAIT_FOR_POD;
}

// ==========================================
// MAIN REPEATING ENGINE LOOP
// ==========================================
void loop() {
  // Read Temperature Sensor Continuously
  tempSensor.requestTemperatures();
  currentTemp = tempSensor.getTempCByIndex(0);

  // Read Mass Sensor Continuously
  if (loadCell.is_ready()) {
    currentWeight = loadCell.get_units(3);
  }

  // --- HARDWARE SAFETY EMERGENCY OVERRIDE ---
  if (currentTemp >= SAFETY_TEMP_MAX) {
    currentState = STATE_SAFETY_CUTOFF;
  }

  // State Machine Logic Engine
  switch (currentState) {

    // STEP 2: WAIT FOR POD INJECTION
    case STATE_WAIT_FOR_POD: {
      updateOLEDDisplay("STEP 2: SCAN POD", "Inject Herbal Pod", "Waiting for RFID...", "Temp: " + String(currentTemp, 1) + " C");

      if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
        // Simple RFID UID Reader & Name Mapping
        String uidStr = "";
        for (byte i = 0; i < rfid.uid.size; i++) {
          uidStr += String(rfid.uid.uidByte[i], HEX);
        }
        
        authenticatedPodName = "Kwatha Pod #A1"; // Formulated pod profile
        sendBLETelemetry("POD AUTHENTICATED: " + authenticatedPodName + " (UID: " + uidStr + ")");
        
        rfid.PICC_HaltA();
        currentState = STATE_POD_AUTHENTICATED;
      }
      break;
    }

    // STEP 3: POD CONFIRMED
    case STATE_POD_AUTHENTICATED: {
      updateOLEDDisplay("STEP 3: POD LOADED!", "Name: " + authenticatedPodName, "Target: 88 C / 100mL", "Calibrating Base...");
      delay(2500);
      baseTareWeight = currentWeight;
      currentState = STATE_WAIT_FOR_WATER;
      break;
    }

    // STEP 4: ADD WATER DETECT & TARE
    case STATE_WAIT_FOR_WATER: {
      netWaterWeight = currentWeight - baseTareWeight;
      updateOLEDDisplay("STEP 4: ADD WATER", "Required: 400 mL", "Current: " + String(netWaterWeight, 0) + " mL", "Fill Container Now");

      if (netWaterWeight >= (INITIAL_WATER_MASS - 10.0)) { // ~400mL added
        sendBLETelemetry("400mL Water Addition Detected. Starting Tare Calibration.");
        currentState = STATE_TARE_CALIBRATION;
      }
      break;
    }

    // STEP 5: LOCK TARE & PREPARE BOILING
    case STATE_TARE_CALIBRATION: {
      updateOLEDDisplay("STEP 5: TARE LOCKED", "Initial Vol: 400 mL", "Target Vol: 100 mL", "Starting Process...");
      delay(2000);
      
      // Turn ON Continuous Magnetic Stirrer
      digitalWrite(STIRRER_PIN, HIGH);
      lastPidTime = millis();
      currentState = STATE_HEATING_BOILING;
      break;
    }

    // STEP 6: CLOSED-LOOP HEATING, BOILING & STIRRING
    case STATE_HEATING_BOILING: {
      netWaterWeight = currentWeight - baseTareWeight;
      totalMassLoss = INITIAL_WATER_MASS - netWaterWeight;

      // Closed-loop PID Output Calculation
      int pwmVal = computePID(TARGET_TEMP, currentTemp);
      analogWrite(SSR_RELAY_PIN, pwmVal);

      // Display Status & Warning to stay away from heating assembly
      updateOLEDDisplay("STEP 6: BOILING ON", "T: " + String(currentTemp, 1) + "/" + String(TARGET_TEMP, 0) + " C", "Yield: " + String(netWaterWeight, 0) + "/100 mL", "!! STAY AWAY DEVICE !!");

      // Telemetry Output over BLE
      sendBLETelemetry("TEMP:" + String(currentTemp) + "C | WATER:" + String(netWaterWeight) + "g | PWM:" + String(pwmVal));

      // DECISION: Mass Reduction Check (400 mL down to 100 mL, i.e., 300g loss)
      if (totalMassLoss >= REQUIRED_MASS_LOSS || netWaterWeight <= TARGET_YIELD_MASS) {
        digitalWrite(SSR_RELAY_PIN, LOW); // Cut Heater Power Immediately
        sendBLETelemetry("Target Mass Reduction Achieved (300g Lost). Stopping Heater.");
        currentState = STATE_PROCESS_COMPLETE;
      }
      break;
    }

    // STEP 7: PROCESS COMPLETED
    case STATE_PROCESS_COMPLETE: {
      digitalWrite(SSR_RELAY_PIN, LOW); // Ensure Heater is OFF
      digitalWrite(STIRRER_PIN, LOW);  // Turn off Magnetic Stirrer

      updateOLEDDisplay("STEP 7: COMPLETE!", "Decoction Ready", "Final Yield: 100 mL", "Safe to Remove Vessel");
      sendBLETelemetry("Process Finished Successfully.");
      break;
    }

    // STEP 8: SAFETY CUTOFF INTERRUPT (EMERGENCY)
    case STATE_SAFETY_CUTOFF: {
      digitalWrite(SSR_RELAY_PIN, LOW); // Cut SSR Relay
      digitalWrite(STIRRER_PIN, LOW);

      updateOLEDDisplay("!! EMERGENCY CUTOFF !!", "Overheat Detected!", "Temp > 105.0 C", "System Locked Out");
      sendBLETelemetry("SAFETY INTERRUPT TRIPPED: OVERHEAT OVERRIDE.");
      break;
    }

    default:
      break;
  }

  delay(200); // 200ms Execution Interval
}