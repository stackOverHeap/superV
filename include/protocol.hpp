#pragma once
#include <stdint.h>
#include <WiFi.h>

#ifdef LOGGING
extern char* COMMANDS[];
#endif

#define PROTOCOL_SIGNATURE 0xfaaa

namespace Protocol
{

    enum class MasterCommand : uint8_t
    {
        INVALID,
        IDENT,
        STATUS,
        ALIVE,
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
        ALIVE,
        CUSTOM // creates a new custom command available to the user
    };

    enum class ClientState : uint8_t
    {
        RUNNING,
        PAUSED,
        WAITING
    };

    using PacketSignature = uint16_t;

#pragma pack(push,1)
    struct PacketHeader
    {
        union
        {
            MasterCommand masterCommandID = MasterCommand::INVALID;
            ClientCommand clientCommandID;
        };

        operator uint8_t* () { return reinterpret_cast<uint8_t*>(this); }

        uint32_t followingLength = 0;
    };
#pragma pack(pop)

    bool sendCommand(WiFiClient& client, MasterCommand command);
    bool sendCommand(WiFiClient& client, MasterCommand command, const char* data);
    bool sendCommand(WiFiClient& client, MasterCommand command, const uint8_t* data, uint32_t length);
} // namespace Protocol

#define PROTOCOL_PACKET_SIZE_LIMIT sizeof(Protocol::PacketSignature) + sizeof(Protocol::PacketHeader) + 512
