Import("env")
import os

# ANSI color codes
BLUE = "\033[34m"
RESET = "\033[0m"

def debug_print(message):
    print(f"{BLUE}[OTA Password Script] {message}{RESET}")

def extract_ota_password(config_file):
    debug_print(f"Reading config file: {config_file}")
    try:
        with open(config_file, 'r') as f:
            for line in f:
                if 'OTA_PASSWORD' in line:
                    # Extract password between quotes
                    password = line.split('"')[1]
                    debug_print(f"Found password in config.h: {password}")
                    return password
    except Exception as e:
        debug_print(f"Error reading config file: {e}")
        return None

config_path = os.path.join(env.get('PROJECT_DIR'), 'src', 'config.h')
debug_print(f"Starting OTA password extraction process")
password = extract_ota_password(config_path)

if password:
    debug_print(f"Setting upload flag with OTA password")
    env.Append(
        UPLOAD_FLAGS=[f'--auth={password}']
    )
    debug_print("Upload flag set successfully")
else:
    debug_print("Warning: Could not find OTA password in config.h")

debug_print("OTA password extraction process completed")