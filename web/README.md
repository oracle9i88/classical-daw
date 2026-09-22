# Classical DAW Web Prototype

这是 `classical-daw` 的纯静态网页版实验入口，不依赖后端、构建工具、账号或外部 CDN。它用于快速验证古典创作的基本交互：

- 钢琴卷帘中点击空白区域新增音符；点击音符后可编辑音高、起始拍、时值和力度。
- 使用浏览器原生 Web Audio API 播放三角波试听，速度可调（30–240 BPM）。
- 导出当前内容为 `classical-daw-sketch.musicxml`，使用引擎约定的 960 divisions-per-quarter（960 PPQ）时间网格。

## 本地运行

在仓库根目录执行：

```bash
python3 -m http.server 8080 --directory web
```

然后打开 <http://localhost:8080/>。由于浏览器对本地文件的模块和下载行为有限制，使用静态服务器比直接双击 `index.html` 更可靠。

## GitHub Pages

将 Pages 的发布目录设为仓库中的 `web/`，或在 Pages 工作流中把 `web/` 复制到发布目录即可。页面没有构建步骤，也不需要配置密钥。

## 当前限制

这是可运行的交互原型，不是完整 DAW：只有单页单声部时间线；网格固定为 4 小节、4/4 拍和 C4–B5；尚未包含 MusicXML 导入、连音/跨小节音符、力度曲线、谱面排版、真实采样器、CoreAudio 设备和项目保存。MusicXML 导出使用四分音符为 960 divisions，支持音符、同起始拍的和弦、空拍和速度；输出不包含外部 DTD/DOCTYPE，便于受限解析器读取。复杂重叠声部会被压缩到这一导出子集。

代码与仓库其余部分采用 [AGPL-3.0-or-later](../LICENSE)。
