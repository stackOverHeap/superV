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
  void (*onCustom)(uint8_t commandIndex) = nullptr;
};

class RemoteCommandClient : private Animator {

  void handleCommand(Protocol::Command command, uint32_t dataLength = 0) override;
  void connectToMaster(bool connectionKnownLost = false);

  void normalLoop();
  void realTimeLoop();
  void resetReceiveState();

  const IPAddress m_masterAdress = IPAddress(192, 168, 4, 1);
  const uint16_t m_masterPort = DEFINE_SERVER_PORT;

  ClientCommandHandlers m_handlers;
 
  bool m_realTime = false;

  char * _deviceId;

public:
  RemoteCommandClient() = default;
  RemoteCommandClient(bool realTimeNeed);

  void setup();
  void loop() override;


  void setCommandHandlers(const ClientCommandHandlers& handlers);
  void registerCustomCommands(const char* commandName, uint8_t commandID);
  void setRealTimeNeed(bool realTimeNeed);

  /*legacy alias*/ void setIdentity(const char* deviceId);

  void startSystem();
  void stopSystem();
  void pauseSystem();
  void resetSystem();
  
};