#include "animator.hpp"
#include "logging.hpp"
#include "protocol.hpp"

uint8_t Animator::m_AnimatorCount = 0;

static void discardBytes(WiFiClient& client, uint32_t length)
{
    uint8_t buffer[32];

    while (length > 0)
    {
        const size_t chunkLength = length < sizeof(buffer) ? length : sizeof(buffer);
        client.readBytes(buffer, chunkLength);
        length -= chunkLength;
    }
}


Animator::Animator(const WiFiClient& client) : WiFiClient(client)
{
    m_AnimatorCount++;
    sprintf(m_Name, "Unauthentified%u", m_AnimatorCount);
    LOGI("New animator connected as %s (%u/%u)", m_Name, m_AnimatorCount, DEFINE_MAX_CLIENT);
    sendCommand(MasterCommand::IDENT);
}

void Animator::poll()
{
    if (m_AwaitingData == 0)
    {
        if (available() < static_cast<int>(sizeof(PacketHeader)))
            return;

        readBytes(reinterpret_cast<uint8_t*>(&m_CurrentPacket), sizeof(m_CurrentPacket));

        if (m_CurrentPacket.signature != PROTOCOL_SIGNATURE)
            return;

        m_AwaitingData = m_CurrentPacket.followingLength;
    }

    if (m_AwaitingData == 0)
        return;

    if (available() < static_cast<int>(m_AwaitingData))
        return;

    if (m_CurrentPacket.clientCommandID == ClientCommand::IDENT)
    {
        const size_t nameLength = m_AwaitingData < sizeof(m_Name) - 1
            ? m_AwaitingData
            : sizeof(m_Name) - 1;
        const size_t bytesRead = readBytes(m_Name, nameLength);
        m_Name[bytesRead] = '\0';
        discardBytes(*this, m_AwaitingData - bytesRead);
        LOGI("Authenticated as %s", m_Name);
    }
    else
    {
        discardBytes(*this, m_AwaitingData);
    }

    m_AwaitingData = 0;
    m_CurrentPacket = {};
}

bool Animator::alive()
{
    return connected() || available() > 0;
}

void Animator::kill(AnimatorSeat& occupiedSeat)
{
    stop();
    occupiedSeat = nullptr;
    m_AnimatorCount--;
}

bool Animator::sendCommand(MasterCommand command)
{
    if (!Protocol::sendCommand(*this, command))
    {
        LOGE("Failed to send command. Disconnecting client.");
        stop();
        flush();
        return false;
    }
    return true;
}

char* Animator::getName()
{
    return m_Name;
}

void Animator::setName(char* nameString)
{
    strncpy(m_Name, nameString, sizeof(m_Name));
}
