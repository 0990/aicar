# AI 控制桌面机器人（ESP32-C3 + DRV8833 + RoboEyes + Go MCP + MQTT）

当前支持两种 MCP 接入方式：

1. MCP Client（Codex / Claude Desktop）接收自然语言。
2. `mcp-server-mqtt-go` 通过 MQTT Broker 与小车通信。
3. `mcp-server-http-go` 直接调用小车固件 HTTP 接口。

说明：两套 MCP server 复用同一组工具名，只是底层通道不同。

## 1. 项目结构

```text
AICar/
├─ platformio.ini
├─ include/robot_config.h
├─ src/main.cpp
├─ docs/http_api.md
├─ docs/mqtt_protocol.md
├─ mcp-server-http-go/
│  ├─ go.mod
│  ├─ go.sum
│  ├─ main.go
│  └─ README.md
└─ mcp-server-mqtt-go/
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
cd mcp-server-mqtt-go
go mod tidy
go run .
```

默认会启动 `streamable-http` 服务，例如：`http://<公网域名或IP>:8080/mcp`。

## 7. Go MCP Server（HTTP 直连）

环境变量：

- `ROBOT_HTTP_BASE_URL`（例如 `http://192.168.1.88`）
- `ROBOT_HTTP_TOKEN`（可选）
- `MCP_TRANSPORT`（默认 `http`，可选 `stdio`）
- `MCP_HTTP_ADDR`（默认 `:8082`）
- `MCP_HTTP_ENDPOINT`（默认 `/mcp`）
- `MCP_HTTP_BEARER_TOKEN`（可选）

启动：

```bash
cd mcp-server-http-go
go mod tidy
go run .
```

这个版本直接请求小车固件 HTTP API，不经过 MQTT。

## 8. MCP 工具

- `robot_health`
- `robot_state`
- `robot_ping`
- `robot_set_wheels`
- `robot_expression`
- `robot_expression_param`

MQTT 版额外提供 `robot_list` 和 `robot_set_active`，HTTP 版没有多机器人管理。

## 9. 典型使用流程

1. 配置目标小车地址并启动 HTTP 版 MCP server。

2. 下发动作与表情：
`robot_set_wheels(left_direction="FORWARD", left_speed=100, right_direction="FORWARD", right_speed=100)`

3. 复杂表情控制：
`robot_expression_param(mood="ANGRY", position="W", curiosity=1, hflicker_amp=2, hold_ms=1800)`

## 10. HTTP 调试接口（可选）

局域网调试时仍可直接调用固件 HTTP 接口，见 `docs/http_api.md`。
