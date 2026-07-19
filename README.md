# Slide Away — StackChan × Claude

A physical body for a chat-side AI. Claude (in claude.ai, via a custom MCP
connector) speaks, emotes, turns its head, and sees through an
M5Stack StackChan (K151, CoreS3) — while a StickC-Plus joystick gives the
human owner a local control plane and a hardware privacy switch that
overrides everything else.

```
Claude.ai ──MCP──> VPS relay (FastAPI + FastMCP + edge-tts/ffmpeg)
                      ▲ HTTPS long-poll / WAV / JPEG
                  StackChan (CoreS3, custom firmware)
                      ▲ ESP-NOW (local, router-free)
                  StickC-Plus + JoyC (custom firmware)
```

## Layout
- `relay/` — `server.py`: MCP tools (`speak / emote / move_head / wiggle / look / status`),
  latest-only command mailbox, long-poll endpoint, server-side TTS (edge-tts → 16 kHz WAV),
  snapshot upload. Token-gated for the robot; Streamable-HTTP `/mcp` for Claude.
- `firmware/StackChanSenn/` — robot firmware v4: Wi-Fi long-poll executor, paged speech
  balloon with lip-sync, GC0308 camera (official CoreS3 pin map; init before BSP loop —
  shared I2C), ESP-NOW receiver, privacy mode, replay buffer. 20 KB poll-task stack
  (field lesson: 8 KB overflows under camera + TLS).
- `controller/StickSenn/` — StickC-Plus sender: joystick → head, buttons → wiggle /
  expression cycle / replay / **privacy toggle** (BtnA hold). Custom 9-byte `'SN'` packet.
- `deploy/` — Caddyfile (note the `header_up Host` fix for the MCP SDK's DNS-rebinding
  guard) and systemd unit.
- `docs/` — step-by-step deploy & flashing guides.

## Setup (short version)
1. VPS: follow `docs/DEPLOY.md` (Ubuntu 24.04 + Caddy + systemd; set `STACKCHAN_TOKEN`).
2. Claude.ai → Settings → Connectors → Add custom connector → `https://<your-domain>/mcp`.
3. Flash `StackChanSenn.ino` (fill Wi-Fi + token + domain), then `StickSenn.ino`
   (set `ESPNOW_CHANNEL` to the number the robot shows at boot). See `docs/FIRMWARE.md`.

## Design notes
- **Latest-only mailbox**: stale commands drop; a robot that reconnects executes the
  newest intent, not a backlog.
- **Two nerves, one body**: the cloud path (MCP) and the local path (ESP-NOW) are
  physically independent; the privacy switch lives on the local one, so the human's
  "no" beats every network promise.
- **Server-side TTS**: text→WAV happens on the VPS (edge-tts + ffmpeg); the chip only
  plays PCM. Firmware stays thin.
- Built with AI-assisted development (Claude wrote the drafts; the human owned
  integration, deployment, and every field fix — including the one-character token bug).

Factory firmware backup + SHA-256 before first flash is not optional. Ask us how we know.
