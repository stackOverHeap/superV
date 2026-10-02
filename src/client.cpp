#include "client.hpp"
#include "protocol.hpp"

#include <stdio.h>

namespace {
constexpr uint32_t MAX_PACKET_PAYLOAD = 4096;
constexpr unsigned long PACKET_TIMEOUT_MS = 1000;
constexpr size_t PACKET_HEADER_SIZE = 7;

bool readExact(WiFiClient& client, uint8_t* buffer, size_t length) {
  const unsigned long start = millis();
  size_t received = 0;

  while (received < length &&
         client.connected() &&
         millis() - start < PACKET_TIMEOUT_MS) {
    if (client.available()) {
      const int value = client.read();
      if (value >= 0) {
        buffer[received++] = static_cast<uint8_t>(value);
      }
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
             reinterpret_cast<const uint8_t*>(payload.c_str()), length) == length;
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
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  _server.begin(port);

  Serial.println();
  Serial.println("WiFi connected");
  Serial.println(WiFi.localIP());
}

void RemoteCommandClient::loop() {
  WiFiClient client = _server.available();
  if (!client) {
    return;
  }

  uint8_t header[PACKET_HEADER_SIZE];
  if (!readExact(client, header, sizeof(header))) {
    client.stop();
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
    Serial.println("Paquet invalide ou trop volumineux");
    client.stop();
    return;
  }

  String payload;
  payload.reserve(payloadLength);

  for (uint32_t i = 0; i < payloadLength; ++i) {
    uint8_t byte;
    if (!readExact(client, &byte, 1)) {
      client.stop();
      return;
    }
    payload += static_cast<char>(byte);
  }

  Serial.print("Commande reçue : ");
  Serial.println(commandId);

  switch (static_cast<MasterCommand>(commandId)) {
    case MasterCommand::START:
      startSystem();
      break;

    case MasterCommand::STOP:
      stopSystem();
      break;

    case MasterCommand::PAUSE:
      _state = ClientState::PAUSED;
      Serial.println("Système en pause");
      break;

    case MasterCommand::RESET:
      _state = ClientState::WAITING;
      Serial.println("Système réinitialisé");
      break;

    case MasterCommand::IDENT: {
      ClientInfo info = getClientInfo();
      String response = "{\"deviceId\":\"";
      response += info.deviceId;
      response += "\",\"firmwareVersion\":\"";
      response += info.firmwareVersion;
      response += "\"}";

      sendClientResponse(client, ClientCommand::IDENT, response);
      break;
    }

    case MasterCommand::STATUS: {
      ClientInfo info = getClientInfo();
      sendClientResponse(
          client,
          ClientCommand::STATUS,
          makeInfoJson(info, _state));
      break;
    }

    case MasterCommand::CUSTOM:
      Serial.println("Commande CUSTOM reçue, mais aucun gestionnaire n'est défini");
      break;

    case MasterCommand::INVALID:
    default:
      Serial.println("Identifiant de commande inconnu");
      break;
  }

  client.stop();
}

void RemoteCommandClient::startSystem() {
  _state = ClientState::RUNNING;
  Serial.println("Système démarré");
}

void RemoteCommandClient::stopSystem() {
  _state = ClientState::WAITING;
  Serial.println("Système arrêté");
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
