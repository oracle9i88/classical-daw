# 2026-09-23 MIDI 演奏事件保留

基线 `df06926` 能读取音符，但丢弃 CC、音色、弯音与触后事件。本轮修复 C++ 核心的
SMF → Score → 原生工程 → Score → SMF 路径；网页是独立数据模型，本轮未同步更新。

## 实现范围

- 保留 Control Change（含踏板/表情/Bank Select/Channel Mode）、Program Change、
  Pitch Bend、Poly Pressure、Channel Pressure。弯音保留原来的 LSB/MSB，不预设乐器弯音范围。
- 保留音符的原始通道、关音力度和开/关音消息序号。每轨控制消息与音符共用源事件顺序，
  避免同 tick 的 bank → program → note-on 或 pedal → note-off 被重新排序。
- 归一化 PPQ 时，控制事件也按绝对 tick 最近取整，单独记录 `rounded_channel_events`。
- 跨小节 tie 的第一段携带开音序号，最后一段携带关音序号/力度；合并后恢复原发声事件。
- 只含控制消息的轨道仍保留。只为它建立一个空记谱小节，晚时刻控制消息不导致巨量空小节。
- 已导入音符保留显式通道，可保存超过 16 轨的已路由工程；这不等于支持超过 16 个独立
  MIDI 通道或多个 MIDI 端口。新建、无显式通道的声部仍用原来的 part-index 后备规则。
- 工程格式 v3 保存以上字段；继续读 v1/v2，未出现的字段恢复默认值。撤销、重做、快照、
  recovery 与 interrupted `.tmp` 使用相同嵌套模型与持久化边界。

新建事件序号为零。同 tick 使用：新建 note-off → 导入事件的源顺序 → 新建控制事件的向量顺序
→ 新建 note-on。这保证新音符的关闭不会排到相邻导入音符的重新触发之后。
编辑器移动音符、改变音高或端点时应清除旧消息序号。导出器检测到旧序号与同音重触边界冲突时，
会明确拒绝并保全已有目标文件，避免写出自身无法读回的零时长音符。

同时修复两个工程读取缺陷：超范围力度在转为 uint8 前校验；`end_project` 后附加数据明确拒绝。

## 验证方法

`tests/midi_channel_events_tests.cpp` 使用手写 SMF 字节与独立输出字节解码器，覆盖所有五类
非音符消息、单/双数据字节 running status、release velocity、384 PPQ 取整、同 tick 顺序、
控制器独立轨、混合导入/新建音符的相邻重触以及失败时文件/模型保全。

`tests/score_midi_events_tests.cpp` 检查原始通道、tie 元数据、控制器独立轨、显式路由大编制和
非法事件拒绝；`tests/project_midi_events_tests.cpp` 检查 v1/v2 兼容、v3 全字段、uint64 序号、
计数上限、撤销/重做/恢复、文件截断与恶意范围值。

真实文件使用 mido 1.3.3 / Python 3.12.13 独立读取，对比每个有通道消息的轨道中，所有
消息的绝对起始 tick、数据字节和相对次序。只将 note-on velocity=0 规范化成等价 note-off。
源文件只读，生成的工程/MIDI 使用临时目录，检查后删除：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
python3 scripts/check_midi_import.py \
  ../baroque-violin-sonata/sonata.midi \
  ../chopin-op9-no1-light/score.midi \
  ../liszt-ballade-romantic/score.midi
python3 scripts/check_midi_performance.py \
  ../baroque-violin-sonata/sonata.midi \
  ../chopin-op9-no1-light/score.midi \
  ../liszt-ballade-romantic/score.midi
```

三份源文件身份见 [上一轮审计](2026-09-23-lilypond-midi.md)。原生工具
`daw_midi_roundtrip INPUT_MIDI NEW_OUTPUT_DIRECTORY` 可单独运行，保留生成文件供查看；
输出目录必须不存在，避免覆盖已有作品。

## 实测结果

本地最终验收：Debug **14/14**、AddressSanitizer + UndefinedBehaviorSanitizer **14/14** 通过。
ASan 在 macOS 使用 `detect_leaks=0`；未执行硬件音频压力测试或泄漏检测。
独立 mido 对比结果如下，两条输出路径分别为直接 SMF 往返和经过原生工程保存/重开：

| 源项目 | 音符数 | 非音符通道事件 | 检查的全部通道消息 | 直接往返 | 工程往返 |
| --- | ---: | ---: | ---: | --- | --- |
| baroque-violin-sonata | 524 | 4 | 1,052 | 通过 | 通过 |
| chopin-op9-no1-light | 1,718 | 276 | 3,712 | 通过 | 通过 |
| liszt-ballade-romantic | 6,958 | 0 | 13,916 | 通过 | 通过 |
| 合计 | 9,200 | 280 | 18,680 | 通过 | 通过 |

这里的“通过”仅指上述归一化后的通道消息数据/时间/次序一致，**不包含**后续速度、拍号、meta
或 SysEx，也不代表正弦播放器已执行这些效果。14 组测试均在本机运行，无 GitHub workflow 重试。

## 仍未实现的行为

数据保存与播放执行不同。正弦诊断播放器仍不解释 CC/踏板、program、pressure、pitch bend
或关音力度；CoreMIDI 录入仍只计数，尚未形成可录制的通道事件流。

MusicXML 当前仍是记谱子集，会丢弃这些原始 MIDI 演奏元数据；新增 `MusicXmlExportReport`
报告事件与音符元数据省略数量，不应使用 MusicXML 作为原生工程备份。Score 的单 BPM/拍号也
仍无法保留完整 tempo/meter 图。SysEx、其他 meta 数据和 MIDI 歌词不在本轮范围内。
默认声部通道 9 的 GM 打击乐规则与多端口乐器路由仍待实现。

实现参照 [MIDI Association 消息表](https://midi.org/summary-of-midi-1-0-messages)。
无第三方实现源码复制；mido 仅为可选开发验证依赖。Tavily 计量调用 **0**，未重跑 GitHub workflow。
