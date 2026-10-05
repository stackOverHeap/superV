#include <Arduino.h>
#include <superv/client.hpp>

namespace {
constexpr char WIFI_SSID[] = "supervisor-net";
constexpr char WIFI_PASSWORD[] = "";

enum class DemoState : uint8_t {
  WAITING,
  RUNNING,
  PAUSED
};

RemoteCommandClient client;
DemoState demoState = DemoState::WAITING;
unsigned long lastLedToggleMs = 0;
bool ledOn = false;
uint32_t resetCount = 0;

void onStart(void*) {
  demoState = DemoState::RUNNING;
}

void onStop(void*) {
  demoState = DemoState::WAITING;
}

void onPause(void*) {
  demoState = DemoState::PAUSED;
}

void onReset(void*) {
  demoState = DemoState::WAITING;
  ++resetCount;
}

void onCustom(void*, const char*, size_t) {}

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
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);

  ClientCommandHandlers handlers;
  handlers.onStart = onStart;
  handlers.onStop = onStop;
  handlers.onPause = onPause;
  handlers.onReset = onReset;
  handlers.onCustom = onCustom;

  client.setIdentity("s2", "1.0.0");
  client.setCommandHandlers(handlers);
  client.begin(WIFI_SSID, WIFI_PASSWORD);
}

void loop() {
  client.loop();
  updateDemoLed();
}
