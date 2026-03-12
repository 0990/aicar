# Robot MQTT Protocol

Topic 前缀默认是 `aicar`（可在固件和 MCP Server 同步修改）。

## 1. Topics

单台机器人 `robot_id=car-001` 的 topic：

- 注册：`aicar/robots/car-001/register`（robot -> broker，retain）
- 状态：`aicar/robots/car-001/status`（robot -> broker，retain + 心跳）
- 命令：`aicar/robots/car-001/cmd`（mcp -> robot）
- 应答：`aicar/robots/car-001/ack`（robot -> broker）

## 2. 自动注册

机器人启动并连上 MQTT 后会自动：

1. 发布 `register`
2. 发布当前 `status`
3. 订阅 `cmd`
4. 持续心跳 `status`

`register` 示例：

```json
{
  "ok": true,
  "robot_id": "car-001",
  "ip": "192.168.1.88",
  "mqtt_cmd_topic": "aicar/robots/car-001/cmd",
  "mqtt_ack_topic": "aicar/robots/car-001/ack",
  "features": ["move", "set_wheels", "expression", "expression_param", "state"]
}
```

## 3. 命令格式（mcp -> robot）

发布到 `.../cmd`：

```json
{
  "req_id": "d52e9c91b8a94c7d",
  "type": "SET_WHEELS",
  "args": {
    "left_direction": "FORWARD",
    "left_speed": 100,
    "right_direction": "FORWARD",
    "right_speed": 100,
    "expression": "HAPPY",
    "expression_hold_ms": 1000
  }
}
```

字段说明：

- `req_id`: 请求唯一 ID
- `type`: 命令类型（不区分大小写）
- `args`: 命令参数对象

支持的 `type`：

- `PING`
- `STATE`
- `HEALTH`
- `SET_WHEELS`
- `MOVE`
- `EXPRESSION`
- `EXPRESSION_PARAM`

## 4. 应答格式（robot -> mcp）

发布到 `.../ack`：

成功：

```json
{
  "robot_id": "car-001",
  "req_id": "d52e9c91b8a94c7d",
  "ok": true,
  "result": {
    "ok": true,
    "motion": "FORWARD",
    "duration_ms": 1000,
    "speed": 180,
    "wheels": {
      "left_direction": "FORWARD",
      "left_speed": 100,
      "right_direction": "FORWARD",
      "right_speed": 100
    },
    "expression": "HAPPY",
    "expression_last_action": "NONE"
  }
}
```

失败：

```json
{
  "robot_id": "car-001",
  "req_id": "d52e9c91b8a94c7d",
  "ok": false,
  "error": "bad direction"
}
```

## 5. `args` 参数说明

### 5.1 SET_WHEELS

- `left_direction`: `FORWARD|BACKWARD`
- `left_speed`: `0..100`
- `right_direction`: `FORWARD|BACKWARD`
- `right_speed`: `0..100`
- `duration_ms`: 可选 `1..10000`
- `expression`: 可选预设表情
- `expression_hold_ms`: 可选 `1..30000`
- 可带 RoboEyes 参数（见 `EXPRESSION_PARAM`）

### 5.2 MOVE

兼容旧接口：

- `direction`: `FORWARD|BACKWARD|LEFT|RIGHT`
- `duration_ms`: 可选 `1..10000`
- `speed`: 可选 `0..255`
- `expression`: 可选预设表情
- `expression_hold_ms`: 可选 `1..30000`
- 可带 RoboEyes 参数（见 `EXPRESSION_PARAM`）

### 5.3 EXPRESSION

- `name`: `NEUTRAL|HAPPY|SAD|ANGRY|SLEEPY|SURPRISED|LOOK_LEFT|LOOK_RIGHT|WINK_LEFT|WINK_RIGHT|BLINK|CONFUSED|LAUGH`
- `hold_ms`: 可选 `1..30000`

### 5.4 EXPRESSION_PARAM

至少一个参数：

- `mood`: `DEFAULT|TIRED|ANGRY|HAPPY`
- `position`: `DEFAULT|CENTER|C|N|NE|E|SE|S|SW|W|NW`
- `curiosity`: `0|1`
- `sweat`: `0|1`
- `cyclops`: `0|1`
- `auto_blink`: `0|1`
- `auto_blink_interval`: `1..30`
- `auto_blink_variation`: `0..30`
- `idle`: `0|1`
- `idle_interval`: `1..30`
- `idle_variation`: `0..30`
- `hflicker_amp`: `0..30`
- `vflicker_amp`: `0..30`
- `action`: `NONE|BLINK|WINK_LEFT|WINK_RIGHT|CONFUSED|LAUGH|OPEN|CLOSE`
- `hold_ms`: 可选 `1..30000`
