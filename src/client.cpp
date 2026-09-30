#include "client.hpp"

// RemoteCommandClient.cpp


void RemoteCommandClient::begin(const char* ssid, const char* password, uint16_t port) {
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  _server = WiFiServer(port); // est ce vraiment nécessaire etant donné qu'on veut juste qu'elle se connecte à la carte des superviseurs ?
  _server.begin();

  Serial.println();
  Serial.println("WiFi connected");
  Serial.println(WiFi.localIP());

  _running = false;
}

void RemoteCommandClient::loop() {
  WiFiClient client = _server.available();

  if (!client) return;

  String request = "";
  while (client.connected() && client.available()) {
    request += (char)client.read();
    delay(2);
  }

  String cmd = request;
  cmd.trim();

  Serial.print("Commande reçue : ");
  Serial.println(cmd);

  if (cmd == "start") {
    startSystem();
  } else if (cmd == "stop") {  // potentiel problème ici, le stop sera commun à chaque client, il faudrait changer localement les stop, start, et info pour controller individuellement chaque carte arduino
    stopSystem();
  } else if (cmd == "info") {
    ClientInfo info = getClientInfo();

    String json = "{";
    json += "\"deviceId\":\"" + info.deviceId + "\",";
    json += "\"firmwareVersion\":\"" + info.firmwareVersion + "\",";
    json += "\"ipAddress\":\"" + info.ipAddress + "\",";
    json += "\"macAddress\":\"" + info.macAddress + "\",";
    json += "\"running\":" + String(info.running ? "true" : "false") + ",";
    json += "\"uptimeMs\":" + String(info.uptimeMs);
    json += "}";

    client.println(json);
  }

  client.stop();
}

void RemoteCommandClient::startSystem() {
  _running = true;
  Serial.println("Système démarré");
}

void RemoteCommandClient::stopSystem() {
  _running = false;
  Serial.println("Système arrêté");
}

ClientInfo RemoteCommandClient::getClientInfo() {
  ClientInfo info;
  info.deviceId = _deviceId;
  info.firmwareVersion = _firmwareVersion;
  info.ipAddress = WiFi.localIP().toString();

  uint8_t mac[6];
  WiFi.macAddress(mac);

  info.macAddress = String(mac[0], HEX) + ":" +
                    String(mac[1], HEX) + ":" +
                    String(mac[2], HEX) + ":" +
                    String(mac[3], HEX) + ":" +
                    String(mac[4], HEX) + ":" +
                    String(mac[5], HEX);

  info.running = _running;
  info.uptimeMs = millis();
  return info;
}

bool RemoteCommandClient::isSystemRunning() const {
  return _running;
}
