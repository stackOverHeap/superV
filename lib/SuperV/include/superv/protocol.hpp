#pragma once
#include <stdint.h>
#include <WiFi.h>
#include <superv/defines.hpp>

#ifdef LOGGING
extern char* COMMANDS[];
#endif

#define PROTOCOL_SIGNATURE 0xfaaa

namespace Protocol
{

    enum class Command : uint8_t
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

    using PacketSignature = uint16_t;

#pragma pack(push,1)
    struct PacketHeader
    {
        Command command;

        operator uint8_t* () { return reinterpret_cast<uint8_t*>(this); }

        uint32_t followingLength = 0;
    };

    struct CustomCommand
    {
        uint8_t commandID = 0;
        char commandName[STATIC_BUFFER_SIZE - 1] = {0};
    };
    
#pragma pack(pop)
} // namespace Protocol