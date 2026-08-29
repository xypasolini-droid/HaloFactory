# 构建验证报告

构建日期：2026-08-28  
构建结果：`SUCCESS`

## 已验证环境

- PlatformIO Core：6.1.19
- pioarduino platform-espressif32：53.03.13-1
- Arduino-ESP32：3.1.3
- ESP-IDF 库：5.3.2
- Xtensa GCC：13.2.0
- Arduino_GFX：1.6.7
- U8g2：2.36.18
- 目标环境：`esp32-s3-devkitc-1`
- 分区：`huge_app.csv`，单应用槽 3,145,728 字节

## 实际构建结果

```text
RAM:   6.2%  (20,236 / 327,680 bytes)
Flash: 44.6% (1,403,780 / 3,145,728 bytes)
========================= [SUCCESS] =========================
```

在首次完整构建后又执行了增量 `pio run`，结果同样为 `SUCCESS`。ESP32-S3 镜像检查显示 bootloader 与 app 固件的校验和、SHA-256 validation hash 均有效。随后通过 `/dev/cu.usbserial-10` 实际烧录到 ESP32-S3，四个区段均通过写入哈希校验；复位后的 115200 串口日志确认设备以 `SPI_FAST_FLASH_BOOT` 启动、进入 `WORKING`，成功发出 `DEVICE_BOOT` 事件，并输出 `# FLOW state=WORKING quality=NONE led=WORKING_GREEN_SOLID`。本次针对现场反馈重做了业务灯映射：生产中绿常亮、未提交选择黄闪、锁定红快闪、完成绿双闪；同时修正了按键处理后复用旧毫秒时间戳可能造成灯相位下溢或选择页立即超时的竞态。

生成的分区表为：

```text
nvs      0x009000  20K
otadata  0x00e000   8K
app0     0x010000   3M
spiffs   0x310000 896K
coredump 0x3f0000  64K
```

## 交付二进制 SHA-256

```text
f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0  firmware/boot_app0.bin
d5a3f3f71099a3f6afe5a608d9bed2dea0e8cd5e6e9f09cbb4120a6ff01375fe  firmware/bootloader.bin
e1d330c24ac3986fe7493e2cdfb0fa9abb808a6995b2eeaf1cb50600435b7502  firmware/firmware.bin
aaae2888c5a6a348004b5b436f47abb25ae32e72d9003902955a998eda723edd  firmware/partitions.bin
```

## 验证边界

本报告证明源码已在与用户原 ZIP 对应的 Arduino Core 3.1.3 / ESP-IDF 5.3.2 工具链中成功编译、链接，且已在用户连接的 ESP32-S3 实物板完成烧录与启动灯态日志验证。监听窗口内没有收到新的按钮事件，因此生产页绿常亮已经由串口确认，其余黄闪、红快闪和绿双闪流程仍需由用户按主 README 的步骤进行目视与按键验收；串口验证也不能代替屏幕物理朝向、实物 R/G 丝印映射和现场按钮手感确认。
