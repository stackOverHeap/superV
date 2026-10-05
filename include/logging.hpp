#pragma once

#include <Arduino.h>

#ifdef ENV_UNO
#include <LibPrintf.h>
#else
#include <Print.h>
#endif

#ifdef LOGGING

#ifndef NO_COLOR
#define LOG_COLOR_RESET "\033[0m"
#define LOG_COLOR_RED "\033[31m"
#define LOG_COLOR_YELLOW "\033[33m"
#define LOG_COLOR_GREEN "\033[32m"
#else
#define LOG_COLOR_RESET ""
#define LOG_COLOR_RED ""
#define LOG_COLOR_YELLOW ""
#define LOG_COLOR_GREEN ""
#endif

#define LOG_BASE(level, color, msg, ...) \
    printf("[%lu][" __FILE__ "] " color level LOG_COLOR_RESET " : " msg "\n", \
           (unsigned long)millis(), ##__VA_ARGS__)

#define LOGE(msg, ...) LOG_BASE("ERROR", LOG_COLOR_RED, msg, ##__VA_ARGS__)
#define LOGW(msg, ...) LOG_BASE("WARNING", LOG_COLOR_YELLOW, msg, ##__VA_ARGS__)
#define LOGI(msg, ...) LOG_BASE("INFO", LOG_COLOR_GREEN, msg, ##__VA_ARGS__)

#else

#define LOGE(msg, ...) do {} while (false)
#define LOGW(msg, ...) do {} while (false)
#define LOGI(msg, ...) do {} while (false)

#endif