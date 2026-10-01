#include <Arduino.h>
#include "master.hpp"

Master master = Master::getInstance();

void setup()
{
  Serial.begin(9600);
  master.init();
}

void loop() {
  master.loop();
}
