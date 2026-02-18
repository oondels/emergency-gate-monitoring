#include <Arduino.h>
#include <Wire.h>
#include <RTClib.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SocketIOclient.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// --- Configuracoes de Rede e Servidor ---
const char *ssid = "DASS-CORP";
const char *password = "dass7425corp";
#define URL_LINK "http://10.110.21.21:3028/portao_emerg"
const char *websockets_server = "10.110.21.21";
const uint16_t websockets_server_port = 3028;

// --- Pins ---
const int BUTTON_PIN = 16;  // GPIO16
const int SIRENE_PIN = 17;  // GPIO17
constexpr int SDA_PIN = 21; // ESP32 default I2C SDA
constexpr int SCL_PIN = 22; // ESP32 default I2C SCL

// --- Identificacao do dispositivo ---
const char *door_id = "5";
const char *name = "Portão Emergência Expedição Nike";

// --- Estado de porta/offline ---
const int ARRAY_SIZE = 20;
String offline_openings[ARRAY_SIZE];
int current_array_index = 0;
bool offlineMode = false;
bool doorState = false;

// --- Debounce ---
int buttonState = LOW;
int lastButtonState = LOW;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

// --- Sirene ---
const unsigned long sirenDuration = 10000;
bool sirenActive = false;
unsigned long sirenStartTime = 0;

// --- Heartbeat/Socket ---
const unsigned long heartbeatIntervalMs = 5000;
const unsigned long heartbeatAckTimeoutMs = 20000;
const unsigned long socketConnectTimeoutMs = 20000;
const unsigned long reconnectBackoffMaxMs = 30000;

bool socketConnected = false;
bool socketConnecting = false;
bool wsReady = false;
bool concludeSent = false;
bool concludeAckReceived = false;
unsigned long concludeSentMs = 0;
unsigned long lastHeartbeatSentMs = 0;
unsigned long lastHeartbeatAckMs = 0;
unsigned long nextSocketRetryMs = 0;
unsigned long socketBackoffMs = 1000;
unsigned long socketConnectAttemptMs = 0;

// --- WiFi ---
unsigned long nextWiFiRetryMs = 0;
unsigned long wiFiBackoffMs = 1000;

// --- HTTP queue (ring buffer) ---
enum HttpEventType : uint8_t
{
  HTTP_EVENT_DOOR_STATE = 0,
  HTTP_EVENT_OFFLINE_SYNC = 1
};

struct HttpQueueItem
{
  HttpEventType type;
  bool open;
};

const size_t HTTP_QUEUE_SIZE = 40;
const unsigned long httpFlushIntervalMs = 1000;
const int httpConnectTimeoutMs = 2000;
const int httpReadTimeoutMs = 3000;

HttpQueueItem httpQueue[HTTP_QUEUE_SIZE];
size_t httpQueueHead = 0;
size_t httpQueueTail = 0;
size_t httpQueueCount = 0;
bool offlineSyncQueued = false;
SemaphoreHandle_t stateMutex = nullptr;
TaskHandle_t httpTaskHandle = nullptr;

// --- Objetos ---
RTC_DS3231 rtc;
SocketIOclient socketIO;

bool lockState(TickType_t timeout = pdMS_TO_TICKS(50))
{
  if (stateMutex == nullptr)
  {
    return false;
  }
  return xSemaphoreTake(stateMutex, timeout) == pdTRUE;
}

void unlockState()
{
  if (stateMutex != nullptr)
  {
    xSemaphoreGive(stateMutex);
  }
}

size_t queueSize()
{
  size_t size = 0;
  if (lockState())
  {
    size = httpQueueCount;
    unlockState();
  }
  return size;
}

void clearOfflineOpeningsLocked()
{
  for (int i = 0; i < ARRAY_SIZE; i++)
  {
    offline_openings[i].clear();
  }
  current_array_index = 0;
}

bool hasOfflineOpenings()
{
  bool found = false;
  if (lockState())
  {
    for (int i = 0; i < ARRAY_SIZE; i++)
    {
      if (offline_openings[i].length())
      {
        found = true;
        break;
      }
    }
    unlockState();
  }
  return found;
}

void offline_monitoring(const String &ts)
{
  if (!lockState())
  {
    Serial.println("[QUEUE] Falha ao bloquear mutex para offline_monitoring");
    return;
  }
  offline_openings[current_array_index] = ts;
  current_array_index = (current_array_index + 1) % ARRAY_SIZE;
  unlockState();
}

bool pushQueue(HttpEventType type, bool open)
{
  if (!lockState())
  {
    Serial.println("[QUEUE] Falha ao bloquear mutex para enqueue");
    return false;
  }

  if (httpQueueCount == HTTP_QUEUE_SIZE)
  {
    HttpEventType droppedType = httpQueue[httpQueueHead].type;
    if (droppedType == HTTP_EVENT_OFFLINE_SYNC)
    {
      offlineSyncQueued = false;
    }
    httpQueueHead = (httpQueueHead + 1) % HTTP_QUEUE_SIZE;
    httpQueueCount--;
    Serial.println("[QUEUE] Fila cheia, descartando item mais antigo");
  }

  httpQueue[httpQueueTail].type = type;
  httpQueue[httpQueueTail].open = open;
  httpQueueTail = (httpQueueTail + 1) % HTTP_QUEUE_SIZE;
  httpQueueCount++;

  if (type == HTTP_EVENT_OFFLINE_SYNC)
  {
    offlineSyncQueued = true;
  }

  size_t size = httpQueueCount;
  unlockState();
  Serial.printf("[QUEUE] Enfileirado tipo=%u tamanho=%u\n", static_cast<unsigned>(type), static_cast<unsigned>(size));
  return true;
}

bool peekQueue(HttpQueueItem &item)
{
  if (!lockState())
  {
    return false;
  }
  if (httpQueueCount == 0)
  {
    unlockState();
    return false;
  }
  item = httpQueue[httpQueueHead];
  unlockState();
  return true;
}

bool popQueue()
{
  if (!lockState())
  {
    return false;
  }
  if (httpQueueCount == 0)
  {
    unlockState();
    return false;
  }

  HttpEventType poppedType = httpQueue[httpQueueHead].type;
  httpQueueHead = (httpQueueHead + 1) % HTTP_QUEUE_SIZE;
  httpQueueCount--;
  if (poppedType == HTTP_EVENT_OFFLINE_SYNC)
  {
    offlineSyncQueued = false;
  }
  size_t size = httpQueueCount;
  unlockState();
  Serial.printf("[QUEUE] Removido item tipo=%u tamanho=%u\n", static_cast<unsigned>(poppedType), static_cast<unsigned>(size));
  return true;
}

void enqueueOfflineSyncIfNeeded()
{
  bool shouldEnqueue = false;
  if (!lockState())
  {
    return;
  }

  bool hasData = false;
  for (int i = 0; i < ARRAY_SIZE; i++)
  {
    if (offline_openings[i].length())
    {
      hasData = true;
      break;
    }
  }

  if (hasData && !offlineSyncQueued)
  {
    shouldEnqueue = true;
  }
  unlockState();

  if (shouldEnqueue)
  {
    pushQueue(HTTP_EVENT_OFFLINE_SYNC, false);
  }
}

String getRtcTimestamp()
{
  DateTime t = rtc.now();
  char ts[24];
  snprintf(ts,
           sizeof(ts),
           "%02d/%02d/%04d %02d:%02d:%02d",
           t.day(),
           t.month(),
           t.year(),
           t.hour(),
           t.minute(),
           t.second());
  return String(ts);
}

String buildDoorPayload(bool open)
{
  DynamicJsonDocument doc(256);
  doc["open"] = open;
  doc["offline_mode"] = false;
  doc["door"] = door_id;

  String payload;
  serializeJson(doc, payload);
  return payload;
}

String buildOfflinePayload()
{
  DynamicJsonDocument doc(1024);
  JsonArray arr = doc.createNestedArray("offline_openings");
  doc["offline_mode"] = true;
  doc["door"] = door_id;

  if (lockState())
  {
    for (int i = 0; i < ARRAY_SIZE; i++)
    {
      if (offline_openings[i].length())
      {
        arr.add(offline_openings[i]);
      }
    }
    unlockState();
  }

  String payload;
  serializeJson(doc, payload);
  return payload;
}

bool sendHttpPayload(const String &payload, const char *label)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.printf("[HTTP] WiFi offline, adiado (%s)\n", label);
    return false;
  }

  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(httpConnectTimeoutMs);
  http.setTimeout(httpReadTimeoutMs);

  if (!http.begin(client, URL_LINK))
  {
    Serial.printf("[HTTP] Falha no begin (%s)\n", label);
    return false;
  }

  http.addHeader("Content-Type", "application/json");
  int code = http.POST((uint8_t *)payload.c_str(), payload.length());
  String resp;
  if (code > 0)
  {
    resp = http.getString();
  }
  else
  {
    resp = http.errorToString(code);
  }
  http.end();

  if (code >= 200 && code < 300)
  {
    Serial.printf("[HTTP] OK (%s) code=%d\n", label, code);
    return true;
  }

  Serial.printf("[HTTP] Erro (%s) code=%d resp=%s\n", label, code, resp.c_str());
  return false;
}

bool processHttpQueueItem(const HttpQueueItem &item)
{
  if (item.type == HTTP_EVENT_DOOR_STATE)
  {
    String payload = buildDoorPayload(item.open);
    bool ok = sendHttpPayload(payload, item.open ? "door_open" : "door_close");
    if (ok)
    {
      if (!hasOfflineOpenings())
      {
        offlineMode = false;
      }
    }
    else
    {
      offlineMode = true;
    }
    return ok;
  }

  if (item.type == HTTP_EVENT_OFFLINE_SYNC)
  {
    if (!hasOfflineOpenings())
    {
      offlineMode = false;
      Serial.println("[HTTP] Sync offline sem dados pendentes");
      return true;
    }

    String payload = buildOfflinePayload();
    bool ok = sendHttpPayload(payload, "offline_sync");
    if (ok)
    {
      if (lockState())
      {
        clearOfflineOpeningsLocked();
        unlockState();
      }
      offlineMode = false;
      Serial.println("[HTTP] Sync offline concluido");
    }
    else
    {
      offlineMode = true;
    }
    return ok;
  }

  return false;
}

void httpFlushTask(void *pvParameters)
{
  (void)pvParameters;
  unsigned long lastFlushMs = 0;
  unsigned long lastOfflineLogMs = 0;

  while (true)
  {
    unsigned long now = millis();
    if (now - lastFlushMs >= httpFlushIntervalMs)
    {
      lastFlushMs = now;

      HttpQueueItem item;
      if (peekQueue(item))
      {
        if (WiFi.status() != WL_CONNECTED)
        {
          if (now - lastOfflineLogMs >= 5000)
          {
            lastOfflineLogMs = now;
            Serial.printf("[HTTP] Aguardando WiFi para flush, fila=%u\n", static_cast<unsigned>(queueSize()));
          }
        }
        else
        {
          if (processHttpQueueItem(item))
          {
            popQueue();
          }
          else
          {
            Serial.printf("[HTTP] Flush falhou, retry pendente, fila=%u\n", static_cast<unsigned>(queueSize()));
          }
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

bool payloadMatchesDoor(JsonVariantConst data)
{
  if (data.isNull() || !data.is<JsonObjectConst>())
  {
    return true;
  }

  const char *payloadDoor = data["door"] | "";
  if (strlen(payloadDoor) == 0)
  {
    return true;
  }
  return strcmp(payloadDoor, door_id) == 0;
}

void sendDoorStatusEvent()
{
  DynamicJsonDocument doc(256);
  JsonArray arr = doc.to<JsonArray>();
  arr.add("door_status");
  JsonObject obj = arr.createNestedObject();
  obj["door"] = door_id;
  obj["status"] = doorState;

  String payload;
  serializeJson(doc, payload);
  socketIO.send(sIOtype_EVENT, (uint8_t *)payload.c_str(), payload.length());
  Serial.printf("[WS] door_status enviado status=%s\n", doorState ? "open" : "close");
}

void sendConcludeConnection()
{
  DynamicJsonDocument doc(256);
  JsonArray arr = doc.to<JsonArray>();
  arr.add("conclude_connection");
  JsonObject obj = arr.createNestedObject();
  obj["door"] = door_id;
  obj["name"] = name;
  obj["status"] = doorState;

  String payload;
  serializeJson(doc, payload);
  socketIO.send(sIOtype_EVENT, (uint8_t *)payload.c_str(), payload.length());

  concludeSent = true;
  concludeSentMs = millis();
  concludeAckReceived = false;
  wsReady = false;
  Serial.printf("[WS] conclude_connection enviado status=%s\n", doorState ? "open" : "close");
}

void scheduleSocketReconnect(const char *reason, unsigned long now)
{
  socketConnected = false;
  socketConnecting = false;
  wsReady = false;
  concludeSent = false;
  concludeAckReceived = false;
  nextSocketRetryMs = now + socketBackoffMs;

  Serial.printf("[WS] Reconnect agendado (%s) em %lu ms\n", reason, socketBackoffMs);
  if (socketBackoffMs < reconnectBackoffMaxMs)
  {
    socketBackoffMs *= 2;
    if (socketBackoffMs > reconnectBackoffMaxMs)
    {
      socketBackoffMs = reconnectBackoffMaxMs;
    }
  }
}

void forceSocketReconnect(const char *reason, unsigned long now)
{
  Serial.printf("[WS] Reconexao forcada: %s\n", reason);
  socketIO.disconnect();
  scheduleSocketReconnect(reason, now);
}

void startSocketConnect(unsigned long now, const char *reason)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    return;
  }

  socketIO.begin(websockets_server,
                 websockets_server_port,
                 "/socket.io/?EIO=4");
  socketConnecting = true;
  socketConnectAttemptMs = now;
  Serial.printf("[WS] Tentando conectar (%s)\n", reason);
}

void handleSocketReconnect(unsigned long now)
{
  if (WiFi.status() != WL_CONNECTED)
  {
    return;
  }

  if (socketConnected)
  {
    return;
  }

  if (socketConnecting)
  {
    if (now - socketConnectAttemptMs > socketConnectTimeoutMs)
    {
      Serial.println("[WS] Timeout de conexao Socket.IO");
      scheduleSocketReconnect("connect_timeout", now);
    }
    return;
  }

  if (now >= nextSocketRetryMs)
  {
    startSocketConnect(now, "backoff");
  }
}

void scheduleWiFiReconnect(const char *reason, unsigned long now)
{
  nextWiFiRetryMs = now + wiFiBackoffMs;
  Serial.printf("[WIFI] Reconnect agendado (%s) em %lu ms\n", reason, wiFiBackoffMs);
  if (wiFiBackoffMs < reconnectBackoffMaxMs)
  {
    wiFiBackoffMs *= 2;
    if (wiFiBackoffMs > reconnectBackoffMaxMs)
    {
      wiFiBackoffMs = reconnectBackoffMaxMs;
    }
  }
}

void handleWiFiReconnect(unsigned long now)
{
  if (WiFi.status() == WL_CONNECTED)
  {
    return;
  }

  if (now < nextWiFiRetryMs)
  {
    return;
  }

  Serial.printf("[WIFI] Tentando reconectar SSID=%s\n", ssid);
  WiFi.disconnect(false, false);
  WiFi.begin(ssid, password);
  scheduleWiFiReconnect("retry", now);
}

void sendHeartbeat(unsigned long now)
{
  if (!socketConnected)
  {
    return;
  }
  if (now - lastHeartbeatSentMs < heartbeatIntervalMs)
  {
    return;
  }

  DynamicJsonDocument doc(256);
  JsonArray arr = doc.to<JsonArray>();
  arr.add("heartbeat");
  JsonObject obj = arr.createNestedObject();
  obj["door"] = door_id;
  obj["ts"] = now;
  obj["rssi"] = WiFi.RSSI();

  String payload;
  serializeJson(doc, payload);
  socketIO.send(sIOtype_EVENT, (uint8_t *)payload.c_str(), payload.length());
  lastHeartbeatSentMs = now;

  Serial.printf("[HB] heartbeat enviado ts=%lu rssi=%d wsReady=%d\n", now, WiFi.RSSI(), wsReady ? 1 : 0);
}

void checkHeartbeatAckWatchdog(unsigned long now)
{
  if (!socketConnected)
  {
    return;
  }
  if (now - lastHeartbeatAckMs > heartbeatAckTimeoutMs)
  {
    forceSocketReconnect("heartbeat_ack_timeout", now);
  }
}

void handleWSEvents(uint8_t *payload, size_t length)
{
  String raw((const char *)payload, length);

  if (raw.startsWith("42"))
  {
    raw.remove(0, 2);
  }

  DynamicJsonDocument doc(768);
  DeserializationError err = deserializeJson(doc, raw);
  if (err || !doc.is<JsonArray>())
  {
    Serial.printf("[WS] Payload invalido: %s\n", raw.c_str());
    return;
  }

  JsonArray arr = doc.as<JsonArray>();
  if (arr.size() < 1 || !arr[0].is<const char *>())
  {
    Serial.printf("[WS] Evento sem nome: %s\n", raw.c_str());
    return;
  }

  const char *event = arr[0].as<const char *>();
  JsonVariantConst data;
  if (arr.size() > 1)
  {
    data = arr[1].as<JsonVariantConst>();
  }

  if (strcmp(event, "start_conclude_connection") == 0)
  {
    sendConcludeConnection();
    return;
  }

  if (strcmp(event, "conclude_ack") == 0)
  {
    if (payloadMatchesDoor(data))
    {
      concludeAckReceived = true;
      wsReady = true;
      Serial.println("[WS] conclude_ack recebido, wsReady=1");
      sendDoorStatusEvent();
    }
    return;
  }

  if (strcmp(event, "heartbeat_ack") == 0)
  {
    if (payloadMatchesDoor(data))
    {
      lastHeartbeatAckMs = millis();
      Serial.printf("[HB] heartbeat_ack recebido ts=%lu\n", lastHeartbeatAckMs);
      if (concludeSent && !wsReady)
      {
        wsReady = true;
        Serial.println("[WS] wsReady=1 por heartbeat_ack apos conclude_connection");
        sendDoorStatusEvent();
      }
    }
    return;
  }

  if (strcmp(event, "get_door_status") == 0)
  {
    if (data.is<JsonObjectConst>())
    {
      const char *requestedDoor = data["door"] | "";
      if (strlen(requestedDoor) == 0 || strcmp(requestedDoor, door_id) == 0)
      {
        sendDoorStatusEvent();
      }
      else
      {
        Serial.printf("[WS] get_door_status ignorado para door=%s\n", requestedDoor);
      }
    }
    else
    {
      sendDoorStatusEvent();
    }
    return;
  }

  Serial.printf("[WS] Evento ignorado: %s\n", event);
}

void socketIOEvent(socketIOmessageType_t type, uint8_t *payload, size_t length)
{
  switch (type)
  {
  case sIOtype_CONNECT:
    socketConnected = true;
    socketConnecting = false;
    wsReady = false;
    concludeSent = false;
    concludeAckReceived = false;
    socketBackoffMs = 1000;
    lastHeartbeatAckMs = millis();
    lastHeartbeatSentMs = 0;
    Serial.println("[WS] Conectado");
    socketIO.send(sIOtype_CONNECT, "/");
    sendDoorStatusEvent();
    break;

  case sIOtype_EVENT:
    handleWSEvents(payload, length);
    break;

  case sIOtype_DISCONNECT:
    Serial.println("[WS] Desconectado");
    scheduleSocketReconnect("disconnect_event", millis());
    break;

  default:
    break;
  }
}

void handleWiFiEvent(arduino_event_id_t event, arduino_event_info_t info)
{
  switch (event)
  {
  case ARDUINO_EVENT_WIFI_STA_CONNECTED:
    Serial.println("[WIFI] STA connected");
    break;

  case ARDUINO_EVENT_WIFI_STA_GOT_IP:
    Serial.printf("[WIFI] Got IP: %s\n", WiFi.localIP().toString().c_str());
    wiFiBackoffMs = 1000;
    nextWiFiRetryMs = 0;
    nextSocketRetryMs = 0;
    if (hasOfflineOpenings())
    {
      enqueueOfflineSyncIfNeeded();
    }
    break;

  case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    Serial.printf("[WIFI] STA disconnected (reason=%d)\n", info.wifi_sta_disconnected.reason);
    offlineMode = true;
    socketIO.disconnect();
    scheduleSocketReconnect("wifi_disconnect", millis());
    scheduleWiFiReconnect("event_disconnect", millis());
    break;

  default:
    break;
  }
}

void enqueueDoorStateChange(bool open, unsigned long now)
{
  pushQueue(HTTP_EVENT_DOOR_STATE, open);

  if (open)
  {
    digitalWrite(SIRENE_PIN, LOW);
    sirenActive = true;
    sirenStartTime = now;
    Serial.println("[QUEUE] Porta abriu, sirene ligada");

    if (WiFi.status() != WL_CONNECTED || offlineMode)
    {
      offlineMode = true;
      String ts = getRtcTimestamp();
      offline_monitoring(ts);
      enqueueOfflineSyncIfNeeded();
      Serial.printf("[QUEUE] Abertura offline armazenada em RTC: %s\n", ts.c_str());
    }
  }
  else
  {
    if (sirenActive)
    {
      digitalWrite(SIRENE_PIN, HIGH);
      sirenActive = false;
      Serial.println("[QUEUE] Porta fechou, sirene desligada");
    }
  }

  if (socketConnected)
  {
    sendDoorStatusEvent();
  }
}

void checkRelayState(int reading, unsigned long now)
{
  if (reading != lastButtonState)
  {
    lastDebounceTime = now;
  }

  if (now - lastDebounceTime > debounceDelay)
  {
    if (reading != buttonState)
    {
      buttonState = reading;
      if (buttonState == HIGH && !doorState)
      {
        doorState = true;
        enqueueDoorStateChange(true, now);
      }
      else if (buttonState == LOW && doorState)
      {
        doorState = false;
        enqueueDoorStateChange(false, now);
      }
    }
  }

  lastButtonState = reading;
}

void checkSirenDuration(unsigned long now)
{
  if (sirenActive && (now - sirenStartTime >= sirenDuration))
  {
    digitalWrite(SIRENE_PIN, HIGH);
    sirenActive = false;
    Serial.println("[QUEUE] Sirene desligada por timeout");
  }
}

void setup()
{
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(SIRENE_PIN, OUTPUT);
  digitalWrite(SIRENE_PIN, HIGH);
  Serial.println("\n[SISTEMA] Inicializando...");

  stateMutex = xSemaphoreCreateMutex();
  if (stateMutex == nullptr)
  {
    Serial.println("[QUEUE] Falha ao criar mutex");
    while (true)
    {
      delay(1000);
    }
  }

  if (lockState())
  {
    clearOfflineOpeningsLocked();
    unlockState();
  }

  Wire.begin(SDA_PIN, SCL_PIN);
  if (!rtc.begin())
  {
    Serial.println("[RTC] Falha na inicializacao");
    while (true)
    {
      delay(10);
    }
  }
  if (rtc.lostPower())
  {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    Serial.println("[RTC] Data/hora ajustadas");
  }

  int initialReading = digitalRead(BUTTON_PIN);
  buttonState = initialReading;
  lastButtonState = initialReading;
  doorState = (initialReading == HIGH);
  if (doorState)
  {
    digitalWrite(SIRENE_PIN, LOW);
    sirenActive = true;
    sirenStartTime = millis();
    Serial.println("[QUEUE] Porta iniciou aberta, sirene ligada");
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.onEvent(handleWiFiEvent);
  WiFi.begin(ssid, password);
  nextWiFiRetryMs = millis() + wiFiBackoffMs;
  Serial.printf("[WIFI] Iniciando conexao com %s\n", ssid);

  socketIO.onEvent(socketIOEvent);
  nextSocketRetryMs = 0;

  BaseType_t taskOk = xTaskCreate(
      httpFlushTask,
      "http_flush",
      6144,
      nullptr,
      1,
      &httpTaskHandle);
  if (taskOk != pdPASS)
  {
    Serial.println("[HTTP] Falha ao iniciar task de flush");
  }
  else
  {
    Serial.println("[HTTP] Task de flush iniciada");
  }
}

void loop()
{
  socketIO.loop();

  unsigned long now = millis();
  handleWiFiReconnect(now);
  handleSocketReconnect(now);
  enqueueOfflineSyncIfNeeded();

  int reading = digitalRead(BUTTON_PIN);
  checkRelayState(reading, now);
  sendHeartbeat(now);
  checkHeartbeatAckWatchdog(now);
  checkSirenDuration(now);

  delay(2);
}
