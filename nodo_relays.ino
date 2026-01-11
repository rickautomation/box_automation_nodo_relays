#include <WiFi.h>              
#include <HTTPClient.h>        
#include <ArduinoJson.h>       
#include <Update.h>            
#include <WiFiClientSecure.h>  
#include <Preferences.h>        
#include <WebServer.h>          
#include <DNSServer.h>          

// ======================================================
// 0. VERSIÓN LOCAL DEL FIRMWARE
// ======================================================
const char* FIRMWARE_VERSION_CODE = "1.0.0";
String latestFirmwareVersion = FIRMWARE_VERSION_CODE;

// ======================================================
// 1. CONFIGURACIÓN DE RED Y FIREBASE
// ======================================================
const char* API_KEY = "AIzaSyAxGSXV2br1SsFu7YyP6NZaTXc_Z40uqA8"; 
const char* RTDB_HOST = "arduinoconfigremota-default-rtdb.firebaseio.com";                   
const char* DEFAULT_SSID = "tili";         
const char* DEFAULT_PASS = "Ubuntu1234$"; 

Preferences preferences;
WebServer server(80);
DNSServer dnsServer;

const char* PREFS_NAMESPACE = "wifi_config";
const char* PREF_SSID = "ssid";
const char* PREF_PASS = "pass";
const char* AP_SSID = "NODO_RELAYS_SETUP"; 

String loadedSsid = "";
String loadedPassword = "";
const int WIFI_RESET_PIN = 9; 

// ======================================================
// 2. CONFIGURACIÓN DINÁMICA
// ======================================================
String backendHost = "192.168.68.84";    
int backendPort = 3000;                  
String endpointRelays = "/relays/status/"; // Endpoint específico para múltiples relays
long intervaloConsultaMs = 3000;            
String remoteFirmwareVersion = "0.0.0"; 
String firmwareUrl = "";                 

const String RTDB_CONFIG_URL_BASE = "https://" + String(RTDB_HOST) + "/.json";
const char* NODE_TYPE_KEY = "NODO_RELAYS"; 

// ======================================================
// 3. HARDWARE RELAYS (ESP32-C3 Mini)
// ======================================================
String boxSerialId = "UNKNOWN"; 
const int NUM_RELAYS = 4;
const int relayPins[NUM_RELAYS] = {1, 2, 3, 4}; // In1, In2, In3, In4
bool relayStates[NUM_RELAYS] = {false, false, false, false};

const int RELAY_ON = LOW;  // Generalmente los módulos de relay activan con LOW
const int RELAY_OFF = HIGH;

// ======================================================
// 4. FUNCIONES
// ======================================================

void obtenerMacReal() {
    WiFi.mode(WIFI_STA);
    delay(100); 
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    boxSerialId = (mac != "000000000000") ? mac : "RELAY_" + String((uint32_t)ESP.getEfuseMac(), HEX);
}

bool conectar_wifi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.print(F("\n📡 Nodo Relays conectando..."));
  WiFi.begin(loadedSsid.c_str(), loadedPassword.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) { delay(500); Serial.print("."); }
  return (WiFi.status() == WL_CONNECTED);
}

void obtener_remote_config() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(RTDB_CONFIG_URL_BASE + "?auth=" + API_KEY);
  if (http.GET() == 200) {
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, http.getString());
    if(doc["remote_config"]["backend_host"]) backendHost = doc["remote_config"]["backend_host"].as<String>();
    if(doc["remote_config"]["backend_port"]) backendPort = doc["remote_config"]["backend_port"].as<int>();
    
    JsonObject fw = doc["firmware_updates"][NODE_TYPE_KEY];
    remoteFirmwareVersion = fw["latest_firmware_version"].as<String>();
    firmwareUrl = fw["firmware_url"].as<String>();
    Serial.println(F("✅ Config Remota Relays Sincronizada."));
  }
  http.end();
}

void consultar_servidor_flask() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = "http://" + backendHost + ":" + String(backendPort) + endpointRelays + boxSerialId;
  
  http.begin(url);
  int code = http.GET();
  if (code == 200) {
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, http.getString());
    
    // Esperamos un array de booleanos o un objeto con los estados
    // Ejemplo esperado: {"relays": [true, false, true, false]}
    JsonArray arr = doc["relays"].as<JsonArray>();
    for(int i=0; i < NUM_RELAYS; i++) {
        bool state = arr[i];
        if (state != relayStates[i]) {
            relayStates[i] = state;
            digitalWrite(relayPins[i], state ? RELAY_ON : RELAY_OFF);
            Serial.printf("🔌 Relay %d cambiado a: %s\n", i+1, state ? "ON" : "OFF");
        }
    }
  }
  http.end();
}

bool check_for_update() {
  if (remoteFirmwareVersion == "0.0.0" || remoteFirmwareVersion == latestFirmwareVersion) return false;
  Serial.println(F("🚀 Iniciando OTA en Nodo Relays..."));
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  if (http.begin(client, firmwareUrl)) {
    if (http.GET() == 200 && Update.begin(http.getSize())) {
      Update.writeStream(http.getStream());
      if (Update.end()) ESP.restart();
    }
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  preferences.begin(PREFS_NAMESPACE, false);
  pinMode(WIFI_RESET_PIN, INPUT_PULLUP);
  
  // Configurar los 4 pines de Relays
  for(int i=0; i < NUM_RELAYS; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], RELAY_OFF);
  }

  obtenerMacReal();
  Serial.println(F("\n--- ⚡ NODO 4 RELAYS (C3 MINI) ---"));
  Serial.print(F("🆔 ID: ")); Serial.println(boxSerialId);

  if (digitalRead(WIFI_RESET_PIN) == LOW) startConfigPortal(); 

  loadedSsid = preferences.getString(PREF_SSID, DEFAULT_SSID);
  loadedPassword = preferences.getString(PREF_PASS, DEFAULT_PASS);

  if (conectar_wifi()) {
    obtener_remote_config();
    check_for_update();
  }
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) conectar_wifi();

  unsigned long tiempoActual = millis();
  static unsigned long lastRun = 0;
  static unsigned long lastFirebase = 0;

  if (tiempoActual - lastRun >= intervaloConsultaMs) {
    lastRun = tiempoActual;
    consultar_servidor_flask();
  }

  if (tiempoActual - lastFirebase >= 60000) {
    lastFirebase = tiempoActual;
    obtener_remote_config();
    check_for_update();
  }
}

void startConfigPortal() {
  WiFi.softAP(AP_SSID);
  dnsServer.start(53, "*", IPAddress(192,168,4,1));
  server.on("/", []() {
    server.send(200, "text/html", "<h1>Portal Relays</h1><form method='POST' action='/save'>SSID: <input name='s'><br>Pass: <input name='p' type='password'><br><input type='submit'></form>");
  });
  server.on("/save", []() {
    preferences.putString(PREF_SSID, server.arg("s"));
    preferences.putString(PREF_PASS, server.arg("p"));
    server.send(200, "text/html", "Guardado. Reiniciando...");
    delay(2000); ESP.restart();
  });
  server.begin();
  while(1) { dnsServer.processNextRequest(); server.handleClient(); }
}