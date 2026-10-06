#include <superv/client.hpp>
#include <superv/logging.hpp>
#include <superv/protocol.hpp>

#include <stdio.h>

using namespace Protocol;

RemoteCommandClient::RemoteCommandClient(bool realTimeNeed) : m_realTime(realTimeNeed)
{

}

void RemoteCommandClient::setup()
{
  WiFi.begin("supervisor-net");
}

void RemoteCommandClient::loop()
{
  // checks if connected to wifi

  auto now = millis();

  if (now % DEFINE_CONNECT_POLL_RATE == 0 && WiFi.status() != WL_CONNECTED)
  {
    LOGW("Could not connect to supervisor network.");
    return;
  }

  if ((now + 250) % DEFINE_CONNECT_POLL_RATE == 0 && !connected())
  {
    connect(m_masterAdress, m_masterPort);
  }

  poll();
}

void RemoteCommandClient::handleCommand(Command command)
{
  switch (command)
  {
  case Command::IDENT:
    sendCommand(Command::IDENT, m_Name, strlen(m_Name));
    break;

  case Command::ALIVE:
    sendCommand(Command::ALIVE);
    break;

  case Command::START:
    startSystem();
    break;

  case Command::STOP:
    stopSystem();
    break;

  case Command::PAUSE:
    pauseSystem();
    break;

  case Command::RESET:
    resetSystem();
    break;

  case Command::CUSTOM:
    break;

  default:
    break;
  }
}

void RemoteCommandClient::setCommandHandlers(const ClientCommandHandlers& handlers)
{
  m_handlers = handlers;
}

void RemoteCommandClient::setRealTimeNeed(bool realTimeNeed)
{
  m_realTime = realTimeNeed;
}

void RemoteCommandClient::setIdentity(const char* deviceId) {
  setName(deviceId);
}

void RemoteCommandClient::startSystem()
{
  m_handlers.onStart();
}

void RemoteCommandClient::stopSystem() {
  m_handlers.onStop();
}

void RemoteCommandClient::pauseSystem() {
  m_handlers.onPause();
}

void RemoteCommandClient::resetSystem() {
  m_handlers.onReset();
}