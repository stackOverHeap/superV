// RemoteCommandClient.h
#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include "protocol.hpp"

struct ClientInfo {
  String deviceId;
  String firmwareVersion;
  String ipAddress;
  String macAddress;
  bool running;
  unsigned long uptimeMs;
};

class RemoteCommandClient {
public:
  void begin(const char* ssid, const char* password, uint16_t port = 5000);
  void loop();

  void startSystem();
  void stopSystem();
  ClientInfo getClientInfo();
  bool isSystemRunning() const;

private:
  WiFiServer _server;
  ClientState _state = ClientState::WAITING;

  String _deviceId = "Arduino_01";
  String _firmwareVersion = "1.0.0";
};