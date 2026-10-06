#include <superv/master.hpp>
#include <WiFi.h>
#include <SimpleCLI.h>

#include <superv/logging.hpp>
#include <superv/protocol.hpp>
#include <stdio.h>

static SimpleCLI cli;

static void addPeerCommand(const char* name, const char* description)
{
    Command command = cli.addCommand(name);
    command.addPositionalArgument("peer");
    command.setDescription(description);
}

template <size_t SIZE>
static bool emplaceFree(AnimatorSeat(&seats)[SIZE], Animator(&locations)[SIZE], WiFiClient& client)
{
    for (int i = 0; i < SIZE; i++)
    {
        if (seats[i] == nullptr) // free seat, claim it
        {
            locations[i] = Animator(client); // copy to location
            char name[STATIC_BUFFER_SIZE];
            snprintf(name, sizeof(name), "anim%d", i + 1);
            locations[i].setName(name);
            seats[i] = &locations[i];
            LOGI("New animator connected as %s (%u/%u)",
                 locations[i].getName(), locations[i].getCount(),
                 DEFINE_MAX_CLIENT);
            return true;
        }
    }

    return false;
}

bool Master::getPeerName(uint8_t peerIndex, char* name, size_t nameSize) const
{
    if (name == nullptr || nameSize == 0)
        return false;

    name[0] = '\0';
    if (peerIndex >= DEFINE_MAX_CLIENT || m_Seats[peerIndex] == nullptr ||
        !m_Seats[peerIndex]->alive())
        return false;

    snprintf(name, nameSize, "%s", m_Seats[peerIndex]->getName());
    return true;
}

void Master::init()
{
    WiFi.beginAP("supervisor-net");
    m_Server.begin(m_ServerPort);
    LOGI("Started server on port %u", m_ServerPort);

    addPeerCommand("ident", "Request the peer identity.");
    addPeerCommand("status", "Request the peer status.");
    addPeerCommand("alive", "Check whether the peer is alive.");
    addPeerCommand("custom", "Send a custom-command request to the peer.");
    addPeerCommand("start", "Start the peer.");
    addPeerCommand("stop", "Stop the peer.");
    addPeerCommand("pause", "Pause the peer.");
    addPeerCommand("reset", "Reset the peer.");
    cli.addCommand("peers").setDescription("List connected peers.");
    cli.addCommand("help").setDescription("Show available commands.");
    m_Interface.begin();
}

Master& Master::getInstance()
{
    static Master instance;
    return instance;
}

bool Master::sendCommandToPeer(uint8_t peerIndex, Protocol::MasterCommand command)
{
    if (peerIndex >= DEFINE_MAX_CLIENT || m_Seats[peerIndex] == nullptr)
        return false;

    Animator& animator = *m_Seats[peerIndex];
    return animator.alive() && animator.sendCommand(command);
}

void Master::loop()
{
    m_Interface.loop(*this);

    WiFiClient client = m_Server.accept();

    if (client)
    {
        if (!emplaceFree(m_Seats, m_Animators, client))
        {
            client.println("ERROR : Too many client connected.");
            client.stop();
            LOGW("Max clients number reached.");
        }
    }

    for (AnimatorSeat& seat : m_Seats)
    {

        if (seat == nullptr) // seat is empty, skip it
            continue;

        Animator& animator = *seat;

        animator.poll();

        if (!animator.alive() || !animator.heartbeat())
        {
            animator.kill(seat);
            continue;
        }

    }

    if (Serial.available())
    {
        String input = Serial.readStringUntil('\n');
        if (input.length() > 0)
            cli.parse(input);
    }

    while (cli.available())
    {
        Command command = cli.getCommand();
        String commandName = command.getName();
        commandName.toLowerCase();

        if (commandName == "help")
        {
            Serial.println(cli.toString());
            continue;
        }

        if (commandName == "peers")
        {
            bool foundPeer = false;
            for (AnimatorSeat seat : m_Seats)
            {
                if (seat == nullptr || !seat->alive())
                    continue;

                Serial.println(seat->getName());
                foundPeer = true;
            }

            if (!foundPeer)
                Serial.println("No connected peers.");
            continue;
        }

        const String peerName = command.getArgument("peer").getValue();
        Animator* peer = nullptr;
        for (AnimatorSeat seat : m_Seats)
        {
            if (seat != nullptr && seat->alive() && peerName == seat->getName())
            {
                peer = seat;
                break;
            }
        }

        if (peer == nullptr)
        {
            Serial.print("No connected peer named \"");
            Serial.print(peerName);
            Serial.println("\".");
            continue;
        }

        Protocol::MasterCommand protocolCommand;
        if (commandName == "ident")
            protocolCommand = Protocol::MasterCommand::IDENT;
        else if (commandName == "status")
            protocolCommand = Protocol::MasterCommand::STATUS;
        else if (commandName == "alive")
            protocolCommand = Protocol::MasterCommand::ALIVE;
        else if (commandName == "custom")
            protocolCommand = Protocol::MasterCommand::CUSTOM;
        else if (commandName == "start")
            protocolCommand = Protocol::MasterCommand::START;
        else if (commandName == "stop")
            protocolCommand = Protocol::MasterCommand::STOP;
        else if (commandName == "pause")
            protocolCommand = Protocol::MasterCommand::PAUSE;
        else if (commandName == "reset")
            protocolCommand = Protocol::MasterCommand::RESET;
        else
        {
            Serial.print("Unknown command: ");
            Serial.println(commandName);
            continue;
        }

        if (peer->sendCommand(protocolCommand))
        {
            Serial.print("Sent ");
            Serial.print(commandName);
            Serial.print(" to ");
            Serial.println(peerName);
        }
        else
        {
            Serial.print("Failed to send ");
            Serial.print(commandName);
            Serial.print(" to ");
            Serial.println(peerName);
        }
    }

    while (cli.errored())
    {
        CommandError error = cli.getError();
        Serial.print("CLI error: ");
        Serial.println(error.toString());
    }
}
