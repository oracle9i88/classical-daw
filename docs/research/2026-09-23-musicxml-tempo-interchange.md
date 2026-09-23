# MusicXML 小节内变速交换：2026-09-23

这轮修复原来 MusicXML 只写初始 BPM、丢掉后续速度的问题。三份本地 LilyPond MIDI
经过 MIDI → Score → MusicXML → Score → MIDI，13 条速度记录的 tick 与 double BPM
全部一致；9,200 个音符及此前已接通的变拍、小节时长也通过比较。李斯特全曲往返后的
诊断 WAV 与原 MIDI 的缓存诊断 WAV 字节哈希相同。

这些证据覆盖有限的交换与离线时序功能，不能据此称为完整 MusicXML、交响乐音源或
商业就绪的专业 DAW。工程格式仍为 v6，已有字段足以存储本轮速度图。

## 行为与范围

- 导出全局速度图一次，放在最长、与其他声部同步的小节序列中。每个速度记录使用
  quarter metronome、`offset sound="yes"`、`sound tempo`，小节内偏移不拆开长音。
- 小节、note、backup、forward、direction 和独立 sound 按结构与顺序处理。
  sound 子节点不会被误认为另一条 measure 事件，声音偏移不会被误认为方向的偏移。
- direction offset 只有 `sound="yes"` 才改变播放位置；省略或 `no` 是视觉偏移。
  sound 自己的 offset 优先于 direction offset，始终按声音时间偏移解释。
  支持小节内部的正、负整数偏移，以及 `960.000` 这样的整数十进制写法。
  视觉偏移可有小数。速度指令不推进音符游标，也不扩大实际小节时长。
- 多个 part 的速度声明按全局 tick 合并。同一 tick、相同 BPM 合并；同 tick 不同
  BPM 明确失败。不同 tick 的重复 BPM 保留。首次声明晚于 tick 0 时，之前维持
  模型默认 120 BPM，不把后面的速度追溯到曲首。
- 没有 sound tempo 时，可从简单数值 metronome 推出四分音符 BPM，支持 maxima
  至 1024th 及至多三个附点。有 sound tempo 时以其为播放依据，显示文字不覆盖它。
- 接受正 BPM，上限 1,000,000；per-minute 数值也受此输入上限约束。输出使用可还原
  double 的普通十进制，不用 XSD decimal 不允许的科学计数法。
- 小节边界及最后小节终点的速度可保留。超出全部存储小节范围的速度导出失败；不扩写
  小节或静默省略。`omitted_tempo_changes` 为兼容已有 API 保留，成功导出时为零。
- 明确拒绝：实际播放偏移的小数 tick、跨小节偏移、tick 溢出、同 tick 矛盾速度、
  `time-only` 反复轮次速度、无 sound tempo 的文字/范围节拍器、复杂节拍等式。
  连续渐快渐慢、反复展开、文字 rit./accel. 解释仍未实现。失败保留调用者的 Score；
  导出验证失败保留已有文件与报告。

依据为此前缓存的 [W3C MusicXML 4.0 XSD](https://github.com/w3c/musicxml/blob/v4.0/schema/musicxml.xsd)
中 direction、sound、offset、metronome 定义，尤其是 offset 的播放开关和 sound
偏移的优先级。schema SHA-256：
`bfe37ed25a9ec00e6f2591d53df260b84efe12aed209ba3ac0a76f9287665a99`。
本轮复用缓存的 XSD 及其两项依赖，联网抓取 0 次，Tavily 计量调用 0 次。

## 本地验证

新增 `daw_musicxml_tempo_tests`，覆盖游标前进/回退、声音偏移覆盖、视觉小数偏移、
缺省初速、弱起、多 part 稀疏与重复声明、附点换算、sound 优先、冗余及高精度速度、
最后终点记录、较短 part 不补长，以及 21 个非法输入和读写失败不污染已有状态的检查。
单长音在 tick 480、960 变速，实测持续 2.75 秒；加 0.1 秒尾音后，12 kHz 输出
34,200 帧，XML 往返前后浮点 samples 完全相同，没有重起音。

```sh
cmake --build build -j 4
ctest --test-dir build --output-on-failure
cmake --build build-sanitize -j 4
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-sanitize --output-on-failure
```

普通构建与 ASan/UBSan 各 **23/23** 通过。两套 CTest 顺序执行，避免老测试共享
临时文件名。没有运行、重试或手动触发 GitHub Actions；提交使用 `[skip ci]`。

真实语料比较脚本已加完整速度图检查，并输出恢复的 MIDI 供后续音频验证：

```sh
c++ -std=c++17 -Isrc/engine/include scripts/check_score_interchange.cpp \
  src/engine/{midi,meter_map,score,score_midi,score_tempo,tempo_map}.cpp \
  -o /tmp/check_score_interchange
/tmp/check_score_interchange out/musicxml-tempo-20260923/corpus \
  ../baroque-violin-sonata/sonata.midi \
  ../chopin-op9-no1-light/score.midi \
  ../liszt-ballade-romantic/score.midi
```

输出目录要求不存在；复验时另选新目录。结果为：

| 文件 | 音符 | 速度记录（含初始） | 规范化 n/d 拍号 | 小节起点与时长 |
| --- | ---: | ---: | ---: | ---: |
| baroque-violin-sonata | 524 | 1 | 1 | 128 |
| chopin-op9-no1-light | 1,718 | 1 | 1 | 170 |
| liszt-ballade-romantic | 6,958 | 11 | 8 | 634 |
| 合计 | 9,200 | 13 | 10 | 932 |

音符按每条非空轨比较 start/end/pitch/velocity，速度比较标准化至 960 PPQ 后的全部
位置和 BPM。该脚本不验证 MIDI 控制器、路由或消息顺序的 XML 保留；当前 XML 仍
省略这些字段，报告中的 channel-event 省略数依次为 4、276、0，note metadata
省略数为 524、1,739、7,159，meter metadata 为 0、1、4。三个文件的 tempo 省略
数现在全部为 0。原始 384 PPQ 的量化边界损失仍按既有 MIDI 导入报告说明。

使用可用的 Python 3.12 与 lxml 校验导出的三份 MusicXML：

```sh
/opt/homebrew/opt/python@3.12/bin/python3.12 scripts/check_musicxml_schema.py \
  --schema out/musicxml-meter-20260923/schema/musicxml.xsd \
  out/musicxml-tempo-20260923/corpus/roundtrip-{1,2,3}.musicxml
```

三份均通过官方 4.0 XSD。对应 XML SHA-256：

- 1：`7aaee57b814aaa637e57d29cb388d14f7b0aa417a0897aa1b94848cc3dbb4ea8`
- 2：`dd480eba768cfc0a7c27a5cc77253ae5aa1d316d50c98c08d63437d921930637`
- 3：`e2fde2af0987aa0a3afd1d6f575287e170e56495b5f5755ee1d8d5340f5202da`

## 整曲播放时序验证

```sh
build/daw_midi_render out/musicxml-tempo-20260923/corpus/roundtrip-3.mid \
  out/musicxml-tempo-20260923/render 48000
```

将新 `render/diagnostic.wav` 与此前
`out/midi-control-render-20260923/liszt/diagnostic.wav` 比较：

- 均为 48 kHz、44,126,071 帧、919.2931458333334 秒，含 0.1 秒尾音。
- 两份 SHA-256 均为
  `07ec34e89646a1aeb1637d91b5812b9d36dd71e2deedc8209e9f0cd5b7f3474a`。
- 诊断报告均为 524 个削波采样，0 个曲末强制释放声部。

这是确定性的单声道正弦测试。该李斯特输入没有非音符通道消息，因此不能把它推广
为“带踏板或乐器路由的所有曲目经 XML 后音频无损”。肖邦文件有 276 条被省略的
控制器消息，本轮没有对它作音频无损宣称。正式交响音源、多端口路由、实时引擎与
编辑界面仍需继续实现。
