# AICar AI Guide

Use this repository with `ai-context` as the main AI control context for the robot.

- Load [ai-context/robot-identity.md](./ai-context/robot-identity.md) when the task benefits from robot embodiment or behavior framing.
- Load [ai-context/.agents/skills/robot-buzzer-patterns/SKILL.md](./ai-context/.agents/skills/robot-buzzer-patterns/SKILL.md) when shaping buzzer output.
- Load [ai-context/.agents/skills/robot-buzzer-patterns/references/patterns.md](./ai-context/.agents/skills/robot-buzzer-patterns/references/patterns.md) for confirmation, error, thinking, or curious/questioning sounds.
- Treat `mcp-server-http-go` as the direct single-robot HTTP controller.
- Treat `mcp-server-mqtt-go` as the MQTT controller for registered robots.
- Prefer short robot-like chirps over long melodies unless the user explicitly asks for something musical.
