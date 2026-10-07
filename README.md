# SuperV

SuperV is a PlatformIO library for remote commands between Arduino Wi-Fi
devices. It provides a client that joins the supervisor's Wi-Fi network and a
master that creates that network, accepts clients, and sends commands over
Serial.

The repository is also a complete PlatformIO project. Its default `client`
environment builds the included LED demonstration for the Arduino UNO R4 WiFi;
the `master` environment builds the supervisor firmware.

## Requirements

- PlatformIO
- Arduino framework
- An Arduino board with Wi-Fi support compatible with the Arduino `WiFi`
  library. The included project configuration targets the Arduino UNO R4 WiFi
  (`renesas-ra`).

The master implementation uses the SimpleCLI dependency, which PlatformIO
resolves from the library manifest.

## Use from another PlatformIO project

Add the repository as a dependency in the consuming project's
`platformio.ini`:

```ini
[env:my_board]
platform = renesas-ra
board = uno_r4_wifi
framework = arduino
lib_deps =
    https://github.com/stackOverHeap/superV.git#v1.0.2
```

Include the client API and register callbacks for commands your application
wants to handle:

```cpp
#include <Arduino.h>
#include <superv/client.hpp>

RemoteCommandClient client;

void onStart(void) {
  // Start your application.
}

void setup() {
  ClientCommandHandlers handlers;
  handlers.onStart = onStart;
  handlers.onStop = nullptr; // callbacks are user-defined, but triggered by the master
  handlers.onPause = nullptr;
  handler.onReset = nullptr;

  client.setIdentity("keyboard-friendly name");
  client.setCommandHandlers(handlers);
  client.setup();
}

void loop() {
  client.loop();
}
```

The `RemoteCommandClient` constructor admit an optional `realTime` boolean value, this feature can be enabled when short loop calls are needed, such as audio equipments.

`setup()` joins the supervisor Wi-Fi network, then connects to the supervisor
at `192.168.4.1` on port `DEFINE_SERVER_PORT` (90 by default).

## Custom commands

Clients can register eight additional application-specific commands that the master
can trigger remotely. Register each command on the client side and provide a
callback that receives the command ID assigned to the call:

```cpp
#include <Arduino.h>
#include <superv/client.hpp>

RemoteCommandClient client;

void onCustom(uint8_t commandIndex) {
  switch (commandIndex) {
    case 0:
      Serial.println("reboot command received");
      break;
    case 1:
      Serial.println("flash command received");
      break;
    default:
      Serial.println("unknown custom command");
      break;
  }
}

void setup() {
  ClientCommandHandlers handlers;
  handlers.onCustom = onCustom;

  client.setIdentity("printer");
  client.setCommandHandlers(handlers);
  client.registerCustomCommands("reboot", 0);
  client.registerCustomCommands("flash", 1);
  client.setup();
}
```

On the supervisor side, the built-in Serial interface exposes the custom command
entry point:

```text
custom <peer> <command>
```

For example, after the peer is connected and has registered `reboot` and
`flash`, the master can send:

```text
custom anim1 reboot
custom anim1 flash
```

The master command list also includes `help <peer>` to display the custom
commands registered by a specific connected client.

For a project that runs the supervisor instead, include
`<superv/master.hpp>`, call `Master::getInstance().init()` in `setup()`, and
call its `loop()` method from the Arduino `loop()`. The master creates the
`supervisor-net` access point and provides its command interface over Serial.
The supervisor screen displays each connected client's identity in its button
slot (for example, `citrouille`). Until the client sends its identity, the slot
shows a fallback name such as `anim1`; disconnected slots can be reused.

## Included LED example

The complete standalone client demonstration is in
`examples/client_led/client_led.ino`. Open this repository as a PlatformIO
project and upload its default `client` environment to flash the same example:

```sh
pio run -e client -t upload
```

The example connects to the `supervisor-net` access point, handles START/STOP/PAUSE/RESET callbacks, and blinks the built-in
LED at a different rate while running or paused. Update the SSID/password in
the example if your supervisor uses different credentials.

To flash the supervisor firmware instead:

```sh
pio run -e master -t upload
```

## Project layout

```text
library.json                  # Manifest used when installing this repository
lib/SuperV/include/superv/    # Public API headers
lib/SuperV/src/               # Library implementation
src/main.cpp                  # Standalone project entry point
examples/client_led/          # Reusable LED demo sketch
```
