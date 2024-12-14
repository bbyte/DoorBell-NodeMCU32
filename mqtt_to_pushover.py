#!/usr/bin/env python3

import paho.mqtt.client as mqtt
import requests
import json
from datetime import datetime
import configparser
import os

# Create config file if it doesn't exist
def create_default_config():
    config = configparser.ConfigParser()
    config['MQTT'] = {
        'broker': '192.168.1.40',
        'port': '1883',
        'username': '',
        'password': '',
    }
    config['PUSHOVER'] = {
        'user_key': '',
        'api_token': ''
    }
    
    with open('mqtt_config.ini', 'w') as configfile:
        config.write(configfile)
    return config

# Load or create configuration
config = configparser.ConfigParser()
if os.path.exists('mqtt_config.ini'):
    config.read('mqtt_config.ini')
else:
    config = create_default_config()

# MQTT Configuration
MQTT_BROKER = config['MQTT']['broker']
MQTT_PORT = int(config['MQTT']['port'])
MQTT_USERNAME = config['MQTT']['username']
MQTT_PASSWORD = config['MQTT']['password']
MQTT_TOPICS = [
    "doorbell/#"  # This will subscribe to all topics under doorbell/
]

# Pushover Configuration
PUSHOVER_USER_KEY = config['PUSHOVER']['user_key']
PUSHOVER_API_TOKEN = config['PUSHOVER']['api_token']
PUSHOVER_API_URL = "https://api.pushover.net/1/messages.json"

def on_connect(client, userdata, flags, rc):
    print(f"Connected to MQTT broker with result code {rc}")
    # Subscribe to all topics
    for topic in MQTT_TOPICS:
        client.subscribe(topic)
        print(f"Subscribed to {topic}")

def on_message(client, userdata, msg):
    topic = msg.topic
    try:
        # Try to parse the payload as JSON
        payload = json.loads(msg.payload.decode())
        message = json.dumps(payload, indent=2)
    except:
        # If not JSON, use raw payload
        message = msg.payload.decode()

    # Create notification message
    notification = f"Topic: {topic}\n{message}"
    
    # Send to Pushover
    data = {
        "token": PUSHOVER_API_TOKEN,
        "user": PUSHOVER_USER_KEY,
        "title": f"Doorbell MQTT: {topic}",
        "message": notification,
        "priority": 0,  # Normal priority
        "sound": "bell"  # Use bell sound for notifications
    }
    
    try:
        response = requests.post(PUSHOVER_API_URL, data=data)
        if response.status_code == 200:
            print(f"Notification sent for topic {topic}. Status: {response.status_code}")
        else:
            print(f"Error sending notification: {response.text}")
    except Exception as e:
        print(f"Error sending notification: {e}")

def main():
    # Verify Pushover configuration
    if not PUSHOVER_USER_KEY or not PUSHOVER_API_TOKEN:
        print("Error: Pushover user key and API token must be configured in mqtt_config.ini")
        return

    client = mqtt.Client()
    client.on_connect = on_connect
    client.on_message = on_message

    if MQTT_USERNAME and MQTT_PASSWORD:
        client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)

    try:
        client.connect(MQTT_BROKER, MQTT_PORT, 60)
        print("Starting MQTT to Pushover bridge...")
        client.loop_forever()
    except KeyboardInterrupt:
        print("\nExiting...")
        client.disconnect()
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    main()
