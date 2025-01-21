# Emergency Gate Monitoring Project
![Aplicativo Monitoramento](https://github.com/oondels/emergency-gate-monitoring/blob/main/images/emergency_gate_front.jpg?raw=true)
![Aplicativo Monitoramento Abertura](https://github.com/oondels/emergency-gate-monitoring/blob/main/images/emergency_gate_openings.jpg?raw=true)
![Aplicativo Monitoramento Sem sinal](https://github.com/oondels/emergency-gate-monitoring/blob/main/images/emergency_gate_no-signal.jpg?raw=true)
## Description

This project aims to monitor emergency gates, recording when they are opened and maintaining a log of these openings. The information is stored in a PostgreSQL database and updated in real-time on the front end with Socket.IO. When a gate is opened, the front end emits an audible and visual alert and sends a notification email.

Focuses on real-time monitoring, automated alerts, and comprehensive reporting to ensure these gates remain functional and secure.

## Key Features:

- Real Time Monitoring.
- Sensors: Installed on emergency gates to detect the open/close status.
- Comprehensive Reporting: Records all occurrences of emergency gate openings, including timestamps.
- User Interface: A user-friendly front end displays real-time updates and logs using Socket.IO.
- Automated Alerts: The front end emits loud sound and visible signals when a gate is opened.
- Email Notifications: Sends real-time notification emails to specified recipients for prompt action.

## Used Components

- **Hardware:**

  - Arduino Wemos R1 D1 WiFi
  - RTC DS3231 Real Time Clock Module
  - SPI-based 0.96" OLED Display
  - Gate sensor

- **Software:**
  - **Back-end:** Node.js, Socket.IO, PostgreSQL, NodeMailer
  - **Front-end:** Vue.js, Socket.IO
  - **Firmware:** C++ (Arduino)

## Hardware Configuration

### Connection of the OLED Display

- **GND:** GND
- **VCC:** 3.3V
- **SCL:** GPIO14 (D5)
- **SDA:** GPIO13 (D7)
- **RST:** GPIO12 (D6)
- **D/C:** GPIO15 (D10/SS)

### Connection of the RTC Module

- **GND:** GND
- **VCC:** 5V
- **D:** GPIO2 (TX1/D9)
- **C:** GPIO5 (D3)
- **NCC:** Não conectado

### Gate Sensor

- **Microswitch:** GPIO4 (D4) - GND

## Instalation and Configuration

### Firmware (Arduino)

1. Clone the repository or download the firmware files.
2. Open the `.ino` file in the Arduino IDE.
3. Configure the required libraries:
   - ArduinoJson
   - Wire
   - RTClib
   - ESP8266WiFi
   - ESP8266HTTPClient
   - WebSocketsClient
   - SocketIOclient
   - SPI
   - Adafruit_GFX
   - Adafruit_SSD1306
4. Configure the Wi-Fi and Server connection parameters:
   ```cpp
   #define SSID "your-SSID"
   #define PASS "your-password"
   #define url_link "http://192.168.0.1:1010/endpoint"
   const char* websockets_server = "192.168.0.1";
   const uint16_t websockets_server_port = 1010;
   ```
5. Set up the Node.js Server
   ```bash
   cd server
   ```
   ```bash
   npm install
   ```
6. Set up your environment variables `.env`
   - Create the `.env` file
   - Add the variables:
     - IP=`db_host`
     - PASS=`db_password`
     - USERS=`db_user`
     - PORT=`db_port || 5432 (Padrão)`
     - DBASE=`db_name`
     - EMAIL=`your_email`
     - EMAIL_PASS=`email_app_password`
