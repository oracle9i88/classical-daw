# MusicXML 小数时间单位：2026-09-23

本轮扩展上一轮仅接受整数源单位的限制：MusicXML divisions、duration 和播放 offset
现在可以是有限十进制小数，只要在明确的输入范围内、并且能精确换算到内部 960 tick
网格。没有改变原生 v6 工程格式、导出精度或实时音频回调。

例如 divisions=24、duration=0.5 现在导入为 20 ticks；divisions=0.5、duration=0.25
导入为 480 ticks。duration=0.5、divisions=960 只有半个内部 tick，仍明确拒绝。

## 算法与边界

- 直接把十进制文本拆为符号、整数分子和十的幂分母，不经过 binary float/double。
- 先去掉末尾小数零；剩余小数位最多 18 位。去掉小数点后的带符号整数尾数必须在
  signed 64-bit 范围内。超过范围失败，不截断。这是实现的资源/精度边界，不能误称
  为 MusicXML 对十进制位数的限制。
- 约分为规范分数，使 `+.50` 和 `0.5000` 相等，负零归一为零。源 divisions 必须
  大于零；duration 归一化后必须为正；offset 可以为负。
- 计算 `raw / divisions * 960` 时，逐对约掉所有分子、分母因子，再检查分母是否为
  1，最后执行带溢出保护的乘法。大数或小数尺度可先相互抵消，不因中间乘积过大而
  拒绝原本可表示的结果。
- 不四舍五入。例如在 divisions=0.1 时，duration=0.100000000000000001 不能被
  当作一拍接收；二者的细微差值会在精确运算中保留，并因不能落到整数 tick 而失败。
- 继承、跨 part 隔离、小节开头切换、声明顺序、偏移覆盖和失败保留目标 Score 的
  规则沿用上一轮。小节中途切换精度仍未实现。
- 内部 Score 和输出 MusicXML 仍为 960 单位；保留音乐时间，不保留原文本的精度
  拼写。速度 BPM 仍使用既有 double 模型；本次精确分数算法只用于源时间单位换算。

规范依据复用缓存的 [W3C MusicXML 4.0 schema](https://github.com/w3c/musicxml/blob/v4.0/schema/musicxml.xsd)：
divisions 是 decimal，positive-divisions 为正数限制。主 schema SHA-256 为
`bfe37ed25a9ec00e6f2591d53df260b84efe12aed209ba3ac0a76f9287665a99`。
本轮联网抓取 0 次、Tavily 计量调用 0 次，复用主 XSD 与两个依赖文件。

## 本地验证

新增 `daw_musicxml_fractional_tests`：

- 同一段包含小数精度切换、和弦、双声部、backup/forward、跨小节延音和变速的乐曲，
  与整数精度基准比较。小节边界、速度位置一致，12 kHz 浮点渲染采样完全相同；
  重新导出为 960 divisions 后仍相同。
- 将 1～100 tick 逐一表示成 divisions=2.4 下的小数 duration，逐项验证整数结果。
- 验证 18 位小数、大整数的可约分比例、负偏移、负零、等价声明、整数 divisions 下
  的分数 duration。
- 23 个失败输入，包含不能表示的子 tick、负/零时长、越界精度/尾数、科学计数法、
  超大转换结果、相差极小的矛盾声明、中途切换、backup/forward 和偏移的不可表示值。
  失败不污染原 Score。

```sh
cmake --build build -j 4
ctest --test-dir build --output-on-failure
cmake --build build-sanitize -j 4
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-sanitize --output-on-failure
```

普通测试与 ASan/UBSan 各 **25/25** 通过。加入两项浮点精度陷阱断言后，该测试目标
在普通及 sanitizer 构建中分别再跑通过。两套完整 CTest 顺序执行，避免旧测试共享
临时文件。没有运行或重试 GitHub Actions，提交继续使用 `[skip ci]`。

## 整曲等价小数样本

为已有 fixture 生成器增加 `--fractional`：先按上一轮方法选择每小节源单位，再把
所有 divisions/duration/offset 同除以 1000，用整数拼接出普通十进制文本。没有浮点
转换；默认整数模式的生成文件保持字节一致。已检查正负值、零和末尾零的拼写。

```sh
python3 scripts/make_musicxml_divisions_fixture.py --fractional \
  out/musicxml-fractional-20260923/fixtures \
  out/musicxml-tempo-20260923/corpus/roundtrip-{1,2,3}.musicxml
c++ -std=c++17 -Isrc/engine/include scripts/check_musicxml_normalization.cpp \
  build/libdaw_engine.a -o /tmp/check_musicxml_normalization
for i in 1 2 3; do
  /tmp/check_musicxml_normalization \
    out/musicxml-tempo-20260923/corpus/roundtrip-$i.musicxml \
    out/musicxml-fractional-20260923/fixtures/mixed-$i.musicxml \
    out/musicxml-fractional-20260923/check-$i
done
/opt/homebrew/opt/python@3.12/bin/python3.12 scripts/check_musicxml_schema.py \
  --schema out/musicxml-meter-20260923/schema/musicxml.xsd \
  out/musicxml-fractional-20260923/fixtures/mixed-{1,2,3}.musicxml
```

生成和比较目录要求不存在，复验需另选新路径。三份样本来自已有的巴洛克、肖邦、李斯特
LilyPond MIDI 经引擎导出的 XML；属于基于真实曲目构造的等价样本，不是新增第三方
制谱软件兼容性语料。

- 合计 **932 小节、9,422 个 XML 音符片段、13 条速度记录**。
- 源精度涵盖 0.001、0.002、0.003、0.004、0.006、0.008、0.012、0.024、0.48、
  0.96、20.16，各 part/小节独立变化。
- 三份归一化 XML 与各自基准逐字节相同；恢复 MIDI 与上一轮基准 MIDI 逐字节相同。
- 三份小数样本均通过官方 MusicXML 4.0 XSD。

小数样本 SHA-256：

- 1：`14993b40097e2e2631f9d7c78ec86f0ec16d401e083b7e4435e24612779e5f40`
- 2：`92e6edff16271c56cb6d2e6bdb49ce61c80d767d24f55c1d30fd31e5eb7120c9`
- 3：`4858891010fdfbf034c0fd1fb88c10c01b2165ad18224f790a5db2779e6f6980`

本轮没有重渲染整曲 WAV；音频采样比较在上述短曲测试完成。原有 MusicXML 交换层的
控制器、路由等省略仍然存在。这轮也不意味着支持所有 MusicXML 语法、任意精度
有理时值、分数半音、完整管弦音源或专业 DAW 的全部功能。
