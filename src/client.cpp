#include "client.hpp"
#include "logging.hpp"
#include "protocol.hpp"

#include <stdio.h>

using Protocol::ClientCommand;
using Protocol::ClientState;
using Protocol::MasterCommand;

namespace {
constexpr uint32_t MAX_PACKET_PAYLOAD = 4096;
constexpr unsigned long PACKET_TIMEOUT_MS = 1000;
constexpr unsigned long RECONNECT_INTERVAL_MS = 2000;
constexpr size_t PACKET_HEADER_SIZE = 7;

bool readExact(WiFiClient& client, uint8_t* buffer, size_t length) {
  const unsigned long start = millis();
  size_t received = 0;

  while (received < length &&
         millis() - start < PACKET_TIMEOUT_MS) {
    if (client.available() > 0) {
      const int value = client.read();
      if (value >= 0) {
        buffer[received++] = static_cast<uint8_t>(value);
      }
    } else if (!client.connected()) {
      break;
    } else {
      delay(1);
    }
  }

  return received == length;
}

bool sendClientResponse(
    WiFiClient& client,
    ClientCommand command,
    const String& payload) {
  const uint32_t length = payload.length();

  const uint8_t header[PACKET_HEADER_SIZE] = {
      static_cast<uint8_t>(PROTOCOL_SIGNATURE & 0xff),
      static_cast<uint8_t>((PROTOCOL_SIGNATURE >> 8) & 0xff),
      static_cast<uint8_t>(command),
      static_cast<uint8_t>(length & 0xff),
      static_cast<uint8_t>((length >> 8) & 0xff),
      static_cast<uint8_t>((length >> 16) & 0xff),
      static_cast<uint8_t>((length >> 24) & 0xff),
  };

  if (client.write(header, sizeof(header)) != sizeof(header)) {
    return false;
  }

  return length == 0 ||
         client.write(
             reinterpret_cast<const uint8_t*>(payload.c_str()),
             length) == length;
}

String stateName(ClientState state) {
  switch (state) {
    case ClientState::RUNNING:
      return "RUNNING";
    case ClientState::PAUSED:
      return "PAUSED";
    case ClientState::WAITING:
    default:
      return "WAITING";
  }
}

String makeInfoJson(const ClientInfo& info, ClientState state) {
  String json;
  json.reserve(220);

  json += "{\"deviceId\":\"";
  json += info.deviceId;
  json += "\",\"firmwareVersion\":\"";
  json += info.firmwareVersion;
  json += "\",\"ipAddress\":\"";
  json += info.ipAddress;
  json += "\",\"macAddress\":\"";
  json += info.macAddress;
  json += "\",\"state\":\"";
  json += stateName(state);
  json += "\",\"running\":";
  json += info.running ? "true" : "false";
  json += ",\"uptimeMs\":";
  json += String(info.uptimeMs);
  json += "}";

  return json;
}
}  // namespace

void RemoteCommandClient::begin(
    const char* ssid,
    const char* password,
    uint16_t port) {
  _serverPort = port;

  WiFi.begin(ssid);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  LOGI("WiFi connected");
  LOGI("Client IP: %s", WiFi.localIP().toString().c_str());
  LOGI(
      "Connecting to supervisor at %s:%u",
      _masterAddress.toString().c_str(),
      _serverPort);

  connectToMaster();
}

void RemoteCommandClient::connectToMaster() {
  if (WiFi.status() != WL_CONNECTED || _client.connected()) {
    return;
  }

  const unsigned long now = millis();

  if (_hasAttemptedConnection &&
      now - _lastConnectAttemptMs < RECONNECT_INTERVAL_MS) {
    return;
  }

  _hasAttemptedConnection = true;
  _lastConnectAttemptMs = now;

  _client.stop();

  if (_client.connect(_masterAddress, _serverPort)) {
    LOGI("Connected to supervisor");
  } else {
    LOGW("Could not connect to supervisor; will retry");
  }
}

void RemoteCommandClient::loop() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  // Si la connexion est perdue et qu'aucune donnée n'attend,
  // on tente de joindre à nouveau le superviseur.
  if (!_client.connected() && _client.available() == 0) {
    _client.stop();
    connectToMaster();
    return;
  }

  if (_client.available() < static_cast<int>(PACKET_HEADER_SIZE)) {
    return;
  }

  uint8_t header[PACKET_HEADER_SIZE];
  if (!readExact(_client, header, sizeof(header))) {
    LOGW("Incomplete packet header");
    _client.stop();
    return;
  }

  const uint16_t signature =
      static_cast<uint16_t>(header[0]) |
      (static_cast<uint16_t>(header[1]) << 8);

  const uint8_t commandId = header[2];

  const uint32_t payloadLength =
      static_cast<uint32_t>(header[3]) |
      (static_cast<uint32_t>(header[4]) << 8) |
      (static_cast<uint32_t>(header[5]) << 16) |
      (static_cast<uint32_t>(header[6]) << 24);

  if (signature != PROTOCOL_SIGNATURE ||
      payloadLength > MAX_PACKET_PAYLOAD) {
    LOGW("Invalid packet signature or payload too large");
    _client.stop();
    return;
  }

  String payload;
  payload.reserve(payloadLength);

  for (uint32_t i = 0; i < payloadLength; ++i) {
    uint8_t byte;

    if (!readExact(_client, &byte, 1)) {
      LOGW("Incomplete packet payload");
      _client.stop();
      return;
    }

    payload += static_cast<char>(byte);
  }

  LOGI("Command received: %u", commandId);

  switch (static_cast<MasterCommand>(commandId)) {
    case MasterCommand::START:
      startSystem();
      break;

    case MasterCommand::STOP:
      stopSystem();
      break;

    case MasterCommand::PAUSE:
      pauseSystem();
      break;

    case MasterCommand::RESET:
      resetSystem();
      break;

    case MasterCommand::IDENT: {
      // The supervisor stores the IDENT payload directly as the client name.
      if (!sendClientResponse(_client, ClientCommand::IDENT, _deviceId)) {
        LOGE("Failed to send IDENT response");
        _client.stop();
      }
      break;
    }

    case MasterCommand::ALIVE:
      if (!sendClientResponse(_client, ClientCommand::ALIVE, String())) {
        LOGE("Failed to send ALIVE response");
        _client.stop();
      }
      break;

    case MasterCommand::STATUS: {
      ClientInfo info = getClientInfo();

      if (!sendClientResponse(
              _client,
              ClientCommand::STATUS,
              makeInfoJson(info, _state))) {
              LOGE("Failed to send STATUS response");
        _client.stop();
      }
      break;
    }

    case MasterCommand::CUSTOM:
      if (_handlers.onCustom != nullptr) {
        _handlers.onCustom(
            _handlerContext,
            payload.c_str(),
            payload.length());
      } else {
        LOGW("CUSTOM command received, but no handler is defined");
      }
      break;

    case MasterCommand::INVALID:
    default:
      LOGW("Unknown command: %u", commandId);
      break;
  }

  // La connexion n'est pas fermée après chaque commande :
  // elle reste disponible pour les échanges suivants.
}

void RemoteCommandClient::setCommandHandlers(
    const ClientCommandHandlers& handlers,
    void* context) {
  _handlers = handlers;
  _handlerContext = context;
}

void RemoteCommandClient::setIdentity(
    const char* deviceId,
    const char* firmwareVersion) {
  if (deviceId != nullptr) {
    _deviceId = deviceId;
  }
  if (firmwareVersion != nullptr) {
    _firmwareVersion = firmwareVersion;
  }
}

void RemoteCommandClient::startSystem() {
  _state = ClientState::RUNNING;
  if (_handlers.onStart != nullptr) {
    _handlers.onStart(_handlerContext);
  }
  LOGI("System started");
}

void RemoteCommandClient::stopSystem() {
  _state = ClientState::WAITING;
  if (_handlers.onStop != nullptr) {
    _handlers.onStop(_handlerContext);
  }
  LOGI("System stopped");
}

void RemoteCommandClient::pauseSystem() {
  _state = ClientState::PAUSED;
  if (_handlers.onPause != nullptr) {
    _handlers.onPause(_handlerContext);
  }
  LOGI("System paused");
}

void RemoteCommandClient::resetSystem() {
  _state = ClientState::WAITING;
  if (_handlers.onReset != nullptr) {
    _handlers.onReset(_handlerContext);
  }
  LOGI("System reset");
}

ClientInfo RemoteCommandClient::getClientInfo() {
  ClientInfo info;

  info.deviceId = _deviceId;
  info.firmwareVersion = _firmwareVersion;
  info.ipAddress = WiFi.localIP().toString();

  uint8_t mac[6];
  WiFi.macAddress(mac);

  char macText[18];
  snprintf(
      macText,
      sizeof(macText),
      "%02x:%02x:%02x:%02x:%02x:%02x",
      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  info.macAddress = macText;
  info.running = (_state == ClientState::RUNNING);
  info.uptimeMs = millis();

  return info;
}

bool RemoteCommandClient::isSystemRunning() const {
  return _state == ClientState::RUNNING;
}
