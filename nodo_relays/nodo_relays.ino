#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>



// ======================================================
// TELEMETRIA RTC
// ======================================================
#include <esp_system.h>
RTC_DATA_ATTR uint32_t rtc_boot_count = 0;
RTC_DATA_ATTR uint32_t rtc_wdt_resets = 0;
RTC_DATA_ATTR uint32_t rtc_wifi_disconnects = 0;
RTC_DATA_ATTR uint32_t rtc_http_errors = 0;

// ======================================================
// 0. VERSIÓN LOCAL DEL FIRMWARE
// ======================================================
const char* FIRMWARE_VERSION_CODE = "1.0.2";
String latestFirmwareVersion = FIRMWARE_VERSION_CODE;

// ======================================================
// 1. CONFIGURACIÓN DE RED Y FIREBASE
// ======================================================
const char* API_KEY = "AIzaSyAxGSXV2br1SsFu7YyP6NZaTXc_Z40uqA8";
const char* RTDB_HOST = "arduinoconfigremota-default-rtdb.firebaseio.com";

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
const int TIEMPO_MAX_CONEXION_WIFI = 30000;

// ======================================================
// 2. CONFIGURACIÓN DINÁMICA
// ======================================================
String backendHost = "192.168.68.58";
int backendPort = 3000;
String endpointRelays = "/relays/status/";
long intervaloConsultaMs = 3000;
String remoteFirmwareVersion = "0.0.0";
String firmwareUrl = "";

const String RTDB_CONFIG_URL_BASE = "https://" + String(RTDB_HOST) + "/.json";
const char* NODE_TYPE_KEY = "NODO_RELAYS";

// ======================================================
// 3. HARDWARE RELAYS (ESP32-C3 Super Mini)
// ======================================================
String boxSerialId = "UNKNOWN";
const int NUM_RELAYS = 8;
const int relayPins[NUM_RELAYS] = {1, 2, 3, 4, 5, 6, 7, 20};
bool relayStates[NUM_RELAYS] = {false, false, false, false, false, false, false, false};

// Referencia para logs; el control sigue siendo por índice desde el backend
const char* RELAY_LABELS[NUM_RELAYS] = {
  "luz", "extractor", "humidificador", "relay4", "relay5", "relay6", "ventilador_12v", "relay8"
};

const int RELAY_ON = LOW;
const int RELAY_OFF = HIGH;

// Pulso automático para electroválvula (-1 = desactivado)
const int VALVE_RELAY_INDEX = -1;
const unsigned long VALVE_PULSE_MS = 1000;
unsigned long valvePulseStartMs = 0;
bool valvePulseActive = false;

// ======================================================
// 4. DECLARACIONES
// ======================================================
void obtenerMacReal();
bool conectar_wifi();
void obtener_remote_config();
void consultar_servidor_flask();
bool check_for_update();
void setRelay(int index, bool on);
void applyRelayCommand(int index, bool requestedOn);
void processValvePulse();
void clearCredentials();
void saveCredentials(const String& ssid, const String& password);
bool loadCredentials();
void resetWifiStack();
bool probarCredencialesWifi(const String& ssid, const String& password, int& estadoFinal);
void startConfigPortal();
void handleRoot();
void handleSave();
void logMessage(String level, String msg);


// ======================================================
// 5. RELAYS
// ======================================================
void setRelay(int index, bool on) {
  if (index < 0 || index >= NUM_RELAYS) return;
  relayStates[index] = on;
  digitalWrite(relayPins[index], on ? RELAY_ON : RELAY_OFF);
  Serial.printf("🔌 Relay %d (%s): %s\n", index + 1, RELAY_LABELS[index], on ? "ON" : "OFF");
}

void applyRelayCommand(int index, bool requestedOn) {
  if (index < 0 || index >= NUM_RELAYS) return;

  if (index == VALVE_RELAY_INDEX && requestedOn && !relayStates[index]) {
    setRelay(index, true);
    valvePulseStartMs = millis();
    valvePulseActive = true;
    return;
  }

  if (requestedOn != relayStates[index]) {
    setRelay(index, requestedOn);
  }
}

void processValvePulse() {
  if (!valvePulseActive || VALVE_RELAY_INDEX < 0) return;
  if (millis() - valvePulseStartMs >= VALVE_PULSE_MS) {
    setRelay(VALVE_RELAY_INDEX, false);
    valvePulseActive = false;
  }
}

// ======================================================
// 6. RED Y CONFIG
// ======================================================
void obtenerMacReal() {
  WiFi.mode(WIFI_STA);
  delay(100);
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  boxSerialId = (mac != "000000000000") ? mac : "RELAY_" + String((uint32_t)ESP.getEfuseMac(), HEX);
}

void clearCredentials() {
  preferences.remove(PREF_SSID);
  preferences.remove(PREF_PASS);
  loadedSsid = "";
  loadedPassword = "";
}

void saveCredentials(const String& ssid, const String& password) {
  preferences.putString(PREF_SSID, ssid);
  preferences.putString(PREF_PASS, password);
  loadedSsid = ssid;
  loadedPassword = password;
  Serial.printf(F("💾 Credenciales guardadas: SSID = %s (pass_len=%d)\n"), ssid.c_str(), password.length());
}

bool loadCredentials() {
  loadedSsid = preferences.getString(PREF_SSID, "");
  loadedPassword = preferences.getString(PREF_PASS, "");
  loadedSsid.trim();
  loadedPassword.trim();
  if (loadedSsid.length() > 0) {
    Serial.printf(F("📝 Credenciales cargadas: SSID = %s (pass_len=%d)\n"), loadedSsid.c_str(), loadedPassword.length());
    return true;
  }
  return false;
}

void mantenerPortalAp() {
  IPAddress localIP(192, 168, 4, 1);
  WiFi.softAPConfig(localIP, localIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID);
}

void resetWifiStack() {
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(1000);
}

bool escanearSsid(const String& ssid) {
  Serial.println(F("🔍 Escaneando redes 2.4 GHz..."));
  int n = WiFi.scanNetworks(false, true);
  if (n <= 0) {
    Serial.println(F("⚠️ Escaneo sin resultados"));
    return false;
  }
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == ssid) {
      Serial.printf(F("📶 '%s' visible — RSSI: %d dBm, canal: %d\n"),
                    ssid.c_str(), WiFi.RSSI(i), WiFi.channel(i));
      return true;
    }
  }
  Serial.printf(F("⚠️ '%s' NO aparece en el escaneo\n"), ssid.c_str());
  Serial.println(F("   → Revisa nombre exacto, que sea 2.4 GHz y que el router esté cerca."));
  return false;
}

bool probarCredencialesWifi(const String& ssid, const String& password, int& estadoFinal) {
  Serial.printf(F("🔐 Probando SSID='%s' pass_len=%d\n"), ssid.c_str(), password.length());

  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true, true);
  delay(300);

  WiFi.mode(WIFI_AP_STA);
  mantenerPortalAp();
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  escanearSsid(ssid);

  WiFi.begin(ssid.c_str(), password.c_str());

  int ultimoEstado = -1;
  unsigned long inicio = millis();
  rtc_wifi_disconnects++;
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < TIEMPO_MAX_CONEXION_WIFI) {
    delay(300);
    dnsServer.processNextRequest();
    server.handleClient();

    int st = WiFi.status();
    if (st != ultimoEstado) {
      Serial.printf(F("   WiFi status: %d\n"), st);
      ultimoEstado = st;
    }
  }

  estadoFinal = WiFi.status();
  if (estadoFinal == WL_CONNECTED) {
    Serial.printf(F("✅ Prueba WiFi OK. IP: %s\n"), WiFi.localIP().toString().c_str());
    WiFi.disconnect(true);
    delay(200);
    mantenerPortalAp();
    return true;
  }

  Serial.printf(F("❌ Prueba WiFi falló. Estado final: %d\n"), estadoFinal);
  if (estadoFinal == WL_DISCONNECTED) {
    Serial.println(F("   Estado 6 = no conectó. Causas típicas: contraseña incorrecta o red 5 GHz."));
  } else if (estadoFinal == WL_IDLE_STATUS) {
    Serial.println(F("   Estado 0 = WiFi en idle. Reintenta; verifica SSID exacto."));
  }

  WiFi.disconnect(true);
  delay(200);
  mantenerPortalAp();
  return false;
}

bool conectar_wifi() {
  if (loadedSsid.length() == 0) return false;
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.printf(F("\n📡 Nodo Relays conectando a: %s\n"), loadedSsid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(loadedSsid.c_str(), loadedPassword.c_str());

  unsigned long start = millis();
  rtc_wifi_disconnects++;
  while (WiFi.status() != WL_CONNECTED && millis() - start < TIEMPO_MAX_CONEXION_WIFI) {
    esp_task_wdt_reset();
    delay(500);
    Serial.print(F("."));
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(F("\n✅ IP: %s\n"), WiFi.localIP().toString().c_str());
    return true;
  }

  Serial.printf(F("\n❌ WiFi falló. Estado: %d\n"), WiFi.status());
  return false;
}

void obtener_remote_config() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("⚠️ obtener_remote_config: WiFi no conectado, omitiendo."));
    return;
  }
  Serial.println(F("📥 Consultando Firebase remote_config..."));
  HTTPClient http;
  http.setTimeout(3000);
  http.begin(RTDB_CONFIG_URL_BASE + "?auth=" + API_KEY);
  int code = http.GET();
  if (code == 200) {
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, http.getString());
    if (err) {
      Serial.printf(F("❌ Firebase JSON inválido: %s\n"), err.c_str());
    } else {
      if (doc["remote_config"]["backend_host"]) backendHost = doc["remote_config"]["backend_host"].as<String>();
      if (doc["remote_config"]["backend_port"]) backendPort = doc["remote_config"]["backend_port"].as<int>();
      if (doc["remote_config"]["intervalo_consulta_ms"]) intervaloConsultaMs = doc["remote_config"]["intervalo_consulta_ms"].as<long>();

      JsonObject fw = doc["firmware_updates"][NODE_TYPE_KEY];
      if (!fw.isNull()) {
        remoteFirmwareVersion = fw["latest_firmware_version"].as<String>();
        firmwareUrl = fw["firmware_url"].as<String>();
      }
      Serial.println(F("✅ Config Remota Relays Sincronizada."));
      Serial.printf(F("   backend: %s:%d | intervalo: %lu ms | fw remoto: %s\n"),
                    backendHost.c_str(), backendPort, intervaloConsultaMs, remoteFirmwareVersion.c_str());
    }
  } else {
    rtc_http_errors++;
    Serial.printf(F("❌ Firebase GET falló. HTTP %d\n"), code);
  }
  http.end();
}

void consultar_servidor_flask() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("⚠️ consultar_servidor_flask: WiFi no conectado, omitiendo."));
    return;
  }
  String url = "http://" + backendHost + ":" + String(backendPort) + endpointRelays + boxSerialId;

  HTTPClient http;
  http.begin(url);
  http.setTimeout(3000);
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    static String lastBody = "";
    bool bodyChanged = body != lastBody;
    if (bodyChanged) {
      lastBody = body;
      Serial.printf(F("✅ Backend OK — body: %s\n"), body.c_str());
    }

    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, body);
    if (!err) {
      JsonArray arr = doc["relays"].as<JsonArray>();
      if (!arr.isNull()) {
        bool requested[NUM_RELAYS];
        for (int i = 0; i < NUM_RELAYS; i++) {
          requested[i] = (i < (int)arr.size()) ? arr[i].as<bool>() : false;
        }

        if (bodyChanged) {
          Serial.print(F("   decodificado: ["));
          for (int i = 0; i < NUM_RELAYS; i++) {
            Serial.print(requested[i] ? '1' : '0');
            if (i < NUM_RELAYS - 1) Serial.print(',');
          }
          Serial.println(']');
        }

        int cambios = 0;
        for (int i = 0; i < NUM_RELAYS; i++) {
          if (requested[i] != relayStates[i]) {
            cambios++;
            applyRelayCommand(i, requested[i]);
            // Pequeño retraso de 150ms entre relés para evitar un pico de 
            // corriente masivo que dispare el Brownout Detector
            esp_task_wdt_reset();
            delay(150); 
          }
        }

        if (bodyChanged && cambios == 0) {
          Serial.println(F("   → sin cambios en GPIO (estado ya coincide)"));
        } else if (cambios > 0) {
          Serial.printf(F("   → %d relay(s) actualizados\n"), cambios);
        }
      } else {
        Serial.printf(F("⚠️ Respuesta sin array 'relays'. Body: %s\n"), body.c_str());
      }
    } else {
      Serial.printf(F("⚠️ JSON inválido del backend: %s | Body: %s\n"), err.c_str(), body.c_str());
    }
  } else {
    rtc_http_errors++;
    Serial.printf(F("⚠️ GET relays falló. HTTP %d (url: %s)\n"), code, url.c_str());
  }
  http.end();
}

bool check_for_update() {
  if (remoteFirmwareVersion == "0.0.0" || remoteFirmwareVersion == latestFirmwareVersion) return false;
  if (firmwareUrl.length() == 0) return false;

  Serial.printf(F("🚀 OTA Relays: %s -> %s\n"), FIRMWARE_VERSION_CODE, remoteFirmwareVersion.c_str());
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (http.begin(client, firmwareUrl)) {
    if (http.GET() == 200 && Update.begin(http.getSize())) {
      Update.writeStream(http.getStream());
      if (Update.end()) ESP.restart();
    }
  }
  return false;
}

// ======================================================
// 7. PORTAL CAUTIVO
// ======================================================
void handleRoot() {
  String html = R"raw(
<!DOCTYPE html>
<html lang="es">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1">
  <title>Nodo Relays</title>
  <style>
    * { box-sizing: border-box; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      margin: 0; padding: 16px;
      background: #0a0a0a; color: #e8e8e8;
      min-height: 100vh; display: flex; align-items: center; justify-content: center;
    }
    .card {
      width: 100%; max-width: 420px;
      background: #141414; border: 1px solid #1f3d24;
      border-radius: 16px; padding: 24px 20px;
      box-shadow: 0 8px 32px rgba(0, 200, 83, 0.12);
    }
    .logo { font-size: 40px; text-align: center; margin-bottom: 8px; }
    h1 { color: #00e676; font-size: 22px; text-align: center; margin: 0 0 8px 0; }
    .sub { text-align: center; color: #9e9e9e; font-size: 14px; margin-bottom: 20px; line-height: 1.4; }
    label { display: block; color: #00c853; font-size: 13px; font-weight: 600; margin: 12px 0 6px 0; }
    input[type="text"], input[type="password"] {
      width: 100%; padding: 14px 12px; font-size: 16px;
      background: #0a0a0a; color: #fff;
      border: 1px solid #2e7d32; border-radius: 10px;
    }
    input:focus { outline: none; border-color: #00e676; box-shadow: 0 0 0 2px rgba(0,230,118,0.2); }
    .btn {
      width: 100%; margin-top: 20px; padding: 15px;
      background: linear-gradient(135deg, #00c853, #2e7d32);
      color: #000; font-size: 17px; font-weight: 700;
      border: none; border-radius: 10px; cursor: pointer;
    }
    .hint {
      margin-top: 16px; padding: 10px; border-radius: 8px;
      background: #1a1a1a; border-left: 3px solid #00e676;
      font-size: 12px; color: #bdbdbd; line-height: 1.5;
    }
    .footer { text-align: center; margin-top: 18px; font-size: 12px; color: #616161; }
  </style>
</head>
<body>
  <div class="card">
    <div class="logo">⚡</div>
    <h1>Nodo Relays</h1>
    <p class="sub">Configura la red Wi-Fi para controlar 8 relays (luz, extractor, humidificador, ventilador 12V).</p>
    <form method="POST" action="/save" enctype="application/x-www-form-urlencoded">
      <label for="ssid">Nombre de la red (SSID)</label>
      <input type="text" id="ssid" name="ssid" required placeholder="MiRedWiFi" autocomplete="off" autocapitalize="none" spellcheck="false">
      <label for="password">Contraseña Wi-Fi</label>
      <input type="password" id="password" name="password" placeholder="Contraseña de la red" autocomplete="new-password" autocapitalize="none">
      <input class="btn" type="submit" value="Probar y Guardar">
    </form>
    <div class="hint">
      Usa red <strong>2.4 GHz</strong>. Mantén <strong>BOOT</strong> al encender para borrar credenciales y volver aquí.
    </div>
    <div class="footer">Firmware v)raw" + String(FIRMWARE_VERSION_CODE) + R"raw( · 8 relays</div>
  </div>
</body>
</html>
)raw";
  server.send(200, "text/html", html);
}

void handleSave() {
  String newSsid = server.arg("ssid");
  String newPassword = server.arg("password");
  if (newPassword.length() == 0) newPassword = server.arg("pass");
  if (newPassword.length() == 0) newPassword = server.arg("p");
  newSsid.trim();
  newPassword.trim();

  Serial.printf(F("📥 Portal recibió: SSID='%s', pass_len=%d\n"), newSsid.c_str(), newPassword.length());

  if (newSsid.length() == 0) {
    server.send(400, "text/html", "<html><body style='background:#0a0a0a;color:#fff;text-align:center;padding:40px;'><h1 style='color:#ff5252;'>SSID vacío</h1><a style='color:#00e676;' href='/'>Volver</a></body></html>");
    return;
  }

  int estadoFinal = WL_DISCONNECTED;
  bool conecto = probarCredencialesWifi(newSsid, newPassword, estadoFinal);

  if (!conecto) {
    String errorHtml = R"raw(
<!DOCTYPE html>
<html lang="es"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Error WiFi</title>
<style>body{font-family:sans-serif;background:#0a0a0a;color:#e8e8e8;text-align:center;padding:32px 20px;}
h1{color:#ff5252;}a{display:inline-block;margin-top:20px;padding:14px 24px;background:#00c853;color:#000;text-decoration:none;border-radius:10px;font-weight:700;}</style>
</head><body><h1>❌ No se pudo conectar</h1><p>Estado WiFi: )raw" + String(estadoFinal) + R"raw(</p><a href='/'>Intentar de nuevo</a></body></html>)raw";
    server.send(200, "text/html", errorHtml);
    WiFi.mode(WIFI_AP_STA);
    mantenerPortalAp();
    return;
  }

  saveCredentials(newSsid, newPassword);
  preferences.end();
  server.send(200, "text/html", "<html><body style='background:#0a0a0a;color:#fff;text-align:center;padding:40px;'><h1 style='color:#00e676;'>✅ Guardado</h1><p>Reiniciando...</p></body></html>");
  delay(1500);
  ESP.restart();
}

void startConfigPortal() {
  resetWifiStack();
  WiFi.mode(WIFI_AP);
  mantenerPortalAp();
  Serial.printf(F("📡 Portal activo. Red: '%s' → http://192.168.4.1\n"), AP_SSID);
  dnsServer.start(53, "*", IPAddress(192, 168, 4, 1));
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound([]() {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
  });
  server.begin();
  unsigned long portalStart = millis();
  while (millis() - portalStart < 180000) { // 3 minutos de timeout
    esp_task_wdt_reset();
    dnsServer.processNextRequest();
    server.handleClient();
    delay(1);
  }
  Serial.println(F("⏳ Timeout del Portal Cautivo (3 min). Reiniciando nodo para buscar Wi-Fi..."));
  delay(1000);
  ESP.restart();

}

// ======================================================
// 8. SETUP / LOOP
// ======================================================

void sendTelemetry() {
  if (WiFi.status() == WL_CONNECTED && backendHost != "") {
    HTTPClient http;
    String url = "http://" + backendHost + ":" + String(backendPort) + "/api/health/metrics";
    http.begin(url);
    http.setTimeout(3000);
    http.addHeader("Content-Type", "application/json");
    String jsonStr = "{\"boxSerialId\":\"" + boxSerialId + "\",";
    jsonStr += "\"boot_count\":" + String(rtc_boot_count) + ",";
    jsonStr += "\"wdt_resets\":" + String(rtc_wdt_resets) + ",";
    jsonStr += "\"wifi_disconnects\":" + String(rtc_wifi_disconnects) + ",";
    jsonStr += "\"http_errors\":" + String(rtc_http_errors) + "}";
    http.POST(jsonStr);
    http.end();
  }
}

void setup() {
  Serial.begin(115200);

  esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_POWERON) {
    rtc_boot_count = 0;
    rtc_wdt_resets = 0;
    rtc_wifi_disconnects = 0;
    rtc_http_errors = 0;
  } else if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT || reason == ESP_RST_PANIC) {
    rtc_wdt_resets++;
  }
  rtc_boot_count++;

  
  // Iniciar Hardware Watchdog (30 segundos) - API v3.x
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 30000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_err_t err = esp_task_wdt_init(&wdt_config);
  if (err == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&wdt_config);
  }
  esp_task_wdt_add(NULL);

  preferences.begin(PREFS_NAMESPACE, false);
  pinMode(WIFI_RESET_PIN, INPUT_PULLUP);
  delay(100);


  for (int i = 0; i < NUM_RELAYS; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], RELAY_OFF);
  }

  resetWifiStack();
  obtenerMacReal();

  Serial.printf(F("\n--- ⚡ NODO 8 RELAYS (C3 MINI) v%s ---\n"), FIRMWARE_VERSION_CODE);
  Serial.print(F("🆔 ID: "));
  Serial.println(boxSerialId);
  Serial.println(F("📋 Mapa: 1=luz 2=extractor 3=humidificador 4-6=config 7=ventilador_12v 8=config"));

  if (digitalRead(WIFI_RESET_PIN) == LOW) {
    Serial.println(F("🚨 BOOT detectado. Borrando credenciales..."));
    clearCredentials();
    startConfigPortal();
  }

  if (!loadCredentials()) {
    Serial.println(F("📡 Sin credenciales. Abriendo portal..."));
    startConfigPortal();
  }

  if (conectar_wifi()) {
    ArduinoOTA.begin();
    logMessage("INFO", "🚀 Setup: WiFi OK, sincronizando config y consultando relays...");
    obtener_remote_config();
    consultar_servidor_flask();
    check_for_update();
    logMessage("INFO", "✅ Setup completo. Entrando al loop principal.");
  } else {

    Serial.println(F("❌ WiFi falló. Abriendo portal..."));
    startConfigPortal();
  }
}

void loop() {
  esp_task_wdt_reset();
  ArduinoOTA.handle();

  processValvePulse();


  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("⚠️ WiFi desconectado, reintentando..."));
    rtc_wifi_disconnects++;
    if (!conectar_wifi()) return;
    Serial.println(F("✅ WiFi reconectado."));
  }

  unsigned long tiempoActual = millis();

  static unsigned long lastTelemetry = 0;
  if (tiempoActual - lastTelemetry >= 3600000 || lastTelemetry == 0) { // 1 hora o al iniciar
    lastTelemetry = tiempoActual == 0 ? 1 : tiempoActual;
    sendTelemetry();
  }

  static unsigned long lastRun = 0;
  static unsigned long lastFirebase = 0;
  static unsigned long lastHeartbeat = 0;

  if (tiempoActual - lastRun >= intervaloConsultaMs) {
    lastRun = tiempoActual;
    consultar_servidor_flask();
  }

  if (tiempoActual - lastFirebase >= 60000) {
    lastFirebase = tiempoActual;
    obtener_remote_config();
    check_for_update();
  }

  if (tiempoActual - lastHeartbeat >= 30000) {
    lastHeartbeat = tiempoActual;
    Serial.printf(F("💓 heartbeat | uptime: %lu s | WiFi: %d | próxima consulta en ~%lu ms\n"),
                  tiempoActual / 1000, WiFi.status(),
                  intervaloConsultaMs - (tiempoActual - lastRun));
  }
}

void logMessage(String level, String msg) {
  Serial.println("[" + level + "] " + msg);
  if (WiFi.status() == WL_CONNECTED && backendHost != "") {
    HTTPClient http;
    String url = "http://" + backendHost + ":" + String(backendPort) + "/sensor-data/logs";
    http.begin(url);
    http.setTimeout(3000);
    http.addHeader("Content-Type", "application/json");
    
    DynamicJsonDocument doc(512);
    doc["boxSerialId"] = boxSerialId;
    doc["level"] = level;
    doc["message"] = msg;
    
    String jsonStr;
    serializeJson(doc, jsonStr);
    http.POST(jsonStr);
    http.end();
  }
}

