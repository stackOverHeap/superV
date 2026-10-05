// RemoteCommandClient.h
#pragma once

#include <Arduino.h>
#include <WiFi.h>

#include "defines.hpp"
#include "protocol.hpp"

struct ClientInfo {
  String deviceId;
  String firmwareVersion;
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
  void begin(
      const char* ssid,
      const char* password,
      uint16_t port = DEFINE_SERVER_PORT);

  void loop();

  void setCommandHandlers(
      const ClientCommandHandlers& handlers,
      void* context = nullptr);
  void setIdentity(const char* deviceId, const char* firmwareVersion);

  void startSystem();
  void stopSystem();
  void pauseSystem();
  void resetSystem();
  ClientInfo getClientInfo();
  bool isSystemRunning() const;

private:
  void connectToMaster();

  WiFiClient _client;
  IPAddress _masterAddress = IPAddress(192, 168, 10, 1);
  uint16_t _serverPort = DEFINE_SERVER_PORT;
  unsigned long _lastConnectAttemptMs = 0;
  bool _hasAttemptedConnection = false;

  Protocol::ClientState _state = Protocol::ClientState::WAITING;
  String _deviceId = "Arduino_01";
  String _firmwareVersion = "1.0.0";
  ClientCommandHandlers _handlers;
  void* _handlerContext = nullptr;
};