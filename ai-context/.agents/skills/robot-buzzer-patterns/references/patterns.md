# Robot Buzzer Presets

Use these presets as starting points for `robot_buzzer`.

## Presets

### Confirm Received

Use for ACK, command accepted, wake response, or short success feedback.

```json
{
  "pattern": [
    { "freq": 1200, "duration_ms": 80 },
    { "freq": 1600, "duration_ms": 100 }
  ]
}
```

Effect: quick upward chirp, clean and affirmative.

### Error Prompt

Use for invalid command, failure, rejection, or blocked action.

```json
{
  "pattern": [
    { "freq": 600, "duration_ms": 180 },
    { "freq": 0, "duration_ms": 80 },
    { "freq": 450, "duration_ms": 220 }
  ]
}
```

Effect: downward, heavier finish, clearly negative.

### Thinking

Use for processing, searching, listening, or waiting states.

```json
{
  "pattern": [
    { "freq": 900, "duration_ms": 60 },
    { "freq": 1100, "duration_ms": 60 },
    { "freq": 1300, "duration_ms": 60 },
    { "freq": 1000, "duration_ms": 70 },
    { "freq": 1200, "duration_ms": 50 },
    { "freq": 0, "duration_ms": 80 }
  ],
  "repeat": 2
}
```

Effect: active, scanning, lightly animated.

### Curious Question

Use for "huh?", uncertainty, or Wall-E-like questioning tone.

```json
{
  "pattern": [
    { "freq": 700, "duration_ms": 120 },
    { "freq": 980, "duration_ms": 100 },
    { "freq": 1350, "duration_ms": 160 }
  ]
}
```

Effect: rising contour with a curious finish.

## Design Notes

- Upward contour suggests questioning, interest, or success.
- Downward contour suggests ending, sadness, or failure.
- Short pitch jumps feel mechanical and robotic.
- Insert silence with `freq=0` to create a spoken pause.
- Prefer concise patterns over long tunes.
