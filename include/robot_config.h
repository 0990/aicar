#pragma once

#include <Arduino.h>

namespace robot {

// DRV8833 pin mapping. Update these pins based on your wiring.
static constexpr uint8_t LEFT_IN1_PIN = 2;
static constexpr uint8_t LEFT_IN2_PIN = 3;
static constexpr uint8_t RIGHT_IN1_PIN = 4;
static constexpr uint8_t RIGHT_IN2_PIN = 5;

// PWM setup.
static constexpr uint8_t LEFT_IN1_CH = 0;
static constexpr uint8_t LEFT_IN2_CH = 1;
static constexpr uint8_t RIGHT_IN1_CH = 2;
static constexpr uint8_t RIGHT_IN2_CH = 3;
static constexpr uint32_t PWM_FREQ_HZ = 20000;
static constexpr uint8_t PWM_RES_BITS = 8;

// Motion defaults.
static constexpr uint8_t DEFAULT_SPEED = 180;      // 0..255
static constexpr uint32_t DEFAULT_MOVE_MS = 700;   // timed move for voice command
static constexpr uint32_t MAX_MOVE_MS = 10000;     // hard safety cap
static constexpr uint32_t FAILSAFE_STOP_MS = 3000; // stop if no command in this window

// WiFi + HTTP API settings.
// Set these before flashing.
static constexpr const char *WIFI_SSID = "YOUR_WIFI_SSID";
static constexpr const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
static constexpr uint16_t HTTP_PORT = 80;
static constexpr const char *HTTP_TOKEN = ""; // optional shared secret from header X-Robot-Token
static constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;

// MQTT settings for internet control (robot in LAN, MCP server in public network).
// Robot connects outward to broker, subscribes command topic, and publishes ack/state.
static constexpr bool MQTT_ENABLED = true;
static constexpr const char *MQTT_BROKER = "broker.emqx.io";
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
static constexpr uint8_t OLED_SDA_PIN = 6;
static constexpr uint8_t OLED_SCL_PIN = 7;
static constexpr uint8_t OLED_I2C_ADDRESS = 0x3C;
static constexpr uint16_t MAX_EXPRESSION_HOLD_MS = 30000;

} // namespace robot
