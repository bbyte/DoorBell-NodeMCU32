#!/usr/bin/env python3

import paho.mqtt.client as mqtt
import json
import os
import time
from datetime import datetime
import configparser
import http.client
import urllib.parse
import sys
import re

# Create sessions directory if it doesn't exist
SESSIONS_DIR = "sessions"
os.makedirs(SESSIONS_DIR, exist_ok=True)

# Read configuration
config = configparser.ConfigParser()
config.read('mqtt_config.ini')

# MQTT settings
MQTT_BROKER = config['MQTT']['broker']
MQTT_PORT = int(config['MQTT']['port'])
MQTT_USERNAME = config['MQTT'].get('username')
MQTT_PASSWORD = config['MQTT'].get('password')

# Pushover settings
PUSHOVER_USER_KEY = config['PUSHOVER']['user_key']
PUSHOVER_API_TOKEN = config['PUSHOVER']['api_token']

def strip_ansi(text):
    """Remove ANSI escape sequences from text"""
    ansi_escape = re.compile(r'\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])')
    return ansi_escape.sub('', text)

class SessionLogger:
    def __init__(self):
        self.current_session_file = None
        self.session_start_time = None
        
    def send_pushover_notification(self, message, title="Doorbell Session"):
        conn = http.client.HTTPSConnection("api.pushover.net:443")
        conn.request("POST", "/1/messages.json",
                    urllib.parse.urlencode({
                        "token": PUSHOVER_API_TOKEN,
                        "user": PUSHOVER_USER_KEY,
                        "title": title,
                        "message": message,
                    }), {"Content-type": "application/x-www-form-urlencoded"})
        conn.getresponse()

    def start_session(self):
        self.session_start_time = datetime.now()
        filename = f"session_{self.session_start_time.strftime('%Y%m%d_%H%M%S')}.csv"
        self.current_session_file = open(os.path.join(SESSIONS_DIR, filename), 'w')
        self.current_session_file.write("delta_ms,adc1_v,adc2_v\n")
        
        # Send Pushover notification
        self.send_pushover_notification("Doorbell session started", "🔔 Session Start")

    def end_session(self):
        if self.current_session_file:
            self.current_session_file.close()
            self.current_session_file = None
            
            # Calculate session duration
            if self.session_start_time is not None:
                duration = datetime.now() - self.session_start_time
                duration_str = str(duration).split('.')[0]  # Remove microseconds
                
                # Send Pushover notification
                self.send_pushover_notification(
                    f"Session ended (Duration: {duration_str})",
                    "🔔 Session End"
                )

    def log_adc_data(self, data):
        if self.current_session_file:
            self.current_session_file.write(f"{data['delta']},{data['adc1_v']},{data['adc2_v']}\n")
            self.current_session_file.flush()  # Ensure data is written immediately

def on_connect(client, userdata, flags, rc):
    print("Connected to MQTT broker with result code " + str(rc))
    # Subscribe to debug topics
    client.subscribe("doorbell/debug")

def on_message(client, userdata, msg):
    raw_payload = None
    cleaned_payload = None
    try:
        raw_payload = msg.payload.decode()
        # Strip ANSI escape sequences before parsing JSON
        cleaned_payload = strip_ansi(raw_payload)
        
        # Try to parse as JSON first
        try:
            data = json.loads(cleaned_payload)
            
            # Check if it's a session status message
            if "status" in data:
                if data["status"] == "started":
                    session_logger.start_session()
                elif data["status"] == "ended":
                    session_logger.end_session()
            
            # Check if it's ADC data (contains adc1_v and adc2_v)
            elif "adc1_v" in data and "adc2_v" in data:
                session_logger.log_adc_data(data)
                
        except json.JSONDecodeError:
            # If it's not JSON, it's a plain debug message - ignore it
            pass
            
    except Exception as e:
        print(f"Error processing message: {e}", file=sys.stderr)
        print(f"Raw message: {raw_payload}", file=sys.stderr)
        print(f"Cleaned message: {cleaned_payload}", file=sys.stderr)

if __name__ == "__main__":
    session_logger = SessionLogger()

    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message

    # Set MQTT credentials if provided
    if MQTT_USERNAME and MQTT_PASSWORD:
        client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)

    try:
        client.connect(MQTT_BROKER, MQTT_PORT, 60)
        print(f"Connected to MQTT broker at {MQTT_BROKER}:{MQTT_PORT}")
        print(f"Logging sessions to directory: {SESSIONS_DIR}")
        client.loop_forever()
    except KeyboardInterrupt:
        print("\nExiting...")
        if session_logger.current_session_file:
            session_logger.current_session_file.close()
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
