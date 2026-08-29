# HaloFactory 后端 Web 展示原型

本目录保存生产协同 MES 后端管理界面的两版单文件静态展示稿，以及随设计包提供的视觉规范与交接资料。

## 展示入口

| 文件 | 定位 | 运行特征 |
| --- | --- | --- |
| [`mes-graphite-dark.html`](mes-graphite-dark.html) | 推荐入口；石墨深色工业监控方案 | 加载 Google Fonts；使用 `localStorage` 记住当前导航页 |
| [`smart-factory-mes-wireframe.html`](smart-factory-mes-wireframe.html) | 初始浅色线框 | 完全内联，无外部依赖 |

## 展示范围

- 订单总览与工序进度
- 订单建档和施工图上传界面
- AI 拆解草稿与生管确认
- 任务派发和车间人员分配
- 生产进度跟踪
- 成套能力、瓶颈风险和调整建议
- 装配质检、包装入库和基础资料

## 查看方式

下载或克隆本仓库后，直接使用现代浏览器打开推荐的 `mes-graphite-dark.html`，或打开 `smart-factory-mes-wireframe.html` 查看初始线框。两者都不需要安装依赖或启动服务器；深色版无法访问 Google Fonts 时会回退到系统字体。

这是静态交互原型，不包含实际后端接口、身份认证或业务数据持久化。深色版只在浏览器本地保存导航页状态。页面中的订单、人员、产量和生产进度均为演示数据，不代表真实生产记录。

## 设计资料

- [`brand-spec.md`](brand-spec.md)：石墨深色方案的颜色、字体和视觉规则。
- [`DESIGN-HANDOFF.md`](DESIGN-HANDOFF.md)：随设计包提供的实施交接说明。
- [`DESIGN-MANIFEST.json`](DESIGN-MANIFEST.json)：设计文件、交互和视口的机器可读清单。

这些文件是设计交付资料，不会被仓库自动执行。

## 来源校验

- 原始归档 `HaloFactory_Web端.zip` SHA-256：`28afb1848238e6c4e5695326ac4719ead06de8df8ff614d907aeefa94eebc5f2`
- 深色展示稿 SHA-256：`9d8f203310df15944e882b56806cba376ac0d83bfbc9984f0f1c4b7e395da6f9`
- 初始线框 SHA-256：`1085faf766178cb9ea7152afdbc1ffe5717959bd77bd4872acf887b18830bbd9`
