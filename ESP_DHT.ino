#include "DHT.h"
#include <ESPAsyncWebServer.h>
#include <DNSServer.h>
#include "FS.h"
#include <ESP8266WiFiMulti.h>
#include <InfluxDbClient.h>
#include <InfluxDbCloud.h>
#include "secrets.h"

#define DEVICE "ESP8266"
#define DHTTYPE DHT22
#define DHTPIN 2
#define DELAY_INTERVAL_MS 1000 // Loop interval in ms
// #define READING_INTERVAL 5 * 60  // Intervalo entre leituras (segundos)
#define READING_INTERVAL 10
#define WIFI_CONNECT_TIMEOUT_MS_CUSTOM 15000
#define WIFI_CONFIG_FILE "/wifi.txt"
#define TZ_INFO "<-03>3"

ESP8266WiFiMulti wifiMulti;

AsyncWebServer server(80);
AsyncEventSource events("/events");
DNSServer dnsServer;
String ssidOptions = "";
String logBuffer = "";

void performWifiScan(const String& selectedSsid = "");

struct WifiCredentials {
  String ssid;
  String password;
};

bool apModeActive = true;
bool reconnectRequested = false;
unsigned long lastWifiReconnectAttempt = 0;
const unsigned long wifiReconnectIntervalMs = 30000;
bool wifiSaveRequested = false;
String pendingWifiSsid;
String pendingWifiPassword;
bool wifiReconnectScheduled = false;
unsigned long wifiReconnectAtMs = 0;
bool apConfigUpdateRequested = false;
unsigned long apConfigUpdateAtMs = 0;
String currentApSsid = WIFI_AP_SSID;
String currentApPassword = WIFI_AP_PASSWORD;
String grafanaURL = GRAFANA_URL;
bool apModeEnabled = true;
unsigned long currentReadingIntervalSeconds = READING_INTERVAL;
unsigned long lastSensorReadAt = 0;
bool factoryResetRequested = false;

InfluxDBClient client(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN, InfluxDbCloud2CACert);
Point sensor("temperature_humidity");
DHT dht(DHTPIN, DHTTYPE);

float h = NAN;
float t = NAN;
float hic = NAN;

void logOutput(const String& message, bool newline = true);

// Offline buffering
void storeOfflineData(float h, float t, float hic, time_t ts);
void resendOfflineData();
void resetRuntimeConfigToDefaults();

void performWifiScan(const String& selectedSsid) {
  ssidOptions = "";
  ssidOptions.reserve(1024);
  int n = WiFi.scanNetworks();
  if (n <= 0) {
    ssidOptions += "<option value=''>Nenhuma rede encontrada</option>";
  } else {
    for (int i = 0; i < n; ++i) {
      String ss = WiFi.SSID(i);
      int rssi = WiFi.RSSI(i);
      String display = ss + " (" + String(rssi) + " dBm)";
      String selectedAttr = (selectedSsid.length() > 0 && ss == selectedSsid) ? " selected" : "";
      ssidOptions += "<option value='" + ss + "'" + selectedAttr + ">" + display + "</option>";
    }
  }
}

WifiCredentials loadWifiCredentials() {
  WifiCredentials credentials;
  if (SPIFFS.exists(WIFI_CONFIG_FILE)) {
    File file = SPIFFS.open(WIFI_CONFIG_FILE, "r");
    if (file) {
      credentials.ssid = file.readStringUntil('\n');
      credentials.password = file.readStringUntil('\n');
      credentials.ssid.trim();
      credentials.password.trim();
      file.close();
    }
  }

  if (credentials.ssid.isEmpty()) {
    credentials.ssid = WIFI_SSID;
    credentials.password = WIFI_PASSWORD;
  }

  return credentials;
}

void loadApCredentials() {
  if (SPIFFS.exists(WIFI_CONFIG_FILE)) {
    File file = SPIFFS.open(WIFI_CONFIG_FILE, "r");
    if (file) {
      String wifiSsid = file.readStringUntil('\n');
      String wifiPassword = file.readStringUntil('\n');
      String apSsid = file.readStringUntil('\n');
      String apPassword = file.readStringUntil('\n');
      String apModeLine = file.readStringUntil('\n');
      String intervalLine = file.readStringUntil('\n');

      wifiSsid.trim();
      wifiPassword.trim();
      apSsid.trim();
      apPassword.trim();
      apModeLine.trim();
      intervalLine.trim();

      if (apSsid.length() > 0) {
        currentApSsid = apSsid;
      }

      if (apPassword.length() > 0) {
        currentApPassword = apPassword;
      }

      if (apModeLine.length() > 0) {
        apModeEnabled = apModeLine != "0";
      }

      if (intervalLine.length() > 0) {
        unsigned long savedInterval = intervalLine.toInt();
        if (savedInterval >= 5) {
          currentReadingIntervalSeconds = savedInterval;
        }
      }

      file.close();
    }
  }
}

bool saveWifiCredentials(const String& ssid, const String& password) {
  const int maxTries = 3;
  int tryCount = 0;
  bool ok = false;
  bool didFormat = false;

  while (tryCount < maxTries && !ok) {
    logOutput("Heap livre antes de salvar: " + String(ESP.getFreeHeap()));

    File file = SPIFFS.open(WIFI_CONFIG_FILE, "w");
    if (!file) {
      tryCount++;
      logOutput("Tentativa " + String(tryCount) + ": Falha ao abrir arquivo de configuracao Wi-Fi.");

      if (!didFormat) {
        logOutput("Tentando formatar SPIFFS e salvar novamente.");
        didFormat = true;
        SPIFFS.format();
      }

      delay(200);
      continue;
    }

    if (file.print(ssid) < 0) {
      logOutput("Erro escrevendo SSID no arquivo.");
      file.close();
      tryCount++;
      delay(200);
      continue;
    }

    if (file.print("\n") < 0) {
      logOutput("Erro escrevendo nova linha depois do SSID.");
      file.close();
      tryCount++;
      delay(200);
      continue;
    }

    if (file.print(password) < 0) {
      logOutput("Erro escrevendo senha no arquivo.");
      file.close();
      tryCount++;
      delay(200);
      continue;
    }

    if (file.print("\n") < 0) {
      logOutput("Erro escrevendo nova linha depois da senha.");
      file.close();
      tryCount++;
      delay(200);
      continue;
    }

    file.close();
    ok = true;
    logOutput("Heap livre apos salvar: " + String(ESP.getFreeHeap()));
  }

  if (!ok) {
    logOutput("Falha ao salvar configuracao Wi-Fi apos " + String(maxTries) + " tentativas.");
  } else {
    logOutput("Configuracao Wi-Fi salva.");
  }

  return ok;
}

bool saveAllCredentials(const String& wifiSsid, const String& wifiPassword, const String& apSsid, const String& apPassword, bool enableApMode, unsigned long readingIntervalSeconds) {
  const int maxTries = 3;
  int tryCount = 0;
  bool ok = false;
  bool didFormat = false;

  while (tryCount < maxTries && !ok) {
    File file = SPIFFS.open(WIFI_CONFIG_FILE, "w");
    if (!file) {
      tryCount++;
      logOutput("Tentativa " + String(tryCount) + ": Falha ao abrir arquivo de configuracao Wi-Fi/AP.");

      if (!didFormat) {
        logOutput("Tentando formatar SPIFFS e salvar novamente.");
        didFormat = true;
        SPIFFS.format();
      }

      delay(200);
      continue;
    }

    file.println(wifiSsid);
    file.println(wifiPassword);
    file.println(apSsid);
    file.println(apPassword);
    file.println(enableApMode ? "1" : "0");
    file.println(String(readingIntervalSeconds));
    file.close();
    ok = true;
  }

  if (!ok) {
    logOutput("Falha ao salvar configuracao AP apos " + String(maxTries) + " tentativas.");
  } else {
    logOutput("Configuracao AP salva.");
  }

  return ok;
}

String wifiStatusPage() {
  String html;
  html.reserve(1800);

  time_t now = time(nullptr);
  struct tm* timeInfo = localtime(&now);
  char sensorTime[20];
  strftime(sensorTime, sizeof(sensorTime), "%Y-%m-%d %H:%M:%S", timeInfo);

  String connectedSsid = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : loadWifiCredentials().ssid;
  String apIpText = apModeActive ? WiFi.softAPIP().toString() : String("n/a");
  String staMac = WiFi.macAddress();
  String apMacText = apModeActive ? WiFi.softAPmacAddress() : String("n/a");

  //HTML page
  html += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>ESP DHT Wi-Fi</title>";
  html += "<style>body{font-family:Arial,sans-serif;max-width:520px;margin:40px auto;padding:16px;background:#f4f7fb;color:#1f2937}";
  html += "h1{font-size:1.6rem} .card{background:#fff;border-radius:12px;padding:18px;box-shadow:0 8px 24px rgba(0,0,0,.08)}";
  html += "select,input,button,a{width:100%;padding:12px;margin:8px 0;box-sizing:border-box;border-radius:8px;border:1px solid #cbd5e1}";
  html += "button,a{display:block;text-align:center;text-decoration:none;background:#0f766e;color:#fff;border:none;font-weight:700}a{background:#334155}</style>";
  html += "</head><body><div class='card'><h1>ESP DHT Wi-Fi</h1>";

  // Sensor status
  html += "<hr><h2>Sensor status</h2>";
  html += "<p>Time: " + String(sensorTime) + "</p>";
  html += "<p>Humidity: " + String(h, 1) + "%</p>";
  html += "<p>Temperature: " + String(t, 1) + "&#x2103;</p>";
  html += "<p>Heat index: " + String(hic, 1) + "&#x2103;</p>";

  // WiFi Settings
  html += "<hr><h2>WiFi settings</h2>";
  html += "<p>Status: ";
  html += (WiFi.status() == WL_CONNECTED) ? "Connected" : "Not connected";
  html += "</p>";
  html += "<p>IP: " + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : apIpText) + "</p>";
  html += "<p>MAC (STA): " + staMac + "</p>";
  html += "<form method='POST' action='/save'>";
  html += "<label>SSID</label>";
  html += "<select name='ssid' required>";
  html += ssidOptions;
  html += "</select>";
  html += "<label>Password</label><input name='password' type='password' maxlength='64'>";
  html += "<button type='submit'>Save and connect</button>";
  html += "</form>";
  html += "<a href='/rescan'>Rescan</a>";

  // AP Settings
  html += "<hr><h2>AP settings</h2>";
  html += "<p>Status: ";
  html += apModeActive ? "Up" : "Down";
  html += "</p>";
  html += "<p>IP: ";
  html += apIpText;
  html += "</p>";
  html += "<p>MAC: ";
  html += apMacText;
  html += "</p>";
  html += "<form method='POST' action='/save'>";
  html += "<label>SSID</label><input name='ap_ssid' maxlength='32' value='" + currentApSsid + "'>";
  html += "<label>Password</label><input name='ap_password' type='password' maxlength='64' value='" + currentApPassword + "'>";
  html += "<label><input type='checkbox' name='ap_mode' value='1'";
  html += apModeEnabled ? " checked" : "";
  html += "> Active</label>";
  html += "<button type='submit'>Save AP settings</button>";
  html += "</form>";

  // ESP settings
  html += "<hr><h2>ESP settings</h2>";
  html += "<form method='POST' action='/save'>";
  html += "<label>Sensor reading interval (seconds)</label>";
  html += "<input name='reading_interval' type='number' min='5' max='86400' step='1' value='" + String(currentReadingIntervalSeconds) + "'>";
  html += "<button type='submit'>Save ESP settings</button>";
  html += "</form>";
  html += "<a href='/events'>Events</a>";
  html += "<a href='" + grafanaURL + "' target='_blank'>Grafana Dashboard</a>";
  html += "<form method='POST' action='/reset' onsubmit=\"return confirm('Reset all settings to defaults? This will overwrite saved Wi-Fi and AP settings. Continue?');\">";
  html += "<button type='submit' style='background:#dc2626'>Factory reset</button>";
  html += "</form>";
  html += "</div></body></html>";
  return html;
}

bool connectToWifi(const String& ssid, const String& password, unsigned long timeoutMs) {
  if (ssid.isEmpty()) {
    return false;
  }

  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(500);
  }

  return WiFi.status() == WL_CONNECTED;
}

void startConfigPortal(bool force = false) {
  if (!force && !apModeEnabled) {
    return;
  }

  if (apModeActive) {
    performWifiScan(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : loadWifiCredentials().ssid);
    return;
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(currentApSsid.c_str(), currentApPassword.c_str());

  IPAddress apIP = WiFi.softAPIP();
  dnsServer.start(53, "*", apIP);
  apModeActive = true;

  // Pre-scan available networks and cache options to avoid scanning inside the web handler
  performWifiScan(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : loadWifiCredentials().ssid);

  logOutput("AP started: " + currentApSsid);
  logOutput("Connect at " + apIP.toString() + " to setup.");
}

void setupWebServer() {
  server.addHandler(&events);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", wifiStatusPage());
  });

  server.on("/rescan", HTTP_GET, [](AsyncWebServerRequest *request) {
    performWifiScan(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : loadWifiCredentials().ssid);
    request->send(200, "text/plain", "OK");
  });

  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    String apSsid = currentApSsid;
    String apPassword = currentApPassword;
    bool enableApMode = apModeEnabled;
    unsigned long readingIntervalSeconds = currentReadingIntervalSeconds;

    WifiCredentials storedWifi = loadWifiCredentials();
    String ssid = storedWifi.ssid;
    String password = storedWifi.password;

    if (request->hasParam("ssid", true)) {
      ssid = request->getParam("ssid", true)->value();
    }

    if (request->hasParam("password", true)) {
      password = request->getParam("password", true)->value();
    }

    if (request->hasParam("ap_ssid", true)) {
      apSsid = request->getParam("ap_ssid", true)->value();
    }

    if (request->hasParam("ap_password", true)) {
      apPassword = request->getParam("ap_password", true)->value();
    }

    if (request->hasParam("reading_interval", true)) {
      String intervalValue = request->getParam("reading_interval", true)->value();
      intervalValue.trim();
      unsigned long parsedInterval = intervalValue.toInt();
      if (parsedInterval >= 5) {
        readingIntervalSeconds = parsedInterval;
      }
    }

    enableApMode = request->hasParam("ap_mode", true);

    ssid.trim();
    password.trim();
    apSsid.trim();
    apPassword.trim();

    if (apSsid.isEmpty()) {
      apSsid = currentApSsid;
    }

    if (apPassword.length() > 0 && apPassword.length() < 8) {
      request->send(400, "text/plain", "Senha do AP precisa ter pelo menos 8 caracteres.");
      return;
    }

    pendingWifiSsid = ssid;
    pendingWifiPassword = password;
    currentApSsid = apSsid;
    currentApPassword = apPassword;
    apModeEnabled = enableApMode;
    currentReadingIntervalSeconds = readingIntervalSeconds;
    wifiSaveRequested = true;
    apConfigUpdateRequested = true;
    apConfigUpdateAtMs = millis() + 1000;
    request->send(200, "text/html", "<html><meta http-equiv='refresh' content='10; url=/'><body><h2>Saving and aplying settings. Redirecting to the main page...</h2></body></html>");
  });

    server.on("/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
      resetRuntimeConfigToDefaults();
      logOutput("Factory reset requested via web UI.");
      request->send(200, "text/html", "<html><meta http-equiv='refresh' content='10; url=/'><body><h2>Resetting to defaults. Saving and redirecting...</h2></body></html>");
    });

  server.onNotFound([](AsyncWebServerRequest *request) {
    if (apModeActive) {
      request->redirect("/");
      return;
    }

    request->send(404, "text/plain", "Not found");
  });

  server.begin();
}

void handleWifiConnected() {
  timeSync(TZ_INFO, "pool.ntp.org", "time.nis.gov");

  if (client.validateConnection()) {
    logOutput("Conectado ao InfluxDB.");
  } else {
    logOutput("Erro InfluxDB: " + client.getLastErrorMessage());
  }
  // Attempt to resend any offline-stored measurements now that we're connected
  time_t now = time(nullptr);
  if (now > 1700000000) {
    resendOfflineData();
  }
}

void logOutput(const String& message, bool newline) {
  time_t now = time(nullptr);
  String logPrefix;

  if (now > 1700000000) {
    struct tm* timeInfo = localtime(&now);
    char timeStr[20];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", timeInfo);
    logPrefix = "[" + String(timeStr) + "] ";
  } else {
    logPrefix = "[uptime " + String(millis() / 1000) + "s] ";
  }

  String logMessage = logPrefix + message;

  if (newline) Serial.println(logMessage);
  else Serial.print(logMessage);

  events.send(logMessage.c_str(), "message");
}

void setup() {
  Serial.begin(74880);
  if (!SPIFFS.begin()) {
    logOutput("SPIFFS failed to start.");
    return;
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);

  setupWebServer();

  logOutput("MAC Wi-Fi: " + WiFi.macAddress());
  logOutput("Conectando ao Wi-Fi...");

  loadApCredentials();

  if (apModeEnabled) {
    startConfigPortal();
  }

  WifiCredentials credentials = loadWifiCredentials();
  if (connectToWifi(credentials.ssid, credentials.password, WIFI_CONNECT_TIMEOUT_MS_CUSTOM)) {
    logOutput("Conectado ao SSID '" + WiFi.SSID() + "'! IP: " + WiFi.localIP().toString());
    performWifiScan(WiFi.SSID());
    handleWifiConnected();
  } else {
    logOutput("Falha na conexao. Iniciando portal de configuracao.");
    startConfigPortal(true);
  }

  sensor.addTag("device", DEVICE);
  dht.begin();
}

void loop() {
  if (apModeActive) {
    dnsServer.processNextRequest();
  }

  if (wifiSaveRequested) {
    wifiSaveRequested = false;

    if (saveAllCredentials(pendingWifiSsid, pendingWifiPassword, currentApSsid, currentApPassword, apModeEnabled, currentReadingIntervalSeconds)) {
      wifiReconnectScheduled = true;
      wifiReconnectAtMs = millis() + 3000;
    } else {
      logOutput("Configuracao nao foi salva; permanecendo no portal.");
      if (!apModeActive) {
        startConfigPortal();
      }
    }

    pendingWifiSsid = "";
    pendingWifiPassword = "";
  }

  if (apConfigUpdateRequested && millis() >= apConfigUpdateAtMs) {
    apConfigUpdateRequested = false;
    if (apModeActive) {
      WiFi.softAPdisconnect(true);
      apModeActive = false;
    }
    if (apModeEnabled) {
      startConfigPortal(true);
    }
  }

  sensor.clearFields();

  h = NAN;
  t = NAN;
  hic = NAN;
  bool sensorOk = false;

  for (int attempt = 0; attempt < 3; ++attempt) {
    delay(250);
    h = dht.readHumidity();
    t = dht.readTemperature();

    if (!isnan(h) && !isnan(t)) {
      hic = dht.computeHeatIndex(t, h, false);
      sensorOk = true;
      break;
    }
  }

  if (!sensorOk) {
    logOutput("Erro ao ler sensor (h=" + String(h) + ", t=" + String(t) + ").");
  } else {
    sensor.addField("h", h);
    sensor.addField("t", t);
    sensor.addField("hic", hic);

    logOutput("Sending: h=" + String(h) + " t=" + String(t) + " hic=" + String(hic));
  }

  if (wifiReconnectScheduled && millis() >= wifiReconnectAtMs && WiFi.status() != WL_CONNECTED) {
    wifiReconnectScheduled = false;

    WifiCredentials credentials = loadWifiCredentials();
    if (!connectToWifi(credentials.ssid, credentials.password, WIFI_CONNECT_TIMEOUT_MS_CUSTOM)) {
      logOutput("Ainda no portal de configuracao enquanto tenta conectar ao novo Wi-Fi.");
      if (apModeEnabled) {
        startConfigPortal(true);
      }
    } else {
      logOutput("Wi-Fi reconectado. IP: " + WiFi.localIP().toString());
      performWifiScan(WiFi.SSID());
      handleWifiConnected();
      // Try resending any buffered offline data
      time_t now = time(nullptr);
      if (now > 1700000000) resendOfflineData();
    }
  }

  if (sensorOk && WiFi.status() != WL_CONNECTED) {
    if (!apModeActive && millis() - lastWifiReconnectAttempt > wifiReconnectIntervalMs) {
      lastWifiReconnectAttempt = millis();
      WifiCredentials credentials = loadWifiCredentials();
      if (!connectToWifi(credentials.ssid, credentials.password, WIFI_CONNECT_TIMEOUT_MS_CUSTOM)) {
        startConfigPortal(true);
      } else {
        logOutput("Wi-Fi reconectado. IP: " + WiFi.localIP().toString());
        performWifiScan(WiFi.SSID());
        handleWifiConnected();
      }
    }

    logOutput("Wi-Fi indisponivel. Dados nao serao armazenados sem horario valido.");
  } else if (sensorOk) {
    time_t now = time(nullptr);
    // Require a valid timestamp before storing data
    if (now <= 1700000000) {
      logOutput("Sem horario valido; dados nao serao armazenados.");
    } else {
      if (!client.writePoint(sensor)) {  // Envio correto com timestamp
        logOutput("InfluxDB error: " + client.getLastErrorMessage());
        storeOfflineData(h, t, hic, now);
      } else {
        logOutput("Dados enviados com sucesso.");
      }
    }
  }

  unsigned long intervalMs = currentReadingIntervalSeconds * 1000UL;
  delay(intervalMs);
}

void resetRuntimeConfigToDefaults() {
  pendingWifiSsid = String(WIFI_SSID);
  pendingWifiPassword = String(WIFI_PASSWORD);
  currentApSsid = String(WIFI_AP_SSID);
  currentApPassword = String(WIFI_AP_PASSWORD);
  apModeEnabled = true;
  currentReadingIntervalSeconds = READING_INTERVAL;

  wifiSaveRequested = true;
  wifiReconnectScheduled = false;
  wifiReconnectAtMs = 0;
  apConfigUpdateRequested = true;
  apConfigUpdateAtMs = millis() + 1000;
  lastWifiReconnectAttempt = 0;
  lastSensorReadAt = 0;

  if (apModeActive) {
    WiFi.softAPdisconnect(true);
    apModeActive = false;
  }
}

void storeOfflineData(float h, float t, float hic, time_t ts) {
  File file = SPIFFS.open("/offline.csv", "a");
  if (!file) {
    logOutput("Falha ao abrir arquivo offline para gravacao.");
    return;
  }

  file.print(String((long)ts));
  file.print(",");
  file.print(String(h, 2));
  file.print(",");
  file.print(String(t, 2));
  file.print(",");
  file.println(String(hic, 2));
  file.close();

  logOutput("Dados armazenados offline.");
}

void resendOfflineData() {
  if (!SPIFFS.exists("/offline.csv")) {
    return;
  }

  File input = SPIFFS.open("/offline.csv", "r");
  if (!input) {
    logOutput("Falha ao abrir arquivo offline para leitura.");
    return;
  }

  File output = SPIFFS.open("/offline.tmp", "w");
  if (!output) {
    logOutput("Falha ao criar arquivo temporario de reenvio.");
    input.close();
    return;
  }

  bool keptAny = false;

  while (input.available()) {
    String line = input.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) {
      continue;
    }

    int firstComma = line.indexOf(',');
    int secondComma = line.indexOf(',', firstComma + 1);
    int thirdComma = line.indexOf(',', secondComma + 1);
    if (firstComma < 0 || secondComma < 0 || thirdComma < 0) {
      continue;
    }

    float h = line.substring(firstComma + 1, secondComma).toFloat();
    float t = line.substring(secondComma + 1, thirdComma).toFloat();
    float hic = line.substring(thirdComma + 1).toFloat();

    Point offlinePoint("temperature_humidity");
    offlinePoint.addTag("device", DEVICE);
    offlinePoint.addField("h", h);
    offlinePoint.addField("t", t);
    offlinePoint.addField("hic", hic);

    if (!client.writePoint(offlinePoint)) {
      output.println(line);
      keptAny = true;
      while (input.available()) {
        String remaining = input.readStringUntil('\n');
        remaining.trim();
        if (remaining.length() > 0) {
          output.println(remaining);
        }
      }
      break;
    }
  }

  input.close();
  output.close();

  SPIFFS.remove("/offline.csv");
  if (keptAny) {
    SPIFFS.rename("/offline.tmp", "/offline.csv");
    logOutput("Alguns dados ainda aguardam reenvio.");
  } else {
    SPIFFS.remove("/offline.tmp");
    logOutput("Fila offline reenviada com sucesso.");
  }
}