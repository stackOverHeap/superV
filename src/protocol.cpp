#include "protocol.hpp"

#include <WiFi.h>
#include <string.h>

namespace
{
bool writePacket(WiFiClient& client, MasterCommand command, const uint8_t* data, uint32_t length)
{
    const uint8_t header[] = {
        PROTOCOL_SIGNATURE,
        PROTOCOL_SIGNATURE >> 8,
        static_cast<uint8_t>(command),
        static_cast<uint8_t>(length),
        static_cast<uint8_t>(length >> 8),
        static_cast<uint8_t>(length >> 16),
        static_cast<uint8_t>(length >> 24),
    };

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
