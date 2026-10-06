#include "benchmark.hpp"
#include <Arduino.h>

#include <superv/logging.hpp>

Chrono::Chrono(const char* benchName) : m_Name(benchName), m_save(m_Start_us)
{
    m_Start_us = micros();
}

Chrono::Chrono(const char* benchName, uint32_t& save, bool avg = false) : m_Name(benchName), m_save(save), makeAverage(avg)
{
    m_Start_us = micros();
}

Chrono::~Chrono()
{
    const auto ellapsed = micros() - m_Start_us;

    if (makeAverage)
    {
        if (m_save > 0)
        {
            m_save = (m_save + ellapsed) / 2;
        }
        else // first time only
        {
            m_save = ellapsed;
        }
    }
    else
    {
        LOGI("Benchmark %s took %u us.", m_Name, ellapsed);
        m_save = micros() - m_Start_us;
    }
}

