/*
 * 智慧工厂工牌：三模块本地验证版
 *
 * 仅使用：
 *   1) ESP32-1732S019（板载 170x320 ST7789）
 *   2) 用户实物的共阴 R/G/B/- LED 模块（业务只使用红、绿，蓝色常灭）
 *   3) KY-004 按键
 *
 * 用户本轮确认的外接线：
 *   LED R -> GPIO15, G -> GPIO16, B -> GPIO17, - -> GND
 *   KY-004 S -> GPIO2, + -> 3V3, - -> GND
 *
 * 交互：
 *   任务页短按：进入问题上报
 *   任务页双击：下一任务（替代概念图中的滑动）
 *   任务页长按 2 秒：进入完工品质确认
 *   选择页短按：切换选项；双击：取消；长按 2 秒：提交
 *
 * 本固件是离线 MVP：屏幕、按键、红绿状态和串口 JSON 闭环。
 * 不包含 Wi-Fi、MQTT/HTTP、NVS 离线队列、后台或真实身份认证。
 */

#include <Arduino.h>
#include <U8g2lib.h>
#include <Arduino_GFX_Library.h>

// -----------------------------------------------------------------------------
// 硬件配置：继承用户已经跑通的 PlatformIO 测试工程
// -----------------------------------------------------------------------------

constexpr uint8_t PIN_TFT_MOSI = 13;
constexpr uint8_t PIN_TFT_SCLK = 12;
constexpr uint8_t PIN_TFT_DC = 11;
constexpr uint8_t PIN_TFT_CS = 10;
constexpr uint8_t PIN_TFT_RST = 1;
constexpr uint8_t PIN_TFT_BL = 14;

constexpr uint8_t PIN_LED_RED = 15;
constexpr uint8_t PIN_LED_GREEN = 16;
constexpr uint8_t PIN_LED_BLUE = 17;
constexpr uint8_t PIN_BUTTON = 2;

// 用户把 LED 公共负极接到 GND，因此 HIGH 点亮颜色脚。
constexpr bool LED_COMMON_ANODE = false;

// 0/2 都是 170x320 竖屏；如果实机上下颠倒，只把 0 改成 2。
constexpr uint8_t TFT_ROTATION = 0;

// 现场默认使用交通灯语义：正常生产绿灯，异常锁定红灯。
// 若必须沿用概念图的“待完成红灯”，可改成 true。
constexpr bool RED_MEANS_TASK_PENDING = false;

// 三模块台架演示时允许在锁定页长按 8 秒复位。
// 正式现场版本必须改成 false，由后台管理事件解除锁定。
constexpr bool DEMO_MODE = true;

Arduino_DataBus *displayBus = new Arduino_ESP32SPI(
    PIN_TFT_DC,
    PIN_TFT_CS,
    PIN_TFT_SCLK,
    PIN_TFT_MOSI,
    GFX_NOT_DEFINED);

Arduino_GFX *gfx = new Arduino_ST7789(
    displayBus,
    PIN_TFT_RST,
    TFT_ROTATION,
    true,   // IPS
    170,    // native width
    320,    // native height
    35, 0,  // rotation 0/1 offsets
    35, 0); // rotation 2/3 offsets

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
// 按键识别
// -----------------------------------------------------------------------------

constexpr uint32_t DEBOUNCE_MS = 35;
constexpr uint32_t MIN_SHORT_PRESS_MS = 50;
constexpr uint32_t MAX_SHORT_PRESS_MS = 700;
constexpr uint32_t DOUBLE_CLICK_WINDOW_MS = 350;
constexpr uint32_t LONG_PRESS_MS = 2000;
constexpr uint32_t DEMO_RESET_HOLD_MS = 8000;
constexpr uint32_t SELECT_TIMEOUT_MS = 10000;
// 完成灯保持 5 秒，避免 100 ms 级双闪在现场被错过。
constexpr uint32_t COMPLETE_SCREEN_MS = 5000;
constexpr uint32_t TRANSIENT_HINT_MS = 1300;

bool lastRawPressed = false;
bool stablePressed = false;
bool longPressTriggered = false;
bool blockLongForCurrentPress = false;
uint32_t rawChangedAtMs = 0;
uint32_t pressedAtMs = 0;
int8_t lastHoldQuarter = -1;

bool clickPending = false;
uint32_t firstClickReleasedAtMs = 0;

bool hintActive = false;
uint32_t hintUntilMs = 0;

// -----------------------------------------------------------------------------
// 前向声明
// -----------------------------------------------------------------------------

void renderCurrentPage();
void setBadgeState(BadgeState nextState);
void showTransientHint(const char *message, uint16_t color = COLOR_AMBER);

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
// LED
// -----------------------------------------------------------------------------

uint8_t ledLevel(bool on) {
  const bool physicalHigh = LED_COMMON_ANODE ? !on : on;
  return physicalHigh ? HIGH : LOW;
}

void setBusinessLed(bool redOn, bool greenOn) {
  digitalWrite(PIN_LED_RED, ledLevel(redOn));
  digitalWrite(PIN_LED_GREEN, ledLevel(greenOn));
  digitalWrite(PIN_LED_BLUE, ledLevel(false)); // 本方案只使用红、绿两色。
}

void initializeLedPins() {
  // Arduino-ESP32 3.x 要求先把引脚配置为 GPIO 输出，再调用 digitalWrite。
  // 对本项目的共阴极 LED，OUTPUT 的默认低电平也会保持灭灯。
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_BLUE, OUTPUT);
  setBusinessLed(false, false);
}

void runLedSelfTest() {
  // 便于第一次上电确认 GPIO15 确实是红、GPIO16 确实是绿。
  setBusinessLed(true, false);
  delay(220);
  setBusinessLed(false, true);
  delay(220);
  setBusinessLed(false, false);
}

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

void updateBusinessLed(uint32_t nowMs) {
  const uint32_t elapsed = nowMs - stateSinceMs;
  const uint32_t phase = elapsed % 1000;
  const LedMode mode = currentLedMode();
  bool redOn = false;
  bool greenOn = false;

  // 每次流程灯态发生变化时只打印一行，便于把屏幕流程与实物灯对应起来。
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

  setBusinessLed(redOn, greenOn);
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

void drawHeader(const char *leftTitle, const char *rightTitle) {
  drawText(8, 28, leftTitle, COLOR_TEXT, 1);
  useChineseFont(1, COLOR_TEXT);
  const uint16_t rightWidth = textWidth(rightTitle);
  gfx->setCursor(gfx->width() - rightWidth - 8, 28);
  gfx->print(rightTitle);
  gfx->drawFastHLine(6, 41, gfx->width() - 12, COLOR_LINE);
}

void drawHoldProgress(uint8_t percent) {
  if (!displayReady) {
    return;
  }
  constexpr int16_t x = 8;
  constexpr int16_t y = 313;
  constexpr int16_t width = 154;
  constexpr int16_t height = 5;
  gfx->fillRect(x, y, width, height, COLOR_BG);
  gfx->drawRect(x, y, width, height, COLOR_LINE);
  const int16_t fillWidth = ((width - 2) * percent) / 100;
  if (fillWidth > 0) {
    gfx->fillRect(x + 1, y + 1, fillWidth, height - 2, COLOR_AMBER);
  }
}

void clearHoldProgress() {
  if (displayReady) {
    gfx->fillRect(8, 313, 154, 5, COLOR_BG);
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
  drawText(9, 68, "现在做", COLOR_TEXT, 1);

  drawCenteredText(116, task.line1, COLOR_AMBER, 3);
  drawCenteredText(166, task.line2, COLOR_AMBER, 3);

  drawText(23, 210, "还要", COLOR_TEXT, 1);
  char remainingText[8];
  snprintf(remainingText, sizeof(remainingText), "%u",
           static_cast<unsigned>(task.remaining));
  drawText(72, 216, remainingText, COLOR_AMBER, 2);
  drawText(128, 210, "件", COLOR_TEXT, 1);

  const uint16_t completed = task.total - task.remaining;
  const uint16_t progressWidth = task.total == 0
                                     ? 0
                                     : static_cast<uint16_t>((150UL * completed) / task.total);
  gfx->drawRoundRect(10, 230, 150, 8, 3, COLOR_LINE);
  if (progressWidth > 2) {
    gfx->fillRoundRect(10, 230, progressWidth, 8, 3, COLOR_AMBER);
  }

  char countText[24];
  snprintf(countText, sizeof(countText), "已完成 %u / %u",
           static_cast<unsigned>(completed),
           static_cast<unsigned>(task.total));
  drawCenteredText(258, countText, COLOR_MUTED, 1);

  gfx->drawFastHLine(6, 269, gfx->width() - 12, COLOR_LINE);
  drawCenteredText(290, "短按上报  双击换任务", COLOR_TEXT, 1);
  drawCenteredText(309, "长按2秒  完工确认", COLOR_AMBER, 1);
  drawHoldProgress(0);
}

void drawIssueIcon(uint8_t index, int16_t centerX, int16_t centerY, uint16_t color) {
  switch (index) {
    case 0: // 断裂零件
      gfx->drawCircle(centerX, centerY, 15, color);
      gfx->drawLine(centerX - 3, centerY - 14, centerX + 3, centerY - 5, color);
      gfx->drawLine(centerX + 3, centerY - 5, centerX - 4, centerY + 3, color);
      gfx->drawLine(centerX - 4, centerY + 3, centerX + 4, centerY + 14, color);
      break;

    case 1: // 缺料箱
      gfx->drawRect(centerX - 16, centerY - 11, 32, 25, color);
      gfx->drawLine(centerX - 16, centerY - 11, centerX, centerY - 19, color);
      gfx->drawLine(centerX + 16, centerY - 11, centerX, centerY - 19, color);
      gfx->drawLine(centerX, centerY - 19, centerX, centerY - 4, color);
      gfx->drawLine(centerX - 10, centerY + 5, centerX + 10, centerY + 5, color);
      break;

    case 2: // 机器/扳手
      gfx->drawCircle(centerX - 8, centerY - 8, 7, color);
      gfx->drawLine(centerX - 3, centerY - 3, centerX + 14, centerY + 14, color);
      gfx->drawLine(centerX + 11, centerY + 15, centerX + 16, centerY + 10, color);
      gfx->fillCircle(centerX + 14, centerY + 13, 2, color);
      break;

    default: // 危险三角形
      gfx->drawTriangle(centerX, centerY - 18,
                        centerX - 18, centerY + 15,
                        centerX + 18, centerY + 15,
                        color);
      gfx->drawFastVLine(centerX, centerY - 8, 13, color);
      gfx->fillCircle(centerX, centerY + 10, 2, color);
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
  drawCenteredText(59, "短按选择  长按确认", COLOR_MUTED, 1);

  constexpr int16_t cardWidth = 73;
  constexpr int16_t cardHeight = 82;
  constexpr int16_t cardXs[2] = {8, 89};
  constexpr int16_t cardYs[2] = {70, 159};

  for (uint8_t i = 0; i < ISSUE_COUNT; ++i) {
    const int16_t x = cardXs[i % 2];
    const int16_t y = cardYs[i / 2];
    const bool selected = i == selectedIssue;
    const uint16_t border = selected ? COLOR_AMBER : COLOR_LINE;
    const uint16_t fill = selected ? COLOR_PANEL_SELECTED : COLOR_PANEL;
    const uint16_t iconColor = selected ? COLOR_AMBER : COLOR_TEXT;

    gfx->fillRoundRect(x, y, cardWidth, cardHeight, 6, fill);
    gfx->drawRoundRect(x, y, cardWidth, cardHeight, 6, border);
    if (selected) {
      gfx->drawRoundRect(x + 2, y + 2, cardWidth - 4, cardHeight - 4, 5, border);
    }
    drawIssueIcon(i, x + cardWidth / 2, y + 29, iconColor);

    useChineseFont(1, selected ? COLOR_AMBER : COLOR_TEXT);
    const uint16_t labelWidth = textWidth(ISSUE_OPTIONS[i].label);
    gfx->setCursor(x + (cardWidth - labelWidth) / 2, y + 70);
    gfx->print(ISSUE_OPTIONS[i].label);
  }

  gfx->drawFastHLine(6, 252, gfx->width() - 12, COLOR_LINE);
  drawCenteredText(275, "双击取消并返回", COLOR_MUTED, 1);
  drawCenteredText(302, "长按2秒  确认上报", COLOR_RED, 1);
  drawHoldProgress(0);
}

void renderQualitySelectPage() {
  gfx->fillScreen(COLOR_BG);
  drawHeader("完工确认", "品质");
  drawCenteredText(69, "当前结果", COLOR_MUTED, 1);

  const char *choiceText = "未选择";
  uint16_t choiceColor = COLOR_AMBER;
  const char *effectText = "短按选择合格或不合格";

  if (qualityChoice == QualityChoice::PASS) {
    choiceText = "合格";
    choiceColor = COLOR_GREEN;
    effectText = "提交后完成当前一件";
  } else if (qualityChoice == QualityChoice::FAIL) {
    choiceText = "不合格";
    choiceColor = COLOR_RED;
    effectText = "提交后任务将被锁定";
  }

  drawCenteredText(150, choiceText, choiceColor, 3);
  drawCenteredText(194, effectText, choiceColor, 1);

  gfx->fillRoundRect(19, 215, 60, 32, 5,
                     qualityChoice == QualityChoice::PASS ? rgb565(10, 57, 22) : COLOR_PANEL);
  gfx->drawRoundRect(19, 215, 60, 32, 5,
                     qualityChoice == QualityChoice::PASS ? COLOR_GREEN : COLOR_LINE);
  drawText(33, 237, "合格",
           qualityChoice == QualityChoice::PASS ? COLOR_GREEN : COLOR_MUTED, 1);

  gfx->fillRoundRect(91, 215, 60, 32, 5,
                     qualityChoice == QualityChoice::FAIL ? rgb565(61, 13, 15) : COLOR_PANEL);
  gfx->drawRoundRect(91, 215, 60, 32, 5,
                     qualityChoice == QualityChoice::FAIL ? COLOR_RED : COLOR_LINE);
  drawText(97, 237, "不合格",
           qualityChoice == QualityChoice::FAIL ? COLOR_RED : COLOR_MUTED, 1);

  gfx->drawFastHLine(6, 259, gfx->width() - 12, COLOR_LINE);
  drawCenteredText(280, "短按切换  双击取消", COLOR_MUTED, 1);
  drawCenteredText(304, "长按2秒  确认提交", COLOR_AMBER, 1);
  drawHoldProgress(0);
}

void renderLockedPage() {
  const bool qualityHold = badgeState == BadgeState::QUALITY_HOLD;
  gfx->fillScreen(COLOR_BG);
  drawHeader(qualityHold ? "品质锁定" : "问题已上报", "锁定");

  gfx->drawTriangle(85, 66, 43, 142, 127, 142, COLOR_RED);
  gfx->drawFastVLine(85, 87, 29, COLOR_RED);
  gfx->fillCircle(85, 128, 3, COLOR_RED);

  if (qualityHold) {
    drawCenteredText(181, "不合格", COLOR_RED, 2);
  } else {
    drawCenteredText(181, ISSUE_OPTIONS[selectedIssue].label, COLOR_RED, 2);
  }

  drawCenteredText(219, "等待组长处理", COLOR_TEXT, 1);
  drawCenteredText(244, "本机不能正式解除", COLOR_MUTED, 1);
  gfx->drawFastHLine(6, 263, gfx->width() - 12, COLOR_LINE);

  if (DEMO_MODE) {
    drawCenteredText(287, "仅演示：长按8秒复位", COLOR_AMBER, 1);
  } else {
    drawCenteredText(287, "请等待后台处理", COLOR_MUTED, 1);
  }
  drawHoldProgress(0);
}

void renderCompletePage() {
  const TaskCard &task = tasks[currentTaskIndex];
  gfx->fillScreen(COLOR_BG);
  drawHeader("装配一线", "完成");

  gfx->drawCircle(85, 106, 43, COLOR_GREEN);
  gfx->drawCircle(85, 106, 41, COLOR_GREEN);
  gfx->drawLine(62, 106, 79, 123, COLOR_GREEN);
  gfx->drawLine(79, 123, 111, 86, COLOR_GREEN);

  drawCenteredText(181, "本件完成", COLOR_GREEN, 2);

  char remainingText[24];
  snprintf(remainingText, sizeof(remainingText), "还要 %u 件",
           static_cast<unsigned>(task.remaining));
  drawCenteredText(221, remainingText, COLOR_TEXT, 1);

  if (task.remaining == 0) {
    drawCenteredText(251, "本任务全部完成", COLOR_GREEN, 1);
  } else {
    drawCenteredText(251, "即将返回当前任务", COLOR_MUTED, 1);
  }

  drawCenteredText(293, "绿灯双闪  完成确认", COLOR_MUTED, 1);
  drawHoldProgress(0);
}

void renderCurrentPage() {
  if (!displayReady) {
    return;
  }

  hintActive = false;
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
  drawCenteredText(116, "智慧工牌", COLOR_AMBER, 2);
  drawCenteredText(157, "三模块本地验证", COLOR_TEXT, 1);
  drawCenteredText(206, "R15 G16 B17  KEY2", COLOR_MUTED, 1);
  drawCenteredText(244, "红灯后绿灯自检", COLOR_MUTED, 1);
}

void showTransientHint(const char *message, uint16_t color) {
  Serial.printf("# UI %s\n", message);
  lastInteractionMs = millis();
  hintActive = true;
  hintUntilMs = lastInteractionMs + TRANSIENT_HINT_MS;

  if (!displayReady) {
    return;
  }

  gfx->fillRoundRect(7, 268, 156, 41, 5, COLOR_PANEL);
  gfx->drawRoundRect(7, 268, 156, 41, 5, color);
  drawCenteredText(294, message, color, 1);
  drawHoldProgress(0);
}

// -----------------------------------------------------------------------------
// 状态转换与业务动作
// -----------------------------------------------------------------------------

void setBadgeState(BadgeState nextState) {
  badgeState = nextState;
  stateSinceMs = millis();
  lastInteractionMs = stateSinceMs;
  clickPending = false;
  hintActive = false;
  lastHoldQuarter = -1;
  // 先刷新灯，再绘制页面：状态切换当下就表达新的业务含义。
  updateBusinessLed(stateSinceMs);
  renderCurrentPage();
}

void handleSingleClick() {
  lastInteractionMs = millis();

  switch (badgeState) {
    case BadgeState::WORKING:
      if (tasks[currentTaskIndex].remaining == 0) {
        showTransientHint("任务已完成，请换任务", COLOR_AMBER);
        break;
      }
      selectedIssue = 0;
      setBadgeState(BadgeState::REPORT_SELECT);
      break;

    case BadgeState::REPORT_SELECT:
      selectedIssue = (selectedIssue + 1) % ISSUE_COUNT;
      renderCurrentPage();
      break;

    case BadgeState::QUALITY_SELECT:
      if (qualityChoice == QualityChoice::NONE) {
        qualityChoice = QualityChoice::PASS;
      } else if (qualityChoice == QualityChoice::PASS) {
        qualityChoice = QualityChoice::FAIL;
      } else {
        qualityChoice = QualityChoice::NONE;
      }
      renderCurrentPage();
      break;

    case BadgeState::EXCEPTION_LOCKED:
    case BadgeState::QUALITY_HOLD:
    case BadgeState::COMPLETE:
      break;
  }
}

void handleDoubleClick() {
  lastInteractionMs = millis();

  switch (badgeState) {
    case BadgeState::WORKING:
      currentTaskIndex = (currentTaskIndex + 1) % TASK_COUNT;
      Serial.printf("# UI switched to task %u/%u (%s)\n",
                    static_cast<unsigned>(currentTaskIndex + 1),
                    static_cast<unsigned>(TASK_COUNT),
                    tasks[currentTaskIndex].taskId);
      renderCurrentPage();
      break;

    case BadgeState::REPORT_SELECT:
    case BadgeState::QUALITY_SELECT:
      qualityChoice = QualityChoice::NONE;
      setBadgeState(BadgeState::WORKING);
      Serial.println("# UI selection cancelled; no business event emitted");
      break;

    case BadgeState::EXCEPTION_LOCKED:
    case BadgeState::QUALITY_HOLD:
    case BadgeState::COMPLETE:
      break;
  }
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

void handleLongPress() {
  lastInteractionMs = millis();

  switch (badgeState) {
    case BadgeState::WORKING:
      if (tasks[currentTaskIndex].remaining == 0) {
        showTransientHint("任务已完成，请换任务", COLOR_AMBER);
        break;
      }
      qualityChoice = QualityChoice::NONE;
      setBadgeState(BadgeState::QUALITY_SELECT);
      break;

    case BadgeState::REPORT_SELECT:
      setBadgeState(BadgeState::EXCEPTION_LOCKED);
      emitEvent("EXCEPTION_RAISED", "category", ISSUE_OPTIONS[selectedIssue].eventValue);
      break;

    case BadgeState::QUALITY_SELECT:
      if (qualityChoice == QualityChoice::NONE) {
        showTransientHint("请先短按选择结果", COLOR_AMBER);
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
        Serial.println("# DEMO_RESET local 8-second hold; this is not a production resolution event");
        qualityChoice = QualityChoice::NONE;
        setBadgeState(BadgeState::WORKING);
      }
      break;

    case BadgeState::COMPLETE:
      break;
  }
}

uint32_t holdThresholdForCurrentState() {
  switch (badgeState) {
    case BadgeState::WORKING:
    case BadgeState::REPORT_SELECT:
    case BadgeState::QUALITY_SELECT:
      return LONG_PRESS_MS;
    case BadgeState::EXCEPTION_LOCKED:
    case BadgeState::QUALITY_HOLD:
      return DEMO_MODE ? DEMO_RESET_HOLD_MS : 0;
    case BadgeState::COMPLETE:
      return 0;
  }
  return 0;
}

// -----------------------------------------------------------------------------
// 按键轮询
// -----------------------------------------------------------------------------

void queueShortClick(uint32_t nowMs) {
  if (!clickPending) {
    clickPending = true;
    firstClickReleasedAtMs = nowMs;
    return;
  }

  if ((nowMs - firstClickReleasedAtMs) <= DOUBLE_CLICK_WINDOW_MS) {
    clickPending = false;
    handleDoubleClick();
  } else {
    // 两次相隔太久：第一下先按单击处理，第二下成为新的候选。
    clickPending = false;
    handleSingleClick();
    if (badgeState == BadgeState::WORKING ||
        badgeState == BadgeState::REPORT_SELECT ||
        badgeState == BadgeState::QUALITY_SELECT) {
      clickPending = true;
      firstClickReleasedAtMs = nowMs;
    }
  }
}

void processPendingClick(uint32_t nowMs) {
  if (!clickPending) {
    return;
  }

  // 第二次按压已经开始时，不在消抖窗口内提前结算第一下。
  if (lastRawPressed || stablePressed) {
    return;
  }

  if ((nowMs - firstClickReleasedAtMs) > DOUBLE_CLICK_WINDOW_MS) {
    clickPending = false;
    handleSingleClick();
  }
}

void pollButton(uint32_t nowMs) {
  const bool rawPressed = digitalRead(PIN_BUTTON) == LOW;

  if (rawPressed != lastRawPressed) {
    lastRawPressed = rawPressed;
    rawChangedAtMs = nowMs;
  }

  if ((nowMs - rawChangedAtMs) >= DEBOUNCE_MS && stablePressed != lastRawPressed) {
    stablePressed = lastRawPressed;

    if (stablePressed) {
      pressedAtMs = nowMs;
      longPressTriggered = false;
      // 不允许把“一个待结算短按 + 紧接的一次长按”组合成提交动作。
      // 第二次按压必须先松开，让它只可能成为双击或无效组合手势。
      blockLongForCurrentPress = clickPending;
      lastHoldQuarter = -1;
      lastInteractionMs = nowMs;
    } else {
      const uint32_t heldMs = nowMs - pressedAtMs;
      clearHoldProgress();

      if (!longPressTriggered &&
          heldMs >= MIN_SHORT_PRESS_MS &&
          heldMs <= MAX_SHORT_PRESS_MS &&
          badgeState != BadgeState::COMPLETE &&
          badgeState != BadgeState::EXCEPTION_LOCKED &&
          badgeState != BadgeState::QUALITY_HOLD) {
        queueShortClick(nowMs);
      } else if (!longPressTriggered &&
                 heldMs > MAX_SHORT_PRESS_MS &&
                 holdThresholdForCurrentState() > 0) {
        if (blockLongForCurrentPress && clickPending) {
          // 安全地结算第一下短按，拒绝把第二下长按解释为提交。
          clickPending = false;
          handleSingleClick();
          showTransientHint("组合操作无效，请重试", COLOR_AMBER);
        } else {
          showTransientHint("未确认，请按到100%", COLOR_AMBER);
        }
      }
      blockLongForCurrentPress = false;
      lastHoldQuarter = -1;
    }
  }

  if (!stablePressed || longPressTriggered) {
    return;
  }

  if (blockLongForCurrentPress) {
    return;
  }

  const uint32_t threshold = holdThresholdForCurrentState();
  if (threshold == 0) {
    return;
  }

  const uint32_t heldMs = nowMs - pressedAtMs;
  uint8_t quarter = static_cast<uint8_t>((heldMs * 4UL) / threshold);
  if (quarter > 4) {
    quarter = 4;
  }

  if (static_cast<int8_t>(quarter) != lastHoldQuarter) {
    lastHoldQuarter = static_cast<int8_t>(quarter);
    drawHoldProgress(quarter * 25);
    Serial.printf("# HOLD %u%%\n", static_cast<unsigned>(quarter * 25));
  }

  if (heldMs >= threshold) {
    longPressTriggered = true;
    clickPending = false;
    handleLongPress();
  }
}

// -----------------------------------------------------------------------------
// Arduino 入口
// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println();
  Serial.println("===== Smart Factory Badge / 3-module local MVP =====");
  Serial.println("# Wiring: LED R=15 G=16 B=17(common GND), button S=2(active LOW)");

  initializeLedPins();
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  // 背光先保持关闭，首帧画完后再打开，减少白屏闪烁。
  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, LOW);

  displayReady = gfx->begin();
  if (displayReady) {
    gfx->setUTF8Print(true);
    if (gfx->width() != 170 || gfx->height() != 320) {
      Serial.printf("# WARNING display logical size is %dx%d, expected 170x320\n",
                    gfx->width(), gfx->height());
    }
    renderSplash();
    digitalWrite(PIN_TFT_BL, HIGH);
  } else {
    Serial.println("# ERROR gfx->begin() failed; button/LED/Serial state machine will still run");
  }

  runLedSelfTest();
  delay(550);

  bootId = esp_random();
  stateSinceMs = millis();
  lastInteractionMs = stateSinceMs;
  emitEvent("DEVICE_BOOT");
  setBadgeState(BadgeState::WORKING);
}

void loop() {
  const uint32_t inputNowMs = millis();

  pollButton(inputNowMs);
  processPendingClick(inputNowMs);

  // 按键处理可能在本轮调用 setBadgeState() 并写入一个更晚的 stateSinceMs。
  // 这里必须重新取时，避免旧时间减新时间发生无符号下溢，导致灯相位跳变
  // 或选择页被误判为已经超时。
  const uint32_t nowMs = millis();
  updateBusinessLed(nowMs);

  if (hintActive && static_cast<int32_t>(nowMs - hintUntilMs) >= 0) {
    hintActive = false;
    renderCurrentPage();
  }

  if ((badgeState == BadgeState::REPORT_SELECT ||
       badgeState == BadgeState::QUALITY_SELECT) &&
      !stablePressed && !lastRawPressed && !clickPending &&
      (nowMs - lastInteractionMs) >= SELECT_TIMEOUT_MS) {
    qualityChoice = QualityChoice::NONE;
    setBadgeState(BadgeState::WORKING);
    Serial.println("# UI selection timed out; no business event emitted");
  }

  if (badgeState == BadgeState::COMPLETE &&
      !stablePressed && !lastRawPressed && !clickPending &&
      (nowMs - stateSinceMs) >= COMPLETE_SCREEN_MS) {
    if (tasks[currentTaskIndex].remaining == 0 && DEMO_MODE) {
      currentTaskIndex = (currentTaskIndex + 1) % TASK_COUNT;
    }
    qualityChoice = QualityChoice::NONE;
    setBadgeState(BadgeState::WORKING);
  }

  delay(1);
}
