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

class Animator : private WiFiClient
{
    static uint8_t m_AnimatorCount;
    char m_Name[STATIC_BUFFER_SIZE] = { 0 };

    Protocol::PacketHeader m_CurrentPacket;
    PacketReceptionStatus m_CurrentPacketStatus = PacketReceptionStatus::WAITING;
    uint32_t m_PacketBytesLeft = 0;
    int      m_LastAvail = 0;       // bytes still in the RX buffer after the previous poll
    uint32_t m_LastReceptionMS = 0;
    uint32_t m_LastHeartbeatMS = 0;
    uint32_t m_LastAliveResponseMS = 0;
    char m_DataBuffer[STATIC_BUFFER_SIZE] = { 0 };

    void receive(int& avail);

    // TODO : implement the custom command list here

public:
    Animator() = default;
    Animator(const WiFiClient& client);
    ~Animator() = default;

    void poll();
    bool alive();
    bool heartbeat();
    void kill(AnimatorSeat& occupiedSeat);
    inline uint8_t getCount() { return m_AnimatorCount; };

    bool sendCommand(Protocol::MasterCommand command);
    char* getName();
    bool isSameConnection(const WiFiClient& client);

    void setName(char* nameString);
    bool addCustom(char* commandString, int len);
};
