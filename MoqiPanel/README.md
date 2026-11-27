# MoqiPanel — 墨奇输入板（绿色免安装）

一个文件夹、双击即用的中文输入面板：不注册输入法、不改注册表、不在文件夹外写任何东西。
面向"装不了（也不想装）中文输入法的英文系统"——用它打中文、翻成英文并复制，再去别处粘贴。

输入引擎就是墨奇输入法的发货链（`MoqiDaemon.exe` + x86 `rime.dll` + 词库数据），
面板只是它的又一个管道客户端，按键语义、候选排序、学习、容错、gram 全是产品原样行为。

## 目录

```
MoqiPanel/
├─ package-panel.bat / .ps1   构建 + 组装 dist\MoqiPanel（双击 bat 即可）
├─ CMakeLists.txt             独立 CMake 工程（不进顶层构建/CI/安装包链）
├─ src/
│  ├─ MoqiPanelMain.cpp       入口；消息循环在这里给输入区"扣键"
│  ├─ PanelWindow.cpp/.h      窗口骨架：创建检查、布局、组字/候选带绘制
│  ├─ PanelActions.cpp        按键闸门、引擎回包、四条复制路径、翻译流程
│  ├─ EngineClient.cpp/.h     管道 RPC + 工作线程（拉起/连接/关闭自带 daemon）
│  ├─ SettingsDialog.cpp/.h   翻译设置对话框
│  ├─ TranslateCore.cpp/.h    配置读写、请求组装、应答解析（纯逻辑）
│  ├─ TranslateHttp.cpp/.h    WinHTTP POST（OpenAI 兼容 /chat/completions）
│  ├─ PanelCore.cpp/.h        标点转换、UTF-8/16、字数护栏（纯逻辑）
│  └─ PanelStyle.h            颜色与小工具
├─ tests/
│  ├─ panel_core_test.cpp         纯逻辑单元测试（ctest）
│  ├─ portable_daemon_check.py    便携 daemon 四条硬指标的集成校验
│  ├─ daemon_failed_engine_check.py  引擎未就绪时 RPC 不得崩 daemon（确定性复现）
│  ├─ daemon_start_race_repro.py  面板时序下连启 daemon，统计异常退出
│  ├─ daemon_crashtrace.py        取证：调试器/跳板/进程内钩子三种方式抓崩溃栈
│  ├─ drive_panel.py              用窗口消息驱动真窗口跑完整场景并截图
│  ├─ punctuation_live_check.py   真窗口标点两层：逐键打出来是英文标点、中文标点进输入区后各复制路径都转换
│  ├─ startup_checks.py           非 ASCII 路径 / 文件夹不完整 → 拒启
│  ├─ outside_writes_check.py     ProcMon 证明零文件夹外写入（含记事本控制组）
│  └─ tools/                      crashtrace（迷你调试器）、tramp（跳板）、crashhook.h
├─ rime/default.custom.yaml   面板专用引擎配置：标点键原样输出英文标点（纯数据，随包进 data\）
├─ vendor/fonts/              Noto Sans SC Regular + OFL 许可
└─ README.txt                 随包的用户说明（UTF-8 带 BOM）
```

## 构建与打包

```
MoqiPanel\package-panel.bat
```
等价于：
```
cmake -S moqi-daemon -B moqi-daemon/build-x86 -G "Visual Studio 17 2022" -A Win32
cmake --build moqi-daemon/build-x86 --config Release --target MoqiDaemon
cmake -S MoqiPanel -B MoqiPanel/build-x86 -G "Visual Studio 17 2022" -A Win32
cmake --build MoqiPanel/build-x86 --config Release
ctest --test-dir MoqiPanel/build-x86 -C Release
```
词库数据取自标准包的 stage 树（`installer\stage\win32\MoqiIM`，由 `package-all.bat` 产出）。
产物 `MoqiPanel\dist\MoqiPanel\`：MoqiPanel.exe、MoqiDaemon.exe、rime.dll(x86)、
`moqi-ime\input_methods\rime\{data,icons,ime.json}`（data 里有面板专用的 `default.custom.yaml`）、
`fonts\`、README.txt，约 220MB。
`user\` 与 `translate.json` 首次运行时自建。

## 对墨奇本体的改动

都在子模块 `moqi-daemon`，共三处；不带 `--portable` 时第一处不生效，后两处是无条件的缺陷修复。

**一、`--portable` 开关**（`main.cpp`，`MoqiDaemon.exe --serve --portable`）

- `MOQI_USER_DATA_DIR` / `APPDATA` / `LOCALAPPDATA` 在进程内重定向到 `<exe目录>\user`：
  用户目录、配置根、日志、AI 配置全部落在文件夹里（这些目录函数本来就按需读环境变量、不缓存）。
- 两处走 `SHGetKnownFolderPath` 的日志目录改经 `localAppDataBase()`（便携模式读重定向后的环境变量）。
- 不起工具栏 / 托盘 / 剪贴板监听线程（面板不是输入法；各 `setXxx` 对未启动的工具栏空转安全）。
- 最后一个客户端断开即 `requestStop()` 退出——面板关闭或崩溃都不留后台进程。

**二、引擎未就绪时不得调 librime**（`DaemonMenuService::gather()`）

`gather()` 由 ON_ACTIVATE 经 `buildActivatePayload` 调到，客户端一连上就来。它原先只看函数指针
非空就调 `RimeGetSchemaList`；而指针在 `loadLibrary()` 之后就全非空了，librime 的组件表却要到
`RimeSetup/RimeInitialize`（引擎初始化的最后一步）才注册。中间落进来的请求是对空组件解引用，
daemon 以 0xC0000005 退出。客户端连管道的轮询相位是随机的，磁盘慢（ProcMon、杀软扫描、
网络共享）时窗口被拉宽就必中——面板把它稳定暴露了出来，正式安装的墨奇同样暴露只是窗口极窄。
修法：`gather()` 入口取 `engineReady`，方案列表、选项读取、当前方案三处调用以它为前提；
未就绪时只收集来自配置文件的部分。

**三、启动预热的菜单缓存改为同步**（`main.cpp` 的 `refreshMenuTreeCache*`）

原先预热线程起一个后台线程抓菜单缓存、并引用预热线程栈上的临时引擎 `warm`，自己"最多等 5 秒"
就 `warm.deactivate()` 并退出。后台线程要排在分发锁后面（客户端一连上就发的 ON_MENU 正持锁做
同一次慢收集），等不到 5 秒以外就会去碰已随线程栈消失的 `warm`——访问冲突，daemon 退出。
修法：刷新主体拆成同步的 `refreshMenuTreeCacheNow()`，预热线程直接同步调用；异步外壳只接收
长寿命对象。

两处缺陷的复现与回归见下表（`daemon_failed_engine_check.py` 是确定性的；预热竞态要用调试器
把时序拖慢才稳定复现：修复前 11 轮崩 8、修复后 0 崩）。

## 设计要点

**按键闸门**（`PanelActions.cpp` 头注释）：消息循环对输入区的键盘消息**不调用** `TranslateMessage`，
先把 `WM_KEYDOWN/UP` 扣住送引擎（`FILTER_KEY_DOWN` → 吃了才 `ON_KEY_DOWN`，键松同理）；
引擎说"吃了"就丢弃，说"没吃"才用**扣住那一刻的键盘状态**补做 `TranslateMessage` 并回放给编辑框。
键永不丢；回放字符不受 Shift 是否已松开影响；与 TSF 两阶段语义逐键等价。
扣住的键按先后顺序回放（前一个没定，后一个再明确也得等）。

**本进程不挂系统输入法**：入口先 `ImmDisableIME(-1)`，输入区再 `ImmAssociateContext(NULL)` 双保险。
否则在装了墨奇/微软拼音的机器上，键会先被系统输入法截走。

**标点分两层**（2026-11-27 定稿）：

1. **打出来就是英文标点**：面板包的数据目录里带一份 `rime/default.custom.yaml`（打包时拷进
   `data\`），把方案的 `punctuator/half_shape` 换成"原样输出"。中文状态下引擎不再转全角，
   组字中打标点照常顶屏（`hao` + `,` → `好,`），Shift 切英文再切回来也不变。
   这是面板包里的**纯数据**：墨奇本体的引擎代码和切换逻辑（中英联动标点、开关记忆）一行没动。
2. **复制时兜底**：四条路径（翻译并复制 / 复制原文 / 重新复制 / 选中自动复制）连同 Ctrl+C、
   右键"复制"、剪切，都过 `convertPunctuation()`——粘贴进来的中文标点也会换掉。
   映射表为用户定稿（23 条，含 `¥→$`、`【】→[]`、`「」→{}`），单元测试逐条锁定。

不靠"把 `ascii_punct` 开关默认打开"来做第 1 层：产品的中英联动会在切回中文时把它压回中文标点
（默认值站不住），而且它打开后标点键不再经过 punctuator——引擎不吞键，面板的按键闸门会把标点
直接回放进编辑框而组字还挂着（实测 `hao` + `,` → 编辑框只有 `,`）。所以面板里也别按方案自带的
Ctrl+Shift+3（这个开关的快捷键），按错了再按一次即可。

**补丁变了要逼引擎重新部署**（`EngineClient::refreshDataPatch`）：librime 的部署脏检查不看共享
目录里的 `*.custom.yaml`（它只在编译那一刻被读到），而"要不要重新部署"看的是 `user\user.yaml`
里的上次部署时间。所以旧文件夹原地覆盖升级（带来新补丁）或删掉补丁改回中文标点之后，引擎会
一直用旧产物。面板按补丁内容留一个戳 `user\panel-data.stamp`：戳不对就删掉 `user\build` 和
`user\user.yaml`（都可再生；只删 build 不够，引擎会直接拿包里不带补丁的预编译产物），引擎随即
重新部署。用户词典在 `user\` 根下，不受影响。本文件夹还有 daemon 活着时不动它的文件（拿单实例
互斥体当探针）。

**翻译不借 daemon 的 AI 通道**：daemon 的分发锁是全局串行的，HTTP 放进去会把所有按键卡住。
面板自己读 `translate.json`（每次翻译前重读，改文件即生效）、自己 POST。
失败时输入区、结果区、剪贴板一律不动。
默认模板 = `https://api.deepseek.com` + `deepseek-flash`（deepseek-chat 已下架，2026-11-27）；
请求对 `deepseek*` 模型默认带 `thinking:{"type":"disabled"}`（附加参数里手填的 thinking 优先；
非 deepseek 模型不发这个字段，别家接口可能不认）。

**路径必须纯 ASCII**：引擎依赖链（LevelDB、marisa-trie）用的是 ANSI 文件 API，
纯英文系统（代码页 1252）上中文路径会让用户词典和编译产物打不开。启动检查直接拦住并说明。

## 验收入口

| 命令 | 验什么 |
|---|---|
| `ctest --test-dir MoqiPanel/build-x86 -C Release` | 纯逻辑（标点映射 23 条、内容戳、字数护栏、配置/应答解析的类型安全），124 项断言 |
| `ctest --test-dir moqi-daemon/build-x86 -C Release` | daemon 既有回归 11 项（改动不得破坏） |
| `python MoqiPanel/tests/portable_daemon_check.py` | 便携 daemon：打字→候选→上屏、数据落点、不碰系统日志、无工具栏窗口、末连接断开自退 |
| `python MoqiPanel/tests/daemon_failed_engine_check.py` | 引擎停在"已加载未初始化"时发 ON_ACTIVATE 不得崩 daemon（修复前必现 0xC0000005） |
| `python MoqiPanel/tests/daemon_start_race_repro.py --rounds 20` | 面板时序下连启 daemon，异常退出数 |
| `python MoqiPanel/tests/daemon_crashtrace.py --rounds 12` | 调试器拖慢时序下抓崩溃栈（预热竞态的回归；`--client` 另有 pipe / tramp / hook 三种取证方式） |
| `python MoqiPanel/tests/drive_panel.py` | 真窗口全场景：打字→候选→上屏、翻译并复制、选中自动复制、恢复、Shift 切英文、设置对话框、关窗 |
| `python MoqiPanel/tests/punctuation_live_check.py` | 真窗口标点两层：32 个标点键打出来是英文标点、组字中标点顶屏、Shift 往返不回退、粘贴进来的 23 种中文标点经复制原文 / Ctrl+A / 拖选 / Ctrl+C 全转换；补丁删除与放回后自动重新部署 |
| `python MoqiPanel/tests/startup_checks.py` | 非 ASCII 路径、文件夹不完整 → 中文提示并拒启 |
| `python MoqiPanel/tests/outside_writes_check.py` | ProcMon：面板与它拉起的 daemon 零文件夹外写入、零注册表写入；`--control` 跑记事本基线 |

`drive_panel.py` 只向面板自己的窗口 `PostMessage`，不注入全局键盘输入；需要 `DEEPSEEK_*`
环境变量才会跑真实翻译（否则该步跳过），测试会临时覆盖剪贴板并在结束后恢复文本内容，
并会把含密钥的 `translate.json` 写进 dist——**交付前重跑
`powershell -File MoqiPanel\package-panel.ps1 -SkipBuild` 重出干净目录**。

带符号的取证构建（不碰发货构建树；`crashtrace` 的符号路径）：
```
cmake -S moqi-daemon -B moqi-daemon/build-x86-sym -G "Visual Studio 17 2022" -A Win32 ^
      -DCMAKE_CXX_FLAGS_RELEASE="-O2 -Ob2 -DNDEBUG -Zi -Gy" ^
      -DCMAKE_EXE_LINKER_FLAGS_RELEASE="-DEBUG -INCREMENTAL:NO"
cmake --build moqi-daemon/build-x86-sym --config Release --target MoqiDaemon
```
（开关要写 `-O2`：Git Bash 会把 `/O2` 当路径改写。）

## 已知约束

- 需要可写目录（U 盘可以，光盘不行）。
- 面板包默认英文标点。想改回中文标点：删掉 `moqi-ime\input_methods\rime\data\default.custom.yaml`
  后重启面板（自动重新部署，十几秒）；复制时的兜底转换仍在。
- 代理：读系统（WinINET）里配置的**手动**代理；PAC 自动配置脚本不认，请在 `translate.json` 的 `proxy` 里手填。
- 真实键盘输入下的系统输入法隔离（`ImmDisableIME`）在本机锁屏状态下无法用合成键验证，需解锁后手测一次。

## 排障提示

- 引擎一次初始化要做大约 1500 次文件操作。平时 0.3~0.9 秒；ProcMon 抓着或挂调试器时会到
  15~40 秒——那是测量工具的开销，不是产品问题（ProcMon 下面板 60 秒内没报"就绪"属正常）。
- daemon 启动即退、面板自动重连后"看起来没事"：先跑 `daemon_failed_engine_check.py`，再想时序。
  **挂调试器会让时序缺陷消失**（线程相对快慢变了），别因为调试器下不复现就判定不存在。
