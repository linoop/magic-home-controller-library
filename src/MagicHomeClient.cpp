#include "MagicHomeClient.h"

#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <DHT.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "esp_system.h"
#include "time.h"

#define BUTTON_PIN 0

static const char *kServerHost = "magichome.co.in";
static const bool kServerUseSSL = true;
static const uint16_t kServerPort = 443;

MagicHomeClient::MagicHomeClient(const MagicHomeConfig &config)
    : config_(config),
      dht_(nullptr),
      sensor_{0.0, 0.0, false, false, 0, ""},
      numSchedules_(0),
    energy_{0.0, 0, 0.0},
    buttonPressTime_(0),
    buttonPressed_(false),
    allRelaysState_(false),
      isConnected_(false),
      isRegistered_(false),
      lastSensorRead_(0),
      lastReconnectAttempt_(0),
      lastHeartbeat_(0)
{
}

void MagicHomeClient::begin()
{
    Serial.begin(115200);
    Serial.println("\n\n=================================");
    Serial.println("ESP32 Magic Home Client");
    Serial.println("=================================\n");

    pinMode(config_.statusLedPin, OUTPUT);
    digitalWrite(config_.statusLedPin, LOW);

    setupRelays();
    loadRelayStates();

    if (config_.dhtType != 0)
    {
        dht_ = new DHT(config_.dhtPin, config_.dhtType);
        dht_->begin();
        Serial.println("[DHT] Sensor initialized");
    }
    else
    {
        Serial.println("[DHT] Sensor disabled (dhtType = 0)");
    }

    setupWiFi();

    Serial.println("\n[API] Registering with server...");
    if (registerDevice())
    {
        Serial.println("[API] Device registration successful");
    }
    else
    {
        Serial.println("[API] Device registration failed - will retry later");
    }

    setupWebSocket();

    Serial.println("\n[READY] System initialized successfully");
    Serial.println("=================================\n");
}

void MagicHomeClient::loop()
{
    webSocket_.loop();

    if (millis() - lastHeartbeat_ >= kHeartbeatIntervalMs)
    {
        if (!isRegistered_)
        {
            Serial.println("[API] Attempting registration...");
            registerDevice();
        }
        else
        {
            sendHeartbeat();
        }
        lastHeartbeat_ = millis();
    }

    if (millis() - lastSensorRead_ >= kSensorIntervalMs)
    {
        readSensorData();
        lastSensorRead_ = millis();
    }

    if (!isConnected_ && (millis() - lastReconnectAttempt_ >= kReconnectIntervalMs))
    {
        reconnectWebSocket();
        lastReconnectAttempt_ = millis();
    }

    if (isConnected_)
    {
        digitalWrite(config_.statusLedPin, HIGH);
    }
    else
    {
        digitalWrite(config_.statusLedPin, (millis() / 500) % 2);
    }
}

void MagicHomeClient::setupWiFi()
{
    Serial.print("[WiFi] Connecting to ");
    Serial.println(config_.ssid);

    WiFi.mode(WIFI_STA);
    WiFi.begin(config_.ssid, config_.password);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20)
    {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("\n[WiFi] Connected!");
        Serial.print("[WiFi] IP Address: ");
        Serial.println(WiFi.localIP());
        Serial.print("[WiFi] MAC Address: ");
        Serial.println(WiFi.macAddress());
    }
    else
    {
        Serial.println("\n[WiFi] Connection failed!");
        Serial.println("[WiFi] Restarting in 5 seconds...");
        delay(5000);
        ESP.restart();
    }
}

void MagicHomeClient::setupRelays()
{
    Serial.println("[RELAY] Initializing relays...");
    for (size_t i = 0; i < config_.relayCount; i++)
    {
        pinMode(config_.relays[i].pin, OUTPUT);
        digitalWrite(config_.relays[i].pin, LOW);
        config_.relays[i].state = false;
        Serial.printf("[RELAY] %s (Pin: %d) - OFF\n",
                      config_.relays[i].name, config_.relays[i].pin);
    }
}

void MagicHomeClient::setRelayState(int pin, bool state)
{
    for (size_t i = 0; i < config_.relayCount; i++)
    {
        if (config_.relays[i].pin == pin)
        {
            digitalWrite(config_.relays[i].pin, state ? HIGH : LOW);
            config_.relays[i].state = state;
            Serial.printf("[RELAY] %s (ID: %d) -> %s\n",
                          config_.relays[i].name, pin, state ? "ON" : "OFF");
            saveRelayStates();
            return;
        }
    }
    Serial.printf("[RELAY] Invalid relay pin: %d\n", pin);
}

void MagicHomeClient::controlRelay(int pin, String action)
{
    action.toLowerCase();

    bool validPin = false;
    for (size_t i = 0; i < config_.relayCount; i++)
    {
        if (config_.relays[i].pin == pin)
        {
            validPin = true;
            break;
        }
    }

    if (!validPin)
    {
        Serial.printf("[RELAY] Invalid pin: %d\n", pin);
        return;
    }

    if (action == "on")
    {
        setRelayState(pin, true);
    }
    else if (action == "off")
    {
        setRelayState(pin, false);
    }
    else if (action == "toggle")
    {
        for (size_t i = 0; i < config_.relayCount; i++)
        {
            if (config_.relays[i].pin == pin)
            {
                setRelayState(pin, !config_.relays[i].state);
                break;
            }
        }
    }

    sendRelayStatus(pin);
}

void MagicHomeClient::turnAllRelaysOn()
{
    Serial.println("[RELAY] Turning all relays ON");
    for (size_t i = 0; i < config_.relayCount; i++)
    {
        digitalWrite(config_.relays[i].pin, HIGH);
        config_.relays[i].state = true;
    }
    saveRelayStates();
    sendRelayStatus();
}

void MagicHomeClient::turnAllRelaysOff()
{
    Serial.println("[RELAY] Turning all relays OFF");
    for (size_t i = 0; i < config_.relayCount; i++)
    {
        digitalWrite(config_.relays[i].pin, LOW);
        config_.relays[i].state = false;
    }
    saveRelayStates();
    sendRelayStatus();
}

void MagicHomeClient::setupWebSocket()
{
    String websocketPath = "/ws/" + String(config_.deviceId) + "/?client=controller";
    const char *wsProtocol = kServerUseSSL ? "wss" : "ws";
    String authHeader = "Authorization: Token " + String(config_.authToken);

    webSocket_.setExtraHeaders(authHeader.c_str());

    Serial.printf("[WebSocket] Connecting to %s://%s:%d%s\n",
                  wsProtocol, kServerHost, kServerPort, websocketPath.c_str());

    if (kServerUseSSL)
    {
        webSocket_.beginSSL(kServerHost, kServerPort, websocketPath);
    }
    else
    {
        webSocket_.begin(kServerHost, kServerPort, websocketPath);
    }

    webSocket_.onEvent([this](WStype_t type, uint8_t *payload, size_t length)
                       { webSocketEvent(type, payload, length); });
    webSocket_.setReconnectInterval(5000);

    Serial.println("[WebSocket] Token authentication enabled");
}

void MagicHomeClient::reconnectWebSocket()
{
    Serial.println("[WebSocket] Attempting to reconnect...");
    webSocket_.disconnect();
    delay(1000);
    setupWebSocket();
}

void MagicHomeClient::webSocketEvent(WStype_t type, uint8_t *payload, size_t length)
{
    switch (type)
    {
    case WStype_DISCONNECTED:
        if (length > 0 && payload != nullptr)
        {
            Serial.printf("[WebSocket] Disconnected: %.*s\n", (int)length, payload);
        }
        else
        {
            Serial.println("[WebSocket] Disconnected!");
        }
        isConnected_ = false;
        break;

    case WStype_CONNECTED:
        Serial.printf("[WebSocket] Connected to: %s\n", payload);
        isConnected_ = true;

        delay(1000);
        sendDeviceInfo();
        sendRelayStatus();
        sendSensorData();
        break;

    case WStype_TEXT:
        Serial.printf("[WebSocket] Received: %s\n", payload);
        handleWebSocketMessage(payload, length);
        break;

    case WStype_ERROR:
        if (length > 0 && payload != nullptr)
        {
            Serial.printf("[WebSocket] Error: %.*s\n", (int)length, payload);
        }
        else
        {
            Serial.println("[WebSocket] Error occurred");
        }
        isConnected_ = false;
        break;

    case WStype_PING:
        Serial.println("[WebSocket] Ping received");
        break;

    case WStype_PONG:
        Serial.println("[WebSocket] Pong received");
        break;
    }
}

void MagicHomeClient::handleWebSocketMessage(uint8_t *payload, size_t length)
{
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, payload, length);

    if (error)
    {
        Serial.printf("[JSON] Parse error: %s\n", error.c_str());
        return;
    }

    if (doc.containsKey("type"))
    {
        String messageType = doc["type"].as<String>();

        if (messageType == "relay_update")
        {
            if (doc.containsKey("pin"))
            {
                int pin = doc["pin"];
                bool state = doc["state"];
                setRelayState(pin, state);
            }
            else if (doc.containsKey("relay_id"))
            {
                int pin = doc["relay_id"];
                bool state = doc["state"];
                setRelayState(pin, state);
            }
        }
        else if (messageType == "request_status")
        {
            sendRelayStatus();
            sendSensorData();
        }
        else if (messageType == "request_sensor")
        {
            sendSensorData();
        }
    }
    else if (doc.containsKey("action"))
    {
        String action = doc["action"].as<String>();
        action.toLowerCase();

        if (action == "status")
        {
            sendRelayStatus();
        }
        else if (action == "sensor" || action == "get_sensor" || action == "temperature")
        {
            sendSensorData();
        }
        else if (action == "all_on")
        {
            turnAllRelaysOn();
        }
        else if (action == "all_off")
        {
            turnAllRelaysOff();
        }
        else if (doc.containsKey("pin"))
        {
            int pin = doc["pin"];
            controlRelay(pin, action);
        }
    }
}

void MagicHomeClient::sendRelayStatus(int specificPin)
{
    StaticJsonDocument<1024> doc;

    if (specificPin >= 0)
    {
        for (size_t i = 0; i < config_.relayCount; i++)
        {
            if (config_.relays[i].pin == specificPin)
            {
                doc["type"] = "relay_status";
                doc["pin"] = config_.relays[i].pin;
                doc["name"] = config_.relays[i].name;
                doc["description"] = config_.relays[i].description;
                doc["state"] = config_.relays[i].state;
                break;
            }
        }
    }
    else
    {
        doc["type"] = "relay_status_all";
        JsonArray relayArray = doc.createNestedArray("relays");

        for (size_t i = 0; i < config_.relayCount; i++)
        {
            JsonObject relay = relayArray.createNestedObject();
            relay["pin"] = config_.relays[i].pin;
            relay["name"] = config_.relays[i].name;
            relay["description"] = config_.relays[i].description;
            relay["state"] = config_.relays[i].state;
        }
    }

    String output;
    serializeJson(doc, output);
    webSocket_.sendTXT(output);
    Serial.printf("[WebSocket] Sent: %s\n", output.c_str());
}

void MagicHomeClient::sendSensorData()
{
    StaticJsonDocument<256> doc;

    doc["type"] = "sensor_data";
    doc["temperature"] = sensor_.valid ? sensor_.temperature : 0.0;
    doc["humidity"] = sensor_.valid ? sensor_.humidity : 0.0;
    doc["temperature_f"] = sensor_.valid ? (sensor_.temperature * 9.0 / 5.0) + 32.0 : 0.0;
    doc["valid"] = sensor_.valid;
    doc["connected"] = sensor_.connected;
    doc["last_update"] = sensor_.lastUpdate;

    if (!sensor_.connected || !sensor_.valid)
    {
        doc["error"] = sensor_.errorMessage;
    }

    String output;
    serializeJson(doc, output);
    webSocket_.sendTXT(output);
    Serial.printf("[WebSocket] Sent: %s\n", output.c_str());

    if (sensor_.valid)
    {
        Serial.printf("[Sensor] Sent: T=%.1fC, H=%.1f%%\n", sensor_.temperature, sensor_.humidity);
    }
    else
    {
        Serial.printf("[Sensor] Sent status: disconnected (%s)\n", sensor_.errorMessage.c_str());
    }
}

void MagicHomeClient::sendDeviceInfo()
{
    StaticJsonDocument<512> doc;

    doc["type"] = "device_info";
    doc["device_name"] = config_.deviceName;
    doc["device_type"] = "home_automation";
    doc["ip_address"] = WiFi.localIP().toString();
    doc["mac_address"] = WiFi.macAddress();
    doc["num_relays"] = config_.relayCount;
    doc["firmware_version"] = "1.0.0";

    String output;
    serializeJson(doc, output);
    webSocket_.sendTXT(output);
    Serial.printf("[WebSocket] Sent: %s\n", output.c_str());
}

void MagicHomeClient::readSensorData()
{
    if (config_.dhtType == 0 || dht_ == nullptr)
    {
        sensor_.valid = false;
        sensor_.connected = false;
        sensor_.errorMessage = "Sensor disabled (dhtType = 0)";
        return;
    }

    float newTemp = dht_->readTemperature();
    float newHumidity = dht_->readHumidity();

    if (!isnan(newTemp) && !isnan(newHumidity))
    {
        bool wasDisconnected = !sensor_.connected;

        sensor_.temperature = newTemp;
        sensor_.humidity = newHumidity;
        sensor_.connected = true;
        sensor_.valid = true;
        sensor_.lastUpdate = millis();
        sensor_.errorMessage = "";

        if (wasDisconnected)
        {
            Serial.println("[DHT] Sensor reconnected!");
            if (isConnected_)
            {
                sendSensorData();
            }
        }
        else if (isConnected_)
        {
            sendSensorData();
        }
    }
    else
    {
        bool wasConnected = sensor_.connected;

        sensor_.connected = false;
        sensor_.valid = false;
        sensor_.errorMessage = "Sensor not connected or failed to read";

        if (wasConnected)
        {
            Serial.println("[DHT] Sensor disconnected! Notifying server...");
            Serial.println("[DHT] Check: wiring (VCC, GND, DATA), power supply, pull-up resistor");
            if (isConnected_)
            {
                sendSensorData();
            }
        }
    }
}

String MagicHomeClient::getServerURL(const char *endpoint)
{
    String url = kServerUseSSL ? "https://" : "http://";
    url += kServerHost;

    bool isDefaultPort = (!kServerUseSSL && kServerPort == 80) ||
                         (kServerUseSSL && kServerPort == 443);
    if (!isDefaultPort)
    {
        url += ":";
        url += String(kServerPort);
    }

    url += endpoint;
    return url;
}

bool MagicHomeClient::registerDevice()
{
    HTTPClient http;
    WiFiClient client;
    WiFiClientSecure secureClient;
    String url = getServerURL("/api/device/register");

    Serial.println("[API] Registering device...");
    if (kServerUseSSL)
    {
        secureClient.setInsecure();
        http.begin(secureClient, url);
    }
    else
    {
        http.begin(client, url);
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Token " + String(config_.authToken));

    StaticJsonDocument<1024> doc;
    doc["device_id"] = config_.deviceId;
    doc["name"] = config_.deviceName;
    doc["description"] = config_.deviceDescription;
    doc["mac_address"] = WiFi.macAddress();
    doc["ip_address"] = WiFi.localIP().toString();

    if (config_.dhtType != 0)
    {
        if (config_.dhtType == DHT11)
        {
            doc["sensor_type"] = "DHT11";
        }
        else if (config_.dhtType == DHT22)
        {
            doc["sensor_type"] = "DHT22";
        }
        else
        {
            doc["sensor_type"] = "DHT";
        }
        doc["has_sensor"] = true;
    }
    else
    {
        doc["sensor_type"] = nullptr;
        doc["has_sensor"] = false;
    }

    JsonArray relaysArray = doc.createNestedArray("relays");
    for (size_t i = 0; i < config_.relayCount; i++)
    {
        JsonObject relay = relaysArray.createNestedObject();
        relay["pin"] = config_.relays[i].pin;
        relay["name"] = config_.relays[i].name;
        relay["description"] = config_.relays[i].description;
    }

    String payload;
    serializeJson(doc, payload);

    int httpCode = http.POST(payload);
    bool success = false;

    if (httpCode > 0)
    {
        String response = http.getString();
        Serial.printf("[API] Response code: %d\n", httpCode);
        Serial.printf("[API] Response: %s\n", response.c_str());

        if (httpCode == 200 || httpCode == 201)
        {
            success = true;
            isRegistered_ = true;
            Serial.println("[API] Device registered successfully");
        }
    }
    else
    {
        Serial.printf("[API] Registration failed: %s\n", http.errorToString(httpCode).c_str());
    }

    http.end();
    return success;
}

bool MagicHomeClient::sendHeartbeat()
{
    if (!isRegistered_)
    {
        return false;
    }

    HTTPClient http;
    WiFiClient client;
    WiFiClientSecure secureClient;
    String url = getServerURL("/api/device/heartbeat");

    if (kServerUseSSL)
    {
        secureClient.setInsecure();
        http.begin(secureClient, url);
    }
    else
    {
        http.begin(client, url);
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Token " + String(config_.authToken));

    StaticJsonDocument<256> doc;
    doc["device_id"] = config_.deviceId;
    doc["ip_address"] = WiFi.localIP().toString();

    String payload;
    serializeJson(doc, payload);

    int httpCode = http.POST(payload);
    bool success = false;

    if (httpCode > 0)
    {
        if (httpCode == 200)
        {
            success = true;
            Serial.println("[API] Heartbeat sent");
        }
    }
    else
    {
        Serial.printf("[API] Heartbeat failed: %s\n", http.errorToString(httpCode).c_str());
    }

    http.end();
    return success;
}

void MagicHomeClient::saveRelayStates()
{
    preferences_.begin("relays", false);
    for (size_t i = 0; i < config_.relayCount; i++)
    {
        String key = "relay_" + String(config_.relays[i].pin);
        preferences_.putBool(key.c_str(), config_.relays[i].state);
    }
    preferences_.end();
    Serial.println("[Storage] Relay states saved");
}

void MagicHomeClient::loadRelayStates()
{
    preferences_.begin("relays", true);
    for (size_t i = 0; i < config_.relayCount; i++)
    {
        String key = "relay_" + String(config_.relays[i].pin);
        if (preferences_.isKey(key.c_str()))
        {
            bool state = preferences_.getBool(key.c_str(), false);
            digitalWrite(config_.relays[i].pin, state ? HIGH : LOW);
            config_.relays[i].state = state;
            Serial.printf("[Storage] Restored relay pin %d to %s\n",
                          config_.relays[i].pin, state ? "ON" : "OFF");
        }
    }
    preferences_.end();
}

void MagicHomeClient::setupOTA()
{
    ArduinoOTA.setPort(3232);
    ArduinoOTA.setHostname("esp32-magic-home");
    ArduinoOTA.setPassword("admin");

    ArduinoOTA.onStart([]()
                       {
                           String type;
                           if (ArduinoOTA.getCommand() == U_FLASH)
                           {
                               type = "sketch";
                           }
                           else
                           {
                               type = "filesystem";
                           }
                           Serial.println("[OTA] Start updating " + type);
                       });

    ArduinoOTA.onEnd([]()
                     { Serial.println("\n[OTA] Update complete"); });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                          { Serial.printf("[OTA] Progress: %u%%\r", (progress / (total / 100))); });

    ArduinoOTA.onError([](ota_error_t error)
                       {
                           Serial.printf("[OTA] Error[%u]: ", error);
                           if (error == OTA_AUTH_ERROR)
                               Serial.println("Auth Failed");
                           else if (error == OTA_BEGIN_ERROR)
                               Serial.println("Begin Failed");
                           else if (error == OTA_CONNECT_ERROR)
                               Serial.println("Connect Failed");
                           else if (error == OTA_RECEIVE_ERROR)
                               Serial.println("Receive Failed");
                           else if (error == OTA_END_ERROR)
                               Serial.println("End Failed");
                       });

    ArduinoOTA.begin();
    Serial.println("[OTA] Ready");
    Serial.print("[OTA] IP address: ");
    Serial.println(WiFi.localIP());
}

void MagicHomeClient::setupMDNS()
{
    if (MDNS.begin("esp32-magic-home"))
    {
        Serial.println("[mDNS] Responder started");
        Serial.println("[mDNS] Access at: http://esp32-magic-home.local");

        MDNS.addService("http", "tcp", 80);
        MDNS.addService("ws", "tcp", 80);
    }
    else
    {
        Serial.println("[mDNS] Failed to start");
    }
}

static hw_timer_t *watchdogTimer = nullptr;

static void IRAM_ATTR resetModule()
{
    ets_printf("[Watchdog] Rebooting...\n");
    esp_restart();
}

void MagicHomeClient::setupWatchdog()
{
    watchdogTimer = timerBegin(0, 80, true);
    timerAttachInterrupt(watchdogTimer, &resetModule, true);
    timerAlarmWrite(watchdogTimer, 30000000, false);
    timerAlarmEnable(watchdogTimer);
    Serial.println("[Watchdog] Enabled (30s timeout)");
}

void MagicHomeClient::feedWatchdog()
{
    timerWrite(watchdogTimer, 0);
}

void MagicHomeClient::checkSchedules()
{
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo))
    {
        return;
    }

    int currentHour = timeinfo.tm_hour;
    int currentMinute = timeinfo.tm_min;

    for (int i = 0; i < numSchedules_; i++)
    {
        if (schedules_[i].enabled &&
            schedules_[i].hour == currentHour &&
            schedules_[i].minute == currentMinute)
        {
            setRelayState(schedules_[i].relayPin, schedules_[i].state);
            Serial.printf("[Schedule] Executed: Relay pin %d -> %s\n",
                          schedules_[i].relayPin,
                          schedules_[i].state ? "ON" : "OFF");
        }
    }
}

void MagicHomeClient::setupTime()
{
    const char *ntpServer = "pool.ntp.org";
    const long gmtOffset_sec = 0;
    const int daylightOffset_sec = 3600;

    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    Serial.println("[NTP] Time sync started");

    struct tm timeinfo;
    if (getLocalTime(&timeinfo))
    {
        Serial.println(&timeinfo, "[NTP] Current time: %A, %B %d %Y %H:%M:%S");
    }
}

float MagicHomeClient::measurePower(int relayPin)
{
    const int sensorPin = 34;
    const float sensitivity = 0.185;
    const float vRef = 3.3;

    (void)relayPin;

    int adcValue = analogRead(sensorPin);
    float voltage = (adcValue / 4095.0) * vRef;
    float current = (voltage - (vRef / 2.0)) / sensitivity;

    return abs(current * 230.0);
}

void MagicHomeClient::sendLogToServer(String level, String message)
{
    if (!isConnected_)
    {
        return;
    }

    StaticJsonDocument<256> doc;
    doc["type"] = "log";
    doc["level"] = level;
    doc["message"] = message;
    doc["timestamp"] = millis();
    doc["device"] = WiFi.macAddress();

    String output;
    serializeJson(doc, output);
    webSocket_.sendTXT(output);
    Serial.printf("[WebSocket] Sent: %s\n", output.c_str());
}

void MagicHomeClient::checkButton()
{
    if (digitalRead(BUTTON_PIN) == LOW)
    {
        if (!buttonPressed_)
        {
            buttonPressed_ = true;
            buttonPressTime_ = millis();
        }
        else if (millis() - buttonPressTime_ > 3000)
        {
            Serial.println("[Button] Factory reset triggered");
            preferences_.begin("relays", false);
            preferences_.clear();
            preferences_.end();
            ESP.restart();
        }
    }
    else
    {
        if (buttonPressed_ && millis() - buttonPressTime_ < 3000)
        {
            allRelaysState_ = !allRelaysState_;
            if (allRelaysState_)
            {
                turnAllRelaysOn();
            }
            else
            {
                turnAllRelaysOff();
            }
        }
        buttonPressed_ = false;
    }
}

void MagicHomeClient::updateEnergyStats()
{
    float currentPower = 0.0;
    for (size_t i = 0; i < config_.relayCount; i++)
    {
        if (config_.relays[i].state)
        {
            currentPower += measurePower(config_.relays[i].pin);
        }
    }

    if (currentPower > energy_.peakPower)
    {
        energy_.peakPower = currentPower;
    }

    unsigned long elapsed = millis() - energy_.startTime;
    float hours = elapsed / 3600000.0;
    energy_.totalKwh += (currentPower / 1000.0) * hours;
    energy_.startTime = millis();
}

void MagicHomeClient::sendEnergyData()
{
    StaticJsonDocument<256> doc;
    doc["type"] = "energy_data";
    doc["total_kwh"] = energy_.totalKwh;
    doc["peak_power"] = energy_.peakPower;
    doc["uptime"] = millis() / 1000;

    String output;
    serializeJson(doc, output);
    webSocket_.sendTXT(output);
    Serial.printf("[WebSocket] Sent: %s\n", output.c_str());
}
