#include <Arduino.h>
#include <Wire.h>
#include <SSD1306Wire.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <XPowersLib.h>
#include <TinyGPS++.h>

// ==========================
// CONFIGURACIÓN DE PINES
// ==========================

#define BUTTON_PIN 38
#define LED_PIN    4

#define OLED_SDA   21
#define OLED_SCL   22
#define OLED_ADDR  0x3C

#define GPS_RX     34
#define GPS_TX     12
#define GPS_BAUD   9600

// ==========================
// CONFIGURACIÓN BLE PROFESOR
// ==========================

#define BLE_NAME "TBeam-SOS-Toni_Alvaro"

#define SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define CHARACTERISTIC_UUID "abcdefab-1234-5678-1234-abcdefabcdef"

// ==========================
// OBJETOS GLOBALES
// ==========================

SSD1306Wire display(OLED_ADDR, OLED_SDA, OLED_SCL);

BLEServer *pServer = nullptr;
BLECharacteristic *pCharacteristic = nullptr;
bool deviceConnected = false;

XPowersLibInterface *power = nullptr;
bool powerOK = false;

TinyGPSPlus gps;
HardwareSerial GPSSerial(1);

// ==========================
// PROTOTIPOS
// ==========================

void oledMsg(const char* line1, const char* line2 = "", const char* line3 = "");

void setupOLED();
void setupBLE();
void setupGPS();

bool iniciarXPower();
void configurarMedidasXPower();
String leerBateriaTexto();

void actualizarGPS();
bool gpsDisponible();
bool gpsTieneFixValido();
bool gpsTieneHoraValida();

String leerGPSTexto();
String leerTiempoGPSTexto();
String crearMensajeSOS();

bool botonPulsado();
void sendSOS();
void mostrarEstadoPeriodico();
void imprimirDiagnosticoGPS();


// ==========================
// CALLBACKS BLE
// ==========================

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    deviceConnected = true;
    Serial.println("BLE Client connectat");
  }

  void onDisconnect(BLEServer* pServer) override {
    deviceConnected = false;
    Serial.println("BLE Client desconnectat");
    pServer->startAdvertising();
  }
};


// ==========================
// OLED
// ==========================

void setupOLED() {
  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);

  oledMsg("T-Beam SOS", "Iniciant...", "");
}

void oledMsg(const char* line1, const char* line2, const char* line3) {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);

  display.drawString(0, 10, line1);

  if (strlen(line2) > 0) {
    display.drawString(0, 25, line2);
  }

  if (strlen(line3) > 0) {
    display.drawString(0, 40, line3);
  }

  display.display();
}


// ==========================
// BLE
// ==========================

void setupBLE() {
  BLEDevice::init(BLE_NAME);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY
  );

  pCharacteristic->addDescriptor(new BLE2902());
  pCharacteristic->setValue("READY");

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);

  BLEDevice::startAdvertising();

  Serial.println("BLE actiu - '" BLE_NAME "'");
}


// ==========================
// XPOWER / BATERÍA
// ==========================

bool iniciarXPower() {
  power = new XPowersAXP2101(Wire, OLED_SDA, OLED_SCL);

  if (power->init()) {
    Serial.println("PMU detectada: AXP2101");
    configurarMedidasXPower();
    return true;
  }

  delete power;
  power = nullptr;

  power = new XPowersAXP192(Wire, OLED_SDA, OLED_SCL);

  if (power->init()) {
    Serial.println("PMU detectada: AXP192");
    configurarMedidasXPower();
    return true;
  }

  delete power;
  power = nullptr;

  Serial.println("ERROR: PMU XPower no detectada");
  return false;
}

void configurarMedidasXPower() {
  if (power == nullptr) return;

  power->disableTSPinMeasure();
  power->enableBattDetection();
  power->enableBattVoltageMeasure();
  power->enableVbusVoltageMeasure();
  power->enableSystemVoltageMeasure();

  Serial.println("Mesures XPower activades");
}

String leerBateriaTexto() {
  if (!powerOK || power == nullptr) {
    return "BAT=NO_PMU";
  }

  uint16_t mVBat = power->getBattVoltage();
  uint16_t mVUsb = power->getVbusVoltage();
  uint16_t mVSys = power->getSystemVoltage();

  bool bateriaConectada = power->isBatteryConnect();
  bool usbConectado = power->isVbusIn();
  bool cargando = power->isCharging();

  String texto = "BAT=";

  if (bateriaConectada && mVBat > 500) {
    texto += String(mVBat / 1000.0, 2);
    texto += "V";

    int porcentaje = power->getBatteryPercent();
    texto += "|";
    texto += String(porcentaje);
    texto += "%";
  } else {
    texto += "NO";
  }

  texto += "|USB=";

  if (usbConectado || mVUsb > 1000) {
    texto += String(mVUsb / 1000.0, 2);
    texto += "V";
  } else {
    texto += "NO";
  }

  texto += "|SYS=";
  texto += String(mVSys / 1000.0, 2);
  texto += "V";

  texto += "|CHG=";
  texto += cargando ? "SI" : "NO";

  return texto;
}


/* ==========================
  GPS
========================== */ 

void setupGPS() {
  GPSSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);
  Serial.println("GPS iniciat a 9600 bauds");
}

void actualizarGPS() {
  while (GPSSerial.available() > 0) {
    gps.encode(GPSSerial.read());
  }
}

bool gpsDisponible() {
  /*
    Si pasados 10 segundos no se ha recibido casi nada,
    probablemente el GPS no está comunicando.
  */
  if (millis() > 10000 && gps.charsProcessed() < 10) {
    return false;
  }

  return true;
}

bool gpsTieneFixValido() {
  if (!gpsDisponible()) {
    return false;
  }

  if (!gps.location.isValid()) {
    return false;
  }

  if (gps.location.age() > 10000) {
    return false;
  }

  return true;
}

bool gpsTieneHoraValida() {
  /*
    Decisión del proyecto:
    la hora solo se acepta cuando también hay fix GPS.
    Así evitamos fechas falsas tipo 30/11/1999.
  */

  if (!gpsTieneFixValido()) {
    return false;
  }

  if (!gps.date.isValid() || !gps.time.isValid()) {
    return false;
  }

  if (gps.date.age() > 10000 || gps.time.age() > 10000) {
    return false;
  }

  int year = gps.date.year();

  if (year < 2024 || year > 2035) {
    return false;
  }

  return true;
}

String leerGPSTexto() {
  if (!gpsDisponible()) {
    return "GPS=NO_DATA";
  }

  if (!gpsTieneFixValido()) {
    return "GPS=NO_FIX";
  }

  String texto = "GPS=";
  texto += String(gps.location.lat(), 6);
  texto += ",";
  texto += String(gps.location.lng(), 6);

  return texto;
}

String leerTiempoGPSTexto() {
  if (!gpsTieneHoraValida()) {
    return "UTC=NO_SYNC";
  }

  char buffer[40];

  snprintf(
    buffer,
    sizeof(buffer),
    "UTC=%02d/%02d/%04d,%02d:%02d:%02d",
    gps.date.day(),
    gps.date.month(),
    gps.date.year(),
    gps.time.hour(),
    gps.time.minute(),
    gps.time.second()
  );

  return String(buffer);
}

void imprimirDiagnosticoGPS() {
  Serial.print("GPS chars: ");
  Serial.println(gps.charsProcessed());

  Serial.print("GPS satellites: ");
  if (gps.satellites.isValid()) {
    Serial.println(gps.satellites.value());
  } else {
    Serial.println("NO_VALID");
  }

  Serial.print("GPS HDOP: ");
  if (gps.hdop.isValid()) {
    Serial.println(gps.hdop.value());
  } else {
    Serial.println("NO_VALID");
  }
}


// ==========================
// BOTÓN
// ==========================

bool botonPulsado() {
  /*
    Antirrebote simple.
    Devuelve true solo una vez cuando el botón pasa a LOW.
  */

  static bool lastReading = HIGH;
  static bool buttonState = HIGH;
  static unsigned long lastDebounceTime = 0;

  bool reading = digitalRead(BUTTON_PIN);

  if (reading != lastReading) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > 50) {
    if (reading != buttonState) {
      buttonState = reading;

      if (buttonState == LOW) {
        lastReading = reading;
        return true;
      }
    }
  }

  lastReading = reading;
  return false;
}


// ==========================
// SOS
// ==========================

String crearMensajeSOS() {
  static int sosCount = 0;
  sosCount++;

  String msg = "SOS|";
  msg += String(sosCount);

  // Se conserva el timestamp simple que ya daba el profesor
  msg += "|TS=";
  msg += String(millis() / 1000);

  // Identificador de la placa
  msg += "|TBEAM01";

  // Nuevos campos del proyecto
  msg += "|";
  msg += leerGPSTexto();

  msg += "|";
  msg += leerTiempoGPSTexto();

  msg += "|";
  msg += leerBateriaTexto();

  return msg;
}

void sendSOS() {
  digitalWrite(LED_PIN, LOW);

  String msg = crearMensajeSOS();

  Serial.print("SOS creat: ");
  Serial.println(msg);

  if (deviceConnected && pCharacteristic) {
    pCharacteristic->setValue(msg.c_str());
    pCharacteristic->notify();

    Serial.println("Enviat per BLE!");

    oledMsg("SOS ENVIAT!", "via BLE", leerGPSTexto().substring(0, 20).c_str());
  } else {
    Serial.println("Sense client BLE");

    oledMsg("SOS CREAT!", "Connecta app", leerGPSTexto().substring(0, 20).c_str());
  }

  delay(500);
  digitalWrite(LED_PIN, HIGH);
}


// ==========================
// ESTADO PERIÓDICO
// ==========================

void mostrarEstadoPeriodico() {
  static unsigned long lastStatus = 0;

  if (millis() - lastStatus < 4000) {
    return;
  }

  lastStatus = millis();

  String bateria = leerBateriaTexto();
  String gpsTexto = leerGPSTexto();
  String tiempoTexto = leerTiempoGPSTexto();

  Serial.println("----- ESTAT -----");
  Serial.println(bateria);
  Serial.println(gpsTexto);
  Serial.println(tiempoTexto);
  imprimirDiagnosticoGPS();

  if (deviceConnected) {
    oledMsg("T-Beam SOS", "BLE CONNECTAT", gpsTexto.substring(0, 20).c_str());
  } else {
    oledMsg("T-Beam SOS", "BLE BUSCANT", gpsTexto.substring(0, 20).c_str());
  }
}


// ==========================
// SETUP
// ==========================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("=== T-BEAM SOS - BLE + GPS + XPower ===");

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  pinMode(BUTTON_PIN, INPUT);

  Wire.begin(OLED_SDA, OLED_SCL);

  setupOLED();

  powerOK = iniciarXPower();

  if (powerOK) {
    Serial.println(leerBateriaTexto());
  } else {
    Serial.println("Sistema sense lectura XPower");
  }

  setupGPS();
  setupBLE();

  Serial.println("PREPARAT!");
  Serial.println("- Cada polsacio = 1 SOS");
  Serial.println("- BLE notify actiu");
  Serial.println("- GPS dona coordenades i hora quan te fix");

  oledMsg("T-Beam SOS", "Polsa boto", "BLE preparat");
}


// ==========================
// LOOP
// ==========================

void loop() {
  actualizarGPS();

  if (botonPulsado()) {
    Serial.println("PULSACIO DETECTADA -> ENVIA SOS!");
    sendSOS();
  }

  mostrarEstadoPeriodico();
}