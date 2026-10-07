#pragma once
#include <WiFi.h>

#include <superv/defines.hpp>
#include <superv/protocol.hpp>

class Animator;

using AnimatorSeat = Animator*;

enum class PacketReceptionStatus
{
    WAITING,
    SIGNATURE,
    HEADER,
    DATA,
    PROCESSED,
    TIMEDOUT
};

class Animator : protected WiFiClient
{
protected:
    static uint8_t m_AnimatorCount;
    char m_Name[STATIC_BUFFER_SIZE] = { 0 };

    Protocol::PacketHeader m_CurrentPacket;
    PacketReceptionStatus m_CurrentPacketStatus = PacketReceptionStatus::WAITING;
    uint32_t m_PacketBytesLeft = 0;
    int m_LastAvail = 0;       // bytes still in the RX buffer after the previous poll
    uint32_t m_LastReceptionMS = 0;
    uint32_t m_LastHeartbeatMS = 0;
    uint32_t m_LastAliveResponseMS = 0;
    char m_DataBuffer[STATIC_BUFFER_SIZE] = { 0 };

    void receiveCommand(int& avail);
    bool createCustom(Protocol::CustomCommand& command);
    virtual void handleCommand(Protocol::Command command, uint32_t dataLength = 0);

    Protocol::CustomCommand m_customCommands[DEFINE_CLIENT_CUSTOM_MAX];
    uint8_t m_registeredCustomCommandMask = 0;
    bool m_synced = false;

public:
    Animator() = default;
    Animator(const WiFiClient& client);
    ~Animator() = default;

    virtual void loop();
    void poll();
    bool alive();
    bool heartbeat();
    void kill(AnimatorSeat& occupiedSeat);
    inline uint8_t getCount() { return m_AnimatorCount; };

    bool sendCommand(Protocol::Command command, const void* payload = nullptr, size_t length = 0);

    const char* getName() const;

    void setName(const char* nameString);

    bool addCustom(char* commandString, int len);
    uint8_t nameToCustomCommandId(const char * commandName);
    Protocol::CustomCommand getCustom(uint8_t index);
    bool isCustomCommandSet(uint8_t index);
};
