# AICar AI Guide

Use this repository with the local MCP servers and official Codex skill layout.

- Treat `mcp-server-http-go` as the direct single-robot HTTP controller.
- Treat `mcp-server-mqtt-go` as the MQTT controller for registered robots.
- When shaping buzzer output for `robot_buzzer`, use the skill in [/.agents/skills/robot-buzzer-patterns/SKILL.md](./.agents/skills/robot-buzzer-patterns/SKILL.md).
- Load [/.agents/skills/robot-buzzer-patterns/references/patterns.md](./.agents/skills/robot-buzzer-patterns/references/patterns.md) for confirmation, error, thinking, or curious/questioning sounds.
- Prefer short robot-like chirps over long melodies unless the user explicitly asks for something musical.

