# 🏥 Neuro Sign Medical Care Station

An assistive smart gesture glove and bedside ICU monitoring station designed for paralysis and motor-impaired patients. This system translates micro-gestures and finger flexions into real-time voice alerts, patient request notifications, environmental telemetry, and home appliance controls.

---

## 🌟 Key Features

- **Assistive Smart Glove (Transmitter - TX)**:
  - **Sensors**: MPU6050 (6-DOF IMU) for hand orientation + 3x Flex Sensors for finger detection.
  - **Wireless Connectivity**: NRF24L01 2.4 GHz RF module for low-latency wireless communication.
  - **Dual Mode Operation**:
    - **Mode 1 (Patient Needs)**: Flat hand orientation translates finger flexions into critical requests (*"I need Water"*, *"I need Food"*, *"I need Help / Emergency"*).
    - **Mode 2 (Appliance Automation)**: Tilted hand orientation toggles 3 relay-driven home appliances (Lights, Fans, Aux).
    - **Emergency Siren Trigger**: Hand tilt gesture triggers immediate audio alarm.

- **Bedside Receiver & Actuation Hub (Receiver - RX)**:
  - **Microcontroller**: Arduino UNO Q (Main) & Arduino NANO (Supportive)
  - **Peripherals**: NRF24L01 transceiver, 3-channel relay module, MAX98357A I2S Class-D amplifier.
  - **Environmental Sensing**: DHT11 (Temp & Humidity), BMP280 (Atmospheric Pressure), SGP40 (Air Quality Index / VOC).
  - **Serial Telemetry**: JSON stream output over USB Serial to the Web Dashboard.

- **Interactive Medical Station Dashboard (Python Flask Web Server)**:
  - **Real-Time Telemetry**: Non-blocking serial stream with Server-Sent Events (SSE).
  - **Smart COM Port Manager**: Auto-discovery, auto-reconnect, baud rate selection, and port diagnosis.
  - **Patient Alert Center**: Live status feeds, audio voice synthesis / alarms, and history logging.
  - **Environment Monitor**: Real-time charts for temperature, humidity, atmospheric pressure, and air quality index (AQI).
  - **Appliance Remote Control**: Bidirectional relay control directly from the web interface.
  - **Simulation Mode**: Built-in simulator for offline testing without hardware attached.

---

## 📁 Repository Structure

```
├── Transmitter/
│   └── Transmitter.ino       # Arduino sketch for the smart glove (TX)
├── Receiver/
│   └── Receiver.ino          # Arduino sketch for bedside receiver hub (RX)
├── Transmitter.ino           # Root sketch file
├── Receiver.ino              # Root sketch file
├── templates/
│   └── index.html            # Web dashboard user interface
├── medical_station.py        # Flask backend server & Serial SSE bridge
├── requirements.txt          # Python dependencies
├── run_medical_station.bat   # Windows one-click startup script
└── .gitignore                # Git ignore rules
```

---

## 🚀 Getting Started

### 1. Hardware Requirements & Wiring

#### Transmitter (Smart Glove)
- **Arduino NANO (Supportive)** / **Arduino UNO Q (Main)**
- **NRF24L01 Module** (CE: Pin 9, CSN: Pin 10, SCK: Pin 13, MOSI: Pin 11, MISO: Pin 12)
- **MPU6050 IMU** (SDA: A4, SCL: A5)
- **3x Flex Sensors** (Pins A0, A1, A2 via voltage divider)

#### Receiver (Bedside Station)
- **Arduino UNO Q (Main) and Arduino NANO (supportive)**
- **NRF24L01 Module** (CE: Pin 9, CSN: Pin 10, SCK: Pin 13, MOSI: Pin 11, MISO: Pin 12)
- **3-Channel Relay Module** (IN1: Pin 3, IN2: Pin 4, IN3: Pin 5)
- **MAX98357A I2S Amplifier** (BCLK: Pin 6, LRC: Pin 7, DIN: Pin 8)
- **DHT11 Sensor** (Data: Pin 2)

---

### 2. Software Installation & Launch

#### Prerequisites
- **Python 3.8+**
- **Arduino IDE** (with `RF24`, `DHT`, `Adafruit BMP280`, `Adafruit SGP40` libraries)

#### Running the Medical Station
1. Clone this repository:
   ```bash
   git clone https://github.com/Rudra-Attri-Pandey/neuro-sign-medical-care-station.git
   cd neuro-sign-medical-care-station
   ```

2. **Windows Quick Start**:
   Double click `run_medical_station.bat` or run:
   ```cmd
   run_medical_station.bat
   ```

3. **Manual Start**:
   ```bash
   pip install -r requirements.txt
   python medical_station.py
   ```
4. Open your browser and navigate to:
   ```
   http://localhost:5000
   ```

---

## 👨‍💻 Author

- **Rudra Attri Pandey** (GitHub: [@Rudra-Attri-Pandey](https://github.com/Rudra-Attri-Pandey))

---

## 🛡️ License

This project is licensed under the MIT License.
