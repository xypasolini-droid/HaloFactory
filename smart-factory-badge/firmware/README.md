# 预编译固件

这里的四个文件由同目录源码在 2026-08-28 实际构建生成。建议优先在项目根目录执行 `pio run -t upload`，因为 PlatformIO 会自动处理串口、复位和烧录地址。

如果明确需要用 esptool 手工烧录，ESP32-S3 进入下载模式后使用以下地址：

```bash
esptool.py --chip esp32s3 --port YOUR_PORT write_flash \
  0x0000 bootloader.bin \
  0x8000 partitions.bin \
  0xe000 boot_app0.bin \
  0x10000 firmware.bin
```

`YOUR_PORT` 必须替换成实际串口。不要把这些地址用于其他型号或不同分区配置的板子。若只更新同一工程且分区表从未改变，可以只写 `0x10000 firmware.bin`；首次烧录应使用完整四文件或直接使用 PlatformIO upload。

SHA-256：

```text
f94c5d786a7a8fab06ac5d10e33bf37711a6697636dc037559ea19cc410a17f0  boot_app0.bin
d5a3f3f71099a3f6afe5a608d9bed2dea0e8cd5e6e9f09cbb4120a6ff01375fe  bootloader.bin
e1d330c24ac3986fe7493e2cdfb0fa9abb808a6995b2eeaf1cb50600435b7502  firmware.bin
aaae2888c5a6a348004b5b436f47abb25ae32e72d9003902955a998eda723edd  partitions.bin
```
