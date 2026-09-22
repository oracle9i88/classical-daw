# 工程变拍与小节网格：2026-09-23

本轮让 MIDI 中的完整拍号图经过 Score、原生工程 v5、保存重开、历史恢复和 MIDI
再导出。音符按实际变拍位置划分小节和连音；不会为了对齐小节移动音符的绝对起止。
这是 C++ 引擎功能，尚未接入网页编辑器或 macOS 图形界面。

## 数据契约

`TimeSignature` 保留 SMF `FF 58` 的四项值：分子 `n`、实际分母 `d`、每次节拍器
点击的 MIDI clock 数 `cc`，以及一个 MIDI 四分音符对应的记谱三十二分音符数 `bb`。
线上第二字节是分母的二进制指数；模型和项目文件存实际分母。
字段与默认值参照 [Mido 官方元事件实现](https://github.com/mido/mido/blob/main/mido/midifiles/meta.py)
和 [Mido 文档](https://mido.readthedocs.io/en/latest/meta_message_types.html#time-signature-0x58)。
未复制第三方实现代码，也未增加生产依赖。

- 初始 `time_signature` 只负责 tick 0。没有初始事件时使用 `4 4 24 8`；第一个事件
  如果发生在正 tick，不把它提前到曲首。
- `meter_changes` 存严格递增、正 tick 的后续条目，最多 1,000,000 个。模型调用者
  提供的重复位置、乱序或非法字段直接拒绝，不在保存时悄悄重排。
- 分子为 1–255；分母为 1–128 的二次幂；`cc` 为 0–255；`bb` 为 1–255。
  超出这一引擎范围的 SMF 明确失败，包括 `bb=0`。
- 文件导入按绝对位置归一化到 960 PPQ，最近整数、半 tick 向上。同一规范化 tick
  的多条事件按轨道遍历次序取最后一条，包括不同原始 tick 舍入后的重合。
- 原始显式拍号事件最多 1,000,001 条，包含重复事件。额外的一条用于初始事件，
  使最大合法模型导出的文件仍可回读。导入后的正 tick 地图另受一百万条上限约束。
- `preserved_time_signature_events` 统计已处理的显式输入条目，包括重合条目；
  `rounded_time_signature_events` 统计位置舍入；`coalesced_time_signature_events`
  统计规范化位置重合。隐含的初始 4/4 不计入这三个输入计数。

检查器的 `meter_changes` JSON 包含初始项，每项为 `[tick,n,d,cc,bb]`；旧 `meter`
二元素字段保留兼容。渲染报告也输出新的导入计数。

## 小节和播放

引擎使用以下小节长度：

```text
measure_ticks = 960 * 32 * n / (d * bb)
```

`bb=8` 时得到通常的 `960 * 4 * n / d`。`cc` 只描述节拍器分组，不改变小节长度。
MIDI-to-Score 要求结果是精确整数 tick；不能表示的长度明确失败，原始 MIDI 模型、
项目保存和离线渲染仍可保留这些合法字节，不强行舍入记谱。

网格策略是本引擎的明确选择：`n/d/bb` 变化的位置开始新小节。如果落在旧小节中途，
旧小节在那里提前结束。重复拍号或仅 `cc` 改变不会重启小节；比如 3/4 → 6/8 虽然
总长度一样，仍属于结构变化。网格最多每个声部一百万小节，最终延音分段也有一百万
片段上限，tick 加法检查溢出。没有音符的控制器轨只创建一个可编辑小节。

跨界音符分成带 tie 的记谱片段，首段保留原 note-on 次序，尾段保留 note-off 次序
和松键力度。再次导出或渲染时合成一次持续发声。拍号、`cc`、`bb` 本身不改变经过
时间；经过时间仍由 MIDI 四分音符 tick 和速度图决定。

这是导入时构建网格的能力。修改现有工程拍号后的编辑命令、重新分小节和图形交互
仍需实现，不能把新增向量当成已完成的拍号编辑器。

## 工程 v5 与 XML 边界

v5 仍保留 v4 的速度段，在 `parts` 前写入：

```text
meter 6 4 72 8
meter_changes 2
meter_change 104160 4 4 24 8
meter_change 169440 6 4 72 8
```

读取 v1–v4 时保留原来的二字段拍号，补上 `cc=24, bb=8` 和空的后续拍号图；v4 的
速度图仍保留。旧文件里已经丢弃的拍号事件不能凭空恢复，需要重新导入原始 MIDI。
撤销、重做、清理历史时保存的当前状态以及恢复/临时文件都保留新图；完整历史栈仍
不写盘。

当前 MusicXML 实现只有固定拍号。若有任何后续拍号条目，或初始 `bb != 8`，导出在
写文件、更新报告之前明确失败，避免小节错位。只有初始 `cc != 24` 时允许输出记谱，
并把 `omitted_meter_playback_metadata` 设为 1。原有后续速度和通道元数据遗漏仍报告。
**变拍 MusicXML 导入/导出尚未实现**；使用 MIDI 或原生工程保留这些信息。

## 验证结果

- 普通 CTest **20/20**；AddressSanitizer + UndefinedBehaviorSanitizer **20/20**。
  `detect_leaks=0`，不把结果表述成泄漏检查。
- 新增三个测试目标：独立 SMF 字节解码及冲突/舍入/上限；变量网格、部分小节、连音
  与声音一致性；项目 v5、v1–v4 兼容和历史恢复。失败时保护模型、报告和既有文件。
- 本地 MIDI 渲染 CLI **14 项检查通过**。
- 独立 mido 1.3.3 比较三个既有 LilyPond 文件。原文件只读，未复制进公开仓库。

| 源文件（相对仓库） | 音符 | 通道消息 | 速度条目 | 拍号条目 |
| --- | ---: | ---: | ---: | ---: |
| `../baroque-violin-sonata/sonata.midi` | 524 | 1,052 | 1 | 1 |
| `../chopin-op9-no1-light/score.midi` | 1,718 | 3,712 | 1 | 1 |
| `../liszt-ballade-romantic/score.midi` | 6,958 | 13,916 | 11 | 8 |
| 合计 | 9,200 | 18,680 | 13 | 10 |

表中地图条目含初始值；通道消息含 note-on/off。直接 SMF 往返和经过 v5 工程的往返
均保留通道数据与相对次序、规范化速度图、拍号四字段及位置。其他元事件、SysEx、
原始指挥轨位置和重复地图事件的原始排列不属于这个一致性结论。

李斯特源文件的初始拍号是 `6/4, cc=72, bb=8`，后续七次在 6/4 与 4/4 之间切换。
第一次位于 tick 104160，相对于曲首 6/4 网格并非完整小节边界，需要部分小节策略。
全部八项经 v5 保存/重开后保留。再次导出的 MIDI 渲染成 48 kHz PCM16 单声道 WAV，
与既有原 MIDI 直接渲染缓存 **逐字节相同**：

- 44,126,071 帧；919.2931458333334 秒（含 100 ms 尾音）。
- SHA-256：`07ec34e89646a1aeb1637d91b5812b9d36dd71e2deedc8209e9f0cd5b7f3474a`。
- 新验证目录：`out/score-meter-20260923/`，受 gitignore 排除。
- 原缓存：`out/midi-control-render-20260923/liszt/diagnostic.wav`。
- 仍有原正弦诊断音色的 524 个削波采样点；这是行为一致性，不是成品音质验收。

## 复现

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
ctest --test-dir build --output-on-failure

# 使用安装了可选开发依赖 mido 的 Python 环境
python3 scripts/check_midi_import.py \
  ../baroque-violin-sonata/sonata.midi \
  ../chopin-op9-no1-light/score.midi \
  ../liszt-ballade-romantic/score.midi
python3 scripts/check_midi_performance.py \
  ../baroque-violin-sonata/sonata.midi \
  ../chopin-op9-no1-light/score.midi \
  ../liszt-ballade-romantic/score.midi

# 输出目录必须尚不存在
./build/daw_midi_roundtrip ../liszt-ballade-romantic/score.midi /tmp/new-meter-roundtrip
./build/daw_midi_render /tmp/new-meter-roundtrip/project.mid /tmp/new-meter-render 48000
python3 scripts/check_midi_render_cli.py
```

复用三份本地 MIDI 和一份既有 WAV 缓存；资料只查官方文档/源码，Tavily 计量调用 **0**。
所有构建、回归和渲染在本地完成，不调用 GitHub workflow。
