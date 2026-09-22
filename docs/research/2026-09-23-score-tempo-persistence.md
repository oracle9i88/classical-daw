# 工程变速信息保留：2026-09-23

后续更新：[工程变拍与小节网格](2026-09-23-score-meter-persistence.md)增加 v5 拍号图。
本文保留 v4 阶段记录；本文末尾的李斯特 XML 检查是当时只保留初始拍号的旧基线。
当前按小节边界交换变拍的能力见 [MusicXML 变拍交换](2026-09-23-musicxml-meter-interchange.md)。

本轮修复 MIDI 导入 Score 后只保留第一个 BPM 的问题。完整的阶梯式速度图现在经过
MIDI → Score → 原生工程 v4 → 重开 → MIDI，并用于离线渲染。连续速度曲线、拍号变化、
MusicXML 中途速度指示、网页模型和 CoreAudio 实时桥接不包含在本轮内。

## 模型与兼容

- `Score::bpm` 是 tick 0 的唯一速度值。
- `Score::tempo_changes` 存储严格递增、tick > 0 的后续变化；重复位置或乱序明确失败。
- BPM 必须有限、大于 0 且不超过 1,000,000。最多一百万个后续变化，读取数量超限时在
  分配前拒绝。MIDI 文件写出仍受 SMF 整数微秒速度字段的可表示范围约束。
- 所有位置沿用 960 PPQ；修改初始 BPM 不会自动按比例修改后续速度。
- 原生格式 v4 在 `bpm` 后、`meter` 前增加以下段落。v1/v2/v3 不含这段，读入时清空
  目标对象原有的后续速度图，保持它们原来的恒定速度语义。

```text
CLASSICAL_DAW_PROJECT 4
divisions 960
bpm 100
tempo_changes 2
tempo 960 60
tempo 1920 120
meter 4 4
```

`scoreTempoMap` 统一转换和验证。保存、恢复副本、撤销、重做、历史快照均保留该向量。
历史对象原来手工复制 Score 标量与 parts，本轮补上 tempo_changes 的交换，防止目标
对象遗留旧速度图。完整撤销栈仍只在内存中；恢复文件保存当前状态。

变速点不会切断连音。音符的 tick 起止不变，渲染使用分段速度换算经过时间，一个连音
仍只有一次起音。

## MusicXML 边界

当前 MusicXML 写出仍只有初始速度，`MusicXmlExportReport::omitted_tempo_changes`
明确统计被省略的后续变化；成功写出时才更新报告。非法速度图在触碰文件前拒绝。
初始 BPM 使用可往返的 double 精度，避免默认六位有效数字造成舍入；科学计数法会展开
为十进制，符合 [MusicXML sound@tempo 的类型要求](https://www.w3.org/2021/06/musicxml40/musicxml-reference/elements/sound/)。
测试包括 `60000000/766667` 和 `0.000001` BPM 的初始速度往返。

## 本地验证

普通 CTest **17/17**，AddressSanitizer + UndefinedBehaviorSanitizer **17/17**。
Leak detection 关闭，因此不将其称为泄漏检查。新增测试验证：

- 60 → 120 → 30 BPM 的独立数学时间与实际发声/静音窗口。
- 指挥轨速度信息、跨变速连音、MIDI 整数微秒精度。
- v4 精确读写、v1–v3 兼容、撤销/重做/清理历史/恢复和 `.tmp` 回退。
- 重复/乱序/零 tick、非法 BPM、截断/不匹配数量和超限输入的失败保全。
- MusicXML 速度遗漏计数、初始速度精度及失败时的文件/报告保护。

独立 mido 1.3.3 比较器现同时比较通道事件和规范化速度图。三个既有 LilyPond 源文件
只读复用，不复制进仓库；共 **9,200 个音符、18,680 条通道消息、13 个速度条目**通过
直接 MIDI 和原生工程两条往返路径。

| 文件（相对仓库） | 通道消息 | 速度条目（包含 tick 0） |
| --- | ---: | ---: |
| `../baroque-violin-sonata/sonata.midi` | 1,052 | 1 |
| `../chopin-op9-no1-light/score.midi` | 3,712 | 1 |
| `../liszt-ballade-romantic/score.midi` | 13,916 | 11 |

比较器为未提供初始速度的 SMF 添加标准默认值；源 PPQ 按已有绝对 tick 舍入规则归一化，
同一位置重复速度按导入器的轨道遍历次序取最后一条。拍号、非速度元数据、SysEx 不在
“通过”的范围内，不把这一结果表述为完整乐谱无损交换。

李斯特文件另作完整音频验收：经过 v4 保存、重开、导出后的 MIDI，再渲染 48 kHz PCM16
单声道 WAV，和上一轮缓存的原 MIDI 直接渲染 WAV **逐字节一致**：

- 帧数：44,126,071；时长：919.2931458333334 秒（含 100 ms 尾音）。
- WAV SHA-256：`07ec34e89646a1aeb1637d91b5812b9d36dd71e2deedc8209e9f0cd5b7f3474a`。
- 本地验证文件：`out/score-tempo-20260923/`（已忽略，不上传公开仓库）。
- 保留原测试音色的 524 个削波采样点；一致性不是成品音质验收。

另外运行了 MusicXML 交换检查：李斯特文件的 6,958 个音符起止、音高和力度保持一致，
报告准确显示省略 10 个后续速度条目及 7,521 个记谱片段的播放元数据。该检查不验证
MusicXML 的速度图保留；相关产物在 `out/score-tempo-xml-20260923/`。

## 复现命令

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
ctest --test-dir build --output-on-failure

# 使用安装了可选开发依赖 mido 的 Python 环境
python3 scripts/check_midi_performance.py \
  ../baroque-violin-sonata/sonata.midi \
  ../chopin-op9-no1-light/score.midi \
  ../liszt-ballade-romantic/score.midi

# 输出目录必须是新的
./build/daw_midi_roundtrip ../liszt-ballade-romantic/score.midi /tmp/new-tempo-roundtrip
./build/daw_midi_render /tmp/new-tempo-roundtrip/project.mid /tmp/new-tempo-render 48000
```

生产引擎不依赖 Python/mido，也未复制第三方实现代码。技术资料只查询了上述官方
MusicXML 页面，复用此前 MIDI 规范与本地文件；Tavily 计量调用 **0**。全部验证在本地
执行，不运行或重试 GitHub workflow。
