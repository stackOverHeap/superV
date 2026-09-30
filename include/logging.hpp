#pragma once

#include <Arduino.h>

#ifdef ENV_UNO
#include <LibPrintf.h>
#else
#include <Print.h>
#endif

#ifdef LOGGING
#define LOGE(msg, ...) \
    printf("[%u]" "[" __FILE__ "]" " ERROR : " msg  "\n", millis(), ##__VA_ARGS__)

#define LOGW(msg, ...) \
    printf("[%u]" "[" __FILE__ "]" " WARNING : " msg "\n", millis(), ##__VA_ARGS__)

#define LOGI(msg, ...) \
    printf("[%u]" "[" __FILE__ "]" " INFO : " msg "\n", millis(), ##__VA_ARGS__)

#else

#define LOGE(msg, ...) do {} while (false)
#define LOGW(msg, ...) do {} while (false)
#define LOGI(msg, ...) do {} while (false)

#endif