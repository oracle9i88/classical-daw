# Classical DAW Web Prototype

这是 `classical-daw` 的纯静态网页版实验入口，不依赖后端、构建工具、账号或外部 CDN。它用于快速验证古典创作的基本交互：

- 钢琴卷帘中点击空白区域新增音符；点击音符后可编辑音高、起始拍、时值和力度。
- 使用浏览器原生 Web Audio API 播放三角波试听，速度可调（30–240 BPM）。
- 导入网页导出的 MusicXML（也支持相同单声部子集的其他文件），解析音符、休止、和弦、力度和速度后更新卷帘。
- 导出当前内容为 `classical-daw-sketch.musicxml`，使用引擎约定的 960 divisions-per-quarter（960 PPQ）时间网格。

## 本地运行

在仓库根目录执行：

```bash
python3 -m http.server 8080 --directory web
```

然后打开 <http://localhost:8080/>。由于浏览器对本地文件的模块和下载行为有限制，使用静态服务器比直接双击 `index.html` 更可靠。

## GitHub Pages

仓库自带 `.github/workflows/pages.yml`：推送到 `main` 后，GitHub Actions 会把 `web/` 作为静态产物发布到 GitHub Pages。第一次使用时，在仓库 Settings → Pages → Build and deployment 中选择 **GitHub Actions**；工作流不需要配置密钥。

## 当前限制

这是可运行的交互原型，不是完整 DAW：只有单页单声部时间线；网格固定为 4 小节、4/4 拍和 C4–B5；尚未包含连音/跨小节音符、力度曲线、谱面排版、真实采样器、CoreAudio 设备和项目保存。MusicXML 导入/导出限定为这一可视范围内的单声部子集：960 divisions、音符/休止/同起始拍和弦、速度和力度。超出 C4–B5、前 4 小节或过细网格的事件会被忽略或量化到网格。

代码与仓库其余部分采用 [AGPL-3.0-or-later](../LICENSE)。
