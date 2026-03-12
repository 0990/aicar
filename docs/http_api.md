# Robot HTTP API (ESP32 + RoboEyes)

Base URL: `http://<robot_ip>:<port>`

- 默认端口：`80`（见 `include/robot_config.h` 的 `HTTP_PORT`）
- 可选鉴权：请求头 `X-Robot-Token: <token>` 或 query `token=<token>`
- 所有响应均为 JSON

## 1. Health / State

- `GET /health`
- `ANY /api/state`

示例响应：

```json
{
  "ok": true,
  "ip": "192.168.1.88",
  "motion": "STOP",
  "default_speed": 180,
  "wheels": {
    "left_direction": "STOP",
    "left_speed": 0,
    "right_direction": "STOP",
    "right_speed": 0
  },
  "expression_engine": "ROBOEYES",
  "expression": "NEUTRAL",
  "expression_last_action": "NONE",
  "expression_style": {
    "mood": "DEFAULT",
    "position": "DEFAULT",
    "curiosity": false,
    "sweat": false,
    "cyclops": false,
    "auto_blink": true,
    "auto_blink_interval": 3,
    "auto_blink_variation": 2,
    "idle": false,
    "idle_interval": 2,
    "idle_variation": 2,
    "hflicker_amp": 0,
    "vflicker_amp": 0
  },
  "oled_ready": true,
  "uptime_ms": 123456
}
```

## 2. Ping

- `ANY /ping`

## 3. Wheel Control

- `ANY /api/wheels`

参数：

- `left_direction`: `FORWARD|BACKWARD`
- `left_speed`: `0..100`
- `right_direction`: `FORWARD|BACKWARD`
- `right_speed`: `0..100`
- `duration_ms`: 可选，`1..10000`
- `expression`: 可选，预设表情
- `expression_hold_ms`: 可选，临时表情保持时长
- 还可带 RoboEyes 参数，字段同 `api/expression/param`

示例：

```bash
curl -X POST "http://192.168.1.88/api/wheels?left_direction=FORWARD&left_speed=100&right_direction=FORWARD&right_speed=100&mood=HAPPY&action=BLINK"
```

## 4. Preset Expression

- `ANY /api/expression`

参数：

- `name`: `NEUTRAL|HAPPY|SAD|ANGRY|SLEEPY|SURPRISED|LOOK_LEFT|LOOK_RIGHT|WINK_LEFT|WINK_RIGHT|BLINK|CONFUSED|LAUGH`
- `hold_ms`: 可选，`1..30000`

示例：

```bash
curl -X POST "http://192.168.1.88/api/expression?name=CONFUSED&hold_ms=1500"
```

## 5. RoboEyes Parameter Expression

- `ANY /api/expression/param`

参数：

- 至少一个参数必填
- `mood`: `DEFAULT|TIRED|ANGRY|HAPPY`
- `position`: `DEFAULT|CENTER|C|N|NE|E|SE|S|SW|W|NW`
- `curiosity`: `0|1|true|false`
- `sweat`: `0|1|true|false`
- `cyclops`: `0|1|true|false`
- `auto_blink`: `0|1|true|false`
- `auto_blink_interval`: `1..30`
- `auto_blink_variation`: `0..30`
- `idle`: `0|1|true|false`
- `idle_interval`: `1..30`
- `idle_variation`: `0..30`
- `hflicker_amp`: `0..30`
- `vflicker_amp`: `0..30`
- `action`: `NONE|BLINK|WINK_LEFT|WINK_RIGHT|CONFUSED|LAUGH|OPEN|CLOSE`
- `hold_ms`: 可选，`1..30000`

示例：

```bash
curl -X POST "http://192.168.1.88/api/expression/param?mood=ANGRY&position=NE&curiosity=1&auto_blink=1&hflicker_amp=2&action=BLINK"
```
