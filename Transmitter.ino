/*
 * =========================================================================
 * Assistive Smart Glove for Paralysis Patients - TRANSMITTER (TX)
 * Arduino Uno / Nano + NRF24L01 + MPU6050 + 3 Flex Sensors
 * =========================================================================
 * 
 * Pin Connections:
 * 
 * [1. NRF24L01 Wireless Module]
 *   VCC        --->  3.3V  (IMPORTANT: Add a 10uF - 100uF capacitor across VCC/GND!)
 *   GND        --->  GND
 *   CE         --->  Pin 9
 *   CSN        --->  Pin 10
 *   SCK        --->  Pin 13
 *   MOSI       --->  Pin 11
 *   MISO       --->  Pin 12
 * 
 * [2. MPU6050 / MPU6500 IMU (GY-521)]
 *   VCC        --->  5V (or 3.3V)
 *   GND        --->  GND
 *   SCL        --->  A5 (I2C SCL)
 *   SDA        --->  A4 (I2C SDA)
 * 
 * [3. 3x Flex Sensors (Voltage Divider with 10k - 22k Resistor)]
 *   Flex 1 (Thumb / Finger 1)   --->  Pin A0
 *   Flex 2 (Index / Finger 2)   --->  Pin A1
 *   Flex 3 (Middle / Finger 3)  --->  Pin A2
 * 
 * =========================================================================
 * Operating Modes:
 *   MODE 1: Flat Hand (Parallel to Bed, |Roll| < 30°) -> Patient Needs Mode
 *           - Finger 1 bent: "I need Water"
 *           - Finger 2 bent: "I need Food"
 *           - Finger 3 bent: "I need Help / Emergency"
 * 
 *   MODE 2: Hand Tilted Left (Roll <= -35°) -> Home Appliance Control Mode
 *           - Finger 1 bent: Toggle Appliance 1 (Relay 1)
 *           - Finger 2 bent: Toggle Appliance 2 (Relay 2)
 *           - Finger 3 bent: Toggle Appliance 3 (Relay 3)
 * =========================================================================
 */

#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>
#include <Wire.h>

// --- NRF24L01 Setup ---
RF24 radio(9, 10); // CE = 9, CSN = 10
const byte address[6] = "00001";

// --- MPU6050 Setup ---
const int MPU_ADDR = 0x68;

// --- Flex Sensor Pins ---
const int FLEX_PIN_1 = A0; // Finger 1
const int FLEX_PIN_2 = A1; // Finger 2
const int FLEX_PIN_3 = A2; // Finger 3

// Flex Sensor Threshold (Adjust based on your resistors & flex sensor readings)
// When bent, the analog value typically crosses this threshold
const int FLEX_BEND_THRESHOLD = 500; 

// --- Data Package to Send via NRF24L01 ---
struct DataPackage {
  float roll;
  float pitch;
  uint8_t mode;         // 1 = Patient Needs Mode, 2 = Home Automation Mode
  uint8_t relayCommand; // 0 = None, 1 = Toggle Relay 1, 2 = Toggle Relay 2, 3 = Toggle Relay 3
  char message[32];     // Patient Feeling / Status message
};

DataPackage data;

// Debounce state tracking
bool lastFlex1 = false;
bool lastFlex2 = false;
bool lastFlex3 = false;
unsigned long lastActionTime = 0;
const unsigned long ACTION_DEBOUNCE = 600; // ms

void initMPU6050() {
  Wire.begin();
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B); // PWR_MGMT_1 register
  Wire.write(0x00); // Wake up MPU6050
  byte status = Wire.endTransmission();

  if (status == 0) {
    Serial.println("[OK] MPU6050 initialized successfully!");
  } else {
    Serial.println("[WARN] MPU6050 not detected. Check I2C wiring (A4=SDA, A5=SCL).");
  }
}

void readOrientation(float &roll, float &pitch) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B); // Starting register for accelerometer readings
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 6, true);

  if (Wire.available() >= 6) {
    int16_t ax = (Wire.read() << 8) | Wire.read();
    int16_t ay = (Wire.read() << 8) | Wire.read();
    int16_t az = (Wire.read() << 8) | Wire.read();

    float ax_g = (float)ax / 16384.0;
    float ay_g = (float)ay / 16384.0;
    float az_g = (float)az / 16384.0;

    roll  = atan2(ay_g, az_g) * 180.0 / PI;
    pitch = atan2(-ax_g, sqrt(ay_g * ay_g + az_g * az_g)) * 180.0 / PI;
  }
}

void setup() {
  Serial.begin(9600);
  delay(500);

  Serial.println("=================================================");
  Serial.println("  Assistive Smart Glove - Transmitter (TX)       ");
  Serial.println("=================================================");

  // Initialize NRF24L01
  if (!radio.begin()) {
    Serial.println("[ERROR] NRF24L01 not responding! Check 3.3V & SPI wiring.");
    while (1);
  }

  radio.setChannel(76);
  radio.setDataRate(RF24_250KBPS);
  radio.setPALevel(RF24_PA_LOW);
  radio.setRetries(15, 15);
  radio.openWritingPipe(address);
  radio.stopListening(); // TX Mode

  // Initialize MPU6050
  initMPU6050();

  // Initialize Flex Sensor analog pins
  pinMode(FLEX_PIN_1, INPUT);
  pinMode(FLEX_PIN_2, INPUT);
  pinMode(FLEX_PIN_3, INPUT);

  Serial.println("[OK] Transmitter Ready. Monitoring Glove Gestures...\n");
}

void loop() {
  // 1. Read MPU6050 Orientation
  readOrientation(data.roll, data.pitch);

  // 2. Read Flex Sensors
  int rawFlex1 = analogRead(FLEX_PIN_1);
  int rawFlex2 = analogRead(FLEX_PIN_2);
  int rawFlex3 = analogRead(FLEX_PIN_3);

  // Determine if fingers are bent
  bool flex1Bent = (rawFlex1 > FLEX_BEND_THRESHOLD);
  bool flex2Bent = (rawFlex2 > FLEX_BEND_THRESHOLD);
  bool flex3Bent = (rawFlex3 > FLEX_BEND_THRESHOLD);

  // Reset per-loop triggers
  data.relayCommand = 0;
  strcpy(data.message, "Normal");

  unsigned long currentMillis = millis();

  // 3. Determine Hand Mode based on Tilt
  // Hand Tilted Left (e.g. Roll <= -35 deg) -> Appliance Control Mode
  // Hand Flat (Parallel to bed, -30 deg < Roll < 30 deg) -> Patient Needs Mode
  if (data.roll <= -35.0) {
    data.mode = 2; // Home Appliance Control Mode
    strcpy(data.message, "Mode: Appliance Control");

    // Check for finger bend triggers with debounce
    if (currentMillis - lastActionTime >= ACTION_DEBOUNCE) {
      if (flex1Bent && !lastFlex1) {
        data.relayCommand = 1; // Toggle Relay 1
        strcpy(data.message, "CMD: Toggle Relay 1");
        lastActionTime = currentMillis;
      } else if (flex2Bent && !lastFlex2) {
        data.relayCommand = 2; // Toggle Relay 2
        strcpy(data.message, "CMD: Toggle Relay 2");
        lastActionTime = currentMillis;
      } else if (flex3Bent && !lastFlex3) {
        data.relayCommand = 3; // Toggle Relay 3
        strcpy(data.message, "CMD: Toggle Relay 3");
        lastActionTime = currentMillis;
      }
    }
  } else {
    data.mode = 1; // Patient Needs / Feelings Mode (Flat Hand)

    if (flex1Bent) {
      strcpy(data.message, "I need Water");
    } else if (flex2Bent) {
      strcpy(data.message, "I need Food");
    } else if (flex3Bent) {
      strcpy(data.message, "I need Medicine/Help");
    } else {
      strcpy(data.message, "Patient Resting");
    }
  }

  // Update previous states
  lastFlex1 = flex1Bent;
  lastFlex2 = flex2Bent;
  lastFlex3 = flex3Bent;

  // 4. Send Payload via NRF24L01
  bool success = radio.write(&data, sizeof(DataPackage));

  // 5. Local Serial Monitor Feedback
  if (success) {
    Serial.print("[TX] Mode: ");
    Serial.print(data.mode == 1 ? "Patient Needs" : "Appliance Control");
    Serial.print(" | Roll: ");
    Serial.print(data.roll, 1);
    Serial.print("° | Msg: \"");
    Serial.print(data.message);
    Serial.print("\" | Flex [F1:");
    Serial.print(rawFlex1);
    Serial.print(" F2:");
    Serial.print(rawFlex2);
    Serial.print(" F3:");
    Serial.print(rawFlex3);
    Serial.println("]");
  } else {
    Serial.println("[TX FAILED] No ACK received from Receiver.");
  }

  delay(100); // 10 Hz refresh rate
}
