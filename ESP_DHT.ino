  #include "DHT.h"
  #include <ESPAsyncWebServer.h>
  #include <DNSServer.h>

  #if defined(ESP32)
    #include <Preferences.h>
  #endif

  #if defined(ESP32)
    #include "SPIFFS.h"
    #include <WiFiMulti.h>
    WiFiMulti wifiMulti;
    #define DEVICE "ESP32"
    #define DHTPIN 15
  #elif defined(ESP8266)
    #include "FS.h"
    #include <ESP8266WiFiMulti.h>
    ESP8266WiFiMulti wifiMulti;
    #define DEVICE "ESP8266"
    #define DHTPIN 2
  #endif

  #define READING_INTERVAL 5 * 60  // Intervalo entre leituras (segundos)
  #define DHTTYPE DHT22
  #define WIFI_CONNECT_TIMEOUT_MS_CUSTOM 15000
  #define WIFI_CONFIG_FILE "/wifi.txt"

  #include <InfluxDbClient.h>
  #include <InfluxDbCloud.h>

  AsyncWebServer server(80);
  AsyncEventSource events("/events");
  DNSServer dnsServer;
  String ssidOptions = "";
  String logBuffer = "";

  void performWifiScan();

  struct WifiCredentials {
    String ssid;
    String password;
  };

  #if defined(ESP32)
  Preferences preferences;
  #endif

  bool apModeActive = false;
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

  InfluxDBClient client(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN, InfluxDbCloud2CACert);
  Point sensor("temperature_humidity");
  DHT dht(DHTPIN, DHTTYPE);

  void logOutput(const String& message, bool newline = true);
  void resendOfflineData();
  String buildLogsPage();

  void appendLogToBuffer(const String& line) {
    const size_t maxLogBufferSize = 4096;
    logBuffer += line;
    logBuffer += "\n";

    if (logBuffer.length() > maxLogBufferSize) {
      int cutPos = logBuffer.indexOf('\n', logBuffer.length() - maxLogBufferSize / 2);
      if (cutPos >= 0) {
        logBuffer = logBuffer.substring(cutPos + 1);
      } else {
        logBuffer = logBuffer.substring(logBuffer.length() - maxLogBufferSize);
      }
    }
  }
  void performWifiScan() {
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
        ssidOptions += "<option value='" + ss + "'>" + display + "</option>";
      }
    }
  }

  WifiCredentials loadWifiCredentials() {
    WifiCredentials credentials;

  #if defined(ESP32)
    preferences.begin("wifi", true);
    credentials.ssid = preferences.getString("ssid", WIFI_SSID);
    credentials.password = preferences.getString("pass", WIFI_PASSWORD);
    preferences.end();
  #elif defined(ESP8266)
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
  #endif

    return credentials;
  }

  void loadApCredentials() {
  #if defined(ESP32)
    preferences.begin("wifi", true);
    currentApSsid = preferences.getString("apssid", WIFI_AP_SSID);
    currentApPassword = preferences.getString("appass", WIFI_AP_PASSWORD);
    preferences.end();
  #elif defined(ESP8266)
    if (SPIFFS.exists(WIFI_CONFIG_FILE)) {
      File file = SPIFFS.open(WIFI_CONFIG_FILE, "r");
      if (file) {
        String wifiSsid = file.readStringUntil('\n');
        String wifiPassword = file.readStringUntil('\n');
        String apSsid = file.readStringUntil('\n');
        String apPassword = file.readStringUntil('\n');

        wifiSsid.trim();
        wifiPassword.trim();
        apSsid.trim();
        apPassword.trim();

        if (apSsid.length() > 0) {
          currentApSsid = apSsid;
        }

        if (apPassword.length() > 0) {
          currentApPassword = apPassword;
        }

        file.close();
      }
    }
  #endif
  }

  bool saveWifiCredentials(const String& ssid, const String& password) {
  #if defined(ESP32)
    preferences.begin("wifi", false);
    preferences.putString("ssid", ssid);
    preferences.putString("pass", password);
    preferences.end();
    return true;
  #elif defined(ESP8266)
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
  #endif
  }

  bool saveAllCredentials(const String& wifiSsid, const String& wifiPassword, const String& apSsid, const String& apPassword) {
  #if defined(ESP32)
    preferences.begin("wifi", false);
    preferences.putString("ssid", wifiSsid);
    preferences.putString("pass", wifiPassword);
    preferences.putString("apssid", apSsid);
    preferences.putString("appass", apPassword);
    preferences.end();
    return true;
  #elif defined(ESP8266)
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
      file.close();
      ok = true;
    }

    if (!ok) {
      logOutput("Falha ao salvar configuracao AP apos " + String(maxTries) + " tentativas.");
    } else {
      logOutput("Configuracao AP salva.");
    }

    return ok;
  #endif
  }

  String wifiStatusPage() {
    String html;
    html.reserve(1800);
    html += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>ESP DHT Wi-Fi</title>";
    html += "<style>body{font-family:Arial,sans-serif;max-width:520px;margin:40px auto;padding:16px;background:#f4f7fb;color:#1f2937}";
    html += "h1{font-size:1.6rem} .card{background:#fff;border-radius:12px;padding:18px;box-shadow:0 8px 24px rgba(0,0,0,.08)}";
    html += "select,input,button,a{width:100%;padding:12px;margin:8px 0;box-sizing:border-box;border-radius:8px;border:1px solid #cbd5e1}";
    html += "button,a{display:block;text-align:center;text-decoration:none;background:#0f766e;color:#fff;border:none;font-weight:700}a{background:#334155}</style>";
    html += "</head><body><div class='card'><h1>ESP DHT Wi-Fi</h1>";
    html += "<p>Status: ";
    html += (WiFi.status() == WL_CONNECTED) ? "Conectado" : "Modo configuracao";
    html += "</p>";
    html += "<p>IP: ";
    html += WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    html += "</p>";
    html += "<p>SSID conectado: ";
    html += WiFi.status() == WL_CONNECTED ? WiFi.SSID() : "Nao conectado";
    html += "</p>";
    html += "<a href='/logs'>Ver logs</a>";
    html += "<form method='POST' action='/save'>";
    html += "<label>SSID</label>";
    html += "<select name='ssid' required>";

    html += ssidOptions;

    html += "</select>";
    html += "<label>Senha</label><input name='password' type='password' maxlength='64'>";
    html += "<button type='submit'>Salvar e conectar</button>";
    html += "</form>";
    html += "<a href='/rescan'>Procurar redes novamente</a>";
    html += "<hr><h2>Configuracao do AP</h2>";
    html += "<p>SSID do AP: " + currentApSsid + "</p>";
    html += "<p>Senha do AP: " + currentApPassword + "</p>";
    html += "<form method='POST' action='/save'>";
    html += "<label>SSID do AP</label><input name='ap_ssid' maxlength='32' value='" + currentApSsid + "'>";
    html += "<label>Senha do AP</label><input name='ap_password' type='password' maxlength='64' value='" + currentApPassword + "'>";
    html += "<button type='submit'>Salvar configuracao do AP</button>";
    html += "</form>";
    html += "</div></body></html>";
    return html;
  }

  String buildLogsPage() {
    String html;
    html.reserve(2200);
    html += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>ESP Logs</title>";
    html += "<style>body{font-family:Arial,sans-serif;max-width:900px;margin:40px auto;padding:16px;background:#111827;color:#e5e7eb}";
    html += ".card{background:#1f2937;border-radius:12px;padding:18px;box-shadow:0 8px 24px rgba(0,0,0,.3)}";
    html += "pre{white-space:pre-wrap;word-wrap:break-word;background:#0b1220;padding:14px;border-radius:10px;overflow:auto;max-height:70vh}";
    html += "a{display:inline-block;margin-bottom:12px;color:#93c5fd;text-decoration:none}</style>";
    html += "</head><body><div class='card'><a href='/'>Voltar</a><h1>Logs</h1><pre>";
    html += logBuffer.length() > 0 ? logBuffer : "Sem logs ainda.";
    html += "</pre></div></body></html>";
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

  void startConfigPortal() {
    if (apModeActive) {
      performWifiScan();
      return;
    }

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(currentApSsid.c_str(), currentApPassword.c_str());

    IPAddress apIP = WiFi.softAPIP();
    dnsServer.start(53, "*", apIP);
    apModeActive = true;

    // Pre-scan available networks and cache options to avoid scanning inside the web handler
    performWifiScan();

    logOutput("AP iniciado: " + currentApSsid);
    logOutput("Conecte-se em " + apIP.toString() + " para configurar o Wi-Fi.");
  }

  void setupWebServer() {
    server.addHandler(&events);

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send(200, "text/html", wifiStatusPage());
    });

    server.on("/logs", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send(200, "text/html", buildLogsPage());
    });

    server.on("/rescan", HTTP_GET, [](AsyncWebServerRequest *request) {
      performWifiScan();
      request->send(200, "text/plain", "OK");
    });

    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
      String ssid;
      String password;
      String apSsid = currentApSsid;
      String apPassword = currentApPassword;

      if (request->hasParam("ssid", true)) {
        ssid = request->getParam("ssid", true)->value();
      }

      if (request->hasParam("ap_ssid", true)) {
        apSsid = request->getParam("ap_ssid", true)->value();
      }

      if (request->hasParam("ap_password", true)) {
        apPassword = request->getParam("ap_password", true)->value();
      }

      if (request->hasParam("password", true)) {
        password = request->getParam("password", true)->value();
      apSsid.trim();
      apPassword.trim();
      }

        ssid = loadWifiCredentials().ssid;
        password = loadWifiCredentials().password;
      password.trim();

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
      wifiSaveRequested = true;
      apConfigUpdateRequested = true;
      apConfigUpdateAtMs = millis() + 1000;
      request->send(200, "text/html", "<html><body><h2>Recebido. Salvando e aplicando configuracoes...</h2></body></html>");

      pendingWifiSsid = ssid;
      pendingWifiPassword = password;
      wifiSaveRequested = true;
      request->send(200, "text/html", "<html><body><h2>Recebido. Salvando e tentando conectar...</h2></body></html>");
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
      resendOfflineData();
    } else {
      logOutput("Erro InfluxDB: " + client.getLastErrorMessage());
    }
  }

  void logOutput(const String& message, bool newline) {
    time_t now = time(nullptr);
    struct tm* timeInfo = localtime(&now);
    char timeStr[20];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", timeInfo);

    String logMessage = "[" + String(timeStr) + "] " + message;

    if (newline) Serial.println(logMessage);
    else Serial.print(logMessage);

    appendLogToBuffer(logMessage);
    events.send(logMessage.c_str(), "message");
  }

  void storeOffline(float h, float t, float hic) {
    time_t now = time(nullptr);
    
  #if defined(ESP32)
    File file = SPIFFS.open("/offline_data.txt", FILE_APPEND);
  #elif defined(ESP8266)
    File file = SPIFFS.open("/offline_data.txt", "a");
  #endif

    if (!file) {
      logOutput("Erro ao salvar dados offline.");
      return;
    }

    String dataLine = String(now) + "," + String(h) + "," + String(t) + "," + String(hic);
    file.println(dataLine);
    file.close();
  }

  void resendOfflineData() {
  #if defined(ESP32) || defined(ESP8266)
    if (!SPIFFS.exists("/offline_data.txt")) return;

  #if defined(ESP32)
    File file = SPIFFS.open("/offline_data.txt", FILE_READ);
  #elif defined(ESP8266)
    File file = SPIFFS.open("/offline_data.txt", "r");
  #endif

    if (!file) {
      logOutput("Erro ao abrir arquivo offline.");
      return;
    }

    bool allSent = true;

    while (file.available()) {
      String line = file.readStringUntil('\n');
      line.trim();

      if (line.length() == 0) continue;

      int comma1 = line.indexOf(',');
      int comma2 = line.indexOf(',', comma1 + 1);
      int comma3 = line.indexOf(',', comma2 + 1);

      time_t timestamp = line.substring(0, comma1).toInt();
      float h = line.substring(comma1 + 1, comma2).toFloat();
      float t = line.substring(comma2 + 1, comma3).toFloat();
      float hic = line.substring(comma3 + 1).toFloat();
      String timestampStr = String(timestamp) + "000000000";  // Timestamp em nanossegundos

      sensor.clearFields();
      sensor.addField("h", h);
      sensor.addField("t", t);
      sensor.addField("hic", hic);
      sensor.setTime(timestampStr);

      

      if (client.writePoint(sensor)) {  // Passando timestamp corretamente
        logOutput("Offline reenviado para InfluxDB: ts=" + String(timestamp) +
            " h=" + String(h) +
            " t=" + String(t) +
            " hic=" + String(hic));
      } else {
        logOutput("Falha ao enviar offline: " + client.getLastErrorMessage());
        allSent = false;
        break;
      }
    }

    file.close();

    if (allSent) {
      SPIFFS.remove("/offline_data.txt");
      logOutput("Todos os dados offline enviados com sucesso.");
    } else {
      logOutput("Falha ao enviar alguns dados offline.");
    }
  #endif
  }

  void setup() {
    Serial.begin(74880);

  #if defined(ESP32)
    if (!SPIFFS.begin(true)) {
      logOutput("Falha ao iniciar SPIFFS.");
      return;
    }
  #elif defined(ESP8266)
    if (!SPIFFS.begin()) {
      logOutput("Falha ao iniciar SPIFFS.");
      return;
    }
  #endif

    WiFi.mode(WIFI_AP_STA);
  #if defined(ESP8266)
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
  #endif

    setupWebServer();

    logOutput("MAC Wi-Fi: " + WiFi.macAddress());
    logOutput("Conectando ao Wi-Fi...");

    loadApCredentials();

    startConfigPortal();

    WifiCredentials credentials = loadWifiCredentials();
    if (connectToWifi(credentials.ssid, credentials.password, WIFI_CONNECT_TIMEOUT_MS_CUSTOM)) {
      logOutput("Conectado ao SSID '" + WiFi.SSID() + "'! IP: " + WiFi.localIP().toString());
      handleWifiConnected();
    } else {
      logOutput("Falha na conexao. Iniciando portal de configuracao.");
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

      if (saveAllCredentials(pendingWifiSsid, pendingWifiPassword, currentApSsid, currentApPassword)) {
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
      startConfigPortal();
    }

    sensor.clearFields();

    delay(250);
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    float hic = dht.computeHeatIndex(t, h, false);

    if (isnan(h) || isnan(t)) {
      delay(10000);
      logOutput("Erro ao ler sensor.");
      return;
    }

    sensor.addField("h", h);
    sensor.addField("t", t);
    sensor.addField("hic", hic);

    logOutput("Enviando: h=" + String(h) + " t=" + String(t) + " hic=" + String(hic));

    if (wifiReconnectScheduled && millis() >= wifiReconnectAtMs && WiFi.status() != WL_CONNECTED) {
      wifiReconnectScheduled = false;

      WifiCredentials credentials = loadWifiCredentials();
      if (!connectToWifi(credentials.ssid, credentials.password, WIFI_CONNECT_TIMEOUT_MS_CUSTOM)) {
        logOutput("Ainda no portal de configuracao enquanto tenta conectar ao novo Wi-Fi.");
      } else {
        logOutput("Wi-Fi reconectado. IP: " + WiFi.localIP().toString());
        handleWifiConnected();
      }
    }

    if (WiFi.status() != WL_CONNECTED) {
      if (!apModeActive && millis() - lastWifiReconnectAttempt > wifiReconnectIntervalMs) {
        lastWifiReconnectAttempt = millis();
        WifiCredentials credentials = loadWifiCredentials();
        if (!connectToWifi(credentials.ssid, credentials.password, WIFI_CONNECT_TIMEOUT_MS_CUSTOM)) {
          startConfigPortal();
        } else {
          logOutput("Wi-Fi reconectado. IP: " + WiFi.localIP().toString());
          handleWifiConnected();
        }
      }

      logOutput("Wi-Fi perdido. Salvando offline.");
      storeOffline(h, t, hic);
    } else {
      if (!client.writePoint(sensor)) {  // Envio correto com timestamp
        logOutput("Erro InfluxDB: " + client.getLastErrorMessage());
        storeOffline(h, t, hic);
      } else {
        logOutput("Dados enviados com sucesso.");
      }
    }

    delay(READING_INTERVAL * 1000);
  }
