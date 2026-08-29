# ESP32-8048S043 预编译固件

这四个文件是从同目录源码于 2026-08-29 实际构建、写入并逐段验证的 ESP32-8048S043C 竖屏触控镜像。界面逻辑尺寸为 480×800，使用板载 GT911 和屏内状态灯，不需要外接按键或 RGB LED。它们不适用于原先的 ESP32-1732S019，也不适用于没有 GT911 的 R 版板卡。

优先在项目根目录使用：

```bash
pio run -t upload --upload-port /dev/cu.usbserial-XXX
```

如果明确需要用 esptool 手工烧录，使用 DIO 80 MHz，地址如下：

```bash
esptool.py --chip esp32s3 \
  --port /dev/cu.usbserial-XXX --baud 115200 \
  --before default_reset --after hard_reset \
  write_flash -z --flash_mode dio --flash_freq 80m --flash_size detect \
  0x0000 bootloader.bin \
  0x8000 partitions.bin \
  0xE000 boot_app0.bin \
  0x10000 firmware.bin
```

不要把 `--flash_mode` 改为 `qio`；这块实物板的本次验证显示，QIO 启动头会在应用运行前反复被看门狗复位。

SHA-256：

```text
f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0  boot_app0.bin
120cca76587e80d2d57899a8448a93e5e0708d385cf6134ce0aaefba2a851385  bootloader.bin
5051bd9c873e36620ef2bf41a23dd5130d8f88bc5f1064937b5edf2afe67b2d1  firmware.bin
aaae2888c5a6a348004b5b436f47abb25ae32e72d9003902955a998eda723edd  partitions.bin
```

文件大小：`bootloader.bin` 18,928 字节，`partitions.bin` 3,072 字节，`boot_app0.bin` 8,192 字节，`firmware.bin` 1,451,824 字节。

本次实机启动识别到 GT911 地址 `0x5D`、Product ID `911`、固件 `0x1060`、原始坐标范围 480×272；程序会先归一化到物理 800×480，再按 `rotation=1` 转为逻辑 480×800。烧录后已实测页面点击、任务切换、问题页取消、品质选择及 1 秒长按提交。
