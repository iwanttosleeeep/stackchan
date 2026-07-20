/*
 * StackChanSenn.ino — v4 (K151 / CoreS3)
 * New in v4: ESP-NOW remote (StickC-Plus+JoyC, custom protocol), paged
 *            speech text, privacy switch, replay. All prior fixes baked in.
 * Libraries: M5Unified, M5GFX, M5Stack-Avatar, ArduinoJson v7,
 *            StackChan-BSP (+deps: IRremoteESP8266, M5Unit-NFC). Board: M5CoreS3.
 */

#include <Arduino.h>
#include <M5StackChan.h>
#include <M5Unified.h>
#include <Avatar.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_camera.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Drawable.h>
#include "EmotionFace.hpp"

// ---------------------------------------------------------------------------
// CONFIG — fill these three in before flashing
// ---------------------------------------------------------------------------
static const char* WIFI_SSID = "YOUR_WIFI_SSID";      // 2.4 GHz only
static const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
static const char* TOKEN     = "YOUR_STACKCHAN_TOKEN";

static const char* BASE_URL  = "https://claude.sehnsucht.uk";  // your relay domain
static const uint32_t POLL_TIMEOUT_MS = 25000;
static const uint32_t RETRY_DELAY_MS  = 3000;
static const size_t   AUDIO_MAX_BYTES = 2 * 1024 * 1024;

// ---------------------------------------------------------------------------
// ESP-NOW REMOTE (our own protocol; sender = StickC-Plus + JoyC, custom fw)
// Packet: 'S','N', cmd, yaw(i16), pitch(i16), flags  -> 9 bytes packed
// ---------------------------------------------------------------------------
enum : uint8_t { RC_HEAD = 0, RC_WIGGLE, RC_EMOTE, RC_PRIVACY, RC_HOME, RC_REPLAY };

typedef struct __attribute__((packed)) {
    char magic0, magic1;      // 'S','N'
    uint8_t cmd;
    int16_t yaw;              // tool convention 20..160
    int16_t pitch;            // tool convention 60..85
    uint8_t flags;
} rc_packet_t;

static volatile rc_packet_t rcLatest;
static volatile bool rcPending = false;

static void onEspNowRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len)
{
    if (len != sizeof(rc_packet_t)) return;
    const rc_packet_t* p = (const rc_packet_t*)data;
    if (p->magic0 != 'S' || p->magic1 != 'N') return;
    memcpy((void*)&rcLatest, p, sizeof(rc_packet_t));   // latest-only, like the mailbox
    rcPending = true;
}

using namespace m5avatar;
Avatar avatar;

static volatile bool privacyMode = false;
static uint8_t volume = 180;
static uint8_t* lastAudio = nullptr;   // replay buffer (PSRAM)
static size_t  lastAudioLen = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static bool httpBegin(HTTPClient& http, WiFiClientSecure& client, const String& url)
{
    client.setInsecure();  // trust is enforced by the token, not the cert chain
    if (!http.begin(client, url)) return false;
    http.addHeader("X-Stackchan-Token", TOKEN);
    return true;
}

static void reportResult(const String& id, bool ok, const String& note)
{
    WiFiClientSecure client;
    HTTPClient http;
    if (!httpBegin(http, client, String(BASE_URL) + "/result")) return;
    http.addHeader("Content-Type", "application/json");
    JsonDocument doc;
    doc["id"] = id;
    doc["ok"] = ok;
    if (note.length()) doc["note"] = note;
    String body;
    serializeJson(doc, body);
    http.POST(body);
    http.end();
}

static void doMove(int toolPitch, int toolYaw, int speed = 400)
{
    toolPitch = constrain(toolPitch, 60, 85);
    toolYaw   = constrain(toolYaw, 20, 160);
    int bspYaw   = (90 - toolYaw) * 10;
    int bspPitch = constrain((85 - toolPitch) * 20, 0, 900);
    M5StackChan.Motion.move(bspYaw, bspPitch, speed);
}

static void doWiggle()
{
    for (int i = 0; i < 2; i++) {
        M5StackChan.Motion.moveYaw(200, 900);  delay(260);
        M5StackChan.Motion.moveYaw(-200, 900); delay(260);
    }
    M5StackChan.Motion.goHome(700);
}

// Kaomoji-inspired faces are drawn as vectors, so no Japanese font or bitmap
// assets are needed.  Eye and mouth parts share this lightweight style state.
enum class EmoteStyle : uint8_t {
    BuiltIn, Strained, Delighted, Dizzy, Determined, Worried, Humming,
    Crying, Wink, Confused, Bashful, Pout, Kiss
};

static EmoteStyle activeStyle = EmoteStyle::BuiltIn;

class BlankPart final : public Drawable {
 public:
    void draw(M5Canvas*, BoundingRect, DrawContext*) override {}
};

class KaomojiEye final : public Drawable {
    bool isLeft;

    static void xEye(M5Canvas* c, int x, int y, uint16_t color)
    {
        c->drawLine(x - 14, y - 12, x + 14, y + 12, color);
        c->drawLine(x - 14, y + 12, x + 14, y - 12, color);
        c->drawLine(x - 13, y - 12, x + 15, y + 12, color);
        c->drawLine(x - 13, y + 12, x + 15, y - 12, color);
    }

    static void caretEye(M5Canvas* c, int x, int y, uint16_t color)
    {
        c->drawLine(x - 16, y + 8, x, y - 9, color);
        c->drawLine(x, y - 9, x + 16, y + 8, color);
        c->drawLine(x - 16, y + 9, x, y - 8, color);
        c->drawLine(x, y - 8, x + 16, y + 9, color);
    }

    static void closedEye(M5Canvas* c, int x, int y, uint16_t color)
    {
        c->drawLine(x - 15, y, x + 15, y, color);
        c->drawLine(x - 15, y + 1, x + 15, y + 1, color);
    }

 public:
    explicit KaomojiEye(bool left) : isLeft(left) {}

    void draw(M5Canvas* c, BoundingRect rect, DrawContext* ctx) override
    {
        const uint16_t color = ctx->getColorDepth() == 1
            ? 1 : ctx->getColorPalette()->get(COLOR_PRIMARY);
        const int x = rect.getCenterX();
        const int y = rect.getCenterY();

        const float eyeOpen = isLeft ? ctx->getLeftEyeOpenRatio()
                                     : ctx->getRightEyeOpenRatio();
        if (eyeOpen < 0.1f && activeStyle != EmoteStyle::Wink) {
            closedEye(c, x, y, color);
            return;
        }

        switch (activeStyle) {
            case EmoteStyle::Strained:
                xEye(c, x, y, color);
                break;
            case EmoteStyle::Delighted:
            case EmoteStyle::Kiss:
                caretEye(c, x, y, color);
                break;
            case EmoteStyle::Dizzy:
                c->drawCircle(x, y, 17, color);
                c->drawCircle(x, y, 10, color);
                c->fillCircle(x + 6, y - 1, 3, color);
                c->drawLine(x + 15, y + 8, x + 23, y + 15, color);
                break;
            case EmoteStyle::Determined:
                c->drawLine(x - 17, y + (isLeft ? -9 : 9),
                            x + 17, y + (isLeft ? 9 : -9), color);
                c->fillCircle(x + (isLeft ? 5 : -5), y + 5, 5, color);
                break;
            case EmoteStyle::Worried:
                c->drawLine(x - 16, y + (isLeft ? 8 : -8),
                            x + 16, y + (isLeft ? -8 : 8), color);
                c->fillCircle(x, y + 9, 4, color);
                break;
            case EmoteStyle::Humming:
                closedEye(c, x, y, color);
                break;
            case EmoteStyle::Crying: {
                c->drawLine(x - 15, y - 5, x, y + 5, color);
                c->drawLine(x, y + 5, x + 15, y - 5, color);
                const int tx = x + (isLeft ? 13 : -13);
                c->fillCircle(tx, y + 20, 5, color);
                c->fillTriangle(tx, y + 6, tx - 5, y + 19, tx + 5, y + 19, color);
                break;
            }
            case EmoteStyle::Wink:
                if (isLeft) caretEye(c, x, y, color);
                else closedEye(c, x, y, color);
                break;
            case EmoteStyle::Confused:
                if (isLeft) c->drawCircle(x, y, 17, color);
                else c->drawCircle(x, y, 9, color);
                c->fillCircle(x, y, isLeft ? 6 : 3, color);
                break;
            case EmoteStyle::Bashful:
                c->drawLine(x - 15, y - 3, x, y + 6, color);
                c->drawLine(x, y + 6, x + 15, y - 3, color);
                for (int i = 0; i < 3; i++) {
                    const int cheekX = x + (isLeft ? 30 : -30) + i * 5;
                    c->drawLine(cheekX, y + 18, cheekX + 6, y + 10, color);
                }
                break;
            case EmoteStyle::Pout:
                c->fillCircle(x - 8, y, 8, color);
                break;
            case EmoteStyle::BuiltIn:
                break;
        }
    }
};

class KaomojiMouth final : public Drawable {
    static void musicNote(M5Canvas* c, int x, int y, uint16_t color)
    {
        c->fillCircle(x, y + 18, 5, color);
        c->drawLine(x + 5, y + 18, x + 5, y - 8, color);
        c->drawLine(x + 5, y - 8, x + 19, y - 3, color);
        c->drawLine(x + 5, y - 6, x + 19, y - 1, color);
    }

    static void puckered(M5Canvas* c, int x, int y, uint16_t color)
    {
        c->fillCircle(x - 7, y, 7, color);
        c->fillCircle(x + 7, y, 7, color);
    }

 public:
    void draw(M5Canvas* c, BoundingRect rect, DrawContext* ctx) override
    {
        const uint16_t color = ctx->getColorDepth() == 1
            ? 1 : ctx->getColorPalette()->get(COLOR_PRIMARY);
        const int x = rect.getCenterX();
        const int y = rect.getCenterY();
        const float open = ctx->getMouthOpenRatio();

        // Preserve lip-sync: while speaking, every style temporarily gets an
        // animated open mouth, then returns to its kaomoji shape.
        if (open > 0.15f) {
            const int h = 5 + (int)(open * 27);
            c->fillRoundRect(x - 23, y - h / 2, 46, h, 7, color);
            return;
        }

        switch (activeStyle) {
            case EmoteStyle::Strained:
                c->drawLine(x - 22, y, x - 11, y - 6, color);
                c->drawLine(x - 11, y - 6, x, y + 2, color);
                c->drawLine(x, y + 2, x + 11, y - 6, color);
                c->drawLine(x + 11, y - 6, x + 22, y, color);
                break;
            case EmoteStyle::Delighted:
                c->fillTriangle(x - 27, y - 9, x + 27, y - 9, x, y + 25, color);
                break;
            case EmoteStyle::Dizzy:
                c->drawLine(x - 22, y - 3, x - 8, y + 4, color);
                c->drawLine(x - 8, y + 4, x + 8, y - 3, color);
                c->drawLine(x + 8, y - 3, x + 22, y + 4, color);
                break;
            case EmoteStyle::Determined:
                c->fillRect(x - 24, y - 2, 48, 5, color);
                break;
            case EmoteStyle::Worried:
            case EmoteStyle::Crying:
                c->drawLine(x - 24, y + 11, x, y - 8, color);
                c->drawLine(x, y - 8, x + 24, y + 11, color);
                break;
            case EmoteStyle::Humming:
                puckered(c, x, y, color);
                musicNote(c, x + 75, y - 45, color);
                break;
            case EmoteStyle::Wink:
            case EmoteStyle::Bashful:
                c->drawLine(x - 25, y - 7, x, y + 12, color);
                c->drawLine(x, y + 12, x + 25, y - 7, color);
                break;
            case EmoteStyle::Confused:
                c->drawCircle(x, y, 10, color);
                break;
            case EmoteStyle::Pout:
                puckered(c, x, y, color);
                break;
            case EmoteStyle::Kiss:
                puckered(c, x, y, color);
                c->fillCircle(x + 61, y - 32, 7, color);
                c->fillCircle(x + 73, y - 32, 7, color);
                c->fillTriangle(x + 55, y - 30, x + 79, y - 30, x + 67, y - 14, color);
                break;
            case EmoteStyle::BuiltIn:
                break;
        }
    }
};

// Keep cloud names and the remote's cycle in one table.
struct Emote {
    const char* name;
    Expression expression;
    EmoteStyle style;
};

static const Emote EMOTES[] = {
    {"neutral",    Expression::Neutral, EmoteStyle::BuiltIn},
    {"happy",      Expression::Happy,   EmoteStyle::BuiltIn},
    {"sleepy",     Expression::Sleepy,  EmoteStyle::BuiltIn},
    {"omg",        Expression::Neutral, EmoteStyle::Pout},
    {"angry",      Expression::Neutral, EmoteStyle::Pout},
    {"wink",       Expression::Neutral, EmoteStyle::Pout},
    {"sobbing",    Expression::Neutral, EmoteStyle::Pout},
    {"crying",     Expression::Neutral, EmoteStyle::Pout},
    {"pout",       Expression::Neutral, EmoteStyle::Pout},
    {"whine",      Expression::Neutral, EmoteStyle::Pout},
    {"cool",       Expression::Neutral, EmoteStyle::Pout},
    {"surprised",  Expression::Neutral, EmoteStyle::Pout},
    {"silent",     Expression::Neutral, EmoteStyle::Pout},
    {"playful",    Expression::Neutral, EmoteStyle::Pout},
    {"kiss",       Expression::Neutral, EmoteStyle::Pout},
    {"awkward",    Expression::Neutral, EmoteStyle::Pout},
    {"worried",    Expression::Neutral, EmoteStyle::Pout},
    {"shocked",    Expression::Neutral, EmoteStyle::Pout},
    {"shy",        Expression::Neutral, EmoteStyle::Pout},
    {"thinking",   Expression::Neutral, EmoteStyle::Pout},
};
static constexpr size_t EMOTE_COUNT = sizeof(EMOTES) / sizeof(EMOTES[0]);
static Face* defaultFace = nullptr;
static Face* downloadedEmotionFace = nullptr;

static void applyEmote(size_t emoteIndex)
{
    const Emote& emote = EMOTES[emoteIndex % EMOTE_COUNT];
    activeStyle = emote.style;
    avatar.setIsAutoBlink(false);
    avatar.setEyeOpenRatio(1.0f);
    avatar.setMouthOpenRatio(0.0f);
    avatar.setRightGaze(0.0f, 0.0f);
    avatar.setLeftGaze(0.0f, 0.0f);

    if (emote.style == EmoteStyle::BuiltIn) {
        if (defaultFace) avatar.setFace(defaultFace);
        avatar.setExpression(emote.expression);
        avatar.setIsAutoBlink(true);
    } else {
        const size_t assetIndex = stackchan_emotions::findAsset(emote.name);
        if (assetIndex < stackchan_emotions::ASSET_COUNT) {
            stackchan_emotions::setActiveAsset(assetIndex);
        }
        if (!downloadedEmotionFace) {
            downloadedEmotionFace = stackchan_emotions::createEmotionFace();
        }
        avatar.setFace(downloadedEmotionFace);
        avatar.setExpression(Expression::Neutral);
        avatar.setIsAutoBlink(false);
    }
}

static bool doEmote(const String& name)
{
    for (size_t i = 0; i < EMOTE_COUNT; i++) {
        if (name == EMOTES[i].name) {
            applyEmote(i);
            return true;
        }
    }
    applyEmote(0);
    return false;
}

// ---------------------------------------------------------------------------
// Speech with PAGED text: long sentences rotate on the balloon while audio
// plays instead of overflowing off-screen.
// ---------------------------------------------------------------------------
static const size_t PAGE_CHARS = 16;        // bytes per page (fits the balloon)
static const uint32_t PAGE_MS  = 2600;      // page dwell time

static void playWithPagedText(const String& text, const uint8_t* buf, size_t len)
{
    // Build pages on word boundaries where possible
    std::vector<String> pages;
    size_t i = 0;
    while (i < text.length()) {
        size_t end = min(i + PAGE_CHARS, (size_t)text.length());
        if (end < text.length()) {
            int sp = text.lastIndexOf(' ', end);
            if (sp > (int)i + 10) end = sp;          // break at a space if sane
        }
        pages.push_back(text.substring(i, end));
        i = (text[end] == ' ') ? end + 1 : end;
    }
    if (pages.empty()) pages.push_back("");

    if (buf && len) M5.Speaker.playWav(buf, len);

    size_t page = 0;
    uint32_t pageAt = 0;
    do {
        if (millis() - pageAt >= PAGE_MS || pageAt == 0) {
            avatar.setSpeechText(pages[page % pages.size()].c_str());
            page++;
            pageAt = millis();
        }
        if (buf && len) avatar.setMouthOpenRatio((float)random(2, 10) / 10.0f);
        delay(120);
    } while (M5.Speaker.isPlaying() || page < pages.size());

    avatar.setMouthOpenRatio(0.0f);
    avatar.setSpeechText("");
}

static bool doSpeak(const String& text, const String& audioPath)
{
    if (audioPath.length() == 0) {          // TTS degraded: text only
        playWithPagedText(text, nullptr, 0);
        return true;
    }

    WiFiClientSecure client;
    HTTPClient http;
    if (!httpBegin(http, client, String(BASE_URL) + audioPath)) return false;
    if (http.GET() != 200) { http.end(); return false; }

    int len = http.getSize();
    if (len <= 0 || (size_t)len > AUDIO_MAX_BYTES) { http.end(); return false; }

    uint8_t* buf = (uint8_t*)ps_malloc(len);
    if (!buf) { http.end(); return false; }

    WiFiClient* stream = http.getStreamPtr();
    int got = 0;
    uint32_t lastData = millis();
    while (got < len && millis() - lastData < 10000) {
        size_t avail = stream->available();
        if (avail) {
            got += stream->readBytes(buf + got, min(avail, (size_t)(len - got)));
            lastData = millis();
        } else delay(5);
    }
    http.end();
    if (got < len) { free(buf); return false; }

    playWithPagedText(text, buf, len);

    if (lastAudio) free(lastAudio);          // keep for the replay button
    lastAudio = buf;
    lastAudioLen = len;
    return true;
}

// ---------------------------------------------------------------------------
// Camera (CoreS3 GC0308) — official pin map; init happens in setup()
// before BSP's loop starts (shared internal I2C).
// ---------------------------------------------------------------------------
static camera_config_t camera_config = {
    .pin_pwdn = -1,  .pin_reset = -1,   .pin_xclk = -1,
    .pin_sscb_sda = 12, .pin_sscb_scl = 11,
    .pin_d7 = 47, .pin_d6 = 48, .pin_d5 = 16, .pin_d4 = 15,
    .pin_d3 = 42, .pin_d2 = 41, .pin_d1 = 40, .pin_d0 = 39,
    .pin_vsync = 46, .pin_href = 38, .pin_pclk = 45,
    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0, .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_RGB565,
    .frame_size = FRAMESIZE_QVGA,
    .jpeg_quality = 0,
    .fb_count = 2,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
    .sccb_i2c_port = -1,
};

static bool camReady = false;

static bool camInit()
{
    if (camReady) return true;
    M5.In_I2C.release();
    camReady = (esp_camera_init(&camera_config) == ESP_OK);
    return camReady;
}

static String doSnapshot()
{
    if (privacyMode) return "privacy mode is ON (remote toggle)";
    if (!camInit()) return "init failed";

    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) return "fb_get failed";
    esp_camera_fb_return(fb);
    fb = esp_camera_fb_get();
    if (!fb) return "fb_get(2) failed";

    uint8_t* jpg = nullptr;
    size_t jpg_len = 0;
    bool conv = frame2jpg(fb, 80, &jpg, &jpg_len);
    esp_camera_fb_return(fb);
    if (!conv || !jpg) return "frame2jpg failed";

    WiFiClientSecure client;
    HTTPClient http;
    int code = -1;
    if (httpBegin(http, client, String(BASE_URL) + "/snapshot")) {
        http.addHeader("Content-Type", "image/jpeg");
        code = http.POST(jpg, jpg_len);
        http.end();
    }
    free(jpg);
    if (code != 200) return "POST failed code=" + String(code);
    return "";
}

// ---------------------------------------------------------------------------
// Remote-control handling (runs in loop(), core 1 main task)
// ---------------------------------------------------------------------------
static void flashSpeech(const String& s, uint32_t ms = 1500)
{
    avatar.setSpeechText(s.c_str());
    delay(ms);
    avatar.setSpeechText("");
}

static bool happyToggle = false;
static uint8_t emoteCycle = 0;

static void handleRC()
{
    rc_packet_t p;
    memcpy(&p, (const void*)&rcLatest, sizeof(p));
    rcPending = false;

    switch (p.cmd) {
        case RC_HEAD:
            doMove(p.pitch, p.yaw, 600);
            break;
        case RC_WIGGLE: doWiggle(); break;
        case RC_EMOTE: {
            applyEmote(emoteCycle++);
            break;
        }
        case RC_PRIVACY:
            privacyMode = !privacyMode;
            flashSpeech(privacyMode ? "PRIVACY: camera OFF" : "camera back ON");
            break;
        case RC_HOME: M5StackChan.Motion.goHome(500); break;
        case RC_REPLAY:
            if (lastAudio && lastAudioLen) M5.Speaker.playWav(lastAudio, lastAudioLen);
            else flashSpeech("nothing to replay yet");
            break;
    }
}

// ---------------------------------------------------------------------------
// Command loop (network task, core 1, 20KB stack)
// ---------------------------------------------------------------------------
static void handleCommand(JsonDocument& doc)
{
    String id     = doc["id"] | "";
    String action = doc["action"] | "";
    bool ok = true;
    String note = "";

    if (action == "speak") {
        String expression = doc["expression"] | "";
        if (expression.length() > 0) doEmote(expression);
        ok = doSpeak(doc["text"] | "", doc["audio"] | "");
        if (!ok) note = "audio fetch/play failed";
    } else if (action == "emote") {
        doEmote(doc["expression"] | "neutral");
    } else if (action == "move") {
        doMove(doc["pitch"] | 80, doc["yaw"] | 90);
    } else if (action == "wiggle") {
        doWiggle();
    } else if (action == "snapshot") {
        String err = doSnapshot();
        ok = (err.length() == 0);
        if (!ok) note = err;
    } else {
        ok = false;
        note = "unknown action";
    }
    reportResult(id, ok, note);
}

static void pollTask(void*)
{
    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            avatar.setExpression(Expression::Sleepy);
            WiFi.reconnect();
            delay(RETRY_DELAY_MS);
            continue;
        }

        WiFiClientSecure client;
        HTTPClient http;
        http.setTimeout(POLL_TIMEOUT_MS);
        if (!httpBegin(http, client, String(BASE_URL) + "/poll")) {
            delay(RETRY_DELAY_MS);
            continue;
        }
        int code = http.GET();
        if (code == 200) {
            String body = http.getString();
            http.end();
            JsonDocument doc;
            if (deserializeJson(doc, body) == DeserializationError::Ok) {
                handleCommand(doc);
            }
        } else {
            http.end();
            if (code != 204) delay(RETRY_DELAY_MS);
        }
    }
}

// ---------------------------------------------------------------------------
void setup()
{
    M5StackChan.begin();
    M5.Speaker.setVolume(volume);

    camInit();                    // claim camera before BSP's loop starts

    avatar.init(8);
    defaultFace = avatar.getFace();
    avatar.setExpression(Expression::Sleepy);
    avatar.setSpeechText("Connecting Wi-Fi...");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) delay(500);

    if (WiFi.status() == WL_CONNECTED) {
        avatar.setSpeechText("Online. Polling home...");
        avatar.setExpression(Expression::Neutral);
    } else {
        avatar.setSpeechText("Wi-Fi failed (2.4GHz only!)");
        doEmote("worried");
    }

    if (WiFi.status() == WL_CONNECTED) {
        if (esp_now_init() == ESP_OK) {
            esp_now_register_recv_cb(onEspNowRecv);
            // show the Wi-Fi channel: the controller must be set to the same one
            char chs[32];
            snprintf(chs, sizeof(chs), "remote channel: %d", WiFi.channel());
            avatar.setSpeechText(chs);
            delay(2500);
        }
    }

    M5StackChan.Motion.goHome(400);
    delay(1200);
    avatar.setSpeechText("");

    xTaskCreatePinnedToCore(pollTask, "poll", 20480, nullptr, 1, nullptr, 1);
}

void loop()
{
    M5StackChan.update();
    if (rcPending) handleRC();
    delay(50);
}
