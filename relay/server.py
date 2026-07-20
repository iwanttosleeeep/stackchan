"""
StackChan Relay — Claude.ai (MCP) <-> ESP32 bridge, single process.

Architecture:
  Claude.ai custom connector --> /mcp       (Streamable HTTP, FastMCP)
  ESP32 long-polls           --> /poll      (latest-only command queue)
  ESP32 reports back         --> /result
  ESP32 fetches speech audio --> /audio/{cmd_id}.wav  (local Piper)
  ESP32 uploads photos       --> /snapshot

Run:  uvicorn server:app --host 127.0.0.1 --port 8011
Env:  STACKCHAN_TOKEN   shared secret; ESP32 must send X-Stackchan-Token
      PIPER_MODEL       path to en_GB-vctk-medium.onnx
      PIPER_SPEAKER     VCTK speaker name (default: p275, speaker id 55)
      PIPER_LENGTH_SCALE  speech timing (<1 faster, >1 slower; default: 1.0)
"""

import asyncio
import logging
import os
import time
import uuid
import wave
from contextlib import asynccontextmanager
from pathlib import Path

from fastapi import FastAPI, Header, HTTPException, Request
from fastapi.responses import FileResponse, JSONResponse
from mcp.server.fastmcp import FastMCP, Image
from piper import PiperVoice, SynthesisConfig

# ----------------------------------------------------------------------------
# Config
# ----------------------------------------------------------------------------
TOKEN = os.environ.get("STACKCHAN_TOKEN", "")
AUDIO_DIR = Path(os.environ.get("AUDIO_DIR", "/tmp/stackchan-audio"))
AUDIO_DIR.mkdir(parents=True, exist_ok=True)
PIPER_MODEL = Path(os.environ.get(
    "PIPER_MODEL", "/opt/stackchan-relay/voices/en_GB-vctk-medium.onnx"
))
PIPER_SPEAKER = os.environ.get("PIPER_SPEAKER", "p275")
PIPER_LENGTH_SCALE = float(os.environ.get("PIPER_LENGTH_SCALE", "1.0"))

logger = logging.getLogger("stackchan-relay")
piper_voice: PiperVoice | None = None
piper_speaker_id: int | None = None
piper_lock = asyncio.Lock()

VALID_EMOTES = {
    "neutral", "happy", "sleepy", "omg", "angry", "wink",
    "sobbing", "crying", "pout", "whine", "cool", "surprised",
    "silent", "playful", "kiss", "awkward", "worried", "shocked",
    "shy", "thinking",
}

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


def _synthesize_sync(text: str, wav: Path) -> None:
    """Render one WAV using the model kept warm for the process lifetime."""
    if piper_voice is None or piper_speaker_id is None:
        raise RuntimeError("Piper voice is not loaded")

    temporary = wav.with_suffix(".wav.tmp")
    try:
        with wave.open(str(temporary), "wb") as wav_file:
            piper_voice.synthesize_wav(
                text,
                wav_file,
                syn_config=SynthesisConfig(
                    speaker_id=piper_speaker_id,
                    length_scale=PIPER_LENGTH_SCALE,
                ),
            )
        temporary.replace(wav)
    finally:
        temporary.unlink(missing_ok=True)


async def _synthesize(text: str, cmd_id: str) -> str:
    """Render text locally to a mono 16-bit WAV that the robot can play."""
    wav = AUDIO_DIR / f"{cmd_id}.wav"
    started = time.perf_counter()
    async with piper_lock:
        await asyncio.to_thread(_synthesize_sync, text, wav)
    elapsed = time.perf_counter() - started
    logger.info(
        "Piper synthesized %d chars in %.3fs (%d bytes)",
        len(text), elapsed, wav.stat().st_size,
    )
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
async def stackchan_speak(text: str, expression: str | None = None) -> str:
    """Make StackChan speak English, optionally with an expression.

    Keep it short and spoken-style; one or two sentences per call. When an
    expression is supplied, it is applied atomically before speech so it cannot
    be lost as a separate command. Omit it to keep the currently displayed face.
    """
    if expression is not None and expression not in VALID_EMOTES:
        return f"unknown expression {expression!r}; valid: {sorted(VALID_EMOTES)}"

    payload = {"text": text}
    if expression is not None:
        payload["expression"] = expression
    cmd = _new_cmd("speak", **payload)
    try:
        cmd["audio"] = await _synthesize(text, cmd["id"])
    except Exception as e:  # TTS hiccup: still dispatch, firmware shows text
        cmd["audio"] = None
        mailbox.put(cmd)
        return f"speak dispatched WITHOUT audio (TTS failed: {e!r}); text shown on screen"
    mailbox.put(cmd)
    face_note = f" with {expression}" if expression is not None else ""
    return f"speak{face_note} {_dispatch_note()}: {text!r}"


@mcp.tool()
def stackchan_emote(expression: str) -> str:
    """Set StackChan's face. Built-ins: neutral, happy, sleepy.
    SVG-derived faces: omg, angry, wink, sobbing, crying, pout, whine,
    cool, surprised, silent, playful, kiss, awkward, worried, shocked, shy,
    thinking."""
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
    global piper_voice, piper_speaker_id

    if not PIPER_MODEL.is_file():
        raise RuntimeError(
            f"Piper model not found: {PIPER_MODEL}. "
            "Download en_GB-vctk-medium.onnx and its .onnx.json config."
        )

    started = time.perf_counter()
    piper_voice = await asyncio.to_thread(PiperVoice.load, PIPER_MODEL)
    piper_speaker_id = piper_voice.config.speaker_id_map.get(PIPER_SPEAKER)
    if piper_speaker_id is None:
        raise RuntimeError(
            f"Piper speaker {PIPER_SPEAKER!r} is unavailable in {PIPER_MODEL}"
        )
    logger.info(
        "Loaded Piper model %s, speaker %s (%d), length scale %.2f in %.3fs",
        PIPER_MODEL, PIPER_SPEAKER, piper_speaker_id,
        PIPER_LENGTH_SCALE, time.perf_counter() - started,
    )

    # Piper/espeak-ng performs some lazy initialization on the first sentence.
    # Pay that cost during service startup instead of during the first command.
    warmup = AUDIO_DIR / f".piper-warmup-{os.getpid()}.wav"
    warmup_started = time.perf_counter()
    try:
        await asyncio.to_thread(_synthesize_sync, "Ready.", warmup)
    finally:
        warmup.unlink(missing_ok=True)
    logger.info("Piper warm-up completed in %.3fs", time.perf_counter() - warmup_started)

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
