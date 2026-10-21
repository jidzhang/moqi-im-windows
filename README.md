# 墨奇输入法 Windows版

墨染清风书雅韵，奇思逸趣入诗文。

Windows 端的输入法前端，负责把 `moqi-ime` 后端接入 **Microsoft Text Services Framework (TSF)**，并提供候选窗、语言栏按钮、组合串上屏等 Windows 平台能力。

当前状态：已实现 **[Rime / 中州韵](https://github.com/rime/librime)** 输入法接入，**fcitx5** 输入法接入中。

项目 TSF 层核心依赖：[`libIME2`](https://github.com/EasyIME/libIME2)。很大程度上参考了[PIME](https://github.com/EasyIME/PIME) 以及 [小狼毫](https://github.com/rime/weasel) 项目。

核心引擎：[`moqi-ime`](https://github.com/gaboolic/moqi-ime)。使用Go语言实现。

默认输入法：[`白霜拼音`](https://github.com/gaboolic/rime-frost)。

> **关于本仓库**：本仓库是墨奇输入法 Windows 版的**深度优化分支**，由 [@jidzhang](https://github.com/jidzhang) 维护。
> 感谢原作者 [@gaboolic](https://github.com/gaboolic)——墨奇输入法、moqi-ime 引擎与白霜拼音均出自其手，
> 本分支的所有优化都建立在上游的成果之上。上游原仓库：<https://github.com/gaboolic/moqi-im-windows>。
>
> 本分支以**测量驱动**的方式做优化：每一项改动都有对照实验数据（40 格全矩阵测评 + 逐对归因），
> 守卫维度零回归才合入。下面所有数字都来自这套测评（口径见文末"测评方法"）。

## 一句话看成绩

**混合连打（全拼 + 首字母，一次空格上屏）** 是真实打字最核心的场景。
本分支把它从 65.5 提升到 **82.4**（公平口径 top-1，+16.95）——
`shid` 空格直接出「是的」，`wom` 出「我们」，不再是「时代」「我吗」。

## 快速开始

从本仓库 [Releases](https://github.com/jidzhang/moqi-im-windows/releases) 下载安装包，三档任选：

| 文件 | 内容 | 适合谁 |
|---|---|---|
| `moqi-im-windows-setup-lite.exe`（约 35MB） | 不含预编译词库 | 想要最小安装包 |
| `moqi-im-windows-setup.exe`（约 73MB） | 含预编译词库 | 绝大多数用户 |
| `moqi-im-windows-setup-full.exe`（约 228MB） | 预编译词库 + 大语法模型 | 想要更好的长句效果 |

另有独立的大语法模型 `zh-hans-t-essay-bgw.gram` 可供已装 lite/标准包的用户后补（放进 `%APPDATA%\Moqi\Rime` 即自动启用，无需管理员权限）。详见 [installer/PACKAGES.md](installer/PACKAGES.md)。

## 特色功能

- 托盘切换输入法状态：中英文 全半角 简繁
- 外观编辑：主题皮肤切换、字体调整、主题色切换、自定义快捷键。一般功能都可通过托盘图标右键修改，强烈自定义修改需求可修改C:\Users\用户名\AppData\Roaming\Moqi\appearance_config.json  现在已支持导入皮肤，可选yaml文件
- 默认使用白霜拼音，支持切换方案集、切换输入方案、更新配置
- 修改了librime的组句算法，支持输出多条整句候选，正确率大大提升
- 置顶短语、超级简拼
- 输入设置：自动插入成对符号、分号键次选
- 同步设置：设置WebDAV后，支持跨设备同步用户词库（WebDAV）
- **云剪贴板（WebDAV）**：与 Android 版互通，默认关闭。在输入法「设置」菜单 →「云剪贴板」中配置 WebDAV 地址与账号；开启后 PC 任意应用复制纯文本会自动上传到 `{墨奇设置目录}/clip/`（默认 `moqi-input-method/clip/`）。按 `Ctrl+0`（可在设置中修改）将云端条目显示为候选列表并上屏。配置保存在 `%APPDATA%\Moqi\cloud_clipboard.json`，密码经 DPAPI 加密存于 `cloud_clipboard.pw`。
- 原生ai功能：支持整句优化、翻译、问答等，只有想不到没有做不到。编辑C:\Users\用户名\AppData\Roaming\Moqi\Rime\ai_config.json 可以接入ai大模型以及自定义提示词、快捷键。为了方便体验，已在安装包里加了默认配置，可以替换成用户自己的apikey

```
{
  "api": {
    "base_url": "",
    "api_key": "",
    "model": ""
  },
  "actions": [
    {
      "name": "写好评",
      "hotkey": "Ctrl+Shift+G",
      "prompt": "请围绕“{{candidate_1}}”生成最多 3 条适合直接发布的中文好评，每条 20 字左右。"
    },
    {
      "name": "优化整句",
      "hotkey": "Ctrl+Shift+O",
      "prompt": "上一句是：{{previous_commit}}\n原始输入拼音是：{{composition}}\n前三个候选词：\n{{candidates_top3}}\n请严格遵循原始输入拼音来选字，只能在符合这串拼音的候选范围内优化整句，不要凭空改成不匹配拼音的字词。只输出 1 条最通顺、最自然的整句候选，不要解释。"
    },
    {
      "name": "中译英",
      "hotkey": "Ctrl+Shift+E",
      "prompt": "请把当前中文内容翻译成自然、地道的英文。 “{{candidate_1}}” 只输出 1 条英文翻译，不要解释。"
    },
    {
      "name": "问答",
      "hotkey": "Ctrl+Shift+Q",
      "prompt": "用户询问： “{{candidate_1}}” 只输出 1 条答案，不要解释。"
    }
  ]
}
```

## 本分支的深度优化

以下全部改动均有对照实验数据支撑，明细见 [测评报告](https://github.com/jidzhang/moqi-ime/blob/main/report/2026-10-16-baseline.md)。

### ① 混合连打重排（旗舰特性）

真实打字的高频形态：前词全拼、后词首字母连打（`wod`→我的、`shid`→是的）。上游引擎在这种输入下会大量被书面语高频词劫持（是的→时代、我要→我也）。本分支的修法是一套组合拳：

- **口语词对先验重排**：用四源对话语料（字幕/新闻/TED/Leipzig）挖掘的 46000+ 词对频次表，对候选页做保守重排——只在"首选词在语料中无据可查"时，才把语料频次 ≥300 的强词对提到首位；
- **提交对齐**：重排只改显示顺序是不够的（引擎空格仍提交引擎自己的首选，显示与上屏会分裂）——换位发生时空格自动改发数字键选择显示首选，所见即所上屏；
- **音节切分门槛**：拼音音节只以 a/e/i/o/u/n/g/r 结尾，据此精确识别"混合形态"才出手——纯全拼连打（两词/三词）的引擎排序已经很好，一个字都不动；
- **学习治理**：逐对归因发现"简拼提交学到的词会劫持后续候选页"（净损 37 对）——混合输入禁用用户词典查询与学习，全拼输入的个性化学习完整保留。

**结果**（默认配置、全新用户目录、每臂带激活证据）：

| 维度 | 优化前 | 优化后 | 变化 |
|---|---|---|---|
| 混合连打（公平口径 top-1） | 65.46 | **82.41** | **+16.95** |
| 混合连打（原始口径 top-1） | 54.90 | **67.80** | +12.90 |
| 两词全拼 / 三词全拼 / 延续 / 整句 / 简拼 | — | 全部持平或更优 | 守卫零回归 |

### ② 准确率地基

- **essay-bgw 大语法模型接入与全套调优**：整句 +1.5、混合组句 +2.4；196.8MB 模型随 full 包内置或放用户目录自动启用
- **口语 bigram 补充词典**：修复 gold 词对 24.2% 在词典完全缺失的洞（是的/的是/的人……，四源语料挖掘，权重公式实测定档）
- **换位容错层**：换位类错字 top-1 23.4% → 51.6%（音节图换位边 + 门控，守卫维度零回归）
- **英文降位**：中文可解析的输入不再被英文词劫持（der→的人），纯英文输入不受影响

### ③ 性能

- **按键延迟 11.4ms → 0.73ms/键**（-94%）：DLL 从每键 4 次轮询 RPC 改为 overlapped 事件驱动读；后端单键 0.40ms
- **首次输入卡顿消除**：修复 librime schema 循环依赖 + 启动即预热后端
- **组字候选直出**：全链路实测逐键无卡顿（RIME slow 日志阈值 30ms 内）

### ④ 输入体验

- **数字/小数点直通**：中文模式下 `4.`、`4.5` 直接上屏（对齐微软拼音）；组字中数字仍用于选候选
- **修复记事本组字虚线被候选窗遮挡**：CUAS 组字浮窗精确锚定（候选窗顶贴合组字窗底，全 Windows 版本的记事本均适用）
- **候选窗**：微软式锚定与同屏、直角、首候选高亮四边贴边；默认 7 候选、每行 9
- **大语法模型开箱自动启用**：装完即用；安装竞态下绝不误删用户配置（带回归测试）
- **切应用瞬间方案就位**：每个应用记住的输入方案在激活瞬间生效（原实现要等首次按键）

### ⑤ 工程

- **GitHub Actions 三档全自动发版**：lite/标准/full + 独立语法模型，tag 推送即出包；CI 缓存提速
- **40 格全矩阵测评纪律**：6 维度 × 3 配置 × 2 链路，每格全新用户目录；快路径/配置层/激活证据等一系列测评陷阱已沉淀为防复发规范
- **测试健康度**：上游遗留失败测试修复 5 项，新增回归测试 8 项

## 功能演示

ai功能演示：<https://github.com/gaboolic/moqi-im-windows/issues/4>
![输入0-ai](others/imgs/input-ai.gif)
![输入1-ai](others/imgs/input-zise.png)

外观配置、切换方案
![菜单主题](others/imgs/caidan-zhuti.png)
![菜单方案集](others/imgs/caidan-schemas.png)
![菜单方案](others/imgs/caidan-schema.png)

同步设置
![同步设置](others/imgs/caidan-tongbu.png)
![webdav](others/imgs/caidan-webdav.png)

多整句、句中任意辅助码演示
![输入整句组句](others/imgs/input-zuju.png)
![输入4-句中辅助码](others/imgs/input-liuxingyusamantianji.gif)

一些输入效果
![输入2](others/imgs/input-mo.png)
![输入3](others/imgs/input-qi.png)

## 运行架构

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

当前默认部署中，`backends.json` 指向 `moqi-ime\server.exe`；Windows 侧通过本地命名管道连到 `MoqLauncher`，`MoqLauncher` 再把请求按行写入后端的标准输入，并从标准输出读取响应。

## 核心职责

- **TSF 文本服务**：实现 `ITfTextInputProcessor`、`ITfKeyEventSink` 等接口
- **Windows UI**：管理候选窗口、消息窗口、语言栏按钮和 preserved keys
- **协议转换**：把按键与状态序列化为 JSON 请求，并应用后端返回的组合串、候选和提交文本
- **后端接入**：通过 `MoqLauncher` 读取 `backends.json`，按语言配置 GUID 路由到对应后端

## 与 `moqi-ime` 的关系

本仓库不实现拼音解析、候选生成、词库管理等输入法核心逻辑，这些能力由 `moqi-ime` 提供。

Windows 侧的职责主要是：

- 把 TSF 生命周期和按键事件转发给后端
- 根据后端返回结果更新候选窗、组合串和状态
- 从已安装后端目录扫描 `input_methods/*/ime.json`，为语言配置提供元数据与配置入口

因此，`moqi-im-windows` 与 `moqi-ime` 需要配套部署。

## 技术栈

- **语言**：C++ 、GO
- **框架**：Microsoft TSF、Win32 API
- **进程/IO**：命名管道、子进程 `stdin/stdout` 转发、`libuv`
- **数据格式**：protobuf(见/proto/moqi.proto)
- **日志**：`spdlog`
- **构建**：CMake + Visual Studio 2022 / MSBuild

## 源码布局

- `MoqiTextService`：TSF 文本服务，产出 `MoqiTextService.dll`
- `MoqLauncher`：后端启动器与消息转发器，产出 `MoqiLauncher.exe`
- `libIME2`：IME/TSF 基础库，也是本项目 TSF 层的核心依赖，来源：[`gaboolic/libIME2`](https://github.com/gaboolic/libIME2)
- `libuv`：Launcher 的事件循环与进程/管道依赖
- `backends.json`：后端清单，定义后端名称、启动命令和工作目录
- `scripts`：构建脚本
- `installer/`：Inno Setup 安装器脚本与输出目录

## 构建

前置：**Visual Studio 2022**、**CMake 3.21+**、Windows SDK。

在本仓库根目录执行：

   `powershell -ExecutionPolicy Bypass -File .\scripts\_all_in_package.ps1`

该脚本会生成安装器：`installer\dist\moqi-im-windows-setup.exe`

## 参考文档

- [Microsoft TSF 文档](https://docs.microsoft.com/en-us/windows/win32/tsf/text-services-framework)
- [ITfTextInputProcessor 接口](https://docs.microsoft.com/en-us/windows/win32/api/msctf/nn-msctf-itftextinputprocessor)
- [PIME](https://github.com/EasyIME/PIME)
- [小狼毫](https://github.com/rime/weasel)

## 测评方法

所有数字来自固定口径的标准测评套件（6 维度 × 3 配置 × 2 链路 = 40 格，每格全新用户目录）：

- **维度**：d1 两词全拼 / d2 三词全拼 / d3 混合连打（前词全拼+后词首字母，一次空格）/ d4 单字母延续 / d5 整句全拼 / d6 整句简拼，词对来自四源真实语料（字幕/新闻/TED/Leipzig）挖掘的 2000 高频对
- **混合连打两个口径**：原始口径（2000 对全算）与公平口径（同一输入串只保留语料频次最高的 gold——多个 gold 共享同一输入时 top-1 数学上只能对一个，公平口径剔除这种不可达损失）
- **守卫纪律**：任何改动必须在 d1/d2/d4/d5/d6 全部零回归（≤0.3 点噪声带）才合入；本文数字均为带激活证据的复测值（重排换位计数、配置自动写入等）

## 致谢

本分支站在下面这些项目和作者的肩膀上：

- **[gaboolic](https://github.com/gaboolic)**：墨奇输入法原作者。moqi-ime 引擎、白霜拼音、墨奇音形、墨奇五笔整句等核心成果均出自其手，本分支的一切优化都基于这些工作
- [Rime / 中州韵 librime](https://github.com/rime/librime)：输入法引擎
- [小狼毫 weasel](https://github.com/rime/weasel)、[PIME](https://github.com/EasyIME/PIME)、[libIME2](https://github.com/EasyIME/libIME2)：Windows TSF 实现的参考与基础库
- [lotem/rime-octagram-data](https://github.com/lotem/rime-octagram-data)：官方语法模型（essay-bgw）

## 赞助支持

如果墨奇输入法 Windows 版帮到了你，欢迎支持本分支的持续维护：

- 爱发电：<https://afdian.com/a/dc99tech>

- 捐赠完全自愿，不承诺任何回报；款项用于支持 **Windows 移植版的维护与优化**
- 本项目基于原作者 [gaboolic](https://github.com/gaboolic) 的开源成果；如想支持上游开发，请访问[上游仓库](https://github.com/gaboolic/moqi-im-windows)
- 以上为本仓库唯一官方捐赠渠道，谨防冒用

## 友情链接

白霜拼音 <https://github.com/gaboolic/rime-frost>

墨奇音形 <https://github.com/gaboolic/rime-shuangpin-fuzhuma>

墨奇五笔整句 <https://github.com/gaboolic/rime-wubi-sentence>

### Star History

[![Star History Chart](https://api.star-history.com/svg?repos=jidzhang/moqi-im-windows&type=Date)](https://star-history.com/#jidzhang/moqi-im-windows&Date)
