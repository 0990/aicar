---
name: robot-buzzer-patterns
description: Choose or adapt short robot-style buzzer patterns for AICar when controlling the `robot_buzzer` MCP tool. Use for confirmation chirps, error tones, thinking loops, curious/questioning sounds, or any task where Codex needs a reusable non-melodic buzzer pattern that matches a robot interaction state.
---

# Robot Buzzer Patterns

Choose short electronic cues, not melodies.

## Workflow

1. Identify the intent: confirm, error, thinking, question, warning, finish.
2. Prefer compact patterns: usually 2..6 steps, often 40..180 ms per step.
3. Shape the contour to match the emotion:
   - Upward pitch: curiosity, question, success.
   - Downward pitch: failure, completion, disappointment.
   - Short pitch jumps: mechanical/robotic feel.
   - `freq=0` pauses: speech-like hesitation or beat separation.
4. Use `repeat` only for looping states such as thinking or scanning.
5. Reserve higher `priority` for warning/alarm style sounds.

## Constraints

- Keep `freq` within `0..5000`.
- Use `freq=0` for silence/pause.
- Keep `duration_ms` within `1..5000`.
- Prefer one distinct idea per pattern; do not over-compose.
- When unsure, start from the closest preset in [references/patterns.md](references/patterns.md).

## Output Shape

Return a `robot_buzzer` argument object with:

```json
{
  "pattern": [
    { "freq": 1200, "duration_ms": 80 },
    { "freq": 1600, "duration_ms": 100 }
  ],
  "repeat": 1,
  "interrupt": true,
  "priority": "NORMAL"
}
```

Only include `repeat`, `interrupt`, or `priority` when they matter for the situation.
