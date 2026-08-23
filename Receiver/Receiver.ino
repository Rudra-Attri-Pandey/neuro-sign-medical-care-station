/*
 * =========================================================================
 * Assistive Smart Glove - RECEIVER (RX) + 3 Relays + MAX98357A + JSON USB Stream
 * Arduino Uno / Nano + NRF24L01 + 3x Relays + MAX98357A + DHT11 + BMP280 + SGP40
 * [MEMORY OPTIMIZED FOR ATmega328P 2KB SRAM]
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
 * [2. 3-Channel Relay Module (Home Appliance Control)]
 *   VCC        --->  5V
 *   GND        --->  GND
 *   IN1 (Appliance 1 / Light) --->  Pin 3
 *   IN2 (Appliance 2 / Fan)   --->  Pin 4
 *   IN3 (Appliance 3 / Aux)   --->  Pin 5
 * 
 * [3. MAX98357A I2S Class-D Audio Amplifier (Emergency Siren)]
 *   VIN / VDD  --->  5V
 *   GND        --->  GND
 *   BCLK       --->  Pin 6 (Bit Clock)
 *   LRC / WS   --->  Pin 7 (Word Select / Left-Right Clock)
 *   DIN        --->  Pin 8 (Data In)
 *   GAIN       --->  Unconnected (or GND for 12dB)
 *   SD_MODE    --->  Unconnected
 *   Speaker +/- ->   Connect to 4Ω / 8Ω Speaker (3W)
 * 
 * [4. DHT11 Temperature & Humidity Sensor]
 *   VCC        --->  5V
 *   GND        --->  GND
 *   DATA / OUT --->  Pin 2
 * 
 * [5. BMP280 Barometric Pressure & Temp Sensor (I2C)]
 *   VCC        --->  3.3V (or 5V if module has 3.3V regulator)
 *   GND        --->  GND
 *   SCL        --->  A5 (I2C SCL)
 *   SDA        --->  A4 (I2C SDA)
 * 
 * [6. SGP40 Air Quality / VOC Sensor (I2C)]
 *   VCC        --->  3.3V (or 5V)
 *   GND        --->  GND
 *   SCL        --->  A5 (I2C SCL - shared with BMP280)
 *   SDA        --->  A4 (I2C SDA - shared with BMP280)
 * =========================================================================
 */

#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>
#include <Wire.h>
#include <DHT.h>
#include <Adafruit_BMP280.h>

// --- NRF24L01 Setup ---
RF24 radio(9, 10); // CE = 9, CSN = 10
const byte address[6] = "00001";

// --- 3-Channel Relay Pins ---
const int RELAY_PIN_1 = 3; // Appliance 1
const int RELAY_PIN_2 = 4; // Appliance 2
const int RELAY_PIN_3 = 5; // Appliance 3

#define RELAY_ON  LOW
#define RELAY_OFF HIGH

bool relay1State = false;
bool relay2State = false;
bool relay3State = false;

// --- MAX98357A I2S Audio Amplifier Pins ---
const int I2S_BCLK = 6; // Bit Clock
const int I2S_LRC  = 7; // Word Select (Left/Right Clock)
const int I2S_DIN  = 8; // Audio Data In

// --- Sensors ---
#define DHTPIN 2
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

Adafruit_BMP280 bmp; // I2C
bool bmpAvailable = false;
bool sgpAvailable = false;

const int SGP40_I2C_ADDR = 0x59;

// --- Data Package Received from Smart Glove TX ---
struct DataPackage {
  float roll;
  float pitch;
  uint8_t mode;         // 1 = Patient Needs Mode, 2 = Home Automation Mode
  uint8_t relayCommand; // 0 = None, 1 = Toggle Relay 1, 2 = Toggle Relay 2, 3 = Toggle Relay 3
  char message[32];     // Patient Feeling / Status message
};

DataPackage txData;
bool newPacketReceived = false;

unsigned long lastSensorReportTime = 0;
const unsigned long SENSOR_REPORT_INTERVAL = 1000; // ms

// Track last patient call to avoid audio spam
unsigned long lastSirenTime = 0;
const unsigned long SIREN_COOLDOWN = 1200; // ms

void applyRelayStates() {
  digitalWrite(RELAY_PIN_1, relay1State ? RELAY_ON : RELAY_OFF);
  digitalWrite(RELAY_PIN_2, relay2State ? RELAY_ON : RELAY_OFF);
  digitalWrite(RELAY_PIN_3, relay3State ? RELAY_ON : RELAY_OFF);
}

// -------------------------------------------------------------------------
// MAX98357A Lightweight I2S Sound Synthesizer
// -------------------------------------------------------------------------
void sendI2SSample(int16_t sample) {
  digitalWrite(I2S_LRC, LOW);
  for (int8_t i = 15; i >= 0; i--) {
    digitalWrite(I2S_BCLK, LOW);
    digitalWrite(I2S_DIN, (sample >> i) & 1);
    digitalWrite(I2S_BCLK, HIGH);
  }
  
  digitalWrite(I2S_LRC, HIGH);
  for (int8_t i = 15; i >= 0; i--) {
    digitalWrite(I2S_BCLK, LOW);
    digitalWrite(I2S_DIN, (sample >> i) & 1);
    digitalWrite(I2S_BCLK, HIGH);
  }
}

void playI2STone(int freqHz, int durationMs) {
  int halfPeriodUs = 500000 / freqHz;
  int cycles = (long)durationMs * 1000 / (halfPeriodUs * 2);
  
  for (int c = 0; c < cycles; c++) {
    for (int k = 0; k < 6; k++) {
      sendI2SSample(12000);
    }
    for (int k = 0; k < 6; k++) {
      sendI2SSample(-12000);
    }
  }
}

void playPatientSiren() {
  playI2STone(960, 160);
  playI2STone(680, 160);
  playI2STone(960, 160);
  playI2STone(680, 160);
  
  digitalWrite(I2S_DIN, LOW);
  digitalWrite(I2S_BCLK, LOW);
}

// -------------------------------------------------------------------------
// Lightweight SGP40 I2C Reader & AQI Calculation (0 - 500 Scale)
// -------------------------------------------------------------------------
uint8_t sgp40_crc(uint8_t d1, uint8_t d2) {
  uint8_t crc = 0xFF;
  uint8_t b[2] = {d1, d2};
  for (uint8_t i = 0; i < 2; i++) {
    crc ^= b[i];
    for (uint8_t bit = 8; bit > 0; --bit) {
      if (crc & 0x80) crc = (crc << 1) ^ 0x31;
      else crc = (crc << 1);
    }
  }
  return crc;
}

uint16_t readSGP40Raw(float temp, float hum) {
  Wire.beginTransmission(SGP40_I2C_ADDR);
  Wire.write(0x26);
  Wire.write(0x0F);
  
  uint16_t hum_ticks = (uint16_t)(hum * 65535.0 / 100.0);
  uint16_t temp_ticks = (uint16_t)((temp + 45.0) * 65535.0 / 175.0);
  
  uint8_t h_msb = hum_ticks >> 8;
  uint8_t h_lsb = hum_ticks & 0xFF;
  Wire.write(h_msb);
  Wire.write(h_lsb);
  Wire.write(sgp40_crc(h_msb, h_lsb));
  
  uint8_t t_msb = temp_ticks >> 8;
  uint8_t t_lsb = temp_ticks & 0xFF;
  Wire.write(t_msb);
  Wire.write(t_lsb);
  Wire.write(sgp40_crc(t_msb, t_lsb));
  
  if (Wire.endTransmission() != 0) return 0;
  
  delay(35);
  
  if (Wire.requestFrom(SGP40_I2C_ADDR, 3) == 3) {
    uint8_t msb = Wire.read();
    uint8_t lsb = Wire.read();
    Wire.read();
    return ((uint16_t)msb << 8) | lsb;
  }
  return 0;
}

// Convert SGP40 raw VOC signal into Standard AQI Index (0 - 500 scale)
uint16_t calculateAQI(uint16_t raw_voc) {
  if (raw_voc == 0) return 25; // Default safe clean baseline
  
  if (raw_voc >= 29000) {
    // 0 - 50: Good (Pure, Fresh Air)
    long v = map(constrain(raw_voc, 29000, 32500), 32500, 29000, 10, 50);
    return (uint16_t)constrain(v, 0, 50);
  } else if (raw_voc >= 24000) {
    // 51 - 100: Moderate (Acceptable Room Air)
    long v = map(constrain(raw_voc, 24000, 29000), 29000, 24000, 51, 100);
    return (uint16_t)constrain(v, 51, 100);
  } else if (raw_voc >= 18000) {
    // 101 - 200: Unhealthy (Elevated VOC / Bio-Effluents)
    long v = map(constrain(raw_voc, 18000, 24000), 24000, 18000, 101, 200);
    return (uint16_t)constrain(v, 101, 200);
  } else {
    // 201 - 500: Very Unhealthy / Hazardous
    long v = map(constrain(raw_voc, 8000, 18000), 18000, 8000, 201, 500);
    return (uint16_t)constrain(v, 201, 500);
  }
}

bool checkSGP40() {
  Wire.beginTransmission(SGP40_I2C_ADDR);
  return (Wire.endTransmission() == 0);
}

// -------------------------------------------------------------------------
// JSON Telemetry Output (Streams cleanly to Python USB Medical Station)
// -------------------------------------------------------------------------
void sendJSONTelemetry() {
  float dht_humidity = dht.readHumidity();
  float dht_temp = dht.readTemperature();

  float bmp_temp = 0.0;
  float bmp_pressure = 0.0;
  if (bmpAvailable) {
    bmp_temp = bmp.readTemperature();
    bmp_pressure = bmp.readPressure() / 100.0F;
  }

  uint16_t raw_voc = 0;
  uint16_t aqi = 25;
  if (sgpAvailable) {
    float comp_temp = (!isnan(dht_temp) && dht_temp > 0) ? dht_temp : 25.0;
    float comp_hum = (!isnan(dht_humidity) && dht_humidity > 0) ? dht_humidity : 50.0;
    raw_voc = readSGP40Raw(comp_temp, comp_hum);
    aqi = calculateAQI(raw_voc);
  }

  // Stream structured JSON line with AQI calculation over USB Serial
  Serial.print(F("{\"glove_connected\":"));
  Serial.print(newPacketReceived ? F("true") : F("false"));
  Serial.print(F(",\"message\":\""));
  Serial.print(newPacketReceived ? txData.message : "Waiting for Glove Signal");
  Serial.print(F("\",\"mode\":"));
  Serial.print(newPacketReceived ? txData.mode : 1);
  Serial.print(F(",\"roll\":"));
  Serial.print(newPacketReceived ? txData.roll : 0.0, 1);
  Serial.print(F(",\"pitch\":"));
  Serial.print(newPacketReceived ? txData.pitch : 0.0, 1);
  Serial.print(F(",\"relay1\":"));
  Serial.print(relay1State ? F("true") : F("false"));
  Serial.print(F(",\"relay2\":"));
  Serial.print(relay2State ? F("true") : F("false"));
  Serial.print(F(",\"relay3\":"));
  Serial.print(relay3State ? F("true") : F("false"));
  Serial.print(F(",\"temperature\":"));
  Serial.print(!isnan(dht_temp) && dht_temp > 0 ? dht_temp : 0.0, 1);
  Serial.print(F(",\"humidity\":"));
  Serial.print(!isnan(dht_humidity) && dht_humidity > 0 ? dht_humidity : 0.0, 1);
  Serial.print(F(",\"pressure\":"));
  Serial.print(bmpAvailable && bmp_pressure > 300.0 ? bmp_pressure : 0.0, 1);
  Serial.print(F(",\"aqi\":"));
  Serial.print(aqi);
  Serial.print(F(",\"voc_raw\":"));
  Serial.print(raw_voc);
  Serial.println(F("}"));
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // 1. Relays Setup
  pinMode(RELAY_PIN_1, OUTPUT);
  pinMode(RELAY_PIN_2, OUTPUT);
  pinMode(RELAY_PIN_3, OUTPUT);
  applyRelayStates();

  // 2. MAX98357A I2S Setup
  pinMode(I2S_BCLK, OUTPUT);
  pinMode(I2S_LRC, OUTPUT);
  pinMode(I2S_DIN, OUTPUT);
  digitalWrite(I2S_BCLK, LOW);
  digitalWrite(I2S_LRC, LOW);
  digitalWrite(I2S_DIN, LOW);

  // 3. NRF24L01 Setup
  if (!radio.begin()) {
    Serial.println(F("{\"error\":\"NRF24L01 hardware not detected\"}"));
    while (1);
  }

  radio.setChannel(76);
  radio.setDataRate(RF24_250KBPS);
  radio.setPALevel(RF24_PA_LOW);
  radio.openReadingPipe(1, address);
  radio.startListening();

  // 4. DHT11 Setup
  dht.begin();

  // 5. BMP280 Setup
  Wire.begin();
  if (bmp.begin(0x76) || bmp.begin(0x77)) {
    bmpAvailable = true;
  }

  // 6. SGP40 Setup
  if (checkSGP40()) {
    sgpAvailable = true;
  }

  strcpy(txData.message, "Patient Resting");
  txData.mode = 1;
}

void loop() {
  // 1. Handle incoming USB Serial Commands from Python Dashboard
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "R1_TOGGLE" || cmd == "TOGGLE_1") {
      relay1State = !relay1State;
      applyRelayStates();
      sendJSONTelemetry();
    } else if (cmd == "R2_TOGGLE" || cmd == "TOGGLE_2") {
      relay2State = !relay2State;
      applyRelayStates();
      sendJSONTelemetry();
    } else if (cmd == "R3_TOGGLE" || cmd == "TOGGLE_3") {
      relay3State = !relay3State;
      applyRelayStates();
      sendJSONTelemetry();
    } else if (cmd == "SIREN_TEST") {
      playPatientSiren();
    }
  }

  // 2. Check for incoming Smart Glove NRF24 wireless packet
  if (radio.available()) {
    radio.read(&txData, sizeof(DataPackage));
    newPacketReceived = true;

    // Execute Relay Toggle Commands (Mode 2)
    if (txData.relayCommand == 1) {
      relay1State = !relay1State;
      applyRelayStates();
    } else if (txData.relayCommand == 2) {
      relay2State = !relay2State;
      applyRelayStates();
    } else if (txData.relayCommand == 3) {
      relay3State = !relay3State;
      applyRelayStates();
    }

    // Sound Siren on Patient Needs (Mode 1)
    if (txData.mode == 1 && strcmp(txData.message, "Patient Resting") != 0) {
      unsigned long now = millis();
      if (now - lastSirenTime >= SIREN_COOLDOWN) {
        lastSirenTime = now;
        playPatientSiren();
      }
    }

    sendJSONTelemetry();
  }

  // 3. Periodic Environmental Telemetry Broadcast (every 1 second)
  unsigned long currentMillis = millis();
  if (currentMillis - lastSensorReportTime >= SENSOR_REPORT_INTERVAL) {
    lastSensorReportTime = currentMillis;
    sendJSONTelemetry();
  }
}
