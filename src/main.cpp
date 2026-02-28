#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <Arduino.h>
#ifdef DEFAULT
#undef DEFAULT
#endif
#include <FluxGarage_RoboEyes.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <Wire.h>
#include <stdlib.h>
#include <string.h>

#include "robot_config.h"

namespace {

enum class Motion { Stop = 0, Forward, Backward, Left, Right };
enum class EyeAction { None = 0, Blink, WinkLeft, WinkRight, Confused, Laugh, Open, Close };

struct EyeStyle {
  uint8_t mood = DEFAULT;
  uint8_t position = DEFAULT;
  bool curiosity = false;
  bool sweat = false;
  bool cyclops = false;
  bool autoBlink = true;
  uint8_t autoBlinkInterval = 3;
  uint8_t autoBlinkVariation = 2;
  bool idle = false;
  uint8_t idleInterval = 2;
  uint8_t idleVariation = 2;
  uint8_t hFlickerAmp = 0;
  uint8_t vFlickerAmp = 0;
};

struct Command {
  Motion motion = Motion::Stop;
  uint32_t durationMs = 0;
  uint8_t speed = robot::DEFAULT_SPEED;
  bool valid = false;
};

struct PresetResolveResult {
  bool valid = false;
  bool hasStyle = false;
  EyeStyle style{};
  bool hasAction = false;
  EyeAction action = EyeAction::None;
  String label = "CUSTOM";
};

struct StyleParamResult {
  bool changed = false;
  EyeStyle style{};
  bool hasAction = false;
  EyeAction action = EyeAction::None;
};

class MotorDriver {
public:
  void begin() {
    ledcSetup(robot::LEFT_IN1_CH, robot::PWM_FREQ_HZ, robot::PWM_RES_BITS);
    ledcSetup(robot::LEFT_IN2_CH, robot::PWM_FREQ_HZ, robot::PWM_RES_BITS);
    ledcSetup(robot::RIGHT_IN1_CH, robot::PWM_FREQ_HZ, robot::PWM_RES_BITS);
    ledcSetup(robot::RIGHT_IN2_CH, robot::PWM_FREQ_HZ, robot::PWM_RES_BITS);
    ledcAttachPin(robot::LEFT_IN1_PIN, robot::LEFT_IN1_CH);
    ledcAttachPin(robot::LEFT_IN2_PIN, robot::LEFT_IN2_CH);
    ledcAttachPin(robot::RIGHT_IN1_PIN, robot::RIGHT_IN1_CH);
    ledcAttachPin(robot::RIGHT_IN2_PIN, robot::RIGHT_IN2_CH);
    stop();
  }

  void drive(Motion motion, uint8_t speed) {
    switch (motion) {
    case Motion::Forward:
      driveDifferential(static_cast<int16_t>(speed), static_cast<int16_t>(speed));
      break;
    case Motion::Backward:
      driveDifferential(static_cast<int16_t>(-speed), static_cast<int16_t>(-speed));
      break;
    case Motion::Left:
      driveDifferential(static_cast<int16_t>(-speed), static_cast<int16_t>(speed));
      break;
    case Motion::Right:
      driveDifferential(static_cast<int16_t>(speed), static_cast<int16_t>(-speed));
      break;
    case Motion::Stop:
    default:
      stop();
      break;
    }
  }

  void stop() { driveDifferential(0, 0); }

private:
  static void writeMotor(uint8_t forwardChannel, uint8_t reverseChannel, int16_t signedSpeed) {
    const uint8_t duty = static_cast<uint8_t>(constrain(abs(signedSpeed), 0, 255));
    if (signedSpeed > 0) {
      ledcWrite(forwardChannel, duty);
      ledcWrite(reverseChannel, 0);
      return;
    }
    if (signedSpeed < 0) {
      ledcWrite(forwardChannel, 0);
      ledcWrite(reverseChannel, duty);
      return;
    }
    ledcWrite(forwardChannel, 0);
    ledcWrite(reverseChannel, 0);
  }

  static void driveDifferential(int16_t left, int16_t right) {
    writeMotor(robot::LEFT_IN1_CH, robot::LEFT_IN2_CH, left);
    writeMotor(robot::RIGHT_IN1_CH, robot::RIGHT_IN2_CH, right);
  }
};

MotorDriver g_motor;
WebServer g_server(robot::HTTP_PORT);
Adafruit_SSD1306 g_oled(robot::OLED_WIDTH, robot::OLED_HEIGHT, &Wire, -1);
RoboEyes<Adafruit_SSD1306> g_roboEyes(g_oled);
WiFiClient g_mqttNetClient;
PubSubClient g_mqttClient(g_mqttNetClient);

uint8_t g_defaultSpeed = robot::DEFAULT_SPEED;
uint32_t g_motionStopAtMs = 0;
uint32_t g_lastCommandAtMs = 0;
Motion g_currentMotion = Motion::Stop;

bool g_oledReady = false;
EyeStyle g_eyeStyle{};
String g_eyePresetName = "NEUTRAL";
String g_lastEyeAction = "NONE";
EyeStyle g_eyeStyleRevert{};
String g_eyePresetRevertName = "NEUTRAL";
uint32_t g_eyeRevertAtMs = 0;

String g_robotId = String(robot::ROBOT_ID);
String g_mqttTopicRegister;
String g_mqttTopicStatus;
String g_mqttTopicCommand;
String g_mqttTopicAck;
uint32_t g_lastMqttHeartbeatAtMs = 0;
uint32_t g_lastMqttReconnectAttemptAtMs = 0;

void applyExpressionFromRequest(const StyleParamResult &styleParams, bool hasPreset,
                                const PresetResolveResult &preset, uint32_t holdMs,
                                uint32_t fallbackHoldMs);

bool parseLongStrict(const String &token, long *out) {
  if (out == nullptr || token.length() == 0) {
    return false;
  }
  char *end = nullptr;
  const long value = strtol(token.c_str(), &end, 10);
  if (end == token.c_str() || *end != '\0') {
    return false;
  }
  *out = value;
  return true;
}

bool parseDuration(const String &token, uint32_t *durationMsOut) {
  if (durationMsOut == nullptr) {
    return false;
  }
  long value = 0;
  if (!parseLongStrict(token, &value) || value <= 0) {
    return false;
  }
  *durationMsOut = static_cast<uint32_t>(constrain(value, 1L, static_cast<long>(robot::MAX_MOVE_MS)));
  return true;
}

bool parseSpeed(const String &token, uint8_t *speedOut) {
  if (speedOut == nullptr) {
    return false;
  }
  long value = 0;
  if (!parseLongStrict(token, &value) || value < 0) {
    return false;
  }
  *speedOut = static_cast<uint8_t>(constrain(value, 0L, 255L));
  return true;
}

bool parseHoldMs(const String &token, uint32_t *holdMsOut) {
  if (holdMsOut == nullptr) {
    return false;
  }
  long value = 0;
  if (!parseLongStrict(token, &value) || value <= 0) {
    return false;
  }
  *holdMsOut =
      static_cast<uint32_t>(constrain(value, 1L, static_cast<long>(robot::MAX_EXPRESSION_HOLD_MS)));
  return true;
}

bool parseIntRange(const String &token, int minValue, int maxValue, int *out) {
  if (out == nullptr) {
    return false;
  }
  long value = 0;
  if (!parseLongStrict(token, &value)) {
    return false;
  }
  if (value < minValue || value > maxValue) {
    return false;
  }
  *out = static_cast<int>(value);
  return true;
}

bool parseBool(const String &token, bool *out) {
  if (out == nullptr || token.length() == 0) {
    return false;
  }
  String value = token;
  value.trim();
  value.toLowerCase();
  if (value == "1" || value == "true" || value == "yes" || value == "on") {
    *out = true;
    return true;
  }
  if (value == "0" || value == "false" || value == "no" || value == "off") {
    *out = false;
    return true;
  }
  return false;
}

bool parseMotionToken(const String &token, Motion *motionOut) {
  if (motionOut == nullptr || token.length() == 0) {
    return false;
  }
  String upper = token;
  upper.trim();
  upper.toUpperCase();
  if (upper == "FORWARD" || upper == "F") {
    *motionOut = Motion::Forward;
    return true;
  }
  if (upper == "BACKWARD" || upper == "B") {
    *motionOut = Motion::Backward;
    return true;
  }
  if (upper == "LEFT" || upper == "L") {
    *motionOut = Motion::Left;
    return true;
  }
  if (upper == "RIGHT" || upper == "R") {
    *motionOut = Motion::Right;
    return true;
  }
  if (upper == "STOP" || upper == "S") {
    *motionOut = Motion::Stop;
    return true;
  }
  return false;
}

bool parseJsonString(const JsonVariantConst &value, String *out) {
  if (out == nullptr || value.isNull()) {
    return false;
  }
  if (!value.is<const char *>()) {
    return false;
  }
  *out = String(value.as<const char *>());
  out->trim();
  return true;
}

bool parseJsonBool(const JsonVariantConst &value, bool *out) {
  if (out == nullptr || value.isNull()) {
    return false;
  }
  if (value.is<bool>()) {
    *out = value.as<bool>();
    return true;
  }
  if (value.is<int>() || value.is<long>()) {
    const long number = value.as<long>();
    if (number == 0) {
      *out = false;
      return true;
    }
    if (number == 1) {
      *out = true;
      return true;
    }
    return false;
  }
  if (value.is<const char *>()) {
    return parseBool(String(value.as<const char *>()), out);
  }
  return false;
}

bool parseJsonIntInRange(const JsonVariantConst &value, int minValue, int maxValue, int *out) {
  if (out == nullptr || value.isNull()) {
    return false;
  }

  long parsed = 0;
  if (value.is<int>() || value.is<long>()) {
    parsed = value.as<long>();
  } else if (value.is<const char *>()) {
    if (!parseLongStrict(String(value.as<const char *>()), &parsed)) {
      return false;
    }
  } else {
    return false;
  }

  if (parsed < minValue || parsed > maxValue) {
    return false;
  }
  *out = static_cast<int>(parsed);
  return true;
}

String motionName(Motion motion) {
  switch (motion) {
  case Motion::Forward:
    return "FORWARD";
  case Motion::Backward:
    return "BACKWARD";
  case Motion::Left:
    return "LEFT";
  case Motion::Right:
    return "RIGHT";
  case Motion::Stop:
  default:
    return "STOP";
  }
}

String moodName(uint8_t mood) {
  switch (mood) {
  case TIRED:
    return "TIRED";
  case ANGRY:
    return "ANGRY";
  case HAPPY:
    return "HAPPY";
  case DEFAULT:
  default:
    return "DEFAULT";
  }
}

String positionName(uint8_t position) {
  switch (position) {
  case N:
    return "N";
  case NE:
    return "NE";
  case E:
    return "E";
  case SE:
    return "SE";
  case S:
    return "S";
  case SW:
    return "SW";
  case W:
    return "W";
  case NW:
    return "NW";
  case DEFAULT:
  default:
    return "DEFAULT";
  }
}

String actionName(EyeAction action) {
  switch (action) {
  case EyeAction::Blink:
    return "BLINK";
  case EyeAction::WinkLeft:
    return "WINK_LEFT";
  case EyeAction::WinkRight:
    return "WINK_RIGHT";
  case EyeAction::Confused:
    return "CONFUSED";
  case EyeAction::Laugh:
    return "LAUGH";
  case EyeAction::Open:
    return "OPEN";
  case EyeAction::Close:
    return "CLOSE";
  case EyeAction::None:
  default:
    return "NONE";
  }
}

bool parseMoodToken(const String &token, uint8_t *moodOut) {
  if (moodOut == nullptr || token.length() == 0) {
    return false;
  }
  String upper = token;
  upper.trim();
  upper.toUpperCase();
  if (upper == "DEFAULT" || upper == "NORMAL" || upper == "NEUTRAL") {
    *moodOut = DEFAULT;
    return true;
  }
  if (upper == "TIRED" || upper == "SLEEPY") {
    *moodOut = TIRED;
    return true;
  }
  if (upper == "ANGRY") {
    *moodOut = ANGRY;
    return true;
  }
  if (upper == "HAPPY") {
    *moodOut = HAPPY;
    return true;
  }
  return false;
}

bool parsePositionToken(const String &token, uint8_t *positionOut) {
  if (positionOut == nullptr || token.length() == 0) {
    return false;
  }
  String upper = token;
  upper.trim();
  upper.toUpperCase();
  if (upper == "DEFAULT" || upper == "CENTER" || upper == "C") {
    *positionOut = DEFAULT;
    return true;
  }
  if (upper == "N") {
    *positionOut = N;
    return true;
  }
  if (upper == "NE") {
    *positionOut = NE;
    return true;
  }
  if (upper == "E") {
    *positionOut = E;
    return true;
  }
  if (upper == "SE") {
    *positionOut = SE;
    return true;
  }
  if (upper == "S") {
    *positionOut = S;
    return true;
  }
  if (upper == "SW") {
    *positionOut = SW;
    return true;
  }
  if (upper == "W") {
    *positionOut = W;
    return true;
  }
  if (upper == "NW") {
    *positionOut = NW;
    return true;
  }
  return false;
}

bool parseActionToken(const String &token, EyeAction *actionOut) {
  if (actionOut == nullptr || token.length() == 0) {
    return false;
  }
  String upper = token;
  upper.trim();
  upper.toUpperCase();
  upper.replace("-", "_");
  if (upper == "NONE") {
    *actionOut = EyeAction::None;
    return true;
  }
  if (upper == "BLINK") {
    *actionOut = EyeAction::Blink;
    return true;
  }
  if (upper == "WINK_LEFT") {
    *actionOut = EyeAction::WinkLeft;
    return true;
  }
  if (upper == "WINK_RIGHT") {
    *actionOut = EyeAction::WinkRight;
    return true;
  }
  if (upper == "CONFUSED") {
    *actionOut = EyeAction::Confused;
    return true;
  }
  if (upper == "LAUGH") {
    *actionOut = EyeAction::Laugh;
    return true;
  }
  if (upper == "OPEN") {
    *actionOut = EyeAction::Open;
    return true;
  }
  if (upper == "CLOSE") {
    *actionOut = EyeAction::Close;
    return true;
  }
  return false;
}

void applyEyeStyle(const EyeStyle &style) {
  if (!g_oledReady) {
    return;
  }

  g_roboEyes.setMood(style.mood);
  g_roboEyes.setPosition(style.position);
  g_roboEyes.setCuriosity(style.curiosity ? ON : OFF);
  g_roboEyes.setSweat(style.sweat ? ON : OFF);
  g_roboEyes.setCyclops(style.cyclops ? ON : OFF);

  if (style.autoBlink) {
    g_roboEyes.setAutoblinker(ON, style.autoBlinkInterval, style.autoBlinkVariation);
  } else {
    g_roboEyes.setAutoblinker(OFF);
  }

  if (style.idle) {
    g_roboEyes.setIdleMode(ON, style.idleInterval, style.idleVariation);
  } else {
    g_roboEyes.setIdleMode(OFF);
  }

  if (style.hFlickerAmp > 0) {
    g_roboEyes.setHFlicker(ON, style.hFlickerAmp);
  } else {
    g_roboEyes.setHFlicker(OFF);
  }

  if (style.vFlickerAmp > 0) {
    g_roboEyes.setVFlicker(ON, style.vFlickerAmp);
  } else {
    g_roboEyes.setVFlicker(OFF);
  }
}

void applyEyeAction(EyeAction action) {
  g_lastEyeAction = actionName(action);
  if (!g_oledReady) {
    return;
  }
  switch (action) {
  case EyeAction::Blink:
    g_roboEyes.blink();
    break;
  case EyeAction::WinkLeft:
    g_roboEyes.blink(true, false);
    break;
  case EyeAction::WinkRight:
    g_roboEyes.blink(false, true);
    break;
  case EyeAction::Confused:
    g_roboEyes.anim_confused();
    break;
  case EyeAction::Laugh:
    g_roboEyes.anim_laugh();
    break;
  case EyeAction::Open:
    g_roboEyes.open();
    break;
  case EyeAction::Close:
    g_roboEyes.close();
    break;
  case EyeAction::None:
  default:
    break;
  }
}

void setEyeStyle(const EyeStyle &style, const String &presetName, uint32_t holdMs) {
  const EyeStyle previousStyle = g_eyeStyle;
  const String previousPreset = g_eyePresetName;

  g_eyeStyle = style;
  g_eyePresetName = presetName;
  g_lastEyeAction = "NONE";
  applyEyeStyle(g_eyeStyle);

  if (holdMs > 0) {
    g_eyeStyleRevert = previousStyle;
    g_eyePresetRevertName = previousPreset;
    g_eyeRevertAtMs = millis() + holdMs;
  } else {
    g_eyeRevertAtMs = 0;
  }
}

bool resolvePreset(const String &token, PresetResolveResult *out) {
  if (out == nullptr || token.length() == 0) {
    return false;
  }

  String upper = token;
  upper.trim();
  upper.toUpperCase();
  upper.replace("-", "_");

  EyeStyle base{};
  base.autoBlink = true;
  base.autoBlinkInterval = 3;
  base.autoBlinkVariation = 2;

  PresetResolveResult result{};
  result.valid = true;
  result.style = base;
  result.label = upper;

  if (upper == "NEUTRAL" || upper == "DEFAULT" || upper == "NORMAL") {
    result.hasStyle = true;
    result.label = "NEUTRAL";
  } else if (upper == "HAPPY") {
    result.hasStyle = true;
    result.style.mood = HAPPY;
    result.label = "HAPPY";
  } else if (upper == "SAD") {
    result.hasStyle = true;
    result.style.mood = TIRED;
    result.style.position = S;
    result.label = "SAD";
  } else if (upper == "ANGRY") {
    result.hasStyle = true;
    result.style.mood = ANGRY;
    result.label = "ANGRY";
  } else if (upper == "SLEEPY" || upper == "TIRED") {
    result.hasStyle = true;
    result.style.mood = TIRED;
    result.style.autoBlink = true;
    result.style.autoBlinkInterval = 1;
    result.style.autoBlinkVariation = 1;
    result.label = "SLEEPY";
  } else if (upper == "SURPRISED") {
    result.hasStyle = true;
    result.style.position = N;
    result.label = "SURPRISED";
  } else if (upper == "LOOK_LEFT") {
    result.hasStyle = true;
    result.style.position = W;
    result.style.curiosity = true;
    result.label = "LOOK_LEFT";
  } else if (upper == "LOOK_RIGHT") {
    result.hasStyle = true;
    result.style.position = E;
    result.style.curiosity = true;
    result.label = "LOOK_RIGHT";
  } else if (upper == "WINK_LEFT") {
    result.hasAction = true;
    result.action = EyeAction::WinkLeft;
    result.label = "WINK_LEFT";
  } else if (upper == "WINK_RIGHT") {
    result.hasAction = true;
    result.action = EyeAction::WinkRight;
    result.label = "WINK_RIGHT";
  } else if (upper == "BLINK") {
    result.hasAction = true;
    result.action = EyeAction::Blink;
    result.label = "BLINK";
  } else if (upper == "CONFUSED") {
    result.hasAction = true;
    result.action = EyeAction::Confused;
    result.label = "CONFUSED";
  } else if (upper == "LAUGH") {
    result.hasAction = true;
    result.action = EyeAction::Laugh;
    result.label = "LAUGH";
  } else {
    return false;
  }

  *out = result;
  return true;
}

bool parseStyleParamsFromArgs(StyleParamResult *out) {
  if (out == nullptr) {
    return false;
  }

  StyleParamResult result{};
  result.style = g_eyeStyle;

  if (g_server.hasArg("mood")) {
    uint8_t mood = DEFAULT;
    if (!parseMoodToken(g_server.arg("mood"), &mood)) {
      return false;
    }
    result.style.mood = mood;
    result.changed = true;
  }
  if (g_server.hasArg("position")) {
    uint8_t position = DEFAULT;
    if (!parsePositionToken(g_server.arg("position"), &position)) {
      return false;
    }
    result.style.position = position;
    result.changed = true;
  }
  if (g_server.hasArg("curiosity")) {
    bool value = false;
    if (!parseBool(g_server.arg("curiosity"), &value)) {
      return false;
    }
    result.style.curiosity = value;
    result.changed = true;
  }
  if (g_server.hasArg("sweat")) {
    bool value = false;
    if (!parseBool(g_server.arg("sweat"), &value)) {
      return false;
    }
    result.style.sweat = value;
    result.changed = true;
  }
  if (g_server.hasArg("cyclops")) {
    bool value = false;
    if (!parseBool(g_server.arg("cyclops"), &value)) {
      return false;
    }
    result.style.cyclops = value;
    result.changed = true;
  }
  if (g_server.hasArg("auto_blink")) {
    bool value = false;
    if (!parseBool(g_server.arg("auto_blink"), &value)) {
      return false;
    }
    result.style.autoBlink = value;
    result.changed = true;
  }
  if (g_server.hasArg("idle")) {
    bool value = false;
    if (!parseBool(g_server.arg("idle"), &value)) {
      return false;
    }
    result.style.idle = value;
    result.changed = true;
  }
  if (g_server.hasArg("auto_blink_interval")) {
    int value = 0;
    if (!parseIntRange(g_server.arg("auto_blink_interval"), 1, 30, &value)) {
      return false;
    }
    result.style.autoBlinkInterval = static_cast<uint8_t>(value);
    result.changed = true;
  }
  if (g_server.hasArg("auto_blink_variation")) {
    int value = 0;
    if (!parseIntRange(g_server.arg("auto_blink_variation"), 0, 30, &value)) {
      return false;
    }
    result.style.autoBlinkVariation = static_cast<uint8_t>(value);
    result.changed = true;
  }
  if (g_server.hasArg("idle_interval")) {
    int value = 0;
    if (!parseIntRange(g_server.arg("idle_interval"), 1, 30, &value)) {
      return false;
    }
    result.style.idleInterval = static_cast<uint8_t>(value);
    result.changed = true;
  }
  if (g_server.hasArg("idle_variation")) {
    int value = 0;
    if (!parseIntRange(g_server.arg("idle_variation"), 0, 30, &value)) {
      return false;
    }
    result.style.idleVariation = static_cast<uint8_t>(value);
    result.changed = true;
  }
  if (g_server.hasArg("hflicker_amp")) {
    int value = 0;
    if (!parseIntRange(g_server.arg("hflicker_amp"), 0, 30, &value)) {
      return false;
    }
    result.style.hFlickerAmp = static_cast<uint8_t>(value);
    result.changed = true;
  }
  if (g_server.hasArg("vflicker_amp")) {
    int value = 0;
    if (!parseIntRange(g_server.arg("vflicker_amp"), 0, 30, &value)) {
      return false;
    }
    result.style.vFlickerAmp = static_cast<uint8_t>(value);
    result.changed = true;
  }
  if (g_server.hasArg("action")) {
    EyeAction action = EyeAction::None;
    if (!parseActionToken(g_server.arg("action"), &action)) {
      return false;
    }
    result.hasAction = true;
    result.action = action;
  }

  *out = result;
  return true;
}

bool parseStyleParamsFromJson(const JsonVariantConst &args, StyleParamResult *out, String *errorOut) {
  if (out == nullptr) {
    return false;
  }
  StyleParamResult result{};
  result.style = g_eyeStyle;

  if (!args.is<JsonObjectConst>()) {
    *out = result;
    return true;
  }
  const JsonObjectConst obj = args.as<JsonObjectConst>();

  if (!obj["mood"].isNull()) {
    String token;
    uint8_t mood = DEFAULT;
    if (!parseJsonString(obj["mood"], &token) || !parseMoodToken(token, &mood)) {
      if (errorOut != nullptr) {
        *errorOut = "bad mood";
      }
      return false;
    }
    result.style.mood = mood;
    result.changed = true;
  }
  if (!obj["position"].isNull()) {
    String token;
    uint8_t position = DEFAULT;
    if (!parseJsonString(obj["position"], &token) || !parsePositionToken(token, &position)) {
      if (errorOut != nullptr) {
        *errorOut = "bad position";
      }
      return false;
    }
    result.style.position = position;
    result.changed = true;
  }
  if (!obj["curiosity"].isNull()) {
    bool value = false;
    if (!parseJsonBool(obj["curiosity"], &value)) {
      if (errorOut != nullptr) {
        *errorOut = "bad curiosity";
      }
      return false;
    }
    result.style.curiosity = value;
    result.changed = true;
  }
  if (!obj["sweat"].isNull()) {
    bool value = false;
    if (!parseJsonBool(obj["sweat"], &value)) {
      if (errorOut != nullptr) {
        *errorOut = "bad sweat";
      }
      return false;
    }
    result.style.sweat = value;
    result.changed = true;
  }
  if (!obj["cyclops"].isNull()) {
    bool value = false;
    if (!parseJsonBool(obj["cyclops"], &value)) {
      if (errorOut != nullptr) {
        *errorOut = "bad cyclops";
      }
      return false;
    }
    result.style.cyclops = value;
    result.changed = true;
  }
  if (!obj["auto_blink"].isNull()) {
    bool value = false;
    if (!parseJsonBool(obj["auto_blink"], &value)) {
      if (errorOut != nullptr) {
        *errorOut = "bad auto_blink";
      }
      return false;
    }
    result.style.autoBlink = value;
    result.changed = true;
  }
  if (!obj["idle"].isNull()) {
    bool value = false;
    if (!parseJsonBool(obj["idle"], &value)) {
      if (errorOut != nullptr) {
        *errorOut = "bad idle";
      }
      return false;
    }
    result.style.idle = value;
    result.changed = true;
  }
  if (!obj["auto_blink_interval"].isNull()) {
    int value = 0;
    if (!parseJsonIntInRange(obj["auto_blink_interval"], 1, 30, &value)) {
      if (errorOut != nullptr) {
        *errorOut = "bad auto_blink_interval";
      }
      return false;
    }
    result.style.autoBlinkInterval = static_cast<uint8_t>(value);
    result.changed = true;
  }
  if (!obj["auto_blink_variation"].isNull()) {
    int value = 0;
    if (!parseJsonIntInRange(obj["auto_blink_variation"], 0, 30, &value)) {
      if (errorOut != nullptr) {
        *errorOut = "bad auto_blink_variation";
      }
      return false;
    }
    result.style.autoBlinkVariation = static_cast<uint8_t>(value);
    result.changed = true;
  }
  if (!obj["idle_interval"].isNull()) {
    int value = 0;
    if (!parseJsonIntInRange(obj["idle_interval"], 1, 30, &value)) {
      if (errorOut != nullptr) {
        *errorOut = "bad idle_interval";
      }
      return false;
    }
    result.style.idleInterval = static_cast<uint8_t>(value);
    result.changed = true;
  }
  if (!obj["idle_variation"].isNull()) {
    int value = 0;
    if (!parseJsonIntInRange(obj["idle_variation"], 0, 30, &value)) {
      if (errorOut != nullptr) {
        *errorOut = "bad idle_variation";
      }
      return false;
    }
    result.style.idleVariation = static_cast<uint8_t>(value);
    result.changed = true;
  }
  if (!obj["hflicker_amp"].isNull()) {
    int value = 0;
    if (!parseJsonIntInRange(obj["hflicker_amp"], 0, 30, &value)) {
      if (errorOut != nullptr) {
        *errorOut = "bad hflicker_amp";
      }
      return false;
    }
    result.style.hFlickerAmp = static_cast<uint8_t>(value);
    result.changed = true;
  }
  if (!obj["vflicker_amp"].isNull()) {
    int value = 0;
    if (!parseJsonIntInRange(obj["vflicker_amp"], 0, 30, &value)) {
      if (errorOut != nullptr) {
        *errorOut = "bad vflicker_amp";
      }
      return false;
    }
    result.style.vFlickerAmp = static_cast<uint8_t>(value);
    result.changed = true;
  }
  if (!obj["action"].isNull()) {
    String token;
    EyeAction action = EyeAction::None;
    if (!parseJsonString(obj["action"], &token) || !parseActionToken(token, &action)) {
      if (errorOut != nullptr) {
        *errorOut = "bad action";
      }
      return false;
    }
    result.hasAction = true;
    result.action = action;
  }

  *out = result;
  return true;
}

bool parseVoiceText(const String &voiceText, Command *cmdOut) {
  if (cmdOut == nullptr) {
    return false;
  }
  String text = voiceText;
  text.trim();
  text.toLowerCase();
  if (text.length() == 0) {
    return false;
  }

  if (text.indexOf("stop") >= 0 || text.indexOf("halt") >= 0 || text.indexOf("停止") >= 0 ||
      text.indexOf("刹车") >= 0 || text.indexOf("停下") >= 0) {
    cmdOut->motion = Motion::Stop;
    cmdOut->durationMs = 0;
    cmdOut->speed = g_defaultSpeed;
    cmdOut->valid = true;
    return true;
  }
  if (text.indexOf("forward") >= 0 || text.indexOf("ahead") >= 0 || text.indexOf("前进") >= 0 ||
      text.indexOf("向前") >= 0) {
    cmdOut->motion = Motion::Forward;
  } else if (text.indexOf("backward") >= 0 || text.indexOf("reverse") >= 0 ||
             text.indexOf("后退") >= 0 || text.indexOf("向后") >= 0) {
    cmdOut->motion = Motion::Backward;
  } else if (text.indexOf("left") >= 0 || text.indexOf("左转") >= 0 || text.indexOf("向左") >= 0) {
    cmdOut->motion = Motion::Left;
  } else if (text.indexOf("right") >= 0 || text.indexOf("右转") >= 0 || text.indexOf("向右") >= 0) {
    cmdOut->motion = Motion::Right;
  } else {
    return false;
  }

  cmdOut->durationMs = robot::DEFAULT_MOVE_MS;
  cmdOut->speed = g_defaultSpeed;
  cmdOut->valid = true;
  return true;
}

Command parseRawCommand(const String &line) {
  Command cmd{};
  String trimmed = line;
  trimmed.trim();
  if (trimmed.length() == 0) {
    return cmd;
  }

  if (trimmed.startsWith("VOICE ") || trimmed.startsWith("voice ")) {
    const int idx = trimmed.indexOf(' ');
    if (idx >= 0) {
      parseVoiceText(trimmed.substring(idx + 1), &cmd);
    }
    return cmd;
  }

  const int firstSpace = trimmed.indexOf(' ');
  String token = (firstSpace < 0) ? trimmed : trimmed.substring(0, firstSpace);
  Motion motion = Motion::Stop;
  if (!parseMotionToken(token, &motion)) {
    return cmd;
  }

  cmd.motion = motion;
  cmd.speed = g_defaultSpeed;
  cmd.durationMs = 0;
  cmd.valid = true;
  if (motion == Motion::Stop) {
    return cmd;
  }

  String rest = (firstSpace < 0) ? "" : trimmed.substring(firstSpace + 1);
  rest.trim();
  if (rest.length() == 0) {
    return cmd;
  }

  const int secondSpace = rest.indexOf(' ');
  String durationToken = (secondSpace < 0) ? rest : rest.substring(0, secondSpace);
  uint32_t durationMs = 0;
  if (parseDuration(durationToken, &durationMs)) {
    cmd.durationMs = durationMs;
  }
  if (secondSpace >= 0) {
    String speedToken = rest.substring(secondSpace + 1);
    speedToken.trim();
    uint8_t speed = g_defaultSpeed;
    if (parseSpeed(speedToken, &speed)) {
      cmd.speed = speed;
    }
  }
  return cmd;
}

void applyCommand(const Command &cmd) {
  if (!cmd.valid) {
    return;
  }
  g_lastCommandAtMs = millis();
  g_currentMotion = cmd.motion;
  g_motor.drive(cmd.motion, cmd.speed);
  if (cmd.motion == Motion::Stop || cmd.durationMs == 0) {
    g_motionStopAtMs = 0;
  } else {
    g_motionStopAtMs = millis() + cmd.durationMs;
  }
}

bool isAuthorized() {
  const String token = String(robot::HTTP_TOKEN);
  if (token.length() == 0) {
    return true;
  }
  String value = g_server.header("X-Robot-Token");
  if (value.length() == 0 && g_server.hasArg("token")) {
    value = g_server.arg("token");
  }
  return value == token;
}

void sendJson(int code, const String &json) { g_server.send(code, "application/json", json); }

String buildStateJson(bool ok) {
  String json = "{\"ok\":";
  json += ok ? "true" : "false";
  json += ",\"ip\":\"";
  json += WiFi.localIP().toString();
  json += "\",\"motion\":\"";
  json += motionName(g_currentMotion);
  json += "\",\"default_speed\":";
  json += String(g_defaultSpeed);
  json += ",\"expression_engine\":\"ROBOEYES\"";
  json += ",\"expression\":\"";
  json += g_eyePresetName;
  json += "\",\"expression_last_action\":\"";
  json += g_lastEyeAction;
  json += "\",\"expression_style\":{";
  json += "\"mood\":\"";
  json += moodName(g_eyeStyle.mood);
  json += "\",\"position\":\"";
  json += positionName(g_eyeStyle.position);
  json += "\",\"curiosity\":";
  json += g_eyeStyle.curiosity ? "true" : "false";
  json += ",\"sweat\":";
  json += g_eyeStyle.sweat ? "true" : "false";
  json += ",\"cyclops\":";
  json += g_eyeStyle.cyclops ? "true" : "false";
  json += ",\"auto_blink\":";
  json += g_eyeStyle.autoBlink ? "true" : "false";
  json += ",\"auto_blink_interval\":";
  json += String(g_eyeStyle.autoBlinkInterval);
  json += ",\"auto_blink_variation\":";
  json += String(g_eyeStyle.autoBlinkVariation);
  json += ",\"idle\":";
  json += g_eyeStyle.idle ? "true" : "false";
  json += ",\"idle_interval\":";
  json += String(g_eyeStyle.idleInterval);
  json += ",\"idle_variation\":";
  json += String(g_eyeStyle.idleVariation);
  json += ",\"hflicker_amp\":";
  json += String(g_eyeStyle.hFlickerAmp);
  json += ",\"vflicker_amp\":";
  json += String(g_eyeStyle.vFlickerAmp);
  json += "}";
  json += ",\"oled_ready\":";
  json += g_oledReady ? "true" : "false";
  json += ",\"uptime_ms\":";
  json += String(millis());
  json += "}";
  return json;
}

String escapeJsonString(const String &input) {
  String output;
  output.reserve(input.length() + 8);
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input.charAt(i);
    switch (c) {
    case '\\':
      output += "\\\\";
      break;
    case '"':
      output += "\\\"";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      output += c;
      break;
    }
  }
  return output;
}

bool readJsonStringArg(const JsonObjectConst &obj, const char *key, String *out) {
  if (out == nullptr) {
    return false;
  }
  const JsonVariantConst v = obj[key];
  if (v.isNull()) {
    return false;
  }
  return parseJsonString(v, out);
}

bool readJsonUIntArg(const JsonObjectConst &obj, const char *key, uint32_t minValue, uint32_t maxValue,
                     bool *hasValue, uint32_t *out) {
  if (hasValue == nullptr || out == nullptr) {
    return false;
  }
  *hasValue = false;
  const JsonVariantConst v = obj[key];
  if (v.isNull()) {
    return true;
  }
  *hasValue = true;

  int parsed = 0;
  if (!parseJsonIntInRange(v, static_cast<int>(minValue), static_cast<int>(maxValue), &parsed)) {
    return false;
  }
  *out = static_cast<uint32_t>(parsed);
  return true;
}

bool readJsonU8Arg(const JsonObjectConst &obj, const char *key, uint8_t minValue, uint8_t maxValue,
                   bool *hasValue, uint8_t *out) {
  if (hasValue == nullptr || out == nullptr) {
    return false;
  }
  *hasValue = false;
  const JsonVariantConst v = obj[key];
  if (v.isNull()) {
    return true;
  }
  *hasValue = true;

  int parsed = 0;
  if (!parseJsonIntInRange(v, static_cast<int>(minValue), static_cast<int>(maxValue), &parsed)) {
    return false;
  }
  *out = static_cast<uint8_t>(parsed);
  return true;
}

bool executeRawCommand(const String &raw, String *resultJson, String *error) {
  String text = raw;
  text.trim();
  if (text.length() == 0) {
    if (error != nullptr) {
      *error = "missing command";
    }
    return false;
  }

  if (text.startsWith("EXPR ") || text.startsWith("EXPRESSION ")) {
    const int idx = text.indexOf(' ');
    PresetResolveResult preset{};
    if (!resolvePreset(text.substring(idx + 1), &preset)) {
      if (error != nullptr) {
        *error = "bad expression";
      }
      return false;
    }
    StyleParamResult none{};
    applyExpressionFromRequest(none, true, preset, 0, 0);
    if (resultJson != nullptr) {
      *resultJson = "{\"ok\":true}";
    }
    return true;
  }

  if (text.startsWith("ACTION ")) {
    const int idx = text.indexOf(' ');
    EyeAction action = EyeAction::None;
    if (!parseActionToken(text.substring(idx + 1), &action)) {
      if (error != nullptr) {
        *error = "bad action";
      }
      return false;
    }
    applyEyeAction(action);
    if (resultJson != nullptr) {
      *resultJson = "{\"ok\":true}";
    }
    return true;
  }

  const Command cmd = parseRawCommand(text);
  if (!cmd.valid) {
    if (error != nullptr) {
      *error = "bad command";
    }
    return false;
  }
  applyCommand(cmd);
  if (resultJson != nullptr) {
    *resultJson = "{\"ok\":true}";
  }
  return true;
}

bool executeCommandFromJson(const String &typeInput, const JsonVariantConst &argsVariant, String *resultJson,
                            String *errorOut) {
  String type = typeInput;
  type.trim();
  type.toUpperCase();

  JsonObjectConst args;
  if (argsVariant.is<JsonObjectConst>()) {
    args = argsVariant.as<JsonObjectConst>();
  }

  if (type == "PING") {
    *resultJson = "{\"ok\":true,\"reply\":\"PONG\"}";
    return true;
  }
  if (type == "STATE" || type == "HEALTH") {
    *resultJson = buildStateJson(true);
    return true;
  }
  if (type == "STOP") {
    Command stop{};
    stop.motion = Motion::Stop;
    stop.durationMs = 0;
    stop.speed = g_defaultSpeed;
    stop.valid = true;
    applyCommand(stop);
    *resultJson = "{\"ok\":true,\"motion\":\"STOP\"}";
    return true;
  }
  if (type == "SPEED") {
    bool hasSpeed = false;
    uint8_t speed = g_defaultSpeed;
    if (!readJsonU8Arg(args, "speed", 0, 255, &hasSpeed, &speed) || !hasSpeed) {
      if (errorOut != nullptr) {
        *errorOut = "missing or bad speed";
      }
      return false;
    }
    g_defaultSpeed = speed;
    *resultJson = String("{\"ok\":true,\"speed\":") + String(g_defaultSpeed) + "}";
    return true;
  }
  if (type == "RAW") {
    String raw;
    if (!readJsonStringArg(args, "command", &raw)) {
      if (errorOut != nullptr) {
        *errorOut = "missing command";
      }
      return false;
    }
    return executeRawCommand(raw, resultJson, errorOut);
  }
  if (type == "EXPRESSION") {
    String expressionName;
    if (!readJsonStringArg(args, "name", &expressionName)) {
      if (errorOut != nullptr) {
        *errorOut = "missing name";
      }
      return false;
    }

    PresetResolveResult preset{};
    if (!resolvePreset(expressionName, &preset)) {
      if (errorOut != nullptr) {
        *errorOut = "bad expression";
      }
      return false;
    }

    uint32_t holdMs = 0;
    bool hasHold = false;
    if (!readJsonUIntArg(args, "hold_ms", 1, robot::MAX_EXPRESSION_HOLD_MS, &hasHold, &holdMs)) {
      if (errorOut != nullptr) {
        *errorOut = "bad hold_ms";
      }
      return false;
    }

    StyleParamResult none{};
    applyExpressionFromRequest(none, true, preset, hasHold ? holdMs : 0, 0);

    *resultJson = "{\"ok\":true,\"expression\":\"" + g_eyePresetName +
                  "\",\"expression_last_action\":\"" + g_lastEyeAction + "\"}";
    return true;
  }
  if (type == "EXPRESSION_PARAM") {
    StyleParamResult styleParams{};
    String parseError;
    if (!parseStyleParamsFromJson(argsVariant, &styleParams, &parseError)) {
      if (errorOut != nullptr) {
        *errorOut = parseError.length() > 0 ? parseError : String("bad expression param");
      }
      return false;
    }
    if (!styleParams.changed && !styleParams.hasAction) {
      if (errorOut != nullptr) {
        *errorOut = "missing expression param";
      }
      return false;
    }

    uint32_t holdMs = 0;
    bool hasHold = false;
    if (!readJsonUIntArg(args, "hold_ms", 1, robot::MAX_EXPRESSION_HOLD_MS, &hasHold, &holdMs)) {
      if (errorOut != nullptr) {
        *errorOut = "bad hold_ms";
      }
      return false;
    }

    PresetResolveResult none{};
    applyExpressionFromRequest(styleParams, false, none, hasHold ? holdMs : 0, 0);
    *resultJson = "{\"ok\":true,\"expression\":\"" + g_eyePresetName +
                  "\",\"expression_last_action\":\"" + g_lastEyeAction + "\"}";
    return true;
  }
  if (type == "MOVE") {
    Command cmd{};
    String rawCommand;
    if (readJsonStringArg(args, "command", &rawCommand)) {
      cmd = parseRawCommand(rawCommand);
    } else {
      String direction;
      if (!readJsonStringArg(args, "direction", &direction)) {
        if (errorOut != nullptr) {
          *errorOut = "missing direction or command";
        }
        return false;
      }
      Motion motion = Motion::Stop;
      if (!parseMotionToken(direction, &motion)) {
        if (errorOut != nullptr) {
          *errorOut = "bad direction";
        }
        return false;
      }
      cmd.motion = motion;
      cmd.speed = g_defaultSpeed;
      cmd.durationMs = 0;
      cmd.valid = true;

      bool hasDuration = false;
      uint32_t durationMs = 0;
      if (!readJsonUIntArg(args, "duration_ms", 1, robot::MAX_MOVE_MS, &hasDuration, &durationMs)) {
        if (errorOut != nullptr) {
          *errorOut = "bad duration_ms";
        }
        return false;
      }
      if (hasDuration && motion != Motion::Stop) {
        cmd.durationMs = durationMs;
      }

      bool hasSpeed = false;
      uint8_t speed = g_defaultSpeed;
      if (!readJsonU8Arg(args, "speed", 0, 255, &hasSpeed, &speed)) {
        if (errorOut != nullptr) {
          *errorOut = "bad speed";
        }
        return false;
      }
      if (hasSpeed) {
        cmd.speed = speed;
      }
    }

    if (!cmd.valid) {
      if (errorOut != nullptr) {
        *errorOut = "bad command";
      }
      return false;
    }

    StyleParamResult styleParams{};
    String parseError;
    if (!parseStyleParamsFromJson(argsVariant, &styleParams, &parseError)) {
      if (errorOut != nullptr) {
        *errorOut = parseError.length() > 0 ? parseError : String("bad expression param");
      }
      return false;
    }

    PresetResolveResult preset{};
    bool hasPreset = false;
    String expression;
    if (readJsonStringArg(args, "expression", &expression)) {
      hasPreset = resolvePreset(expression, &preset);
      if (!hasPreset) {
        if (errorOut != nullptr) {
          *errorOut = "bad expression";
        }
        return false;
      }
    }

    uint32_t holdMs = 0;
    bool hasHold = false;
    if (!readJsonUIntArg(args, "expression_hold_ms", 1, robot::MAX_EXPRESSION_HOLD_MS, &hasHold,
                         &holdMs)) {
      if (errorOut != nullptr) {
        *errorOut = "bad expression_hold_ms";
      }
      return false;
    }

    applyCommand(cmd);
    applyExpressionFromRequest(styleParams, hasPreset, preset, hasHold ? holdMs : 0, cmd.durationMs);

    *resultJson = "{\"ok\":true,\"motion\":\"" + motionName(cmd.motion) + "\",\"duration_ms\":" +
                  String(cmd.durationMs) + ",\"speed\":" + String(cmd.speed) + ",\"expression\":\"" +
                  g_eyePresetName + "\",\"expression_last_action\":\"" + g_lastEyeAction + "\"}";
    return true;
  }
  if (type == "TEXT") {
    String text;
    if (!readJsonStringArg(args, "text", &text)) {
      if (errorOut != nullptr) {
        *errorOut = "missing text";
      }
      return false;
    }

    Command cmd{};
    const bool hasMotion = parseVoiceText(text, &cmd);

    StyleParamResult styleParams{};
    String parseError;
    if (!parseStyleParamsFromJson(argsVariant, &styleParams, &parseError)) {
      if (errorOut != nullptr) {
        *errorOut = parseError.length() > 0 ? parseError : String("bad expression param");
      }
      return false;
    }

    PresetResolveResult preset{};
    bool hasPreset = false;
    String expression;
    if (readJsonStringArg(args, "expression", &expression)) {
      hasPreset = resolvePreset(expression, &preset);
      if (!hasPreset) {
        if (errorOut != nullptr) {
          *errorOut = "bad expression";
        }
        return false;
      }
    }

    if (!hasMotion && !hasPreset && !styleParams.changed && !styleParams.hasAction) {
      if (errorOut != nullptr) {
        *errorOut = "text parse failed";
      }
      return false;
    }

    if (hasMotion) {
      bool hasDuration = false;
      uint32_t durationMs = 0;
      if (!readJsonUIntArg(args, "duration_ms", 1, robot::MAX_MOVE_MS, &hasDuration, &durationMs)) {
        if (errorOut != nullptr) {
          *errorOut = "bad duration_ms";
        }
        return false;
      }
      if (hasDuration && cmd.motion != Motion::Stop) {
        cmd.durationMs = durationMs;
      }

      bool hasSpeed = false;
      uint8_t speed = g_defaultSpeed;
      if (!readJsonU8Arg(args, "speed", 0, 255, &hasSpeed, &speed)) {
        if (errorOut != nullptr) {
          *errorOut = "bad speed";
        }
        return false;
      }
      if (hasSpeed) {
        cmd.speed = speed;
      }

      applyCommand(cmd);
    }

    uint32_t holdMs = 0;
    bool hasHold = false;
    if (!readJsonUIntArg(args, "expression_hold_ms", 1, robot::MAX_EXPRESSION_HOLD_MS, &hasHold,
                         &holdMs)) {
      if (errorOut != nullptr) {
        *errorOut = "bad expression_hold_ms";
      }
      return false;
    }
    applyExpressionFromRequest(styleParams, hasPreset, preset, hasHold ? holdMs : 0,
                               hasMotion ? cmd.durationMs : 0);

    *resultJson = "{\"ok\":true,\"motion\":\"" + String(hasMotion ? motionName(cmd.motion) : "NONE") +
                  "\",\"duration_ms\":" + String(hasMotion ? cmd.durationMs : 0) + ",\"speed\":" +
                  String(hasMotion ? cmd.speed : g_defaultSpeed) + ",\"expression\":\"" + g_eyePresetName +
                  "\",\"expression_last_action\":\"" + g_lastEyeAction + "\"}";
    return true;
  }

  if (errorOut != nullptr) {
    *errorOut = "unknown command type";
  }
  return false;
}

void handleHealth() { sendJson(200, buildStateJson(true)); }

void handlePing() {
  if (!isAuthorized()) {
    sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
    return;
  }
  sendJson(200, "{\"ok\":true,\"reply\":\"PONG\"}");
}

void handleState() {
  if (!isAuthorized()) {
    sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
    return;
  }
  sendJson(200, buildStateJson(true));
}

void applyExpressionFromRequest(const StyleParamResult &styleParams, bool hasPreset,
                                const PresetResolveResult &preset, uint32_t holdMs,
                                uint32_t fallbackHoldMs) {
  bool styleChanged = false;
  EyeStyle style = g_eyeStyle;
  String presetName = g_eyePresetName;

  if (hasPreset && preset.hasStyle) {
    style = preset.style;
    presetName = preset.label;
    styleChanged = true;
  }
  if (styleParams.changed) {
    style = styleParams.style;
    presetName = "PARAM";
    styleChanged = true;
  }

  uint32_t actualHold = holdMs;
  if (actualHold == 0 && fallbackHoldMs > 0) {
    actualHold = fallbackHoldMs;
  }
  if (styleChanged) {
    setEyeStyle(style, presetName, actualHold);
  }

  EyeAction action = EyeAction::None;
  bool hasAction = false;
  if (hasPreset && preset.hasAction) {
    action = preset.action;
    hasAction = true;
  }
  if (styleParams.hasAction) {
    action = styleParams.action;
    hasAction = true;
  }
  if (hasAction) {
    applyEyeAction(action);
  }
}

void handleMove() {
  if (!isAuthorized()) {
    sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
    return;
  }

  Command cmd{};
  if (g_server.hasArg("command")) {
    cmd = parseRawCommand(g_server.arg("command"));
  } else if (g_server.hasArg("direction")) {
    Motion motion = Motion::Stop;
    if (!parseMotionToken(g_server.arg("direction"), &motion)) {
      sendJson(400, "{\"ok\":false,\"error\":\"bad direction\"}");
      return;
    }
    cmd.motion = motion;
    cmd.speed = g_defaultSpeed;
    cmd.durationMs = 0;
    cmd.valid = true;
    if (motion != Motion::Stop && g_server.hasArg("duration_ms")) {
      parseDuration(g_server.arg("duration_ms"), &cmd.durationMs);
    }
    if (g_server.hasArg("speed")) {
      parseSpeed(g_server.arg("speed"), &cmd.speed);
    }
  } else {
    sendJson(400, "{\"ok\":false,\"error\":\"missing direction or command\"}");
    return;
  }

  if (!cmd.valid) {
    sendJson(400, "{\"ok\":false,\"error\":\"bad command\"}");
    return;
  }

  StyleParamResult styleParams{};
  if (!parseStyleParamsFromArgs(&styleParams)) {
    sendJson(400, "{\"ok\":false,\"error\":\"bad expression param\"}");
    return;
  }

  PresetResolveResult preset{};
  bool hasPreset = false;
  if (g_server.hasArg("expression")) {
    hasPreset = resolvePreset(g_server.arg("expression"), &preset);
    if (!hasPreset) {
      sendJson(400, "{\"ok\":false,\"error\":\"bad expression\"}");
      return;
    }
  }

  uint32_t holdMs = 0;
  if (g_server.hasArg("expression_hold_ms")) {
    parseHoldMs(g_server.arg("expression_hold_ms"), &holdMs);
  }

  applyCommand(cmd);
  applyExpressionFromRequest(styleParams, hasPreset, preset, holdMs, cmd.durationMs);

  String json = "{\"ok\":true,\"motion\":\"";
  json += motionName(cmd.motion);
  json += "\",\"duration_ms\":";
  json += String(cmd.durationMs);
  json += ",\"speed\":";
  json += String(cmd.speed);
  json += ",\"expression\":\"";
  json += g_eyePresetName;
  json += "\",\"expression_last_action\":\"";
  json += g_lastEyeAction;
  json += "\"}";
  sendJson(200, json);
}

void handleText() {
  if (!isAuthorized()) {
    sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
    return;
  }
  if (!g_server.hasArg("text")) {
    sendJson(400, "{\"ok\":false,\"error\":\"missing text\"}");
    return;
  }

  const String text = g_server.arg("text");
  Command cmd{};
  const bool hasMotion = parseVoiceText(text, &cmd);

  StyleParamResult styleParams{};
  if (!parseStyleParamsFromArgs(&styleParams)) {
    sendJson(400, "{\"ok\":false,\"error\":\"bad expression param\"}");
    return;
  }

  PresetResolveResult preset{};
  bool hasPreset = false;
  if (g_server.hasArg("expression")) {
    hasPreset = resolvePreset(g_server.arg("expression"), &preset);
    if (!hasPreset) {
      sendJson(400, "{\"ok\":false,\"error\":\"bad expression\"}");
      return;
    }
  }

  if (!hasMotion && !hasPreset && !styleParams.changed && !styleParams.hasAction) {
    sendJson(400, "{\"ok\":false,\"error\":\"text parse failed\"}");
    return;
  }

  if (hasMotion) {
    if (cmd.motion != Motion::Stop && g_server.hasArg("duration_ms")) {
      parseDuration(g_server.arg("duration_ms"), &cmd.durationMs);
    }
    if (g_server.hasArg("speed")) {
      parseSpeed(g_server.arg("speed"), &cmd.speed);
    }
    applyCommand(cmd);
  }

  uint32_t holdMs = 0;
  if (g_server.hasArg("expression_hold_ms")) {
    parseHoldMs(g_server.arg("expression_hold_ms"), &holdMs);
  }
  applyExpressionFromRequest(styleParams, hasPreset, preset, holdMs, hasMotion ? cmd.durationMs : 0);

  String json = "{\"ok\":true,\"motion\":\"";
  json += hasMotion ? motionName(cmd.motion) : String("NONE");
  json += "\",\"duration_ms\":";
  json += hasMotion ? String(cmd.durationMs) : String(0);
  json += ",\"speed\":";
  json += hasMotion ? String(cmd.speed) : String(g_defaultSpeed);
  json += ",\"expression\":\"";
  json += g_eyePresetName;
  json += "\",\"expression_last_action\":\"";
  json += g_lastEyeAction;
  json += "\"}";
  sendJson(200, json);
}

void handleExpression() {
  if (!isAuthorized()) {
    sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
    return;
  }
  if (!g_server.hasArg("name")) {
    sendJson(400, "{\"ok\":false,\"error\":\"missing name\"}");
    return;
  }

  PresetResolveResult preset{};
  if (!resolvePreset(g_server.arg("name"), &preset)) {
    sendJson(400, "{\"ok\":false,\"error\":\"bad expression\"}");
    return;
  }

  uint32_t holdMs = 0;
  if (g_server.hasArg("hold_ms")) {
    parseHoldMs(g_server.arg("hold_ms"), &holdMs);
  }

  StyleParamResult none{};
  applyExpressionFromRequest(none, true, preset, holdMs, 0);

  String json = "{\"ok\":true,\"expression\":\"";
  json += g_eyePresetName;
  json += "\",\"expression_last_action\":\"";
  json += g_lastEyeAction;
  json += "\",\"hold_ms\":";
  json += String(holdMs);
  json += "}";
  sendJson(200, json);
}

void handleExpressionParam() {
  if (!isAuthorized()) {
    sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
    return;
  }

  StyleParamResult styleParams{};
  if (!parseStyleParamsFromArgs(&styleParams)) {
    sendJson(400, "{\"ok\":false,\"error\":\"bad expression param\"}");
    return;
  }
  if (!styleParams.changed && !styleParams.hasAction) {
    sendJson(400, "{\"ok\":false,\"error\":\"missing expression param\"}");
    return;
  }

  uint32_t holdMs = 0;
  if (g_server.hasArg("hold_ms")) {
    parseHoldMs(g_server.arg("hold_ms"), &holdMs);
  }

  PresetResolveResult none{};
  applyExpressionFromRequest(styleParams, false, none, holdMs, 0);

  String json = "{\"ok\":true,\"expression\":\"";
  json += g_eyePresetName;
  json += "\",\"expression_last_action\":\"";
  json += g_lastEyeAction;
  json += "\",\"hold_ms\":";
  json += String(holdMs);
  json += "}";
  sendJson(200, json);
}

void handleStop() {
  if (!isAuthorized()) {
    sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
    return;
  }
  Command stop{};
  stop.motion = Motion::Stop;
  stop.durationMs = 0;
  stop.speed = g_defaultSpeed;
  stop.valid = true;
  applyCommand(stop);
  sendJson(200, "{\"ok\":true,\"motion\":\"STOP\"}");
}

void handleSpeed() {
  if (!isAuthorized()) {
    sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
    return;
  }
  if (!g_server.hasArg("speed")) {
    sendJson(400, "{\"ok\":false,\"error\":\"missing speed\"}");
    return;
  }
  uint8_t speed = g_defaultSpeed;
  if (!parseSpeed(g_server.arg("speed"), &speed)) {
    sendJson(400, "{\"ok\":false,\"error\":\"bad speed\"}");
    return;
  }
  g_defaultSpeed = speed;
  String json = "{\"ok\":true,\"speed\":";
  json += String(g_defaultSpeed);
  json += "}";
  sendJson(200, json);
}

void handleRaw() {
  if (!isAuthorized()) {
    sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
    return;
  }
  if (!g_server.hasArg("command")) {
    sendJson(400, "{\"ok\":false,\"error\":\"missing command\"}");
    return;
  }

  String raw = g_server.arg("command");
  raw.trim();

  if (raw.startsWith("EXPR ") || raw.startsWith("EXPRESSION ")) {
    const int idx = raw.indexOf(' ');
    PresetResolveResult preset{};
    if (!resolvePreset(raw.substring(idx + 1), &preset)) {
      sendJson(400, "{\"ok\":false,\"error\":\"bad expression\"}");
      return;
    }
    StyleParamResult none{};
    applyExpressionFromRequest(none, true, preset, 0, 0);
    sendJson(200, "{\"ok\":true}");
    return;
  }

  if (raw.startsWith("ACTION ")) {
    const int idx = raw.indexOf(' ');
    EyeAction action = EyeAction::None;
    if (!parseActionToken(raw.substring(idx + 1), &action)) {
      sendJson(400, "{\"ok\":false,\"error\":\"bad action\"}");
      return;
    }
    applyEyeAction(action);
    sendJson(200, "{\"ok\":true}");
    return;
  }

  Command cmd = parseRawCommand(raw);
  if (!cmd.valid) {
    sendJson(400, "{\"ok\":false,\"error\":\"bad command\"}");
    return;
  }
  applyCommand(cmd);
  sendJson(200, "{\"ok\":true}");
}

void handleNotFound() { sendJson(404, "{\"ok\":false,\"error\":\"not_found\"}"); }

bool connectWifi() {
  const String ssid = String(robot::WIFI_SSID);
  if (ssid.length() == 0 || ssid.startsWith("YOUR_")) {
    return false;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(robot::WIFI_SSID, robot::WIFI_PASSWORD);
  const uint32_t startAt = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startAt > robot::WIFI_CONNECT_TIMEOUT_MS) {
      return false;
    }
    delay(300);
  }
  return true;
}

bool setupEyes() {
  if (!robot::OLED_ENABLED) {
    return false;
  }
  Wire.begin(robot::OLED_SDA_PIN, robot::OLED_SCL_PIN);
  if (!g_oled.begin(SSD1306_SWITCHCAPVCC, robot::OLED_I2C_ADDRESS)) {
    return false;
  }
  g_oled.clearDisplay();
  g_oled.display();
  g_roboEyes.begin(robot::OLED_WIDTH, robot::OLED_HEIGHT, robot::OLED_MAX_FPS);
  g_roboEyes.setDisplayColors(0, 1);
  g_oledReady = true;
  applyEyeStyle(g_eyeStyle);
  g_roboEyes.open();
  return true;
}

void buildMqttTopics() {
  const String prefix = String(robot::MQTT_TOPIC_PREFIX);
  const String base = prefix + "/robots/" + g_robotId;
  g_mqttTopicRegister = base + "/register";
  g_mqttTopicStatus = base + "/status";
  g_mqttTopicCommand = base + "/cmd";
  g_mqttTopicAck = base + "/ack";
}

bool publishMqtt(const String &topic, const String &payload, bool retained) {
  if (!robot::MQTT_ENABLED || !g_mqttClient.connected()) {
    return false;
  }
  return g_mqttClient.publish(topic.c_str(), payload.c_str(), retained);
}

void publishMqttStatus(bool retained) {
  String payload = buildStateJson(true);
  if (payload.endsWith("}")) {
    payload.remove(payload.length() - 1);
    payload += ",\"robot_id\":\"";
    payload += escapeJsonString(g_robotId);
    payload += "\",\"ts_ms\":";
    payload += String(millis());
    payload += "}";
  }
  publishMqtt(g_mqttTopicStatus, payload, retained);
}

void publishMqttRegister() {
  String payload = "{\"ok\":true,\"robot_id\":\"";
  payload += escapeJsonString(g_robotId);
  payload += "\",\"ip\":\"";
  payload += WiFi.localIP().toString();
  payload += "\",\"mqtt_cmd_topic\":\"";
  payload += g_mqttTopicCommand;
  payload += "\",\"mqtt_ack_topic\":\"";
  payload += g_mqttTopicAck;
  payload += "\",\"features\":[\"move\",\"text\",\"expression\",\"expression_param\",\"raw\",\"state\"]}";
  publishMqtt(g_mqttTopicRegister, payload, true);
}

void publishMqttAck(const String &reqId, bool ok, const String &resultOrError, bool resultIsJson) {
  String payload = "{\"robot_id\":\"";
  payload += escapeJsonString(g_robotId);
  payload += "\",\"req_id\":\"";
  payload += escapeJsonString(reqId);
  payload += "\",\"ok\":";
  payload += ok ? "true" : "false";
  if (ok) {
    payload += ",\"result\":";
    payload += resultIsJson ? resultOrError : ("\"" + escapeJsonString(resultOrError) + "\"");
  } else {
    payload += ",\"error\":\"";
    payload += escapeJsonString(resultOrError);
    payload += "\"";
  }
  payload += "}";
  publishMqtt(g_mqttTopicAck, payload, false);
}

void onMqttMessage(char *topic, byte *payload, unsigned int length) {
  (void)topic;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    publishMqttAck("", false, String("bad json: ") + String(err.c_str()), false);
    return;
  }

  String reqId = "";
  if (!doc["req_id"].isNull()) {
    reqId = String(doc["req_id"] | "");
  }
  String type = "";
  if (!doc["type"].isNull()) {
    type = String(doc["type"] | "");
  }
  if (type.length() == 0) {
    publishMqttAck(reqId, false, "missing type", false);
    return;
  }

  String resultJson;
  String errorText;
  const bool ok = executeCommandFromJson(type, doc["args"], &resultJson, &errorText);
  if (!ok) {
    publishMqttAck(reqId, false, errorText.length() > 0 ? errorText : String("command failed"), false);
    return;
  }

  publishMqttAck(reqId, true, resultJson, true);
  publishMqttStatus(false);
}

bool connectMqtt() {
  if (!robot::MQTT_ENABLED || WiFi.status() != WL_CONNECTED) {
    return false;
  }
  if (g_mqttClient.connected()) {
    return true;
  }

  String clientId = String(robot::MQTT_CLIENT_ID_PREFIX) + g_robotId + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  clientId.toLowerCase();
  String willPayload = String("{\"ok\":false,\"robot_id\":\"") + escapeJsonString(g_robotId) +
                       "\",\"online\":false,\"error\":\"mqtt_disconnected\"}";

  bool connected = false;
  if (String(robot::MQTT_USERNAME).length() > 0) {
    connected = g_mqttClient.connect(clientId.c_str(), robot::MQTT_USERNAME, robot::MQTT_PASSWORD,
                                     g_mqttTopicStatus.c_str(), 0, true, willPayload.c_str());
  } else {
    connected = g_mqttClient.connect(clientId.c_str(), g_mqttTopicStatus.c_str(), 0, true,
                                     willPayload.c_str());
  }
  if (!connected) {
    return false;
  }

  g_mqttClient.subscribe(g_mqttTopicCommand.c_str(), 0);
  publishMqttRegister();
  publishMqttStatus(true);
  g_lastMqttHeartbeatAtMs = millis();
  return true;
}

void runMqttTasks() {
  if (!robot::MQTT_ENABLED || WiFi.status() != WL_CONNECTED) {
    return;
  }
  const uint32_t now = millis();
  if (!g_mqttClient.connected()) {
    if (now - g_lastMqttReconnectAttemptAtMs >= robot::MQTT_RECONNECT_INTERVAL_MS) {
      g_lastMqttReconnectAttemptAtMs = now;
      connectMqtt();
    }
    return;
  }

  g_mqttClient.loop();
  if (robot::MQTT_HEARTBEAT_MS > 0 &&
      (now - g_lastMqttHeartbeatAtMs >= static_cast<uint32_t>(robot::MQTT_HEARTBEAT_MS))) {
    g_lastMqttHeartbeatAtMs = now;
    publishMqttStatus(false);
  }
}

void startHttpServer() {
  g_server.on("/health", HTTP_GET, handleHealth);
  g_server.on("/ping", HTTP_ANY, handlePing);
  g_server.on("/api/state", HTTP_ANY, handleState);
  g_server.on("/api/move", HTTP_ANY, handleMove);
  g_server.on("/api/text", HTTP_ANY, handleText);
  g_server.on("/api/expression", HTTP_ANY, handleExpression);
  g_server.on("/api/expression/param", HTTP_ANY, handleExpressionParam);
  g_server.on("/api/stop", HTTP_ANY, handleStop);
  g_server.on("/api/speed", HTTP_ANY, handleSpeed);
  g_server.on("/api/raw", HTTP_ANY, handleRaw);
  g_server.onNotFound(handleNotFound);
  g_server.begin();
}

void runSafetyStop() {
  const uint32_t now = millis();
  if (g_motionStopAtMs != 0 && static_cast<int32_t>(now - g_motionStopAtMs) >= 0) {
    g_motor.stop();
    g_currentMotion = Motion::Stop;
    g_motionStopAtMs = 0;
  }
  if (robot::FAILSAFE_STOP_MS > 0 && g_lastCommandAtMs > 0 &&
      static_cast<int32_t>(now - g_lastCommandAtMs) >= static_cast<int32_t>(robot::FAILSAFE_STOP_MS)) {
    g_motor.stop();
    g_currentMotion = Motion::Stop;
    g_lastCommandAtMs = now;
  }
}

void runEyesTasks() {
  const uint32_t now = millis();
  if (g_eyeRevertAtMs != 0 && static_cast<int32_t>(now - g_eyeRevertAtMs) >= 0) {
    g_eyeRevertAtMs = 0;
    g_eyeStyle = g_eyeStyleRevert;
    g_eyePresetName = g_eyePresetRevertName;
    g_lastEyeAction = "NONE";
    applyEyeStyle(g_eyeStyle);
  }
  if (g_oledReady) {
    g_roboEyes.update();
  }
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  randomSeed(static_cast<uint32_t>(micros()));

  g_motor.begin();
  EyeStyle bootStyle{};
  bootStyle.autoBlink = true;
  bootStyle.autoBlinkInterval = 3;
  bootStyle.autoBlinkVariation = 2;
  g_eyeStyle = bootStyle;
  g_eyeStyleRevert = bootStyle;
  buildMqttTopics();

  const bool oledOk = setupEyes();
  const bool wifiOk = connectWifi();
  bool mqttOk = false;
  if (robot::MQTT_ENABLED) {
    g_mqttClient.setServer(robot::MQTT_BROKER, robot::MQTT_PORT);
    g_mqttClient.setCallback(onMqttMessage);
    if (wifiOk) {
      mqttOk = connectMqtt();
    }
  }
  startHttpServer();

  Serial.println();
  Serial.println("MCP Robot Executor Ready (HTTP + MQTT)");
  Serial.printf("WiFi: %s\n", wifiOk ? "connected" : "failed");
  if (wifiOk) {
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
  }
  Serial.printf("OLED: %s\n", oledOk ? "ready (RoboEyes)" : "disabled_or_not_found");
  if (robot::MQTT_ENABLED) {
    Serial.printf("MQTT: %s\n", mqttOk ? "connected" : "disconnected");
    Serial.printf("MQTT broker: %s:%u\n", robot::MQTT_BROKER, robot::MQTT_PORT);
    Serial.printf("Robot ID: %s\n", g_robotId.c_str());
    Serial.printf("MQTT cmd topic: %s\n", g_mqttTopicCommand.c_str());
    Serial.printf("MQTT ack topic: %s\n", g_mqttTopicAck.c_str());
  } else {
    Serial.println("MQTT: disabled");
  }
  Serial.println("HTTP API:");
  Serial.println("  GET  /health");
  Serial.println("  ANY  /ping");
  Serial.println("  ANY  /api/state");
  Serial.println("  ANY  /api/move?direction=FORWARD&duration_ms=800&speed=180&expression=HAPPY");
  Serial.println("  ANY  /api/expression?name=CONFUSED");
  Serial.println(
      "  ANY  /api/expression/param?mood=ANGRY&position=NE&curiosity=1&hflicker_amp=2&action=BLINK");
  Serial.println("  ANY  /api/text?text=向左转并且开心一点");
  Serial.println("  ANY  /api/stop");
  Serial.println("  ANY  /api/speed?speed=200");
  Serial.println("  ANY  /api/raw?command=EXPR%20HAPPY");
}

void loop() {
  g_server.handleClient();
  runMqttTasks();
  runSafetyStop();
  runEyesTasks();
  delay(2);
}
