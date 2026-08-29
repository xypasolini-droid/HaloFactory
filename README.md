# HaloFactory｜协同工厂

## 产品定位

**随身生产看板**

## Slogan

**把下一步，戴在胸前。**

## 辅助传播句

**做什么、怎么做、做到哪儿，一眼有数；**<br>
**有问题，随手就报。**

## 作品描述

HaloFactory 是一款面向制造现场的 AI 协同工牌，也是一块跟着员工移动的“随身生产看板”。它把当前任务、操作要点、完成进度和下一步行动直接呈现在员工胸前，让每个人随时知道自己现在做什么、怎么做、已经做到哪里。

当现场出现缺料、设备异常、质量疑问或需要支援等问题时，员工可以快速上报，系统结合当前人员、工位、工序和任务信息，将问题准确传递给对应负责人，并持续同步处理状态。

HaloFactory 让任务跟着人走、问题留在现场解决，减少信息不同步、交接不清和问题无人跟进造成的漏做、错做与返工。

## 代码内容

本仓库集中保存两套彼此独立的 HaloFactory 智慧工牌 PlatformIO/Arduino 固件工程。

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

## 后端 Web 展示原型

[`backend-web/mes-graphite-dark.html`](backend-web/mes-graphite-dark.html) 是推荐的深色工业风展示入口；[`backend-web/smart-factory-mes-wireframe.html`](backend-web/smart-factory-mes-wireframe.html) 保留为初始浅色线框。两者均展示从订单图纸、AI 拆解与任务派发，到车间进度、成套风险、质检及包装入库的生产协同流程。

两个原型的 CSS 和 JavaScript 均已内联，下载或克隆仓库后可直接使用浏览器打开。深色版会加载 Google Fonts，并使用 `localStorage` 记住当前导航页；浅色版无外部依赖。页面中的订单、人员、产量和生产进度均为演示数据，不代表真实生产记录，也不包含实际后端接口或业务数据持久化功能。

## 来源归档

两套工程按原 ZIP 目录结构解压，工程内部文件未作修改。为便于核验，原始归档的 SHA-256 如下：

| 原始归档 | SHA-256 |
| --- | --- |
| `smart-factory-badge.zip` | `cbb161cc3360b80d1955276750b4461856da27b7bb83a72ecdc6100d304fe151` |
| `smart-factory-badge-8048S043.zip` | `5e166ccdeb9a66ba0498d23d0870d333d0cfaaa07e3183776a55529b23dfa143` |

## 使用提示

请进入对应硬件版本目录后，按照该目录的 `README.md` 使用 PlatformIO 构建或烧录。两套工程都是离线 MVP，不能替代真实身份认证、联网后台、断网补传或工业安全联锁。

原始归档未包含开源许可证。若计划公开或分发本仓库，请先确认相关权利并补充适用许可证。随附的预编译镜像和构建报告可能保留构建路径、串口或设备标识等环境信息；公开前建议重新构建或清理这些产物。
