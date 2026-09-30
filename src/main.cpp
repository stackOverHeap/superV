#include <Arduino.h>
#include "logging.hpp"
#include "client.hpp"



RemoteCommandClient client;

void setup() {
  Serial.begin(9600);
  delay(1000); // Attendre que le port série soit prêt
  LOGI("Initialisation du client WiFi");
  client.begin("MON_RESEAU", "MOT_DE_PASSE", 5000);
}

void loop() {
  client.loop();

  if (client.isSystemRunning()) {
       LOGI("Système actif");

  } else {
     LOGW("Système inactif");
  }
}

