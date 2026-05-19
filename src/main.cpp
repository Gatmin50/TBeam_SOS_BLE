#include <Arduino.h>
#include <Wire.h>
#include <SSD1306Wire.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <XPowersLib.h>

#define BUTTON_PIN 38    // Botó usuari
#define LED_PIN    4     // LED usuari
#define OLED_SDA   21
#define OLED_SCL   22
#define OLED_ADDR  0x3C

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

#define SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define CHARACTERISTIC_UUID "abcdefab-1234-5678-1234-abcdefabcdef"

// PMU XPower por I2C usando los mismos pines que la OLED
#define PMU_SDA OLED_SDA
#define PMU_SCL OLED_SCL

/*
  Lectura antigua por ADC.
  La mantenemos como apoyo, pero el apartado 3 lo hacemos con XPowersLib.
*/
#define nivel_alimentacion_pin 35
#define factor_de_voltaje 2.0
#define voltaje_orinetativo 3.3

// Objetos globales
XPowersLibInterface *power = nullptr;
bool powerOK = false;

SSD1306Wire display(OLED_ADDR, OLED_SDA, OLED_SCL);
BLEServer *pServer = nullptr;
BLECharacteristic *pCharacteristic = nullptr;
bool deviceConnected = false;

// Prototipos
bool iniciarXPower();
void configurarMedidasXPower();
String leerBateriaTexto();
void nivel_alimentacion();
void setupBLE();
void sendSOS();
void oledMsg(const char* line1, const char* line2 = "", const char* line3 = "");

// Callbacks BLE
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

// OLED
void oledMsg(const char* line1, const char* line2, const char* line3) {
  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  
  int y = 10;

  display.drawString(0, y, line1);
  y += 15;

  if (strlen(line2) > 0) {
    display.drawString(0, y, line2);
    y += 15;
  }

  if (strlen(line3) > 0) {
    display.drawString(0, y, line3);
  }

  display.display();
}

// BLE
void setupBLE() {
  BLEDevice::init("TBeam-SOS-Toni_Alvaro");

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
  
  Serial.println("BLE actiu - 'TBeam-SOS-Toni_Alvaro'");
}

void configurarMedidasXPower() {
  if (power == nullptr) return;

  /*
    Activamos las mediciones internas de la PMU.
    Sin esto puede devolver 0 mV aunque la placa esté alimentada.
  */
  power->disableTSPinMeasure();
  power->enableBattDetection();
  power->enableVbusVoltageMeasure();
  power->enableBattVoltageMeasure();
  power->enableSystemVoltageMeasure();

  Serial.println("Mediciones XPower activadas");
}

// XPower / PMU
bool iniciarXPower() {
  /*
    La LilyGO T-Beam puede montar distintas PMU según la versión.
    Probamos primero AXP2101 y luego AXP192.
  */

  power = new XPowersAXP2101(Wire, PMU_SDA, PMU_SCL);

  if (power->init()) {
    Serial.println("PMU detectada: AXP2101");
    configurarMedidasXPower();
    return true;
  }

  delete power;
  power = nullptr;

  power = new XPowersAXP192(Wire, PMU_SDA, PMU_SCL);

  if (power->init()) {
    Serial.println("PMU detectada: AXP192");
    configurarMedidasXPower();
    return true;
  }

  delete power;
  power = nullptr;

  Serial.println("ERROR: No se ha detectado PMU XPower");
  return false;
}

String leerBateriaTexto() {
  if (!powerOK || power == nullptr) {
    return "PWR=PMU_NO_DETECTADA";
  }

  uint16_t mVBat = power->getBattVoltage();
  uint16_t mVUsb = power->getVbusVoltage();
  uint16_t mVSys = power->getSystemVoltage();

  bool bateriaConectada = power->isBatteryConnect();
  bool usbConectado = power->isVbusIn();
  bool cargando = power->isCharging();

  String texto = "";

  texto += "BAT=";
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

// SOS BLE
void sendSOS() {
  digitalWrite(LED_PIN, LOW);

  static int sosCount = 0;
  sosCount++;

  String bateria = leerBateriaTexto();

  String msg = "SOS|";
  msg += String(sosCount);
  msg += "|TS=";
  msg += String(millis() / 1000);
  msg += "|";
  msg += bateria;
  msg += "|TBEAM01";

  Serial.println("SOS #" + String(sosCount) + ": " + msg);

  if (deviceConnected && pCharacteristic) {
    pCharacteristic->setValue(msg.c_str());
    pCharacteristic->notify();

    Serial.println("Enviat per BLE!");

    String linea2 = "#" + String(sosCount);
    String linea3 = bateria.substring(0, 20);

    oledMsg("SOS ENVIAT!", linea2.c_str(), linea3.c_str());
  } else {
    Serial.println("!!!!Sense client BLE!!!!");

    String linea2 = "#" + String(sosCount);
    oledMsg("SOS CREAT!", linea2.c_str(), "Connecta app");
  }

  delay(500);
  digitalWrite(LED_PIN, HIGH);

  if (deviceConnected) {
    oledMsg("T-Beam SOS", "BLE CONNECTAT", "Polsa boto");
  } else {
    oledMsg("T-Beam SOS", "BLE BUSCANT", "Connecta app");
  }
}

// Lectura antigua por ADC, solo como comparación
void nivel_alimentacion() {
  int valor_adc = analogRead(nivel_alimentacion_pin);
  float voltaje = (valor_adc / 4095.0) * voltaje_orinetativo * factor_de_voltaje;

  Serial.print("Nivel de alimentacion ADC: ");
  Serial.print(voltaje);
  Serial.println(" V");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("=== T-BEAM SOS v3 (cada polsacio) ===");
  
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  pinMode(BUTTON_PIN, INPUT);

  // I2C compartido por OLED y PMU
  Wire.begin(OLED_SDA, OLED_SCL);
  
  // OLED
  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);

  oledMsg("T-Beam SOS v3", "Sense GPS", "Iniciant...");

  // Inicialización XPower
  powerOK = iniciarXPower();

  if (powerOK) {
    Serial.println(leerBateriaTexto());
  } else {
    Serial.println("Sistema continua sense lectura XPower");
  }
  
  setupBLE();
  
  Serial.println("PREPARAT!");
  Serial.println("- Cada polsacio = 1 SOS");
  Serial.println("- nRF Connect / LightBlue -> TBeam-SOS-Toni_Alvaro");
  Serial.println("- Notificacions ON");
  
  oledMsg("T-Beam SOS v3", "Polsa boto", "BLE preparat");
}

void loop() {
  /*
    Antirrebote correcto:
    - lastReading guarda la última lectura física del pin.
    - buttonState guarda el estado estable del botón.
    - Solo enviamos SOS cuando el estado estable pasa a LOW.
  */

  static bool lastReading = HIGH;
  static bool buttonState = HIGH;
  static unsigned long lastDebounceTime = 0;

  bool reading = digitalRead(BUTTON_PIN);

  // Si cambia la lectura física, reiniciamos el temporizador de rebote
  if (reading != lastReading) {
    lastDebounceTime = millis();
  }

  // Si la lectura se mantiene estable más de 50 ms, aceptamos el cambio
  if ((millis() - lastDebounceTime) > 50) {
    if (reading != buttonState) {
      buttonState = reading;

      // Botón activo a LOW: solo enviamos SOS al pulsar, no al soltar
      if (buttonState == LOW) {
        Serial.println("PULSACIO DETECTADA -> ENVIA SOS!");
        sendSOS();
      }
    }
  }

  lastReading = reading;

  // Estado BLE y batería cada 4 segundos
  static unsigned long lastStatus = 0;

  if (millis() - lastStatus > 4000) {
    lastStatus = millis();

    String bateria = leerBateriaTexto();
    String bateriaCorta = bateria.substring(0, 20);

    Serial.println(bateria);

    if (deviceConnected) {
      oledMsg("T-Beam SOS", "BLE CONNECTAT", bateriaCorta.c_str());
    } else {
      oledMsg("T-Beam SOS", "BLE BUSCANT", bateriaCorta.c_str());
    }
  }
}