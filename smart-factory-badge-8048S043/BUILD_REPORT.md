# ESP32-8048S043 竖屏触摸版构建与实机验证报告

验证日期：2026-08-29

## 最终方案

本版完全取消外接 KY-004 和 RGB LED：

- 业务输入全部改为板载 GT911 电容触摸。
- 业务灯全部改为屏幕内的状态灯动画。
- GPIO11、GPIO12、GPIO13 和 GPIO17 不用于业务输入/输出，不需要任何外接接线。
- GPIO2 仅用于 RGB 屏背光，不是业务状态 LED。

触摸页面设计为：任务页三个按钮；问题页点选问题卡、取消或长按 1 秒确认；品质页点选合格/不合格、取消或长按 1 秒提交；锁定页在演示模式下长按 8 秒复位。

## 目标实物

- 芯片：ESP32-S3 QFN56 rev 0.2。
- MAC：`58:e6:c5:5a:da:b8`。
- Flash：16 MB，Quad，3.3V。
- PSRAM：8 MB，Octal。
- 显示：ESP32-8048S043，RGB 面板物理/原生扫描 800×480 RGB565；软件 `rotation=1` 后逻辑画布为 480×800 竖屏。
- 触摸：GT911 电容触摸的 ESP32-8048S043C 版。
- USB 转串口：CH340，验证端口 `/dev/cu.usbserial-110`。

## 工具链

- PlatformIO Core 6.1.19。
- pioarduino platform-espressif32 53.03.13-1。
- Arduino-ESP32 3.1.3。
- ESP-IDF 5.3.2。
- Arduino_GFX 1.6.7。
- U8g2 2.36.18。
- esptool 4.8.1。

## 实际构建配置

- Flash 启动模式：DIO 80 MHz。
- Arduino 内存类型：`qio_opi`，即 Quad Flash 运行库 + Octal PSRAM。
- Flash 容量：16 MB。
- 分区：`huge_app.csv`，应用上限 3,145,728 字节。
- RGB 面板原生尺寸：800×480。
- 软件 rotation 后逻辑画布：480×800，竖屏。
- 单帧 RGB565 显示帧缓冲：800×480×2 = 768,000 字节，PSRAM。rotation 不改变帧缓冲容量。
- RGB 像素时钟：12.5 MHz。
- C 版显示同步极性：HSYNC / VSYNC 空闲高电平。
- GT911：SDA GPIO19、SCL GPIO20、RST GPIO38、I²C 100 kHz、每 12 ms 轮询。
- GT911 INT 不使用，不依赖 GPIO18 与 R17 是否连通。

## 最新触摸版构建结果

```text
RAM:   21,104 / 327,680 bytes (6.4%)
Flash: 1,451,456 / 3,145,728 bytes (46.1%)
firmware.bin: 1,451,824 bytes
Result: SUCCESS
```

`firmware.bin` 的 1,451,824 字节是最新触摸版构建产物的实际文件大小，与 PlatformIO 报告的 Flash 占用 1,451,456 字节是两个不同指标，不应混为同一数值。该应用镜像已用于本次烧录和启动验证。

工程仍使用 `lib_archive = no` 直接链接库对象，避免低磁盘空间构建机再生成一份巨大的 Arduino_GFX 临时静态库。这只影响构建中间形式，不改变固件功能。

## GT911 实机识别与轮询

初始化没有使用 INT，而是对 `0x5D` 和 `0x14` 两个合法 7-bit 地址执行产品 ID 读取。只有 `0x8140` 返回的前三个字节严格等于 ASCII `911` 时才接受地址；单纯 I²C ACK 不算识别成功。

实机已识别：

| 项目 | 实测值 |
|---|---:|
| I²C 地址 | `0x5D` |
| Product ID | `911` |
| Firmware | `0x1060` |
| Reported resolution | `480×272` |
| Effective raw range | `480×272` |

这里的 `480×272` 是已留存串口证据中的 reported/effective range；本轮证据未保留 `0x8047..0x804B` config block 的实际 Xmax/Ymax 日志行，因此不从 reported 值反推或臆造 config 值。

运行时每 12 ms 轮询 `0x814E`。只有 data-ready 包才读取 `0x814F` 起始的点数据，并在消费后向 `0x814E` 写 0。业务仅接受单点，多点或非法点数会被拒绝，避免误提交。1 秒业务长按阈值为 1,000 ms，要求样本新鲜度不超过 60 ms、移动不超过 28 个逻辑像素；连续 300 ms 无有效样本则按释放/异常处理。

坐标路径为：

```text
GT911 raw 480x272
    -> 自动归一化为物理 800x480
    -> Arduino_GFX rotation=1
    -> 逻辑 480x800

logicalX = physicalY
logicalY = 799 - physicalX
```

该路径除启动日志外也已通过实机手指测试确认方向正确；“下一任务”、问题页进入/取消和品质 PASS 路径均能按预期命中。四个边角的 raw→logical 逐点日志尚未单独留档，因此边角校准仍保留为待验收项。

## 实机烧录和启动

最新 1,451,824 字节的触摸版 `firmware.bin` 已烧录并启动。DIO Flash、8 MB OPI PSRAM、GT911 身份/固件/分辨率读取、480×800 逻辑尺寸和业务状态机进入 `WORKING` 已由日志验证。

启动日志关键行：

```text
mode:DIO, clock div:1
===== Smart Factory Badge / ESP32-8048S043 =====
# Display: physical RGB565 800x480, logical portrait 480x800, rotation=1
# Input: onboard GT911 polling; no external button or LED required
# GT911 polling I2C 100kHz SDA=19 SCL=20 RST=38; INT unused
# GT911 firmware=0x1060 reported_resolution=480x272 valid=yes
# GT911 effective raw range=480x272
# GT911 ready address=0x5D product=911
# TOUCH transform swapXY=0 mirrorX=0 mirrorY=0 rotation=1
# PSRAM found=yes size=8388608 bytes required=768000 bytes
# DISPLAY ready logical=480x800 backlight=on
{"event_type":"DEVICE_BOOT", ... "state":"WORKING"}
# FLOW state=WORKING quality=NONE led=WORKING_GREEN_SOLID
```

这里的 `led=WORKING_GREEN_SOLID` 表示屏内状态灯模式，不表示 GPIO 上存在外接 LED。

先前将启动头强制为 QIO 时曾导致进入应用前看门狗复位；恢复 DIO 后可完整进入应用。`platformio.ini` 已固化 DIO 模式。

## 屏内状态与触摸流程

| 流程 | 交互 | 屏内状态灯 |
|---|---|---|
| 任务页 | 点击上报问题 / 下一任务 / 完工确认 | 绿灯常亮 |
| 问题页 | 点选问题卡，取消返回，长按确认 1 秒提交 | 黄灯慢闪 |
| 品质页 | 点选合格/不合格，取消返回，长按提交 1 秒 | 黄灯双脉冲 |
| 问题/不合格锁定 | 演示模式可长按 8 秒复位 | 红灯快闪 |
| 完成当前一件 | 短暂完成页后返回流程 | 绿灯双闪 |

实机已完成“下一任务”连续切换、问题页进入/取消、品质页选择 PASS，以及静止长按 1,000 ms 提交；串口产生 `QUALITY_SUBMITTED=PASS` 和 `PART_COMPLETED`。切页后的 release suppression 也已验证，同一次触摸不会穿透并触发新页面。其余分支仍按下一节逐项保留，不将一条 PASS 路径扩大为全流程通过。

## 验证状态与边界

| 验证项 | 状态 | 说明 |
|---|---|---|
| 最新触摸版编译 | 已完成 | RAM/Flash/镜像大小如上 |
| 最新应用镜像烧录与启动 | 已完成 | 已进入 `WORKING` |
| DIO Flash 和 8 MB OPI PSRAM | 已完成 | 启动日志正常 |
| 物理 800×480 / 逻辑 480×800 显示初始化 | 已完成 | `DISPLAY ready logical=480x800` |
| GT911 双地址严格 `911` 识别 | 已完成 | 实机命中 `0x5D` |
| GT911 固件/原始范围读取 | 已完成 | `0x1060`、`480×272` |
| 无外接按键/LED 启动 | 已完成 | 日志明确为 onboard GT911 + screen status |
| 触摸方向与基本页面命中 | 已完成 | 方向正确；三个任务入口均已使用，“下一任务”可连续切换 |
| 问题页进入与取消 | 已完成 | 可返回任务页，无触摸穿透 |
| 品质 PASS 选择与 1 秒提交 | 已完成 | 静止 1,000 ms；产生 `QUALITY_SUBMITTED=PASS`、`PART_COMPLETED` |
| 切页 release suppression | 已完成 | 同一次触摸未穿透到新页面 |
| 四角 raw→logical 逐点日志 | **待完成** | 当前仅确认整体方向正确，未单独留存四角日志 |
| 四种问题分类及异常 1 秒长按 | **待完成** | 需逐类选择并核对 `EXCEPTION_RAISED` 与锁定页 |
| FAIL 提交与锁定 | **待完成** | PASS 路径不能替代 FAIL 分支 |
| 演示锁定 8 秒长按复位 | **待完成** | 需实物持续触摸验收 |
| 屏内状态灯各动画的目视验收 | **待完成** | 启动 `FLOW` 日志不能替代全流程目视验收 |

本版仍是离线 MVP，不包含 Wi-Fi、MQTT/HTTP、NVS 离线队列、后台对账、真实身份认证和正式锁定解除。正式上线前必须把 `DEMO_MODE` 改为 `false`。

## 最终 verify 与 SHA-256

烧录后已对 bootloader、partitions、boot_app0 和最新触摸版应用镜像逐段执行 `verify_flash`，四段均返回 `digest matched`。当前 `firmware/` 文件的大小与 SHA-256 也已重新只读核对：

| 文件 | 字节数 | SHA-256 | 烧录后校验 |
|---|---:|---|---|
| `bootloader.bin` | 18,928 | `120cca76587e80d2d57899a8448a93e5e0708d385cf6134ce0aaefba2a851385` | `digest matched` |
| `partitions.bin` | 3,072 | `aaae2888c5a6a348004b5b436f47abb25ae32e72d9003902955a998eda723edd` | `digest matched` |
| `boot_app0.bin` | 8,192 | `f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0` | `digest matched` |
| `firmware.bin` | 1,451,824 | `5051bd9c873e36620ef2bf41a23dd5130d8f88bc5f1064937b5edf2afe67b2d1` | `digest matched` |

因此，最终四段镜像的烧录后校验与 hash 已完成；尚未完成的是上一节明确列出的剩余触摸分支和状态灯人工目视验收。
