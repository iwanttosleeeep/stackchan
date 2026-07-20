/*
 * StickSenn.ino — custom sender firmware for StickC-Plus + Hat Mini JoyC
 * Controls StackChanSenn v4 over ESP-NOW (our 9-byte 'SN' protocol).
 *
 * Controls:
 *   Joystick        -> head (yaw/pitch), auto-calibrates center at boot
 *   BtnA click      -> wiggle
 *   BtnA hold (1s+) -> PRIVACY toggle (camera on/off)
 *   BtnB click      -> cycle expression
 *   BtnB hold (1s+) -> replay Senn's last sentence
 *
 * Board: M5Stick-C-Plus (check: arduino-cli board listall | grep -i stick)
 * Libraries: M5Unified only.
 * CONFIG: set ESPNOW_CHANNEL to the channel the robot shows at boot
 *         ("remote channel: N" on its speech bubble).
 */

#include <M5Unified.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ------------------------- CONFIG -------------------------
static const uint8_t ESPNOW_CHANNEL = 11;   // <-- match robot's boot message!

// JoyC hat: I2C 0x54 on StickC-Plus hat port (SDA=G0, SCL=G26)
static const uint8_t JOY_ADDR = 0x54;

enum : uint8_t { RC_HEAD = 0, RC_WIGGLE, RC_EMOTE, RC_PRIVACY, RC_HOME, RC_REPLAY };

typedef struct __attribute__((packed)) {
    char magic0, magic1;
    uint8_t cmd;
    int16_t yaw;      // 20..160 (90 center)
    int16_t pitch;    // 60..85  (80 home)
    uint8_t flags;
} rc_packet_t;

static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static uint16_t cx = 2048, cy = 2048;   // joystick center (calibrated at boot)

static bool joyRead(uint16_t& x, uint16_t& y)
{
    uint8_t d[2];
    Wire.beginTransmission(JOY_ADDR); Wire.write(0x00);
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom((int)JOY_ADDR, 2);
    if (Wire.available() < 2) return false;
    d[0] = Wire.read(); d[1] = Wire.read();
    x = (d[1] << 8) | d[0];
    delay(2);
    Wire.beginTransmission(JOY_ADDR); Wire.write(0x02);
    if (Wire.endTransmission(false) != 0) return false;
    Wire.requestFrom((int)JOY_ADDR, 2);
    if (Wire.available() < 2) return false;
    d[0] = Wire.read(); d[1] = Wire.read();
    y = (d[1] << 8) | d[0];
    return true;
}

static void sendCmd(uint8_t cmd, int16_t yaw = 90, int16_t pitch = 80)
{
    rc_packet_t p = {'S','N', cmd, yaw, pitch, 0};
    esp_now_send(BCAST, (uint8_t*)&p, sizeof(p));
}

static void show(const char* line1, const char* line2 = "")
{
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setCursor(6, 20); M5.Display.print(line1);
    M5.Display.setCursor(6, 50); M5.Display.print(line2);
}

void setup()
{
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setRotation(3);
    M5.Display.setTextSize(2);

    Wire.begin(0, 26);            // hat port I2C

    WiFi.mode(WIFI_STA);
    esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    if (esp_now_init() != ESP_OK) { show("esp-now FAIL"); for(;;) delay(1000); }
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BCAST, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    // calibrate joystick center: average 10 reads at rest
    uint32_t sx = 0, sy = 0; uint16_t x, y; int n = 0;
    for (int i = 0; i < 10; i++) { if (joyRead(x, y)) { sx += x; sy += y; n++; } delay(20); }
    if (n) { cx = sx / n; cy = sy / n; }

    show("StickSenn ready", ("ch " + String(ESPNOW_CHANNEL)).c_str());
}

static uint32_t lastHeadSend = 0;
static int16_t lastYaw = 90, lastPitch = 80;
// Must match the receiver's EMOTES table.  Showing the selected name makes
// the expression cycle predictable even when the robot is across the room.
static const char* const EMOTE_NAMES[] = {
    "neutral", "happy", "sleepy", "OMG", "angry", "wink",
    "sobbing", "crying", "pout", "whine", "cool", "surprised",
    "silent", "playful", "kiss", "awkward", "worried", "shocked",
    "shy", "thinking",
};
static const size_t EMOTE_COUNT = sizeof(EMOTE_NAMES) / sizeof(EMOTE_NAMES[0]);
static uint8_t emoteCycle = 0;

void loop()
{
    M5.update();

    // ---- buttons ----
    if (M5.BtnA.wasHold())        { sendCmd(RC_PRIVACY); show("PRIVACY", "toggled"); }
    else if (M5.BtnA.wasClicked()){ sendCmd(RC_WIGGLE);  show("wiggle!"); }
    if (M5.BtnB.wasHold())        { sendCmd(RC_REPLAY);  show("replay"); }
    else if (M5.BtnB.wasClicked()){
        sendCmd(RC_EMOTE);
        show("emote", EMOTE_NAMES[emoteCycle++ % EMOTE_COUNT]);
    }
    if (M5.BtnPWR.wasClicked())   { sendCmd(RC_HOME);    show("home"); }

    // ---- joystick -> head, 15 Hz max, deadzone around center ----
    uint16_t x, y;
    if (millis() - lastHeadSend > 66 && joyRead(x, y)) {
        int dx = (int)x - (int)cx;      // -2048..2047 approx
        int dy = (int)y - (int)cy;
        const int DEAD = 150;
        if (abs(dx) > DEAD || abs(dy) > DEAD) {
            int16_t yaw   = 90 + (dx * 70) / 2048;    // 20..160 (sign field-tested)
            int16_t pitch = 80 - (dy * 20) / 2048;    // ~60..85 band
            yaw   = constrain(yaw, 20, 160);
            pitch = constrain(pitch, 60, 85);
            if (abs(yaw - lastYaw) > 2 || abs(pitch - lastPitch) > 2) {
                sendCmd(RC_HEAD, yaw, pitch);
                lastYaw = yaw; lastPitch = pitch;
                lastHeadSend = millis();
            }
        }
    }
    delay(10);
}
