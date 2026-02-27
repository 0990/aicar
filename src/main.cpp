#include <Arduino.h>
#include <U8g2lib.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>
#include <stdlib.h>
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

enum class Expression {
  Neutral = 0,
  Happy,
  Sad,
  Angry,
  Sleepy,
  Surprised,
  LookLeft,
  LookRight,
  WinkLeft,
  WinkRight,
  Blink,
};

struct ExpressionParams {
  int8_t gazeX = 0;        // -10..10
  int8_t gazeY = 0;        // -8..8
  uint8_t openness = 78;   // 0..100
  int8_t browTilt = 0;     // -35..35
  int8_t browLift = 0;     // -12..12
  uint8_t pupilSize = 4;   // 1..8
  int8_t leftOpen = -1;    // -1 means use openness
  int8_t rightOpen = -1;   // -1 means use openness
  bool autoBlink = true;
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

class EyeDisplay {
public:
  EyeDisplay() : m_display(U8G2_R0, U8X8_PIN_NONE) {}

  void begin() {
    if (!robot::OLED_ENABLED) {
      return;
    }
    Wire.begin(robot::OLED_SDA_PIN, robot::OLED_SCL_PIN);
    m_display.setI2CAddress(static_cast<uint8_t>(robot::OLED_I2C_ADDRESS << 1));
    m_display.begin();
    m_display.setContrast(180);
    m_ready = true;
    scheduleNextBlink();
    drawNow();
  }

  bool ready() const { return m_ready; }

  void setExpression(Expression expression) {
    m_useParamMode = false;
    m_expression = expression;
    if (m_expression == Expression::Blink) {
      m_eyeClosed = true;
    }
    if (m_expression != Expression::Blink) {
      m_eyeClosed = false;
      scheduleNextBlink();
    }
    drawNow();
  }

  void setParam(const ExpressionParams &params) {
    m_useParamMode = true;
    m_param = params;
    m_eyeClosed = false;
    scheduleNextBlink();
    drawNow();
  }

  void update() {
    if (!m_ready) {
      return;
    }
    const uint32_t now = millis();
    bool redraw = false;
    const bool autoBlinkAllowed =
        m_useParamMode ? m_param.autoBlink
                       : (m_expression != Expression::Blink && m_expression != Expression::Sleepy &&
                          m_expression != Expression::WinkLeft && m_expression != Expression::WinkRight);
    if (autoBlinkAllowed && !m_eyeClosed && static_cast<int32_t>(now - m_nextBlinkAtMs) >= 0) {
      m_eyeClosed = true;
      m_blinkEndAtMs = now + 140;
      redraw = true;
    }
    if (m_eyeClosed && autoBlinkAllowed && static_cast<int32_t>(now - m_blinkEndAtMs) >= 0) {
      m_eyeClosed = false;
      scheduleNextBlink();
      redraw = true;
    }
    if (redraw) {
      drawNow();
    }
  }

private:
  U8G2_SSD1306_128X64_NONAME_F_HW_I2C m_display;
  bool m_ready = false;
  bool m_useParamMode = false;
  bool m_eyeClosed = false;
  Expression m_expression = Expression::Neutral;
  ExpressionParams m_param{};
  uint32_t m_nextBlinkAtMs = 0;
  uint32_t m_blinkEndAtMs = 0;

  void scheduleNextBlink() {
    m_nextBlinkAtMs = millis() + static_cast<uint32_t>(random(2400, 5200));
  }

  static void normalizeEye(int cx, int cy, int w, int h, int *xOut, int *yOut) {
    *xOut = cx - w / 2;
    *yOut = cy - h / 2;
  }

  void drawOpenEye(int cx, int cy, int w, int h, int pupilDx, int pupilDy, int pupilR) {
    int x = 0;
    int y = 0;
    normalizeEye(cx, cy, w, h, &x, &y);
    m_display.drawRBox(x, y, w, h, 6);
    m_display.setDrawColor(0);
    m_display.drawDisc(cx + pupilDx, cy + pupilDy, pupilR, U8G2_DRAW_ALL);
    m_display.setDrawColor(1);
  }

  void drawClosedEye(int cx, int cy, int w) {
    const int x0 = cx - w / 2;
    const int x1 = cx + w / 2;
    m_display.drawLine(x0, cy, x1, cy);
    m_display.drawLine(x0, cy + 1, x1, cy + 1);
  }

  void drawHalfClosedEye(int cx, int cy, int w, int h, int pupilDx) {
    const int x = cx - w / 2;
    const int y = cy - h / 2;
    m_display.drawRBox(x, y + h / 3, w, h / 2, 5);
    m_display.setDrawColor(0);
    m_display.drawDisc(cx + pupilDx, cy + h / 5, 3, U8G2_DRAW_ALL);
    m_display.setDrawColor(1);
  }

  void drawBrowsAngry() {
    m_display.drawLine(18, 16, 48, 10);
    m_display.drawLine(80, 10, 110, 16);
    m_display.drawLine(18, 17, 48, 11);
    m_display.drawLine(80, 11, 110, 17);
  }

  void drawBrowsSad() {
    m_display.drawLine(18, 10, 48, 16);
    m_display.drawLine(80, 16, 110, 10);
    m_display.drawLine(18, 11, 48, 17);
    m_display.drawLine(80, 17, 110, 11);
  }

  void drawExpressionLabel(const char *label) {
    m_display.setFont(u8g2_font_5x8_tr);
    m_display.drawStr(2, 63, label);
  }

  static int valueOrFallback(int preferred, int fallback, int minV, int maxV) {
    if (preferred < minV || preferred > maxV) {
      return fallback;
    }
    return preferred;
  }

  void drawParamEye(int cx, int cy, int eyeW, int eyeH, int openPct, int gazeX, int gazeY, int pupilSize) {
    openPct = constrain(openPct, 0, 100);
    gazeX = constrain(gazeX, -10, 10);
    gazeY = constrain(gazeY, -8, 8);
    pupilSize = constrain(pupilSize, 1, 8);

    if (openPct < 8) {
      drawClosedEye(cx, cy, eyeW);
      return;
    }

    const int dynamicH = map(openPct, 0, 100, 2, eyeH);
    const int x = cx - eyeW / 2;
    const int y = cy - dynamicH / 2;
    m_display.drawRBox(x, y, eyeW, dynamicH, 6);

    const int safePupil = constrain(pupilSize, 1, max(1, dynamicH / 2 - 1));
    const int maxDx = max(0, eyeW / 2 - safePupil - 3);
    const int maxDy = max(0, dynamicH / 2 - safePupil - 2);
    const int pupilDx = constrain(gazeX, -maxDx, maxDx);
    const int pupilDy = constrain(gazeY, -maxDy, maxDy);

    m_display.setDrawColor(0);
    m_display.drawDisc(cx + pupilDx, cy + pupilDy, safePupil, U8G2_DRAW_ALL);
    m_display.setDrawColor(1);
  }

  void drawParamBrows(int browTilt, int browLift) {
    browTilt = constrain(browTilt, -35, 35);
    browLift = constrain(browLift, -12, 12);

    const int baseY = 12 - browLift;
    const int dy = map(abs(browTilt), 0, 35, 0, 9);
    const bool inwardDown = browTilt > 0;

    const int leftOuterY = inwardDown ? baseY - dy : baseY + dy;
    const int leftInnerY = inwardDown ? baseY + dy : baseY - dy;
    const int rightInnerY = inwardDown ? baseY + dy : baseY - dy;
    const int rightOuterY = inwardDown ? baseY - dy : baseY + dy;

    m_display.drawLine(18, leftOuterY, 48, leftInnerY);
    m_display.drawLine(18, leftOuterY + 1, 48, leftInnerY + 1);
    m_display.drawLine(80, rightInnerY, 110, rightOuterY);
    m_display.drawLine(80, rightInnerY + 1, 110, rightOuterY + 1);
  }

  void drawParametric() {
    const int leftX = 38;
    const int rightX = 90;
    const int eyeY = 30;
    const int eyeW = 34;
    const int eyeH = 24;
    const int baseOpen = constrain(static_cast<int>(m_param.openness), 0, 100);
    const int leftOpen = valueOrFallback(static_cast<int>(m_param.leftOpen), baseOpen, 0, 100);
    const int rightOpen = valueOrFallback(static_cast<int>(m_param.rightOpen), baseOpen, 0, 100);

    if (m_eyeClosed) {
      drawClosedEye(leftX, eyeY, eyeW);
      drawClosedEye(rightX, eyeY, eyeW);
    } else {
      drawParamEye(leftX, eyeY, eyeW, eyeH, leftOpen, m_param.gazeX, m_param.gazeY, m_param.pupilSize);
      drawParamEye(rightX, eyeY, eyeW, eyeH, rightOpen, m_param.gazeX, m_param.gazeY, m_param.pupilSize);
      drawParamBrows(m_param.browTilt, m_param.browLift);
    }
    drawExpressionLabel("PARAM");
  }

  void drawNow() {
    if (!m_ready) {
      return;
    }
    m_display.clearBuffer();

    if (m_useParamMode) {
      drawParametric();
      m_display.sendBuffer();
      return;
    }

    const int leftX = 38;
    const int rightX = 90;
    const int eyeY = 30;
    const int eyeW = 34;
    const int eyeH = 24;

    if (m_eyeClosed || m_expression == Expression::Blink) {
      drawClosedEye(leftX, eyeY, eyeW);
      drawClosedEye(rightX, eyeY, eyeW);
      drawExpressionLabel("BLINK");
      m_display.sendBuffer();
      return;
    }

    switch (m_expression) {
    case Expression::Happy:
      drawOpenEye(leftX, eyeY, eyeW, eyeH, 0, 1, 4);
      drawOpenEye(rightX, eyeY, eyeW, eyeH, 0, 1, 4);
      m_display.drawLine(18, 49, 34, 54);
      m_display.drawLine(110, 49, 94, 54);
      drawExpressionLabel("HAPPY");
      break;
    case Expression::Sad:
      drawOpenEye(leftX, eyeY, eyeW, eyeH, 0, 3, 4);
      drawOpenEye(rightX, eyeY, eyeW, eyeH, 0, 3, 4);
      drawBrowsSad();
      drawExpressionLabel("SAD");
      break;
    case Expression::Angry:
      drawOpenEye(leftX, eyeY, eyeW, eyeH, 0, 0, 5);
      drawOpenEye(rightX, eyeY, eyeW, eyeH, 0, 0, 5);
      drawBrowsAngry();
      drawExpressionLabel("ANGRY");
      break;
    case Expression::Sleepy:
      drawHalfClosedEye(leftX, eyeY, eyeW, eyeH, 0);
      drawHalfClosedEye(rightX, eyeY, eyeW, eyeH, 0);
      drawExpressionLabel("SLEEPY");
      break;
    case Expression::Surprised:
      drawOpenEye(leftX, eyeY, eyeW - 4, eyeH + 6, 0, 0, 2);
      drawOpenEye(rightX, eyeY, eyeW - 4, eyeH + 6, 0, 0, 2);
      drawExpressionLabel("SURPRISED");
      break;
    case Expression::LookLeft:
      drawOpenEye(leftX, eyeY, eyeW, eyeH, -6, 0, 4);
      drawOpenEye(rightX, eyeY, eyeW, eyeH, -6, 0, 4);
      drawExpressionLabel("LOOK_LEFT");
      break;
    case Expression::LookRight:
      drawOpenEye(leftX, eyeY, eyeW, eyeH, 6, 0, 4);
      drawOpenEye(rightX, eyeY, eyeW, eyeH, 6, 0, 4);
      drawExpressionLabel("LOOK_RIGHT");
      break;
    case Expression::WinkLeft:
      drawClosedEye(leftX, eyeY, eyeW);
      drawOpenEye(rightX, eyeY, eyeW, eyeH, 0, 0, 4);
      drawExpressionLabel("WINK_LEFT");
      break;
    case Expression::WinkRight:
      drawOpenEye(leftX, eyeY, eyeW, eyeH, 0, 0, 4);
      drawClosedEye(rightX, eyeY, eyeW);
      drawExpressionLabel("WINK_RIGHT");
      break;
    case Expression::Neutral:
    default:
      drawOpenEye(leftX, eyeY, eyeW, eyeH, 0, 0, 4);
      drawOpenEye(rightX, eyeY, eyeW, eyeH, 0, 0, 4);
      drawExpressionLabel("NEUTRAL");
      break;
    }
    m_display.sendBuffer();
  }
};

struct Command {
  Motion motion = Motion::Stop;
  uint32_t durationMs = 0;
  uint8_t speed = robot::DEFAULT_SPEED;
  bool valid = false;
};

MotorDriver g_motor;
EyeDisplay g_eyes;
WebServer g_server(robot::HTTP_PORT);
uint8_t g_defaultSpeed = robot::DEFAULT_SPEED;
uint32_t g_motionStopAtMs = 0;
uint32_t g_lastCommandAtMs = 0;
Motion g_currentMotion = Motion::Stop;
Expression g_expression = Expression::Neutral;
bool g_expressionParamMode = false;
ExpressionParams g_expressionParams{};
Expression g_expressionRevertTo = Expression::Neutral;
bool g_expressionRevertToParamMode = false;
ExpressionParams g_expressionParamsRevertTo{};
uint32_t g_expressionRevertAtMs = 0;

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

bool parseHoldMs(const String &token, uint32_t *holdMsOut) {
  if (holdMsOut == nullptr || token.length() == 0) {
    return false;
  }
  const long value = token.toInt();
  if (value <= 0) {
    return false;
  }
  *holdMsOut =
      static_cast<uint32_t>(constrain(value, 1L, static_cast<long>(robot::MAX_EXPRESSION_HOLD_MS)));
  return true;
}

bool parseIntRange(const String &token, int minValue, int maxValue, int *out) {
  if (out == nullptr || token.length() == 0) {
    return false;
  }
  char *end = nullptr;
  const long value = strtol(token.c_str(), &end, 10);
  if (end == token.c_str() || *end != '\0') {
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

bool parseExpressionToken(const String &token, Expression *expressionOut) {
  if (expressionOut == nullptr || token.length() == 0) {
    return false;
  }
  String upper = token;
  upper.trim();
  upper.toUpperCase();
  upper.replace("-", "_");
  if (upper == "NEUTRAL" || upper == "NORMAL") {
    *expressionOut = Expression::Neutral;
    return true;
  }
  if (upper == "HAPPY") {
    *expressionOut = Expression::Happy;
    return true;
  }
  if (upper == "SAD") {
    *expressionOut = Expression::Sad;
    return true;
  }
  if (upper == "ANGRY") {
    *expressionOut = Expression::Angry;
    return true;
  }
  if (upper == "SLEEPY") {
    *expressionOut = Expression::Sleepy;
    return true;
  }
  if (upper == "SURPRISED") {
    *expressionOut = Expression::Surprised;
    return true;
  }
  if (upper == "LOOK_LEFT") {
    *expressionOut = Expression::LookLeft;
    return true;
  }
  if (upper == "LOOK_RIGHT") {
    *expressionOut = Expression::LookRight;
    return true;
  }
  if (upper == "WINK_LEFT") {
    *expressionOut = Expression::WinkLeft;
    return true;
  }
  if (upper == "WINK_RIGHT") {
    *expressionOut = Expression::WinkRight;
    return true;
  }
  if (upper == "BLINK") {
    *expressionOut = Expression::Blink;
    return true;
  }
  return false;
}

String expressionName(Expression expression) {
  switch (expression) {
  case Expression::Happy:
    return "HAPPY";
  case Expression::Sad:
    return "SAD";
  case Expression::Angry:
    return "ANGRY";
  case Expression::Sleepy:
    return "SLEEPY";
  case Expression::Surprised:
    return "SURPRISED";
  case Expression::LookLeft:
    return "LOOK_LEFT";
  case Expression::LookRight:
    return "LOOK_RIGHT";
  case Expression::WinkLeft:
    return "WINK_LEFT";
  case Expression::WinkRight:
    return "WINK_RIGHT";
  case Expression::Blink:
    return "BLINK";
  case Expression::Neutral:
  default:
    return "NEUTRAL";
  }
}

String currentExpressionName() { return g_expressionParamMode ? String("PARAM") : expressionName(g_expression); }

bool parseExpressionParamsFromArgs(ExpressionParams *paramsOut, bool *changedOut) {
  if (paramsOut == nullptr || changedOut == nullptr) {
    return false;
  }

  ExpressionParams params = g_expressionParamMode ? g_expressionParams : ExpressionParams{};
  bool changed = false;

  if (g_server.hasArg("openness")) {
    int value = 0;
    if (!parseIntRange(g_server.arg("openness"), 0, 100, &value)) {
      return false;
    }
    params.openness = static_cast<uint8_t>(value);
    changed = true;
  }
  if (g_server.hasArg("gaze_x")) {
    int value = 0;
    if (!parseIntRange(g_server.arg("gaze_x"), -10, 10, &value)) {
      return false;
    }
    params.gazeX = static_cast<int8_t>(value);
    changed = true;
  }
  if (g_server.hasArg("gaze_y")) {
    int value = 0;
    if (!parseIntRange(g_server.arg("gaze_y"), -8, 8, &value)) {
      return false;
    }
    params.gazeY = static_cast<int8_t>(value);
    changed = true;
  }
  if (g_server.hasArg("brow_tilt")) {
    int value = 0;
    if (!parseIntRange(g_server.arg("brow_tilt"), -35, 35, &value)) {
      return false;
    }
    params.browTilt = static_cast<int8_t>(value);
    changed = true;
  }
  if (g_server.hasArg("brow_lift")) {
    int value = 0;
    if (!parseIntRange(g_server.arg("brow_lift"), -12, 12, &value)) {
      return false;
    }
    params.browLift = static_cast<int8_t>(value);
    changed = true;
  }
  if (g_server.hasArg("pupil")) {
    int value = 0;
    if (!parseIntRange(g_server.arg("pupil"), 1, 8, &value)) {
      return false;
    }
    params.pupilSize = static_cast<uint8_t>(value);
    changed = true;
  }
  if (g_server.hasArg("left_open")) {
    int value = 0;
    if (!parseIntRange(g_server.arg("left_open"), 0, 100, &value)) {
      return false;
    }
    params.leftOpen = static_cast<int8_t>(value);
    changed = true;
  }
  if (g_server.hasArg("right_open")) {
    int value = 0;
    if (!parseIntRange(g_server.arg("right_open"), 0, 100, &value)) {
      return false;
    }
    params.rightOpen = static_cast<int8_t>(value);
    changed = true;
  }
  if (g_server.hasArg("auto_blink")) {
    bool value = true;
    if (!parseBool(g_server.arg("auto_blink"), &value)) {
      return false;
    }
    params.autoBlink = value;
    changed = true;
  }

  *paramsOut = params;
  *changedOut = changed;
  return true;
}

bool inferExpressionFromText(const String &input, Expression *expressionOut) {
  if (expressionOut == nullptr) {
    return false;
  }
  String text = input;
  text.trim();
  text.toLowerCase();
  if (text.length() == 0) {
    return false;
  }

  if (text.indexOf("开心") >= 0 || text.indexOf("高兴") >= 0 || text.indexOf("happy") >= 0 ||
      text.indexOf("smile") >= 0) {
    *expressionOut = Expression::Happy;
    return true;
  }
  if (text.indexOf("难过") >= 0 || text.indexOf("伤心") >= 0 || text.indexOf("sad") >= 0) {
    *expressionOut = Expression::Sad;
    return true;
  }
  if (text.indexOf("生气") >= 0 || text.indexOf("愤怒") >= 0 || text.indexOf("angry") >= 0) {
    *expressionOut = Expression::Angry;
    return true;
  }
  if (text.indexOf("困") >= 0 || text.indexOf("累") >= 0 || text.indexOf("sleepy") >= 0 ||
      text.indexOf("tired") >= 0) {
    *expressionOut = Expression::Sleepy;
    return true;
  }
  if (text.indexOf("惊讶") >= 0 || text.indexOf("惊喜") >= 0 || text.indexOf("surprise") >= 0 ||
      text.indexOf("wow") >= 0) {
    *expressionOut = Expression::Surprised;
    return true;
  }
  if (text.indexOf("看左") >= 0 || text.indexOf("向左看") >= 0 || text.indexOf("look left") >= 0) {
    *expressionOut = Expression::LookLeft;
    return true;
  }
  if (text.indexOf("看右") >= 0 || text.indexOf("向右看") >= 0 || text.indexOf("look right") >= 0) {
    *expressionOut = Expression::LookRight;
    return true;
  }
  if (text.indexOf("左眨眼") >= 0 || text.indexOf("wink left") >= 0) {
    *expressionOut = Expression::WinkLeft;
    return true;
  }
  if (text.indexOf("右眨眼") >= 0 || text.indexOf("wink right") >= 0) {
    *expressionOut = Expression::WinkRight;
    return true;
  }
  if (text.indexOf("眨眼") >= 0 || text.indexOf("blink") >= 0) {
    *expressionOut = Expression::Blink;
    return true;
  }
  if (text.indexOf("平静") >= 0 || text.indexOf("normal") >= 0 || text.indexOf("neutral") >= 0) {
    *expressionOut = Expression::Neutral;
    return true;
  }
  return false;
}

void applyPresetExpression(Expression expression) {
  g_expressionParamMode = false;
  g_expression = expression;
  g_eyes.setExpression(expression);
}

void applyParamExpression(const ExpressionParams &params) {
  g_expressionParamMode = true;
  g_expressionParams = params;
  g_eyes.setParam(params);
}

void setExpression(Expression expression, uint32_t holdMs) {
  const bool previousParamMode = g_expressionParamMode;
  const Expression previousExpression = g_expression;
  const ExpressionParams previousParams = g_expressionParams;

  applyPresetExpression(expression);

  if (holdMs > 0) {
    g_expressionRevertToParamMode = previousParamMode;
    g_expressionRevertTo = previousExpression;
    g_expressionParamsRevertTo = previousParams;
    g_expressionRevertAtMs = millis() + holdMs;
  } else {
    g_expressionRevertAtMs = 0;
  }
}

void setExpressionParams(const ExpressionParams &params, uint32_t holdMs) {
  const bool previousParamMode = g_expressionParamMode;
  const Expression previousExpression = g_expression;
  const ExpressionParams previousParams = g_expressionParams;

  applyParamExpression(params);

  if (holdMs > 0) {
    g_expressionRevertToParamMode = previousParamMode;
    g_expressionRevertTo = previousExpression;
    g_expressionParamsRevertTo = previousParams;
    g_expressionRevertAtMs = millis() + holdMs;
  } else {
    g_expressionRevertAtMs = 0;
  }
}

void revertExpression() {
  if (g_expressionRevertToParamMode) {
    applyParamExpression(g_expressionParamsRevertTo);
  } else {
    applyPresetExpression(g_expressionRevertTo);
  }
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
  json += "\",\"expression_mode\":\"";
  json += g_expressionParamMode ? String("PARAM") : String("PRESET");
  json += "\",\"expression\":\"";
  json += currentExpressionName();
  json += "\"";
  if (g_expressionParamMode) {
    json += ",\"expression_param\":{";
    json += "\"openness\":";
    json += String(g_expressionParams.openness);
    json += ",\"gaze_x\":";
    json += String(g_expressionParams.gazeX);
    json += ",\"gaze_y\":";
    json += String(g_expressionParams.gazeY);
    json += ",\"brow_tilt\":";
    json += String(g_expressionParams.browTilt);
    json += ",\"brow_lift\":";
    json += String(g_expressionParams.browLift);
    json += ",\"pupil\":";
    json += String(g_expressionParams.pupilSize);
    json += ",\"left_open\":";
    json += String(g_expressionParams.leftOpen);
    json += ",\"right_open\":";
    json += String(g_expressionParams.rightOpen);
    json += ",\"auto_blink\":";
    json += g_expressionParams.autoBlink ? "true" : "false";
    json += "}";
  }
  json += ",\"default_speed\":";
  json += String(g_defaultSpeed);
  json += ",\"uptime_ms\":";
  json += String(millis());
  json += "}";
  return json;
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

  bool hasExpression = false;
  Expression expression = Expression::Neutral;
  if (g_server.hasArg("expression")) {
    if (!parseExpressionToken(g_server.arg("expression"), &expression)) {
      sendJson(400, "{\"ok\":false,\"error\":\"bad expression\"}");
      return;
    }
    hasExpression = true;
  }

  ExpressionParams expressionParams{};
  bool hasParamExpression = false;
  if (!parseExpressionParamsFromArgs(&expressionParams, &hasParamExpression)) {
    sendJson(400, "{\"ok\":false,\"error\":\"bad expression param\"}");
    return;
  }

  uint32_t holdMs = 0;
  if (g_server.hasArg("expression_hold_ms")) {
    parseHoldMs(g_server.arg("expression_hold_ms"), &holdMs);
  }

  applyCommand(cmd);
  if (hasParamExpression) {
    if (holdMs == 0 && cmd.durationMs > 0) {
      holdMs = cmd.durationMs;
    }
    setExpressionParams(expressionParams, holdMs);
  } else if (hasExpression) {
    if (holdMs == 0 && cmd.durationMs > 0) {
      holdMs = cmd.durationMs;
    }
    setExpression(expression, holdMs);
  }

  String json = "{\"ok\":true,\"motion\":\"";
  json += motionName(cmd.motion);
  json += "\",\"duration_ms\":";
  json += String(cmd.durationMs);
  json += ",\"speed\":";
  json += String(cmd.speed);
  json += ",\"expression\":\"";
  json += currentExpressionName();
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

  Expression expression = Expression::Neutral;
  bool hasExpression = false;
  ExpressionParams expressionParams{};
  bool hasParamExpression = false;
  if (!parseExpressionParamsFromArgs(&expressionParams, &hasParamExpression)) {
    sendJson(400, "{\"ok\":false,\"error\":\"bad expression param\"}");
    return;
  }

  if (g_server.hasArg("expression")) {
    if (!parseExpressionToken(g_server.arg("expression"), &expression)) {
      sendJson(400, "{\"ok\":false,\"error\":\"bad expression\"}");
      return;
    }
    hasExpression = true;
  } else if (hasParamExpression) {
    hasExpression = true;
  } else if (inferExpressionFromText(text, &expression)) {
    hasExpression = true;
  }

  if (!hasMotion && !hasExpression) {
    sendJson(400, "{\"ok\":false,\"error\":\"text parse failed\"}");
    return;
  }

  if (hasMotion) {
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
  }

  if (hasExpression) {
    uint32_t holdMs = 0;
    if (g_server.hasArg("expression_hold_ms")) {
      parseHoldMs(g_server.arg("expression_hold_ms"), &holdMs);
    } else if (hasMotion && cmd.durationMs > 0) {
      holdMs = cmd.durationMs;
    } else if (expression == Expression::Blink) {
      holdMs = 200;
    }
    if (hasParamExpression) {
      setExpressionParams(expressionParams, holdMs);
    } else {
      setExpression(expression, holdMs);
    }
  }

  String json = "{\"ok\":true,\"motion\":\"";
  json += hasMotion ? motionName(cmd.motion) : String("NONE");
  json += "\",\"duration_ms\":";
  json += hasMotion ? String(cmd.durationMs) : String(0);
  json += ",\"speed\":";
  json += hasMotion ? String(cmd.speed) : String(g_defaultSpeed);
  json += ",\"expression\":\"";
  json += currentExpressionName();
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

  Expression expression = Expression::Neutral;
  if (!parseExpressionToken(g_server.arg("name"), &expression)) {
    sendJson(400, "{\"ok\":false,\"error\":\"bad expression\"}");
    return;
  }

  uint32_t holdMs = 0;
  if (g_server.hasArg("hold_ms")) {
    parseHoldMs(g_server.arg("hold_ms"), &holdMs);
  } else if (expression == Expression::Blink) {
    holdMs = 200;
  }
  setExpression(expression, holdMs);

  String json = "{\"ok\":true,\"expression\":\"";
  json += currentExpressionName();
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

  ExpressionParams params{};
  bool changed = false;
  if (!parseExpressionParamsFromArgs(&params, &changed)) {
    sendJson(400, "{\"ok\":false,\"error\":\"bad expression param\"}");
    return;
  }
  if (!changed) {
    sendJson(400, "{\"ok\":false,\"error\":\"missing expression param\"}");
    return;
  }

  uint32_t holdMs = 0;
  if (g_server.hasArg("hold_ms")) {
    parseHoldMs(g_server.arg("hold_ms"), &holdMs);
  }
  setExpressionParams(params, holdMs);

  String json = "{\"ok\":true,\"expression\":\"PARAM\",\"hold_ms\":";
  json += String(holdMs);
  json += ",\"expression_param\":{";
  json += "\"openness\":";
  json += String(g_expressionParams.openness);
  json += ",\"gaze_x\":";
  json += String(g_expressionParams.gazeX);
  json += ",\"gaze_y\":";
  json += String(g_expressionParams.gazeY);
  json += ",\"brow_tilt\":";
  json += String(g_expressionParams.browTilt);
  json += ",\"brow_lift\":";
  json += String(g_expressionParams.browLift);
  json += ",\"pupil\":";
  json += String(g_expressionParams.pupilSize);
  json += ",\"left_open\":";
  json += String(g_expressionParams.leftOpen);
  json += ",\"right_open\":";
  json += String(g_expressionParams.rightOpen);
  json += ",\"auto_blink\":";
  json += g_expressionParams.autoBlink ? "true" : "false";
  json += "}}";
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
    String token = raw.substring(idx + 1);
    Expression expression = Expression::Neutral;
    if (!parseExpressionToken(token, &expression)) {
      sendJson(400, "{\"ok\":false,\"error\":\"bad expression\"}");
      return;
    }
    setExpression(expression, 0);
    String json = "{\"ok\":true,\"expression\":\"";
    json += currentExpressionName();
    json += "\"}";
    sendJson(200, json);
    return;
  }

  Command cmd = parseRawCommand(raw);
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
  json += ",\"expression\":\"";
  json += currentExpressionName();
  json += "\"}";
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

void runExpressionTasks() {
  const uint32_t now = millis();
  if (g_expressionRevertAtMs != 0 && static_cast<int32_t>(now - g_expressionRevertAtMs) >= 0) {
    g_expressionRevertAtMs = 0;
    revertExpression();
  }
  g_eyes.update();
}

} // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  randomSeed(static_cast<uint32_t>(micros()));

  g_motor.begin();
  g_eyes.begin();
  setExpression(Expression::Neutral, 0);

  const bool wifiOk = connectWifi();
  startHttpServer();

  Serial.println();
  Serial.println("MCP Robot HTTP Executor Ready");
  Serial.printf("WiFi: %s\n", wifiOk ? "connected" : "failed");
  if (wifiOk) {
    Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
  }
  Serial.printf("OLED: %s\n", g_eyes.ready() ? "ready" : "disabled_or_not_found");
  Serial.println("HTTP API:");
  Serial.println("  GET  /health");
  Serial.println("  ANY  /ping");
  Serial.println("  ANY  /api/state");
  Serial.println("  ANY  /api/move?direction=FORWARD&duration_ms=800&speed=180&expression=HAPPY");
  Serial.println("  ANY  /api/text?text=向左转并且开心一点");
  Serial.println("  ANY  /api/expression?name=ANGRY&hold_ms=2000");
  Serial.println("  ANY  /api/expression/param?openness=70&gaze_x=-5&brow_tilt=18&pupil=3");
  Serial.println("  ANY  /api/stop");
  Serial.println("  ANY  /api/speed?speed=200");
  Serial.println("  ANY  /api/raw?command=FORWARD%20800%20180");
}

void loop() {
  g_server.handleClient();
  runSafetyStop();
  runExpressionTasks();
  delay(2);
}
