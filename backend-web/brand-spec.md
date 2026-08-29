# Brand Spec — 石墨精工 Graphite（工厂 MES 深色工业方案）

深色工业级监控仪表盘。近黑石墨底 + 冷钢质感表面 + 单一琥珀金强调，
适合车间大屏与生管长时间监控。高级感来自克制的深色层次、精确的数据密度、
以及等宽字体带来的仪表盘精工感。

## OKLch 令牌

| 令牌 | 值 | 用途 |
|---|---|---|
| `--bg` | `oklch(0.18 0.012 250)` ≈ `#0e1116` | 页面石墨底（非纯黑） |
| `--surface` | `oklch(0.235 0.014 250)` ≈ `#161b22` | 卡片/表面 |
| `--fg` | `oklch(0.93 0.006 250)` ≈ `#e7ecf2` | 前景文字（非纯白） |
| `--muted` | `oklch(0.66 0.012 250)` ≈ `#8b95a3` | 次级文字 |
| `--border` | `oklch(1 0 0 / 0.09)` | 半透明白发丝边框 |
| `--accent` | `oklch(0.80 0.14 78)` ≈ `#e0a94a` | 琥珀金，唯一强调色 |

语义色（仅用于状态与数据编码）：
`--success oklch(0.72 0.15 150)`、`--warn oklch(0.79 0.14 78)`、`--danger oklch(0.66 0.19 25)`、`--info oklch(0.70 0.11 235)`。

## 字体栈

- Display：`"Space Grotesk", "PingFang SC", "Microsoft YaHei", sans-serif`（几何工业标题）
- Body：`"IBM Plex Sans", "PingFang SC", "Microsoft YaHei", sans-serif`
- Mono：`"IBM Plex Mono", "SFMono-Regular", Menlo, monospace`（表格数字对齐）

## 视觉规则

1. 强调色每屏至多两处（激活导航 + 主 CTA）；语义色不计入强调预算。
2. 所有数据（编号、计划数、进度、套量）用等宽字体，右对齐或表格对齐，建立仪表盘精工感。
3. 卡片无左侧色条、无圆角滥用；层次靠表面亮度差 + 发丝边框，不靠阴影堆叠。
4. 激活态用琥珀金左侧发光竖条（唯一装饰主张），其余导航为幽灵态。
5. hover 用 OKLch L 通道 ±0.06 提亮表面 + 边框增强，绝不降低前景对比。
