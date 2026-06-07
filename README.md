# ESP DHT
This is a project to monitor the temperature and umidity using a ESP01s and a DHT22 then upload to a InfluxDB / Grafana.


Create a `secrets.h` file to set the following constants:

```
#ifndef SECRETS_H
#define SECRETS_H

#define WIFI_AP_SSID ""
#define WIFI_AP_PASSWORD ""
#define WIFI_SSID "SSID"
#define WIFI_PASSWORD "PASS"
#define INFLUXDB_URL ""
#define INFLUXDB_TOKEN ""
#define INFLUXDB_ORG ""
#define INFLUXDB_BUCKET ""

#endif
```