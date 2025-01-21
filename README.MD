# Projeto de Monitoramento de Portões de Emergência
![Aplicativo Monitoramento](https://github.com/oondels/emergency-gate-monitoring/blob/main/images/emergency_gate_front.jpg?raw=true)
![Aplicativo Monitoramento Abertura](https://github.com/oondels/emergency-gate-monitoring/blob/main/images/emergency_gate_openings.jpg?raw=true)
![Aplicativo Monitoramento Sem sinal](https://github.com/oondels/emergency-gate-monitoring/blob/main/images/emergency_gate_no-signal.jpg?raw=true)
## Descrição

Este projeto tem como objetivo monitorar as portas de emergência, registrando quando estão abertas e mantendo um registro dessas aberturas. Essas informações são armazenadas em um banco de dados PostgreSQL e atualizadas em tempo real no front end com Socket.IO. Quando uma porta é aberta, o front end emite um alerta sonoro e visual e envia um e-mail de notificação.

O projeto foca no monitoramento em tempo real, alertas automáticos e relatórios abrangentes para garantir que essas portas permaneçam funcionais e seguras.

## Funcionalidades Chave:

- Monitoramento em tempo real
- Sensores: Instalado em portões de emergência para detectar abeturas.
- Relatórios Abrangentes: Registra todas as movimentações nos portões.
- Interface de Usuário: Visual amigável e intuitivo, mostrando em tempo real as atualizações com Socket.io.
- Alertas automáticos: O aplicativo emite alertas sonoros e visuais quando o portão é aberto.
- Notificação via email: Envio de email em tempo real relatando a abertura dos portões.

## Componentes Utilizados

- **Hardware:**

  - Arduino Wemos R1 D1 WiFi
  - Módulo Real Time Clock RTC DS3231
  - Display OLED SPI-based 0.96″
  - Sensor de portão

- **Software:**
  - **Back-end:** Node.js, Socket.IO, PostgreSQL, NodeMailer
  - **Front-end:** Vue.js, Socket.IO
  - **Firmware:** C++ (Arduino)

## Configuração do Hardware

### Conexão do Display OLED

- **GND:** GND
- **VCC:** 3.3V
- **SCL:** GPIO14 (D5)
- **SDA:** GPIO13 (D7)
- **RST:** GPIO12 (D6)
- **D/C:** GPIO15 (D10/SS)

### Conexão do Módulo RTC

- **GND:** GND
- **VCC:** 5V
- **D:** GPIO2 (TX1/D9)
- **C:** GPIO5 (D3)
- **NCC:** Não conectado

### Sensor de Portão

- **Micro:** GPIO4 (D4) - GND

## Instalação e Configuração

### Firmware (Arduino)

1. Clone o repositório ou baixe os arquivos de firmware.
2. Abra o arquivo `.ino` no Arduino IDE.
3. Configure as bibliotecas necessárias:
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
4. Configure os parâmetros de conexão WiFi e de Servidor:
   ```cpp
   #define SSID "your-SSID"
   #define PASS "your-password"
   #define url_link "http://192.168.0.1:1010/endpoint"
   const char* websockets_server = "192.168.0.1";
   const uint16_t websockets_server_port = 1010;
   ```
5. Configure Servidor Node.js
   ```bash
   cd server
   ```
   ```bash
   npm install
   ```
6. Configure suas variáveis de ambiente `.env`
   - Crie o arquivo `.env`
   - Adicione as variáveis:
     - IP=`db_host`
     - PASS=`db_password`
     - USERS=`db_user`
     - PORT=`db_port || 5432 (Padrão)`
     - DBASE=`db_name`
     - EMAIL=`seu_email`
     - EMAIL_PASS=`email_app_password`
