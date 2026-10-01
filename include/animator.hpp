#pragma once
#include <WiFi.h>

#include "defines.hpp"
#include "protocol.hpp"

class Animator;

using AnimatorSeat = Animator *;

class Animator : private WiFiClient
{ 
    static uint8_t m_AnimatorCount;

    PacketHeader m_CurrentPacket;
    uint32_t m_AwaitingData = 0;
    char m_Name[64] = {0};

    // TODO : implement the custom command list here

public:
    Animator() = default;
    Animator(const WiFiClient &client);
    ~Animator() = default;

    void poll();
    bool alive();
    void kill(AnimatorSeat& occupiedSeat);

    bool sendCommand(MasterCommand command);
    char *getName();

    void setName(char *nameString);
    bool addCustom(char *commandString, int len);
};
