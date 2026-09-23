# MusicXML 变拍交换与工程 v6：2026-09-23

后续：[MusicXML 变速交换](2026-09-23-musicxml-tempo-interchange.md) 已补上本文记录时
仍被省略的后续速度指令。以下保留变拍迭代当时的测试记录。

本轮接通按小节边界变拍的 MIDI → Score → MusicXML → Score → MIDI 路径，并补上
小节实际时长的持久化。三份本地 LilyPond 文件共 9,200 个音符、10 个规范化 n/d 拍号
条目、932 个小节的起点与时长通过往返比较；导出的三份文件通过官方 MusicXML 4.0
XSD 校验。这仍是有限的 C++ 交换层，不能据此称为完整 MusicXML 或专业 DAW 成品。

## 已实现的行为

- 在已有小节起点写出 `<attributes><time>`，保留规范化 n/d 变化。不移动原音符，
  不自动重新划分用户尚未整理的工程；落在存储小节内部或全部小节之外的结构变化拒绝导出。
- 每个 part 独立计算拍号延续与小节时间。只有共同区间的起点和有效拍号一致，才合并
  为 Score 的全局拍号图。较短声部可为共同小节序列的前缀；没有显式时长的末小节，
  可以借用较长声部已有的下一条小节边界，包括仅有控制器的空声部。
- 短小节、空小节及最后一个短小节保留实际时长。最后一个发声结束后仍有空白时，
  用 `<forward>` 明确推进到该小节终点；多声部最后的游标不是最远端时也补齐。
- 读入时将 attributes 与 note/backup/forward 按出现顺序处理。只接受位于任何
  note/forward/backup 之前的拍号声明。后置声明，即使 backup 已回到零，也明确拒绝，
  防止新拍号被错误地追溯应用到之前的音符。
- 不完整小节使用 `implicit="yes"`；读入时取全部音符/forward 的最远终点作为实际
  时长。普通小节沿用 `max(名义长度, 最远终点)`，保留既有超长声部行为。没有任何
  时长内容的空 implicit 小节无法推算长度，明确拒绝。

`implicit` 的规范含义是隐藏小节号，常见于弱起，并不直接编码时长。上面的实际长度
推算是本实现的明确规则，不把它误称为属性自身的时值语义。依据：
[W3C measure](https://www.w3.org/2021/06/musicxml40/musicxml-reference/elements/measure-partwise/)、
[attributes](https://www.w3.org/2021/06/musicxml40/musicxml-reference/elements/attributes/)、
[time](https://www.w3.org/2021/06/musicxml40/musicxml-reference/elements/time/)、
[forward](https://www.w3.org/2021/06/musicxml40/musicxml-reference/elements/forward/)。

## 工程时长与兼容

`ScoreMeasure::duration` 为小节显式总长度，包含尾部空白。`0` 保持旧版“未指定”的
含义，不根据旧文件凭空恢复已经缺失的末尾时长。大于零时要求：

- `start + duration` 不溢出；每个音符完全处在这个区间。
- 如果有下一小节，当前终点必须等于下一小节起点。
- 原始 MIDI 导入把已计算的网格跨度填入该字段；XML 导入保存解析出的实际跨度。

原生工程 v6 在每个 measure 的 `start` 后增加一行，note 自己的 duration 不变：

```text
measure 1
number 2
start 3840
duration 1440
notes 1
```

v1–v5 仍可读取，measure duration 默认为零，既有速度、拍号、歌词和通道数据按各自
版本保持。撤销、重做、恢复副本与 `.tmp` 回退都保留显式时长。旧文件缺少的末尾
空白需从原始 XML 重新导入。新增时长描述记谱范围，不改变离线渲染器原有的“最后
演奏事件加尾音”规则，也不把无声小节当成新音符。

## 明确的交换限制

当前 XML 保留 canonical n/d 图，所有 `bb` 必须为 8。`cc` 非默认初始值计一次遗漏；
后续每条 cc 非默认或 n/d 与前条相同的事件再计一次，均通过
`omitted_meter_playback_metadata` 显示。相邻相同 n/d 不生成重复记谱事件。原生工程
和 MIDI 仍保存原始四字段图。

不同声部的异拍、单独谱表的 `time@number`、复合或无拍号、小节内变拍、
`non-controlling="yes"` 仍不支持。MusicXML 允许任意 token 小节标签；本模型仍只有
正整数编号，零和非数字标签明确拒绝。XML divisions 仍要求 960；`.mxl`、完整排版、
调号、方向性力度与连续速度曲线也没有在这一轮补齐。

后续速度变化、原始 CC/program/bend 等事件、note MIDI 路由/次序/松键力度仍会在
XML 路径遗漏并报告。因此这里的音符一致性不是演奏效果或完整音频一致性结论。

## 独立校验与结果

普通 CTest **22/22**；AddressSanitizer + UndefinedBehaviorSanitizer **22/22**。
Leak detection 关闭，不把它称为泄漏检查。新增两组测试包括：

- 4/4 → 3/4 → 6/8、连音、尾部空白、空小节、多声部最远终点、末尾不足拍长。
- 缺省/重复拍号、不同 part 的冲突及缺失声明、after-backup 声明、复合/非法输入。
- 显式时长的边界/溢出、v1–v5 兼容、v6 保存和历史恢复、失败时的文件和报告保全。

`check_score_interchange.cpp` 同时比较音符和每个 part 的小节起点；原 Score 存有显式
时长时，也比较恢复后的 duration。拍号比较忽略 cc，仅验证 bb=8 的 n/d 规范化图。

| 源文件（相对仓库） | 音符 | n/d 条目 | 小节起点及显式时长 |
| --- | ---: | ---: | ---: |
| `../baroque-violin-sonata/sonata.midi` | 524 | 1 | 128 |
| `../chopin-op9-no1-light/score.midi` | 1,718 | 1 | 170 |
| `../liszt-ballade-romantic/score.midi` | 6,958 | 8 | 634 |
| 合计 | 9,200 | 10 | 932 |

李斯特文件七次后续换拍全部保留。报告仍显示省略 10 次后续速度变化、7,159 个记谱
片段的原始播放元数据，以及 4 个非默认节拍器设置；这几项不在 XML 一致性结论内。
另运行独立 mido 比较，经过 v6 原生工程的 18,680 条通道消息、13 个速度条目、10 个
完整四字段拍号条目均保持一致。

独立 XSD 校验还发现旧初始速度 direction 缺少必需的 direction-type；本轮补上
`metronome/quarter/per-minute` 后再写 sound，使输出满足
[W3C direction 结构](https://www.w3.org/2021/06/musicxml40/musicxml-reference/elements/direction/)。
三个实际输出均通过校验。故意移除 direction-type、使用截断 XML 和缺失 schema 的
负例分别返回失败，不会误报通过；源文件内容不变。

规范文件只缓存到忽略目录，未复制进生产源码。固定版本为 W3C 官方
[MusicXML v4.0 schema](https://raw.githubusercontent.com/w3c/musicxml/v4.0/schema/musicxml.xsd)，
同目录另需 xml.xsd、xlink.xsd。主 schema SHA-256：
`bfe37ed25a9ec00e6f2591d53df260b84efe12aed209ba3ac0a76f9287665a99`。
新脚本 `check_musicxml_schema.py` 使用可选 lxml，读取本地 schema 快照，全程禁用网络、
DTD 加载和实体展开，输出源文件与三个 schema 的哈希。引擎本身没有新增第三方依赖。

## 复现

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
ctest --test-dir build --output-on-failure

c++ -std=c++17 -Isrc/engine/include scripts/check_score_interchange.cpp \
  src/engine/{midi,meter_map,score,score_midi,score_tempo,tempo_map}.cpp \
  -o /tmp/check_score_interchange

# 输出目录必须尚不存在
/tmp/check_score_interchange /tmp/new-xml-meter-check \
  ../baroque-violin-sonata/sonata.midi \
  ../chopin-op9-no1-light/score.midi \
  ../liszt-ballade-romantic/score.midi

# 在装有可选 lxml 的 Python 环境中；先把三个官方 XSD 放在本地同目录
python3 scripts/check_musicxml_schema.py --schema /path/to/musicxml.xsd \
  /tmp/new-xml-meter-check/roundtrip-1.musicxml \
  /tmp/new-xml-meter-check/roundtrip-2.musicxml \
  /tmp/new-xml-meter-check/roundtrip-3.musicxml
```

本地证据在 `out/musicxml-meter-20260923/`，不上传音乐文件或生成的乐谱。复用三份
既有 MIDI；Tavily 计量调用 **0**。全部检查在本地执行，没有运行或重试 GitHub workflow。
