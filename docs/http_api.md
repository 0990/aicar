# Robot HTTP API (ESP32 + OLED Expressions)

Base URL: `http://<robot_ip>:<port>`  
Default port: `80` (`HTTP_PORT` in `include/robot_config.h`).

Optional auth:

- Header: `X-Robot-Token: <token>`
- Or query: `?token=<token>`

All responses are JSON.

## Health and State

- `GET /health`
- `GET /api/state`

State response example:

```json
{
  "ok": true,
  "ip": "192.168.1.88",
  "motion": "FORWARD",
  "expression_mode": "PARAM",
  "expression": "PARAM",
  "expression_param": {
    "openness": 74,
    "gaze_x": -4,
    "gaze_y": 1,
    "brow_tilt": 18,
    "brow_lift": 3,
    "pupil": 3,
    "left_open": 74,
    "right_open": 74,
    "auto_blink": true
  },
  "default_speed": 180,
  "uptime_ms": 123456
}
```

## Ping

- `POST /ping`

## Movement

- `POST /api/move`
- Query params:
  - `direction`: `FORWARD|BACKWARD|LEFT|RIGHT|STOP`
  - `duration_ms` (optional): `1..10000`
  - `speed` (optional): `0..255`
  - `expression` (optional): preset expression name
  - `expression_hold_ms` (optional): hold/revert duration
  - Parametric expression options (optional):
    - `openness` (`0..100`)
    - `gaze_x` (`-10..10`)
    - `gaze_y` (`-8..8`)
    - `brow_tilt` (`-35..35`)
    - `brow_lift` (`-12..12`)
    - `pupil` (`1..8`)
    - `left_open` (`0..100`)
    - `right_open` (`0..100`)
    - `auto_blink` (`0|1|true|false`)
- Alternative:
  - `command=FORWARD 800 180`

Example:

```bash
curl -X POST "http://192.168.1.88/api/move?direction=FORWARD&duration_ms=800&speed=180&openness=86&gaze_x=-3&brow_tilt=16&pupil=3"
```

## Text Intent

- `POST /api/text`
- Query params:
  - `text` (required)
  - `duration_ms` (optional)
  - `speed` (optional)
  - `expression` (optional)
  - `expression_hold_ms` (optional)
  - Any parametric expression option listed above

Example:

```bash
curl -X POST --get "http://192.168.1.88/api/text" --data-urlencode "text=向左转并且开心一点" --data "openness=85" --data "gaze_x=-3"
```

## Preset Expression

- `POST /api/expression`
- Query params:
  - `name`: `NEUTRAL|HAPPY|SAD|ANGRY|SLEEPY|SURPRISED|LOOK_LEFT|LOOK_RIGHT|WINK_LEFT|WINK_RIGHT|BLINK`
  - `hold_ms` (optional)

## Parametric Expression (AI-Generated)

- `POST /api/expression/param`
- Query params:
  - At least one param field is required:
    - `openness`, `gaze_x`, `gaze_y`, `brow_tilt`, `brow_lift`, `pupil`, `left_open`, `right_open`, `auto_blink`
  - `hold_ms` (optional)

Example:

```bash
curl -X POST "http://192.168.1.88/api/expression/param?openness=72&gaze_x=5&gaze_y=-1&brow_tilt=-12&brow_lift=-2&pupil=4&auto_blink=1"
```

## Stop / Speed / Raw

- `POST /api/stop`
- `POST /api/speed?speed=200`
- `POST /api/raw?command=FORWARD%20800%20180`
- `POST /api/raw?command=EXPR%20HAPPY`

