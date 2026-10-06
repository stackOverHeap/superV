#pragma once
#include <WiFi.h>

#include <superv/defines.hpp>
#include <superv/animator.hpp>
#include <superv/interface.hpp>

class Master
{
    WiFiServer m_Server;
    const int m_ServerPort;

    Animator m_Animators[DEFINE_MAX_CLIENT];
    AnimatorSeat m_Seats[DEFINE_MAX_CLIENT] = {nullptr};
    Interface m_Interface;
    
    uint8_t m_ConnectedClient = 0;

    Master() : m_ServerPort(DEFINE_SERVER_PORT) {};
    Master(int port) : m_ServerPort(port) {};

public:
    static Master &getInstance();
    void loop();
    void init();
    bool sendCommandToPeer(uint8_t peerIndex, Protocol::MasterCommand command);
    bool getPeerName(uint8_t peerIndex, char* name, size_t nameSize) const;

};