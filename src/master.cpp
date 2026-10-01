#include "master.hpp"
#include <WiFi.h>
#include <Vector.h>

#include "logging.hpp"
#include "protocol.hpp"

template <size_t SIZE>
static bool emplaceFree(AnimatorSeat (&seats)[SIZE], Animator (&locations)[SIZE], WiFiClient& client)
{
    for (int i = 0; i < SIZE; i++)
    {
        if (seats[i] == nullptr) // free seat, claim it
        {
            locations[i] = Animator(client); // copy to location
            seats[i] = &locations[i];
            return true;
        }
    }

    return false;
}

void Master::init()
{
    WiFi.config(arduino::IPAddress(192, 168, 10, 1));
    WiFi.beginAP("supervisor-net");
    m_Server.begin(m_ServerPort);
    LOGI("Started server on port %u", m_ServerPort);
}

Master &Master::getInstance()
{
    static Master instance;
    return instance;
}

void Master::loop()
{
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

        Animator &animator = *seat;

        animator.poll();

        if (!animator.alive()) // free seat if animator is not responding
        {
            LOGI("Animator %s diconnected. (%u/%u)", animator.getName(), m_ConnectedClient, DEFINE_MAX_CLIENT);
            animator.kill(seat);
            continue;
        }
        
    }

}
