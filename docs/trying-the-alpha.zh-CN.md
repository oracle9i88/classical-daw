# 古典音乐爱好者试用入口

这是开发中的开源 Alpha。适合愿意使用命令行、检查导入结果的试用者；还不是可以替代成熟 DAW 的桌面软件。代码按仓库现有 AGPL-3.0 许可证开放，许可证本轮没有改变。

## 不装音源也能先检查乐谱

需要 Git、CMake 3.20+、C++17 编译器；macOS 还需要 Xcode Command Line Tools。下面从仓库根目录执行，不会运行 GitHub Actions，也不会打开音频设备：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
build/daw_performance_import --check examples/classical-study/study.musicxml
```

[原创练习](../examples/classical-study/study.musicxml)有八小节、双谱表、跨小节延音和换踏板。它是程序试用素材，不是从出版乐谱转录的作品。此例会报告三处相邻同键音的一 tick 分隔；检查通过不等于无损导入。

也可以检查自己的 `.musicxml`、`.xml`、`.mid`、`.midi`。多乐器谱先用 `--part N` 选择一个声部；`.mxl` 请先从记谱软件导出为未压缩 MusicXML。程序不修改源谱。

## 在 macOS 听真实钢琴

目前实时演奏编辑器接的是本机 **Pianoteq 9 AU**，需要自行安装、取得使用许可；仓库不提供商业音源或其预设文件。网页原型不能加载这套本地 AU。

先用离线渲染生成本机钢琴状态（不会打开扬声器），再导入演奏工程。所有目标目录必须是新的：

```sh
build/daw_piano_render examples/classical-study/study.musicxml study-piano
build/daw_performance_import examples/classical-study/study.musicxml study-piano/piano.aupreset study-work
build/daw_performance_play study-work
```

编辑器中输入：

```text
provenance
notes 0 12
play
stop
range 3 7
play
repeat on
stop
repeat off
range-clear
curve-adopt 1 0 64
curves
gain -18
undo
save "study-edited"
quit
```

`play` 才打开音频设备。`range` 和 `seek` 设置位置后保持停止；`play` 会静默重放前面的 MIDI 和音频处理，以恢复踏板、持音和音源尾音，再从指定位置出声。准备耗时随位置增长，会打印耗时。段落重复会重建音源，有间隙，**不是无缝循环**；重复和定位设置不写入工程。开头/结尾加 128 帧淡化，仅用于段落试听。

`notes` 显示真正的音符 ID、音高、小节、声部和时间。用列出的 ID 执行 `edit ID 起点偏移毫秒 时值比例 力度`，不要照抄其他曲子的音符 ID。已在本次播放中发生的起音编辑会被拒绝；停止后可以编辑。谱面与演奏编辑共享撤销栈。

`curve-adopt` 把已导入的踏板转换为可编辑的阶梯曲线，保留同一时刻先松后踩的顺序。`curve-step` 可新建阶梯曲线；`curve-put` 保留线性插值行为。详见[本轮行为与格式](alpha-readiness.md)。

导出采用保存的工程输出增益，与试听的目标增益一致，不会自动把电平拉满：

```sh
build/daw_performance_render study-edited study-bounce
```


单独导出导入记录，不加载音源：

```sh
build/daw_performance_provenance study-work > import-report.json
```

记录指向导入时的记谱音符、小节和修改前后值；之后编辑不会改写这份历史。旧工程没有记录时会明确提示，不能自动补造。含记录的新工程使用 v3 格式，旧版程序不能打开；新版仍能读 v1/v2。

## 意外退出后恢复

每条成功的编辑命令后，编辑器在源工程旁边保存恢复点，保留最近两份完整修订。看到 `Autosaved revision=...` 才表示本次恢复点写入成功；`AUTOSAVE FAILED` 表示编辑仍在内存，需要手动 `save`。

```sh
build/daw_performance_recover list study-work
build/daw_performance_recover restore study-work "上条命令列出的完整恢复目录" study-recovered
```

恢复到新目录，原工程保持不动。恢复数据包含工程、演奏和本机音源状态，不包含撤销历史。校验不符、源工程后来被修改、损坏或丢失时会拒绝恢复，不能当作独立备份方案。这里验证的是进程退出恢复，不是断电持久性。

## 钢琴＋大提琴多声部编辑

已保存的原生多乐器工程可以迁移到演奏编辑器，要求每条路由有本机 AU 状态文件。迁移本身不加载插件、不打开设备：

```sh
build/daw_performance_session_import /path/to/session.dawsession duet-work
# 源工程引用冻结音频时，明确从谱子重新编译演奏；原缓存保持不动：
build/daw_performance_session_import /path/to/session.dawsession duet-work --unfreeze
build/daw_performance_play duet-work
```

上面两条迁移命令按源工程选择其一，目标必须是新目录。在编辑器中用 `routes` 查声部 ID，`notes-part "声部ID" 0 20` 查音符；`curve-adopt-part "声部ID" 曲线ID 通道 11` 接入已有表情消息，`route "声部ID" 增益dB 声像 静音0或1 独奏0或1` 编辑混音。这些操作与音符编辑共享撤销栈。用列出的音符 ID，不要照抄其他工程。

源工程的主增益若大于 0 dB，迁移会将这部分增益加到每条轨道，并把文档主增益设为 0 dB，打印调整说明；每条轨道的实际输出增益不变。如果任一轨道合计超过 +12 dB（静音轨也检查），会在保存前明确拒绝，请保留原工程播放或自行调整混音。程序不会偷偷降低音量。延后的音符若延长了合奏结尾，其他声部的踏板/表情曲线也能延伸到这个共同结尾。

目前最多两件实时 AU（Pianoteq 9 / SWAM Cello 3），统一换页和插件延迟补偿。SWAM 会检查保存状态的移调和音域；旧的 −12 半音状态不会自动改写。多声部播放从零开始，定位/段落试听会明确报尚未接入。非零音乐性轨道提前量也会被拒绝。自动恢复包含所有路由状态；合奏导出不打开设备，使用同一图并移除文件开头的共同算法延迟。详见[多声部编辑与格式 v4](multipart-performance-editor.md)。

## 目前的边界

- MusicXML 导入会进行有报告的修复；反复、装饰音、力度解释、复杂记谱的完整保真仍未完成。新工程会保存读取阶段的分类计数，以及进入试听前的逐条缩音、静音和断延音记录。倚音等读取阶段的逐条原始位置仍未覆盖。保留原谱，详见[修复记录](import-repair-provenance.md)。
- 多声部演奏文档和两件 AU 实时编辑已接通；十二声部冻结/实时混合编辑、焦点切换和周转时间测量尚未完成。多轨离线渲染、冻结轨播放仍是另一条路径，见[多乐器工程](sessions.md)。
- 没有完整的原生图形编辑器、通用插件扫描器、低延迟 MIDI 键盘监听，也没有签名安装包。
- 仓库内的历史测试数字只覆盖记录的版本与工况。打开某一首曲子不代表整个古典曲库都能正确导入；请对照谱面和听感。

反馈请到仓库 Issues，说明提交版本、系统、输入格式、最短复现步骤、预期与实际结果。可以先用这首原创示例复现；不要把私人曲谱、授权音源状态或密钥贴到公开 Issue。
