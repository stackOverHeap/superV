// RemoteCommandClient.h
#pragma once

#include <Arduino.h>
#include <WiFi.h>

#include <superv/defines.hpp>
#include <superv/protocol.hpp>
#include <superv/animator.hpp>

struct ClientCommandHandlers {
  void (*onStart)(void) = nullptr;
  void (*onStop)(void) = nullptr;
  void (*onPause)(void) = nullptr;
  void (*onReset)(void) = nullptr;
  void (*onCustom)(void) = nullptr;
};

class RemoteCommandClient : public Animator{
public:
  RemoteCommandClient() = default;
  RemoteCommandClient(bool realTimeNeed);

  void setup();
  void loop() override;

  void handleCommand(Protocol::Command command) override;

  void setCommandHandlers(const ClientCommandHandlers& handlers);
  void setRealTimeNeed(bool realTimeNeed);

  /*legacy alias*/ void setIdentity(const char* deviceId);

  void startSystem();
  void stopSystem();
  void pauseSystem();
  void resetSystem();

  bool isSystemRunning() const;

private:
  void connectToMaster(bool connectionKnownLost = false);

  void normalLoop();
  void realTimeLoop();
  void resetReceiveState();

  const IPAddress m_masterAdress = IPAddress(192, 168, 4, 1);
  const uint16_t m_masterPort = DEFINE_SERVER_PORT;

  ClientCommandHandlers m_handlers;


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

  Protocol::ClientState _state = Protocol::ClientState::WAITING;
  String _deviceId = "Arduino_01";
  Protocol::CustomCommand m_customCommands[DEFINE_CLIENT_CUSTOM_MAX];
  uint8_t m_customCommandCount = 0;
  void* _handlerContext = nullptr;
};