# 2026-09-23：LilyPond MIDI 与交响乐交换审计

## 范围与结论

后续更新：通道事件及工程格式 v3 的保留修复见
[MIDI 演奏事件保留记录](2026-09-23-midi-performance-events.md)；本文件保留本轮基线与当时验收结果。

本记录审计 C++ 引擎，不把网页功能等同于引擎功能。代码基线为
`7e471f55839cb7568a43608ab0bc255d5284cec2`；以下缺口描述这个基线，后续修复应以回归检查为准。
基线审计为本地代码、测试和现有文件检查；后续修复查阅了下方列出的官方规范。
计量搜索（Tavily）调用为 0，未触发 GitHub workflow。

现有实现足以证明基础 MIDI/MusicXML 交换路径，但还不能保证交响乐的演奏语义往返不变。
最先要解决的是非 960 PPQ 的导入与延音线的播放合并，再补通道事件、速度/拍号图和乐器路由。
“能打开文件”与“音符、时间和演奏控制保持正确”是两个不同的验收条件。

## 真实本地文件证据

以下路径相对于 `classical-daw/` 仓库根目录。这些文件位于相邻项目，**不在公开仓库内**，
不能假设其他开发者克隆后即可使用；可移植回归应另建最小合成夹具，不直接复制完整作品。
这里的 MIDI header 字段和 SHA-256 是本轮实际读取结果：

| 相对路径 | SMF format | 轨数 | PPQ | SHA-256 |
| --- | ---: | ---: | ---: | --- |
| `../baroque-violin-sonata/sonata.midi` | 1 | 3 | 384 | `ff115a34afcf7c9dffa14ec57fbcddea0aa78f3a43b9434d7a2e9d90f5dddc3a` |
| `../chopin-op9-no1-light/score.midi` | 1 | 3 | 384 | `fc4472f982019cae7a2d4e04095aa8fb208f4930f56c5d710967fdba9976c62d` |
| `../liszt-ballade-romantic/score.midi` | 1 | 3 | 384 | `d346e05d5de310cc990df85a14deea31cdc4de01012268a9bd820cb78e89a8cc` |

配套源文件分别为 `../baroque-violin-sonata/sonata.ly`、
`../chopin-op9-no1-light/score.ly` 与 `../liszt-ballade-romantic/render-score.ly`。
第一个项目的 `verification.json` 记录 LilyPond 2.24.4，源谱的 MIDI score 使用
`\unfoldRepeats`，并指定 violin/cello 音色。因此测试反复段应比较展开后的演奏事件，
不能从 MIDI 反推原谱的反复记号。

在仓库根目录执行以下只读命令，可复核 header 和文件身份：

```sh
python3 - <<'PY'
from pathlib import Path
import hashlib

paths = (
    "../baroque-violin-sonata/sonata.midi",
    "../chopin-op9-no1-light/score.midi",
    "../liszt-ballade-romantic/score.midi",
)
for name in paths:
    data = Path(name).read_bytes()
    assert len(data) >= 14 and data[:4] == b"MThd", name
    header_length = int.from_bytes(data[4:8], "big")
    assert header_length >= 6 and len(data) >= 8 + header_length, name
    fmt = int.from_bytes(data[8:10], "big")
    tracks = int.from_bytes(data[10:12], "big")
    division = int.from_bytes(data[12:14], "big")
    assert division > 0 and not division & 0x8000, name
    print(name, f"format={fmt}", f"tracks={tracks}", f"ppq={division}",
          f"sha256={hashlib.sha256(data).hexdigest()}", sep="\t")
PY
```

## 按优先级排列的缺口

### P0：接受常见 PPQ，并明确时间精度

基线 `src/engine/midi.cpp:235` 与 `src/engine/score_midi.cpp:176` 拒绝非 960 PPQ；
上述三个 384 PPQ 文件因此不能走导入链。这是代码分支与真实 header 的直接对照，
本轮未把“改 header 为 960”当作修复：只改 header 会改变播放速度。

修复验收：

- 覆盖 384、480、960 PPQ，保留音符、轨名、通道、力度与 tempo 事件的绝对位置。
- 对绝对 onset/end 分别换算，禁止逐 delta 舍入造成累计漂移；计算须检查溢出。
- 384→960 比例为 5/2，奇数源 tick 无法精确落在内部整数 tick。
  必须声明舍入规则和误差边界，不得称任意输入“无损”。128 源 tick 的三连音应精确成为 320 tick。
- 对高 PPQ 短音舍入为零时长、源 PPQ 为零、SMPTE division、极大 tick 设置明确结果。
- 在无法精确表示时保留诊断；不要通过隐式最小时长修正掩盖变化。

### P0：延音线合并为一个持续发声事件

基线 `scoreToMidiFile` 在 `src/engine/score_midi.cpp:109–115` 逐音输出，未读取
`tie_start/tie_stop`。跨小节延音会再次触发 Note On；`renderScore` 使用同一转换路径，
也会受影响。MusicXML 中保留了 tie 字段，不代表播放已执行 tie。

修复验收：

- 两段、三段、跨小节 tie chain 仅输出一个 Note On/Off，时长等于整条链。
- 按 part、staff、voice、音高及连续时间匹配，不能把不同声部的同音错误合并。
- 起始力度决定攻击；连线后续段的不同力度不得制造新的攻击。
- 检查孤立 stop、未闭合 start、不同音高、不连续时间与溢出；明确拒绝或诊断策略。
- 不带 tie 的相邻同音仍是两次发声；普通 slur 不等同于 tie。

### P1：保存乐器与连续演奏控制

基线 `src/engine/midi.cpp:322–345` 正确消耗 Program Change/Pressure 的字节，
但不保存这些事件；CC、Pitch Bend 也不进入模型。`MidiTrack` 只有轨名和音符。
因此文件可以解析，但往返会丢音色、踏板、表情、声像与调制等信息。

验收应包含带时间戳的 Program Change、CC 1/7/10/11/64、Pitch Bend，比较事件值、
通道、绝对 tick 和同 tick 的相对顺序。先保留交换数据，再分别声明哪些事件参与播放。
Note Off release velocity 若暂不保留也应记录为限制。

### P1：乐器路由不等于轨道下标

基线 `score_midi.cpp:87/108` 限制 16 parts，并按 `part_index` 分配 0..15。
这使第十声部进入 GM 打击乐通道 9；16 声部上限也不足以覆盖完整交响乐编制。
MIDI→Score 用 channel+1 作为 voice，Score→MIDI 却按 part 重分配通道；
Type 0 文件中的多通道语义因而不能往返。

验收应覆盖 Type 0 的同轨多通道、两个轨共用通道、明确打击乐轨、十个旋律声部、
超过 16 声部的端口/路由策略。应显式建模轨、乐器、通道与端口，不静默复用通道。

### P1：速度图、拍号图和弱起

SMF 层已有 tempo map，但基线 `midiToScore` 仅保留第一个 BPM（189–203）；
`midi.cpp:294–308` 仅保留第一个拍号。MusicXML 遇不同 tempo/meter 会报冲突。
`Score` 的单 BPM/拍号无法表达交响乐中途速度、变拍和渐慢。

基线 MusicXML 读取在 `score.cpp:546–551` 使用 `max(名义小节长度, cursor)`，
不足拍弱起会被补成长小节。MIDI→Score 则将跨小节长音整个放入起始小节，未拆分为 tie。

验收应覆盖 4/4→3/4、tempo 变化前后的 tick→秒积分、弱起后第一完整小节、
多声部末尾 cursor 与实际最长声部不相同、跨两条小节线长音。谱面片段与持续发声事件
应分开建模；MIDI 不自带完整谱面语义，不能声称自动还原所有连音/弱起记谱。

### P1：C++ MusicXML 的力度往返

MIDI note velocity 与 Score→MIDI 已保留力度；基线 C++ MusicXML `writeNote` 与
读取路径没有写/读力度，回读得到默认值 100。网页支持不能作为 C++ 核心已支持的证据。
用至少三个不同力度音符验证 Score→MusicXML→Score，且区分 note velocity 与渐强曲线。

### P2：异常文件与同音重叠的诊断

基线 `midi.cpp:338–351` 忽略孤立 Note Off，并在 EOT 为未关闭音符合成尾长。
同通道同音重叠以 LIFO 配对；普通 MIDI 不含 note ID，需要公开配对策略，不能承诺
在所有重叠情况下恢复作者意图。Type 0 reader 也尚未与 writer 一样拒绝多 track header。

验收应覆盖孤立 Note Off、悬挂 Note On、零时长、同音交叠、同 tick 重触、
EOT 后仍有轨内数据与 Type 0 多轨。导入诊断要与“成功读到一部分音符”分开返回，
避免用户误认为源文件完全正确。

## 已有测试的覆盖与边界

`tests/test_engine.cpp` 已覆盖基础 Type 0/1、running status、单字节事件边界、
两轨名字/通道、固定 6/8、三连音元数据、多个 voice/part、tempo map 与部分非法输入。
多数夹具为自写后自读，现有三连音检查只证明 640 tick 与 3:2 元数据可往返；
现有 tie fixture 甚至是不同音高的 start/stop，因此不是延音播放正确性的证据。
本轮审计未重新执行测试，也未把现有通过率当成上述缺口已经解决。

在本地执行现有检查，不需要 GitHub Actions：

```sh
cmake -S . -B build-local-interop -DCMAKE_BUILD_TYPE=Debug
cmake --build build-local-interop --parallel 4
ctest --test-dir build-local-interop --output-on-failure
```

## 本轮与后续验收要求

本轮先由 PPQ 与 tie 两个实现任务补回归；这份记录不提前宣布它们已修好。
验收结果应注明基线/修复提交、检查命令、实测通过的场景与仍未支持的语义。

真实文件验收不能只比文件字节或只听正弦预览。至少逐轨比较：

1. 音符数量、pitch、velocity、channel，以及绝对 onset/end 的四分音符单位值。
2. tick→秒后的关键速度边界与曲终时间；误差须符合声明的量化规则。
3. tempo/meter、Program Change 与控制器事件；未建模事件要报告丢失，不可默认为相等。
4. 导入失败时原工程不变；导出失败时已有目标文件不被损坏。

CI 中应使用可独立运行的最小合成夹具，并按语义断言预期值；三个相邻真实项目文件
适合作为本机额外验证，不应成为公共测试环境的隐式依赖。

## 修复后的验收记录

本节记录同日修复后的结果；前文的行号和缺口保留为基线证据。

- SMF Type 0/1 正 PPQ 1–32767 导入后统一为 960 PPQ。按绝对起止时间分别取最近整数，
  恰好半 tick 向上取整；不累计 delta 舍入。无法表示的零时长短音明确拒绝。
- `MidiImportReport` / `daw_midi_inspect` 报告舍入、被忽略的通道/meta/SysEx/后续拍号事件与同音重叠。
- 连续 tie 按 part/staff/voice/音高合并为一次持续发声；跨小节 MIDI 长音拆为带 tie 的记谱片段。
  同轨同通道同音高重叠在 Score 转换入口明确拒绝；普通 MIDI 读取仍以 LIFO 配对并报告歧义。
- MIDI 发音力度限定为 1–127，零力度不得被写成意外的关音指令；休止符可为零。
  MusicXML 自身允许保存零动态，非零力度经标准 `note dynamics` 百分比往返。
- 修复真实钢琴 MIDI 中“长音持续时短音进入”的 MusicXML 导出失败。
  不同起点用 `backup` 保持时序；同起点按时长降序输出，满足和弦附加音不能比前音更长的规则。
  读取后用所有声部到达的最大时刻推进小节，避免由最后一个短声部决定小节长度。

最终本地 Debug **11/11**、AddressSanitizer + UndefinedBehaviorSanitizer **11/11** 通过。
macOS 上 ASan 使用 `detect_leaks=0`；这不是泄漏检测或音频硬件压力测试。命令：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-sanitize --parallel 4
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-sanitize --output-on-failure
```

使用 **mido 1.3.3 / Python 3.12.13** 作为独立 SMF 读取参考，逐轨比较音符起止、音高、力度、
通道及 tempo 位置。下面的半 tick 是引擎 tick（1/960 四分音符），不是半拍：

| 文件 | 音符数 | tempo 位置数 | 需舍入的音符边界数 | 最大误差/tick | 忽略通道事件 | 忽略后续拍号事件 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| baroque-violin-sonata | 524 | 1 | 0 | 0 | 4 | 0 |
| chopin-op9-no1-light | 1,718 | 1 | 83 | 0.5 | 276 | 0 |
| liszt-ballade-romantic | 6,958 | 11 | 259 | 0.5 | 0 | 7 |

三份共 **9,200 个音符**的上述字段全部匹配；这不表示被忽略的事件也已保留。
复验脚本使用可选开发依赖 mido，源作品不复制、不修改：

```sh
python3 scripts/check_midi_import.py \
  ../baroque-violin-sonata/sonata.midi \
  ../chopin-op9-no1-light/score.midi \
  ../liszt-ballade-romantic/score.midi
```

随后运行 MIDI → Score → MusicXML → Score → MIDI 的完整转换链：三份分别 **524 / 1,718 /
6,958** 个音符，按非空轨的起止 tick、音高、力度多重集合比较，全部一致。空 conductor 轨
按既有规则省略；这项检查允许已知的通道重分配，**不验证速度/拍号图或控制器语义**。
源文件 SHA-256 在检查前后相同。仓库内保留通用复验工具，输出目录必须不存在：

```sh
interop_dir="$(mktemp -d /tmp/classical-daw-interchange.XXXXXX)"
c++ -std=c++17 -Isrc/engine/include scripts/check_score_interchange.cpp \
  build/libdaw_engine.a -o "$interop_dir/check-score"
"$interop_dir/check-score" "$interop_dir/results" \
  ../baroque-violin-sonata/sonata.midi \
  ../chopin-op9-no1-light/score.midi \
  ../liszt-ballade-romantic/score.midi
```

此完整链检查使用本引擎自己的 MusicXML 读写器，不能替代独立软件互操作或完整 XSD 验证。
它与前面的独立 mido 导入对照、手工字节夹具和固定预期时序回归互相补充。

尚未完成：CC/踏板/表情、Program Change、Pitch Bend 的数据保留与播放，完整速度/拍号图在
Score/工程中的持久化，乐器/通道/端口路由，弱起、键调拼写和完整谱面语义。
正弦诊断音色不能用于验收真实管弦乐音质。当前修复没有让整个项目变成商用成熟 DAW。

本轮实现依据（只读官方资料；无第三方源代码复制）：

- [LilyPond 2.24 Creating MIDI files](https://lilypond.org/doc/v2.24/Documentation/notation/creating-midi-files)：演奏事件与展开反复。
- [MusicXML note](https://www.w3.org/2021/06/musicxml40/musicxml-reference/elements/note/)：动态百分比、子元素顺序。
- [MusicXML tie](https://www.w3.org/2021/06/musicxml40/musicxml-reference/elements/tie/)：发声延音标记。
- [MusicXML chord](https://www.w3.org/2021/06/musicxml40/musicxml-reference/elements/chord/)：和弦时长与光标语义。
- [MusicXML backup](https://www.w3.org/2021/06/musicxml40/musicxml-reference/elements/backup/)：正时长的光标回退。
