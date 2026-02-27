#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <ctype.h>
#include <string.h>

#include "robot_config.h"

namespace {

enum class Motion {
  Stop = 0,
  Forward,
  Backward,
  Left,
  Right,
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

struct Command {
  Motion motion = Motion::Stop;
  uint32_t durationMs = 0;
  uint8_t speed = robot::DEFAULT_SPEED;
  bool valid = false;
};

MotorDriver g_motor;
WebServer g_server(robot::HTTP_PORT);
uint8_t g_defaultSpeed = robot::DEFAULT_SPEED;
uint32_t g_motionStopAtMs = 0;
uint32_t g_lastCommandAtMs = 0;

bool parseDuration(const String &token, uint32_t *durationMsOut) {
  if (durationMsOut == nullptr || token.length() == 0) {
    return false;
  }
  const long value = token.toInt();
  if (value <= 0) {
    return false;
  }
  *durationMsOut = static_cast<uint32_t>(constrain(value, 1L, static_cast<long>(robot::MAX_MOVE_MS)));
  return true;
}

bool parseSpeed(const String &token, uint8_t *speedOut) {
  if (speedOut == nullptr || token.length() == 0) {
    return false;
  }
  const long value = token.toInt();
  if (value < 0) {
    return false;
  }
  *speedOut = static_cast<uint8_t>(constrain(value, 0L, 255L));
  return true;
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
    if (idx < 0) {
      return cmd;
    }
    String voiceText = trimmed.substring(idx + 1);
    parseVoiceText(voiceText, &cmd);
    return cmd;
  }

  int firstSpace = trimmed.indexOf(' ');
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

  int secondSpace = rest.indexOf(' ');
  String durationToken = (secondSpace < 0) ? rest : rest.substring(0, secondSpace);
  uint32_t durationMs = 0;
  if (parseDuration(durationToken, &durationMs)) {
    cmd.durationMs = durationMs;
  }
  if (secondSpace < 0) {
    return cmd;
  }

  String speedToken = rest.substring(secondSpace + 1);
  speedToken.trim();
  uint8_t speed = g_defaultSpeed;
  if (parseSpeed(speedToken, &speed)) {
    cmd.speed = speed;
  }
  return cmd;
}

void applyCommand(const Command &cmd) {
  if (!cmd.valid) {
    return;
  }
  g_lastCommandAtMs = millis();
  g_motor.drive(cmd.motion, cmd.speed);

  if (cmd.motion == Motion::Stop || cmd.durationMs == 0) {
    g_motionStopAtMs = 0;
  } else {
    g_motionStopAtMs = millis() + cmd.durationMs;
  }
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

void handleHealth() {
  String json = "{\"ok\":true,\"ip\":\"";
  json += WiFi.localIP().toString();
  json += "\",\"port\":";
  json += String(robot::HTTP_PORT);
  json += "}";
  sendJson(200, json);
}

void handlePing() {
  if (!isAuthorized()) {
    sendJson(401, "{\"ok\":false,\"error\":\"unauthorized\"}");
    return;
  }
  sendJson(200, "{\"ok\":true,\"reply\":\"PONG\"}");
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
      uint32_t durationMs = 0;
      if (parseDuration(g_server.arg("duration_ms"), &durationMs)) {
        cmd.durationMs = durationMs;
      }
    }
    if (g_server.hasArg("speed")) {
      uint8_t speed = g_defaultSpeed;
      if (parseSpeed(g_server.arg("speed"), &speed)) {
        cmd.speed = speed;
      }
    }
  } else {
    sendJson(400, "{\"ok\":false,\"error\":\"missing direction or command\"}");
    return;
  }

  if (!cmd.valid) {
    sendJson(400, "{\"ok\":false,\"error\":\"bad command\"}");
    return;
  }

  applyCommand(cmd);
  String json = "{\"ok\":true,\"motion\":\"";
  json += motionName(cmd.motion);
  json += "\",\"duration_ms\":";
  json += String(cmd.durationMs);
  json += ",\"speed\":";
  json += String(cmd.speed);
  json += "}";
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

  Command cmd{};
  if (!parseVoiceText(g_server.arg("text"), &cmd)) {
    sendJson(400, "{\"ok\":false,\"error\":\"text parse failed\"}");
    return;
  }

  if (cmd.motion != Motion::Stop && g_server.hasArg("duration_ms")) {
    uint32_t durationMs = 0;
    if (parseDuration(g_server.arg("duration_ms"), &durationMs)) {
      cmd.durationMs = durationMs;
    }
  }
  if (g_server.hasArg("speed")) {
    uint8_t speed = g_defaultSpeed;
    if (parseSpeed(g_server.arg("speed"), &speed)) {
      cmd.speed = speed;
    }
  }

  applyCommand(cmd);
  String json = "{\"ok\":true,\"motion\":\"";
  json += motionName(cmd.motion);
  json += "\",\"duration_ms\":";
  json += String(cmd.durationMs);
  json += ",\"speed\":";
  json += String(cmd.speed);
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
  Command cmd = parseRawCommand(g_server.arg("command"));
  if (!cmd.valid) {
    sendJson(400, "{\"ok\":false,\"error\":\"bad command\"}");
    return;
  }
  applyCommand(cmd);
  String json = "{\"ok\":true,\"motion\":\"";
  json += motionName(cmd.motion);
  json += "\",\"duration_ms\":";
  json += String(cmd.durationMs);
  json += ",\"speed\":";
  json += String(cmd.speed);
  json += "}";
  sendJson(200, json);
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

void startHttpServer() {
  g_server.on("/health", HTTP_GET, handleHealth);
  g_server.on("/ping", HTTP_ANY, handlePing);
  g_server.on("/api/move", HTTP_ANY, handleMove);
  g_server.on("/api/text", HTTP_ANY, handleText);
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
    g_motionStopAtMs = 0;
  }
  if (robot::FAILSAFE_STOP_MS > 0 && g_lastCommandAtMs > 0 &&
      static_cast<int32_t>(now - g_lastCommandAtMs) >= static_cast<int32_t>(robot::FAILSAFE_STOP_MS)) {
    g_motor.stop();
    g_lastCommandAtMs = now;
  }
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(300);

  g_motor.begin();
  const bool wifiOk = connectWifi();
  startHttpServer();

  Serial.println();
  Serial.println("MCP Robot HTTP Executor Ready");
  Serial.printf("WiFi: %s\n", wifiOk ? "connected" : "failed");
  if (wifiOk) {
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
  }
  Serial.println("HTTP API:");
  Serial.println("  GET  /health");
  Serial.println("  ANY  /ping");
  Serial.println("  ANY  /api/move?direction=FORWARD&duration_ms=800&speed=180");
  Serial.println("  ANY  /api/text?text=向左转&duration_ms=700&speed=180");
  Serial.println("  ANY  /api/stop");
  Serial.println("  ANY  /api/speed?speed=200");
  Serial.println("  ANY  /api/raw?command=FORWARD%20800%20180");
}

void loop() {
  g_server.handleClient();
  runSafetyStop();
  delay(2);
}
