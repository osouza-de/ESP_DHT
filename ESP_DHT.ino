#include "DHT.h"
#include <ESPAsyncWebServer.h>
#include "secrets.h"

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

#define READING_INTERVAL 20  // Intervalo entre leituras (segundos)
#define DHTTYPE DHT22

#include <InfluxDbClient.h>
#include <InfluxDbCloud.h>

AsyncWebServer server(80);
AsyncEventSource events("/events");

InfluxDBClient client(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN, InfluxDbCloud2CACert);
Point sensor("temperature_humidity");
DHT dht(DHTPIN, DHTTYPE);

void logOutput(const String& message, bool newline = true) {
  time_t now = time(nullptr);
  struct tm* timeInfo = localtime(&now);
  char timeStr[20];
  strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", timeInfo);

  String logMessage = "[" + String(timeStr) + "] " + message;

  if (newline) Serial.println(logMessage);
  else Serial.print(logMessage);

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
      logOutput("Offline: Dados enviados com timestamp " + String(timestamp));
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
  Serial.begin(115200);

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

  WiFi.mode(WIFI_STA);
  wifiMulti.addAP(WIFI_SSID, WIFI_PASSWORD);
  logOutput("MAC Wi-Fi: " + WiFi.macAddress());
  logOutput("Conectando ao Wi-Fi...", false);

  while (wifiMulti.run() != WL_CONNECTED) {
    delay(500);
    logOutput(".", false);
  }
  logOutput(" Conectado! IP: " + WiFi.localIP().toString());

  server.addHandler(&events);
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "ESP SSE ativo.");
  });
  server.begin();

  timeSync(TZ_INFO, "pool.ntp.org", "time.nis.gov");

  if (client.validateConnection()) {
    logOutput("Conectado ao InfluxDB.");
  } else {
    logOutput("Erro InfluxDB: " + client.getLastErrorMessage());
  }

  sensor.addTag("device", DEVICE);
  dht.begin();
}

void loop() {
  sensor.clearFields();

  delay(250);
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  float hic = dht.computeHeatIndex(t, h, false);

  if (isnan(h) || isnan(t)) {
    logOutput("Erro ao ler sensor.");
    return;
  }

  sensor.addField("h", h);
  sensor.addField("t", t);
  sensor.addField("hic", hic);

  logOutput("Enviando: h=" + String(h) + " t=" + String(t) + " hic=" + String(hic));

  time_t now = time(nullptr);
  // String nowStr = String(now) + "000000000";  // Timestamp em nanossegundos

  if (wifiMulti.run() != WL_CONNECTED) {
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
