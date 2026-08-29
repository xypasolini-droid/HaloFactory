/*
 * 智慧工厂工牌：ESP32-8048S043 竖屏适配版
 *
 * 仅使用 ESP32-8048S043 板载模块：
 *   1) 800x480 RGB 屏，界面旋转为 480x800
 *   2) GT911 电容触摸，I2C GPIO19/20，RST GPIO38
 *   3) 屏内业务状态灯，不连接任何外置按键或 LED
 *
 * 交互：
 *   任务页：点击“上报问题 / 下一任务 / 完工确认”
 *   问题页：点击问题卡，取消立即返回，长按确认按钮 1 秒提交
 *   品质页：点击合格/不合格，取消立即返回，长按确认按钮 1 秒提交
 *
 * 本固件是离线 MVP：屏幕触控、屏内状态和串口 JSON 闭环。
 * 不包含 Wi-Fi、MQTT/HTTP、NVS 离线队列、后台或真实身份认证。
 */

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Arduino_GFX_Library.h>

// -----------------------------------------------------------------------------
// 硬件配置：ESP32-8048S043 / ST7262-compatible RGB565
// -----------------------------------------------------------------------------

constexpr uint8_t PIN_TFT_BL = 2;
constexpr uint8_t PIN_TOUCH_SDA = 19;
constexpr uint8_t PIN_TOUCH_SCL = 20;
constexpr uint8_t PIN_TOUCH_RST = 38;

// 现场默认使用交通灯语义：正常生产绿灯，异常锁定红灯。
// 若必须沿用概念图的“待完成红灯”，可改成 true。
constexpr bool RED_MEANS_TASK_PENDING = false;

// 三模块台架演示时允许在锁定页长按 8 秒复位。
// 正式现场版本必须改成 false，由后台管理事件解除锁定。
constexpr bool DEMO_MODE = true;

// RGB 面板始终按物理 800x480 建立帧缓冲；rotation=1 后，Arduino_GFX
// 对业务绘图暴露 480x800 的竖屏逻辑坐标。如果整机安装方向相反，只需改为 3。
constexpr uint8_t DISPLAY_ROTATION = 1;
constexpr int16_t DISPLAY_LOGICAL_WIDTH = 480;
constexpr int16_t DISPLAY_LOGICAL_HEIGHT = 800;

Arduino_ESP32RGBPanel *rgbPanel = new Arduino_ESP32RGBPanel(
    40 /* DE */, 41 /* VSYNC */, 39 /* HSYNC */, 42 /* PCLK */,
    45 /* R0 */, 48 /* R1 */, 47 /* R2 */, 21 /* R3 */, 14 /* R4 */,
    5 /* G0 */, 6 /* G1 */, 7 /* G2 */, 15 /* G3 */, 16 /* G4 */, 4 /* G5 */,
    8 /* B0 */, 3 /* B1 */, 46 /* B2 */, 9 /* B3 */, 1 /* B4 */,
    1 /* C variant: HSYNC idle high */, 8 /* front porch */, 4 /* pulse */, 8 /* back porch */,
    1 /* C variant: VSYNC idle high */, 8 /* front porch */, 4 /* pulse */, 8 /* back porch */,
    1 /* PCLK active negative */, 12500000 /* pixel clock */,
    false /* RGB565 byte order */,
    0 /* DE idle low */, 0 /* PCLK idle low */, 0 /* no bounce buffer */);

Arduino_GFX *gfx = new Arduino_RGB_Display(
    800 /* width */,
    480 /* height */,
    rgbPanel,
    DISPLAY_ROTATION /* portrait: logical 480x800 */,
    true /* auto flush */);

// -----------------------------------------------------------------------------
// 视觉常量
// -----------------------------------------------------------------------------

constexpr uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue) {
  return static_cast<uint16_t>(((red & 0xF8) << 8) |
                               ((green & 0xFC) << 3) |
                               (blue >> 3));
}

constexpr uint16_t COLOR_BG = rgb565(5, 8, 10);
constexpr uint16_t COLOR_PANEL = rgb565(14, 18, 20);
constexpr uint16_t COLOR_PANEL_SELECTED = rgb565(42, 32, 8);
constexpr uint16_t COLOR_TEXT = rgb565(245, 243, 234);
constexpr uint16_t COLOR_MUTED = rgb565(145, 149, 144);
constexpr uint16_t COLOR_LINE = rgb565(88, 83, 69);
constexpr uint16_t COLOR_AMBER = rgb565(255, 176, 0);
constexpr uint16_t COLOR_RED = rgb565(255, 77, 79);
constexpr uint16_t COLOR_GREEN = rgb565(57, 211, 83);

bool displayReady = false;

// -----------------------------------------------------------------------------
// 业务模型
// -----------------------------------------------------------------------------

struct TaskCard {
  const char *taskId;
  const char *line1;
  const char *line2;
  uint16_t total;
  uint16_t remaining;
};

TaskCard tasks[] = {
    {"WO018-OP10", "备料", "上线", 10, 6},
    {"WO018-OP20", "装左", "门板", 20, 18},
    {"WO018-OP30", "锁紧", "螺栓", 12, 12},
    {"WO018-OP40", "终检", "装箱", 8, 8},
};

constexpr uint8_t TASK_COUNT = sizeof(tasks) / sizeof(tasks[0]);
uint8_t currentTaskIndex = 1; // 与概念图一致：上电先显示 2/4“装左门板”。

struct IssueOption {
  const char *label;
  const char *eventValue;
};

constexpr IssueOption ISSUE_OPTIONS[] = {
    {"有坏件", "DEFECTIVE_PART"},
    {"缺材料", "MATERIAL_SHORTAGE"},
    {"机器坏", "MACHINE_FAILURE"},
    {"有危险", "SAFETY_HAZARD"},
};

constexpr uint8_t ISSUE_COUNT = sizeof(ISSUE_OPTIONS) / sizeof(ISSUE_OPTIONS[0]);

enum class BadgeState : uint8_t {
  WORKING,
  REPORT_SELECT,
  QUALITY_SELECT,
  EXCEPTION_LOCKED,
  QUALITY_HOLD,
  COMPLETE,
};

enum class QualityChoice : uint8_t {
  NONE,
  PASS,
  FAIL,
};

BadgeState badgeState = BadgeState::WORKING;
QualityChoice qualityChoice = QualityChoice::NONE;
uint8_t selectedIssue = 0;
uint32_t stateSinceMs = 0;
uint32_t lastInteractionMs = 0;

uint32_t bootId = 0;
uint32_t eventSequence = 0;

// -----------------------------------------------------------------------------
// GT911 触摸输入
// -----------------------------------------------------------------------------

constexpr uint8_t GT911_ADDRESS_PRIMARY = 0x5D;
constexpr uint8_t GT911_ADDRESS_FALLBACK = 0x14;
constexpr uint16_t GT911_REG_CONFIG_VERSION = 0x8047;
constexpr uint16_t GT911_REG_PRODUCT_ID = 0x8140;
constexpr uint16_t GT911_REG_STATUS = 0x814E;
constexpr uint16_t GT911_REG_FIRST_POINT = 0x814F;

constexpr uint16_t TOUCH_EXPECTED_RAW_WIDTH = 800;
constexpr uint16_t TOUCH_EXPECTED_RAW_HEIGHT = 480;
// 实机若发现触摸方向与画面相反，只改这三个常量，不改页面坐标。
constexpr bool TOUCH_SWAP_RAW_AXES = false;
constexpr bool TOUCH_MIRROR_RAW_X = false;
constexpr bool TOUCH_MIRROR_RAW_Y = false;

constexpr uint32_t TOUCH_POLL_INTERVAL_MS = 12;
constexpr uint32_t TOUCH_RELEASE_TIMEOUT_MS = 300;
constexpr uint32_t TOUCH_HOLD_SAMPLE_FRESH_MS = 60;
constexpr uint32_t TOUCH_DEBOUNCE_MS = 80;
constexpr int16_t TOUCH_MOVE_TOLERANCE = 28;
constexpr uint32_t CONFIRM_HOLD_MS = 1000;
constexpr uint32_t DEMO_RESET_HOLD_MS = 8000;
constexpr uint32_t SELECT_TIMEOUT_MS = 10000;
// 完成灯保持 5 秒，避免 100 ms 级双闪在现场被错过。
constexpr uint32_t COMPLETE_SCREEN_MS = 5000;
constexpr uint32_t TRANSIENT_HINT_MS = 1300;

bool touchReady = false;
uint8_t touchAddress = 0;
char touchProductId[5] = "----";
uint8_t touchConfigVersion = 0;
uint16_t touchFirmwareVersion = 0;
uint16_t touchConfiguredWidth = 0;
uint16_t touchConfiguredHeight = 0;
uint16_t touchReportedWidth = 0;
uint16_t touchReportedHeight = 0;
uint16_t touchRawWidth = TOUCH_EXPECTED_RAW_WIDTH;
uint16_t touchRawHeight = TOUCH_EXPECTED_RAW_HEIGHT;

bool touchDown = false;
bool touchMoved = false;
bool touchLongTriggered = false;
bool touchInputSuppressedUntilRelease = false;
uint16_t touchStartRawX = 0;
uint16_t touchStartRawY = 0;
uint16_t touchCurrentRawX = 0;
uint16_t touchCurrentRawY = 0;
int16_t touchStartX = 0;
int16_t touchStartY = 0;
int16_t touchCurrentX = 0;
int16_t touchCurrentY = 0;
uint32_t touchPressedAtMs = 0;
uint32_t lastTouchSampleMs = 0;
uint32_t lastTouchReleaseMs = 0;
uint32_t nextTouchPollMs = 0;
uint32_t lastTouchLogMs = 0;
uint16_t lastLoggedRawX = 0xFFFF;
uint16_t lastLoggedRawY = 0xFFFF;
bool touchHoldEligibilityActive = false;
uint32_t touchHoldEligibleSinceMs = 0;
int8_t lastTouchHoldPercent = -1;

bool hintActive = false;
uint32_t hintUntilMs = 0;

// -----------------------------------------------------------------------------
// 前向声明
// -----------------------------------------------------------------------------

void renderCurrentPage();
void setBadgeState(BadgeState nextState);
void showTransientHint(const char *message, uint16_t color = COLOR_AMBER);
void renderStatusPill(bool force = false);
void drawTouchFooter();

// -----------------------------------------------------------------------------
// 名称与串口事件
// -----------------------------------------------------------------------------

const char *stateName(BadgeState state) {
  switch (state) {
    case BadgeState::WORKING:          return "WORKING";
    case BadgeState::REPORT_SELECT:    return "REPORT_SELECT";
    case BadgeState::QUALITY_SELECT:   return "QUALITY_SELECT";
    case BadgeState::EXCEPTION_LOCKED: return "EXCEPTION_LOCKED";
    case BadgeState::QUALITY_HOLD:     return "QUALITY_HOLD";
    case BadgeState::COMPLETE:         return "COMPLETE";
  }
  return "UNKNOWN";
}

const char *qualityName(QualityChoice choice) {
  switch (choice) {
    case QualityChoice::NONE: return "NONE";
    case QualityChoice::PASS: return "PASS";
    case QualityChoice::FAIL: return "FAIL";
  }
  return "UNKNOWN";
}

void emitEvent(const char *eventType,
               const char *payloadKey = nullptr,
               const char *payloadValue = nullptr) {
  ++eventSequence;
  const TaskCard &task = tasks[currentTaskIndex];

  char eventId[64];
  snprintf(eventId, sizeof(eventId), "BADGE-DEMO-01-%08lX-%06lu",
           static_cast<unsigned long>(bootId),
           static_cast<unsigned long>(eventSequence));

  Serial.printf(
      "{\"schema_version\":1,\"event_id\":\"%s\","
      "\"badge_id\":\"BADGE-DEMO-01\",\"worker_id\":\"W102\","
      "\"line_id\":\"ASSEMBLY-1\",\"task_id\":\"%s\","
      "\"task_index\":%u,\"task_count\":%u,\"remaining\":%u,"
      "\"event_type\":\"%s\",\"state\":\"%s\"",
      eventId,
      task.taskId,
      static_cast<unsigned>(currentTaskIndex + 1),
      static_cast<unsigned>(TASK_COUNT),
      static_cast<unsigned>(task.remaining),
      eventType,
      stateName(badgeState));

  if (payloadKey != nullptr && payloadValue != nullptr) {
    Serial.printf(",\"payload\":{\"%s\":\"%s\"}", payloadKey, payloadValue);
  }

  Serial.printf(
      ",\"boot_id\":\"%08lX\",\"device_sequence\":%lu,\"device_ms\":%lu}\n",
      static_cast<unsigned long>(bootId),
      static_cast<unsigned long>(eventSequence),
      static_cast<unsigned long>(millis()));
}

// -----------------------------------------------------------------------------
// 屏内业务状态灯
// -----------------------------------------------------------------------------

enum class LedMode : uint8_t {
  PENDING_RED_SOLID,
  WORKING_GREEN_SOLID,
  ISSUE_SELECT_YELLOW_SLOW,
  QUALITY_AWAIT_YELLOW_DOUBLE,
  LOCKED_RED_FAST,
  COMPLETE_GREEN_DOUBLE,
};

LedMode lastReportedLedMode = LedMode::PENDING_RED_SOLID;
BadgeState lastReportedLedState = BadgeState::WORKING;
QualityChoice lastReportedQualityChoice = QualityChoice::NONE;
bool ledModeHasBeenReported = false;
bool screenStatusRedOn = false;
bool screenStatusGreenOn = false;
LedMode lastDrawnLedMode = LedMode::PENDING_RED_SOLID;
bool lastDrawnStatusRedOn = false;
bool lastDrawnStatusGreenOn = false;
bool statusPillHasBeenDrawn = false;

LedMode currentLedMode() {
  switch (badgeState) {
    case BadgeState::WORKING:
      return RED_MEANS_TASK_PENDING
                 ? LedMode::PENDING_RED_SOLID
                 : LedMode::WORKING_GREEN_SOLID;

    case BadgeState::REPORT_SELECT:
      return LedMode::ISSUE_SELECT_YELLOW_SLOW;

    case BadgeState::QUALITY_SELECT:
      // 这里只表示“正在选择、尚未提交”。PASS/FAIL 必须长按确认后才改变业务灯。
      return LedMode::QUALITY_AWAIT_YELLOW_DOUBLE;

    case BadgeState::EXCEPTION_LOCKED:
    case BadgeState::QUALITY_HOLD:
      return LedMode::LOCKED_RED_FAST;

    case BadgeState::COMPLETE:
      return LedMode::COMPLETE_GREEN_DOUBLE;
  }

  return LedMode::LOCKED_RED_FAST;
}

const char *ledModeName(LedMode mode) {
  switch (mode) {
    case LedMode::PENDING_RED_SOLID:          return "PENDING_RED_SOLID";
    case LedMode::WORKING_GREEN_SOLID:       return "WORKING_GREEN_SOLID";
    case LedMode::ISSUE_SELECT_YELLOW_SLOW:  return "ISSUE_SELECT_YELLOW_SLOW";
    case LedMode::QUALITY_AWAIT_YELLOW_DOUBLE:return "QUALITY_AWAIT_YELLOW_DOUBLE";
    case LedMode::LOCKED_RED_FAST:           return "LOCKED_RED_FAST";
    case LedMode::COMPLETE_GREEN_DOUBLE:     return "COMPLETE_GREEN_DOUBLE";
  }
  return "UNKNOWN";
}

void updateScreenStatus(uint32_t nowMs) {
  const uint32_t elapsed = nowMs - stateSinceMs;
  const uint32_t phase = elapsed % 1000;
  const LedMode mode = currentLedMode();
  bool redOn = false;
  bool greenOn = false;

  // 每次流程灯态发生变化时只打印一行，便于把屏幕流程与业务事件对应起来。
  if (!ledModeHasBeenReported ||
      mode != lastReportedLedMode ||
      badgeState != lastReportedLedState ||
      qualityChoice != lastReportedQualityChoice) {
    Serial.printf("# FLOW state=%s quality=%s led=%s\n",
                  stateName(badgeState), qualityName(qualityChoice), ledModeName(mode));
    lastReportedLedMode = mode;
    lastReportedLedState = badgeState;
    lastReportedQualityChoice = qualityChoice;
    ledModeHasBeenReported = true;
  }

  switch (mode) {
    case LedMode::PENDING_RED_SOLID:
      redOn = true;
      break;

    case LedMode::WORKING_GREEN_SOLID:
      greenOn = true;
      break;

    case LedMode::ISSUE_SELECT_YELLOW_SLOW:
      // 黄灯慢闪：正在选择问题分类，尚未上报。
      redOn = greenOn = phase < 500;
      break;

    case LedMode::QUALITY_AWAIT_YELLOW_DOUBLE:
      // 黄灯双脉冲：等待选择合格/不合格。
      redOn = greenOn = phase < 120 || (phase >= 250 && phase < 370);
      break;

    case LedMode::LOCKED_RED_FAST:
      // 问题已上报或品质不合格：红灯快闪并保持锁定。
      redOn = ((elapsed / 200) % 2) == 0;
      break;

    case LedMode::COMPLETE_GREEN_DOUBLE:
      // 每秒绿灯双闪；完成页延长到 5 秒，现场可以清楚看到。
      greenOn = phase < 120 || (phase >= 250 && phase < 370);
      break;
  }

  const bool visualChanged = !statusPillHasBeenDrawn ||
                             mode != lastDrawnLedMode ||
                             redOn != lastDrawnStatusRedOn ||
                             greenOn != lastDrawnStatusGreenOn;
  screenStatusRedOn = redOn;
  screenStatusGreenOn = greenOn;
  if (visualChanged && displayReady) {
    renderStatusPill(true);
  }
}

// -----------------------------------------------------------------------------
// 文字与基础绘图
// -----------------------------------------------------------------------------

void useChineseFont(uint8_t scale, uint16_t color) {
  gfx->setFont(u8g2_font_unifont_h_chinese);
  gfx->setTextSize(scale, scale);
  gfx->setTextColor(color);
}

uint16_t textWidth(const char *text) {
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  gfx->getTextBounds(text, 0, 20, &x1, &y1, &width, &height);
  return width;
}

void drawText(int16_t x,
              int16_t baselineY,
              const char *text,
              uint16_t color = COLOR_TEXT,
              uint8_t scale = 1) {
  useChineseFont(scale, color);
  gfx->setCursor(x, baselineY);
  gfx->print(text);
}

void drawCenteredText(int16_t baselineY,
                      const char *text,
                      uint16_t color = COLOR_TEXT,
                      uint8_t scale = 1) {
  useChineseFont(scale, color);
  const uint16_t width = textWidth(text);
  int16_t x = (static_cast<int16_t>(gfx->width()) - static_cast<int16_t>(width)) / 2;
  if (x < 2) {
    x = 2;
  }
  gfx->setCursor(x, baselineY);
  gfx->print(text);
}

constexpr int16_t UI_MARGIN_X = 20;
constexpr int16_t UI_HEADER_BASELINE_Y = 48;
constexpr int16_t UI_HEADER_LINE_Y = 66;
constexpr int16_t UI_FOOTER_LINE_Y = 744;
constexpr int16_t UI_FOOTER_TEXT_Y = 773;
constexpr int16_t UI_HOLD_X = 20;
constexpr int16_t UI_HOLD_Y = 786;
constexpr int16_t UI_HOLD_WIDTH = DISPLAY_LOGICAL_WIDTH - (UI_HOLD_X * 2);
constexpr int16_t UI_HOLD_HEIGHT = 9;

constexpr int16_t STATUS_PILL_X = 348;
constexpr int16_t STATUS_PILL_Y = 12;
constexpr int16_t STATUS_PILL_W = 112;
constexpr int16_t STATUS_PILL_H = 42;

struct TouchRegion {
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
};

constexpr TouchRegion TASK_REPORT_BUTTON = {20, 430, 210, 88};
constexpr TouchRegion TASK_NEXT_BUTTON = {250, 430, 210, 88};
constexpr TouchRegion TASK_QUALITY_BUTTON = {20, 540, 440, 96};

constexpr TouchRegion ISSUE_CARD_REGIONS[ISSUE_COUNT] = {
    {20, 116, 208, 190},
    {252, 116, 208, 190},
    {20, 326, 208, 190},
    {252, 326, 208, 190},
};
constexpr TouchRegion REPORT_CANCEL_BUTTON = {20, 548, 210, 90};
constexpr TouchRegion REPORT_CONFIRM_BUTTON = {250, 548, 210, 90};

constexpr TouchRegion QUALITY_PASS_BUTTON = {20, 290, 204, 120};
constexpr TouchRegion QUALITY_FAIL_BUTTON = {256, 290, 204, 120};
constexpr TouchRegion QUALITY_CANCEL_BUTTON = {20, 526, 210, 100};
constexpr TouchRegion QUALITY_CONFIRM_BUTTON = {250, 526, 210, 100};

constexpr TouchRegion LOCK_RESET_BUTTON = {40, 548, 400, 104};

bool pointInRegion(int16_t x, int16_t y, const TouchRegion &region) {
  return x >= region.x && y >= region.y &&
         x < region.x + region.w && y < region.y + region.h;
}

bool tapStayedInRegion(int16_t startX,
                       int16_t startY,
                       int16_t endX,
                       int16_t endY,
                       const TouchRegion &region) {
  return pointInRegion(startX, startY, region) &&
         pointInRegion(endX, endY, region);
}

bool touchBusinessReady() {
  const bool rangeValid = touchRawWidth >= 100 && touchRawWidth <= 4096 &&
                          touchRawHeight >= 100 && touchRawHeight <= 4096;
  return displayReady && touchReady && rangeValid;
}

void drawActionButton(const TouchRegion &region,
                      const char *label,
                      uint16_t accent,
                      bool selected = false) {
  const uint16_t fill = selected
                            ? (accent == COLOR_GREEN
                                   ? rgb565(10, 57, 22)
                                   : (accent == COLOR_RED ? rgb565(61, 13, 15)
                                                          : COLOR_PANEL_SELECTED))
                            : COLOR_PANEL;
  const uint16_t border = selected ? accent : COLOR_LINE;
  const uint16_t labelColor = selected ? accent : COLOR_TEXT;

  gfx->fillRoundRect(region.x, region.y, region.w, region.h, 14, fill);
  gfx->drawRoundRect(region.x, region.y, region.w, region.h, 14, border);
  if (selected) {
    gfx->drawRoundRect(region.x + 4, region.y + 4,
                       region.w - 8, region.h - 8, 11, accent);
  }

  uint8_t labelScale = 2;
  useChineseFont(labelScale, labelColor);
  uint16_t labelWidth = textWidth(label);
  if (labelWidth > static_cast<uint16_t>(region.w - 16)) {
    labelScale = 1;
    useChineseFont(labelScale, labelColor);
    labelWidth = textWidth(label);
  }
  const int16_t labelX = region.x +
                         (region.w - static_cast<int16_t>(labelWidth)) / 2;
  const int16_t labelBaseline = region.y + region.h / 2 +
                                (labelScale == 2 ? 12 : 6);
  gfx->setCursor(labelX < region.x + 4 ? region.x + 4 : labelX,
                 labelBaseline);
  gfx->print(label);
}

const char *statusPillLabel(LedMode mode) {
  switch (mode) {
    case LedMode::PENDING_RED_SOLID:           return "待工";
    case LedMode::WORKING_GREEN_SOLID:        return "生产";
    case LedMode::ISSUE_SELECT_YELLOW_SLOW:   return "选择";
    case LedMode::QUALITY_AWAIT_YELLOW_DOUBLE:return "品质";
    case LedMode::LOCKED_RED_FAST:            return "锁定";
    case LedMode::COMPLETE_GREEN_DOUBLE:      return "完成";
  }
  return "状态";
}

void renderStatusPill(bool force) {
  if (!displayReady) {
    return;
  }

  const LedMode mode = currentLedMode();
  if (!force && statusPillHasBeenDrawn && mode == lastDrawnLedMode &&
      screenStatusRedOn == lastDrawnStatusRedOn &&
      screenStatusGreenOn == lastDrawnStatusGreenOn) {
    return;
  }

  uint16_t modeColor = COLOR_MUTED;
  if (mode == LedMode::WORKING_GREEN_SOLID ||
      mode == LedMode::COMPLETE_GREEN_DOUBLE) {
    modeColor = COLOR_GREEN;
  } else if (mode == LedMode::PENDING_RED_SOLID ||
             mode == LedMode::LOCKED_RED_FAST) {
    modeColor = COLOR_RED;
  } else {
    modeColor = COLOR_AMBER;
  }

  uint16_t dotColor = COLOR_LINE;
  if (screenStatusRedOn && screenStatusGreenOn) {
    dotColor = COLOR_AMBER;
  } else if (screenStatusRedOn) {
    dotColor = COLOR_RED;
  } else if (screenStatusGreenOn) {
    dotColor = COLOR_GREEN;
  }

  gfx->fillRoundRect(STATUS_PILL_X, STATUS_PILL_Y,
                     STATUS_PILL_W, STATUS_PILL_H, 14, COLOR_PANEL);
  gfx->drawRoundRect(STATUS_PILL_X, STATUS_PILL_Y,
                     STATUS_PILL_W, STATUS_PILL_H, 14, modeColor);
  gfx->fillCircle(STATUS_PILL_X + 21, STATUS_PILL_Y + 21, 8, dotColor);
  drawText(STATUS_PILL_X + 39, STATUS_PILL_Y + 27,
           statusPillLabel(mode), modeColor, 1);

  lastDrawnLedMode = mode;
  lastDrawnStatusRedOn = screenStatusRedOn;
  lastDrawnStatusGreenOn = screenStatusGreenOn;
  statusPillHasBeenDrawn = true;
}

void drawHeader(const char *leftTitle, const char *rightTitle) {
  drawText(UI_MARGIN_X, UI_HEADER_BASELINE_Y, leftTitle, COLOR_TEXT, 2);
  useChineseFont(2, COLOR_TEXT);
  const uint16_t rightWidth = textWidth(rightTitle);
  int16_t rightX = STATUS_PILL_X - 12 - static_cast<int16_t>(rightWidth);
  if (rightX < 230) {
    rightX = 230;
  }
  gfx->setCursor(rightX, UI_HEADER_BASELINE_Y);
  gfx->print(rightTitle);
  gfx->drawFastHLine(UI_MARGIN_X, UI_HEADER_LINE_Y,
                     gfx->width() - (UI_MARGIN_X * 2), COLOR_LINE);
  renderStatusPill(true);
}

void drawTouchFooter() {
  if (!displayReady) {
    return;
  }
  gfx->drawFastHLine(UI_MARGIN_X, UI_FOOTER_LINE_Y,
                     gfx->width() - (UI_MARGIN_X * 2), COLOR_LINE);
  drawCenteredText(UI_FOOTER_TEXT_Y,
                   touchReady ? "GT911 触摸已连接" : "触摸未连接",
                   touchReady ? COLOR_MUTED : COLOR_RED,
                   1);
}

void drawHoldProgress(uint8_t percent) {
  if (!displayReady) {
    return;
  }
  gfx->fillRect(UI_HOLD_X, UI_HOLD_Y, UI_HOLD_WIDTH, UI_HOLD_HEIGHT,
                COLOR_BG);
  gfx->drawRect(UI_HOLD_X, UI_HOLD_Y, UI_HOLD_WIDTH, UI_HOLD_HEIGHT,
                COLOR_LINE);
  const int16_t fillWidth = ((UI_HOLD_WIDTH - 2) * percent) / 100;
  if (fillWidth > 0) {
    gfx->fillRect(UI_HOLD_X + 1, UI_HOLD_Y + 1, fillWidth,
                  UI_HOLD_HEIGHT - 2, COLOR_AMBER);
  }
}

void clearHoldProgress() {
  if (displayReady) {
    gfx->fillRect(UI_HOLD_X, UI_HOLD_Y, UI_HOLD_WIDTH, UI_HOLD_HEIGHT,
                  COLOR_BG);
  }
}

// -----------------------------------------------------------------------------
// 各页面
// -----------------------------------------------------------------------------

void renderTaskPage() {
  const TaskCard &task = tasks[currentTaskIndex];
  char progressLabel[12];
  snprintf(progressLabel, sizeof(progressLabel), "%u/%u",
           static_cast<unsigned>(currentTaskIndex + 1),
           static_cast<unsigned>(TASK_COUNT));

  gfx->fillScreen(COLOR_BG);
  drawHeader("装配一线", progressLabel);
  drawText(28, 108, "现在做", COLOR_TEXT, 2);

  char taskText[32];
  snprintf(taskText, sizeof(taskText), "%s%s", task.line1, task.line2);
  drawCenteredText(205, taskText, COLOR_AMBER, 4);

  char remainingText[24];
  snprintf(remainingText, sizeof(remainingText), "还要 %u 件",
           static_cast<unsigned>(task.remaining));
  drawCenteredText(276, remainingText, COLOR_TEXT, 2);

  const uint16_t completed = task.total - task.remaining;
  constexpr int16_t progressX = 40;
  constexpr int16_t progressY = 316;
  constexpr int16_t progressW = 400;
  constexpr int16_t progressH = 20;
  const uint16_t progressWidth = task.total == 0
                                     ? 0
                                     : static_cast<uint16_t>(((progressW - 4UL) * completed) / task.total);
  gfx->drawRoundRect(progressX, progressY, progressW, progressH, 8, COLOR_LINE);
  if (progressWidth > 0) {
    gfx->fillRoundRect(progressX + 2, progressY + 2,
                       progressWidth, progressH - 4, 6, COLOR_AMBER);
  }

  char countText[24];
  snprintf(countText, sizeof(countText), "已完成 %u / %u",
           static_cast<unsigned>(completed),
           static_cast<unsigned>(task.total));
  drawCenteredText(376, countText, COLOR_MUTED, 2);

  drawActionButton(TASK_REPORT_BUTTON, "上报问题", COLOR_RED);
  drawActionButton(TASK_NEXT_BUTTON, "下一任务", COLOR_AMBER);
  drawActionButton(TASK_QUALITY_BUTTON, "完工确认", COLOR_GREEN);

  char taskIdText[32];
  snprintf(taskIdText, sizeof(taskIdText), "任务 %s", task.taskId);
  drawCenteredText(692, taskIdText, COLOR_MUTED, 1);
  drawTouchFooter();
  drawHoldProgress(0);
}

void drawIssueIcon(uint8_t index, int16_t centerX, int16_t centerY, uint16_t color) {
  switch (index) {
    case 0: // 断裂零件
      gfx->drawCircle(centerX, centerY, 34, color);
      gfx->drawCircle(centerX, centerY, 32, color);
      gfx->drawLine(centerX - 7, centerY - 31, centerX + 7, centerY - 12, color);
      gfx->drawLine(centerX + 7, centerY - 12, centerX - 9, centerY + 7, color);
      gfx->drawLine(centerX - 9, centerY + 7, centerX + 9, centerY + 31, color);
      break;

    case 1: // 缺料箱
      gfx->drawRect(centerX - 36, centerY - 24, 72, 56, color);
      gfx->drawLine(centerX - 36, centerY - 24, centerX, centerY - 42, color);
      gfx->drawLine(centerX + 36, centerY - 24, centerX, centerY - 42, color);
      gfx->drawLine(centerX, centerY - 42, centerX, centerY - 8, color);
      gfx->drawLine(centerX - 23, centerY + 12, centerX + 23, centerY + 12, color);
      break;

    case 2: // 机器/扳手
      gfx->drawCircle(centerX - 18, centerY - 18, 15, color);
      gfx->drawCircle(centerX - 18, centerY - 18, 13, color);
      gfx->drawLine(centerX - 7, centerY - 7, centerX + 32, centerY + 32, color);
      gfx->drawLine(centerX + 24, centerY + 36, centerX + 36, centerY + 24, color);
      gfx->fillCircle(centerX + 31, centerY + 31, 5, color);
      break;

    default: // 危险三角形
      gfx->drawTriangle(centerX, centerY - 43,
                        centerX - 44, centerY + 38,
                        centerX + 44, centerY + 38,
                        color);
      gfx->drawTriangle(centerX, centerY - 39,
                        centerX - 40, centerY + 35,
                        centerX + 40, centerY + 35,
                        color);
      gfx->drawFastVLine(centerX, centerY - 22, 32, color);
      gfx->fillCircle(centerX, centerY + 25, 5, color);
      break;
  }
}

void renderReportSelectPage() {
  char issuePosition[12];
  snprintf(issuePosition, sizeof(issuePosition), "%u/%u",
           static_cast<unsigned>(selectedIssue + 1),
           static_cast<unsigned>(ISSUE_COUNT));

  gfx->fillScreen(COLOR_BG);
  drawHeader("发现问题", issuePosition);
  drawCenteredText(101, "点击一个问题分类", COLOR_MUTED, 1);

  constexpr int16_t cardWidth = 208;
  constexpr int16_t cardHeight = 190;
  constexpr int16_t cardXs[2] = {20, 252};
  constexpr int16_t cardYs[2] = {116, 326};

  for (uint8_t i = 0; i < ISSUE_COUNT; ++i) {
    const int16_t x = cardXs[i % 2];
    const int16_t y = cardYs[i / 2];
    const bool selected = i == selectedIssue;
    const uint16_t border = selected ? COLOR_AMBER : COLOR_LINE;
    const uint16_t fill = selected ? COLOR_PANEL_SELECTED : COLOR_PANEL;
    const uint16_t iconColor = selected ? COLOR_AMBER : COLOR_TEXT;

    gfx->fillRoundRect(x, y, cardWidth, cardHeight, 14, fill);
    gfx->drawRoundRect(x, y, cardWidth, cardHeight, 14, border);
    if (selected) {
      gfx->drawRoundRect(x + 4, y + 4, cardWidth - 8, cardHeight - 8, 11, border);
    }
    drawIssueIcon(i, x + cardWidth / 2, y + 68, iconColor);

    useChineseFont(2, selected ? COLOR_AMBER : COLOR_TEXT);
    const uint16_t labelWidth = textWidth(ISSUE_OPTIONS[i].label);
    gfx->setCursor(x + (cardWidth - labelWidth) / 2, y + 164);
    gfx->print(ISSUE_OPTIONS[i].label);
  }

  drawActionButton(REPORT_CANCEL_BUTTON, "取消", COLOR_MUTED);
  drawActionButton(REPORT_CONFIRM_BUTTON, "长按1秒确认", COLOR_RED, true);
  drawCenteredText(688, "长按确认可避免误报", COLOR_MUTED, 1);
  drawTouchFooter();
  drawHoldProgress(0);
}

void renderQualitySelectPage() {
  gfx->fillScreen(COLOR_BG);
  drawHeader("完工确认", "品质");
  drawCenteredText(108, "点击选择检验结果", COLOR_MUTED, 1);

  const char *choiceText = "未选择";
  uint16_t choiceColor = COLOR_AMBER;
  const char *effectText = "尚未选择";

  if (qualityChoice == QualityChoice::PASS) {
    choiceText = "合格";
    choiceColor = COLOR_GREEN;
    effectText = "提交后完成当前一件";
  } else if (qualityChoice == QualityChoice::FAIL) {
    choiceText = "不合格";
    choiceColor = COLOR_RED;
    effectText = "提交后任务将被锁定";
  }

  drawCenteredText(198, choiceText, choiceColor, 3);
  drawCenteredText(248, effectText, choiceColor, 1);

  drawActionButton(QUALITY_PASS_BUTTON, "合格", COLOR_GREEN,
                   qualityChoice == QualityChoice::PASS);
  drawActionButton(QUALITY_FAIL_BUTTON, "不合格", COLOR_RED,
                   qualityChoice == QualityChoice::FAIL);

  drawActionButton(QUALITY_CANCEL_BUTTON, "取消", COLOR_MUTED);
  drawActionButton(QUALITY_CONFIRM_BUTTON, "长按1秒提交", COLOR_AMBER, true);
  drawCenteredText(680, "提交后将记录品质事件", COLOR_MUTED, 1);
  drawTouchFooter();
  drawHoldProgress(0);
}

void renderLockedPage() {
  const bool qualityHold = badgeState == BadgeState::QUALITY_HOLD;
  gfx->fillScreen(COLOR_BG);
  drawHeader(qualityHold ? "品质锁定" : "问题已上报", "锁定");

  constexpr int16_t centerX = DISPLAY_LOGICAL_WIDTH / 2;
  gfx->drawTriangle(centerX, 96, 154, 270, 326, 270, COLOR_RED);
  gfx->drawTriangle(centerX, 104, 162, 266, 318, 266, COLOR_RED);
  gfx->drawFastVLine(centerX, 146, 70, COLOR_RED);
  gfx->fillCircle(centerX, 240, 8, COLOR_RED);

  if (qualityHold) {
    drawCenteredText(370, "不合格", COLOR_RED, 3);
  } else {
    drawCenteredText(370, ISSUE_OPTIONS[selectedIssue].label, COLOR_RED, 3);
  }

  drawCenteredText(438, "等待组长处理", COLOR_TEXT, 2);
  drawCenteredText(482, "正式模式不可本机解除", COLOR_MUTED, 1);

  if (DEMO_MODE) {
    drawActionButton(LOCK_RESET_BUTTON, "长按8秒演示复位", COLOR_AMBER, true);
    drawCenteredText(692, "仅供台架演示", COLOR_MUTED, 1);
  } else {
    gfx->fillRoundRect(LOCK_RESET_BUTTON.x, LOCK_RESET_BUTTON.y,
                       LOCK_RESET_BUTTON.w, LOCK_RESET_BUTTON.h,
                       14, COLOR_PANEL);
    gfx->drawRoundRect(LOCK_RESET_BUTTON.x, LOCK_RESET_BUTTON.y,
                       LOCK_RESET_BUTTON.w, LOCK_RESET_BUTTON.h,
                       14, COLOR_LINE);
    drawCenteredText(610, "请等待后台处理", COLOR_MUTED, 2);
  }
  drawTouchFooter();
  drawHoldProgress(0);
}

void renderCompletePage() {
  const TaskCard &task = tasks[currentTaskIndex];
  gfx->fillScreen(COLOR_BG);
  drawHeader("装配一线", "完成");

  constexpr int16_t centerX = DISPLAY_LOGICAL_WIDTH / 2;
  constexpr int16_t centerY = 220;
  gfx->drawCircle(centerX, centerY, 108, COLOR_GREEN);
  gfx->drawCircle(centerX, centerY, 103, COLOR_GREEN);
  gfx->drawLine(174, 220, 222, 268, COLOR_GREEN);
  gfx->drawLine(222, 268, 310, 166, COLOR_GREEN);

  drawCenteredText(430, "本件完成", COLOR_GREEN, 4);

  char remainingText[24];
  snprintf(remainingText, sizeof(remainingText), "还要 %u 件",
           static_cast<unsigned>(task.remaining));
  drawCenteredText(505, remainingText, COLOR_TEXT, 2);

  if (task.remaining == 0) {
    drawCenteredText(558, "本任务全部完成", COLOR_GREEN, 2);
  } else {
    drawCenteredText(558, "即将返回当前任务", COLOR_MUTED, 2);
  }

  drawCenteredText(676, "完成状态双闪", COLOR_MUTED, 1);
  drawTouchFooter();
  drawHoldProgress(0);
}

void renderCurrentPage() {
  if (!displayReady) {
    return;
  }

  hintActive = false;
  statusPillHasBeenDrawn = false;
  switch (badgeState) {
    case BadgeState::WORKING:
      renderTaskPage();
      break;
    case BadgeState::REPORT_SELECT:
      renderReportSelectPage();
      break;
    case BadgeState::QUALITY_SELECT:
      renderQualitySelectPage();
      break;
    case BadgeState::EXCEPTION_LOCKED:
    case BadgeState::QUALITY_HOLD:
      renderLockedPage();
      break;
    case BadgeState::COMPLETE:
      renderCompletePage();
      break;
  }
}

void renderSplash() {
  if (!displayReady) {
    return;
  }
  gfx->fillScreen(COLOR_BG);
  statusPillHasBeenDrawn = false;
  renderStatusPill(true);
  drawCenteredText(190, "智慧工牌", COLOR_AMBER, 4);
  drawCenteredText(254, "竖屏触控任务终端", COLOR_TEXT, 2);
  drawCenteredText(322, "ESP32-8048S043", COLOR_MUTED, 2);
  drawCenteredText(366, "物理800x480  逻辑480x800", COLOR_MUTED, 1);

  constexpr int16_t panelX = 40;
  constexpr int16_t panelY = 420;
  constexpr int16_t panelW = 400;
  constexpr int16_t panelH = 210;
  gfx->fillRoundRect(panelX, panelY, panelW, panelH, 18, COLOR_PANEL);
  gfx->drawRoundRect(panelX, panelY, panelW, panelH, 18,
                     touchReady ? COLOR_GREEN : COLOR_RED);
  drawCenteredText(466,
                   touchReady ? "GT911 触摸已连接" : "触摸未连接",
                   touchReady ? COLOR_GREEN : COLOR_RED,
                   2);

  if (touchReady) {
    char productText[40];
    snprintf(productText, sizeof(productText), "Product %s  地址 0x%02X",
             touchProductId, static_cast<unsigned>(touchAddress));
    drawCenteredText(526, productText, COLOR_TEXT, 1);

    char rangeText[40];
    snprintf(rangeText, sizeof(rangeText), "配置 v%u  范围 %ux%u",
             static_cast<unsigned>(touchConfigVersion),
             static_cast<unsigned>(touchRawWidth),
             static_cast<unsigned>(touchRawHeight));
    drawCenteredText(574, rangeText, COLOR_MUTED, 1);
    drawCenteredText(610, "请点击页面按钮操作", COLOR_MUTED, 1);
  } else {
    drawCenteredText(535, "屏幕和串口仍可运行", COLOR_MUTED, 1);
    drawCenteredText(582, "检查GPIO19/20与RST38", COLOR_MUTED, 1);
  }
  drawTouchFooter();
  drawHoldProgress(0);
}

void showTransientHint(const char *message, uint16_t color) {
  Serial.printf("# UI %s\n", message);
  lastInteractionMs = millis();
  hintActive = true;
  hintUntilMs = lastInteractionMs + TRANSIENT_HINT_MS;

  if (!displayReady) {
    return;
  }

  constexpr int16_t hintX = 24;
  constexpr int16_t hintY = 662;
  constexpr int16_t hintWidth = 432;
  constexpr int16_t hintHeight = 76;
  gfx->fillRoundRect(hintX, hintY, hintWidth, hintHeight, 14, COLOR_PANEL);
  gfx->drawRoundRect(hintX, hintY, hintWidth, hintHeight, 14, color);
  drawCenteredText(710, message, color, 2);
  drawHoldProgress(0);
}

// -----------------------------------------------------------------------------
// 状态转换与业务动作
// -----------------------------------------------------------------------------

void setBadgeState(BadgeState nextState) {
  badgeState = nextState;
  stateSinceMs = millis();
  lastInteractionMs = stateSinceMs;
  hintActive = false;
  lastTouchHoldPercent = -1;
  touchHoldEligibilityActive = false;
  if (touchDown) {
    // 长按提交会在手指仍按着时切页；新页面必须等本次触摸释放。
    touchInputSuppressedUntilRelease = true;
  }
  // 先更新状态相位，再绘制页面，让页首状态点立即表达新状态。
  updateScreenStatus(stateSinceMs);
  renderCurrentPage();
}

void submitQualityPass() {
  TaskCard &task = tasks[currentTaskIndex];
  if (task.remaining == 0) {
    showTransientHint("任务已完成，请换任务", COLOR_AMBER);
    return;
  }

  --task.remaining;
  setBadgeState(BadgeState::COMPLETE);
  // All confirmed-result events describe the post-action state and remaining count.
  emitEvent("QUALITY_SUBMITTED", "result", "PASS");
  emitEvent("PART_COMPLETED");

  if (task.remaining == 0) {
    emitEvent("TASK_COMPLETED");
  }
}

void advanceToNextTask() {
  currentTaskIndex = (currentTaskIndex + 1) % TASK_COUNT;
  qualityChoice = QualityChoice::NONE;
  lastInteractionMs = millis();
  Serial.printf("# UI switched to task %u/%u (%s)\n",
                static_cast<unsigned>(currentTaskIndex + 1),
                static_cast<unsigned>(TASK_COUNT),
                tasks[currentTaskIndex].taskId);
  renderCurrentPage();
}

void handleTouchTap(int16_t startX,
                    int16_t startY,
                    int16_t endX,
                    int16_t endY) {
  if (!touchBusinessReady()) {
    Serial.println("# TOUCH business tap ignored: display/touch readiness gate is closed");
    return;
  }
  lastInteractionMs = millis();

  switch (badgeState) {
    case BadgeState::WORKING:
      if (tapStayedInRegion(startX, startY, endX, endY,
                            TASK_REPORT_BUTTON)) {
        if (tasks[currentTaskIndex].remaining == 0) {
          showTransientHint("任务已完成，请换任务", COLOR_AMBER);
        } else {
          selectedIssue = 0;
          setBadgeState(BadgeState::REPORT_SELECT);
        }
      } else if (tapStayedInRegion(startX, startY, endX, endY,
                                   TASK_NEXT_BUTTON)) {
        advanceToNextTask();
      } else if (tapStayedInRegion(startX, startY, endX, endY,
                                   TASK_QUALITY_BUTTON)) {
        if (tasks[currentTaskIndex].remaining == 0) {
          showTransientHint("任务已完成，请换任务", COLOR_AMBER);
        } else {
          qualityChoice = QualityChoice::NONE;
          setBadgeState(BadgeState::QUALITY_SELECT);
        }
      }
      break;

    case BadgeState::REPORT_SELECT:
      for (uint8_t i = 0; i < ISSUE_COUNT; ++i) {
        if (tapStayedInRegion(startX, startY, endX, endY,
                              ISSUE_CARD_REGIONS[i])) {
          selectedIssue = i;
          renderCurrentPage();
          return;
        }
      }
      if (tapStayedInRegion(startX, startY, endX, endY,
                            REPORT_CANCEL_BUTTON)) {
        qualityChoice = QualityChoice::NONE;
        setBadgeState(BadgeState::WORKING);
        Serial.println("# UI issue selection cancelled; no business event emitted");
      } else if (tapStayedInRegion(startX, startY, endX, endY,
                                   REPORT_CONFIRM_BUTTON)) {
        showTransientHint("请长按1秒确认上报", COLOR_AMBER);
      }
      break;

    case BadgeState::QUALITY_SELECT:
      if (tapStayedInRegion(startX, startY, endX, endY,
                            QUALITY_PASS_BUTTON)) {
        qualityChoice = QualityChoice::PASS;
        renderCurrentPage();
      } else if (tapStayedInRegion(startX, startY, endX, endY,
                                   QUALITY_FAIL_BUTTON)) {
        qualityChoice = QualityChoice::FAIL;
        renderCurrentPage();
      } else if (tapStayedInRegion(startX, startY, endX, endY,
                                   QUALITY_CANCEL_BUTTON)) {
        qualityChoice = QualityChoice::NONE;
        setBadgeState(BadgeState::WORKING);
        Serial.println("# UI quality selection cancelled; no business event emitted");
      } else if (tapStayedInRegion(startX, startY, endX, endY,
                                   QUALITY_CONFIRM_BUTTON)) {
        if (qualityChoice == QualityChoice::NONE) {
          showTransientHint("请先选择合格或不合格", COLOR_AMBER);
        } else {
          showTransientHint("请长按1秒确认提交", COLOR_AMBER);
        }
      }
      break;

    case BadgeState::EXCEPTION_LOCKED:
    case BadgeState::QUALITY_HOLD:
      if (tapStayedInRegion(startX, startY, endX, endY,
                            LOCK_RESET_BUTTON)) {
        showTransientHint(DEMO_MODE ? "请长按8秒演示复位" : "正式模式不可本机解除",
                          DEMO_MODE ? COLOR_AMBER : COLOR_RED);
      }
      break;

    case BadgeState::COMPLETE:
      break;
  }
}

uint32_t activeTouchHoldThreshold(uint32_t nowMs) {
  if (!touchBusinessReady() || !touchDown || touchMoved ||
      touchInputSuppressedUntilRelease ||
      touchLongTriggered ||
      (nowMs - lastTouchSampleMs) > TOUCH_HOLD_SAMPLE_FRESH_MS) {
    return 0;
  }

  switch (badgeState) {
    case BadgeState::REPORT_SELECT:
      return pointInRegion(touchStartX, touchStartY, REPORT_CONFIRM_BUTTON) &&
                     pointInRegion(touchCurrentX, touchCurrentY, REPORT_CONFIRM_BUTTON)
                 ? CONFIRM_HOLD_MS
                 : 0;

    case BadgeState::QUALITY_SELECT:
      return pointInRegion(touchStartX, touchStartY, QUALITY_CONFIRM_BUTTON) &&
                     pointInRegion(touchCurrentX, touchCurrentY, QUALITY_CONFIRM_BUTTON)
                 ? CONFIRM_HOLD_MS
                 : 0;

    case BadgeState::EXCEPTION_LOCKED:
    case BadgeState::QUALITY_HOLD:
      return DEMO_MODE && pointInRegion(touchStartX, touchStartY, LOCK_RESET_BUTTON) &&
                     pointInRegion(touchCurrentX, touchCurrentY, LOCK_RESET_BUTTON)
                 ? DEMO_RESET_HOLD_MS
                 : 0;

    case BadgeState::WORKING:
    case BadgeState::COMPLETE:
      return 0;
  }
  return 0;
}

void runConfirmedTouchAction() {
  if (!touchBusinessReady()) {
    Serial.println("# TOUCH business hold ignored: display/touch readiness gate is closed");
    return;
  }
  lastInteractionMs = millis();

  switch (badgeState) {
    case BadgeState::REPORT_SELECT:
      setBadgeState(BadgeState::EXCEPTION_LOCKED);
      emitEvent("EXCEPTION_RAISED", "category",
                ISSUE_OPTIONS[selectedIssue].eventValue);
      break;

    case BadgeState::QUALITY_SELECT:
      if (qualityChoice == QualityChoice::NONE) {
        showTransientHint("请先选择合格或不合格", COLOR_AMBER);
      } else if (qualityChoice == QualityChoice::PASS) {
        submitQualityPass();
      } else {
        setBadgeState(BadgeState::QUALITY_HOLD);
        emitEvent("QUALITY_SUBMITTED", "result", "FAIL");
      }
      break;

    case BadgeState::EXCEPTION_LOCKED:
    case BadgeState::QUALITY_HOLD:
      if (DEMO_MODE) {
        Serial.println("# DEMO_RESET local 8-second touch hold; not a production resolution event");
        qualityChoice = QualityChoice::NONE;
        setBadgeState(BadgeState::WORKING);
      }
      break;

    case BadgeState::WORKING:
    case BadgeState::COMPLETE:
      break;
  }
}

void processTouchHold(uint32_t nowMs) {
  const uint32_t threshold = activeTouchHoldThreshold(nowMs);
  if (threshold == 0) {
    if (lastTouchHoldPercent > 0) {
      drawHoldProgress(0);
    }
    lastTouchHoldPercent = 0;
    touchHoldEligibilityActive = false;
    return;
  }

  if (!touchHoldEligibilityActive) {
    touchHoldEligibilityActive = true;
    touchHoldEligibleSinceMs = nowMs;
  }
  const uint32_t heldMs = nowMs - touchHoldEligibleSinceMs;
  uint8_t percent = static_cast<uint8_t>((heldMs * 100UL) / threshold);
  if (percent > 100) {
    percent = 100;
  }
  // 5% 步进局部刷新，兼顾反馈平滑和 RGB 帧缓冲写入量。
  percent = static_cast<uint8_t>((percent / 5) * 5);
  if (static_cast<int8_t>(percent) != lastTouchHoldPercent) {
    lastTouchHoldPercent = static_cast<int8_t>(percent);
    drawHoldProgress(percent);
  }

  if (heldMs >= threshold) {
    touchLongTriggered = true;
    Serial.printf("# TOUCH_HOLD confirmed duration=%lu ms state=%s\n",
                  static_cast<unsigned long>(heldMs), stateName(badgeState));
    runConfirmedTouchAction();
  }
}

// -----------------------------------------------------------------------------
// GT911 I2C、坐标变换与轮询
// -----------------------------------------------------------------------------

bool gt911ReadRegister(uint8_t address,
                       uint16_t reg,
                       uint8_t *data,
                       uint8_t length) {
  Wire.beginTransmission(address);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) {
    return false;
  }

  const uint8_t received = Wire.requestFrom(address, length, true);
  if (received != length) {
    while (Wire.available()) {
      Wire.read();
    }
    return false;
  }
  for (uint8_t i = 0; i < length; ++i) {
    data[i] = Wire.read();
  }
  return true;
}

bool gt911WriteByte(uint8_t address, uint16_t reg, uint8_t value) {
  Wire.beginTransmission(address);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  Wire.write(value);
  return Wire.endTransmission(true) == 0;
}

bool gt911AddressResponds(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission(true) == 0;
}

bool gt911ReadValidatedProductId(uint8_t address, uint8_t productBytes[4]) {
  if (!gt911AddressResponds(address)) {
    return false;
  }

  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    if (gt911ReadRegister(address, GT911_REG_PRODUCT_ID, productBytes, 4) &&
        productBytes[0] == '9' &&
        productBytes[1] == '1' &&
        productBytes[2] == '1') {
      return true;
    }
    delay(12);
  }
  return false;
}

void resetGT911() {
  // INT 不作为输入或中断使用。板上地址绑带可能选择 0x5D 或 0x14，
  // 因此复位后按优先顺序探测两者。
  pinMode(PIN_TOUCH_RST, OUTPUT);
  digitalWrite(PIN_TOUCH_RST, LOW);
  delay(20);
  digitalWrite(PIN_TOUCH_RST, HIGH);
  delay(120);
}

void initializeTouch() {
  touchReady = false;
  touchAddress = 0;
  touchConfigVersion = 0;
  touchFirmwareVersion = 0;
  touchConfiguredWidth = 0;
  touchConfiguredHeight = 0;
  touchReportedWidth = 0;
  touchReportedHeight = 0;
  touchRawWidth = TOUCH_EXPECTED_RAW_WIDTH;
  touchRawHeight = TOUCH_EXPECTED_RAW_HEIGHT;
  memcpy(touchProductId, "----", sizeof(touchProductId));

  Wire.begin(PIN_TOUCH_SDA, PIN_TOUCH_SCL, 100000);
  Wire.setTimeOut(30);
  Serial.printf("# GT911 polling I2C 100kHz SDA=%u SCL=%u RST=%u; INT unused\n",
                static_cast<unsigned>(PIN_TOUCH_SDA),
                static_cast<unsigned>(PIN_TOUCH_SCL),
                static_cast<unsigned>(PIN_TOUCH_RST));

  // 两轮硬复位可覆盖上一次程序在控制器中留下的未清状态。
  uint8_t productBytes[4] = {};
  for (uint8_t attempt = 0; attempt < 2 && touchAddress == 0; ++attempt) {
    resetGT911();
    if (gt911ReadValidatedProductId(GT911_ADDRESS_PRIMARY, productBytes)) {
      touchAddress = GT911_ADDRESS_PRIMARY;
    } else if (gt911ReadValidatedProductId(GT911_ADDRESS_FALLBACK, productBytes)) {
      touchAddress = GT911_ADDRESS_FALLBACK;
    }
  }

  if (touchAddress == 0) {
    Serial.println("# WARNING Product ID '911' not found at 0x5D or 0x14; touch disabled");
    return;
  }

  for (uint8_t i = 0; i < sizeof(productBytes); ++i) {
    if (productBytes[i] == 0) {
      touchProductId[i] = '\0';
      break;
    }
    touchProductId[i] = productBytes[i] >= 32 && productBytes[i] <= 126
                            ? static_cast<char>(productBytes[i])
                            : '.';
  }
  touchProductId[4] = '\0';

  bool configuredRangeValid = false;
  bool configRead = false;
  uint8_t config[5] = {};
  for (uint8_t attempt = 0; attempt < 3 && !configuredRangeValid; ++attempt) {
    if (gt911ReadRegister(touchAddress, GT911_REG_CONFIG_VERSION,
                          config, sizeof(config))) {
      configRead = true;
      touchConfigVersion = config[0];
      touchConfiguredWidth = static_cast<uint16_t>(config[1]) |
                             (static_cast<uint16_t>(config[2]) << 8);
      touchConfiguredHeight = static_cast<uint16_t>(config[3]) |
                              (static_cast<uint16_t>(config[4]) << 8);
      configuredRangeValid = touchConfiguredWidth >= 100 &&
                             touchConfiguredWidth <= 4096 &&
                             touchConfiguredHeight >= 100 &&
                             touchConfiguredHeight <= 4096;
    }
    if (!configuredRangeValid) {
      delay(12);
    }
  }
  if (configRead) {
    Serial.printf("# GT911 config version=0x%02X Xmax=%u Ymax=%u valid=%s\n",
                  static_cast<unsigned>(touchConfigVersion),
                  static_cast<unsigned>(touchConfiguredWidth),
                  static_cast<unsigned>(touchConfiguredHeight),
                  configuredRangeValid ? "yes" : "no");
  } else {
    Serial.println("# WARNING GT911 config block 0x8047..0x804B read failed");
  }

  bool reportedRangeValid = false;
  bool firmwareBlockRead = false;
  uint8_t firmwareAndResolution[6] = {};
  for (uint8_t attempt = 0; attempt < 3 && !reportedRangeValid; ++attempt) {
    if (gt911ReadRegister(touchAddress, 0x8144,
                          firmwareAndResolution, sizeof(firmwareAndResolution))) {
      firmwareBlockRead = true;
      touchFirmwareVersion = static_cast<uint16_t>(firmwareAndResolution[0]) |
                             (static_cast<uint16_t>(firmwareAndResolution[1]) << 8);
      touchReportedWidth = static_cast<uint16_t>(firmwareAndResolution[2]) |
                           (static_cast<uint16_t>(firmwareAndResolution[3]) << 8);
      touchReportedHeight = static_cast<uint16_t>(firmwareAndResolution[4]) |
                            (static_cast<uint16_t>(firmwareAndResolution[5]) << 8);
      reportedRangeValid = touchReportedWidth >= 100 && touchReportedWidth <= 4096 &&
                           touchReportedHeight >= 100 && touchReportedHeight <= 4096;
    }
    if (!reportedRangeValid) {
      delay(12);
    }
  }
  if (firmwareBlockRead) {
    Serial.printf("# GT911 firmware=0x%04X reported_resolution=%ux%u valid=%s\n",
                  static_cast<unsigned>(touchFirmwareVersion),
                  static_cast<unsigned>(touchReportedWidth),
                  static_cast<unsigned>(touchReportedHeight),
                  reportedRangeValid ? "yes" : "no");
  } else {
    Serial.println("# WARNING GT911 firmware/resolution 0x8144..0x8149 read failed");
  }

  if (!reportedRangeValid && !configuredRangeValid) {
    Serial.println("# ERROR GT911 has no valid X/Y range after retries; touch disabled for safety");
    return;
  }

  if (reportedRangeValid) {
    touchRawWidth = touchReportedWidth;
    touchRawHeight = touchReportedHeight;
  } else if (configuredRangeValid) {
    touchRawWidth = touchConfiguredWidth;
    touchRawHeight = touchConfiguredHeight;
  }

  if (reportedRangeValid && configuredRangeValid &&
      (touchReportedWidth != touchConfiguredWidth ||
       touchReportedHeight != touchConfiguredHeight)) {
    Serial.printf("# WARNING GT911 resolution mismatch config=%ux%u reported=%ux%u; using reported\n",
                  static_cast<unsigned>(touchConfiguredWidth),
                  static_cast<unsigned>(touchConfiguredHeight),
                  static_cast<unsigned>(touchReportedWidth),
                  static_cast<unsigned>(touchReportedHeight));
  }
  Serial.printf("# GT911 effective raw range=%ux%u\n",
                static_cast<unsigned>(touchRawWidth),
                static_cast<unsigned>(touchRawHeight));

  // 清掉复位前可能残留的 data-ready 标志。
  if (!gt911WriteByte(touchAddress, GT911_REG_STATUS, 0)) {
    Serial.println("# ERROR GT911 initial status clear failed; touch disabled for safety");
    return;
  }
  touchReady = true;
  Serial.printf("# GT911 ready address=0x%02X product=%s\n",
                static_cast<unsigned>(touchAddress), touchProductId);
  Serial.printf("# TOUCH transform swapXY=%u mirrorX=%u mirrorY=%u rotation=%u\n",
                TOUCH_SWAP_RAW_AXES ? 1U : 0U,
                TOUCH_MIRROR_RAW_X ? 1U : 0U,
                TOUCH_MIRROR_RAW_Y ? 1U : 0U,
                static_cast<unsigned>(DISPLAY_ROTATION));
}

uint16_t normalizeRawCoordinate(uint16_t raw,
                                uint16_t configuredCount,
                                uint16_t physicalCount) {
  if (configuredCount < 2 || physicalCount < 2) {
    return 0;
  }
  const uint16_t clamped = raw >= configuredCount
                               ? configuredCount - 1
                               : raw;
  return static_cast<uint16_t>(
      (static_cast<uint32_t>(clamped) * (physicalCount - 1UL) +
       (configuredCount - 2UL) / 2UL) /
      (configuredCount - 1UL));
}

void mapRawTouchToLogical(uint16_t rawX,
                          uint16_t rawY,
                          uint16_t &physicalX,
                          uint16_t &physicalY,
                          int16_t &logicalX,
                          int16_t &logicalY) {
  if (TOUCH_SWAP_RAW_AXES) {
    physicalX = normalizeRawCoordinate(rawY, touchRawHeight,
                                       TOUCH_EXPECTED_RAW_WIDTH);
    physicalY = normalizeRawCoordinate(rawX, touchRawWidth,
                                       TOUCH_EXPECTED_RAW_HEIGHT);
  } else {
    physicalX = normalizeRawCoordinate(rawX, touchRawWidth,
                                       TOUCH_EXPECTED_RAW_WIDTH);
    physicalY = normalizeRawCoordinate(rawY, touchRawHeight,
                                       TOUCH_EXPECTED_RAW_HEIGHT);
  }

  if (TOUCH_MIRROR_RAW_X) {
    physicalX = (TOUCH_EXPECTED_RAW_WIDTH - 1) - physicalX;
  }
  if (TOUCH_MIRROR_RAW_Y) {
    physicalY = (TOUCH_EXPECTED_RAW_HEIGHT - 1) - physicalY;
  }

  // Arduino_GFX rotation=1: physical (800x480) -> logical (480x800).
  if (DISPLAY_ROTATION == 1) {
    logicalX = static_cast<int16_t>(physicalY);
    logicalY = static_cast<int16_t>((TOUCH_EXPECTED_RAW_WIDTH - 1) - physicalX);
  } else { // 支持整机反装时把 DISPLAY_ROTATION 改成 3。
    logicalX = static_cast<int16_t>((TOUCH_EXPECTED_RAW_HEIGHT - 1) - physicalY);
    logicalY = static_cast<int16_t>(physicalX);
  }

  if (logicalX < 0) logicalX = 0;
  if (logicalY < 0) logicalY = 0;
  if (logicalX >= DISPLAY_LOGICAL_WIDTH) logicalX = DISPLAY_LOGICAL_WIDTH - 1;
  if (logicalY >= DISPLAY_LOGICAL_HEIGHT) logicalY = DISPLAY_LOGICAL_HEIGHT - 1;
}

void logTouchCoordinate(uint8_t trackId,
                        uint16_t rawX,
                        uint16_t rawY,
                        uint16_t physicalX,
                        uint16_t physicalY,
                        int16_t logicalX,
                        int16_t logicalY) {
  Serial.printf("# TOUCH id=%u raw=(%u,%u) config=(%u,%u) reported=(%u,%u) effective=(%u,%u) physical=(%u,%u) logical=(%d,%d)\n",
                static_cast<unsigned>(trackId),
                static_cast<unsigned>(rawX),
                static_cast<unsigned>(rawY),
                static_cast<unsigned>(touchConfiguredWidth),
                static_cast<unsigned>(touchConfiguredHeight),
                static_cast<unsigned>(touchReportedWidth),
                static_cast<unsigned>(touchReportedHeight),
                static_cast<unsigned>(touchRawWidth),
                static_cast<unsigned>(touchRawHeight),
                static_cast<unsigned>(physicalX),
                static_cast<unsigned>(physicalY),
                logicalX, logicalY);
}

void acceptTouchPoint(uint8_t trackId,
                      uint16_t rawX,
                      uint16_t rawY,
                      uint32_t nowMs) {
  uint16_t physicalX = 0;
  uint16_t physicalY = 0;
  int16_t logicalX = 0;
  int16_t logicalY = 0;
  mapRawTouchToLogical(rawX, rawY, physicalX, physicalY, logicalX, logicalY);

  const uint16_t deltaLogX = rawX > lastLoggedRawX
                                 ? rawX - lastLoggedRawX
                                 : lastLoggedRawX - rawX;
  const uint16_t deltaLogY = rawY > lastLoggedRawY
                                 ? rawY - lastLoggedRawY
                                 : lastLoggedRawY - rawY;
  const bool shouldLog = !touchDown || deltaLogX >= 24 || deltaLogY >= 24 ||
                         (nowMs - lastTouchLogMs) >= 250;
  if (shouldLog) {
    logTouchCoordinate(trackId, rawX, rawY,
                       physicalX, physicalY, logicalX, logicalY);
    lastLoggedRawX = rawX;
    lastLoggedRawY = rawY;
    lastTouchLogMs = nowMs;
  }

  lastTouchSampleMs = nowMs;
  if (!touchDown) {
    touchDown = true;
    touchMoved = false;
    touchLongTriggered = false;
    touchHoldEligibilityActive = false;
    lastTouchHoldPercent = -1;
    touchInputSuppressedUntilRelease =
        lastTouchReleaseMs != 0 &&
        (nowMs - lastTouchReleaseMs) < TOUCH_DEBOUNCE_MS;
    touchStartRawX = rawX;
    touchStartRawY = rawY;
    touchStartX = logicalX;
    touchStartY = logicalY;
    touchPressedAtMs = nowMs;
    lastInteractionMs = nowMs;
  }

  touchCurrentRawX = rawX;
  touchCurrentRawY = rawY;
  touchCurrentX = logicalX;
  touchCurrentY = logicalY;

  const int16_t moveX = touchCurrentX > touchStartX
                            ? touchCurrentX - touchStartX
                            : touchStartX - touchCurrentX;
  const int16_t moveY = touchCurrentY > touchStartY
                            ? touchCurrentY - touchStartY
                            : touchStartY - touchCurrentY;
  if (moveX > TOUCH_MOVE_TOLERANCE || moveY > TOUCH_MOVE_TOLERANCE) {
    touchMoved = true;
  }
}

void cancelTouchGesture(uint32_t nowMs,
                        const char *reason,
                        bool suppressUntilExplicitRelease) {
  touchDown = false;
  touchMoved = false;
  touchLongTriggered = false;
  touchInputSuppressedUntilRelease = suppressUntilExplicitRelease;
  touchHoldEligibilityActive = false;
  lastTouchReleaseMs = nowMs;
  lastTouchHoldPercent = -1;
  clearHoldProgress();
  Serial.printf("# TOUCH gesture cancelled: %s%s\n",
                reason,
                suppressUntilExplicitRelease
                    ? "; waiting for explicit count=0 release"
                    : "");
}

void finishTouchRelease(uint32_t nowMs) {
  if (!touchDown) {
    return;
  }

  const uint32_t heldMs = nowMs - touchPressedAtMs;
  const bool suppress = touchInputSuppressedUntilRelease;
  const bool wasLong = touchLongTriggered;
  const bool wasMoved = touchMoved;
  const int16_t pressedX = touchStartX;
  const int16_t pressedY = touchStartY;
  const int16_t releasedX = touchCurrentX;
  const int16_t releasedY = touchCurrentY;

  touchDown = false;
  touchInputSuppressedUntilRelease = false;
  touchLongTriggered = false;
  touchMoved = false;
  touchHoldEligibilityActive = false;
  lastTouchReleaseMs = nowMs;
  lastTouchHoldPercent = -1;
  clearHoldProgress();

  Serial.printf("# TOUCH release logical=(%d,%d) held=%lu ms%s%s\n",
                releasedX, releasedY,
                static_cast<unsigned long>(heldMs),
                suppress ? " suppressed" : "",
                wasMoved ? " moved" : "");

  if (!touchBusinessReady() || suppress || wasLong || wasMoved ||
      heldMs < TOUCH_DEBOUNCE_MS) {
    return;
  }
  handleTouchTap(pressedX, pressedY, releasedX, releasedY);
}

void pollTouch(uint32_t nowMs) {
  if (!touchReady || static_cast<int32_t>(nowMs - nextTouchPollMs) < 0) {
    return;
  }
  nextTouchPollMs = nowMs + TOUCH_POLL_INTERVAL_MS;

  uint8_t status = 0;
  if (!gt911ReadRegister(touchAddress, GT911_REG_STATUS, &status, 1)) {
    if (touchDown && (nowMs - lastTouchSampleMs) >= TOUCH_RELEASE_TIMEOUT_MS) {
      cancelTouchGesture(nowMs, "I2C/status timeout", true);
    }
    return;
  }

  if ((status & 0x80) == 0) {
    if (touchDown && (nowMs - lastTouchSampleMs) >= TOUCH_RELEASE_TIMEOUT_MS) {
      cancelTouchGesture(nowMs, "data-ready timeout", true);
    }
    return;
  }

  const uint8_t pointCount = status & 0x0F;
  if (pointCount == 0) {
    if (touchDown) {
      finishTouchRelease(nowMs);
    } else if (touchInputSuppressedUntilRelease) {
      touchInputSuppressedUntilRelease = false;
      lastTouchReleaseMs = nowMs;
      lastTouchHoldPercent = -1;
      clearHoldProgress();
      Serial.println("# TOUCH explicit count=0 release; input suppression cleared");
    }
  } else if (pointCount == 1) {
    // 首点记录从 0x814F 的 track id 开始，坐标依次在 0x8150..0x8153。
    uint8_t point[8] = {};
    if (gt911ReadRegister(touchAddress, GT911_REG_FIRST_POINT,
                          point, sizeof(point))) {
      const uint8_t trackId = point[0];
      const uint16_t rawX = static_cast<uint16_t>(point[1]) |
                            (static_cast<uint16_t>(point[2]) << 8);
      const uint16_t rawY = static_cast<uint16_t>(point[3]) |
                            (static_cast<uint16_t>(point[4]) << 8);
      if (touchInputSuppressedUntilRelease && !touchDown) {
        // 超时/多指之后即使又看到一个点，也必须等明确 count=0，
        // 不能把同一根尚未抬起的手指当成新手势。
        if ((nowMs - lastTouchLogMs) >= 500) {
          uint16_t physicalX = 0;
          uint16_t physicalY = 0;
          int16_t logicalX = 0;
          int16_t logicalY = 0;
          mapRawTouchToLogical(rawX, rawY,
                               physicalX, physicalY, logicalX, logicalY);
          logTouchCoordinate(trackId, rawX, rawY,
                             physicalX, physicalY, logicalX, logicalY);
          lastTouchLogMs = nowMs;
        }
      } else {
        acceptTouchPoint(trackId, rawX, rawY, nowMs);
      }
    } else {
      Serial.println("# WARNING GT911 point read failed");
    }
  } else if (pointCount <= 5) {
    if (!touchInputSuppressedUntilRelease || touchDown) {
      cancelTouchGesture(nowMs, "multiple touch points", true);
      Serial.printf("# TOUCH multi-touch count=%u rejected for business safety\n",
                    static_cast<unsigned>(pointCount));
    }
  } else {
    if (!touchInputSuppressedUntilRelease || touchDown) {
      cancelTouchGesture(nowMs, "invalid touch point count", true);
      Serial.printf("# WARNING GT911 invalid point count=%u\n",
                    static_cast<unsigned>(pointCount));
    }
  }

  // 每个 data-ready 包都必须清零，否则控制器不会发布下一帧。
  if (!gt911WriteByte(touchAddress, GT911_REG_STATUS, 0)) {
    Serial.println("# WARNING GT911 status clear failed");
    if (!touchInputSuppressedUntilRelease || touchDown) {
      cancelTouchGesture(nowMs, "status clear failure", true);
    }
  }
}

// -----------------------------------------------------------------------------
// Arduino 入口
// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println();
  Serial.println("===== Smart Factory Badge / ESP32-8048S043 =====");
  Serial.println("# Display: physical RGB565 800x480, logical portrait 480x800, rotation=1");
  Serial.println("# Display timing: C variant PCLK=12.5MHz, backlight=GPIO2");
  Serial.println("# Input: onboard GT911 polling; no external button or LED required");

  // 背光先保持关闭，首帧画完后再打开，减少白屏闪烁。
  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, LOW);

  // 触摸初始化与显示互不依赖；探测失败时页面会明确提示，但不会阻塞启动。
  initializeTouch();

  constexpr size_t requiredFramebufferBytes = 800UL * 480UL * 2UL;
  const size_t psramBytes = ESP.getPsramSize();
  Serial.printf("# PSRAM found=%s size=%u bytes required=%u bytes\n",
                psramFound() ? "yes" : "no",
                static_cast<unsigned>(psramBytes),
                static_cast<unsigned>(requiredFramebufferBytes));

  if (!psramFound() || psramBytes < requiredFramebufferBytes) {
    displayReady = false;
    Serial.println("# ERROR OPI PSRAM is unavailable or too small; RGB display not started");
  } else {
    displayReady = gfx->begin();
  }

  if (displayReady) {
    gfx->setUTF8Print(true);
    if (gfx->width() != DISPLAY_LOGICAL_WIDTH ||
        gfx->height() != DISPLAY_LOGICAL_HEIGHT) {
      Serial.printf("# WARNING display logical size is %dx%d, expected %dx%d\n",
                    gfx->width(), gfx->height(),
                    DISPLAY_LOGICAL_WIDTH, DISPLAY_LOGICAL_HEIGHT);
    }
    renderSplash();
    digitalWrite(PIN_TFT_BL, HIGH);
    Serial.printf("# DISPLAY ready logical=%dx%d backlight=on\n",
                  gfx->width(), gfx->height());
  } else {
    Serial.println("# ERROR gfx->begin() failed; touch/Serial state machine will still run");
  }

  delay(850);

  bootId = esp_random();
  stateSinceMs = millis();
  lastInteractionMs = stateSinceMs;
  emitEvent("DEVICE_BOOT");
  setBadgeState(BadgeState::WORKING);
}

void loop() {
  const uint32_t inputNowMs = millis();
  pollTouch(inputNowMs);
  processTouchHold(inputNowMs);

  // 触摸处理可能在本轮调用 setBadgeState() 并写入更晚的 stateSinceMs；
  // 重新取时可避免无符号下溢导致状态点相位或页面超时跳变。
  const uint32_t nowMs = millis();
  updateScreenStatus(nowMs);

  if (hintActive && static_cast<int32_t>(nowMs - hintUntilMs) >= 0) {
    hintActive = false;
    renderCurrentPage();
  }

  if ((badgeState == BadgeState::REPORT_SELECT ||
       badgeState == BadgeState::QUALITY_SELECT) &&
      !touchDown &&
      (nowMs - lastInteractionMs) >= SELECT_TIMEOUT_MS) {
    qualityChoice = QualityChoice::NONE;
    setBadgeState(BadgeState::WORKING);
    Serial.println("# UI selection timed out; no business event emitted");
  }

  if (badgeState == BadgeState::COMPLETE &&
      !touchDown &&
      (nowMs - stateSinceMs) >= COMPLETE_SCREEN_MS) {
    if (tasks[currentTaskIndex].remaining == 0 && DEMO_MODE) {
      currentTaskIndex = (currentTaskIndex + 1) % TASK_COUNT;
    }
    qualityChoice = QualityChoice::NONE;
    setBadgeState(BadgeState::WORKING);
  }

  delay(1);
}
