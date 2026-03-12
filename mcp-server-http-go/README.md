# AICar MCP Server (HTTP Direct)

这个工程直接通过小车固件的 HTTP 接口调用机器人，不依赖 MQTT。

## 环境变量

- `ROBOT_HTTP_BASE_URL`：小车地址，例如 `http://192.168.1.88`
- `ROBOT_HTTP_TOKEN`：可选，对应固件 `X-Robot-Token`
- `MCP_TRANSPORT`：默认 `http`，可选 `stdio`
- `MCP_HTTP_ADDR`：默认 `:8082`
- `MCP_HTTP_ENDPOINT`：默认 `/mcp`
- `MCP_HTTP_BEARER_TOKEN`：可选，保护 MCP HTTP 入口

## 启动

```bash
cd mcp-server-http-go
go mod tidy
go run .
```

## 工具

- `robot_health`
- `robot_state`
- `robot_ping`
- `robot_buzzer`
- `robot_set_wheels`
- `robot_expression`
- `robot_expression_param`
