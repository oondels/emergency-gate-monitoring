#include <Arduino.h>
#include <Wire.h>
#include <RTClib.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SocketIOclient.h>
#include <ArduinoJson.h>

// —————— Configurações de Rede e Servidor ——————
const char *ssid = "DASS-CORP";
const char *password = "dass7425corp";
#define URL_LINK "http://10.100.1.43:3028/portao_emerg"
const char *websockets_server = "10.100.1.43";
const uint16_t websockets_server_port = 3028;

// —————— Pins ——————
const int BUTTON_PIN = 16;  // GPIO16
const int SIRENE_PIN = 17;  // GPIO17
constexpr int SDA_PIN = 21; // ESP32 default I2C SDA
constexpr int SCL_PIN = 22; // ESP32 default I2C SCL

// —————— Buffers e estados ——————
const int ARRAY_SIZE = 20;
String offline_openings[ARRAY_SIZE];
int current_array_index = 0;

const char *door_id = "2";
const char *name = "Portão Emergência Doca";
bool offlineMode = false;
bool doorState = false;

// —————— Debounce ——————
int buttonState = LOW;
int lastButtonState = LOW;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

// —————— Heartbeat ——————
unsigned long previousMillis = 0;
const unsigned long heartbeatInterval = 5000;

// —————— Sirene ——————
const unsigned long sirenDuration = 10000; // Duração da sirene em ms
bool sirenActive = false;
unsigned long sirenStartTime = 0;

// —————— Objetos ——————
RTC_DS3231 rtc;
SocketIOclient socketIO;

void sendData(bool open)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    offlineMode = true;
    Serial.println("[HTTP] WiFi desconectado");
    return;
  }

  HTTPClient http;
  http.begin(URL_LINK);
  http.addHeader("Content-Type", "application/json");

  char buf[128];
  snprintf(buf, sizeof(buf),
           "{\"open\":%s,\"offline_mode\":false,\"door\":\"%s\"}",
           open ? "true" : "false",
           door_id);

  int code = http.POST(buf);
  String resp = http.getString();
  http.end();

  if (code != 200)
  {
    offlineMode = true;
    Serial.printf("[HTTP] Erro %d: %s\n", code, resp.c_str());
  }
  else
  {
    Serial.println("[HTTP] Enviado com sucesso");
  }
}

void sendArray()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("[HTTP] WiFi desconectado (array)");
    return;
  }

  DynamicJsonDocument doc(1024);
  JsonArray arr = doc.createNestedArray("offline_openings");
  for (int i = 0; i < ARRAY_SIZE; i++)
  {
    if (offline_openings[i].length())
    {
      arr.add(offline_openings[i]);
    }
  }
  doc["offline_mode"] = true;
  doc["door"] = door_id;

  String payload;
  serializeJson(doc, payload);

  HTTPClient http;
  http.begin(URL_LINK);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  String resp = http.getString();
  http.end();

  if (code == 200)
  {
    offlineMode = false;
    Serial.println("[HTTP] Buffer offline enviado");
    for (int i = 0; i < ARRAY_SIZE; i++)
    {
      offline_openings[i].clear();
    }
  }
  else
  {
    Serial.printf("[HTTP] Erro (array) %d: %s\n", code, resp.c_str());
  }
}

void offline_monitoring(const String &ts)
{
  offline_openings[current_array_index] = ts;
  current_array_index = (current_array_index + 1) % ARRAY_SIZE;
}

void handleWSEvents(uint8_t *payload, size_t length)
{
  // Serial.printf("[WS Event] Evento: %.*s\n", length, payload);
  // Analisar o payload recebido
  String raw = String((const char *)payload).substring(0, length);

  // Payload de evento
  if (raw.startsWith("42"))
  {
    Serial.println("[WS Event] Evento 42");
    raw.remove(0, 2); // remove "42"
  }

  // Esperamos agora um JSON Array: ["event", {...}]
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, raw);
  if (err)
  {
    Serial.printf("[WS Event] JSON inválido: %s | Conteúdo: %s\n", err.c_str(), raw.c_str());
    return;
  }

  if (!doc.is<JsonArray>())
  {
    Serial.printf("[WS Event] Formato inesperado (não é array): %s\n", raw.c_str());
    return;
  }

  JsonArray arr = doc.as<JsonArray>();
  if (arr.size() < 1 || !arr[0].is<const char *>())
  {
    Serial.printf("[WS Event] Array sem evento: %s\n", raw.c_str());
    return;
  }

  const char *event = arr[0].as<const char *>();
  JsonVariant data = JsonVariant(); // valor nulo por padrão
  if (arr.size() > 1)
  {
    data = arr[1];
  }

  Serial.printf("[WS Event] Evento: %s\n", event);
  if (data.isNull())
  {
    Serial.println("[WS Event] (sem payload)");
  }

  // Requisição de status da porta
  if (strcmp(event, "get_door_status") == 0)
  {
    if (data.containsKey("door"))
    {
      const char *doorId = data["door"];
      Serial.printf("[WS Event] Status da porta requisitado: %s\n", doorId);

      DynamicJsonDocument obj(256);
      auto arr = obj.to<JsonArray>();
      arr.add("door_status");
      JsonObject respObj = arr.createNestedObject();

      if (strcmp(doorId, door_id) != 0)
      {
        Serial.println("[WS Event] ID da porta não corresponde");
        return;
      }

      respObj["door"] = doorId;
      respObj["status"] = doorState;

      String payload;
      serializeJson(obj, payload);

      socketIO.send(sIOtype_EVENT,
                    (uint8_t *)payload.c_str(),
                    payload.length());
      Serial.println("[WS Event] Status da porta enviado");
    }
  }

  // Router de eventos
  if (strcmp(event, "start_conclude_connection") == 0)
  {
    DynamicJsonDocument doc(256);
    auto arr = doc.to<JsonArray>();
    arr.add("conclude_connection");
    JsonObject obj = arr.createNestedObject();

    obj["door"] = door_id;
    obj["name"] = name;
    obj["status"] = digitalRead(BUTTON_PIN) == HIGH ? true : false;
    Serial.print("Status da porta: ");
    Serial.println(digitalRead(BUTTON_PIN) == HIGH ? "Aberta" : "Fechada");
    String payload;
    serializeJson(doc, payload);

    socketIO.send(sIOtype_EVENT,
                  (uint8_t *)payload.c_str(),
                  payload.length());
    Serial.println("[WS Event] conclude_connection enviado");
  }
  else
  {
    // Serial.println("[WS Event] Evento não mapeado");
  }
};

void socketIOEvent(socketIOmessageType_t type,
                   uint8_t *payload,
                   size_t length)
{
  switch (type)
  {
  case sIOtype_CONNECT:
    Serial.println("[WS] Conectado");
    socketIO.send(sIOtype_CONNECT, "/");
    break;
  case sIOtype_EVENT:
    handleWSEvents(payload, length);
    break;
  case sIOtype_DISCONNECT:
    Serial.println("[WS] Desconectado");
    break;
  default:
    break;
  }
}

void checkRelayState(int reading, unsigned long now)
{
  // Debounce e lógica de abertura/fechamento…
  if (reading != lastButtonState)
    lastDebounceTime = now;
  if (now - lastDebounceTime > debounceDelay)
  {
    if (reading != buttonState)
    {
      buttonState = reading;
      if (buttonState == HIGH && !doorState)
      {
        // Marca porta como aberta
        doorState = true;

        // Liga sirene ao abrir a porta
        digitalWrite(SIRENE_PIN, LOW);
        sirenActive = true;
        sirenStartTime = now;

        if (offlineMode)
        {
          DateTime t = rtc.now();
          String ts = String(t.day()) + "/" + t.month() + "/" + t.year() + " " + t.hour() + ":" + t.minute() + ":" + t.second();
          offline_monitoring(ts);
          sendArray();
          Serial.println("[Offline] Abertura armazenada");
        }
        else
        {
          Serial.println("[Porta] Aberta, enviando");
          sendData(true);
        }
      }
      // Logica de atualizacao de porta fechada
      else if (buttonState == LOW && doorState)
      {
        // Desliga sirene se prota estiver fechada
        if (sirenActive)
        {
          digitalWrite(SIRENE_PIN, HIGH);
          sirenActive = false;
          Serial.println("[Sirene] Desligada - porta aberta");
        }

        doorState = false;
        if (!offlineMode)
        {
          Serial.println("[Porta] Fechada, enviando");
          sendData(false);
        }
      }
    }
  }
  lastButtonState = reading;
}

void serverHeartBeat(unsigned long now)
{
  // Heartbeat como evento Socket.WS
  if (now - previousMillis >= heartbeatInterval)
  {
    previousMillis = now;
    DynamicJsonDocument doc(256);
    auto arr = doc.to<JsonArray>();
    arr.add("heartbeat");
    JsonObject obj = arr.createNestedObject();
    obj[door_id] = now;
    String payload;
    serializeJson(doc, payload);

    socketIO.send(sIOtype_EVENT,
                  (uint8_t *)payload.c_str(),
                  payload.length());
    Serial.println("[WS] Heartbeat enviado");
  }
}

void checkSirenDuration(unsigned long now)
{
  if (sirenActive && (now - sirenStartTime >= sirenDuration))
  {
    digitalWrite(SIRENE_PIN, HIGH);
    sirenActive = false;
    Serial.println("[Sirene] Desligada após timeout");
  }
}

void setup()
{
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(SIRENE_PIN, OUTPUT);
  digitalWrite(SIRENE_PIN, HIGH);
  Serial.println("\n[Sistema] Iniciando...");

  // I2C para o RTC
  Wire.begin(SDA_PIN, SCL_PIN);
  if (!rtc.begin())
  {
    Serial.println("[RTC] Falha na inicialização");
    while (1)
      delay(10);
  }
  if (rtc.lostPower())
  {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    Serial.println("[RTC] Data/hora ajustadas");
  }

  // Conecta WiFi
  Serial.printf("[WiFi] Conectando em %s\n", ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[WiFi] Conectado com sucesso");
  Serial.printf("[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[WiFi] MAC: %s\n", WiFi.macAddress().c_str());

  // Verifica estado inicial do relé ao iniciar
  // TODO: Verificar se funcionalidade não causará erro
  int reading = digitalRead(BUTTON_PIN);
  unsigned long now = millis();
  checkRelayState(reading, now);

  // Socket.WS: host, porta e URL (forçando EIO=4)
  socketIO.begin(websockets_server,
                 websockets_server_port,
                 "/socket.io/?EIO=4");

  // Callback único para todos eventos Socket.IO
  socketIO.onEvent(socketIOEvent);

  // Limpa buffer offline
  for (int i = 0; i < ARRAY_SIZE; i++)
  {
    offline_openings[i].clear();
  }
}

void loop()
{
  socketIO.loop(); // processa Socket.IO

  unsigned long now = millis();
  int reading = digitalRead(BUTTON_PIN);

  checkRelayState(reading, now);
  serverHeartBeat(now);
  checkSirenDuration(now);
}
