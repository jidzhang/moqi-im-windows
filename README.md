# 墨奇输入法 Windows版

墨染清风书雅韵，奇思逸趣入诗文。

Windows 端的输入法前端，负责把 `moqi-ime` 后端接入 **Microsoft Text Services Framework (TSF)**，并提供候选窗、语言栏按钮、组合串上屏等 Windows 平台能力。

当前状态：已实现 **[Rime / 中州韵](https://github.com/rime/librime)** 输入法接入，**fcitx5** 输入法接入中。

项目 TSF 层核心依赖：[`libIME2`](https://github.com/EasyIME/libIME2)。很大程度上参考了[PIME](https://github.com/EasyIME/PIME) 以及 [小狼毫](https://github.com/rime/weasel) 项目。

核心引擎：[`moqi-ime`](https://github.com/gaboolic/moqi-ime)。使用Go语言实现。

默认输入法：[`白霜拼音`](https://github.com/gaboolic/rime-frost)——经过[公开评测](https://github.com/gaboolic/rime-schema-compare/blob/main/report/latest.md)，词频准确度在拼音方案中最高的方案，与墨奇输入法同源、深度适配。

> **关于本仓库**：本仓库是墨奇输入法 Windows 版的**深度优化分支**，由 [@jidzhang](https://github.com/jidzhang) 维护。
> 感谢原作者 [@gaboolic](https://github.com/gaboolic)——墨奇输入法、moqi-ime 引擎与白霜拼音均出自其手，
> 本分支的所有优化都建立在上游的成果之上。上游原仓库：<https://github.com/gaboolic/moqi-im-windows>。

## 为什么用这个版本

**混合连打：想打的词一次空格直出。** 打「我要」不用打全 `woyao`，打 `woy` 加空格就是「我要」；打 `shid` 出「是的」而不是「时代」。这是真实打字里最常用也最省力的打法，本分支把它做对了——公平口径命中率从 65.5 提升到 **82.4**（百分制）。

**打字跟手。** 每个按键的处理时间从 11.4 毫秒降到 0.73 毫秒，砍掉 94%；装完第一个字就不会卡。

**功能齐全，持续维护。** 继承上游全部功能（整句输入、AI 增强、云同步、换肤……），每项优化都有自动化测评背书，改一处验全套，其它场景零退步才发布。

## 下载安装

从本仓库 [Releases](https://github.com/jidzhang/moqi-im-windows/releases) 下载安装包，三档任选：

| 文件 | 内容 | 适合谁 |
|---|---|---|
| `moqi-im-windows-setup-lite.exe`（约 35MB） | 不含预编译词库 | 想要最小安装包 |
| `moqi-im-windows-setup.exe`（约 73MB） | 含预编译词库 | 绝大多数用户 |
| `moqi-im-windows-setup-full.exe`（约 228MB） | 预编译词库 + 大语法模型 | 想要更好的长句效果 |

已装 lite/标准包想后补大语法模型？下载 `zh-hans-t-gram.gram` 放进 `%APPDATA%\Moqi\Rime` 即自动启用——不需要管理员权限，升级重装也不会丢。详见 [三档安装包说明](installer/PACKAGES.md)。

## 功能一览

**输入与准确率**

- 混合连打：前词全拼 + 后词首字母（`woy`→我要），一次空格直出心里那个词（本分支核心优化，见下文）
- 整句输入：连打一整句拼音，直接给出多条完整句子候选，不用逐词确认
- 错字宽容：「哪怕」打成「拿怕」、拼音字母打反（换位），都能出正确的词并标注读音
- 置顶短语、超级简拼：常用词一两个字母直达
- 数字直通：中文模式下打 `4.`、`4.5` 直接上屏，不用切英文（组字中数字仍是选词键）

![整句组句](others/imgs/input-zuju.png)

**智能增强**

- 原生 AI：接入大模型后，选中候选按快捷键即可「写好评」「优化整句」「中译英」「问答」「润色」，提示词和快捷键都能自定义（[功能演示](https://github.com/gaboolic/moqi-im-windows/issues/4)）
- **AI 默认关闭、绝不联网**；想启用（约 1 分钟）：
  1. 打开配置文件 `%APPDATA%\Moqi\ai_config.json`（若不存在，则编辑安装目录下的 `moqi-ime\input_methods\rime\ai_config.json`，首次启动会自动复制一份到上面位置）
  2. 把键名 `"base_url删掉我即可用AI功能"` 改成 `"base_url"`（冒号后面的网址不用动）
  3. 在 `"api_key"` 里填入你自己的 key；也支持任意 OpenAI 兼容接口——改 `base_url` 和 `model` 即可指向私有部署
  4. 托盘菜单 →「重启墨奇引擎」生效
- 大语法模型：essay 级语言模型参与组句排序，长句和上下文延续更准；装 full 包或按上文放置模型文件即自动启用

![AI 功能](others/imgs/input-ai.gif)

**个性化与同步**

- 用户词库学习：全拼输入的用词习惯会被记住，越用越顺手
- WebDAV 同步：设置后跨设备同步用户词库；云剪贴板与 Android 版互通——PC 上复制的内容，手机上按 `Ctrl+0` 直接粘贴上屏（默认关闭，托盘「设置」→「云剪贴板」开启，密码本地加密存储）
- 外观：主题皮肤、字体、主题色、快捷键都可在托盘菜单调整，支持导入皮肤文件

![外观与方案](others/imgs/caidan-zhuti.png)

**体验细节**

- 候选窗：微软式跟随光标、直角窗口、首候选高亮四边贴满；托盘切换中英文/全半角/简繁
- 支持切换方案集与输入方案（白霜拼音/音形/五笔整句等），每个应用记住自己的方案，切换应用瞬间就位
- 输入设置：自动插入成对符号、分号键次选

## 我们做了哪些深度优化

以下每项都经过对照实验验证（方法见下节），守卫场景零退步才合入。

### 混合连打：一次空格，直出心里那个词

混合连打的问题在于一个简写串背后有十几个候选抢位置，出来的经常不是心里那个词：`shid` 想要「是的」出来「时代」，`woy` 想要「我要」出来「我也」。原因是双重的——这些口语搭配大量不在词库里；在词库里的也排不过书面语高频词（「这样」的词频是「这一」的三倍）。我们分四步修通：

1. **补词**：从字幕、新闻、TED、小说四类语料挖掘口语高频搭配，缺失的 1938 条补进词库，权重按真实语料频次定档；
2. **排序**：引入「口语搭配先验」——候选词若是语料中真实出现过的搭配，就提到前面；但只替换语料中毫无踪迹的首选，绝不越权打乱正常排序；
3. **提交对齐**：排序改了还不够——按空格时引擎默认提交自己的第一名，屏幕显示和实际上屏会不一致。现在空格自动提交屏幕上显示的那个词，所见即所得；
4. **治理学习**：逐条归因发现简拼提交学到的词会反过来污染之后的候选（净损 37 对）。混合输入不再写入用户词典，全拼输入的个性化学习原样保留。

**效果**：公平口径命中率 65.5 → **82.4**；原始口径 54.9 → 67.8。六个场景全部持平或更优。

### 其余优化一览

| 方面 | 成果 |
|---|---|
| 按键延迟 | 11.4ms → 0.73ms/键（-94%）；首次输入卡顿消除 |
| 整句质量 | 大语法模型接入与全套调优，整句 +1.4 |
| 错字宽容 | 换位类错字命中率 23% → 52% |
| 英文干扰 | 中文可解析的输入不再被英文词抢首选（der→的人） |
| 显示修复 | 记事本组字下划线被候选窗遮挡（所有 Windows 版本） |
| 安装体验 | 三档安装包；语法模型放用户目录自动启用；GitHub Actions 全自动发版 |

## 这些数字怎么来的

上面所有数字来自一套固定的自动化测评：六个真实打字场景（两词、三词、混合连打、单字母延续、整句、简拼）× 三种配置 × 两条链路共 40 组，每组全新用户目录；词料是真实语料挖掘的 2000 个高频搭配和 3466 条整句。规矩只有一条：目标场景必须提升，其它场景一分不能掉。完整实验记录在 [moqi-ime 评测报告](https://github.com/jidzhang/moqi-ime/blob/main/report/2026-10-16-baseline.md)。

## 开发者专区

**运行架构**

```
┌─────────────────────────────────────────────┐
│           Windows 应用程序                 │
│      (记事本 / Word / 浏览器 / 其他)        │
├─────────────────────────────────────────────┤
│  MoqiTextService.dll                        │
│  - TSF 接口实现                             │
│  - 按键事件/组合串/候选窗处理               │
│  - 通过命名管道连接 Launcher                │
├─────────────────────────────────────────────┤
│  MoqiLauncher.exe                            │
│  - 读取 backends.json                       │
│  - 管理后端进程生命周期                     │
│  - 在命名管道与后端 stdin/stdout 之间转发消息│
├─────────────────────────────────────────────┤
│  moqi-ime\server.exe                        │
│  - Go 后端进程                              │
│  - 加载 input_methods\*\ime.json            │
│  - 返回候选、状态、上屏文本等               │
└─────────────────────────────────────────────┘
```

本仓库不实现拼音解析、候选生成等核心逻辑（由 `moqi-ime` 提供），负责 TSF 生命周期、按键转发、候选窗 UI 与后端接入，两仓配套部署。

- **技术栈**：C++ / Go、Microsoft TSF、Win32 API、命名管道 + libuv、protobuf、spdlog、CMake + Visual Studio 2022
- **源码布局**：`MoqiTextService`（TSF 文本服务/DLL）、`MoqLauncher`（后端启动器）、`libIME2`（TSF 基础库）、`installer/`（Inno Setup 安装器）、`scripts/`（构建脚本）
- **构建**：装好 Visual Studio 2022、CMake 3.21+、Windows SDK 后，根目录执行 `powershell -ExecutionPolicy Bypass -File .\scripts\_all_in_package.ps1`
- **参考**：[Microsoft TSF 文档](https://docs.microsoft.com/en-us/windows/win32/tsf/text-services-framework) · [PIME](https://github.com/EasyIME/PIME) · [小狼毫](https://github.com/rime/weasel)

## 致谢

本分支站在下面这些项目和作者的肩膀上：

- **[gaboolic](https://github.com/gaboolic)**：墨奇输入法原作者。moqi-ime 引擎、白霜拼音、墨奇音形、墨奇五笔整句等核心成果均出自其手，本分支的一切优化都基于这些工作
- [Rime / 中州韵 librime](https://github.com/rime/librime)：输入法引擎
- [小狼毫 weasel](https://github.com/rime/weasel)、[PIME](https://github.com/EasyIME/PIME)、[libIME2](https://github.com/EasyIME/libIME2)：Windows TSF 实现的参考与基础库
- [lotem/rime-octagram-data](https://github.com/lotem/rime-octagram-data)：官方语法模型（essay-bgw）

## 友情链接

白霜拼音 <https://github.com/gaboolic/rime-frost>

墨奇音形 <https://github.com/gaboolic/rime-shuangpin-fuzhuma>

墨奇五笔整句 <https://github.com/gaboolic/rime-wubi-sentence>

### Star History

[![Star History Chart](https://api.star-history.com/svg?repos=jidzhang/moqi-im-windows&type=Date)](https://star-history.com/#jidzhang/moqi-im-windows&Date)

## 赞赏支持

如果墨奇输入法帮到了你，欢迎请作者喝杯咖啡——**金额随意，一分也是心意**。微信扫描（或长按识别）下方二维码即可：

<p align="center">
  <img src="others/imgs/wx-zanshang.jpg" width="280" alt="微信赞赏码">
</p>

- 也可以在手机微信里打开[这篇介绍文章](https://mp.weixin.qq.com/s/Kb9NoCpAo2MkKuJubmWYww)，在文末点「喜欢作者」
- 捐赠完全自愿，不承诺任何回报；款项用于支持 **Windows 移植版的维护与优化**
- 本项目基于原作者 [gaboolic](https://github.com/gaboolic) 的开源成果；如想支持上游开发，请访问[上游仓库](https://github.com/gaboolic/moqi-im-windows)
- 以上为本仓库官方捐赠渠道，谨防冒用
