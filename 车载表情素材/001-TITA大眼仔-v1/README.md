# 001 · TITA 大眼仔 · v1

| 项目 | 值 |
|---|---|
| `pack_id` | `tita` |
| 画布 | 360×360，黑底 |
| 状态数量 | 8 |
| GIF 基线 | 15 FPS 候选预览 |
| EAF | 8-bit 索引色 BGRA + RLE，12×30 行分块 |
| 验证状态 | 结构与 RLE 回读已检查；颜色、方向、帧率和内存待实机验证 |

本包从用户提供的 TITA 大眼仔固件资源中筛选车载候选动画，并将内部文件名统一为 `car_<state>`。源固件、视频、MJPEG、原始 GIF 和归档位于 [`../源素材/001-TITA大眼仔-v1/`](../源素材/001-TITA大眼仔-v1/)；对外分发前需确认原素材授权范围。

## 状态映射

| 文件名 | 中文预览名 | 原始语义/来源 | 建议用途 |
|---|---|---|---|
| `car_cruise` | 巡航 | `idle_loop` | 默认中性循环 |
| `car_wake` | 唤醒 | `boot` | 进入模式时单次播放 |
| `car_wait` | 等候/校准 | `AP_loop` | 等待传感器或校准 |
| `car_reverse` | 倒车 | `R_loop` | 仅有可信倒车信号时使用 |
| `car_turn_left` | 左转 | `turn_left_loop` | 左转持续状态 |
| `car_turn_right` | 右转 | `turn_right_loop` | 右转持续状态 |
| `car_bump` | 颠簸 | `nonsensical_loop` | 短时颠簸/眩晕 |
| `car_signal_lost` | 信号中断 | `stranded_loop` | 传感器异常或失联 |

本包没有加速、减速、急刹专用动画。当前固件对 `car_accelerate`、`car_decelerate`、`car_hard_brake` 显式回退到本包 `car_cruise`；不得将充电、音乐、盲区或双闪素材改名冒充。

## 目录内容

- `gif/`：8 个电脑端预览动画。
- `eaf/`：8 个同名实机候选文件及 `manifest.json`。
- `全部表情预览.gif`：全部状态同步动态总览，中文标签。
- `全部静态表情预览.png`：每个状态的代表姿态，中文标签。

接入前阅读 [`../02-素材使用规范.md`](../02-素材使用规范.md)。预览通过不代表 EAF 已完成设备验收。
