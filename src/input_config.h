#ifndef INPUT_CONFIG_H
#define INPUT_CONFIG_H

// Input mode selection (uncomment only one)
// #define INPUT_MODE_DIGITAL
#define INPUT_MODE_ANALOG

// ADC Configuration
#ifdef INPUT_MODE_ANALOG
    #define MIN_SESSION_DURATION 200    // Minimum valid session duration in ms
    #define ADC_THRESHOLD 3.0          // Voltage threshold for button detection (3.0V)
    #define ADC_HYSTERESIS 0.3         // Voltage hysteresis to prevent bouncing (0.3V)
    #define ADC_SAMPLE_INTERVAL 5      // How often to sample ADC in milliseconds
    #define MAX_SESSION_SAMPLES 1000   // Maximum number of samples per session
    #define ADC_DROPOUT_TOLERANCE 15   // Maximum time in ms to tolerate voltage drops
    
    /// @brief Structure to store a single ADC reading with voltage values and visualization
    struct ADCReading {
        float voltage1;                 ///< Voltage reading from ADC1 (0-3.3V)
        float voltage2;                 ///< Voltage reading from ADC2 (0-3.3V)
        unsigned long delta;            ///< Time since session start in milliseconds
        char graph[42];                ///< ANSI-colored bar graph representation (41 chars + null)
    };
    
    /// @brief Structure to store complete session data including all readings
    struct ADCSession {
        unsigned long startTime;        ///< Session start timestamp
        unsigned long endTime;          ///< Session end timestamp
        bool isActive;                  ///< Whether session is currently active
        float maxVoltage;              ///< Maximum voltage recorded during session
        int buttonDetected;            ///< Which button was detected (-1=none, 0=DOWNSTAIRS, 1=DOOR)
        int numReadings;               ///< Number of readings stored in the session
        struct ADCReading readings[MAX_SESSION_SAMPLES];  ///< Array of all readings during session
    };
#endif

#endif // INPUT_CONFIG_H
