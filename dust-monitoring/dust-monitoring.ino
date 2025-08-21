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
#include <csignal>
#include <WioCellular.h>
#include <ArduinoJson.h>
#include <GroveDriverPack.h>

#define SEARCH_ACCESS_TECHNOLOGY (WioCellularNetwork::SearchAccessTechnology::LTEM)
#define LTEM_BAND (WioCellularNetwork::ALL_LTEM_BAND)
static const char APN[] = "soracom.io";

static const char HOST[] = "uni.soracom.io";
static constexpr int PORT = 23080;

// template<typename MODULE> using CellularClient = WioCellularTcpClient2<MODULE>;  // TCP
template<typename MODULE> using CellularClient = WioCellularUdpClient2<MODULE>;  // UDP

static constexpr int INTERVAL = 1000 * 10;             // [ms]
static constexpr int POWER_ON_TIMEOUT = 1000 * 20;     // [ms]
static constexpr int NETWORK_TIMEOUT = 1000 * 60 * 3;  // [ms]
static constexpr int CONNECT_TIMEOUT = 1000 * 10;      // [ms]
static constexpr int RECEIVE_TIMEOUT = 1000 * 10;      // [ms]

static void abortHandler(int sig) {
  Serial.printf("ABORT: Signal %d received\n", sig);
  yield();

  vTaskSuspendAll();  // FreeRTOS
  while (true) {
    ledOn(LED_BUILTIN);
    nrfx_coredep_delay_us(100000);  // Spin
    ledOff(LED_BUILTIN);
    nrfx_coredep_delay_us(100000);  // Spin
  }
}

static GroveBoard Board;
static GrovePM25HM3301 PM{ &Board.I2C };
static JsonDocument JsonDoc;

void setup(void) {
  signal(SIGABRT, abortHandler);
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
  digitalWrite(LED_BUILTIN, HIGH);

  WioCellular.begin();
  if (WioCellular.powerOn(POWER_ON_TIMEOUT) != WioCellularResult::Ok) abort();

  WioNetwork.config.searchAccessTechnology = SEARCH_ACCESS_TECHNOLOGY;
  WioNetwork.config.ltemBand = LTEM_BAND;
  WioNetwork.config.apn = APN;
  WioNetwork.begin();
  if (!WioNetwork.waitUntilCommunicationAvailable(NETWORK_TIMEOUT)) abort();

  digitalWrite(PIN_VGROVE_ENABLE, VGROVE_ENABLE_ON);
  delay(500);
  Board.I2C.Enable();
  if (!PM.Init()) abort();

  digitalWrite(LED_BUILTIN, LOW);
}

void loop(void) {
  digitalWrite(LED_BUILTIN, HIGH);

  JsonDoc.clear();
  if (measure(JsonDoc)) {
    send(JsonDoc);
  }

  digitalWrite(LED_BUILTIN, LOW);

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

  Serial.print("Connecting ");
  Serial.print(HOST);
  Serial.print(":");
  Serial.println(PORT);

  {
    CellularClient<WioCellularModule> client{ WioCellular };
    if (!client.open(WioNetwork.config.pdpContextId, HOST, PORT)) {
      Serial.printf("ERROR: Failed to open %s\n", WioCellularResultToString(client.getLastResult()));
      return false;
    }

    if (!client.waitForConnect(CONNECT_TIMEOUT)) {
      Serial.printf("ERROR: Failed to connect %s\n", WioCellularResultToString(client.getLastResult()));
      return false;
    }

    Serial.print("Sending ");
    std::string str;
    serializeJson(doc, str);
    printData(Serial, str.data(), str.size());
    Serial.println();
    if (!client.send(str.data(), str.size())) {
      Serial.printf("ERROR: Failed to send socket %s\n", WioCellularResultToString(client.getLastResult()));
      return false;
    }

    Serial.println("Receiving");
    static uint8_t recvData[WioCellular.RECEIVE_SOCKET_SIZE_MAX];
    size_t recvSize;
    if (!client.receive(recvData, sizeof(recvData), &recvSize, RECEIVE_TIMEOUT)) {
      Serial.printf("ERROR: Failed to receive socket %s\n", WioCellularResultToString(client.getLastResult()));
      return false;
    }

    printData(Serial, recvData, recvSize);
    Serial.println();
  }

  Serial.println("### Completed");

  return true;
}

template<typename T>
void printData(T &stream, const void *data, size_t size) {
  auto p = static_cast<const char *>(data);

  for (; size > 0; --size, ++p)
    stream.write(0x20 <= *p && *p <= 0x7f ? *p : '.');
}
