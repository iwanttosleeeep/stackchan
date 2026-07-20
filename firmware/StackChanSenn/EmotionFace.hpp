#pragma once

#include <Avatar.h>
#include <cstring>

#include "EmotionAssets.h"

namespace stackchan_emotions {

using namespace m5avatar;

static volatile size_t active_asset_index = 0;

inline void setActiveAsset(size_t index) {
  active_asset_index = index % ASSET_COUNT;
}

inline size_t findAsset(const char* name) {
  for (size_t i = 0; i < ASSET_COUNT; ++i) {
    if (strcmp(name, ASSETS[i].name) == 0) return i;
  }
  return ASSET_COUNT;
}

class BlankPart final : public Drawable {
 public:
  void draw(M5Canvas*, BoundingRect, DrawContext*) override {}
};

class EmotionArtwork final : public Drawable {
  static void drawSpeakingMouth(M5Canvas* canvas, const Asset& asset,
                                int offsetX, int offsetY, float open) {
    if (open <= 0.05f) return;

    int left = 115;
    int width = 96;
    if (strcmp(asset.name, "silent") == 0) {
      left = 101;
      width = 137;
    } else if (strcmp(asset.name, "playful") == 0) {
      left = 105;
      width = 122;
    } else if (strcmp(asset.name, "cool") == 0) {
      left = 125;
      width = 77;
    }

    const uint16_t black = canvas->color565(0, 0, 0);
    const uint16_t white = canvas->color565(255, 255, 255);
    canvas->fillRect(offsetX + left, offsetY + 119, width, 67, black);

    const int x = offsetX + 163;
    const int y = offsetY + 149;
    const int radiusY = 4 + static_cast<int>(open * 15.0f);
    canvas->fillEllipse(x, y, 23, radiusY, white);
    if (radiusY > 7) {
      canvas->fillEllipse(x, y, 17, radiusY - 4, black);
    }
  }

  static void drawWinkEye(M5Canvas* canvas, int offsetX, int offsetY,
                          uint32_t now) {
    const uint16_t white = canvas->color565(255, 255, 255);
    const uint16_t black = canvas->color565(0, 0, 0);
    const uint32_t phase = now % 2200;
    const int x = offsetX + 230;
    const int y = offsetY + 96;

    // Stay surprised/open most of the time, then reveal the artwork's original
    // curved wink underneath for a quick, readable blink.
    if (phase < 1850) {
      canvas->fillRect(x - 27, y - 22, 54, 44, black);
      canvas->fillEllipse(x, y, 9, 15, white);
      canvas->fillCircle(x - 5, y - 3, 3, black);
    }
  }

  static void drawFloatingHeart(M5Canvas* canvas, int offsetX, int offsetY,
                                uint32_t now) {
    const uint32_t phase = now % 2200;
    if (phase >= 1750) return;  // brief pause before the next heart appears

    const float progress = phase / 1750.0f;
    const int x = offsetX + 216 + static_cast<int>(24.0f * progress);
    const int y = offsetY + 145 - static_cast<int>(21.0f * progress);
    const uint16_t red = canvas->color565(255, 79, 79);
    canvas->fillCircle(x - 7, y - 5, 8, red);
    canvas->fillCircle(x + 7, y - 5, 8, red);
    canvas->fillTriangle(x - 15, y - 3, x + 15, y - 3, x, y + 14, red);
  }

 public:
  void draw(M5Canvas* canvas, BoundingRect, DrawContext* ctx) override {
    const Asset& asset = ASSETS[active_asset_index % ASSET_COUNT];
    const float breath = ctx->getBreath();
    int x = 0;
    int y = 0;

    // Subtle movement keeps the downloaded artwork recognizable.  The full
    // image is intentionally never distorted: only its position changes.
    switch (asset.motion) {
      case Motion::Bob:
        y = breath > 0.5f ? 2 : 0;
        break;
      case Motion::Float:
        y = breath > 0.5f ? 0 : 2;
        break;
      case Motion::Shake:
        x = breath < 0.33f ? -2 : (breath > 0.66f ? 2 : 0);
        break;
      case Motion::Tear:
        y = breath > 0.5f ? 3 : 0;
        break;
      case Motion::Still:
        break;
    }

    // The generated PNG is 320x320 because Quick Look produces square
    // thumbnails.  maxHeight clips it to the CoreS3's visible 320x240 area.
    canvas->drawPng(asset.data, asset.size, x, y, 320, 240);
    drawSpeakingMouth(canvas, asset, x, y, ctx->getMouthOpenRatio());

    const uint32_t now = lgfx::millis();
    switch (asset.effect) {
      case Effect::Wink:
        drawWinkEye(canvas, x, y, now);
        break;
      case Effect::Kiss:
        drawFloatingHeart(canvas, x, y, now);
        break;
      case Effect::None:
        break;
    }
  }
};

inline Face* createEmotionFace() {
  return new Face(new EmotionArtwork(), new BoundingRect(0, 0),
                  new BlankPart(), new BoundingRect(0, 0),
                  new BlankPart(), new BoundingRect(0, 0),
                  new BlankPart(), new BoundingRect(0, 0),
                  new BlankPart(), new BoundingRect(0, 0));
}

}  // namespace stackchan_emotions
