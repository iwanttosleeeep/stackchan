#include <Avatar.h>
#include <M5Unified.h>

#include <algorithm>
#include <cstdio>

#include "../../../firmware/StackChanSenn/EmotionFace.hpp"

using namespace m5avatar;

namespace {

Avatar avatar;
Face* default_face = nullptr;
Face* downloaded_face = nullptr;

struct PreviewItem {
  const char* name;
  bool built_in;
  Expression expression;
  size_t asset_index;
};

const PreviewItem PREVIEW_ITEMS[] = {
    {"neutral", true, Expression::Neutral, 0},
    {"happy", true, Expression::Happy, 0},
    {"sleepy", true, Expression::Sleepy, 0},
    {"OMG", false, Expression::Neutral, 0},
    {"angry", false, Expression::Neutral, 1},
    {"wink", false, Expression::Neutral, 2},
    {"sobbing", false, Expression::Neutral, 3},
    {"crying", false, Expression::Neutral, 4},
    {"pout", false, Expression::Neutral, 5},
    {"whine", false, Expression::Neutral, 6},
    {"cool", false, Expression::Neutral, 7},
    {"surprised", false, Expression::Neutral, 8},
    {"silent", false, Expression::Neutral, 9},
    {"playful", false, Expression::Neutral, 10},
    {"kiss", false, Expression::Neutral, 11},
    {"awkward", false, Expression::Neutral, 12},
    {"worried", false, Expression::Neutral, 13},
    {"shocked", false, Expression::Neutral, 14},
    {"shy", false, Expression::Neutral, 15},
    {"thinking", false, Expression::Neutral, 16},
};

constexpr size_t kPreviewCount =
    sizeof(PREVIEW_ITEMS) / sizeof(PREVIEW_ITEMS[0]);
constexpr uint32_t kPreviewIntervalMs = 2400;

size_t preview_index = 0;
uint32_t last_change_ms = 0;

void showCurrentItem() {
  const PreviewItem& item = PREVIEW_ITEMS[preview_index];
  if (item.built_in) {
    avatar.setFace(default_face);
    avatar.setExpression(item.expression);
    avatar.setIsAutoBlink(true);
  } else {
    stackchan_emotions::setActiveAsset(item.asset_index);
    avatar.setFace(downloaded_face);
    avatar.setExpression(Expression::Neutral);
    avatar.setIsAutoBlink(false);
  }
  std::printf("Expression: %s\n", item.name);
  std::fflush(stdout);
}

}  // namespace

void setup() {
  M5.begin();

  default_face = avatar.getFace();
  downloaded_face = stackchan_emotions::createEmotionFace();
  avatar.init(8);

  const auto rect = avatar.getFace()->getBoundingRect();
  const auto scale_w = M5.Display.width() / static_cast<float>(rect->getWidth());
  const auto scale_h = M5.Display.height() / static_cast<float>(rect->getHeight());
  avatar.setScale(std::min(scale_w, scale_h));
  const auto offset_x = (rect->getWidth() - M5.Display.width()) / 2;
  const auto offset_y = (rect->getHeight() - M5.Display.height()) / 2;
  avatar.setPosition(-offset_y, -offset_x);

  std::puts("StackChan expression preview");
  std::puts("Cycling through 3 built-ins + 17 SVG-derived expressions.");
  std::puts("Close the SDL window or press Ctrl+C to stop.\n");
  showCurrentItem();
  last_change_ms = lgfx::millis();
}

void loop() {
  M5.update();
  const uint32_t now = lgfx::millis();
  if (now - last_change_ms >= kPreviewIntervalMs) {
    preview_index = (preview_index + 1) % kPreviewCount;
    showCurrentItem();
    last_change_ms = now;
  }
  lgfx::delay(10);
}
