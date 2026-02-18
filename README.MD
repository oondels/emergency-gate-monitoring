# 🚪 Projeto de Monitoramento de Portões de Emergência

![Aplicativo Monitoramento](https://github.com/oondels/emergency-gate-monitoring/blob/main/images/emergency_gate_front.jpg?raw=true)
![Aplicativo Monitoramento Abertura](https://github.com/oondels/emergency-gate-monitoring/blob/main/images/emergency_gate_openings.jpg?raw=true)
![Aplicativo Monitoramento Sem sinal](https://github.com/oondels/emergency-gate-monitoring/blob/main/images/emergency_gate_no-signal.jpg?raw=true)

## 📋 Visão Geral

Este projeto implementa um sistema de monitoramento em tempo real para portões de emergência, com foco em robustez operacional e rastreabilidade:

- Firmware ESP32 (PlatformIO) para leitura do sensor, acionamento da sirene e integração com backend.
- Servidor Node.js com Socket.IO + endpoint HTTP para sincronização de eventos.
- Aplicação web (Vue.js) para monitoramento, alertas e histórico.

O objetivo é garantir operação contínua mesmo com instabilidade de rede, mantendo segurança local (sirene), sincronização de eventos e visibilidade centralizada.

## ✨ Funcionalidades

- Monitoramento em tempo real via Socket.IO.
- Handshake explícito de conexão com validação de porta.
- Heartbeat com ACK e watchdog de saúde da sessão.
- Reconexão automática de Wi-Fi e Socket.IO com backoff exponencial.
- Fila HTTP em memória (ring buffer), sem bloquear o loop principal.
- Modo offline com armazenamento de aberturas via RTC DS3231.
- Sincronização automática de eventos pendentes quando a conectividade retorna.
- Debounce de leitura de porta e controle de sirene preservados.

## 🏗️ Arquitetura

```text
┌─────────────────┐         ┌──────────────────┐         ┌─────────────────┐
│  Dispositivo    │◄───────►│   Servidor       │◄───────►│   Front-end     │
│  ESP32          │         │   Node.js        │         │   Vue.js        │
│                 │         │                  │         │                 │
│ • Sensor porta  │         │ • Socket.IO      │         │ • Dashboard     │
│ • RTC DS3231    │         │ • Express        │         │ • Alertas       │
│ • Sirene        │         │ • PostgreSQL     │         │ • Histórico     │
│ • Wi-Fi         │         │ • API /portao... │         │ • Status online │
└─────────────────┘         └──────────────────┘         └─────────────────┘
       Socket.IO (estado/heartbeat) + HTTP (persistência de eventos)
```

## 🧠 Firmware ESP32 (estado atual)

Arquivo principal: `src/main.cpp`

### Conectividade Socket.IO

- Conexão WebSocket explícita:
  - `socketIO.begin(websockets_server, websockets_server_port, "/socket.io/?EIO=4&transport=websocket")`
- `socketIO.loop()` executa continuamente no `loop()`.
- Em `sIOtype_CONNECT`, mantém `socketIO.send(sIOtype_CONNECT, "/")`.

### Handshake obrigatório

Fluxo suportado:

1. Servidor envia `start_conclude_connection`.
2. ESP32 responde imediatamente com:
   - `conclude_connection` + `{door, name, status}`.
3. `wsReady` só é considerado ativo quando:
   - chega `conclude_ack`, ou
   - após envio do `conclude_connection` + primeiro `heartbeat_ack` válido.

### Heartbeat robusto

Enviado a cada 5 segundos no formato novo:

```json
["heartbeat", { "door": "5", "ts": 123456, "rssi": -58 }]
```

Comportamento:

- `heartbeat_ack` atualiza `lastHeartbeatAckMs`.
- Se ficar mais de 20s sem ACK, o socket é reconectado de forma forçada.

### Reconexão com backoff

- Wi-Fi: 1s, 2s, 4s... até 30s.
- Socket.IO: 1s, 2s, 4s... até 30s.
- Ambos com logs de motivo e tentativa.

### Fila HTTP não bloqueante

- Eventos de abertura/fechamento não fazem `HTTPClient.POST` no fluxo da borda do botão.
- Eventos vão para ring buffer em memória.
- Uma task de flush (`httpFlushTask`) envia no máximo 1 requisição por ciclo (1s).
- Timeouts curtos:
  - connect timeout: 2000 ms
  - read timeout: 3000 ms
- Se falhar, o evento permanece na fila para retry.

### Modo offline com RTC

- Aberturas sem conectividade são armazenadas em buffer local com timestamp do DS3231.
- Ao recuperar rede, o firmware enfileira sincronização para `/portao_emerg` com `offline_mode=true`.

### Segurança local da sirene

- A sirene continua sendo acionada imediatamente ao abrir a porta.
- Desligamento por fechamento ou timeout máximo configurado.

## 📦 Estrutura do Repositório

```text
emergency-gate-monitoring/
├── src/
│   └── main.cpp              # Firmware ESP32 (PlatformIO)
├── test/
│   └── doorRepository.js     # Repositório de dados (PostgreSQL)
├── images/                   # Capturas da aplicação
├── platformio.ini            # Configuração de build/deps
└── README.MD
```

## 🔧 Configuração do Firmware

Ajuste em `src/main.cpp`:

```cpp
const char *ssid = "SUA_REDE_WIFI";
const char *password = "SUA_SENHA_WIFI";

#define URL_LINK "http://IP_SERVIDOR:3028/portao_emerg"
const char *websockets_server = "IP_SERVIDOR";
const uint16_t websockets_server_port = 3028;

const char *door_id = "1";
const char *name = "Portão Emergência 1";
```

## 🚀 Build e Upload (PlatformIO)

```bash
pio run
pio run --target upload
pio device monitor -b 115200
```

## 🔌 Pinagem ESP32

| Componente | GPIO | Função |
|---|---|---|
| Sensor/relé da porta | GPIO16 | `INPUT_PULLUP` |
| Sirene | GPIO17 | `OUTPUT` |
| RTC SDA | GPIO21 | I2C Data |
| RTC SCL | GPIO22 | I2C Clock |

## 📡 Protocolo de Comunicação

### Dispositivo → Servidor (Socket.IO)

| Evento | Payload | Descrição |
|---|---|---|
| `conclude_connection` | `{door, name, status}` | Conclusão do handshake |
| `heartbeat` | `{door, ts, rssi}` | Keep-alive do dispositivo |
| `door_status` | `{door, status}` | Estado atual da porta |

### Servidor → Dispositivo (Socket.IO)

| Evento | Payload | Descrição |
|---|---|---|
| `start_conclude_connection` | `-` | Inicia handshake obrigatório |
| `conclude_ack` | `{door}` | Confirmação de handshake |
| `heartbeat_ack` | `{door,...}` | ACK do heartbeat |
| `get_door_status` | `{door}` | Requisição de estado atual |

### Endpoint HTTP

`POST /portao_emerg`

Payload online:

```json
{
  "open": true,
  "door": "1",
  "offline_mode": false
}
```

Payload de sincronização offline:

```json
{
  "door": "1",
  "offline_mode": true,
  "offline_openings": ["18/02/2026 10:45:02", "18/02/2026 11:02:18"]
}
```

## 📊 Logs Técnicos

Prefixos padronizados no firmware:

- `[WIFI]` estado de rede e reconexão.
- `[WS]` conexão, handshake e eventos Socket.IO.
- `[HB]` envio/ack de heartbeat.
- `[HTTP]` envio/retry de persistência.
- `[QUEUE]` operações da fila local.

Exemplos:

```text
[WIFI] Got IP: 10.100.1.80
[WS] Conectado
[WS] conclude_connection enviado status=open
[HB] heartbeat enviado ts=123456 rssi=-57 wsReady=0
[HB] heartbeat_ack recebido ts=123470
[HTTP] OK (door_open) code=200
[QUEUE] Enfileirado tipo=0 tamanho=3
```

## 🧪 Checklist de Validação Manual

1. Boot com Wi-Fi ok.
2. Handshake concluído (`start_conclude_connection` → `conclude_connection` → `conclude_ack`/`heartbeat_ack`).
3. Heartbeat + ACK estável por mais de 2 minutos.
4. Simular perda de rede e verificar reconexão automática com backoff.
5. Abrir/fechar porta durante falhas e validar fila HTTP sem travar o socket.

## 🐛 Troubleshooting Rápido

### ESP32 não conecta no servidor

- Validar SSID/senha.
- Validar IP e porta do backend.
- Verificar no monitor serial logs `[WIFI]` e `[WS]`.

### Eventos não persistem

- Verificar endpoint `POST /portao_emerg`.
- Observar logs `[HTTP]` e tamanho da `[QUEUE]`.
- Confirmar conectividade entre ESP32 e backend.

### Dispositivo aparece desconectado no dashboard

- Confirmar recebimento de `heartbeat_ack`.
- Verificar timeout de ACK e reconexão do socket.

## 👥 Contribuição

Sugestões e melhorias são bem-vindas via pull request.

---

**Status**: Em produção

**Última atualização**: 18/02/2026
