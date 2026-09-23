# MusicXML 源时间单位归一化：2026-09-23

本轮让 MusicXML 导入不再局限于 `<divisions>960</divisions>`。正整数源精度可在
各 part 独立设置、沿用或在小节开头改变；音符、休止符、backup、forward、声音速度
偏移统一精确换算到内部 960 tick 网格。工程格式仍为 v6，导出仍使用 960 divisions。

## 规则与边界

MusicXML 的 divisions 表示一个四分音符包含多少个源时间单位。例如 divisions=24、
duration=12 是半个四分音符，即内部 480 ticks。该比例与音符的 written type 或
连音符拼写分开；已解析的时值不会再次套用 tuplet 比例。

实现先约分 `960 / source_divisions`，再检查源值是否能整除分母，最后检查乘法的
有符号溢出。这样不会因为 `raw * 960` 中间值溢出而拒绝原本合法的比例；测试包含
`INT64_MAX / INT64_MAX` 恰好为一拍的输入。无法精确表示的时间直接失败，不逐音符
四舍五入，避免累积漂移与声部错位。此策略与已有 SMF 导入的显式舍入策略不同。

- 源 divisions 必须为正整数，源 duration 和播放 offset 使用有符号 64 位可表示的
  整数值；duration 归一化后必须大于零。`24.000`、`1.0` 等整数十进制写法可接受。
- 当前不支持分数源值，即使某个分数可以换算到整数 tick；例如 divisions=24 下的
  duration=0.5 仍明确拒绝。该限制不等于 MusicXML 规范禁止小数。
- 不要求 divisions 本身整除 960。divisions=7、duration=7 可导入为 960 ticks，
  但 duration=1 会因无法精确表示而失败。
- 每个 part 单独继承 divisions，不从前一个 part 继承。完全没有声明时保留旧版的
  960 默认值；这是兼容已有工程样本的行为，不声称是 MusicXML 标准默认值。
- 小节开头可以改变 divisions，但声明必须在该小节音符、游标移动和播放速度事件
  之前。同一个小节开头的相互矛盾声明失败。中途声明即使 backup 已回到零，也拒绝，
  防止用新单位重新解释旧声部。纯文字 direction 不占用时间，不阻止后续前置声明。
- direction 与 sound offset 使用当前 part 的精度换算，沿用上一轮已验证的播放
  开关、声音偏移覆盖规则和跨小节限制。只影响视觉的偏移不参与播放归一化。
- 导入失败保留调用者原有 Score。原始音符分组、连音符、延音线、力度、声部、速度图
  和小节时长经归一化后由现有模型继续处理。

依据复用了已缓存的 [W3C MusicXML 4.0 schema](https://github.com/w3c/musicxml/blob/v4.0/schema/musicxml.xsd)
中 divisions、attributes/divisions、backup 的定义。标准允许 decimal，并说明 backup
时长不应跨越小节或小节内 divisions 变化。本实现支持上面明确列出的整数及前置声明
子集。本轮联网抓取 0 次，Tavily 计量调用 0 次，复用一套缓存 XSD（主文件及两个依赖）。

## 本地检查

新增 `daw_musicxml_divisions_tests`，覆盖：

- 4、12、24、48、96、384、480、960、1920、10080 共十种源精度，分别测试继承以及
  第二小节切换为两倍精度。相同音乐共二十个等价输入，包含弱起、双声部、forward、
  backup、跨小节延音、和弦和两次速度变化。音符表现和 12 kHz 浮点渲染采样完全一致。
- 同曲不同 part 使用不同源精度、每 part 的旧版默认隔离、三连音整数算术、休止符、
  time-modification 拼写、高范围可约分输入和归一化后再次导出的播放一致性。
- 21 个拒绝案例：零/负数/分数/科学计数法/超范围/空精度，重复或矛盾声明，中途及
  backup 后声明，速度之后声明，不能精确换算的音符/forward/backup/tempo offset，
  正负乘法溢出、分数时长、零时长。失败保持目标工程原状态。

```sh
cmake --build build -j 4
ctest --test-dir build --output-on-failure
cmake --build build-sanitize -j 4
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-sanitize --output-on-failure
```

普通与 ASan/UBSan 两套 CTest 均 **24/24** 通过，顺序执行，避免旧测试共享临时文件。
补充休止符/连音符断言后，两种构建中的该目标均再次通过。未运行或重试 GitHub Actions。

## 从真实曲目构造等价精度样本

使用上一轮由三份 LilyPond MIDI 导出的规范 XML，按小节改写时间单位；这不是新找到的
第三方 MusicXML 兼容性语料，不能据此声称全面兼容所有制谱软件。

`scripts/make_musicxml_divisions_fixture.py` 用独立的 Python 整数算法计算每小节最小
可精确精度，并与 20160 交替，各 part 独立选择。只调整 divisions、duration、offset；
不改音高、速度值、歌词、延音或其他元数据。要求源文件每 part 保持 960 divisions，
输出目录必须不存在。`check_musicxml_normalization.cpp` 将基准与变体分别导入、重新
输出成规范 960 XML，逐字节比较，并输出变体恢复的 MIDI。

```sh
python3 scripts/make_musicxml_divisions_fixture.py out/musicxml-divisions-20260923/fixtures \
  out/musicxml-tempo-20260923/corpus/roundtrip-{1,2,3}.musicxml
c++ -std=c++17 -Isrc/engine/include scripts/check_musicxml_normalization.cpp \
  build/libdaw_engine.a -o /tmp/check_musicxml_normalization
for i in 1 2 3; do
  /tmp/check_musicxml_normalization \
    out/musicxml-tempo-20260923/corpus/roundtrip-$i.musicxml \
    out/musicxml-divisions-20260923/fixtures/mixed-$i.musicxml \
    out/musicxml-divisions-20260923/check-$i
done
```

复验需更换所有已存在的输出目录。三个比较全部通过：

| 基准曲目 | 小节 | XML 音符片段（含延音拆分） | 速度记录 | 变体出现的 divisions |
| --- | ---: | ---: | ---: | --- |
| 巴洛克奏鸣曲 | 128 | 524 | 1 | 1、2、20160 |
| 肖邦 | 170 | 1,739 | 1 | 1、2、6、480、960、20160 |
| 李斯特 | 634 | 7,159 | 11 | 1、2、3、4、6、8、12、24、480、960、20160 |

合计 932 小节，归一化输出逐字节一致。恢复 MIDI 也与上一轮规范 XML 恢复的 MIDI
逐字节一致。此次没有重新渲染整曲 WAV；短曲浮点采样比较已在新增测试中完成。
上轮原始 MIDI 的 9,200 个音符与这里的 9,422 个 XML 片段是延音合并前后两种计数。

三份变体通过缓存的官方 MusicXML 4.0 XSD：

```sh
/opt/homebrew/opt/python@3.12/bin/python3.12 scripts/check_musicxml_schema.py \
  --schema out/musicxml-meter-20260923/schema/musicxml.xsd \
  out/musicxml-divisions-20260923/fixtures/mixed-{1,2,3}.musicxml
```

变体 SHA-256：

- 1：`74a0447edeeaaac97c56a3cd3f5f9efe43fed8863a571e0bbf4ce644b3928e01`
- 2：`a94ce616a8bfff1e448ac5e0fa239b6317e66c0049f72b8e7ea136cd017ea5de`
- 3：`95570b9bd3fc672554f800e25af237d5640834eb0abdbaab7faf35147fab8025`

另将第一个样本的一个音高修改后，比较器正确退出 1；对已有输出目录重运行生成器也
正确拒绝。源曲目没有修改。原 XML 交换层会省略的 MIDI 控制器、路由和消息顺序不在
这轮比较范围内。小节文字编号、分数源时间、中途精度切换、更完整的 MusicXML 语法、
正式音源、实时播放和编辑界面仍需继续实现。
