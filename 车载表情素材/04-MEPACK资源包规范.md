# MEPACK 车载表情设备资源包规范

版本：v1.0。适用于从已批准的素材母版生成构建期或 TF 卡使用的只读车载表情资源包。

## 1. 两类产物及边界

素材母版与设备资源包必须分开管理：

```text
NNN-表情包名称-vN/                 可审阅、可追溯的素材母版
├── gif/
├── eaf/
│   └── manifest.json
├── README.md
├── 全部表情预览.gif
└── 全部静态表情预览.png

设备资源包/NNN-<pack_id>-vN.mepack  只读设备资源包
```

- 母版是生成 `.mepack` 的唯一来源，保留 GIF、EAF、总览、README 和转换证据。
- `.mepack` 只包含设备清单、固定索引和 EAF 字节；不包含 GIF、预览、README、设计稿、提示词、视频或脚本。
- `.mepack` 是纯数据容器，不能执行代码。视觉审批不自动授权打包；正式 GIF/EAF 导出完成后，只有用户明确要求打包、构建或 TF 交付时才生成。
- 构建期选择器与 TF 卡加载器使用同一格式，不维护第二套状态清单或命名规则。

## 2. 文件名与身份

设备包固定命名为 `NNN-<pack_id>-vN.mepack`，例如 `004-default-v1.mepack`。编号、稳定 `pack_id` 和内容版本必须与母版 README 一致。

以下版本互相独立：

- `schema_version`：MEPACK 二进制和清单结构版本；当前为 `1`。
- `package.version`：表情内容版本；与母版目录末尾的 `vN` 一致。
- `min_firmware`：最低兼容固件；测试包可为空，正式对外包必须填写并完成对应固件加载验证。

## 3. MEPACK v1 二进制布局

所有整数均为小端。v1 不对 EAF 再做 ZIP 压缩，便于 TF 卡随机读取目标动画；EAF 已使用索引色与 RLE。

```text
0x0000  128 字节固定包头
0x0080  UTF-8 JSON 设备清单（紧凑、键排序）
        N × 96 字节固定状态索引
        零填充到 4096 字节边界
        EAF payload；每个 EAF 起点按 4 字节对齐
```

包头包含：

- 魔数 `MEPK`、`schema_version=1`、包头长度和标志；
- 清单长度、状态数量、索引项长度和索引总长度；
- payload 起点、payload 长度和整包长度；
- 清单 SHA-256、完整 payload SHA-256；
- 保留区，v1 必须为零。

每个 96 字节状态索引包含：

- 最长 31 字节的小写 ASCII 逻辑状态 ID，不带 `car_` 前缀；
- 相对 payload 的偏移、EAF 长度和 EAF SHA-256；
- FPS、循环标志、宽、高、帧数和保留字段。

清单与固定索引中的偏移、大小、哈希、尺寸、帧数、FPS 和循环标志必须一致。设备先校验包头与清单，再按需读取某个 EAF；不需要把整包装入内存。

## 4. 设备清单

MEPACK v1 清单至少包含：

```json
{
  "format": "mepack",
  "schema_version": 1,
  "package": {
    "number": "004",
    "pack_id": "default",
    "display_name": "默认表情",
    "version": 1
  },
  "compatibility": {
    "board": "CyberVoc-Board-V2_0",
    "display_width": 360,
    "display_height": 360,
    "eaf_encoding": "EAF 8-bit indexed BGRA palette + RLE",
    "loader_schema": 1,
    "min_firmware": null
  },
  "fallback": {"default_state": "cruise"},
  "signature": {"algorithm": "none", "status": "unsigned"},
  "states": []
}
```

状态 ID 使用 `cruise`、`idle_01`、`turn_left` 等逻辑名；母版文件仍命名为 `car_<state>.eaf`。每个状态记录源文件名、payload 偏移、大小、SHA-256、360×360 尺寸、帧数、FPS、循环标志和结束策略。

新包必须包含 `cruise` 与 `idle_01`～`idle_03`。其他固件请求状态缺失时按“目标状态 → 同包 cruise → Flash 内置中性表情”回退。已登记的规则生效前历史包只有显式使用打包器兼容选项时才可缺少待机组，不能借此创建新的不完整包。

## 5. 生成与校验

从仓库根目录执行：

```powershell
python 车载表情素材/工具/生成mepack.py pack 车载表情素材/004-默认表情-v1
python 车载表情素材/工具/生成mepack.py verify 车载表情素材/设备资源包/004-default-v1.mepack
```

默认输出到母版同级的 `设备资源包/`，并生成同名 `.mepack.sha256` 发布校验文件。也可显式指定输出：

```powershell
python 车载表情素材/工具/生成mepack.py pack <母版目录> --output <目标.mepack> --min-firmware <固件版本>
```

打包器必须执行并在任一失败时停止：

- 母版目录、编号、版本、README `pack_id` 与输出名称检查；
- GIF/EAF 文件集合与 `eaf/manifest.json` 一一对应，无缺失或多余项；
- GIF 为 360×360；EAF 魔数、长度、校验和、帧索引和逐帧尺寸有效；
- EAF 帧数、字节数和 SHA-256 与母版 manifest 一致；
- 必备状态、状态 ID、FPS、循环和结束策略有效；
- 写包后使用 `verify` 完整回读，核对包头、清单、索引、payload 和逐个 EAF。

同一输入和参数必须生成逐字节一致的 `.mepack`。容器不写入构建时间、绝对路径等不稳定字段。

## 6. TF 卡放置与加载契约

TF 卡统一使用：

```text
/sdcard/miaoban/packs/
├── 001-tita-v1.mepack
├── 004-default-v1.mepack
└── 005-nomi-v1.mepack
```

固件加载器实现时必须满足：

1. 扫描时只读取包头、清单和索引；路径必须限制在表情包目录，拒绝非法名称、重复身份和越界偏移。
2. 菜单只展示通过格式、兼容性和完整性校验的包，同时显示来源、名称和版本。
3. 切换时先停止或持有旧动画，再把目标状态对应的单个 EAF 读入 PSRAM；校验成功后才切换并保存 `source + pack_id + version`。
4. 缓存当前动画和巡航回退即可；文件读取、整包校验和哈希计算不得在显示或 IMU 高频回调中执行。
5. TF 卡拔出、读取失败、包损坏或 PSRAM 不足时保留仍有效的当前缓存，并在下一状态安全回退到 Flash 内置包。
6. 未完成真实加载器、菜单和实机播放验证前，只能把 `.mepack` 标为“主机格式验证通过”，不能标为“TF 卡可用”。

## 7. 完整性、签名与发布

- v1 打包器生成的测试包以 SHA-256 检测损坏，清单明确标记 `unsigned`；SHA-256 本身不证明来源可信。
- 允许用户自行复制的本地 TF 包可以先采用无签名模式。网络下载、公开分发或第三方包市场启用前，必须扩展签名块并由固件验证可信公钥。
- 正式发布记录包文件名、字节数、整包 SHA-256、母版 manifest SHA-256、打包命令、工具版本、目标固件和设备验证状态。
- 修改 EAF、状态映射、FPS、循环、回退或其他可见/运行行为时递增内容版本并重新打包；只改变容器结构时递增 `schema_version`，不得静默改变 v1 解析含义。

## 8. 当前实现状态

当前仓库已提供主机端 MEPACK v1 生成和完整回读工具。V2.0 已挂载 `/sdcard`，但固件尚未实现 MEPACK 扫描、PSRAM 单动画缓存和菜单选择；生成成功不等于已经能在设备上读取使用。
