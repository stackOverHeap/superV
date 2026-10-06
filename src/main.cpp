#include <Arduino.h>
#include <superv/logging.hpp>
#include "benchmark.hpp"

#if defined(MASTER_MODE) && defined(CLIENT_MODE)
#error "Select exactly one build mode: MASTER_MODE or CLIENT_MODE"
#elif defined(MASTER_MODE)
#include <superv/master.hpp>
#include <interface.hpp>

Master& master = Master::getInstance();
Interface interface;

void setup() {
  Serial.begin(9600);
  LOGI("Starting SuperV in master mode");
  master.init();
  interface.begin();
}

void loop() {
  interface.loop(master);
  master.loop();
}

#elif defined(CLIENT_MODE)
#include <superv/client.hpp>

namespace {
  constexpr char WIFI_SSID[] = "supervisor-net";

  enum class DemoState : uint8_t {
    WAITING,
    RUNNING,
    PAUSED
  };

  RemoteCommandClient client(true);
  DemoState demoState = DemoState::WAITING;
  unsigned long lastLedToggleMs = 0;
  bool ledOn = false;
  uint32_t resetCount = 0;

  void onStart(void*) {
    demoState = DemoState::RUNNING;
    LOGI("[APP] START applique: le programme de demonstration tourne.");
  }

  void onStop(void*) {
    demoState = DemoState::WAITING;
    LOGI("[APP] STOP applique: le programme de demonstration est arrete.");
  }

  void onPause(void*) {
    demoState = DemoState::PAUSED;
    LOGI("[APP] PAUSE applique: le programme de demonstration est en pause.");
  }

  void onReset(void*) {
    demoState = DemoState::WAITING;
    resetCount++;
    LOGI("[APP] RESET applique. Nombre de resets: %lu", resetCount);
  }

  void onCustom(void*, const char* payload, size_t length) {
    LOGI("[APP] CUSTOM recu: %.*s", static_cast<int>(length), payload);
  }

  void updateDemoLed() {
    if (demoState == DemoState::WAITING) {
      ledOn = false;
      digitalWrite(LED_BUILTIN, LOW);
      return;
    }

    const unsigned long intervalMs =
      demoState == DemoState::RUNNING ? 250 : 1000;
    const unsigned long now = millis();

    if (now - lastLedToggleMs >= intervalMs) {
      lastLedToggleMs = now;
      ledOn = !ledOn;
      digitalWrite(LED_BUILTIN, ledOn ? HIGH : LOW);
    }
  }
}  // namespace

void setup() {
  auto bench = Chrono("setup()");
  Serial.begin(9600);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  ClientCommandHandlers handlers;
  handlers.onStart = onStart;
  handlers.onStop = onStop;
  handlers.onPause = onPause;
  handlers.onReset = onReset;
  handlers.onCustom = onCustom;

  client.setIdentity("s2");
  client.setCommandHandlers(handlers);

  LOGI("Demonstration de l'interface client SuperV");
  LOGI("Connexion au point d'acces du superviseur...");
  client.setup();
}


void loop() {
  uint32_t time = 0;
  int cycles = 0;

  while (cycles < 100)
  {
    auto bench = Chrono("loop()", time, true);
    client.loop();
    updateDemoLed();
    cycles++;
  }
  LOGI("avg loop time is %u us on 100 cycles", time);
  cycles = 0;

}

#else
#error "Define MASTER_MODE or CLIENT_MODE in the PlatformIO environment"
#endif
