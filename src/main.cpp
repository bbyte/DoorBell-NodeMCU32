#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <DFRobotDFPlayerMini.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include "esp_task_wdt.h"
#include "config.h"
#include "input_config.h"

// Debug macros
#ifdef DEBUG_ENABLE
    #define DEBUG_PRINT(x) if (config.debug_enabled) Serial.print(x)
    #define DEBUG_PRINTLN(x) if (config.debug_enabled) Serial.println(x)
    #define DEBUG_PRINTF(x, ...) if (config.debug_enabled) Serial.printf(x, __VA_ARGS__)
    #define MQTT_DEBUG(x) if (config.debug_enabled) mqtt.publish("doorbell/debug", x)
    #define MQTT_DEBUG_F(...) if (config.debug_enabled) { char debug_msg[512]; snprintf(debug_msg, sizeof(debug_msg), __VA_ARGS__); mqtt.publish("doorbell/debug", debug_msg); }
#else
    #define DEBUG_PRINT(x)
    #define DEBUG_PRINTLN(x)
    #define DEBUG_PRINTF(x, ...)
    #define MQTT_DEBUG(x)
    #define MQTT_DEBUG_F(...)
#endif

// Pin definitions
const int BUTTON_DOWNSTAIRS = 27;  // GPIO27 for downstairs button
const int BUTTON_DOOR = 14;         // GPIO14 for door button
const int DFPLAYER_RX = 16;        // GPIO16 for DFPlayer RX
const int DFPLAYER_TX = 17;        // GPIO17 for DFPlayer TX
const int DFPLAYER_BUSY = 26;      // GPIO26 for DFPlayer BUSY pin
const int ADC_PIN1 = 32;           // GPIO32 for ADC reading
const int ADC_PIN2 = 33;           // GPIO33 for ADC reading
// Built-in LED pin is already defined in framework

// EEPROM size and addresses
#define EEPROM_SIZE 512
#define EEPROM_VALID_ADDR 0
#define EEPROM_CONFIG_ADDR 1

// Configuration structure
struct Config {
    char wifi_ssid[32];
    char wifi_password[64];
    char backup_wifi_ssid[32];
    char backup_wifi_password[64];
    char mqtt_server[64];
    char mqtt_port[6];
    char backup_mqtt_server[64];
    char backup_mqtt_port[6];
    char mqtt_user[32];
    char mqtt_password[32];
    uint8_t downstairs_track;
    uint8_t door_track;
    uint8_t downstairs_volume;     // Volume in percentage (0-100)
    uint8_t door_volume;           // Volume in percentage (0-100)
    uint16_t button_cooldown_ms;   // Cooldown period in milliseconds (default 15000)
    uint16_t volume_reset_ms;      // Time after which volume resets to 0 (default 60000)
    bool debug_enabled;            // MQTT-controlled debug flag
};

Config config;

// Global objects
WiFiClient espClient;
PubSubClient mqtt(espClient);
DFRobotDFPlayerMini dfPlayer;
HardwareSerial dfPlayerSerial(2); // Using UART2

// Global variables for button states and timing
struct ButtonState {
    bool isPressed;
    bool wasPressed;
    unsigned long pressStartTime;
    unsigned long lastValidPressTime;
    bool isValidPress;
};

ButtonState buttonStates[2];  // Index 0 for DOWNSTAIRS, 1 for DOOR

// Global variables for timing and state
unsigned long lastPlayTime = 0;
unsigned long volumeResetTimer = 0;
unsigned long ledStartTime = 0;
unsigned long lastAdcRead = 0;      // Timestamp for last ADC reading
unsigned long currentTime = 0;      // Current time in milliseconds
unsigned long lastPlaybackCheck = 0;  // New variable to track last playback check
bool isPlaying = false;
bool normalLedOn = false;

// Add structure for pending play requests
struct PlayRequest {
    bool pending;
    int track;
    int volume;  // Volume in percentage (0-100)
} playRequest;

// Timer structure
struct Timer {
    bool active;
    unsigned long startTime;
    unsigned long durationMs;
    int track;
    int volume;
} timer = {false, 0, 0, 0, 0};

// Global variables for session tracking
#ifdef INPUT_MODE_ANALOG
ADCSession currentSession = {0, 0, false, 0.0, -1, 0};
unsigned long lastValidVoltage = 0;  // Timestamp of last valid voltage reading
#endif

// Function declarations
void setupWiFi();
void setupMQTT();
void setupDFPlayer();
void callback(char* topic, byte* payload, unsigned int length);
void reconnect();
void loadConfig();
void saveConfig();
void publishConfig();
void clearEEPROM();
void publishDeviceStatus();
void checkButtons();
void handleNormalDoorbell(int buttonIndex);
void handleSimulatedButton(int button);
void checkADC();

// Helper function to convert percentage volume to DFPlayer volume (0-30)
uint8_t percentToVolume(uint8_t percent) {
    return (percent * 30) / 100;
}

// Function to check if a button press is valid
bool isValidButtonPress(ButtonState& state, unsigned long currentTime) {
    if (state.isPressed && !state.wasPressed) {
        // Button just pressed
        state.pressStartTime = currentTime;
        state.wasPressed = true;
        state.isValidPress = false;
    } 
    else if (state.isPressed && state.wasPressed) {
        // Button is still pressed - check duration
        if (currentTime - state.pressStartTime >= 200) {
            state.isValidPress = true;
            state.lastValidPressTime = currentTime;
        }
    }
    else if (!state.isPressed && state.wasPressed) {
        // Button was released
        state.wasPressed = false;
        state.isValidPress = false;
    }
    
    return state.isValidPress;
}

// Function to check all buttons and return their states
void checkButtons() {
    static int prevDownstairsState = -1;  // Initialize to -1 to ensure first read is always sent
    static int prevDoorState = -1;
    unsigned long currentTime = currentTime;
    
    // Check downstairs button
    int downstairsState = digitalRead(BUTTON_DOWNSTAIRS);
    buttonStates[0].isPressed = downstairsState == HIGH;
    bool downstairsValid = isValidButtonPress(buttonStates[0], currentTime);
    if (downstairsState != prevDownstairsState) {
        MQTT_DEBUG_F("Downstairs button: digitalRead=%d, isPressed=%d, wasPressed=%d, isValid=%d", 
                     downstairsState, buttonStates[0].isPressed, buttonStates[0].wasPressed, downstairsValid);
        prevDownstairsState = downstairsState;
    }
    
    // Check door button
    int doorState = digitalRead(BUTTON_DOOR);
    buttonStates[1].isPressed = doorState == HIGH;
    bool doorValid = isValidButtonPress(buttonStates[1], currentTime);
    if (doorState != prevDoorState) {
        MQTT_DEBUG_F("Door button: digitalRead=%d, isPressed=%d, wasPressed=%d, isValid=%d", 
                     doorState, buttonStates[1].isPressed, buttonStates[1].wasPressed, doorValid);
        prevDoorState = doorState;
    }
}

void setup() {
    Serial.begin(115200);

    // Initialize watchdog: restart if loop is blocked for over 10 seconds
    esp_task_wdt_init(10, true);
    esp_task_wdt_add(NULL);

    MQTT_DEBUG_F("Starting Doorbell...");
    
    // Initialize EEPROM
    EEPROM.begin(EEPROM_SIZE);
    
    // Setup hardware
    pinMode(BUTTON_DOWNSTAIRS, INPUT_PULLDOWN);
    pinMode(BUTTON_DOOR, INPUT_PULLDOWN);
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(DFPLAYER_BUSY, INPUT);  // Configure BUSY pin as input
    digitalWrite(LED_BUILTIN, LOW);
    
    // Configure ADC resolution
    analogReadResolution(12);  // Set ADC resolution to 12 bits
    
    // Configure ADC pins
    pinMode(ADC_PIN1, INPUT);
    pinMode(ADC_PIN2, INPUT);
    
    // Check if both buttons are pressed during startup to reset config
    if (digitalRead(BUTTON_DOWNSTAIRS) == HIGH && digitalRead(BUTTON_DOOR) == HIGH) {
        MQTT_DEBUG_F("Both buttons pressed during startup - resetting to defaults");
        clearEEPROM();
        delay(1000); // Give some time to release buttons
    }
    
    // Load configuration
    loadConfig();
    
    // Initialize DFPlayer
    dfPlayerSerial.begin(9600, SERIAL_8N1, DFPLAYER_RX, DFPLAYER_TX);
    delay(200);  // Give DFPlayer time to initialize
    
    bool dfPlayerInitialized = false;
    int retries = 3;
    while (retries > 0 && !dfPlayerInitialized) {
        if (dfPlayer.begin(dfPlayerSerial)) {
            MQTT_DEBUG_F("DFPlayer initialized successfully");
            dfPlayer.setTimeOut(500);
            dfPlayer.volume(0);  // Start with volume at 0
            dfPlayer.EQ(DFPLAYER_EQ_NORMAL);
            dfPlayer.outputDevice(DFPLAYER_DEVICE_SD);
            dfPlayerInitialized = true;
            break;
        }
        MQTT_DEBUG_F("Failed to initialize DFPlayer, retrying...");
        delay(1000);
        retries--;
    }
    if (!dfPlayerInitialized) {
        MQTT_DEBUG_F("Unable to begin DFPlayer after multiple attempts");
    }
    
    // Setup WiFi and MQTT
    setupWiFi();
    
    // Give some time for WiFi to fully stabilize
    delay(500);
    
    // Initialize OTA first, before mDNS
    ArduinoOTA.setHostname("doorbell");
    ArduinoOTA.setPort(3232);
    
    ArduinoOTA.setPassword(OTA_PASSWORD);
    
    ArduinoOTA.onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH) {
            type = "sketch";
        } else {
            type = "filesystem";
        }
        MQTT_DEBUG_F("Start updating %s", type);
    });
    
    ArduinoOTA.onEnd([]() {
        MQTT_DEBUG_F("\nEnd");
    });
    
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        // MQTT_DEBUG_F("Progress: %u%%\r", (progress / (total / 100)));
    });
    
    ArduinoOTA.onError([](ota_error_t error) {
        MQTT_DEBUG_F("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) {
            MQTT_DEBUG_F("Auth Failed");
        } else if (error == OTA_BEGIN_ERROR) {
            MQTT_DEBUG_F("Begin Failed");
        } else if (error == OTA_CONNECT_ERROR) {
            MQTT_DEBUG_F("Connect Failed");
        } else if (error == OTA_RECEIVE_ERROR) {
            MQTT_DEBUG_F("Receive Failed");
        } else if (error == OTA_END_ERROR) {
            MQTT_DEBUG_F("End Failed");
        }
    });
    
    ArduinoOTA.begin();
    MQTT_DEBUG_F("OTA initialized");
    MQTT_DEBUG_F("OTA available on IP: %s Port: 3232\n", WiFi.localIP().toString().c_str());
    
    // Try mDNS after OTA is set up
    if (WiFi.status() == WL_CONNECTED) {
        if (MDNS.begin("doorbell")) {
            MQTT_DEBUG_F("mDNS responder started");
            MDNS.addService("arduino", "tcp", 3232); // Add service for OTA
        } else {
            MQTT_DEBUG_F("Error setting up MDNS responder!");
        }
    } else {
        MQTT_DEBUG_F("WiFi not connected - skipping MDNS setup");
    }
    
    setupMQTT();
    
    // Publish initial device status
    publishDeviceStatus();
}

void loop() {
    ArduinoOTA.handle();

    // Feed watchdog to prevent unwanted resets
    esp_task_wdt_reset();
    
    if (!mqtt.connected()) {
        reconnect();
    }
    mqtt.loop();

    currentTime = millis();  // Update current time

    // Check timer
    if (timer.active) {
        unsigned long elapsed = currentTime - timer.startTime;
        if (elapsed >= timer.durationMs) {
            timer.active = false;
            
            // Play the specified track
            playRequest.pending = true;
            playRequest.track = timer.track;
            playRequest.volume = timer.volume;
            
            // Publish timer ended message
            char endMsg[128];
            snprintf(endMsg, sizeof(endMsg), 
                    "{\"status\":\"ended\",\"seconds\":%lu,\"track\":%d,\"volume\":%d}", 
                    timer.durationMs/1000, timer.track, timer.volume);
            mqtt.publish("doorbell/timer/status", endMsg);
            MQTT_DEBUG("Timer ended, playing track");
        }
    }

    // Check if playback has finished
    if (currentTime - lastPlaybackCheck >= 200) {  // Still keep the 200ms check interval
        lastPlaybackCheck = currentTime;
        
        bool isBusy = digitalRead(DFPLAYER_BUSY);
        
        if (isBusy && isPlaying) {  // If HIGH (not busy) and was playing, playback has finished
            MQTT_DEBUG_F("Playback finished (BUSY pin HIGH)");
            isPlaying = false;
            digitalWrite(LED_BUILTIN, LOW);  // Turn off LED
            dfPlayer.volume(0);  // Reset volume after playback
            MQTT_DEBUG("Ready for next playback");
        }
    }
    
    // Handle pending play requests
    if (playRequest.pending && !isPlaying) {
        MQTT_DEBUG_F("Starting playback - Track: %d, Volume: %d%%", playRequest.track, playRequest.volume);
        dfPlayer.volume(percentToVolume(playRequest.volume));
        MQTT_DEBUG("Volume set");
        dfPlayer.play(playRequest.track);
        MQTT_DEBUG("Track played");
        delay(500);
        lastPlayTime = currentTime;
        volumeResetTimer = currentTime;
        isPlaying = true;
        digitalWrite(LED_BUILTIN, HIGH);
        playRequest.pending = false;
        MQTT_DEBUG("Playback started");
    }

    // Check and handle buttons
#ifdef INPUT_MODE_DIGITAL
    checkButtons();
#else
    checkADC();
#endif

    // Handle button actions
    if (buttonStates[0].isValidPress) {  // Downstairs button
        handleNormalDoorbell(0);
    }
    if (buttonStates[1].isValidPress) {  // Door button
        handleNormalDoorbell(1);
    }
}

void setupWiFi() {
    delay(10);
    MQTT_DEBUG_F("\n=== WiFi Setup ===");
    
    // Set WiFi mode explicitly
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();  // Disconnect from any previous connections
    delay(100);
    
    MQTT_DEBUG_F("Attempting to connect to primary WiFi SSID: %s\n", config.wifi_ssid);
    WiFi.begin(config.wifi_ssid, config.wifi_password);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        MQTT_DEBUG_F(".");
        attempts++;
    }
    
    if (WiFi.status() != WL_CONNECTED && strlen(config.backup_wifi_ssid) > 0) {
        MQTT_DEBUG_F("\nPrimary WiFi connection failed");
        MQTT_DEBUG_F("Attempting to connect to backup WiFi SSID: %s\n", config.backup_wifi_ssid);
        WiFi.begin(config.backup_wifi_ssid, config.backup_wifi_password);
        attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            delay(500);
            MQTT_DEBUG_F(".");
            attempts++;
        }
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        MQTT_DEBUG_F("\nWiFi connected successfully!");
        MQTT_DEBUG_F("Connected to SSID: %s\n", WiFi.SSID().c_str());
        MQTT_DEBUG_F("IP address: %s\n", WiFi.localIP().toString().c_str());
        MQTT_DEBUG_F("Signal strength (RSSI): %d dBm\n", WiFi.RSSI());
        
        // Initialize MDNS
        bool mdnsStarted = MDNS.begin("doorbell");
        if (!mdnsStarted) {
            MQTT_DEBUG_F("Error setting up MDNS responder!");
        } else {
            MQTT_DEBUG_F("mDNS responder started");
            MDNS.addService("arduino", "tcp", 3232); // Advertise OTA service
        }
    } else {
        MQTT_DEBUG_F("\nFailed to connect to any WiFi network");
        MQTT_DEBUG_F("Device will continue to work in offline mode");
    }
    MQTT_DEBUG_F("=================\n");
}

void setupMQTT() {
    MQTT_DEBUG_F("\n=== MQTT Setup ===");
    MQTT_DEBUG_F("Connecting to MQTT server: %s:%s\n", config.mqtt_server, config.mqtt_port);
    mqtt.setServer(config.mqtt_server, atoi(config.mqtt_port));
    mqtt.setCallback(callback);
}

void setupDFPlayer() {
    dfPlayerSerial.begin(9600, SERIAL_8N1, DFPLAYER_RX, DFPLAYER_TX);
    delay(200);  // Give DFPlayer time to initialize
    
    bool dfPlayerInitialized = false;
    int retries = 3;
    while (retries > 0 && !dfPlayerInitialized) {
        if (dfPlayer.begin(dfPlayerSerial)) {
            MQTT_DEBUG_F("DFPlayer initialized successfully");
            dfPlayer.setTimeOut(500);
            dfPlayer.volume(0);  // Start with volume at 0
            dfPlayer.EQ(DFPLAYER_EQ_NORMAL);
            dfPlayer.outputDevice(DFPLAYER_DEVICE_SD);
            dfPlayerInitialized = true;
            break;
        }
        MQTT_DEBUG_F("Failed to initialize DFPlayer, retrying...");
        delay(1000);
        retries--;
    }
    if (!dfPlayerInitialized) {
        MQTT_DEBUG_F("Unable to begin DFPlayer after multiple attempts");
    }
}

void callback(char* topic, byte* payload, unsigned int length) {
    // Safety check for topic
    if (!topic || strlen(topic) < 2) {
        MQTT_DEBUG("Error: Invalid topic received");
        return;
    }

    // Create a local copy of the topic to ensure it's properly null-terminated
    char topic_copy[128];
    strncpy(topic_copy, topic, sizeof(topic_copy) - 1);
    topic_copy[sizeof(topic_copy) - 1] = '\0';
    
    // Create a buffer for the payload
    char message[length + 1];
    memcpy(message, payload, length);
    message[length] = '\0';
    
    // Debug message
    MQTT_DEBUG_F("Received on topic '%s': %s", topic_copy, message);

    // List of commands that don't require JSON payload
    const char* noJsonCommands[] = {
        "doorbell/simulate/door",
        "doorbell/simulate/downstairs",
        "doorbell/get/config",
        "doorbell/get/all",
        "doorbell/timer/stop"
    };
    const int noJsonCommandsCount = sizeof(noJsonCommands) / sizeof(noJsonCommands[0]);

    // List of commands that require JSON payload
    const char* jsonCommands[] = {
        "doorbell/set/button/downstairs",
        "doorbell/set/button/door",
        "doorbell/set/config",
        "doorbell/timer/set"
    };
    const int jsonCommandsCount = sizeof(jsonCommands) / sizeof(jsonCommands[0]);

    bool isCommand = false;

    // Check for special commands first (reboot and play)
    if (strcmp(topic_copy, "doorbell/system/reboot") == 0) {
        isCommand = true;
        if (strcmp(message, "REBOOT") == 0) {
            MQTT_DEBUG("Rebooting device...");
            mqtt.loop();
            delay(100);
            ESP.restart();
        } else {
            MQTT_DEBUG("To reboot, send 'REBOOT' to doorbell/system/reboot");
        }
        return;
    }
    
    // Handle play command (special format)
    if (strncmp(topic_copy, "doorbell/play/", 14) == 0) {
        isCommand = true;
        const char* track_str = topic_copy + 14;
        int track = atoi(track_str);
        char debug_msg[64];
        snprintf(debug_msg, sizeof(debug_msg), "Received play command for track %d", track);
        MQTT_DEBUG(debug_msg);
        if (track > 0) {
            MQTT_DEBUG("Queueing track to play");
            playRequest.pending = true;
            playRequest.track = track;
            playRequest.volume = 100; // max volume in percentage
        }
        return;
    }

    // Check if command is in no-JSON list
    for (int i = 0; i < noJsonCommandsCount; i++) {
        if (strcmp(topic_copy, noJsonCommands[i]) == 0) {
            isCommand = true;
            // Handle no-JSON commands
            if (strcmp(noJsonCommands[i], "doorbell/simulate/door") == 0) {
                MQTT_DEBUG("Simulating door button press");
                handleSimulatedButton(BUTTON_DOOR);
            }
            else if (strcmp(noJsonCommands[i], "doorbell/simulate/downstairs") == 0) {
                MQTT_DEBUG("Simulating downstairs button press");
                handleSimulatedButton(BUTTON_DOWNSTAIRS);
            }
            else if (strcmp(noJsonCommands[i], "doorbell/get/config") == 0) {
                MQTT_DEBUG("Getting config");
                publishConfig();
            }
            else if (strcmp(noJsonCommands[i], "doorbell/get/all") == 0) {
                MQTT_DEBUG("Getting all settings");
                publishConfig();
                publishDeviceStatus();
            }
            else if (strcmp(noJsonCommands[i], "doorbell/timer/stop") == 0) {
                if (timer.active) {
                    timer.active = false;
                    mqtt.publish("doorbell/timer/status", "{\"status\":\"stopped\"}");
                    MQTT_DEBUG("Timer stopped");
                } else {
                    mqtt.publish("doorbell/timer/status", "{\"status\":\"error\",\"message\":\"No active timer\"}");
                    MQTT_DEBUG("Error: No active timer to stop");
                }
            }
            return;
        }
    }

    // Check if command is in JSON list
    for (int i = 0; i < jsonCommandsCount; i++) {
        if (strcmp(topic_copy, jsonCommands[i]) == 0) {
            isCommand = true;
            
            // Parse JSON for commands that require it
            DynamicJsonDocument doc(200);
            DeserializationError error = deserializeJson(doc, message);
            
            if (error) {
                char error_msg[64];
                snprintf(error_msg, sizeof(error_msg), "Failed to parse JSON: %s", error.c_str());
                MQTT_DEBUG(error_msg);
                return;
            }

            // Handle JSON commands
            if (strcmp(topic_copy, "doorbell/timer/set") == 0) {
                if (timer.active) {
                    mqtt.publish("doorbell/timer/status", "{\"status\":\"error\",\"message\":\"Timer already active\"}");
                    MQTT_DEBUG("Error: Timer already active");
                    return;
                }

                if (!doc.containsKey("seconds") || !doc.containsKey("track") || !doc.containsKey("volume")) {
                    mqtt.publish("doorbell/timer/status", "{\"status\":\"error\",\"message\":\"Missing required fields\"}");
                    MQTT_DEBUG("Error: Missing required timer fields");
                    return;
                }

                int seconds = doc["seconds"].as<int>();
                if (seconds <= 0) {
                    mqtt.publish("doorbell/timer/status", "{\"status\":\"error\",\"message\":\"Invalid duration\"}");
                    MQTT_DEBUG("Error: Invalid timer duration");
                    return;
                }

                timer.active = true;
                timer.startTime = millis();
                timer.durationMs = (unsigned long)seconds * 1000;
                timer.track = doc["track"].as<int>();
                timer.volume = doc["volume"].as<int>();

                char statusMsg[128];
                snprintf(statusMsg, sizeof(statusMsg), 
                        "{\"status\":\"started\",\"seconds\":%d,\"track\":%d,\"volume\":%d}", 
                        seconds, timer.track, timer.volume);
                mqtt.publish("doorbell/timer/status", statusMsg);
                MQTT_DEBUG_F("Timer started for %d seconds", seconds);
            }
            else if (strcmp(topic_copy, "doorbell/set/button/downstairs") == 0) {
                MQTT_DEBUG("Setting downstairs button config");
                if (doc.containsKey("track")) {
                    config.downstairs_track = doc["track"];
                    char debug_msg[64];
                    snprintf(debug_msg, sizeof(debug_msg), "Set downstairs track to %d", config.downstairs_track);
                    MQTT_DEBUG(debug_msg);
                }
                if (doc.containsKey("volume")) {
                    config.downstairs_volume = doc["volume"];
                    char debug_msg[64];
                    snprintf(debug_msg, sizeof(debug_msg), "Set downstairs volume to %d%%", config.downstairs_volume);
                    MQTT_DEBUG(debug_msg);
                }
                saveConfig();
            }
            else if (strcmp(topic_copy, "doorbell/set/button/door") == 0) {
                MQTT_DEBUG("Setting door button config");
                if (doc.containsKey("track")) {
                    config.door_track = doc["track"];
                    char debug_msg[64];
                    snprintf(debug_msg, sizeof(debug_msg), "Set door track to %d", config.door_track);
                    MQTT_DEBUG(debug_msg);
                }
                if (doc.containsKey("volume")) {
                    config.door_volume = doc["volume"];
                    char debug_msg[64];
                    snprintf(debug_msg, sizeof(debug_msg), "Set door volume to %d%%", config.door_volume);
                    MQTT_DEBUG(debug_msg);
                }
                saveConfig();
            }
            else if (strcmp(topic_copy, "doorbell/set/config") == 0) {
                MQTT_DEBUG("Setting device config");
                // Update WiFi settings
                if (doc.containsKey("wifi_ssid")) {
                    strlcpy(config.wifi_ssid, doc["wifi_ssid"], sizeof(config.wifi_ssid));
                }
                if (doc.containsKey("wifi_password")) {
                    strlcpy(config.wifi_password, doc["wifi_password"], sizeof(config.wifi_password));
                }
                if (doc.containsKey("backup_wifi_ssid")) {
                    strlcpy(config.backup_wifi_ssid, doc["backup_wifi_ssid"], sizeof(config.backup_wifi_ssid));
                }
                if (doc.containsKey("backup_wifi_password")) {
                    strlcpy(config.backup_wifi_password, doc["backup_wifi_password"], sizeof(config.backup_wifi_password));
                }
                
                // Update MQTT settings
                if (doc.containsKey("mqtt_server")) {
                    strlcpy(config.mqtt_server, doc["mqtt_server"], sizeof(config.mqtt_server));
                }
                if (doc.containsKey("mqtt_port")) {
                    strlcpy(config.mqtt_port, doc["mqtt_port"], sizeof(config.mqtt_port));
                }
                if (doc.containsKey("backup_mqtt_server")) {
                    strlcpy(config.backup_mqtt_server, doc["backup_mqtt_server"], sizeof(config.backup_mqtt_server));
                }
                if (doc.containsKey("backup_mqtt_port")) {
                    strlcpy(config.backup_mqtt_port, doc["backup_mqtt_port"], sizeof(config.backup_mqtt_port));
                }
                
                // Update debug setting
                if (doc.containsKey("debug_enabled")) {
                    config.debug_enabled = doc["debug_enabled"].as<bool>();
                    MQTT_DEBUG_F("Debug mode %s", config.debug_enabled ? "enabled" : "disabled");
                }
                
                saveConfig();
            }
            return;
        }
    }

    // If we get here and isCommand is still false, it means we didn't recognize the command
    if (!isCommand) {
        char errorMsg[128];
        snprintf(errorMsg, sizeof(errorMsg), "{\"status\":\"error\",\"message\":\"Unknown command: %s\"}", topic_copy);
        mqtt.publish("doorbell/error", errorMsg);
    }
}

void reconnect() {
    while (!mqtt.connected()) {
        MQTT_DEBUG_F("Attempting MQTT connection...");
        
        // Create a random client ID
        String clientId = "DoorBell-";
        clientId += String(random(0xffff), HEX);
        
        // Attempt to connect
        if (mqtt.connect(clientId.c_str(), config.mqtt_user, config.mqtt_password)) {
            MQTT_DEBUG_F("Connected to MQTT");
            
            // Subscribe to all set commands (require JSON)
            mqtt.subscribe("doorbell/set/#");
            // Subscribe to all get commands (no JSON)
            mqtt.subscribe("doorbell/get/#");
            // Subscribe to all simulation commands (no JSON)
            mqtt.subscribe("doorbell/simulate/#");
            // Subscribe to play commands (no JSON)
            mqtt.subscribe("doorbell/play/#");
            // Subscribe to system commands
            mqtt.subscribe("doorbell/system/#");
            // Subscribe to timer commands (but not status)
            mqtt.subscribe("doorbell/timer/set");
            mqtt.subscribe("doorbell/timer/stop");
            
            publishDeviceStatus();
        } else {                
            // Try backup MQTT server if configured
            if (strlen(config.backup_mqtt_server) > 0) {
                mqtt.setServer(config.backup_mqtt_server, atoi(config.backup_mqtt_port));
            }
            
            delay(5000);
        }
    }
}

void loadConfig() {
    if (EEPROM.read(EEPROM_VALID_ADDR) == 0xAA) {
        EEPROM.get(EEPROM_CONFIG_ADDR, config);
    } else {
        // Set defaults
        strlcpy(config.wifi_ssid, WIFI_SSID, sizeof(config.wifi_ssid));
        strlcpy(config.wifi_password, WIFI_PASSWORD, sizeof(config.wifi_password));
        strlcpy(config.mqtt_server, MQTT_SERVER, sizeof(config.mqtt_server));
        snprintf(config.mqtt_port, sizeof(config.mqtt_port), "%d", MQTT_PORT);
        
        config.downstairs_track = 1;
        config.door_track = 2;
        config.downstairs_volume = 50; // Default volume in percentage
        config.door_volume = 50;       // Default volume in percentage
        
        config.button_cooldown_ms = 15000; // 15 seconds default
        config.volume_reset_ms = 60000;   // 1 minute default
        
        config.debug_enabled = false; // Default debug mode
        
        saveConfig();
    }
}

void saveConfig() {
    EEPROM.write(EEPROM_VALID_ADDR, 0xAA);
    EEPROM.put(EEPROM_CONFIG_ADDR, config);
    EEPROM.commit();
}

void publishConfig() {
    DynamicJsonDocument configObj(512);
    
    // WiFi settings (mask passwords)
    configObj["wifi_ssid"] = config.wifi_ssid;
    configObj["wifi_password"] = "********";
    configObj["backup_wifi_ssid"] = config.backup_wifi_ssid;
    configObj["backup_wifi_password"] = "********";
    
    // MQTT settings (mask password)
    configObj["mqtt_server"] = config.mqtt_server;
    configObj["mqtt_port"] = config.mqtt_port;
    configObj["backup_mqtt_server"] = config.backup_mqtt_server;
    configObj["backup_mqtt_port"] = config.backup_mqtt_port;
    configObj["mqtt_user"] = config.mqtt_user;
    configObj["mqtt_password"] = "********";
    
    // Button configurations
    JsonObject downstairsConfig = configObj.createNestedObject("downstairs");
    downstairsConfig["track"] = config.downstairs_track;
    downstairsConfig["volume"] = config.downstairs_volume;
    
    JsonObject doorConfig = configObj.createNestedObject("door");
    doorConfig["track"] = config.door_track;
    doorConfig["volume"] = config.door_volume;
    
    // Timing configurations
    JsonObject timingConfig = configObj.createNestedObject("timing");
    timingConfig["button_cooldown_ms"] = config.button_cooldown_ms;
    timingConfig["volume_reset_ms"] = config.volume_reset_ms;
    
    // Debug configuration
    configObj["debug_enabled"] = config.debug_enabled;
    
    char buffer[512];
    ArduinoJson::serializeJson(configObj, buffer);
    
    MQTT_DEBUG("Published config");
}

void clearEEPROM() {
    MQTT_DEBUG_F("Clearing EEPROM...");
    for (int i = 0; i < EEPROM_SIZE; i++) {
        EEPROM.write(i, 0);
    }
    EEPROM.commit();
    MQTT_DEBUG_F("EEPROM cleared!");
}

void publishDeviceStatus() {
    if (!mqtt.connected()) {
        return;
    }
    
    // Create a JSON document for device status
    DynamicJsonDocument statusDoc(512);
    
    // Device information
    statusDoc["status"] = "online";
    statusDoc["ip"] = WiFi.localIP().toString();
    statusDoc["rssi"] = WiFi.RSSI();
    statusDoc["wifi_ssid"] = WiFi.SSID();
    statusDoc["hostname"] = "doorbell";
    
    // MQTT connection info
    statusDoc["mqtt_server"] = config.mqtt_server;
    statusDoc["mqtt_port"] = config.mqtt_port;
    
    // Current configuration
    JsonObject configObj = statusDoc.createNestedObject("config");
    configObj["downstairs_track"] = config.downstairs_track;
    configObj["door_track"] = config.door_track;
    configObj["downstairs_volume"] = config.downstairs_volume;
    configObj["door_volume"] = config.door_volume;
    
    char buffer[512];
    ArduinoJson::serializeJson(statusDoc, buffer);
    
    // Publish to status topic
    mqtt.publish("doorbell/status", buffer, true);  // retain flag set to true
    MQTT_DEBUG("Published device status");
}

// Function to handle normal doorbell operation
void handleNormalDoorbell(int buttonIndex) {
    // Check if we're within cooldown period or if melody is already playing
    if (isPlaying || (currentTime - lastPlayTime < config.button_cooldown_ms)) {
        return;
    }

    if (buttonIndex == 0) {  // DOWNSTAIRS
        dfPlayer.volume(percentToVolume(config.downstairs_volume));
        dfPlayer.play(config.downstairs_track);
        char eventMsg[128];
        snprintf(eventMsg, sizeof(eventMsg), 
                "{\"type\":\"button_press\",\"button\":\"downstairs\",\"track\":%d,\"volume\":%d}", 
                config.downstairs_track, config.downstairs_volume);
        mqtt.publish("doorbell/event", eventMsg);
    } else {  // DOOR
        dfPlayer.volume(percentToVolume(config.door_volume));
        dfPlayer.play(config.door_track);
        char eventMsg[128];
        snprintf(eventMsg, sizeof(eventMsg), 
                "{\"type\":\"button_press\",\"button\":\"door\",\"track\":%d,\"volume\":%d}", 
                config.door_track, config.door_volume);
        mqtt.publish("doorbell/event", eventMsg);
    }
    delay(500);
    
    lastPlayTime = currentTime;
    volumeResetTimer = currentTime;
    isPlaying = true;
    digitalWrite(LED_BUILTIN, HIGH);
}

// Function to handle simulated button presses from MQTT
void handleSimulatedButton(int button) {
    MQTT_DEBUG_F(button == BUTTON_DOOR ? "Simulating door button" : "Simulating downstairs button");
    handleNormalDoorbell(button == BUTTON_DOOR ? 1 : 0);
}

#ifdef INPUT_MODE_ANALOG
// Function to analyze the completed session and determine which button was pressed
void analyzeSession(ADCSession& session) {
    if (session.numReadings == 0) {
        DEBUG_PRINTLN("Session has no readings, skipping analysis");
        return;
    }
    
    unsigned long sessionDuration = session.endTime - session.startTime;
    if (sessionDuration < MIN_SESSION_DURATION) {
        DEBUG_PRINTF("Session too short: %lu ms (minimum: %d ms)\n", sessionDuration, MIN_SESSION_DURATION);
        return;
    }
    
    // The button type was already determined at session start
    if (session.buttonDetected == 1) {
        DEBUG_PRINTLN("Triggering DOOR button (determined at session start)");
        handleSimulatedButton(BUTTON_DOOR);
    } else if (session.buttonDetected == 0) {
        DEBUG_PRINTLN("Triggering DOWNSTAIRS button (determined at session start)");
        handleSimulatedButton(BUTTON_DOWNSTAIRS);
    } else {
        DEBUG_PRINTLN("No button was detected at session start, ignoring");
    }
    
    // Create JSON array of all readings
    DynamicJsonDocument doc(16384); // Adjust size based on MAX_SESSION_SAMPLES
    doc["status"] = "ended";
    doc["duration"] = sessionDuration;
    doc["max_voltage"] = session.maxVoltage;
    doc["button"] = session.buttonDetected;
    doc["num_readings"] = session.numReadings;
    
    JsonArray readings = doc.createNestedArray("readings");
    for (int i = 0; i < session.numReadings; i++) {
        JsonObject reading = readings.createNestedObject();
        reading["v1"] = session.readings[i].voltage1;
        reading["v2"] = session.readings[i].voltage2;
        reading["delta"] = session.readings[i].delta;
        reading["graph"] = session.readings[i].graph;
    }
    
    String output;
    ArduinoJson::serializeJson(doc, output);
    MQTT_DEBUG_F("Session data: %s", output.c_str());
}
#endif

// Function to read and process ADC values
void checkADC() {
#ifdef INPUT_MODE_ANALOG
    static unsigned long lastAdcRead = 0;
    static unsigned long lastDebugPrint = 0;
    currentTime = millis();
    
    if (currentTime - lastAdcRead >= ADC_SAMPLE_INTERVAL) {
        lastAdcRead = currentTime;
        
        // Read ADC values (12-bit resolution: 0-4095)
        int adc1_value = analogRead(ADC_PIN1);
        int adc2_value = analogRead(ADC_PIN2);
        
        // Convert to voltage (3.3V max)
        float voltage1 = (adc1_value * 3.3) / 4095.0;
        float voltage2 = (adc2_value * 3.3) / 4095.0;
        
        // Print debug info every 1 second when not in a session
        if (!currentSession.isActive && currentTime - lastDebugPrint >= 1000) {
            DEBUG_PRINTF("ADC Values - ADC1: %d (%.2fV), ADC2: %d (%.2fV)\n", 
                        adc1_value, voltage1, adc2_value, voltage2);
            lastDebugPrint = currentTime;
        }
        
        // Check if we need to start a new session (using threshold)
        if ((voltage1 >= ADC_THRESHOLD || voltage2 >= ADC_THRESHOLD) && !currentSession.isActive && !isPlaying) {
            DEBUG_PRINTF("Starting new session - ADC1: %.2fV, ADC2: %.2fV\n", voltage1, voltage2);
            currentSession.startTime = currentTime;
            currentSession.isActive = true;
            currentSession.maxVoltage = max(voltage1, voltage2);
            currentSession.numReadings = 0;
            
            // Determine button type based on which ADC started the session with >3V
            if (voltage2 >= ADC_THRESHOLD) {
                currentSession.buttonDetected = 1; // DOOR takes priority if ADC2 is high
                DEBUG_PRINTLN("Session started by DOOR button (ADC2)");
            } else if (voltage1 >= ADC_THRESHOLD) {
                currentSession.buttonDetected = 0; // DOWNSTAIRS only if ADC2 was not high
                DEBUG_PRINTLN("Session started by DOWNSTAIRS button (ADC1)");
            }
            
            MQTT_DEBUG("Session started");
        }
        
        // Update session data if active
        if (currentSession.isActive) {
            if (currentSession.numReadings >= MAX_SESSION_SAMPLES) {
                DEBUG_PRINTLN("Session buffer full, ending session");
                currentSession.isActive = false;
                return;
            }
            
            currentSession.maxVoltage = max(currentSession.maxVoltage, max(voltage1, voltage2));
            
            // Create new reading
            ADCReading& reading = currentSession.readings[currentSession.numReadings];
            reading.voltage1 = voltage1;
            reading.voltage2 = voltage2;
            reading.delta = currentTime - currentSession.startTime;
            
            // Create bar graphs with different characters for each voltage
            char* graph = reading.graph;
            int v1_bars = (int)((voltage1 * 20) / 3.3); // Scale to 20 characters max
            int v2_bars = (int)((voltage2 * 20) / 3.3);
            
            for(int i = 0; i < 20; i++) {
                graph[i] = (i < v1_bars) ? '#' : '.';      // First voltage uses #
                graph[i+21] = (i < v2_bars) ? '*' : '.';   // Second voltage uses *
            }
            graph[20] = ' '; // separator
            graph[41] = '\0';
            
            currentSession.numReadings++;
            
            // Print debug info every 100ms during session
            if (currentTime - lastDebugPrint >= 100) {
                DEBUG_PRINTF("Session ongoing - Readings: %d, ADC1: %.2fV, ADC2: %.2fV\n", 
                            currentSession.numReadings, voltage1, voltage2);
                lastDebugPrint = currentTime;
            }
            
            // Publish current reading for debug
            if (config.debug_enabled) {
                char msg[256];
                snprintf(msg, sizeof(msg), 
                        "{\"adc1_v\":%.2f,\"adc2_v\":%.2f,\"delta\":%lu,\"graph\":\"\033[38;5;46m%.*s\033[0m \033[38;5;220m%s\033[0m\"}", 
                        voltage1, voltage2, reading.delta, 20, graph, graph + 21);
                MQTT_DEBUG(msg);
            }
            
            // If voltage drops below threshold minus hysteresis OR minimum session duration met, end session
            if ((voltage1 < (ADC_THRESHOLD - ADC_HYSTERESIS) && voltage2 < (ADC_THRESHOLD - ADC_HYSTERESIS))) {
                // Check if this is just a temporary dropout
                if (currentTime - lastValidVoltage <= ADC_DROPOUT_TOLERANCE) {
                    // This is within our tolerance window, keep the session going
                    DEBUG_PRINTF("Voltage dropout detected but within tolerance window (%lu ms)\n", 
                               currentTime - lastValidVoltage);
                } else {
                    // Voltage has been low for too long, end the session
                    DEBUG_PRINTF("Ending session - Final voltages ADC1: %.2fV, ADC2: %.2fV\n", voltage1, voltage2);
                    currentSession.endTime = currentTime;
                    
                    // Only analyze if session meets minimum duration
                    if (currentSession.endTime - currentSession.startTime >= MIN_SESSION_DURATION) {
                        // Analyze the completed session
                        analyzeSession(currentSession);
                    } else {
                        DEBUG_PRINTF("Session too short (%lu ms), ignoring\n", 
                                   currentSession.endTime - currentSession.startTime);
                    }
                    
                    // Reset session
                    currentSession.isActive = false;
                    currentSession.maxVoltage = 0.0;
                    currentSession.buttonDetected = -1;
                    currentSession.numReadings = 0;
                }
            } else if (currentTime - currentSession.startTime >= MIN_SESSION_DURATION) {
                // Session has met minimum duration, end it
                DEBUG_PRINTF("Session reached minimum duration (%d ms), ending\n", MIN_SESSION_DURATION);
                currentSession.endTime = currentTime;
                analyzeSession(currentSession);
                
                // Reset session
                currentSession.isActive = false;
                currentSession.maxVoltage = 0.0;
                currentSession.buttonDetected = -1;
                currentSession.numReadings = 0;
            } else {
                // Update lastValidVoltage timestamp since we have good readings
                if (voltage1 >= ADC_THRESHOLD || voltage2 >= ADC_THRESHOLD) {
                    lastValidVoltage = currentTime;
                }
            }
        }
    }
#endif
}
