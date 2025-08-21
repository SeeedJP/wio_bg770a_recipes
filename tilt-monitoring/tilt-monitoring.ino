/*
 * tilt-monitoring.ino
 * Copyright (C) Seeed K.K.
 * MIT License
 */

////////////////////////////////////////////////////////////////////////////////
// Libraries:
//   http://librarymanager#ArduinoJson 7.0.4
//
// Additional parts:
//   Grove - I2C <-> Grove - ADXL345 - 3-Axis Digital Accelerometer(±16g) (SKU#101020054)

#include <Adafruit_TinyUSB.h>
#include <csignal>
#include <WioCellular.h>
#include <ArduinoJson.h>
#include <Wire.h>

#define SEARCH_ACCESS_TECHNOLOGY (WioCellularNetwork::SearchAccessTechnology::LTEM)  // https://seeedjp.github.io/Wiki/Wio_BG770A/kb/kb4.html
#define LTEM_BAND (WioCellularNetwork::ALL_LTEM_BAND)                                // https://seeedjp.github.io/Wiki/Wio_BG770A/kb/kb4.html
static const char APN[] = "soracom.io";

static const char HOST[] = "uni.soracom.io";
static constexpr int PORT = 23080;

// template<typename MODULE> using CellularClient = WioCellularTcpClient2<MODULE>;  // TCP
template<typename MODULE> using CellularClient = WioCellularUdpClient2<MODULE>;  // UDP

static constexpr int INTERVAL = 1000 * 60;             // [ms]
static constexpr int POWER_ON_TIMEOUT = 1000 * 20;     // [ms]
static constexpr int POWER_ON_DELAY_TIME = 1000 * 20;  // [ms]
static constexpr int NETWORK_TIMEOUT = 1000 * 60 * 3;  // [ms]
static constexpr int CONNECT_TIMEOUT = 1000 * 10;      // [ms]
static constexpr int RECEIVE_TIMEOUT = 1000 * 10;      // [ms]
static constexpr int POWER_OFF_DELAY_TIME = 1000 * 3;  // [ms]

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

static JsonDocument JsonDoc;

void setup() {
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

  // Network configuration
  WioNetwork.config.searchAccessTechnology = SEARCH_ACCESS_TECHNOLOGY;
  WioNetwork.config.ltemBand = LTEM_BAND;
  WioNetwork.config.apn = APN;

  // Start WioCellular
  WioCellular.begin();

  digitalWrite(LED_BUILTIN, LOW);
}

void loop() {
  digitalWrite(LED_BUILTIN, HIGH);

  // Power on the cellular module
  if (WioCellular.powerOn(POWER_ON_TIMEOUT) != WioCellularResult::Ok) abort();
  delay(POWER_ON_DELAY_TIME);
  WioNetwork.begin();

  // Measure and send
  if (WioNetwork.waitUntilCommunicationAvailable(NETWORK_TIMEOUT)) {
    JsonDoc.clear();
    if (measure(JsonDoc)) {
      send(JsonDoc);
    }
  }

  // Power off the cellular module
  WioCellular.doWorkUntil(POWER_OFF_DELAY_TIME);
  WioNetwork.end();
  if (WioCellular.powerOff() != WioCellularResult::Ok) abort();

  digitalWrite(LED_BUILTIN, LOW);

  WioCellular.doWorkUntil(INTERVAL);
}

static bool measure(JsonDocument &doc) {
  Serial.println("### Measuring");

  digitalWrite(PIN_VGROVE_ENABLE, VGROVE_ENABLE_ON);
  delay(2 + 2);

  AccelInitialize();
  delay(100);
  float x;
  float y;
  float z;
  AccelReadXYZ(&x, &y, &z);

  digitalWrite(PIN_VGROVE_ENABLE, VGROVE_ENABLE_OFF);

  Serial.print(x);
  Serial.print(' ');
  Serial.print(y);
  Serial.print(' ');
  Serial.println(z);

  doc["uptime"] = millis() / 1000;
  doc["accel_x"] = x;
  doc["accel_y"] = y;
  doc["accel_z"] = z;

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

#define I2C_ADDRESS (0x53)
#define REG_POWER_CTL (0x2d)
#define REG_DATAX0 (0x32)

static void AccelInitialize() {
  Wire.begin();
  Wire.beginTransmission(I2C_ADDRESS);
  Wire.write(REG_POWER_CTL);
  Wire.write(0x08);
  Wire.endTransmission();
}

static void AccelReadXYZ(float *x, float *y, float *z) {
  Wire.beginTransmission(I2C_ADDRESS);
  Wire.write(REG_DATAX0);
  Wire.endTransmission();

  if (Wire.requestFrom(I2C_ADDRESS, 6) != 6) {
    *x = 0;
    *y = 0;
    *z = 0;
    return;
  }

  int16_t val;
  ((uint8_t *)&val)[0] = Wire.read();
  ((uint8_t *)&val)[1] = Wire.read();
  *x = (float)val * 2.f / 512;
  ((uint8_t *)&val)[0] = Wire.read();
  ((uint8_t *)&val)[1] = Wire.read();
  *y = (float)val * 2.f / 512;
  ((uint8_t *)&val)[0] = Wire.read();
  ((uint8_t *)&val)[1] = Wire.read();
  *z = (float)val * 2.f / 512;
}
