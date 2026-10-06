// RemoteCommandClient.h
#pragma once

#include <Arduino.h>
#include <WiFi.h>

#include <superv/defines.hpp>
#include <superv/protocol.hpp>

struct ClientInfo {
  String deviceId;
  String ipAddress;
  String macAddress;
  bool running = false;
  unsigned long uptimeMs = 0;
};

struct ClientCommandHandlers {
  void (*onStart)(void* context) = nullptr;
  void (*onStop)(void* context) = nullptr;
  void (*onPause)(void* context) = nullptr;
  void (*onReset)(void* context) = nullptr;
  void (*onCustom)(void* context, const char* payload, size_t length) = nullptr;
};

class RemoteCommandClient {
public:
  RemoteCommandClient() = default;
  RemoteCommandClient(bool realTimeNeed);
  void setup();
  void loop();

  void setCommandHandlers(const ClientCommandHandlers& handlers, void* context = nullptr);
  void setRealTimeNeed(bool realTimeNeed);
  void setIdentity(const char* deviceId);
  void createCustom(const char * commandName);

  void startSystem();
  void stopSystem();
  void pauseSystem();
  void resetSystem();
  ClientInfo getClientInfo();
  bool isSystemRunning() const;

private:
  void connectToMaster(bool connectionKnownLost = false);

  void normalLoop();
  void realTimeLoop();
  void resetReceiveState();

  WiFiClient _client;
  IPAddress _masterAddress = IPAddress(192, 168, 4, 1);
  uint16_t _serverPort = DEFINE_SERVER_PORT;
  unsigned long _lastConnectAttemptMs = 0;
  unsigned long _lastRealtimePollMs = 0;
  unsigned long _lastRealtimeWifiCheckMs = 0;
  unsigned long _lastRealtimeReconnectCheckMs = 0;
  unsigned long _receiveStartedMs = 0;
  bool _hasAttemptedConnection = false;
  bool m_realTime = false;
  uint8_t m_receiveHeader[7] = {};
  uint8_t m_receiveHeaderBytes = 0;
  uint8_t m_receiveCommand = 0;
  uint32_t m_receivePayloadLength = 0;
  String m_receivePayload;

  Protocol::ClientState _state = Protocol::ClientState::WAITING;
  String _deviceId = "Arduino_01";
  ClientCommandHandlers _handlers;
  Protocol::CustomCommand m_customCommands[DEFINE_CLIENT_CUSTOM_MAX];
  uint8_t m_customCommandCount = 0;
  void* _handlerContext = nullptr;
};