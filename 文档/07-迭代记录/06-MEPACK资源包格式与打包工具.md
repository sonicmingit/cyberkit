# 06-MEPACK 资源包格式与打包工具

状态：已完成（主机打包范围）。关联需求：需求-005。

## 目标、范围与非目标

本轮将已批准车载表情素材母版统一转换为构建选择器和 TF 卡可共用的 MEPACK v1 设备包，并以 004 默认表情验证真实产物。

范围包括素材规则、确定性主机打包/回读工具、技能说明和 004 测试包。未修改固件 MEPACK 加载器、菜单、PSRAM 缓存、分区、OTA 或烧录流程，也未宣称测试包已能在设备上播放。

## 基线与设计

- 分支：`feature/car-mode`；工作区开始时已有未提交固件、资源和文档修改，本轮未覆盖或回滚这些改动。
- V2.0 已在 `/sdcard` 挂载 TF 卡，但当前 Emote 播放器仍接收内存指针，不能直接把 TF 文件路径作为动画源。
- 素材母版继续保留 GIF、EAF、README 和两份总览；设备包输出到独立的 `车载表情素材/设备资源包/`。
- MEPACK v1 使用 128 字节包头、UTF-8 JSON 清单、每状态 96 字节固定索引、4096 字节对齐的 payload 区和原始 EAF 数据。清单、payload、单 EAF 和整包均有 SHA-256 证据；当前签名状态显式为 `unsigned`。

## 修改文件与实际行为

- 新增 `车载表情素材/04-MEPACK资源包规范.md`，定义母版/设备包边界、二进制布局、清单、命名、TF 路径、加载契约和安全边界。
- 新增 `车载表情素材/工具/生成mepack.py`，提供 `pack` 与 `verify` 子命令。打包前校验母版身份、GIF/EAF 配对、360×360、EAF 结构以及 manifest 帧数、大小和哈希；输出采用临时文件替换并生成 `.sha256`。
- 更新素材 README、使用规范、命名规则和切换方案，使构建选择器与 TF 卡共用 MEPACK，并明确设备加载尚未实现。
- 更新 `.codex/skills/create-car-expression-pack`，把设备打包设为正式资源导出之后、固件接入之前的独立授权阶段。

## 验证命令与结果

```powershell
$env:PYTHONUTF8='1'
python -m py_compile 车载表情素材/工具/生成mepack.py
python 车载表情素材/工具/生成mepack.py pack 车载表情素材/004-默认表情-v1
python 车载表情素材/工具/生成mepack.py verify 车载表情素材/设备资源包/004-default-v1.mepack
python 车载表情素材/工具/生成mepack.py pack 车载表情素材/004-默认表情-v1 --output build/mepack-test/004-default-v1.mepack
```

结果：

- `004-default-v1.mepack`：5,415,432 字节，19 个状态，EAF payload 5,407,240 字节（含 4 字节对齐填充）。
- 整包 SHA-256：`f56fca3da6a67762ea2f10c4c2ffdc33c788d9007d3ec13e1a559f616d287b7c`。
- 正式输出与 `build/mepack-test` 二次输出 SHA-256 相同，证明同输入和参数的结果可复现。
- 将测试副本末字节翻转后，`verify` 以退出码 1 拒绝并报告 payload SHA-256 无效；正式包和素材母版未被修改。
- 技能使用系统 `quick_validate.py` 校验；Markdown 链接、差异和空白错误检查通过。

本轮属于主机工具和文档，不需要重新编译固件。没有烧录或 TF 实机验证。

## 后续入口

1. 固件实现 `PackProvider/PackManager`：扫描 `/sdcard/miaoban/packs/`，解析包头/清单/索引并按需把一个 EAF 读入 PSRAM。
2. 在车载设置菜单展示来源、名称和版本；切换成功后持久化 `source + pack_id + version`，失败保留旧包。
3. 让构建期资源选择器读取同一 MEPACK，替代 `prepare_v2_assets.py` 中写死的包列表。
4. 完成拔卡、损坏包、内存不足、语音抢占、帧率和长时间播放实机验证后，再提升需求-005 状态。
