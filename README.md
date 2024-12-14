# Smart Doorbell with ESP32

This project implements a smart doorbell system using NodeMCU32S and DFPlayer Mini. It features WiFi connectivity with fallback options, MQTT communication, OTA updates, and configurable sound tracks for different buttons.

## Hardware Requirements

- NodeMCU32S
- DFPlayer Mini
- 2 push buttons
- SD card with audio files
- Speaker for DFPlayer

## Wiring

1. DFPlayer Mini:
   - RX -> GPIO16
   - TX -> GPIO17
   - BUSY -> GPIO26 (Required for stable operation)
   - VCC -> 5V
   - GND -> GND

2. Buttons:
   - Downstairs Button -> GPIO32 (and GND)
   - Door Button -> GPIO33 (and GND)

## Operation Modes

### Digital Mode
The default mode uses digital input (0 or 1) for button detection. This is straightforward and works well with simple button setups where the input signal is clean and stable.

### Analog Mode
When connecting to an existing building doorbell system, the input signal might be unstable or have varying voltage levels. In analog mode, the system reads voltage values and uses an algorithm to analyze the input pattern to determine valid button presses.

The current algorithm works by:
- Monitoring voltage changes over time
- Analyzing the pattern of voltage changes
- Using thresholds and timing to determine valid button presses

Note: The analog detection algorithm may need adjustment for different building systems as voltage patterns can vary. You can modify the thresholds and timing parameters in the configuration.

## Features

- Two configurable buttons (downstairs and door)
- WiFi connectivity with backup configuration
- MQTT communication with backup server
- OTA (Over-The-Air) updates
- Configurable tracks and volume for each button (volume in percentage 0-100%)
- Persistent configuration storage in EEPROM
- Fallback functionality when offline
- Timer functionality for scheduling events

## MQTT Topics and Commands

### Subscribe Topics

#### System Control
- `doorbell/system/reboot` - Reboot the device
  - Payload: Send "REBOOT" to confirm reboot
  ```bash
  mosquitto_pub -t "doorbell/system/reboot" -m "REBOOT"
  ```

#### Status and Configuration
- `doorbell/get/config` - Request current configuration
  - No payload required
  ```bash
  mosquitto_pub -t "doorbell/get/config" -m ""
  ```
- `doorbell/get/status` - Request current device status
  - No payload required
  ```bash
  mosquitto_pub -t "doorbell/get/status" -m ""
  ```

#### Button Configuration
- `doorbell/set/button/downstairs` - Configure downstairs button
  ```json
  {
    "track": 1,
    "volume": 50  // Volume in percentage (0-100)
  }
  ```
- `doorbell/set/button/door` - Configure door button
  ```json
  {
    "track": 2,
    "volume": 50  // Volume in percentage (0-100)
  }
  ```

#### Button Simulation
- `doorbell/simulate/door` - Simulate door button press
  - No payload required
  ```bash
  mosquitto_pub -t "doorbell/simulate/door" -m ""
  ```
- `doorbell/simulate/downstairs` - Simulate downstairs button press
  - No payload required
  ```bash
  mosquitto_pub -t "doorbell/simulate/downstairs" -m ""
  ```

#### Direct Play Control
- `doorbell/play/{track_number}` - Play specific track
  - Replace {track_number} with the track number to play
  ```bash
  mosquitto_pub -t "doorbell/play/1" -m ""
  ```

#### Device Configuration
- `doorbell/set/config` - Update device configuration
  ```json
  {
    "wifi_ssid": "YourWiFi",
    "wifi_password": "YourPassword",
    "backup_wifi_ssid": "BackupWiFi",
    "backup_wifi_password": "BackupPassword",
    "mqtt_server": "mqtt.local",
    "mqtt_port": "1883",
    "backup_mqtt_server": "backup.mqtt.local",
    "backup_mqtt_port": "1883"
  }
  ```

#### Timer Control
- `doorbell/timer/set` - Set a new timer
  ```json
  {
    "seconds": 126,    // Duration in seconds
    "track": 1,        // Track to play when timer ends
    "volume": 100      // Volume in percentage (0-100)
  }
  ```
- `doorbell/timer/stop` - Stop the current timer
  - No payload required
  ```bash
  mosquitto_pub -t "doorbell/timer/stop" -m ""
  ```

### Publish Topics (Device to Server)

- `doorbell/status` - Device status updates
  ```json
  {
    "system": "reboot",
    "message": "Rebooting device..."
  }
  ```
  ```json
  {
    "emergency": true,
    "message": "Emergency mode activated"
  }
  ```

- `doorbell/debug` - Debug messages from the device
  - Contains various operational messages and command confirmations

- `doorbell/timer/status` - Timer status updates
  ```json
  // Timer started
  {
    "status": "started",
    "seconds": 126,
    "track": 1,
    "volume": 100
  }
  ```
  ```json
  // Timer ended
  {
    "status": "ended",
    "seconds": 126,
    "track": 1,
    "volume": 100
  }
  ```
  ```json
  // Timer stopped
  {
    "status": "stopped"
  }
  ```
  ```json
  // Timer error
  {
    "status": "error",
    "message": "Timer already active"
  }
  ```

### Testing Examples

1. Get current configuration:
```bash
mosquitto_pub -t "doorbell/get/config" -m ""
```

2. Set door button configuration:
```bash
mosquitto_pub -t "doorbell/set/button/door" -m '{"track": 1, "volume": 50}'
```

3. Play specific track:
```bash
mosquitto_pub -t "doorbell/play/1" -m ""
```

4. Set a new timer:
```bash
mosquitto_pub -t "doorbell/timer/set" -m '{"seconds": 126, "track": 1, "volume": 100}'
```

5. Monitor all device messages:
```bash
mosquitto_sub -t "doorbell/#" -v
```

## Message Formats

### Volume Control
All volume settings in the system are specified as percentages (0-100%). The system automatically converts these percentages to the appropriate hardware values for the DFPlayer Mini module (0-30). For example:
- 50% volume → 15 on DFPlayer
- 100% volume → 30 on DFPlayer
- 0% volume → 0 on DFPlayer

### Button Configuration
```json
{
    "track": 1,
    "volume": 50  // Volume in percentage (0-100)
}
```

### Play Command
```json
{
    "track": 1,
    "volume": 50  // Volume in percentage (0-100)
}
```

### Configuration Update
```json
{
    "wifi_ssid": "YourWiFi",
    "wifi_password": "YourPassword",
    "backup_wifi_ssid": "BackupWiFi",
    "backup_wifi_password": "BackupPassword",
    "mqtt_server": "mqtt.local",
    "mqtt_port": "1883",
    "backup_mqtt_server": "backup.mqtt.local",
    "backup_mqtt_port": "1883"
}
```

## Setup Instructions

1. Install PlatformIO in VS Code
2. Clone this repository
3. Copy audio files to SD card (numbered tracks, e.g., 0001.mp3)
4. Update default WiFi and MQTT settings in the code
5. Build and upload the project
6. For subsequent updates, use OTA update functionality

## OTA Updates

The device will be available as "doorbell.local" for OTA updates. You can update it using PlatformIO or Arduino IDE.

## Configuration Instructions

1. Copy the example configuration file to create your own config:
   ```bash
   cp src/config.h.example src/config.h
   ```

2. Edit `src/config.h` with your personal settings:
   - WiFi credentials (primary and backup)
   - MQTT server details (primary and backup)
   - OTA password
   - Audio configuration

The `config.h` file is ignored by git to keep your personal settings private.

## Configuration Options

### WiFi Settings
- `WIFI_SSID` - Primary WiFi network name
- `WIFI_PASSWORD` - Primary WiFi password
- `BACKUP_WIFI_SSID` - Backup WiFi network name
- `BACKUP_WIFI_PASSWORD` - Backup WiFi password

### MQTT Settings
- `MQTT_SERVER` - Primary MQTT server address
- `MQTT_PORT` - Primary MQTT server port
- `BACKUP_MQTT_SERVER` - Backup MQTT server address
- `BACKUP_MQTT_PORT` - Backup MQTT server port
- `MQTT_USER` - MQTT username
- `MQTT_PASSWORD` - MQTT password

### OTA Settings
- `OTA_PASSWORD` - Password for OTA updates
- `OTA_HOSTNAME` - Device hostname for OTA updates

### Audio Configuration
- `DOWNSTAIRS_TRACK` - Track number for downstairs button
- `DOOR_TRACK` - Track number for door button
- `DEFAULT_VOLUME` - Default volume level
