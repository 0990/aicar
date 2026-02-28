# AI 控制桌面机器人（ESP32-C3 + DRV8833 + RoboEyes + Go MCP + MQTT）

当前主架构：

1. MCP Client（Codex / Claude Desktop）接收自然语言。
2. Go MCP Server 运行在公网，连接 MQTT Broker。
3. 小车在内网启动后主动连接 MQTT 并自动注册。
4. MCP Server 通过 MQTT 下发命令，小车执行后回 ACK。

说明：固件仍保留 HTTP 接口用于局域网调试，但 MCP 主通道已改为 MQTT。

## 1. 项目结构

```text
AICar/
├─ platformio.ini
├─ include/robot_config.h
├─ src/main.cpp
├─ docs/http_api.md
├─ docs/mqtt_protocol.md
└─ mcp-server-go/
   ├─ go.mod
   ├─ go.sum
   └─ main.go
```

## 2. 硬件连接

### 2.1 底盘驱动（DRV8833）

- `LEFT_IN1_PIN = 2`
- `LEFT_IN2_PIN = 3`
- `RIGHT_IN1_PIN = 4`
- `RIGHT_IN2_PIN = 5`

### 2.2 OLED（0.96"，I2C，SSD1306）

- 屏幕接口：`VCC / GND / SCL / SDA`
- 默认配置：
- `OLED_SDA_PIN = 6`
- `OLED_SCL_PIN = 7`
- `OLED_I2C_ADDRESS = 0x3C`
- `OLED_WIDTH = 128`
- `OLED_HEIGHT = 64`

## 3. ESP32 配置与烧录

编辑 `include/robot_config.h`：

- WiFi：`WIFI_SSID`、`WIFI_PASSWORD`
- MQTT：
- `MQTT_ENABLED`
- `MQTT_BROKER`、`MQTT_PORT`
- `MQTT_USERNAME`、`MQTT_PASSWORD`
- `MQTT_TOPIC_PREFIX`（默认 `aicar`）
- `ROBOT_ID`（每台车唯一）

构建与烧录：

```bash
C:\Users\xujialong\.platformio\penv\Scripts\platformio.exe run
C:\Users\xujialong\.platformio\penv\Scripts\platformio.exe run -t upload
C:\Users\xujialong\.platformio\penv\Scripts\platformio.exe device monitor -b 115200
```

## 4. MQTT 自动注册与控制

小车启动后会自动：

- 连接 MQTT Broker
- 发布注册信息到 `.../register`（retain）
- 订阅 `.../cmd`
- 持续发布 `.../status`（心跳）
- 执行命令并发布 `.../ack`

完整协议见 `docs/mqtt_protocol.md`。

## 5. RoboEyes 控制参数

- `mood`: `DEFAULT/TIRED/ANGRY/HAPPY`
- `position`: `DEFAULT/N/NE/E/SE/S/SW/W/NW`
- `curiosity`: `0/1`
- `sweat`: `0/1`
- `cyclops`: `0/1`
- `auto_blink`: `0/1`
- `auto_blink_interval`: `1..30`
- `auto_blink_variation`: `0..30`
- `idle`: `0/1`
- `idle_interval`: `1..30`
- `idle_variation`: `0..30`
- `hflicker_amp`: `0..30`
- `vflicker_amp`: `0..30`
- `action`: `NONE/BLINK/WINK_LEFT/WINK_RIGHT/CONFUSED/LAUGH/OPEN/CLOSE`

## 6. Go MCP Server（MQTT）

环境变量：

- `ROBOT_MQTT_BROKER`（例如 `tcp://broker.example.com:1883`）
- `ROBOT_MQTT_USERNAME`（可选）
- `ROBOT_MQTT_PASSWORD`（可选）
- `ROBOT_MQTT_TOPIC_PREFIX`（默认 `aicar`）
- `MCP_TRANSPORT`（默认 `http`，可选 `stdio`）
- `MCP_HTTP_ADDR`（默认 `:8080`）
- `MCP_HTTP_ENDPOINT`（默认 `/mcp`）
- `MCP_HTTP_BEARER_TOKEN`（可选，开启后需 `Authorization: Bearer <token>`）

启动：

```bash
cd mcp-server-go
go mod tidy
go run .
```

默认会启动 `streamable-http` 服务，例如：`http://<公网域名或IP>:8080/mcp`。

## 7. MCP 工具

- `robot_list`（查看已注册机器人）
- `robot_set_active`（设置默认机器人）
- `robot_health`
- `robot_state`
- `robot_ping`
- `robot_move`
- `robot_expression`
- `robot_expression_param`
- `robot_text_control`
- `robot_ai_behavior`
- `robot_speed`
- `robot_send_raw`
- `robot_stop`

## 8. 典型使用流程

1. 启动小车，确认已注册：
`robot_list()`

2. 选中目标机器人：
`robot_set_active(robot_id="car-001")`

3. 下发动作与表情：
`robot_move(direction="FORWARD", duration_ms=1000, expression="HAPPY", expression_hold_ms=1000)`

4. 复杂表情控制：
`robot_expression_param(mood="ANGRY", position="W", curiosity=1, hflicker_amp=2, hold_ms=1800)`

## 9. HTTP 调试接口（可选）

局域网调试时仍可直接调用固件 HTTP 接口，见 `docs/http_api.md`。
