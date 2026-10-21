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

**混合连打**（比如打 "wo yao" 时只打 `woy` 加空格）是真实打字最常用的省力打法，也是所有拼音输入法最难的场景——一个简写串背后有十几个候选词在抢位置。本分支把它从 65.5 分提升到 **82.4 分**（百分制，公平口径），守卫的其它五个场景（全拼两词/三词、延续、整句、简拼）全部持平或更好。

## 深度优化：我们都做了什么

本分支坚持"先测量、后动手"：每项优化都先建对照实验，改完必须复测且其它场景零退步才合入。以下是经得起复测的成果（口径见文末"测评方法"）。

### 让混合连打"一次空格直出心里那个词"

问题：打 `shid` 想要「是的」，出来的却是「时代」；打 `woy` 想要「我要」，出来「我也」。原因有二：这些口语词根本不在词库里；或者在词库里但排不过书面语高频词（「这样」的词频是「这一」的三倍）。

四步修法，每步都有数据：

1. **补词**：从字幕、新闻、TED 演讲、小说四类语料挖掘口语高频词对，缺的补进词库（1938 条），权重按语料频次定档——两词纯简拼场景 +8.5；
2. **提排序**：光有词不够，还要让它在同音候选里排第一。引入"口语词对先验"：候选词若在语料中作为搭配真实出现过且频次显著，就提到前面——只替换语料中毫无踪迹的首选（全拼场景引擎排得很好，一个字都不动）；
3. **管提交**：排序改了还不够——引擎按自己的第一名上屏，会和屏幕上显示的首选不一致（看着是「是的」，按空格出来「时代」）。重排发生时空格自动改为"选第一个候选"，所见即所得；
4. **治学习**：逐条归因发现，简拼提交会把学到的词写进用户词典，反过来污染之后的候选（净损 37 对）——混合输入不再写入用户词典，全拼输入的个性化学习原样保留。

效果：混合连打 54.9 → 67.8（原始口径）/ 65.5 → **82.4**（公平口径）。

### 让"跟着上文走"更聪明

输入法应该记得你刚说过什么：上一句结尾是「我们」，下一句打 `y` 首选就该是「要」而不是「一」。实现了基于对话语料的**上下文接续预测**与**词对后继重排**——只在语料证据显著（数倍差距）时才干预，绝不为了微弱信号打乱排序。

### 让大语法模型真正帮上忙

接入 206MB 的 essay 级语法模型后，整句和长句的组句质量明显提升（整句 +1.4）。但大模型是把双刃剑：没有配套调优时混合场景反而劣化 10 分以上。定稿的配套包括：组句候选生成、英文词降位（中文拼音可解析的输入不再被英文词抢走首选）、句子质量分档——这些只有在模型就位时才启用，模型不在时自动回到安全路径。

### 把按键延迟砍掉 94%

打字跟手是底线。逐项定位延迟来源后：TSF 前端从"每敲一键问四次后端"的轮询模式改成事件驱动（11.4ms → 0.73ms/键），后端单键处理压到 0.4ms；再修掉首次输入必卡的引擎配置死循环，配合启动预热——从装完到打字全程无感。

### 错字宽容：打错音也能出对字

「哪怕」打成「拿怕」、「给予」打成「给于」，现在都能直接出正确的词，还会在候选旁标注正确读音。换位类错字（相邻拼音字母打反）的正确率从 23% 提到 52%。

### 顺手修的一批体验问题

- 中文模式打 `4.` 直接出 `4.`（不再吞数字卡句号），对齐微软拼音
- 修复记事本里组字下划线被候选窗遮住的显示问题（所有 Windows 版本的记事本）
- 候选窗：微软式贴边跟随、直角窗、首候选高亮四边贴满
- 大语法模型放用户目录即自动启用，升级重装不丢，无需管理员权限
- 切换应用时，该应用记住的输入方案立刻就位

### 工程与质量

- **三档安装包 + GitHub Actions 一键自动发版**：tag 一推，lite/标准/full 三个安装包加语法模型自动构建上传
- **40 格标准测评**贯穿开发全程（6 场景 × 3 配置 × 2 链路），每次改动全矩阵验证零退步
- 修复上游遗留测试 5 项，新增回归测试 10+ 项——踩过的每个坑都变成防复发测试

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

上面所有数字不是拍脑袋，来自一套固定的自动化测评：六个真实打字场景（两词、三词、混合连打、延续、整句、简拼），每个场景跑三种配置（无语法模型 / 纯语法模型 / 完整产品配置），再加引擎直连对照——一共 40 格，每格都用全新的用户目录，杜绝历史数据互相干扰。词料来自真实语料挖掘的 2000 个高频词对和 3466 条知乎整句。任何改动都要过这套测评：目标场景必须提升，其它场景一分都不能掉。

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
