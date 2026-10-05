#pragma once
#include <Arduino.h>
#include <stdint.h>

class Chrono
{
private:
    uint32_t m_Start_us = 0;
    uint32_t& m_save;
    const char* m_Name = nullptr;
    const bool makeAverage = false;
public:
    Chrono(const char* benchName);
    Chrono(const char* benchName, uint32_t& save, bool avg);
    ~Chrono();
};