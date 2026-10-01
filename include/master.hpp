#pragma once
#include <WiFi.h>
#include <Vector.h>

#include "defines.hpp"
#include "animator.hpp"

class Master
{
    WiFiServer m_Server;
    const int m_ServerPort;

    Animator m_Animators[DEFINE_MAX_CLIENT];
    AnimatorSeat m_Seats[DEFINE_MAX_CLIENT] = {nullptr};
    
    uint8_t m_ConnectedClient = 0;

    Master() : m_ServerPort(DEFINE_SERVER_PORT) {};
    Master(int port) : m_ServerPort(port) {};

public:
    static Master &getInstance();
    void loop();
    void init();

};