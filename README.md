# AI 控制桌面机器人（ESP32-C3 + DRV8833 + Go MCP + HTTP）

当前通信架构：

1. MCP Client（Codex/Claude Desktop 等）接收文字指令
2. 调用本仓库 Go MCP Server
3. Go MCP Server 通过 HTTP 请求 ESP32
4. ESP32 执行动作并返回 JSON 状态

已移除 INMP441 麦克风采集链路。

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

## 2. ESP32 固件（HTTP 执行器）

### 2.1 硬件

- 主控：ESP32-C3
- 驱动：DRV8833
- 电机：双 N20

默认 GPIO（可在 `include/robot_config.h` 修改）：

- `LEFT_IN1_PIN = 2`
- `LEFT_IN2_PIN = 3`
- `RIGHT_IN1_PIN = 4`
- `RIGHT_IN2_PIN = 5`

### 2.2 WiFi 配置

在 [robot_config.h](E:/CodeRepo/AICar/include/robot_config.h) 设置：

- `WIFI_SSID`
- `WIFI_PASSWORD`
- `HTTP_PORT`（默认 80）
- `HTTP_TOKEN`（可选，作为共享密钥）

### 2.3 编译/烧录

```bash
pio run
pio run -t upload
pio device monitor -b 115200
```

串口日志会打印设备 IP 和 HTTP 路由。

## 3. 先用 curl 验证 HTTP

```bash
curl "http://<robot_ip>/health"
curl -X POST "http://<robot_ip>/ping"
curl -X POST "http://<robot_ip>/api/move?direction=FORWARD&duration_ms=800&speed=180"
curl -X POST "http://<robot_ip>/api/stop"
```

完整接口见 [http_api.md](E:/CodeRepo/AICar/docs/http_api.md)。

## 4. Go MCP Server

### 4.1 环境变量

- `ROBOT_BASE_URL`：例如 `http://192.168.1.88`
- `ROBOT_HTTP_TOKEN`：如果启用了 `HTTP_TOKEN`

### 4.2 启动

```bash
cd mcp-server-go
go mod tidy
go run .
```

## 5. MCP Tools（Go 服务提供）

- `robot_set_endpoint`：设置/切换机器人 URL 与 token
- `robot_health`：检查 `/health`
- `robot_ping`：检查 `/ping`
- `robot_move`：方向控制（FORWARD/BACKWARD/LEFT/RIGHT/STOP）
- `robot_text_control`：把自然语言直接交给 ESP32 的 `/api/text`
- `robot_ai_text_move`：在 MCP 侧先做文本方向解析再调用 `/api/move`
- `robot_speed`：设置默认速度
- `robot_send_raw`：发送原始命令字符串
- `robot_stop`：急停

## 6. 建议联调顺序

1. 先直接用 curl 调 ESP32 HTTP API。
2. 再启动 Go MCP Server，调用 `robot_health` / `robot_move`。
3. 最后让 AI 使用 `robot_text_control` 或 `robot_ai_text_move`。

