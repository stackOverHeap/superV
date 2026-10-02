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

class RemoteCommandClient {
public:
  void begin(
      const char* ssid,
      const char* password,
      uint16_t port = DEFINE_SERVER_PORT);

  void loop();

  void startSystem();
  void stopSystem();
  ClientInfo getClientInfo();
  bool isSystemRunning() const;

private:
  void connectToMaster();

  WiFiClient _client;
  IPAddress _masterAddress = IPAddress(192, 168, 10, 1);
  uint16_t _serverPort = DEFINE_SERVER_PORT;
  unsigned long _lastConnectAttemptMs = 0;
  bool _hasAttemptedConnection = false;

  ClientState _state = ClientState::WAITING;
  String _deviceId = "Arduino_01";
  String _firmwareVersion = "1.0.0";
};