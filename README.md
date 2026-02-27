# AI 控制桌面机器人（ESP32-C3 + DRV8833 + OLED + Go MCP + HTTP）

当前架构：

1. MCP Client（Codex/Claude Desktop）接收自然语言
2. 调用 Go MCP Server
3. Go MCP Server 通过 HTTP 控制 ESP32
4. ESP32 控制电机动作 + 0.96 寸 OLED 眼睛表情

现在支持两种表情控制模式：

- 预设模式：`HAPPY/SAD/ANGRY...`
- 参数化模式（AI 生成）：通过连续参数实时生成眼睛形态（开眼度、视线、眉毛、瞳孔等）

## 1. 项目结构

```text
AICar/
├─ platformio.ini
├─ include/robot_config.h
├─ src/main.cpp
├─ docs/http_api.md
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

### 2.2 OLED（0.96", I2C）

- 屏幕接口：`VCC / GND / SCL / SDA`
- 默认配置（可改）：  
  `OLED_SDA_PIN = 6`  
  `OLED_SCL_PIN = 7`  
  `OLED_I2C_ADDRESS = 0x3C`

请在 [robot_config.h](E:/CodeRepo/AICar/include/robot_config.h) 根据你的接线修改。

## 3. ESP32 配置与烧录

在 [robot_config.h](E:/CodeRepo/AICar/include/robot_config.h) 设置：

- `WIFI_SSID`
- `WIFI_PASSWORD`
- `HTTP_PORT`（默认 80）
- `HTTP_TOKEN`（可选）

构建与烧录：

```bash
pio run
pio run -t upload
pio device monitor -b 115200
```

## 4. HTTP 接口

完整文档见 [http_api.md](E:/CodeRepo/AICar/docs/http_api.md)。

常用示例：

```bash
curl "http://<robot_ip>/health"
curl -X POST "http://<robot_ip>/api/move?direction=FORWARD&duration_ms=800&speed=180&expression=HAPPY"
curl -X POST "http://<robot_ip>/api/expression/param?openness=74&gaze_x=-4&gaze_y=1&brow_tilt=16&brow_lift=2&pupil=3&auto_blink=1"
curl -X POST --get "http://<robot_ip>/api/text" --data-urlencode "text=向左转并且开心一点"
curl -X POST "http://<robot_ip>/api/expression?name=ANGRY&hold_ms=1500"
curl -X POST "http://<robot_ip>/api/stop"
```

## 5. Go MCP Server

环境变量：

- `ROBOT_BASE_URL`（例如 `http://192.168.1.88`）
- `ROBOT_HTTP_TOKEN`（若启用令牌）

启动：

```bash
cd mcp-server-go
go mod tidy
go run .
```

## 6. MCP 工具

- `robot_set_endpoint`
- `robot_health`
- `robot_state`
- `robot_ping`
- `robot_move`（支持 `expression` 和参数化表情参数）
- `robot_expression`（直接控制 OLED 表情）
- `robot_expression_param`（直接设置 AI 生成的参数化表情）
- `robot_text_control`（文本直传给 ESP32 解析）
- `robot_ai_behavior`（MCP 侧解析文本里的动作+表情）
- `robot_speed`
- `robot_send_raw`
- `robot_stop`

## 7. 典型 AI 控制示例

1. “向前走 1 秒，开心一点”  
`robot_move(direction=FORWARD, duration_ms=1000, expression=HAPPY, expression_hold_ms=1000)`

2. “做个生气表情 2 秒”  
`robot_expression(name=ANGRY, hold_ms=2000)`

3. “向左转并眨眼”  
`robot_ai_behavior(text="向左转并眨眼", duration_ms=700, expression_hold_ms=300)`

4. “生成更生动的眼神：半眯眼、向左看、眉毛上扬”  
`robot_expression_param(openness=55, gaze_x=-6, gaze_y=1, brow_tilt=-8, brow_lift=4, pupil=3, auto_blink=1, hold_ms=1800)`
