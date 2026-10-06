#include <superv/client.hpp>
#include <superv/logging.hpp>
#include <superv/protocol.hpp>

#include <stdio.h>

using namespace Protocol;


constexpr uint32_t MAX_PACKET_PAYLOAD = 4096;
constexpr unsigned long PACKET_TIMEOUT_MS = 1000;
constexpr unsigned long RECONNECT_INTERVAL_MS = 2000;
constexpr unsigned long REALTIME_POLL_INTERVAL_MS = 10;
constexpr unsigned long REALTIME_WIFI_CHECK_INTERVAL_MS = 1000;
constexpr size_t PACKET_HEADER_SIZE = 7;
constexpr size_t REALTIME_READ_LIMIT = 256;

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
    }
    else if (!client.connected()) {
      break;
    }
    else {
      delay(1);
    }
  }

  return received == length;
}

bool sendClientResponse(WiFiClient& client, ClientCommand command, const void* payload = nullptr, size_t length = 0) {

  PacketSignature signature = PROTOCOL_SIGNATURE;

  PacketHeader header;
  header.clientCommandID = command;
  header.followingLength = length;

  uint32_t sent = 0;
  sent += client.write(reinterpret_cast<uint8_t*>(&signature), sizeof(PacketSignature));
  sent += client.write(header, sizeof(PacketHeader));
  sent += client.write(reinterpret_cast<const uint8_t*>(payload), length);

  LOGI("Sent %u, payload %p, length %u, sent %u", command, payload, length, sent);

  return sent == sizeof(PacketSignature) + sizeof(header) + length;
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

RemoteCommandClient::RemoteCommandClient(bool realTimeNeed) : m_realTime(realTimeNeed)
{

}

void RemoteCommandClient::setup()
{
  WiFi.begin("supervisor-net");
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED && retry < 5) {
    delay(200);
    retry++;
  }
  LOGI("Client IP: %s", WiFi.localIP().toString().c_str());
  LOGI(
    "Connecting to supervisor at %s:%u",
    _masterAddress.toString().c_str(),
    _serverPort);

  connectToMaster();
}

void RemoteCommandClient::connectToMaster(bool connectionKnownLost) {
  if (WiFi.status() != WL_CONNECTED ||
    (!connectionKnownLost && _client.connected())) {
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
  }
  else {
    LOGW("Could not connect to supervisor; will retry");
  }
}

void RemoteCommandClient::loop()
{
  if (m_realTime)
  {
    realTimeLoop();
  }
  else
  {
    normalLoop();
  }
}

void RemoteCommandClient::normalLoop() {

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
    if (!sendClientResponse(_client, ClientCommand::IDENT, _deviceId.c_str(), _deviceId.length())) {
      LOGE("Failed to send IDENT response");
    }
    break;
  }

  case MasterCommand::ALIVE:
    if (!sendClientResponse(_client, ClientCommand::ALIVE, nullptr, 0)) {
      LOGE("Failed to send ALIVE response");
      _client.stop();
    }
    break;

  case MasterCommand::STATUS: {
    ClientInfo info = getClientInfo();

    if (!sendClientResponse(_client, ClientCommand::STATUS, nullptr, 0))
    {
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
    }
    else {
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

void RemoteCommandClient::realTimeLoop()
{
  const unsigned long now = millis();
  if (now - _lastRealtimePollMs < REALTIME_POLL_INTERVAL_MS) {
    return;
  }
  _lastRealtimePollMs = now;

  if (now - _lastRealtimeWifiCheckMs >= REALTIME_WIFI_CHECK_INTERVAL_MS) {
    _lastRealtimeWifiCheckMs = now;
    if (WiFi.status() != WL_CONNECTED) {
      resetReceiveState();
      return;
    }
  }

  if (m_receiveHeaderBytes > 0 &&
    now - _receiveStartedMs >= PACKET_TIMEOUT_MS) {
    LOGW("Incomplete packet");
    resetReceiveState();
    _client.stop();
    return;
  }

  int available = _client.available();
  if (available <= 0) {
    if (now - _lastRealtimeReconnectCheckMs >= RECONNECT_INTERVAL_MS) {
      _lastRealtimeReconnectCheckMs = now;
      if (!_client.connected()) {
        resetReceiveState();
        connectToMaster(true);
      }
    }
    return;
  }

  if (m_receiveHeaderBytes < PACKET_HEADER_SIZE) {
    if (m_receiveHeaderBytes == 0) {
      _receiveStartedMs = now;
    }
    const size_t headerBytesNeeded = PACKET_HEADER_SIZE - m_receiveHeaderBytes;
    const size_t bytesToRead = available < static_cast<int>(headerBytesNeeded)
      ? static_cast<size_t>(available)
      : headerBytesNeeded;
    const int bytesRead = _client.read(
      m_receiveHeader + m_receiveHeaderBytes,
      bytesToRead);
    if (bytesRead <= 0) {
      return;
    }
    m_receiveHeaderBytes += static_cast<uint8_t>(bytesRead);
    available -= bytesRead;

    if (m_receiveHeaderBytes < PACKET_HEADER_SIZE) {
      return;
    }

    const uint16_t signature =
      static_cast<uint16_t>(m_receiveHeader[0]) |
      (static_cast<uint16_t>(m_receiveHeader[1]) << 8);

    m_receiveCommand = m_receiveHeader[2];
    m_receivePayloadLength =
      static_cast<uint32_t>(m_receiveHeader[3]) |
      (static_cast<uint32_t>(m_receiveHeader[4]) << 8) |
      (static_cast<uint32_t>(m_receiveHeader[5]) << 16) |
      (static_cast<uint32_t>(m_receiveHeader[6]) << 24);

    if (signature != PROTOCOL_SIGNATURE ||
      m_receivePayloadLength > MAX_PACKET_PAYLOAD) {
      LOGW("Invalid packet signature or payload too large");
      resetReceiveState();
      _client.stop();
      return;
    }

    m_receivePayload = "";
    m_receivePayload.reserve(m_receivePayloadLength);
  }

  if (m_receivePayload.length() < m_receivePayloadLength && available > 0) {
    char payloadChunk[REALTIME_READ_LIMIT];
    const uint32_t payloadBytesNeeded =
      m_receivePayloadLength - m_receivePayload.length();
    size_t bytesToRead = available < static_cast<int>(REALTIME_READ_LIMIT)
      ? static_cast<size_t>(available)
      : REALTIME_READ_LIMIT;
    if (bytesToRead > payloadBytesNeeded) {
      bytesToRead = payloadBytesNeeded;
    }

    const int bytesRead = _client.read(
      reinterpret_cast<uint8_t*>(payloadChunk),
      bytesToRead);
    if (bytesRead <= 0) {
      return;
    }
    m_receivePayload.concat(
      payloadChunk,
      static_cast<unsigned int>(bytesRead));
  }

  if (m_receivePayload.length() < m_receivePayloadLength) {
    return;
  }

  const uint8_t commandId = m_receiveCommand;
  const String& payload = m_receivePayload;
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
    if (!sendClientResponse(_client, ClientCommand::IDENT, _deviceId.c_str(), _deviceId.length())) {
      LOGE("Failed to send IDENT response");
      _client.stop();
    }
    break;
  }

  case MasterCommand::ALIVE:
    if (!sendClientResponse(_client, ClientCommand::ALIVE, nullptr, 0)) {
      LOGE("Failed to send ALIVE response");
      _client.stop();
    }
    break;

  case MasterCommand::STATUS: {
    ClientInfo info = getClientInfo();

    if (!sendClientResponse(_client, ClientCommand::STATUS, nullptr, 0))
    {
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
    }
    else {
      LOGW("CUSTOM command received, but no handler is defined");
    }
    break;

  case MasterCommand::INVALID:
  default:
    LOGW("Unknown command: %u", commandId);
    break;
  }

  resetReceiveState();
}

void RemoteCommandClient::resetReceiveState()
{
  m_receiveHeaderBytes = 0;
  m_receiveCommand = 0;
  m_receivePayloadLength = 0;
  _receiveStartedMs = 0;
  m_receivePayload = "";
}

void RemoteCommandClient::setCommandHandlers(
  const ClientCommandHandlers& handlers,
  void* context) {
  _handlers = handlers;
  _handlerContext = context;
}

void RemoteCommandClient::setRealTimeNeed(bool realTimeNeed)
{
  m_realTime = realTimeNeed;
}

void RemoteCommandClient::setIdentity(const char* deviceId) {
  if (deviceId != nullptr) {
    _deviceId = deviceId;
  }
}

void RemoteCommandClient::createCustom(const char* commandName)
{
  if (m_customCommandCount < DEFINE_CLIENT_CUSTOM_MAX)
  {
    Protocol::CustomCommand command;
    strncpy(command.commandName, commandName, sizeof(Protocol::CustomCommand::commandName));
    command.commandID = m_customCommandCount;

    m_customCommands[command.commandID] = command;

    sendClientResponse(_client, ClientCommand::CUSTOM, &m_customCommands[m_customCommandCount],
                        min(strlen(commandName) + sizeof(Protocol::CustomCommand::commandID), sizeof(Protocol::CustomCommand)));
    m_customCommandCount++;
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
