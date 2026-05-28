#include <Arduino.h>
#include <Wire.h>               // Librería para comunicación I2C (necesaria para la pantalla y el chip de energía)
#include <SSD1306Wire.h>        // Librería para controlar la pantalla OLED
#include <BLEDevice.h>          // Librerías base para manejar Bluetooth Low Energy (BLE)
#include <BLEServer.h>          // Permite que el ESP32 actúe como servidor BLE
#include <BLEUtils.h>           // Utilidades extra para BLE
#include <BLE2902.h>            // Descriptor necesario para que el móvil reciba notificaciones BLE
#include <XPowersLib.h>         // Librería para leer la batería desde el chip de gestión de energía (AXP)
#include <TinyGPS++.h>          // Librería para descifrar los datos en crudo que envía el módulo GPS

// ---------- DEFINICIÓN DE PINES Y CONSTANTES ----------
#define BUTTON_PIN 38    // Pin físico donde está conectado el botón central
#define LED_PIN    4     // Pin del LED azul de usuario integrado en la placa
#define OLED_SDA   21    // Pin de datos (SDA) para la pantalla OLED por I2C
#define OLED_SCL   22    // Pin de reloj (SCL) para la pantalla OLED por I2C
#define OLED_ADDR  0x3C  // Dirección I2C estándar de estas pantallas OLED

#define GPS_RX     34    // Pin por donde el ESP32 RECIBE datos del GPS
#define GPS_TX     12    // Pin por donde el ESP32 ENVÍA datos al GPS (poco usado aquí)
#define GPS_BAUD   9600  // Velocidad de comunicación de la antena GPS

#define SCREEN_WIDTH 128 // Ancho de la pantalla OLED en píxeles
#define SCREEN_HEIGHT 64 // Alto de la pantalla OLED en píxeles

// Los UUID son identificadores únicos universales. 
// Le dicen a la app del móvil (como nRF Connect) qué "servicio" y "característica" estamos ofreciendo.
#define SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define CHARACTERISTIC_UUID "abcdefab-1234-5678-1234-abcdefabcdef"

// ---------- CREACIÓN DE OBJETOS GLOBALES ----------
SSD1306Wire display(OLED_ADDR, OLED_SDA, OLED_SCL); // Objeto para controlar la pantalla
BLEServer *pServer = nullptr;                       // Puntero al servidor BLE
BLECharacteristic *pCharacteristic = nullptr;       // Puntero a la característica (por donde enviamos datos)
bool deviceConnected = false;                       // Variable que guarda si hay un móvil conectado o no

XPowersLibInterface *power;                         // Objeto genérico para el chip de energía
TinyGPSPlus gps;                                    // Objeto que procesará las frases del GPS
HardwareSerial gpsSer(1);                           // Puerto serie por hardware (Serial 1) dedicado al GPS

// ---------- CALLBACKS DE BLUETOOTH ----------
// Esta clase "escucha" los eventos del Bluetooth. Se dispara automáticamente cuando alguien se conecta o desconecta.
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    deviceConnected = true; // Si alguien se conecta, marcamos la variable como verdadera
    Serial.println("BLE Client connectat");
  }
  void onDisconnect(BLEServer* pServer) override { 
    deviceConnected = false; // Si se desconectan, marcamos como falsa
    Serial.println("BLE Client desconnectat");
    pServer->startAdvertising(); // Volvemos a emitir la señal BLE para que otros nos puedan encontrar
  }
};

// ---------- FUNCIÓN PARA ACTUALIZAR LA PANTALLA ----------
// Recibe hasta 3 líneas de texto. Si no le pasas la línea 2 o 3, simplemente las deja en blanco.
void oledMsg(const char* line1, const char* line2 = "", const char* line3 = "") {
  display.clear(); // Borra lo que haya en la pantalla
  display.setFont(ArialMT_Plain_10); // Selecciona la fuente y el tamaño
  display.setTextAlignment(TEXT_ALIGN_LEFT); // Alinea el texto a la izquierda
  
  int y = 10; // Coordenada vertical inicial
  display.drawString(0, y, line1); // Dibuja la primera línea
  y += 15; // Baja 15 píxeles
  
  if (strlen(line2) > 0) { // Si la línea 2 tiene texto, la dibuja
    display.drawString(0, y, line2);
    y += 15; // Baja otros 15 píxeles
  }
  if (strlen(line3) > 0) { // Si la línea 3 tiene texto, la dibuja
    display.drawString(0, y, line3);
  }
  display.display(); // Envía los datos a la pantalla física para que los muestre
}

// ---------- FUNCIONES DE GPS Y BATERÍA ----------
// Comprueba si el GPS tiene datos reales y no basura. Necesita al menos 4 satélites para ser preciso.
bool hasFix() { 
  return gps.location.isValid() && gps.satellites.value() >= 4; 
}

// Devuelve un String formateado con la latitud, longitud y la hora actual
String getGPSInfo() {
  if (!hasFix()) return "Buscant satelits..."; // Si no hay conexión satelital, avisa
  
  char buf[40]; // Creamos un espacio temporal en memoria para montar la frase
  // Formateamos las variables del GPS en el texto (%.5f = float de 5 decimales, %02d = entero de 2 dígitos)
  snprintf(buf, sizeof(buf), "%.5f,%.5f %02d:%02d:%02d", 
            gps.location.lat(), gps.location.lng(), 
            gps.time.hour(), gps.time.minute(), gps.time.second());
  return String(buf);
}

// Lee el porcentaje de batería desde el chip AXP
String getPowerInfo() {
  // Comprueba que el chip existe y que la batería está físicamente conectada
  if (power && power->isBatteryConnect()) {
    return String(power->getBatteryPercent()) + "%";
  }
  return "NO_BAT"; // Si usas el T-Beam solo con USB sin batería 18650
}

// ---------- CONFIGURACIÓN DEL BLUETOOTH ----------
void setupBLE() {
  BLEDevice::init("TBeam-SOS-Álavaro-Toni"); // Inicializa el chip BLE y le pone el nombre que verás en el móvil
  pServer = BLEDevice::createServer(); // Crea el servidor
  pServer->setCallbacks(new MyServerCallbacks()); // Le asigna las reglas de conexión que creamos arriba
  
  BLEService *pService = pServer->createService(SERVICE_UUID); // Crea el servicio principal
  
  // Crea la característica por la que vamos a enviar los datos. 
  // PROPERTY_READ deja que el móvil lea el dato, NOTIFY permite que el ESP32 le mande alertas al móvil automáticamente.
  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY
  );
  
  pCharacteristic->addDescriptor(new BLE2902()); // Descriptor obligatorio para que funcionen las notificaciones en iOS/Android
  pCharacteristic->setValue("READY"); // Valor inicial por defecto
  
  pService->start(); // Arranca el servicio
  
  // Configura y arranca el "anuncio" (advertising) para que el dispositivo sea visible
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06); // Configuración recomendada para dispositivos Apple
  BLEDevice::startAdvertising();
  
  Serial.println("BLE actiu - 'TBeam-SOS'");
}

// ---------- FUNCIÓN QUE SE EJECUTA AL PULSAR EL BOTÓN ----------
void sendSOS() {
  digitalWrite(LED_PIN, LOW); // Enciende el LED azul (funciona con lógica invertida, LOW es encendido)
  
  static int sosCount = 0; // Variable que guarda cuántas veces se ha pulsado (no se borra al salir de la función)
  sosCount++;
  
  // Monta el mensaje final juntando la base del profe + ubicación GPS + batería
  String baseMsg = "SOS|" + String(sosCount) + "|TS=" + String(millis()/1000) + "|TBEAM01";
  String finalMsg = baseMsg + "|" + (hasFix() ? getGPSInfo() : "NO_GPS") + "|" + getPowerInfo();
  
  Serial.println("SOS #" + String(sosCount) + ": " + finalMsg); // Lo imprime por el monitor serie
  
  // Si hay un móvil conectado, se lo enviamos
  if (deviceConnected && pCharacteristic) {
    pCharacteristic->setValue(finalMsg.c_str()); // Carga el mensaje en la característica
    pCharacteristic->notify(); // Dispara la notificación al móvil
    Serial.println("Enviat per BLE!");
    oledMsg("SOS ENVIAT!", ("#" + String(sosCount)).c_str(), "via BLE"); // Muestra el aviso en la pantalla
  } else {
    // Si no hay móvil, avisa de que el SOS se generó pero no se mandó a nadie
    Serial.println("!!!!Sense client BLE!!!!");
    oledMsg("SOS ENVIAT!", ("#" + String(sosCount)).c_str(), "Connecta app");
  }
  
  delay(500);  // Pausa medio segundo para que el usuario vea que el LED parpadea
  digitalWrite(LED_PIN, HIGH); // Apaga el LED azul
  
  // Devuelve la pantalla a su estado normal mostrando la info del GPS
  oledMsg("T-Beam SOS", "Polsa botó", getGPSInfo().c_str()); 
}

// ---------- SETUP: SE EJECUTA UNA VEZ AL ENCENDER ----------
void setup() {
  Serial.begin(115200); // Inicia el monitor serie para ver logs en el PC
  delay(1000);
  Serial.println("=== T-BEAM SOS v3 + GPS + BAT ==="); 
  
  pinMode(LED_PIN, OUTPUT); // Configura el LED como salida
  digitalWrite(LED_PIN, HIGH); // Asegura que el LED empiece apagado
  pinMode(BUTTON_PIN, INPUT);  // Configura el pin del botón para leer pulsaciones
  
  Wire.begin(OLED_SDA, OLED_SCL); // Inicia el bus I2C. Muy importante hacerlo antes de inicializar la pantalla o la energía
  
  // Inicialización de la pantalla OLED
  display.init();
  display.flipScreenVertically(); // Voltea la imagen (los T-Beam suelen llevar la pantalla al revés)
  display.setFont(ArialMT_Plain_10);
  oledMsg("T-Beam SOS v3", "Iniciant calul...", "Espera..."); 
  
  // Inicialización del chip de energía de la batería
  // --- Inicializar Chip de Energía (Soporta AXP2101 o AXP192) CON PROTECCIÓN ---
  power = new XPowersAXP2101(Wire, OLED_SDA, OLED_SCL);
  if (!power->init()) { 
    delete power; 
    power = new XPowersAXP192(Wire, OLED_SDA, OLED_SCL); 
    if (!power->init()) {
      delete power;
      power = nullptr; // <-- ESTO FALTABA: Si ambos fallan, lo anulamos para evitar el cuelgue
      Serial.println("Avís: No s'ha trobat xip de bateria AXP");
    }
  }
  
  if (power) {
    power->enableBattVoltageMeasure(); 
  }

  // Inicialización del GPS
  gpsSer.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX); // Inicia el puerto serie dedicado solo a hablar con la antena GPS

  setupBLE(); // Llama a la función que configura el Bluetooth
  
  Serial.println("PREPARAT!");
  Serial.println("- Cada polsació = 1 SOS");
  Serial.println("- nRF Connect -> TBeam-SOS");
  Serial.println("- Notificacions ON");
  
  oledMsg("T-Beam SOS v3", "Polsa botó", "BLE preparat"); // Mensaje de bienvenida final
}

// ---------- LOOP: SE EJECUTA EN BUCLE INFINITO ----------
void loop() {
  // 1. LEER GPS CONSTANTEMENTE
  // Mientras haya datos crudos llegando, se los pasamos a la librería
  while (gpsSer.available()) gps.encode(gpsSer.read());

  // 2. LECTURA DEL BOTÓN (Filtro Anti-rebotes corregido)
  static bool buttonState = HIGH;             // Estado estable confirmado del botón
  static bool lastButtonState = HIGH;         // Última lectura (con posible ruido eléctrico)
  static unsigned long lastDebounceTime = 0;  // Cronómetro del último cambio físico

  bool reading = digitalRead(BUTTON_PIN); // Leemos el pin físicamente

  // Si hay un cambio (alguien lo ha tocado o hay ruido)
  if (reading != lastButtonState) {
    lastDebounceTime = millis(); // Reiniciamos el cronómetro
  }

  // Si han pasado 50ms sin que la señal cambie (es una pulsación real y estable)
  if ((millis() - lastDebounceTime) > 50) {
    // Si este nuevo estado estable es distinto al que teníamos guardado
    if (reading != buttonState) {
      buttonState = reading; // Actualizamos el estado real del botón
      
      // Si el estado confirmado es LOW (presionado a fondo)
      if (buttonState == LOW) {
        Serial.println("PULSACIÓ DETECTADA -> ENVIA SOS!");
        sendSOS();
      }
    }
  }
  
  lastButtonState = reading; // Guardamos la lectura para el siguiente ciclo del loop

  // 3. ACTUALIZACIÓN PERIÓDICA DE LA PANTALLA
  static unsigned long lastStatus = 0; 
  
  // Si han pasado 4000 milisegundos (4 segundos)...
  if (millis() - lastStatus > 4000) {
    lastStatus = millis(); 
    
    // Muestra en pantalla el estado del Bluetooth y la ubicación/hora del GPS
    if (deviceConnected) {
      oledMsg("T-Beam SOS", "BLE CONNECTAT", getGPSInfo().c_str()); 
    } else {
      oledMsg("T-Beam SOS", "BLE BUSCANT", getGPSInfo().c_str());
    }
  }
}