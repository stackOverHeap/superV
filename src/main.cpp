#include <Arduino.h>
#include "logging.hpp"
#include "client.hpp"

RemoteCommandClient client;

bool checkLocalTrigger() {
  // Exemple de déclenchement local : un bouton ou capteur sur la broche 2.
  // Remplace cette logique par ton vrai capteur ou condition locale.
  const uint8_t localTriggerPin = 2;
  static bool triggerAlreadyHandled = false;

  pinMode(localTriggerPin, INPUT_PULLUP);

  bool triggerDetected = (digitalRead(localTriggerPin) == LOW);

  if (triggerDetected && !triggerAlreadyHandled) {
    triggerAlreadyHandled = true;
    return true;
  }

  if (!triggerDetected) {
    triggerAlreadyHandled = false;
  }

  return false;
}

void runLocalProgram() {
  // Ici tu mets le code local à exécuter lorsque le système est actif.
  static unsigned long lastLocalLog = 0;

  if (millis() - lastLocalLog >= 1000) {
    LOGI("Programme local en cours");
    lastLocalLog = millis();
  }
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  Serial.begin(9600);
  delay(1000);
  LOGI("Initialisation du client WiFi");
  client.begin("MON_RESEAU", "MOT_DE_PASSE", 5000);
}

void loop() {
  client.loop();

  // Priorité au superviseur : si une commande distante a modifié l'état,
  // le système reste dans cet état. Sinon, un déclenchement local peut lancer le programme.
  if (!client.isSystemRunning() && checkLocalTrigger()) {
    client.startSystem();
    LOGI("Démarrage local déclenché");
  }

  if (client.isSystemRunning()) {
    runLocalProgram();
    digitalWrite(LED_BUILTIN, HIGH);
  } else {
    digitalWrite(LED_BUILTIN, LOW);
    LOGW("Système inactif");
    delay(200);
  }
}

