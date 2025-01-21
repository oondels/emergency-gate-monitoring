#include <Arduino.h>
#include <ArduinoJson.h>
#include <Hash.h>
#include <Wire.h>
#include <RTClib.h>

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WebSocketsClient.h>
#include <SocketIOclient.h>

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Conexão Display OLED
#define OLED_MOSI 13
#define OLED_CLK 14
#define OLED_DC 15
#define OLED_CS 16
#define OLED_RESET 12
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// Conexão Servidor
#define SSID "example-SSID"
#define PASS "example-password"
#define url_link "http://192.168.0.1:1010/portao_emerg"
const char* websockets_server = "192.168.0.1";
const uint16_t websockets_server_port = 1010;

// Armazenamento de aberturas offlines
const int array_size = 20;
int current_array_index = 0;
int time_count = 0;
String offline_openings[array_size];

const char* door_number = "2";
const int buttonPin = D2;
const int ledPin = LED_BUILTIN;

bool offlineMode = false;
bool doorState = false;
int buttonState = 0;
int lastButtonState = LOW;
unsigned long lastDebounceTime = 0;
unsigned long debounceDelay = 50;

unsigned long displayTime = 0;
const unsigned long messageDuration = 2500;
bool displayMessage = false;

unsigned long previousMillis = 0;
const unsigned long heartbeatInterval = 60000;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &SPI, OLED_DC, OLED_RESET, OLED_CS);
SocketIOclient socketIO;
RTC_DS3231 rtc;

void setup() {
  Serial.begin(115200);
  pinMode(ledPin, OUTPUT);
  pinMode(buttonPin, INPUT_PULLUP);

  // Inicializa a tela OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC)) {
    Serial.println(F("Falha ao inicializar a tela OLED"));
    for (;;)
      ;
  }
  Serial.println(F("Tela OLED inicializada"));

  updateDisplay("Configurando", "Monitoramento.");

  delay(2000);
  display.clearDisplay();
  display.display();

  Wire.begin(D4, D1);  // SDA, SCL - Inicializa a comunicação modulo RTC

  // Inicializa o RTC
  if (!rtc.begin()) {
    Serial.println("Não foi possível encontrar o RTC");
    while (1)
      ;
  }

  // Verifica se o RTC perdeu a energia e, se sim, define a data e hora
  if (rtc.lostPower()) {
    Serial.println("RTC perdeu a energia, configurando a data e hora!");
    // Define a data e hora para a compilação do código
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  Serial.println();
  Serial.println();
  Serial.println();

  updateDisplay("Conectando", "WIFI");

  // Conexão com Wifi
  WiFi.begin(SSID, PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.println("Conectando ao WiFi...");
  }

  updateDisplay("WIFI", "Conectado");

  delay(1500);

  Serial.println("");
  Serial.print("Connected! IP address: ");
  Serial.println(WiFi.localIP());

  // Conexão com Servidor WebSocket (Socket.io)
  socketIO.begin(websockets_server, websockets_server_port, "/socket.io/?EIO=4");
  socketIO.onEvent(socketIOEvent);

  for (int i = 0; i < array_size; i++) {
    offline_openings[i] = "";
  }
}

void loop() {
  if (offlineMode) {
    updateDisplay("Monitoramento", "Offline!");
  } else if (WiFi.status() != WL_CONNECTED) {
    updateDisplay("WIFI Desconectado!", "");
  } else if (!displayMessage) {
    updateDisplay("Monitoramento", "Portao");
  }

  socketIO.loop();
  unsigned long currentMillis = millis();

  int reading = digitalRead(buttonPin);

  if (reading != lastButtonState) {
    lastDebounceTime = currentMillis;
  }

  // Garantindo que pelo menos 50ms tenham passado desde a última mudança de estado do botão.
  if ((currentMillis - lastDebounceTime) > debounceDelay) {
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == HIGH) {
        digitalWrite(ledPin, LOW);

        if (doorState == false) {
          doorState = true;
          previousMillis = currentMillis;

          // Verificando se esta em modo offline para armazenar as aberturas em array interno
          if (offlineMode) {
            DateTime now = rtc.now();
            String dateTime = String(now.day()) + "/" + String(now.month()) + "/" + String(now.year()) + " " + String(now.hour()) + ":" + String(now.minute()) + ":" + String(now.second());
            offline_monitoring(dateTime);
            sendArray();
            displayTime = currentMillis;
            displayMessage = true;
          } else {
            updateDisplay("Porta Aberta!", "Enviando dados...");
            sendData(true);
            displayTime = currentMillis;
            displayMessage = true;
          }
        }
      } else {
        digitalWrite(ledPin, HIGH);
        if (doorState == true) {
          doorState = false;
          if (offlineMode == false) {
            sendData(false);
          }
        }
      }
    }
  }

  if (displayMessage && (currentMillis - displayTime >= messageDuration)) {
    displayMessage = false;
    updateDisplay("Monitoramento", "Portao");
  }

  lastButtonState = reading;

  // Enviar sinais periodicamente para o servidor
  if (currentMillis - previousMillis >= heartbeatInterval) {
    previousMillis = currentMillis;

    // create JSON message for Socket.IO (event)
    DynamicJsonDocument doc(1024);
    JsonArray array = doc.to<JsonArray>();

    array.add("heartbeat");
    JsonObject param1 = array.createNestedObject();
    param1[door_number] = (uint32_t)currentMillis;
    String payload;
    serializeJson(doc, payload);
    socketIO.sendEVENT(payload);
  }
}

void socketIOEvent(socketIOmessageType_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case sIOtype_DISCONNECT:
      // Serial.printf("[IOc] Disconnected!\n");
      break;

    case sIOtype_CONNECT:
      Serial.printf("Connected to url: %s\n", payload);

      // join default namespace (no auto join in Socket.IO V3)
      socketIO.send(sIOtype_CONNECT, "/");
      break;

    // Captação de eventos
    case sIOtype_EVENT:
      // Serial.printf("[IOc] get event: %s\n", payload);
      {
        String payloadStr = String((char*)payload);
        // Serial.println(payloadStr);

        // Conversão da resposta para JSON
        DynamicJsonDocument doc(1024);
        DeserializationError error = deserializeJson(doc, payloadStr);

        if (!error) {
          // Separando array em string ([0]) e JSON ([1])
          const char* eventName = doc[0];
          JsonObject data = doc[1];

          if (strcmp(eventName, "connection_lost") == 0) {
            const char* message = data["message"];
          }
        } else {
          Serial.print("deserializeJson() failed: ");
          Serial.println(error.c_str());
        }
      }

      break;

    case sIOtype_ACK:
      Serial.printf("[IOc] get ack: %u\n", length);
      hexdump(payload, length);
      break;

    case sIOtype_ERROR:
      Serial.printf("[IOc] get error: %u\n", length);
      hexdump(payload, length);
      break;

    case sIOtype_BINARY_EVENT:
      Serial.printf("[IOc] get binary: %u\n", length);
      hexdump(payload, length);
      break;

    case sIOtype_BINARY_ACK:
      Serial.printf("[IOc] get binary ack: %u\n", length);
      hexdump(payload, length);
      break;
  }
}

void updateDisplay(String text, String text2) {
  display.clearDisplay();
  display.drawRect(0, 0, SCREEN_WIDTH, 30, SSD1306_WHITE);
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(20, 10);
  display.println(F("Dass"));
  display.setTextSize(1);
  display.setCursor(20, 45);
  display.println(text);
  if (text2.length() > 0) {
    display.setCursor(20, 55);
    display.println(text2);
  }
  display.display();
}

void offline_monitoring(String time) {
  offline_openings[current_array_index] = time;
  current_array_index = (current_array_index + 1) % array_size;
  if (time_count < array_size) {
    time_count++;
  }
}

void sendData(bool state) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;

    http.begin(client, url_link);
    http.addHeader("Content-Type", "application/json");

    char doorData[100];
    if (state == true) {
      snprintf(doorData, sizeof(doorData), "{\"open\": true, \"offline_mode\":false, \"door\": \"%s\"}", door_number);
    } else {
      snprintf(doorData, sizeof(doorData), "{\"open\": false, \"offline_mode\":false, \"door\": \"%s\"}", door_number);
    }

    int httpResponseCode = http.POST(doorData);

    const String& result = http.getString();
    if (httpResponseCode != 200) {
      offlineMode = true;
      Serial.print("Erro ao enviar dados.");
      Serial.println(result);
    }

    http.end();
  } else {
    offlineMode = true;
    Serial.println("WiFi Disconnected");
  }
}

void sendArray() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;

    http.begin(client, url_link);
    http.addHeader("Content-Type", "application/json");

    DynamicJsonDocument doc(1024);
    JsonArray array = doc.createNestedArray("offline_openings");

    for (int i = 0; i < array_size; i++) {
      if (offline_openings[i] != "") {
        array.add(offline_openings[i]);
      }
    }

    doc["offline_mode"] = true;
    doc["door"] = door_number;
    String jsonString;
    serializeJson(doc, jsonString);

    int httpResponseCode = http.POST(jsonString);
    if (httpResponseCode > 0) {
      if (httpResponseCode == 200) {
        offlineMode = false;

        const String& result = http.getString();
        Serial.println(result);
        for (int i = 0; i < array_size; i++) {
          offline_openings[i] = "";
        }
      } else {
        Serial.print("Error on http request: ");
        Serial.println(http.errorToString(httpResponseCode).c_str());
      }
    } else {
      Serial.print("Error on sending Offline Monitoring: ");
      Serial.println(http.errorToString(httpResponseCode).c_str());
    }

    http.end();
  } else {
    Serial.println("WiFi Disconnected");
  }
}
