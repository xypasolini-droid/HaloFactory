# 智慧工厂工牌固件合集

本仓库集中保存两套彼此独立的智慧工厂工牌 PlatformIO/Arduino 固件工程。

| 目录 | 硬件版本 | 交互方式 |
| --- | --- | --- |
| [`smart-factory-badge/`](smart-factory-badge/) | ESP32-1732S019，170×320 ST7789 屏 | KY-004 按键与外置红/绿 LED |
| [`smart-factory-badge-8048S043/`](smart-factory-badge-8048S043/) | ESP32-8048S043，800×480 RGB 屏 | GT911 电容触摸与屏内状态灯 |

每个目录都包含：

- `src/main.cpp`：固件源码
- `platformio.ini`：PlatformIO 工程配置
- `README.md`：硬件、交互、构建及验收说明
- `BUILD_REPORT.md`：归档随附的构建记录
- `firmware/`：预编译烧录镜像及其说明

## 来源归档

两套工程按原 ZIP 目录结构解压，工程内部文件未作修改。为便于核验，原始归档的 SHA-256 如下：

| 原始归档 | SHA-256 |
| --- | --- |
| `smart-factory-badge.zip` | `cbb161cc3360b80d1955276750b4461856da27b7bb83a72ecdc6100d304fe151` |
| `smart-factory-badge-8048S043.zip` | `5e166ccdeb9a66ba0498d23d0870d333d0cfaaa07e3183776a55529b23dfa143` |

## 使用提示

请进入对应硬件版本目录后，按照该目录的 `README.md` 使用 PlatformIO 构建或烧录。两套工程都是离线 MVP，不能替代真实身份认证、联网后台、断网补传或工业安全联锁。

原始归档未包含开源许可证。若计划公开或分发本仓库，请先确认相关权利并补充适用许可证。随附的预编译镜像和构建报告可能保留构建路径、串口或设备标识等环境信息；公开前建议重新构建或清理这些产物。
