"""
StackChan Relay — Claude.ai (MCP) <-> ESP32 bridge, single process.

Architecture:
  Claude.ai custom connector --> /mcp       (Streamable HTTP, FastMCP)
  ESP32 long-polls           --> /poll      (latest-only command queue)
  ESP32 reports back         --> /result
  ESP32 fetches speech audio --> /audio/{cmd_id}.wav  (edge-tts + ffmpeg)
  ESP32 uploads photos       --> /snapshot

Run:  uvicorn server:app --host 127.0.0.1 --port 8011
Env:  STACKCHAN_TOKEN   shared secret; ESP32 must send X-Stackchan-Token
      TTS_VOICE         e.g. "en-GB-RyanNeural" (see `edge-tts --list-voices`)
"""

import asyncio
import os
import time
import uuid
from contextlib import asynccontextmanager
from pathlib import Path

import edge_tts
from fastapi import FastAPI, Header, HTTPException, Request
from fastapi.responses import FileResponse, JSONResponse
from mcp.server.fastmcp import FastMCP, Image

# ----------------------------------------------------------------------------
# Config
# ----------------------------------------------------------------------------
TOKEN = os.environ.get("STACKCHAN_TOKEN", "")
TTS_VOICE = os.environ.get("TTS_VOICE", "en-GB-RyanNeural")
AUDIO_DIR = Path(os.environ.get("AUDIO_DIR", "/tmp/stackchan-audio"))
AUDIO_DIR.mkdir(parents=True, exist_ok=True)

VALID_EMOTES = {"neutral", "happy", "sleepy", "doubt", "sad", "angry"}

# ----------------------------------------------------------------------------
# Latest-only command mailbox (no FIFO: stale commands drop)
# ----------------------------------------------------------------------------
class Mailbox:
    def __init__(self) -> None:
        self._cmd: dict | None = None
        self._event = asyncio.Event()
        self.last_seen: float = 0.0        # last time the ESP32 polled
        self.last_result: dict | None = None

    def put(self, cmd: dict) -> None:
        self._cmd = cmd                    # overwrite: latest-only
        self._event.set()

    async def take(self, timeout: float) -> dict | None:
        try:
            await asyncio.wait_for(self._event.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            return None
        cmd, self._cmd = self._cmd, None
        self._event.clear()
        return cmd

    @property
    def online(self) -> bool:
        return (time.time() - self.last_seen) < 45  # ~2 poll cycles

mailbox = Mailbox()

# Latest snapshot from the robot's camera
snapshot_bytes: bytes | None = None
snapshot_event = asyncio.Event()


def _new_cmd(action: str, **payload) -> dict:
    return {"id": f"cmd_{uuid.uuid4().hex[:10]}", "action": action, **payload}


async def _synthesize(text: str, cmd_id: str) -> str:
    """Render text to WAV (16 kHz mono s16) so the robot can play it natively."""
    mp3 = AUDIO_DIR / f"{cmd_id}.mp3"
    wav = AUDIO_DIR / f"{cmd_id}.wav"
    await edge_tts.Communicate(text, TTS_VOICE).save(str(mp3))
    proc = await asyncio.create_subprocess_exec(
        "ffmpeg", "-y", "-i", str(mp3), "-ar", "16000", "-ac", "1",
        "-sample_fmt", "s16", str(wav),
        stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.DEVNULL,
    )
    await proc.wait()
    mp3.unlink(missing_ok=True)
    if proc.returncode != 0 or not wav.is_file():
        raise RuntimeError("ffmpeg transcode failed")
    return f"/audio/{cmd_id}.wav"


def _dispatch_note() -> str:
    return "dispatched" if mailbox.online else (
        "queued, but the robot has not polled in the last 45s — it may be "
        "offline; the command will run when it reconnects"
    )

# ----------------------------------------------------------------------------
# MCP tools (what Claude.ai sees)
# ----------------------------------------------------------------------------
mcp = FastMCP("stackchan", stateless_http=True)


@mcp.tool()
async def stackchan_speak(text: str) -> str:
    """Make StackChan say something out loud (Chinese or English).
    Keep it short and spoken-style; one or two sentences per call."""
    cmd = _new_cmd("speak", text=text)
    try:
        cmd["audio"] = await _synthesize(text, cmd["id"])
    except Exception as e:  # TTS hiccup: still dispatch, firmware shows text
        cmd["audio"] = None
        mailbox.put(cmd)
        return f"speak dispatched WITHOUT audio (TTS failed: {e!r}); text shown on screen"
    mailbox.put(cmd)
    return f"speak {_dispatch_note()}: {text!r}"


@mcp.tool()
def stackchan_emote(expression: str) -> str:
    """Set StackChan's face. One of: neutral, happy, sleepy, doubt, sad, angry."""
    if expression not in VALID_EMOTES:
        return f"unknown expression {expression!r}; valid: {sorted(VALID_EMOTES)}"
    mailbox.put(_new_cmd("emote", expression=expression))
    return f"emote({expression}) {_dispatch_note()}"


@mcp.tool()
def stackchan_move_head(pitch: int = 80, yaw: int = 90) -> str:
    """Turn StackChan's head. pitch: 60-85 (up/down, hardware-safe range),
    yaw: 20-160 (left/right). 80/90 is home position."""
    pitch = max(60, min(85, pitch))   # never exceed the 5-85 servo limit
    yaw = max(20, min(160, yaw))
    mailbox.put(_new_cmd("move", pitch=pitch, yaw=yaw))
    return f"move(pitch={pitch}, yaw={yaw}) {_dispatch_note()}"


@mcp.tool()
def stackchan_wiggle() -> str:
    """Make StackChan wiggle its head left-right (a happy little shake)."""
    mailbox.put(_new_cmd("wiggle"))
    return f"wiggle {_dispatch_note()}"


@mcp.tool()
async def stackchan_look():
    """Take a photo through StackChan's front camera and return it.
    Use when you want to see what is in front of the robot."""
    global snapshot_bytes
    snapshot_event.clear()
    mailbox.put(_new_cmd("snapshot"))
    if not mailbox.online:
        return ("snapshot queued, but the robot appears offline; "
                "no image will arrive until it reconnects")
    try:
        await asyncio.wait_for(snapshot_event.wait(), timeout=20.0)
    except asyncio.TimeoutError:
        return "snapshot command sent but no image arrived within 20s"
    return Image(data=snapshot_bytes, format="jpeg")


@mcp.tool()
def get_stackchan_status() -> dict:
    """Check whether StackChan is online and see its last reported result."""
    return {
        "online": mailbox.online,
        "seconds_since_last_poll": round(time.time() - mailbox.last_seen, 1)
        if mailbox.last_seen else None,
        "last_result": mailbox.last_result,
    }

# ----------------------------------------------------------------------------
# FastAPI app: robot-facing endpoints + mounted MCP
# ----------------------------------------------------------------------------
@asynccontextmanager
async def _lifespan(_: FastAPI):
    # FastMCP's streamable-HTTP transport needs its session manager running.
    async with mcp.session_manager.run():
        yield


app = FastAPI(title="stackchan-relay", lifespan=_lifespan)


def _check_token(x_stackchan_token: str | None) -> None:
    if not TOKEN or x_stackchan_token != TOKEN:
        raise HTTPException(status_code=401, detail="bad token")


@app.get("/poll")
async def poll(request: Request, x_stackchan_token: str | None = Header(None)):
    """ESP32 long-poll: hold up to 20s, return a command or 204."""
    _check_token(x_stackchan_token)
    mailbox.last_seen = time.time()
    cmd = await mailbox.take(timeout=20.0)
    mailbox.last_seen = time.time()
    if cmd is None:
        return JSONResponse(status_code=204, content=None)
    return cmd


@app.post("/result")
async def result(request: Request, x_stackchan_token: str | None = Header(None)):
    _check_token(x_stackchan_token)
    mailbox.last_result = await request.json()
    return {"ok": True}


@app.get("/audio/{name}")
async def audio(name: str, x_stackchan_token: str | None = Header(None)):
    _check_token(x_stackchan_token)
    path = AUDIO_DIR / name
    if not path.is_file() or path.parent != AUDIO_DIR:
        raise HTTPException(status_code=404)
    return FileResponse(path, media_type="audio/wav")


@app.post("/snapshot")
async def snapshot(request: Request, x_stackchan_token: str | None = Header(None)):
    _check_token(x_stackchan_token)
    global snapshot_bytes
    snapshot_bytes = await request.body()
    snapshot_event.set()
    mailbox.last_result = {"snapshot_bytes": len(snapshot_bytes)}
    return {"ok": True}


@app.get("/healthz")
def healthz():
    return {"ok": True, "robot_online": mailbox.online}


# Mount MCP under /mcp — this is the URL Claude.ai's custom connector uses.
app.mount("/", mcp.streamable_http_app())
