#pragma once

#include <Arduino.h>

namespace robot {

// DRV8833 pin mapping. Update these pins based on your wiring.
static constexpr uint8_t LEFT_IN1_PIN = 0;
static constexpr uint8_t LEFT_IN2_PIN = 1;
static constexpr uint8_t RIGHT_IN1_PIN = 2;
static constexpr uint8_t RIGHT_IN2_PIN = 3;

// Motion defaults.
static constexpr uint8_t DEFAULT_SPEED = 180;      // 0..255
static constexpr uint32_t DEFAULT_MOVE_MS = 700;   // timed move for voice command
static constexpr uint32_t MAX_MOVE_MS = 10000;     // hard safety cap
static constexpr uint32_t FAILSAFE_STOP_MS = 3000; // stop if no command in this window

// Passive buzzer output. Update pin based on your wiring.
static constexpr bool BUZZER_ENABLED = true;
static constexpr uint8_t BUZZER_PIN = 10;
static constexpr uint16_t MAX_BUZZER_FREQ_HZ = 5000;
static constexpr uint16_t MAX_BUZZER_STEP_MS = 5000;
static constexpr uint8_t MAX_BUZZER_PATTERN_STEPS = 16;
static constexpr uint8_t MAX_BUZZER_REPEAT = 10;

// WiFi + HTTP API settings.
// Set these before flashing.
static constexpr const char *WIFI_SSID = "1";
static constexpr const char *WIFI_PASSWORD = "2";
static constexpr uint16_t HTTP_PORT = 80;
static constexpr const char *HTTP_TOKEN = ""; // optional shared secret from header X-Robot-Token
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;

// MQTT settings for internet control (robot in LAN, MCP server in public network).
// Robot connects outward to broker, subscribes command topic, and publishes ack/state.
static constexpr bool MQTT_ENABLED = true;
static constexpr const char *MQTT_BROKER = "10.229.1.186";
static constexpr uint16_t MQTT_PORT = 1883;
static constexpr const char *MQTT_USERNAME = "";
static constexpr const char *MQTT_PASSWORD = "";
static constexpr const char *MQTT_CLIENT_ID_PREFIX = "aicar-esp32-";
static constexpr const char *MQTT_TOPIC_PREFIX = "aicar";
static constexpr const char *ROBOT_ID = "car-001";
static constexpr uint32_t MQTT_RECONNECT_INTERVAL_MS = 3000;
static constexpr uint32_t MQTT_HEARTBEAT_MS = 5000;

// 0.96" OLED (SSD1306 over I2C: VCC/GND/SCL/SDA).
// Update pins based on your board wiring.
static constexpr bool OLED_ENABLED = true;
static constexpr uint8_t OLED_WIDTH = 128;
static constexpr uint8_t OLED_HEIGHT = 64;
static constexpr uint8_t OLED_MAX_FPS = 60;
static constexpr uint8_t OLED_SDA_PIN = 8;
static constexpr uint8_t OLED_SCL_PIN = 9;
static constexpr uint8_t OLED_I2C_ADDRESS = 0x3C;
static constexpr uint16_t MAX_EXPRESSION_HOLD_MS = 30000;

} // namespace robot
