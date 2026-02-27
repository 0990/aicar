# Robot HTTP API (ESP32)

ESP32 exposes HTTP endpoints on `http://<robot_ip>:<port>`.

- Default port: `80` (`HTTP_PORT` in `include/robot_config.h`)
- Optional auth token:
  - Header: `X-Robot-Token: <token>`
  - or query: `?token=<token>`

All responses are JSON.

## Health

- `GET /health`
- Response:

```json
{"ok":true,"ip":"192.168.1.88","port":80}
```

## Ping

- `POST /ping`
- Response:

```json
{"ok":true,"reply":"PONG"}
```

## Move

- `POST /api/move`
- Query params:
  - `direction`: `FORWARD|BACKWARD|LEFT|RIGHT|STOP`
  - `duration_ms` (optional): `1..10000`
  - `speed` (optional): `0..255`
- Alternative:
  - `command=FORWARD 800 180`

Example:

```bash
curl -X POST "http://192.168.1.88/api/move?direction=FORWARD&duration_ms=800&speed=180"
```

## Text Intent

- `POST /api/text`
- Query params:
  - `text` (required)
  - `duration_ms` (optional)
  - `speed` (optional)

Example:

```bash
curl -X POST --get "http://192.168.1.88/api/text" --data-urlencode "text=向左转一下"
```

## Stop

- `POST /api/stop`

## Set Default Speed

- `POST /api/speed?speed=200`

## Raw Command

- `POST /api/raw?command=FORWARD%20800%20180`

