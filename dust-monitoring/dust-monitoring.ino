/*
 * dust-monitoring.ino
 * Copyright (C) Seeed K.K.
 * MIT License
 */

////////////////////////////////////////////////////////////////////////////////
// Libraries:
//   http://librarymanager#ArduinoJson 7.0.4
//   http://librarymanager#GroveDriverPack 1.12.0
// 
// Additional parts:
//   Grove - I2C <-> Grove - Laser PM2.5 Air Quality Sensor for Arduino - HM3301 (SKU#101020613)

#include <Adafruit_TinyUSB.h>
#include <climits>
#include <WioCellular.h>
#include <ArduinoJson.h>
#include <GroveDriverPack.h>

#define SEARCH_ACCESS_TECHNOLOGY (WioCellularNetwork::SearchAccessTechnology::LTEM)
#define LTEM_BAND (WioCellularNetwork::NTTDOCOMO_LTEM_BAND)
static const char APN[] = "soracom.io";

static const char HOST[] = "uni.soracom.io";
static constexpr int PORT = 23080;

static constexpr int INTERVAL = 1000 * 60 * 5;      // [ms]
static constexpr int POWER_ON_TIMEOUT = 1000 * 20;  // [ms]
static constexpr int RECEIVE_TIMEOUT = 1000 * 10;   // [ms]

static GroveBoard Board;
static GrovePM25HM3301 PM{ &Board.I2C };
static JsonDocument JsonDoc;

void setup(void) {
  Serial.begin(115200);
  {
    const auto start = millis();
    while (!Serial && millis() - start < 5000) {
      delay(2);
    }
  }
  Serial.println();
  Serial.println();

  Serial.println("Startup");

  WioCellular.begin();
  if (WioCellular.powerOn(POWER_ON_TIMEOUT) != WioCellularResult::Ok) abort();

  WioNetwork.config.searchAccessTechnology = SEARCH_ACCESS_TECHNOLOGY;
  WioNetwork.config.ltemBand = LTEM_BAND;
  WioNetwork.config.apn = APN;
  WioNetwork.begin();

  WioCellular.enableGrovePower();
  delay(500);
  Board.I2C.Enable();
  if (!PM.Init()) abort();
}

void loop(void) {
  if (WioNetwork.canCommunicate()) {
    digitalWrite(LED_BUILTIN, HIGH);

    JsonDoc.clear();
    if (measure(JsonDoc)) {
      send(JsonDoc);
    }

    digitalWrite(LED_BUILTIN, LOW);
  }

  Serial.flush();
  WioCellular.doWorkUntil(INTERVAL);
}

static bool measure(JsonDocument &doc) {
  Serial.println("### Measuring");

  PM.Read();
  if (PM.Concentration_1_Atmospheric == INT_MAX) {
    Serial.println("ERROR: Failed to read PM sensor");
    return false;
  }
  Serial.print(PM.Concentration_1_Atmospheric);
  Serial.print('\t');
  Serial.print(PM.Concentration_2_5_Atmospheric);
  Serial.print('\t');
  Serial.print(PM.Concentration_10_Atmospheric);
  Serial.println();

  doc["uptime"] = millis() / 1000;
  doc["pm010"] = PM.Concentration_1_Atmospheric;
  doc["pm025"] = PM.Concentration_2_5_Atmospheric;
  doc["pm100"] = PM.Concentration_10_Atmospheric;

  Serial.println("### Completed");

  return true;
}

static bool send(const JsonDocument &doc) {
  Serial.println("### Sending");

  int socketId;
  if (WioCellular.getSocketUnusedConnectId(WioNetwork.config.pdpContextId, &socketId) != WioCellularResult::Ok) {
    Serial.println("ERROR: Failed to get unused connect id");
    return false;
  }

  Serial.print("Connecting ");
  Serial.print(HOST);
  Serial.print(":");
  Serial.println(PORT);
  if (WioCellular.openSocket(WioNetwork.config.pdpContextId, socketId, "TCP", HOST, PORT, 0) != WioCellularResult::Ok) {
    Serial.println("ERROR: Failed to open socket");
    return false;
  }

  bool result = true;

  if (result) {
    Serial.print("Sending ");
    std::string str;
    serializeJson(doc, str);
    printData(Serial, str.data(), str.size());
    Serial.println();
    if (WioCellular.sendSocket(socketId, str.data(), str.size()) != WioCellularResult::Ok) {
      Serial.println("ERROR: Failed to send socket");
      result = false;
    }
  }

  static uint8_t recvData[WioCellular.RECEIVE_SOCKET_SIZE_MAX];
  size_t recvSize;
  if (result) {
    Serial.println("Receiving");
    if (WioCellular.receiveSocket(socketId, recvData, sizeof(recvData), &recvSize, RECEIVE_TIMEOUT) != WioCellularResult::Ok) {
      Serial.println("ERROR: Failed to receive socket");
      result = false;
    } else {
      printData(Serial, recvData, recvSize);
      Serial.println();
    }
  }

  if (WioCellular.closeSocket(socketId) != WioCellularResult::Ok) {
    Serial.println("ERROR: Failed to close socket");
    result = false;
  }

  if (result)
    Serial.println("### Completed");

  return result;
}

template<typename T>
void printData(T &stream, const void *data, size_t size) {
  auto p = static_cast<const char *>(data);

  for (; size > 0; --size, ++p)
    stream.write(0x20 <= *p && *p <= 0x7f ? *p : '.');
}
