#include <superv/protocol.hpp>

#include <WiFi.h>
#include <string.h>

#ifdef LOGGING
char* COMMANDS[256] =
{
    "INVALID",
    "IDENT",
    "STATUS",
    "ALIVE",
    "CUSTOM",
    "START",
    "STOP",
    "PAUSED",
    "RESET"
};
#endif

namespace Protocol
{
bool writePacket(WiFiClient& client, Protocol::MasterCommand command, const uint8_t* data, uint32_t length)
{
    const Protocol::PacketSignature sig = PROTOCOL_SIGNATURE;

    const uint8_t header[] = {
        static_cast<uint8_t>(command),
        static_cast<uint8_t>(length),
        static_cast<uint8_t>(length >> 8),
        static_cast<uint8_t>(length >> 16),
        static_cast<uint8_t>(length >> 24),
    };

    if (client.write(reinterpret_cast<const uint8_t*>(&sig), sizeof(PacketSignature)) != sizeof(PacketSignature))
        return false;
    
    if (client.write(header, sizeof(header)) != sizeof(header))
        return false;

    if (length > 0 && client.write(data, length) != length)
        return false;

    return true;
}
} // namespace

bool Protocol::sendCommand(WiFiClient& client, MasterCommand command)
{
    return writePacket(client, command, nullptr, 0);
}

bool Protocol::sendCommand(WiFiClient& client, MasterCommand command, const char* data)
{
    if (data == nullptr)
        return false;

    const size_t length = strlen(data);
    if (length > UINT32_MAX)
        return false;

    return sendCommand(client, command, reinterpret_cast<const uint8_t*>(data), static_cast<uint32_t>(length));
}

bool Protocol::sendCommand(WiFiClient& client, MasterCommand command, const uint8_t* data, uint32_t length)
{
    if (length > 0 && data == nullptr)
        return false;

    return writePacket(client, command, data, length);
}
