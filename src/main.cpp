#include <Arduino.h>
#include <Wire.h>
#include <SSD1306Wire.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define BUTTON_PIN 38    // Botó 
#define LED_PIN    4     // LED usuari
#define OLED_SDA   21
#define OLED_SCL   22
#define OLED_ADDR  0x3C

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define CHARACTERISTIC_UUID "abcdefab-1234-5678-1234-abcdefabcdef"

SSD1306Wire display(OLED_ADDR, OLED_SDA, OLED_SCL);
BLEServer *pServer = nullptr;
BLECharacteristic *pCharacteristic = nullptr;
bool deviceConnected = false;

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

void oledMsg(const char* line1, const char* line2 = "", const char* line3 = "") {
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

void setupBLE() {
  BLEDevice::init("TBeam-SOS");
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
  
  Serial.println("BLE actiu - 'TBeam-SOS'");
}

void sendSOS() {
  digitalWrite(LED_PIN, LOW);
  
  // Missatge SOS amb contador i timestamp
  static int sosCount = 0;
  sosCount++;
  String msg = "SOS|" + String(sosCount) + "|TS=" + String(millis()/1000) + "|TBEAM01";
  
  Serial.println("SOS #" + String(sosCount) + ": " + msg);
  
  if (deviceConnected && pCharacteristic) {
    pCharacteristic->setValue(msg.c_str());
    pCharacteristic->notify();
    Serial.println("Enviat per BLE!");
    oledMsg("SOS ENVIAT!", ("#" + String(sosCount)).c_str(), "via BLE");
  } else {
    Serial.println("!!!!Sense client BLE!!!!");
    oledMsg("SOS ENVIAT!", ("#" + String(sosCount)).c_str(), "Connecta app");
  }
  
  delay(500);  // Feedback visual curt
  digitalWrite(LED_PIN, HIGH);
  
  // Estat normal
  oledMsg("T-Beam SOS", "Polsa botó", "BLE connectat");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("=== T-BEAM SOS v3 (cada polsació) ===");
  
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  pinMode(BUTTON_PIN, INPUT);
  
  // OLED SSD1306Wire
  display.init();
  display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);
  oledMsg("T-Beam SOS v3", "Sense GPS", "Iniciant...");
  
  setupBLE();
  
  Serial.println("PREPARAT!");
  Serial.println("- Cada polsació = 1 SOS");
  Serial.println("- nRF Connect → TBeam-SOS");
  Serial.println("- Notificacions ON");
  
  oledMsg("T-Beam SOS v3", "Polsa botó", "BLE preparat");
}

void loop() {
  // Detecció polsació simple (sense long press)
  static bool lastButtonState = HIGH;
  static unsigned long lastDebounceTime = 0;
  bool reading;

  reading = digitalRead(BUTTON_PIN);
  // Canvi estat amb anti-rebote
  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }
  
  if ((millis() - lastDebounceTime) > 50) {  // Anti-rebote 50ms
    reading = digitalRead(BUTTON_PIN);
  
    if (reading != lastButtonState) {
      // PULSACIÓ DETECTADA → ENVIA SOS!
      Serial.println("PULSACIÓ DETECTADA → ENVIA SOS!");
      sendSOS();
      delay(300);  // Anti-repeticions ràpides
    }
  }
  
  lastButtonState = reading;

  // Estat BLE cada 4s
  static unsigned long lastStatus = 0;
  if (millis() - lastStatus > 4000) {
    lastStatus = millis();
    
    if (deviceConnected) {
      oledMsg("T-Beam SOS", "BLE CONNECTAT", "Polsa botó");
    } else {
      oledMsg("T-Beam SOS", "BLE BUSCANT", "BLE cercant");
    }
  }
}