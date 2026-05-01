# Magic Home Controller Library

ESP32 client library for Magic Home devices. It connects to WiFi, registers with a Magic Home server, opens a WebSocket for real-time control, manages relay outputs, and optionally reports DHT sensor data.

## Features

- WiFi setup with restart on failure
- WebSocket control and status updates
- Device registration and heartbeat API
- Relay persistence in ESP32 Preferences
- Optional DHT11/DHT22 sensor reporting
- OTA and mDNS helpers (present but not called by default)
- Simple button handling for all-on/all-off and factory reset
- Basic energy statistics payloads

## Requirements

- ESP32 Arduino core
- ArduinoJson
- WebSocketsClient
- Preferences (ESP32)
- DHT library

PlatformIO `library.json` is included for build metadata.

## Quick Start

1. Add this library to your PlatformIO or Arduino project.
2. Define relays and configuration.
3. Create a `MagicHomeClient`, call `begin()` in `setup()`, and `loop()` in `loop()`.

### Example

```cpp
#include <MagicHomeClient.h>
#include <DHT.h>

RelayConfig relays[] = {
    {23, "Relay 1", "Main Light", false},
    {22, "Relay 2", "Fan", false},
};

MagicHomeConfig config = {
    .ssid = "YOUR_WIFI",
    .password = "YOUR_PASS",
    .authToken = "YOUR_TOKEN",
    .deviceId = "device-001",
    .deviceName = "Living Room",
    .deviceDescription = "Living room controller",
    .statusLedPin = 2,
    .dhtPin = 4,
    .dhtType = DHT22,
    .relays = relays,
    .relayCount = sizeof(relays) / sizeof(relays[0]),
};

MagicHomeClient client(config);

void setup()
{
    client.begin();
}

void loop()
{
    client.loop();
}
```

## Configuration Notes

- Set `dhtType` to `0` to disable the sensor.
- Relay states are saved in Preferences under `relays` namespace.
- Button is hardcoded to `GPIO0` (`BUTTON_PIN`). A long press (>3s) clears relay states and reboots.
- Status LED is driven by `statusLedPin` and blinks when disconnected.

## Server Integration

The client connects to a hardcoded host:

- Host: `magichome.co.in`
- SSL: enabled
- Port: 443

Endpoints used:

- `POST /api/device/register`
- `POST /api/device/heartbeat`
- `GET /ws/<deviceId>/?client=controller` (WebSocket)

WebSocket messages handled:

- `relay_update` (by `pin` or `relay_id`)
- `request_status`
- `request_sensor`
- `action` values: `status`, `sensor`, `get_sensor`, `temperature`, `all_on`, `all_off`, `on`, `off`, `toggle`

Status payloads sent:

- `relay_status` or `relay_status_all`
- `sensor_data`
- `device_info`
- `energy_data`
- `log`

## Project Review Notes

- `setupOTA()`, `setupMDNS()`, `setupWatchdog()`, `checkSchedules()`, `setupTime()`, `checkButton()`, `updateEnergyStats()`, `sendEnergyData()` are implemented but never called in `begin()` or `loop()`. If these are required, add calls in the main flow.
- OTA password is hardcoded to `admin` in `setupOTA()`. Consider making it configurable.
- The WebSocket auth is sent via header: `Authorization: Token <authToken>`.
- `sendRelayStatus()` and `sendSensorData()` are called on connect; there is no backoff for repeated registration failures beyond the heartbeat interval.

## License

See the LICENSE file.
