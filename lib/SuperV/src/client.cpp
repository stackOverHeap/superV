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
  connect(m_masterAdress, m_masterPort);
}

void RemoteCommandClient::loop()
{
  // checks if connected to wifi

  auto now = millis();

  if (now % DEFINE_CONNECT_POLL_RATE == 0 && WiFi.status() != WL_CONNECTED)
  {
    m_synced = false;
    LOGW("Could not connect to supervisor network.");
    return;
  }

  if ((now + 250) % DEFINE_CONNECT_POLL_RATE == 0 && !connected())
  {
    m_synced = false;
    connect(m_masterAdress, m_masterPort);
    return;
  }

  if (!m_synced)
  {
    for (uint8_t i = 0; i < DEFINE_CLIENT_CUSTOM_MAX; i++)
    {
      if (bitRead(m_registeredCustomCommandMask, i))
        sendCommand(Command::CUSTOM, &m_customCommands[i], sizeof(CustomCommand::commandID) + strlen(m_customCommands[i].commandName));
    }
    m_synced = true;
  }

  poll();
}

void RemoteCommandClient::handleCommand(Command command, uint32_t dataLength)
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
    if (dataLength < sizeof(CustomCommand::commandID) || m_handlers.onCustom == nullptr)
      break;
    m_handlers.onCustom(reinterpret_cast<CustomCommand*>(m_DataBuffer)->commandID);
    break;

  default:
    break;
  }
}

void RemoteCommandClient::setCommandHandlers(const ClientCommandHandlers& handlers)
{
  m_handlers = handlers;
}

void RemoteCommandClient::registerCustomCommands(const char* commandName, uint8_t commandID)
{
  if (commandID >= DEFINE_CLIENT_CUSTOM_MAX)
    return;

  CustomCommand custom;
  strncpy(custom.commandName, commandName, sizeof(CustomCommand::commandName) - 1);
  custom.commandName[sizeof(custom.commandName) - 1] = '\0';
  custom.commandID = commandID;

  if (createCustom(custom))
    m_synced = false;

  return;
}

void RemoteCommandClient::setRealTimeNeed(bool realTimeNeed)
{
  m_realTime = realTimeNeed;
}

void RemoteCommandClient::setIdentity(const char* deviceId)
{
  setName(deviceId);
}

void RemoteCommandClient::startSystem()
{
  if (m_handlers.onStart)
    m_handlers.onStart();
}

void RemoteCommandClient::stopSystem()
{
  if (m_handlers.onStop)
    m_handlers.onStop();
}

void RemoteCommandClient::pauseSystem()
{
  if (m_handlers.onPause)
    m_handlers.onPause();
}

void RemoteCommandClient::resetSystem()
{
  if (m_handlers.onReset)
    m_handlers.onReset();
}