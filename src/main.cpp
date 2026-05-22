#include <Arduino.h>
#include <Wire.h>
#include <SSD1306Wire.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <XPowersLib.h>
#include <TinyGPS++.h>

#define BTN_PIN 38
#define LED_PIN 4

SSD1306Wire display(0x3C, 21, 22);
BLECharacteristic *pChar;
XPowersLibInterface *power;
TinyGPSPlus gps;
HardwareSerial gpsSer(1);
bool bleConnected = false;

// ---------- BLE Callback ----------
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer*) { bleConnected = true; }
  void onDisconnect(BLEServer *s) { bleConnected = false; s->startAdvertising(); }
};

// ---------- Función Helper OLED ----------
void oled(String a, String b = "", String c = "") {
  display.clear();
  display.drawString(0, 10, a);
  display.drawString(0, 25, b);
  display.drawString(0, 40, c);
  display.display();
}

// ---------- Lógica de GPS y Tiempo ----------
bool hasFix() { 
  return gps.location.isValid() && gps.satellites.value() >= 4; 
}

String getGPSInfo() {
  // Solo devuelve los datos si hay satélites conectados
  if (!hasFix()) return "Buscant satelits...";
  
  char buf[40];
  snprintf(buf, sizeof(buf), "%.5f,%.5f %02d:%02d:%02d", 
           gps.location.lat(), gps.location.lng(), 
           gps.time.hour(), gps.time.minute(), gps.time.second());
  return String(buf);
}

// ---------- Lógica Botón Compacta ----------
bool isButtonPressed() {
  static uint32_t t = 0; static bool stable = HIGH, last = HIGH;
  bool r = digitalRead(BTN_PIN);
  if (r != last) t = millis();
  if (millis() - t > 50 && r != stable) { stable = r; last = r; return stable == LOW; }
  last = r; return false;
}

// ---------- SETUP ----------
void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, HIGH);
  pinMode(BTN_PIN, INPUT);

  Wire.begin(21, 22);
  display.init(); display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10); display.setTextAlignment(TEXT_ALIGN_LEFT);

  // Inicializar Energía (Soporta AXP2101 o AXP192)
  power = new XPowersAXP2101(Wire, 21, 22);
  if (!power->init()) { delete power; power = new XPowersAXP192(Wire, 21, 22); power->init(); }
  if (power) power->enableBattVoltageMeasure();

  // Iniciar GPS
  gpsSer.begin(9600, SERIAL_8N1, 34, 12);

  // Iniciar BLE
  BLEDevice::init("TBeam-SOS");
  BLEServer *pServer = BLEDevice::createServer(); 
  pServer->setCallbacks(new ServerCB());
  BLEService *srv = pServer->createService("12345678-1234-1234-1234-1234567890ab");
  pChar = srv->createCharacteristic("abcdefab-1234-5678-1234-abcdefabcdef", BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pChar->addDescriptor(new BLE2902());
  srv->start(); BLEDevice::startAdvertising();
}

// ---------- LOOP ----------
void loop() {
  while (gpsSer.available()) gps.encode(gpsSer.read());

  if (isButtonPressed()) {
    digitalWrite(LED_PIN, LOW);
    
    String batInfo = power ? String(power->getBatteryPercent()) + "%" : "NO_BAT";
    String msg = "SOS|" + (hasFix() ? getGPSInfo() : "NO_GPS") + "|" + batInfo;
    
    Serial.println("SEND|" + msg);
    if (bleConnected) { pChar->setValue(msg.c_str()); pChar->notify(); }
    
    oled("SOS ENVIAT!", bleConnected ? "via BLE" : "Sense BLE", getGPSInfo());
    delay(400); digitalWrite(LED_PIN, HIGH);
  }

  static uint32_t lastUpdate = 0;
  if (millis() - lastUpdate > 4000) {
    lastUpdate = millis();
    oled("T-Beam SOS", bleConnected ? "BLE: CONNECTAT" : "BLE: BUSCANT", getGPSInfo());
  }
}