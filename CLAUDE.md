# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is an ESP32-based smart doorbell system using PlatformIO framework. The system features dual-mode input detection (digital/analog), MQTT communication, OTA updates, audio playback via DFPlayer Mini, and front door relay control.

## Core Architecture

### Hardware Platform
- **Target**: NodeMCU32S (ESP32)
- **Framework**: Arduino
- **Build System**: PlatformIO

### Key Components
- **Input Modes**: Digital (simple button presses) or Analog (voltage analysis for building integration)
- **Audio System**: DFPlayer Mini module with SD card storage
- **Communication**: WiFi with MQTT protocol (primary/backup servers)
- **Door Control**: Relay-based front door unlock mechanism
- **Safety Features**: Hardware watchdog timer, button debouncing, cooldown periods

### Configuration System
The system uses a dual-layer configuration approach:
1. **Compile-time config**: `src/config.h` (WiFi, MQTT, OTA credentials)
2. **Runtime config**: EEPROM-stored settings (tracks, volumes, timing)
3. **Input mode config**: `src/input_config.h` (digital vs analog detection)

## Development Commands

### Build and Upload
```bash
# Initial USB upload
pio run --target upload

# OTA updates (after first upload)
pio run --target upload --upload-port doorbell.local
```

### Monitoring and Debugging
```bash
# Serial monitor
pio device monitor

# Enable debug build (uncomment in platformio.ini)
# build_flags = -DDEBUG_ENABLE
```

### Configuration Management
```bash
# Copy example config (required for first build)
cp src/config.h.example src/config.h
```

## Code Architecture

### Main Components (`src/main.cpp`)
- **Config Management**: EEPROM-based persistent configuration with factory reset capability
- **Input Detection**: Dual-mode system supporting both digital buttons and analog voltage analysis
- **MQTT Handler**: Subscribe/publish system for remote control and status reporting
- **Audio Control**: DFPlayer Mini integration with percentage-based volume control
- **Relay Control**: Timed door unlock sequence with safety timeouts
- **OTA System**: Network-based firmware updates with password protection

### Key Data Structures
- `Config`: Main configuration struct stored in EEPROM
- `ButtonState`: Button press tracking with debouncing and cooldown
- `ADCSession`/`ADCReading`: Analog input analysis (when using analog mode)
- `PlayRequest`: Audio playback queue management

### Pin Assignments
- GPIO27: Downstairs button (digital) / ADC1 input (analog)
- GPIO14: Door button (digital) / ADC2 input (analog)
- GPIO32/33: ADC inputs for analog voltage monitoring
- GPIO16/17: DFPlayer Mini UART communication
- GPIO26: DFPlayer Mini BUSY signal
- GPIO4: Front door relay control

### Input Mode Selection
The system supports two input detection modes configured in `input_config.h`:
- **Digital Mode**: Simple HIGH/LOW button detection
- **Analog Mode**: Voltage pattern analysis for integration with existing building systems

## MQTT Communication

### Control Topics (Subscribe)
- `doorbell/set/config`: Update device configuration
- `doorbell/set/button/{downstairs|door}`: Configure button settings
- `doorbell/simulate/{downstairs|door}`: Trigger button press simulation
- `doorbell/play/{track_number}`: Direct track playback
- `doorbell/timer/set`: Configure timer with track/volume
- `doorbell/command`: Device commands (open_front_door)
- `doorbell/system/reboot`: System restart

### Status Topics (Publish)
- `doorbell/status`: Device status and configuration
- `doorbell/event`: Button press events
- `doorbell/debug`: Debug messages (when enabled)
- `doorbell/error`: Error conditions
- `doorbell/timer/status`: Timer state changes

## Support Scripts

### Python Integration
- `mqtt_to_ntfy.py`: Bridge MQTT events to Ntfy notifications
- `mqtt_to_pushover.py`: Bridge MQTT events to Pushover notifications
- `session_logger.py`: Debug tool for logging MQTT sessions
- `scripts/pre_build.py`: PlatformIO build script for OTA password extraction

### Configuration Files
- `mqtt_config.ini`: Configuration template for notification scripts
- `requirements.txt`: Python dependencies for support scripts

## Testing and Development

### MQTT Testing Commands
```bash
# Get current configuration
mosquitto_pub -t "doorbell/get/config" -m ""

# Simulate button press
mosquitto_pub -t "doorbell/simulate/door" -m ""

# Monitor all topics
mosquitto_sub -t "doorbell/#" -v
```

### Factory Reset Procedure
1. Power off device
2. Hold both buttons while powering on
3. Release after 1 second
4. Device resets to default configuration

## File Structure Guidelines

- **Hardware abstraction**: Pin definitions and hardware interfaces in main.cpp header
- **Configuration management**: Separate config files for compile-time vs runtime settings
- **Mode selection**: Input detection algorithms isolated in input_config.h
- **Build automation**: Pre-build scripts handle OTA password extraction
- **Support tools**: Python scripts for external integration and debugging