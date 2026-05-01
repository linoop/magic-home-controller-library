#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <WebSocketsClient.h>

struct RelayConfig
{
    int pin;
    const char *name;
    const char *description;
    bool state;
};

struct MagicHomeConfig
{
    const char *ssid;
    const char *password;
    const char *authToken;

    const char *deviceId;
    const char *deviceName;
    const char *deviceDescription;

    uint8_t statusLedPin;

    int dhtPin;
    int dhtType; // 0 disables DHT, otherwise use DHT11/DHT22

    RelayConfig *relays;
    size_t relayCount;
};

class DHT;

class MagicHomeClient
{
public:
    explicit MagicHomeClient(const MagicHomeConfig &config);

    void begin();
    void loop();

private:
    struct SensorState
    {
        float temperature;
        float humidity;
        bool connected;
        bool valid;
        unsigned long lastUpdate;
        String errorMessage;
    };

    struct Schedule
    {
        int relayPin;
        int hour;
        int minute;
        bool state;
        bool enabled;
    };

    struct EnergyData
    {
        float totalKwh;
        unsigned long startTime;
        float peakPower;
    };

    void setupWiFi();
    void setupRelays();
    void setupWebSocket();
    void webSocketEvent(WStype_t type, uint8_t *payload, size_t length);
    void handleWebSocketMessage(uint8_t *payload, size_t length);
    void controlRelay(int pin, String action);
    void setRelayState(int pin, bool state);
    void turnAllRelaysOn();
    void turnAllRelaysOff();
    void sendRelayStatus(int specificPin = -1);
    void readSensorData();
    void sendSensorData();
    void sendDeviceInfo();
    void reconnectWebSocket();

    bool registerDevice();
    bool sendHeartbeat();
    String getServerURL(const char *endpoint);

    void saveRelayStates();
    void loadRelayStates();

    void setupOTA();
    void setupMDNS();
    void setupWatchdog();
    void feedWatchdog();
    void checkSchedules();
    void setupTime();
    float measurePower(int relayPin);
    void sendLogToServer(String level, String message);
    void checkButton();
    void updateEnergyStats();
    void sendEnergyData();

    MagicHomeConfig config_;
    WebSocketsClient webSocket_;
    Preferences preferences_;
    DHT *dht_;

    SensorState sensor_;
    Schedule schedules_[10];
    int numSchedules_;
    EnergyData energy_;

    unsigned long buttonPressTime_;
    bool buttonPressed_;
    bool allRelaysState_;

    bool isConnected_;
    bool isRegistered_;
    unsigned long lastSensorRead_;
    unsigned long lastReconnectAttempt_;
    unsigned long lastHeartbeat_;

    static const unsigned long kSensorIntervalMs = 30000;
    static const unsigned long kReconnectIntervalMs = 5000;
    static const unsigned long kHeartbeatIntervalMs = 60000;
};
