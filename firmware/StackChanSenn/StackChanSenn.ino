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

// ---------------------------------------------------------------------------
// CONFIG — fill these three in before flashing
// ---------------------------------------------------------------------------
static const char* WIFI_SSID = "YOUR_WIFI_SSID";      // 2.4 GHz only
static const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";
static const char* TOKEN     = "YOUR_STACKCHAN_TOKEN";

static const char* BASE_URL  = "https://claude.example.com";  // your relay domain
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

static void doEmote(const String& e)
{
    if      (e == "happy")  avatar.setExpression(Expression::Happy);
    else if (e == "sleepy") avatar.setExpression(Expression::Sleepy);
    else if (e == "doubt")  avatar.setExpression(Expression::Doubt);
    else if (e == "sad")    avatar.setExpression(Expression::Sad);
    else if (e == "angry")  avatar.setExpression(Expression::Angry);
    else                    avatar.setExpression(Expression::Neutral);
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
            static const Expression cyc[] = {Expression::Happy, Expression::Doubt,
                                             Expression::Sleepy, Expression::Neutral};
            avatar.setExpression(cyc[emoteCycle++ % 4]);
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

    avatar.init();
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
        avatar.setExpression(Expression::Sad);
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
