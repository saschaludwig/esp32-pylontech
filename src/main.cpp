#include <WiFi.h>
#include <AsyncTCP.h>
#include <AsyncMqttClient.h>
#include <WiFiManager.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <Ticker.h>
#include "config.h"
#include "pages.h"
#include "pylontech.h"

AsyncMqttClient mqttClient;
Ticker mqttReconnectTimer;
AsyncWebServer webServer(80);
Config config;
Preferences prefs;
WiFiManager wm;
Pylonclient client;

unsigned long wifiLostSince = 0;
unsigned long wifiReconnectCount = 0;
bool wifiNeedsReconnect = false;
bool wifiSetupDone = false;
String savedSSID;
String savedPSK;
unsigned long lastScanTime = 0;
const unsigned long WIFI_WATCHDOG_TIMEOUT_MS = 300000; // 5 minutes
const unsigned long WIFI_SCAN_COOLDOWN_MS = 30000; // 30 seconds between scans

void connectToBestAP() {
  if (savedSSID.length() == 0) {
    dbgln("[wifi] no saved SSID, skipping scan");
    return;
  }
  unsigned long now = millis();
  if (lastScanTime > 0 && (now - lastScanTime) < WIFI_SCAN_COOLDOWN_MS) {
    dbgln("[wifi] scan cooldown active, using default reconnect");
    WiFi.begin(savedSSID.c_str(), savedPSK.c_str());
    return;
  }
  lastScanTime = now;
  dbgln("[wifi] scanning for best AP...");
  int n = WiFi.scanNetworks();
  int bestRSSI = -999;
  int bestIndex = -1;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == savedSSID && WiFi.RSSI(i) > bestRSSI) {
      bestRSSI = WiFi.RSSI(i);
      bestIndex = i;
    }
  }
  if (bestIndex >= 0) {
    dbg("[wifi] best AP: ");
    dbg(WiFi.BSSIDstr(bestIndex));
    dbg(" RSSI: ");
    dbgln(bestRSSI);
    WiFi.begin(savedSSID.c_str(), savedPSK.c_str(), WiFi.channel(bestIndex), WiFi.BSSID(bestIndex));
  } else {
    dbgln("[wifi] no AP found, trying default...");
    WiFi.begin(savedSSID.c_str(), savedPSK.c_str());
  }
  WiFi.scanDelete();
}

void connectToMqtt() {
  dbgln("Connecting to MQTT...");
  mqttClient.connect();
}

void mqttPublish(String topic, String value){
  mqttClient.publish((config.getMqttPrefix() + "/" + topic).c_str(), 0, false, value.c_str());
}

void onMqttConnect(bool sessionPresent) {
  mqttClient.setWill((config.getMqttPrefix() + "/online").c_str(), 0, false, "0");
  mqttPublish("online", "1");
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
  debugSerial.println("Disconnected from MQTT.");

  if (WiFi.isConnected()) {
    mqttReconnectTimer.once(2, connectToMqtt);
  }
}

void setup() {
  debugSerial.begin(115200);
  dbgln();
  dbgln("[config] load")
  prefs.begin("pylontech");
  config.begin(&prefs);
  dbgln("[wifi] start");
  WiFi.mode(WIFI_STA);
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    dbgln("[wifi] disconnected");
    if (wifiLostSince == 0) {
      wifiLostSince = millis();
      if (wifiLostSince == 0) wifiLostSince = 1;
    }
    if (wifiSetupDone) {
      wifiNeedsReconnect = true;
    }
  }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (wifiLostSince > 0) {
      wifiReconnectCount++;
    }
    wifiLostSince = 0;
    dbg("[wifi] connected, IP: ");
    dbgln(WiFi.localIP());
    if (config.getMqttHost().length() > 0) {
      mqttReconnectTimer.once(2, connectToMqtt);
    }
  }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_GOT_IP);
  wm.setClass("invert");
  wm.autoConnect();
  savedSSID = WiFi.SSID();
  savedPSK = WiFi.psk();
  wifiSetupDone = true;
  dbgln("[wifi] finished");
  if (config.getMqttHost().length() > 0){
    dbgln("[mqtt] start");
    dbg("connecting to ");
    dbg(config.getMqttHost().c_str());
    dbg(":");
    dbgln(config.getMqttPort());
    IPAddress ip;
    if (ip.fromString(config.getMqttHost())){
      mqttClient.setServer(ip, config.getMqttPort());
    }
    else{
      mqttClient.setServer(config.getMqttHost().c_str(), config.getMqttPort());
    }
    if (config.getMqttUsername().length() > 0){
      mqttClient.setCredentials(strdup(config.getMqttUsername().c_str()), strdup(config.getMqttPassword().c_str()));
    }
    mqttClient.onConnect(onMqttConnect);
    mqttClient.onDisconnect(onMqttDisconnect);
    connectToMqtt();
    dbgln("[mqtt] finished");
  }  
  client.Begin(&pylonSerial);
  setupPages(&webServer, &wm, &config, &client, &mqttClient);
  webServer.begin();
  pylonSerial.begin(115200, SERIAL_8N1);
  dbgln("[setup] finished");
}

void loop() {
  if (wifiNeedsReconnect) {
    wifiNeedsReconnect = false;
    connectToBestAP();
  }
  if (!WiFi.isConnected()) {
    if (wifiLostSince > 0 && (millis() - wifiLostSince) > WIFI_WATCHDOG_TIMEOUT_MS) {
      dbgln("[wifi] reconnect timeout, rebooting...");
      ESP.restart();
    }
    delay(1000);
    return;
  }
  if (mqttClient.connected()){
    for (size_t i = 0; i < config.getModuleCount(); i++)
    {
      uint8_t major = 0;
      uint8_t minor = 0;
      dbg("polling pylontech module ");
      dbgln(i);
      mqttPublish("polling", String(i));
      auto frame = client.SendCommand(Pylonframe(2 + i, CommandInformation::ProtocolVersion));
      if (frame.HasError){
        dbg("version failed for ");
        dbgln(i);
        dbg("with code");
        dbgln(frame.Cid2);
        continue;
      }
      else{
        major = frame.MajorVersion;
        minor = frame.MinorVersion;
      }

      frame = client.SendCommand(Pylonframe(major, minor, 2 + i, CommandInformation::ManufacturerInfo));
      if (frame.HasError){
        dbg("manufacturer failed for ");
        dbgln(i);
        dbg("with code");
        dbgln(frame.Cid2);
      }
      else
      {
        auto manufacturer = Pylonframe::PylonManufacturerInfo(frame.Info);
        manufacturer.publish([i](String name, String value){mqttPublish(String(i) + "/" + name, value);});
      }

      frame = client.SendCommand(Pylonframe(major, minor, 2 + i, CommandInformation::FirmwareInfo));
      if (frame.HasError){
        dbg("firmware failed for ");
        dbgln(i);
        dbg("with code");
        dbgln(frame.Cid2);
      }
      else
      {
        auto firmware = Pylonframe::PylonFirmwareInfo(frame.Info);
        firmware.publish([i](String name, String value){mqttPublish(String(i) + "/" + name, value);});
      }

      frame = client.SendCommand(Pylonframe(major, minor, 2 + i, CommandInformation::Serialnumber));
      if (frame.HasError){
        dbg("serialnumber failed for ");
        dbgln(i);
        dbg("with code");
        dbgln(frame.Cid2);
      }
      else
      {
        auto serialnumber = Pylonframe::PylonSerialnumber(frame.Info);
        serialnumber.publish([i](String name, String value){mqttPublish(String(i) + "/" + name, value);});
      }

      frame = client.SendCommand(Pylonframe(major, minor, 2 + i, CommandInformation::SystemParameterFixedPoint));
      if (frame.HasError){
        dbg("system failed for ");
        dbgln(i);
        dbg("with code");
        dbgln(frame.Cid2);
      }
      else
      {
        auto system = Pylonframe::PylonSystemParameter(frame.Info);
        system.publish([i](String name, String value){mqttPublish(String(i) + "/" + name, value);});
      }

      frame = client.SendCommand(Pylonframe(major, minor, 2 + i, CommandInformation::GetChargeDischargeManagementInfo));
      if (frame.HasError){
        dbg("chargeDischarge failed for ");
        dbgln(i);
        dbg("with code");
        dbgln(frame.Cid2);
      }
      else
      {
        auto chargeDischarge = Pylonframe::PylonChargeDischargeManagementInfo(frame.Info);
        chargeDischarge.publish([i](String name, String value){mqttPublish(String(i) + "/" + name, value);});
      }

      frame = client.SendCommand(Pylonframe(major, minor, 2 + i, CommandInformation::AnalogValueFixedPoint));
      if (frame.HasError){
        dbg("analog failed for ");
        dbgln(i);
        dbg("with code");
        dbgln(frame.Cid2);
      }
      else
      {
        auto analog = Pylonframe::PylonAnalogValue(frame.Info);
        analog.publish([i](String name, String value){mqttPublish(String(i) + "/" + name, value);});
      }

      frame = client.SendCommand(Pylonframe(major, minor, 2 + i, CommandInformation::AlarmInfo));
      if (frame.HasError){
        dbg("alarm failed for ");
        dbgln(i);
        dbg("with code");
        dbgln(frame.Cid2);
      }
      else
      {
        auto alarm = Pylonframe::PylonAlarmInfo(frame.Info);
        alarm.publish([i](String name, String value){mqttPublish(String(i) + "/" + name, value);});
      }
    }
    
    delay(config.getInterval());
  }
}
