# Classical DAW Web Prototype

这是 `classical-daw` 的纯静态网页版实验入口，不依赖后端、构建工具、账号或外部 CDN。它用于快速验证古典创作的基本交互：

- 钢琴卷帘中点击空白区域新增音符；点击音符后可编辑音高、起始拍、时值和力度。
- 使用浏览器原生 Web Audio API 播放三角波试听，速度可调（30–240 BPM）。
- 选中音符后支持歌词、撤销/重做，以及 2/4、3/4、4/4、6/8 拍号编辑。
- 导入网页导出的 MusicXML（也支持相同单声部子集的其他文件），解析音符、休止、和弦、单个歌词文本、力度和速度后更新卷帘。
- 导入标准 MIDI Type 0/1；解析音符、首个 tempo 与首个拍号，并把结果量化到当前 1/4 拍网格。
- 选中音符后可输入该音符的歌词；导出时写入 MusicXML 的 `<lyric><text>…</text></lyric>`。
- 导出标准 MIDI Type 1（960 PPQ），包含 conductor tempo/拍号轨和音符轨，可被常见 DAW 打开。
- 可保存/打开版本化 JSON 工程文件；旧版工程缺少拍号时按 4/4 兼容，坏文件不会覆盖当前工程。
- 编辑有节流的浏览器本地恢复副本；重新打开页面时可显式恢复或丢弃，存储失败不会阻断编辑。
- 导出当前内容为 `classical-daw-sketch.musicxml`，使用引擎约定的 960 divisions-per-quarter（960 PPQ）时间网格。

## 本地运行

在仓库根目录执行：

```bash
python3 -m http.server 8080 --directory web
```

然后打开 <http://localhost:8080/>。由于浏览器对本地文件的模块和下载行为有限制，使用静态服务器比直接双击 `index.html` 更可靠。

## GitHub Pages

仓库自带 `.github/workflows/pages.yml`：推送到 `main` 后，GitHub Actions 会把 `web/` 作为静态产物发布到 GitHub Pages。工作流会在首次运行时请求启用 Pages；如果仓库策略禁止 workflow 自动启用，请在 Settings → Pages → Build and deployment 中选择 **GitHub Actions** 后再推送一次。工作流不需要配置密钥。

## 当前限制

这是可运行的交互原型，不是完整 DAW：只有单页单声部时间线；网格显示 4 小节、C4–B5，拍号支持常用的 2/4、3/4、4/4、6/8；尚未包含连音/跨小节音符、力度曲线、谱面排版、真实采样器或 CoreAudio 设备。MusicXML 导入/导出限定为这一可视范围内的单声部子集：960 divisions、音符/休止/同起始拍和弦、单个歌词文本、速度、力度和首个拍号。MIDI 导入支持标准 Type 0/1 的 PPQ 文件，严格校验轨道、VLQ、EOT、音符配对、tempo 与拍号；有效但超出 C4–B5、前 4 小节或 1/4 拍网格的事件会被忽略或量化。MIDI 导出为 Type 1、960 PPQ，生成 conductor tempo/拍号轨和音符轨。网页 JSON 工程文件只保存当前原型的音符编辑状态，不是完整项目包；浏览器本地副本也只用于恢复当前原型状态。

代码与仓库其余部分采用 [AGPL-3.0-or-later](../LICENSE)。
