#include <superv/animator.hpp>
#include <superv/logging.hpp>
#include <superv/protocol.hpp>
#include <string.h>

using namespace Protocol;

uint8_t Animator::m_AnimatorCount = 0;


Animator::Animator(const WiFiClient& client) : WiFiClient(client)
{
    m_LastHeartbeatMS = millis();
    m_LastAliveResponseMS = m_LastHeartbeatMS;
    m_AnimatorCount++;
    sprintf(m_Name, "Unauthentified%u", m_AnimatorCount);
    LOGI("New animator connected as %s (%u/%u)", m_Name, m_AnimatorCount, DEFINE_MAX_CLIENT);
    sendCommand(Command::IDENT);
}

void Animator::loop()
{
}

void Animator::poll()
{
    const uint32_t now = millis();
    int avail = available();

    if (avail > m_LastAvail)
        m_LastReceptionMS = now;

    // Mid-packet, or leftover bytes while WAITING (= a partial signature,
    // since garbage is flushed every poll). Idle + empty never times out.
    // DATA is excluded: it is a complete packet processed on the next call.
    const bool midPacket = (m_CurrentPacketStatus != PacketReceptionStatus::WAITING)
        && (m_CurrentPacketStatus != PacketReceptionStatus::DATA);
    const bool incomplete = midPacket || (avail > 0);

    const auto sinceLastReception = now - m_LastReceptionMS;
    if (incomplete && sinceLastReception > DEFINE_RX_TIMEOUT_MS)
    {
        LOGW("Packet reception from %s timed out, dropping partial data.", m_Name);

        uint8_t trash[32];
        while (avail > 0)                          // drain with few modem calls
        {
            const size_t n = readBytes(trash, min(static_cast<size_t>(avail), sizeof(trash)));
            if (n == 0)
                break;
            avail -= n;
        }

        m_CurrentPacketStatus = PacketReceptionStatus::WAITING;
        m_PacketBytesLeft = sizeof(PacketSignature);
        m_LastAvail = 0;
        return;
    }

    receiveCommand(avail);
    m_LastAvail = avail;                           // must be what is really left
}

void Animator::receiveCommand(int& avail)
{
    if (m_CurrentPacketStatus == PacketReceptionStatus::WAITING)
    {
        while (avail && peek() != (PROTOCOL_SIGNATURE & 0xFF))
        {
            read();
            avail--;
        } // flush garbage

        if (avail < 2)
            return; // idle, or lone signature byte -> the timeout in poll() handles it

        read(); // take the first known good byte of the signature
        avail--;

        if (peek() == ((PROTOCOL_SIGNATURE >> 8) & 0xFF))
        {
            read(); // consume the second byte
            avail--;
            m_CurrentPacketStatus = PacketReceptionStatus::SIGNATURE;
            m_PacketBytesLeft = sizeof(PacketHeader);
        }
    }

    if (avail < m_PacketBytesLeft) // not enough bytes in the RX buffer to satisfy what is wanted
        return;

    if (m_CurrentPacketStatus == PacketReceptionStatus::SIGNATURE)
    {
        readBytes(reinterpret_cast<uint8_t*>(&m_CurrentPacket), sizeof(PacketHeader));
        avail -= sizeof(PacketHeader);

        if (m_CurrentPacket.followingLength > 0)
        {
            if (m_CurrentPacket.followingLength > sizeof(m_DataBuffer)) // skip if size does not fit buffer
            {
                m_CurrentPacketStatus = PacketReceptionStatus::WAITING;
                m_PacketBytesLeft = sizeof(PacketSignature);
            }
            else
            {
                m_CurrentPacketStatus = PacketReceptionStatus::HEADER;
                m_PacketBytesLeft = m_CurrentPacket.followingLength;
            }
        }
        else
        {
            m_CurrentPacketStatus = PacketReceptionStatus::DATA;
            m_PacketBytesLeft = 0;
        }
    }

    if (avail < m_PacketBytesLeft) // not enough bytes in the RX buffer to satisfy what is wanted
        return;

    if (m_CurrentPacketStatus == PacketReceptionStatus::HEADER)
    {
        memset(m_DataBuffer, 0, sizeof(m_DataBuffer));
        readBytes(reinterpret_cast<uint8_t*>(&m_DataBuffer), m_CurrentPacket.followingLength);
        avail -= m_CurrentPacket.followingLength;

        m_CurrentPacketStatus = PacketReceptionStatus::DATA;
        m_PacketBytesLeft = 0;
    }

    if (avail < m_PacketBytesLeft) // not enough bytes in the RX buffer to satisfy what is wanted
        return;

    if (m_CurrentPacketStatus == PacketReceptionStatus::DATA)
    {
        LOGI("Received command %s from %s", COMMANDS[static_cast<uint8_t>(m_CurrentPacket.command)], m_Name);

        handleCommand(m_CurrentPacket.command);
        m_CurrentPacketStatus = PacketReceptionStatus::WAITING;
        m_PacketBytesLeft = sizeof(PacketSignature);
    }
}

bool Animator::alive()
{
    return connected();
}

bool Animator::heartbeat()
{
    const uint32_t now = millis();
    if (now - m_LastAliveResponseMS >= DEFINE_HEARTBEAT_TIMEOUT_MS)
    {
        LOGW("Animator %s missed its heartbeat timeout.", m_Name);
        return false;
    }

    if (now - m_LastHeartbeatMS >= DEFINE_HEARTBEAT_INTERVAL_MS)
    {
        m_LastHeartbeatMS = now;
        return sendCommand(Command::ALIVE);
    }

    return true;
}

void Animator::kill(AnimatorSeat& occupiedSeat)
{
    m_AnimatorCount--;
    LOGI("Animator %s disconnected. (%u/%u)", m_Name, m_AnimatorCount, DEFINE_MAX_CLIENT);
    stop();
    occupiedSeat = nullptr;
}

bool Animator::sendCommand(Protocol::Command command, const void* payload, size_t length)
{
    PacketSignature signature = PROTOCOL_SIGNATURE;

    PacketHeader header;
    header.command = command;
    header.followingLength = length;

    uint32_t sent = 0;
    sent += write(reinterpret_cast<uint8_t*>(&signature), sizeof(PacketSignature));
    sent += write(header, sizeof(PacketHeader));
    sent += write(reinterpret_cast<const uint8_t*>(payload), length);

    LOGI("Sent %u, payload %p, length %u, sent %u", command, payload, length, sent);

    return sent == sizeof(PacketSignature) + sizeof(PacketHeader) + length;
}

bool Animator::createCustom(Protocol::CustomCommand& command)
{

    if (command.commandID >= DEFINE_CLIENT_CUSTOM_MAX)
        return false;

    m_customCommandCount = command.commandID + 1;
    m_customCommands[command.commandID] = command;
    return true;
}

void Animator::handleCommand(Command command)
{
    switch (command)
    {
    case Command::IDENT:
        if (m_CurrentPacket.followingLength == 0)
            break;
        memset(m_Name, 0, sizeof(m_Name));
        strncpy(m_Name, m_DataBuffer, sizeof(m_Name) - 1);
        break;

    case Command::ALIVE:
        m_LastAliveResponseMS = millis();
        break;

    case Command::CUSTOM:
    {
        Protocol::CustomCommand command;
        memcpy(&command, m_DataBuffer, min(sizeof(m_DataBuffer), sizeof(Protocol::CustomCommand)));
        createCustom(command);
    }
    break;
    default:
        break;
    }
}

const char* Animator::getName() const
{
    return m_Name;
}

void Animator::setName(const char* nameString)
{
    strncpy(m_Name, nameString, sizeof(m_Name) - 1);
    m_Name[sizeof(m_Name) - 1] = '\0';
}

uint8_t Animator::getCustomCount()
{
    return m_customCommandCount;
}

Protocol::CustomCommand Animator::getCustom(uint8_t index)
{
    if (index < DEFINE_CLIENT_CUSTOM_MAX)
    {
        return m_customCommands[index];
    }

    return CustomCommand();
}
