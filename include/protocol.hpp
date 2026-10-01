#pragma once
#include <stdint.h>
#include <WiFi.h>

#define PROTOCOL_SIGNATURE 0xfaaa

enum class MasterCommand : uint8_t
{
    INVALID,
    IDENT,
    STATUS,
    CUSTOM, // call a client-created custom command

    START,
    STOP,
    PAUSE,
    RESET
};

enum class ClientCommand : uint8_t
{
    INVALID,
    IDENT,
    STATUS,
    CUSTOM // creates a new custom command available to the user
};

enum class ClientState : uint8_t
{
    RUNNING,
    PAUSED,
    WAITING
};

#pragma pack(push,1)
struct PacketHeader
{
    uint16_t signature = 0;
    union
    {
        MasterCommand masterCommandID = MasterCommand::INVALID;
        ClientCommand clientCommandID;
    };

    operator uint8_t*() { return reinterpret_cast<uint8_t *>(this); }

    uint32_t followingLength = 0;
};
#pragma pack(pop)

namespace Protocol
{
    bool sendCommand(WiFiClient& client, MasterCommand command);
    bool sendCommand(WiFiClient& client, MasterCommand command, const char* data);
    bool sendCommand(WiFiClient& client, MasterCommand command, const uint8_t* data, uint32_t length);
} // namespace Protocol
