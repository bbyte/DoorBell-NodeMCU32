#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <DFRobotDFPlayerMini.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include "config.h"

// Debug macros
#ifdef DEBUG_ENABLE
    #define DEBUG_PRINT(x) Serial.print(x)
    #define DEBUG_PRINTLN(x) Serial.println(x)
    #define DEBUG_PRINTF(x, ...) Serial.printf(x, __VA_ARGS__)
#else
    #define DEBUG_PRINT(x)
    #define DEBUG_PRINTLN(x)
    #define DEBUG_PRINTF(x, ...)
#endif

// Pin definitions
const int BUTTON_DOWNSTAIRS = 27;  // GPIO27 for downstairs button
const int BUTTON_DOOR = 14;         // GPIO14 for door button
const int DFPLAYER_RX = 16;        // GPIO16 for DFPlayer RX
const int DFPLAYER_TX = 17;        // GPIO17 for DFPlayer TX
// Built-in LED pin is already defined in framework

// Emergency mode settings
const int EMERGENCY_LED_INTERVAL = 200; // LED flash interval in ms

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
    uint8_t downstairs_volume; // Volume in percentage (0-100)
    uint8_t door_volume;       // Volume in percentage (0-100)
    // Emergency configuration
    uint8_t emergency_track;
    uint8_t emergency_volume;  // Volume in percentage (0-100)
    uint16_t emergency_duration;     // Duration in seconds (0 = indefinite)
    uint8_t panic_press_threshold;   // Number of presses to trigger panic
    uint16_t panic_window;          // Window in seconds for panic detection
    uint16_t button_cooldown_ms;    // Cooldown period in milliseconds (default 15000)
    uint16_t volume_reset_ms;       // Time after which volume resets to 0 (default 60000)
};

Config config;

// Global objects
WiFiClient espClient;
PubSubClient mqtt(espClient);
DFRobotDFPlayerMini dfPlayer;
HardwareSerial dfPlayerSerial(2); // Using UART2

// Button debounce variables
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 200;
unsigned long buttonDownstairsPressStart = 0;  // Track when downstairs button started being pressed
unsigned long buttonDoorPressStart = 0;        // Track when door button started being pressed
const unsigned long BUTTON_PRESS_DURATION = 800;  // Duration required for a valid button press
bool buttonDownstairsWasPressed = false;     // Track if button was previously pressed
bool buttonDoorWasPressed = false;           // Track if button was previously pressed

// Global variables
bool emergencyMode = false;
unsigned long lastEmergencyLedToggle = 0;
unsigned long firstPressTime = 0;        // Time of first press in current sequence
int pressCount = 0;                      // Count of presses in current window
unsigned long emergencyStartTime = 0;    // When emergency mode was activated
unsigned long lastPlayTime = 0;          // Last time a melody was played
unsigned long volumeResetTimer = 0;      // Timer for volume reset
bool isPlaying = false;                  // Track if currently playing
unsigned long ledStartTime = 0;          // When LED started flashing for normal play
bool normalLedOn = false;                // Track if LED is on for normal play

// Add structure for pending play requests
struct PlayRequest {
    bool pending;
    int track;
    int volume; // Volume in percentage (0-100)
} playRequest = {false, 0, 0};

// Helper function to convert percentage volume to DFPlayer volume (0-30)
uint8_t percentToVolume(uint8_t percent) {
    return (percent * 30) / 100;
}

// Function declarations
void setupWiFi();
void setupOTA();
void setupMQTT();
void setupDFPlayer();
void handleButton(int button, bool simulated);
void callback(char* topic, byte* payload, unsigned int length);
void reconnect();
void loadConfig();
void saveConfig();
void publishConfig();
void clearEEPROM();
void publishDeviceStatus();

void setup() {
    Serial.begin(115200);
    
    // Initialize EEPROM
    EEPROM.begin(EEPROM_SIZE);
    
    // Setup hardware
    pinMode(BUTTON_DOWNSTAIRS, INPUT_PULLDOWN);
    pinMode(BUTTON_DOOR, INPUT_PULLDOWN);
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
    
    // Check if both buttons are pressed during startup to reset config
    if (digitalRead(BUTTON_DOWNSTAIRS) == HIGH && digitalRead(BUTTON_DOOR) == HIGH) {
        DEBUG_PRINTLN("Both buttons pressed during startup - resetting to defaults");
        clearEEPROM();
        delay(1000); // Give some time to release buttons
    }
    
    // Load configuration
    loadConfig();
    
    // Initialize DFPlayer
    dfPlayerSerial.begin(9600, SERIAL_8N1, DFPLAYER_RX, DFPLAYER_TX);
    setupDFPlayer();
    
    // Set initial volume to 0
    dfPlayer.volume(0);
    
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
        DEBUG_PRINTLN("Start updating " + type);
    });
    
    ArduinoOTA.onEnd([]() {
        DEBUG_PRINTLN("\nEnd");
    });
    
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        DEBUG_PRINTF("Progress: %u%%\r", (progress / (total / 100)));
    });
    
    ArduinoOTA.onError([](ota_error_t error) {
        DEBUG_PRINTF("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) DEBUG_PRINTLN("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) DEBUG_PRINTLN("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) DEBUG_PRINTLN("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) DEBUG_PRINTLN("Receive Failed");
        else if (error == OTA_END_ERROR) DEBUG_PRINTLN("End Failed");
    });
    
    ArduinoOTA.begin();
    DEBUG_PRINTLN("OTA initialized");
    DEBUG_PRINTF("OTA available on IP: %s Port: 3232\n", WiFi.localIP().toString().c_str());
    
    // Try mDNS after OTA is set up
    if (WiFi.status() == WL_CONNECTED) {
        if (MDNS.begin("doorbell")) {
            DEBUG_PRINTLN("mDNS responder started");
            MDNS.addService("arduino", "tcp", 3232); // Add service for OTA
        } else {
            DEBUG_PRINTLN("Error setting up MDNS responder!");
        }
    } else {
        DEBUG_PRINTLN("WiFi not connected - skipping MDNS setup");
    }
    
    setupMQTT();
    
    // Publish initial device status
    publishDeviceStatus();
}

void loop() {
    // Handle OTA updates
    ArduinoOTA.handle();
    
    // Check WiFi connection
    if (WiFi.status() != WL_CONNECTED) {
        setupWiFi();
    }
    
    // Handle MQTT connection
    if (!mqtt.connected()) {
        reconnect();
    }
    mqtt.loop();
    
    // Handle pending play requests
    if (playRequest.pending) {
        unsigned long currentTime = millis();
        
        // Check if we need to reset volume
        if (isPlaying && (currentTime - volumeResetTimer >= config.volume_reset_ms)) {
            dfPlayer.volume(0);
            isPlaying = false;
        }

        // Only play if not already playing or cooldown has passed
        if (!isPlaying || (currentTime - lastPlayTime >= config.button_cooldown_ms)) {
            dfPlayer.volume(percentToVolume(playRequest.volume));
            dfPlayer.play(playRequest.track);
            lastPlayTime = currentTime;
            volumeResetTimer = currentTime;
            isPlaying = true;
            mqtt.publish("doorbell/debug", "Playing queued track");
        }
        playRequest.pending = false;
    }

    // Handle emergency LED if in emergency mode
    if (emergencyMode) {
        unsigned long currentMillis = millis();
        
        // Handle LED flashing
        if (currentMillis - lastEmergencyLedToggle >= EMERGENCY_LED_INTERVAL) {
            lastEmergencyLedToggle = currentMillis;
            digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
        }
        
        // Check if emergency duration has elapsed
        if (config.emergency_duration > 0 && 
            (currentMillis - emergencyStartTime) >= (config.emergency_duration * 1000UL)) {
            // Auto-disable emergency mode
            emergencyMode = false;
            dfPlayer.stop();
            dfPlayer.volume(percentToVolume(config.door_volume)); // Reset to normal volume
            digitalWrite(LED_BUILTIN, LOW);
            mqtt.publish("doorbell/status", "{\"emergency\": false, \"message\": \"Emergency mode auto-disabled after timeout\"}");
        }
    }

    // Handle LED for normal play (non-emergency)
    if (normalLedOn && !emergencyMode) {
        if (millis() - ledStartTime >= 5000) {  // 5 seconds passed
            digitalWrite(LED_BUILTIN, LOW);  // Turn off LED
            normalLedOn = false;
        }
    }

    // Check buttons only if not in emergency mode
    if (!emergencyMode) {
        unsigned long currentTime = millis();
        
        // Handle downstairs button
        bool downstairsState = digitalRead(BUTTON_DOWNSTAIRS) == HIGH;
        if (downstairsState && !buttonDownstairsWasPressed) {
            // Button just pressed - record start time
            buttonDownstairsPressStart = currentTime;
            buttonDownstairsWasPressed = true;
        } else if (downstairsState && buttonDownstairsWasPressed) {
            // Button is still pressed - check duration
            if (currentTime - buttonDownstairsPressStart >= BUTTON_PRESS_DURATION) {
                handleButton(BUTTON_DOWNSTAIRS, false);
                buttonDownstairsPressStart = currentTime;  // Reset start time to prevent multiple triggers
            }
        } else if (!downstairsState && buttonDownstairsWasPressed) {
            // Button was released
            buttonDownstairsWasPressed = false;
        }
        
        // Handle door button
        bool doorState = digitalRead(BUTTON_DOOR) == HIGH;
        if (doorState && !buttonDoorWasPressed) {
            // Button just pressed - record start time
            buttonDoorPressStart = currentTime;
            buttonDoorWasPressed = true;
        } else if (doorState && buttonDoorWasPressed) {
            // Button is still pressed - check duration
            if (currentTime - buttonDoorPressStart >= BUTTON_PRESS_DURATION) {
                handleButton(BUTTON_DOOR, false);
                buttonDoorPressStart = currentTime;  // Reset start time to prevent multiple triggers
            }
        } else if (!doorState && buttonDoorWasPressed) {
            // Button was released
            buttonDoorWasPressed = false;
        }
    }
}

void setupWiFi() {
    delay(10);
    DEBUG_PRINTLN("\n=== WiFi Setup ===");
    
    // Set WiFi mode explicitly
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();  // Disconnect from any previous connections
    delay(100);
    
    DEBUG_PRINTF("Attempting to connect to primary WiFi SSID: %s\n", config.wifi_ssid);
    WiFi.begin(config.wifi_ssid, config.wifi_password);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        DEBUG_PRINT(".");
        attempts++;
    }
    
    if (WiFi.status() != WL_CONNECTED && strlen(config.backup_wifi_ssid) > 0) {
        DEBUG_PRINTLN("\nPrimary WiFi connection failed");
        DEBUG_PRINTF("Attempting to connect to backup WiFi SSID: %s\n", config.backup_wifi_ssid);
        WiFi.begin(config.backup_wifi_ssid, config.backup_wifi_password);
        attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            delay(500);
            DEBUG_PRINT(".");
            attempts++;
        }
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        DEBUG_PRINTLN("\nWiFi connected successfully!");
        DEBUG_PRINTF("Connected to SSID: %s\n", WiFi.SSID().c_str());
        DEBUG_PRINTF("IP address: %s\n", WiFi.localIP().toString().c_str());
        DEBUG_PRINTF("Signal strength (RSSI): %d dBm\n", WiFi.RSSI());
        
        // Initialize MDNS
        bool mdnsStarted = MDNS.begin("doorbell");
        if (!mdnsStarted) {
            DEBUG_PRINTLN("Error setting up MDNS responder!");
        } else {
            DEBUG_PRINTLN("mDNS responder started");
            MDNS.addService("arduino", "tcp", 3232); // Advertise OTA service
        }
    } else {
        DEBUG_PRINTLN("\nFailed to connect to any WiFi network");
        DEBUG_PRINTLN("Device will continue to work in offline mode");
    }
    DEBUG_PRINTLN("=================\n");
}

void setupOTA() {
    ArduinoOTA.setHostname("doorbell");
    ArduinoOTA.setPort(3232);  // Explicitly set port

    
    ArduinoOTA.onStart([]() {
        String type;
        if (ArduinoOTA.getCommand() == U_FLASH) {
            type = "sketch";
        } else {
            type = "filesystem";
        }
        DEBUG_PRINTLN("Start updating " + type);
    });
    
    ArduinoOTA.onEnd([]() {
        DEBUG_PRINTLN("\nEnd");
    });
    
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        DEBUG_PRINTF("Progress: %u%%\r", (progress / (total / 100)));
    });
    
    ArduinoOTA.onError([](ota_error_t error) {
        DEBUG_PRINTF("Error[%u]: ", error);
        if (error == OTA_AUTH_ERROR) DEBUG_PRINTLN("Auth Failed");
        else if (error == OTA_BEGIN_ERROR) DEBUG_PRINTLN("Begin Failed");
        else if (error == OTA_CONNECT_ERROR) DEBUG_PRINTLN("Connect Failed");
        else if (error == OTA_RECEIVE_ERROR) DEBUG_PRINTLN("Receive Failed");
        else if (error == OTA_END_ERROR) DEBUG_PRINTLN("End Failed");
    });
    
    ArduinoOTA.begin();
    DEBUG_PRINTLN("OTA initialized and ready for updates on port 3232");
}

void setupMQTT() {
    mqtt.setServer(config.mqtt_server, atoi(config.mqtt_port));
    mqtt.setCallback(callback);
}

void setupDFPlayer() {
    if (!dfPlayer.begin(dfPlayerSerial)) {
        Serial.println("Unable to begin DFPlayer");
        // while(true);
    }
    dfPlayer.setTimeOut(500);
    dfPlayer.volume(20);  // Set initial volume (0-30)
    dfPlayer.EQ(DFPLAYER_EQ_NORMAL);
    dfPlayer.outputDevice(DFPLAYER_DEVICE_SD);
}

void handleButton(int button, bool simulated) {
    unsigned long currentTime = millis();
    
    // Debounce check
    if (currentTime - lastDebounceTime < debounceDelay) {
        return;
    }
    lastDebounceTime = currentTime;

    // Check if we need to reset volume
    if (isPlaying && (currentTime - volumeResetTimer >= config.volume_reset_ms)) {
        dfPlayer.volume(0);
        isPlaying = false;
    }

    // Panic button detection logic
    if (!emergencyMode && button == BUTTON_DOOR) {  // Only check door button for panic
        if (pressCount == 0) {
            // First press in a new sequence
            firstPressTime = currentTime;
            pressCount = 1;
        } else {
            // Check if we're still within the time window
            if (currentTime - firstPressTime <= (config.panic_window * 1000UL)) {
                pressCount++;
                
                // Check if we've reached panic threshold
                if (pressCount >= config.panic_press_threshold) {
                    // Trigger emergency mode
                    emergencyMode = true;
                    emergencyStartTime = currentTime;  // Start emergency timer
                    dfPlayer.volume(percentToVolume(config.emergency_volume));
                    dfPlayer.loop(config.emergency_track);
                    digitalWrite(LED_BUILTIN, HIGH);
                    
                    // Publish emergency notification
                    StaticJsonDocument<200> statusDoc;
                    statusDoc["emergency"] = true;
                    statusDoc["trigger"] = "panic_button";
                    statusDoc["presses"] = pressCount;
                    statusDoc["window_ms"] = currentTime - firstPressTime;
                    
                    char buffer[256];
                    serializeJson(statusDoc, buffer);
                    mqtt.publish("doorbell/status", buffer);
                    
                    pressCount = 0;
                    return;
                }
            } else {
                // Outside time window, reset counter
                firstPressTime = currentTime;
                pressCount = 1;
            }
        }
    }

    // Normal doorbell handling (only if not in emergency mode)
    if (!emergencyMode) {
        // Check if we're within cooldown period or if melody is already playing
        if (isPlaying && (currentTime - lastPlayTime < config.button_cooldown_ms)) {
            return;
        }

        if (button == BUTTON_DOWNSTAIRS) {
            dfPlayer.volume(percentToVolume(config.downstairs_volume));
            dfPlayer.play(config.downstairs_track);
            mqtt.publish("doorbell/event", "downstairs");
            lastPlayTime = currentTime;
            volumeResetTimer = currentTime;
            isPlaying = true;
            // Turn on LED for 5 seconds
            digitalWrite(LED_BUILTIN, HIGH);
            ledStartTime = currentTime;
            normalLedOn = true;
        } else if (button == BUTTON_DOOR) {
            dfPlayer.volume(percentToVolume(config.door_volume));
            dfPlayer.play(config.door_track);
            mqtt.publish("doorbell/event", "door");
            lastPlayTime = currentTime;
            volumeResetTimer = currentTime;
            isPlaying = true;
            // Turn on LED for 5 seconds
            digitalWrite(LED_BUILTIN, HIGH);
            ledStartTime = currentTime;
            normalLedOn = true;
        }
    }
}

void callback(char* topic, byte* payload, unsigned int length) {
    // Safety check for topic
    if (!topic || strlen(topic) < 2) {
        mqtt.publish("doorbell/debug", "Error: Invalid topic received");
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
    char debug_msg[512];
    snprintf(debug_msg, sizeof(debug_msg), "Received on topic '%s': %s", topic_copy, message);
    mqtt.publish("doorbell/debug", debug_msg);
    
    // Handle simulation commands (no JSON needed)
    if (strcmp(topic_copy, "doorbell/simulate/door") == 0) {
        mqtt.publish("doorbell/debug", "Simulating door button press");
        handleButton(BUTTON_DOOR, true);
        return;
    }
    else if (strcmp(topic_copy, "doorbell/simulate/downstairs") == 0) {
        mqtt.publish("doorbell/debug", "Simulating downstairs button press");
        handleButton(BUTTON_DOWNSTAIRS, true);
        return;
    }
    else if (strcmp(topic_copy, "doorbell/system/reboot") == 0) {
        // Check if there's a confirmation payload
        if (strcmp(message, "REBOOT") == 0) {
            mqtt.publish("doorbell/status", "{\"system\": \"reboot\", \"message\": \"Rebooting device...\"}");
            // Give MQTT time to send the message
            mqtt.loop();
            delay(100);
            ESP.restart();
        } else {
            mqtt.publish("doorbell/status", "{\"system\": \"reboot\", \"message\": \"To reboot, send 'REBOOT' to doorbell/system/reboot\"}");
        }
        return;
    }
    
    // Handle get commands (no JSON needed)
    if (strcmp(topic_copy, "doorbell/get/config") == 0) {
        mqtt.publish("doorbell/debug", "Getting config");
        publishConfig();
        return;
    }
    else if (strcmp(topic_copy, "doorbell/get/status") == 0) {
        mqtt.publish("doorbell/debug", "Getting status");
        publishDeviceStatus();
        return;
    }
    
    // Handle play command (no JSON needed)
    if (strncmp(topic_copy, "doorbell/play/", 14) == 0) {
        const char* track_str = topic_copy + 14;
        int track = atoi(track_str);
        char debug_msg[64];
        snprintf(debug_msg, sizeof(debug_msg), "Received play command for track %d", track);
        mqtt.publish("doorbell/debug", debug_msg);
        if (track > 0) {
            mqtt.publish("doorbell/debug", "Queueing track to play");
            playRequest.pending = true;
            playRequest.track = track;
            playRequest.volume = 100; // max volume in percentage
        }
        return;
    }
    
    // Handle emergency mode toggle
    if (strcmp(topic_copy, "doorbell/emergency") == 0) {
        if (strcmp(message, "ON") == 0 && !emergencyMode) {
            emergencyMode = true;
            emergencyStartTime = millis();  // Start emergency timer
            dfPlayer.volume(percentToVolume(config.emergency_volume));
            dfPlayer.loop(config.emergency_track);
            digitalWrite(LED_BUILTIN, HIGH);
            mqtt.publish("doorbell/status", "{\"emergency\": true, \"message\": \"Emergency mode activated\"}");
        }
        else if (strcmp(message, "OFF") == 0 && emergencyMode) {
            emergencyMode = false;
            dfPlayer.stop();
            dfPlayer.volume(percentToVolume(config.door_volume)); // Reset to normal volume
            digitalWrite(LED_BUILTIN, LOW);
            mqtt.publish("doorbell/status", "{\"emergency\": false, \"message\": \"Emergency mode deactivated\"}");
        }
        return;
    }
    
    // All remaining commands require JSON
    if (strncmp(topic_copy, "doorbell/set/", 12) != 0) {
        mqtt.publish("doorbell/debug", "Error: Unknown command");
        return;
    }
    
    // Parse JSON for set commands
    StaticJsonDocument<200> doc;
    DeserializationError error = deserializeJson(doc, message);
    
    if (error) {
        char error_msg[64];
        snprintf(error_msg, sizeof(error_msg), "Failed to parse JSON for set command: %s", error.c_str());
        mqtt.publish("doorbell/debug", error_msg);
        return;
    }
    
    // Handle set commands (all require JSON)
    if (strcmp(topic_copy, "doorbell/set/button/downstairs") == 0) {
        mqtt.publish("doorbell/debug", "Setting downstairs button config");
        if (doc.containsKey("track")) {
            config.downstairs_track = doc["track"];
            char debug_msg[64];
            snprintf(debug_msg, sizeof(debug_msg), "Set downstairs track to %d", config.downstairs_track);
            mqtt.publish("doorbell/debug", debug_msg);
        }
        if (doc.containsKey("volume")) {
            config.downstairs_volume = doc["volume"];
            char debug_msg[64];
            snprintf(debug_msg, sizeof(debug_msg), "Set downstairs volume to %d%%", config.downstairs_volume);
            mqtt.publish("doorbell/debug", debug_msg);
        }
        saveConfig();
    }
    else if (strcmp(topic_copy, "doorbell/set/button/door") == 0) {
        mqtt.publish("doorbell/debug", "Setting door button config");
        if (doc.containsKey("track")) {
            config.door_track = doc["track"];
            char debug_msg[64];
            snprintf(debug_msg, sizeof(debug_msg), "Set door track to %d", config.door_track);
            mqtt.publish("doorbell/debug", debug_msg);
        }
        if (doc.containsKey("volume")) {
            config.door_volume = doc["volume"];
            char debug_msg[64];
            snprintf(debug_msg, sizeof(debug_msg), "Set door volume to %d%%", config.door_volume);
            mqtt.publish("doorbell/debug", debug_msg);
        }
        saveConfig();
    }
    else if (strcmp(topic_copy, "doorbell/set/config") == 0) {
        mqtt.publish("doorbell/debug", "Setting device config");
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
        saveConfig();
    }
    else if (strcmp(topic_copy, "doorbell/set/emergency") == 0) {
        mqtt.publish("doorbell/debug", "Setting emergency config");
        if (doc.containsKey("track")) {
            config.emergency_track = doc["track"];
        }
        if (doc.containsKey("volume")) {
            config.emergency_volume = doc["volume"];
        }
        if (doc.containsKey("duration")) {
            config.emergency_duration = doc["duration"];
        }
        if (doc.containsKey("panic_threshold")) {
            config.panic_press_threshold = doc["panic_threshold"];
        }
        if (doc.containsKey("panic_window")) {
            config.panic_window = doc["panic_window"];
        }
        if (doc.containsKey("button_cooldown_ms")) {
            config.button_cooldown_ms = doc["button_cooldown_ms"];
        }
        if (doc.containsKey("volume_reset_ms")) {
            config.volume_reset_ms = doc["volume_reset_ms"];
        }
        saveConfig();
        publishConfig();
    }
}

void reconnect() {
    while (!mqtt.connected()) {
        DEBUG_PRINTLN("Attempting MQTT connection...");
        
        // Create a random client ID
        String clientId = "DoorBell-";
        clientId += String(random(0xffff), HEX);
        
        // Attempt to connect
        if (mqtt.connect(clientId.c_str(), config.mqtt_user, config.mqtt_password)) {
            DEBUG_PRINTLN("Connected to MQTT");
            
            // Subscribe to all set commands (require JSON)
            mqtt.subscribe("doorbell/set/#");
            // Subscribe to all get commands (no JSON)
            mqtt.subscribe("doorbell/get/#");
            // Subscribe to all simulation commands (no JSON)
            mqtt.subscribe("doorbell/simulate/#");
            // Subscribe to play commands (no JSON)
            mqtt.subscribe("doorbell/play/#");
            // Subscribe to emergency toggle
            mqtt.subscribe("doorbell/emergency");
            // Subscribe to system commands
            mqtt.subscribe("doorbell/system/#");
            
            publishDeviceStatus();
        } else {
            DEBUG_PRINT("Failed to connect to MQTT, rc=");
            DEBUG_PRINTLN(mqtt.state());
            DEBUG_PRINTLN("Trying backup MQTT server...");
            
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
        
        // Default emergency settings
        config.emergency_track = 99;
        config.emergency_volume = 100; // Default volume in percentage
        config.emergency_duration = 60;    // 1 minute default
        config.panic_press_threshold = 5;  // 5 presses
        config.panic_window = 20;         // 20 seconds window
        config.button_cooldown_ms = 15000; // 15 seconds default
        config.volume_reset_ms = 60000;   // 1 minute default
        
        saveConfig();
    }
}

void saveConfig() {
    EEPROM.write(EEPROM_VALID_ADDR, 0xAA);
    EEPROM.put(EEPROM_CONFIG_ADDR, config);
    EEPROM.commit();
}

void publishConfig() {
    StaticJsonDocument<512> configObj;
    
    // WiFi settings
    configObj["wifi_ssid"] = config.wifi_ssid;
    configObj["backup_wifi_ssid"] = config.backup_wifi_ssid;
    
    // MQTT settings
    configObj["mqtt_server"] = config.mqtt_server;
    configObj["mqtt_port"] = config.mqtt_port;
    configObj["backup_mqtt_server"] = config.backup_mqtt_server;
    configObj["backup_mqtt_port"] = config.backup_mqtt_port;
    
    // Button settings
    configObj["downstairs_track"] = config.downstairs_track;
    configObj["door_track"] = config.door_track;
    configObj["downstairs_volume"] = config.downstairs_volume;
    configObj["door_volume"] = config.door_volume;
    
    // Emergency settings
    configObj["emergency_track"] = config.emergency_track;
    configObj["emergency_volume"] = config.emergency_volume;
    configObj["emergency_duration"] = config.emergency_duration;
    configObj["panic_press_threshold"] = config.panic_press_threshold;
    configObj["panic_window"] = config.panic_window;
    configObj["button_cooldown_ms"] = config.button_cooldown_ms;
    configObj["volume_reset_ms"] = config.volume_reset_ms;
    
    char buffer[512];
    serializeJson(configObj, buffer);
}

void clearEEPROM() {
    DEBUG_PRINTLN("Clearing EEPROM...");
    for (int i = 0; i < EEPROM_SIZE; i++) {
        EEPROM.write(i, 0);
    }
    EEPROM.commit();
    DEBUG_PRINTLN("EEPROM cleared!");
}

void publishDeviceStatus() {
    if (!mqtt.connected()) {
        return;
    }
    
    // Create a JSON document for device status
    StaticJsonDocument<512> statusDoc;
    
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
    
    // Emergency mode status
    statusDoc["emergency"] = emergencyMode;
    
    char buffer[512];
    serializeJson(statusDoc, buffer);
    
    // Publish to status topic
    mqtt.publish("doorbell/status", buffer, true);  // retain flag set to true
    DEBUG_PRINTLN("Published device status");
}
