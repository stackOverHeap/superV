#include <superv/protocol.hpp>

#include <WiFi.h>
#include <string.h>
#include <superv/logging.hpp>

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