# 墨奇输入法 Windows 安装包说明

## 一、五个下载怎么选（2026-10-27 实测重写）

实测口径：产品原生栈（rime.dll + C++ daemon，无 GUI）· 整句 120 句 + 混拼 500 对 ·
每臂全新用户目录 · 复跑一致。方法与冻结工件见 benchmark 仓
`docs/protocol_native_stack.md`、结论见 `docs/install_recommendation.md`。

| 文件 | 内容 | 整句 | 混拼 | 适合谁 |
|---|---|---|---|---|
| `moqi-im-windows-setup.exe` | 预编译词库，无语法模型 | 54.17% | **41.40%** | **推荐给绝大多数用户**：混拼最优、体积适中、装完即用 |
| `moqi-im-windows-setup-lite.exe` | 不含预编译词库与模型 | 54.17% | 41.40% | 只想要最小包；与标准版解码同分，仅首跑现场编译词库（约 20 秒） |
| `moqi-im-windows-setup-essay.exe` | + `zh-hans-t-essay-bgw`（206MB） | **58.33%（+4.16）** | 38.20%（−3.20） | 长句优先，能接受混拼小幅下降 |
| `moqi-im-windows-setup-wanxiang.exe` | + `wanxiang-lts-zh-hans`（398MB） | **63.33%（+9.16）** | 31.80%（−9.60） | 只写长文/连续整句的重度用户；**混拼明显变差，日常短句不建议** |
| `moqi-im-windows-setup-wiki.exe` | + `zh-moqi-wiki`（64.5MB，自训/纯维基 CC BY-SA） | 54.17%（±0） | 41.40%（±0） | **不建议**：与无模型档双维度同分（正考虑下架） |

一句话：**日常用标准版**；写长文用 wanxiang；中间选择 essay；极限小包用 lite；
wiki 档暂无收益。

所有安装包都是**通用包**：引擎固定 32 位（`MoqiDaemon.exe` + `rime.dll`），同时安装 32/64 位两个薄 TSF DLL。32 位 Windows 和 64 位 Windows 用同一个安装包；64 位系统上的 32 位应用走 `SysWOW64` 客户端，64 位应用走 `System32` 客户端，都连同一个 daemon。

## 二、单独安装语法模型（第 4 个文件）

1. 下载 `zh-hans-t-essay-bgw.gram`。
2. **推荐：复制到用户目录（不需要管理员权限，升级/重装也不会丢）**：

   ```text
   C:\Users\<你的用户名>\AppData\Roaming\Moqi\Rime\zh-hans-t-essay-bgw.gram
   ```

   资源管理器地址栏粘贴 `%APPDATA%\Moqi\Rime` 回车即可打开。

   也支持放安装目录（需管理员权限，但**覆盖安装时会被删掉**）：

   ```text
   C:\Program Files (x86)\MoqiIM\moqi-ime\input_methods\rime\data\
   ```

3. 让墨奇重新加载：托盘图标右键 → **“刷新配置(&R)”**，或结束 `MoqiDaemon.exe` 后重新启动。
4. 默认配置 `grammar_model` 为 `auto`，会自动选择上述两个目录里 **≥50MB 的最大 `.gram`**（2026-10-27 由 64MB 下调，自训模型 64.5MB 由此可被自动识别），不需要改配置。

复制后目录里应该有（二选一即可，推荐用户目录）：

```text
%APPDATA%\Moqi\Rime\zh-hans-t-essay-bgw.gram            ← 推荐（普通权限，升级不丢）
C:\Program Files (x86)\MoqiIM\...\rime\data\zh-hans-t-essay-bgw.gram  ← 备选（管理员）
```

> **gram 现状（2026-11-04）**：标准包自带 8.6MB `zh-moqi.gram`（实测整句 +3.5 分），
> lite 自带 1.1MB 裁剪版；两档都不是“零 gram”。essay / wanxiang / wiki 各带一个
> 大模型。也可以把 `.gram` 放用户目录单独启用。

> 覆盖安装会先清空 `{app}\moqi-ime` 再释放新文件，放在安装目录里的 `.gram` 会被删掉；
> 放在 `%APPDATA%\Moqi\Rime\` 里的不受影响。

## 三、改配置里的模型名

配置文件：`%APPDATA%\Moqi\local_behavior.json`（没有就新建，UTF-8 编码）：

```json
{
  "grammar_model": "auto"
}
```

可选值：

- `"auto"`（默认）：自动选上面两个目录里最大的、≥50MB 的 `.gram`。
- `"zh-hans-t-essay-bgw"`：显式指定。**值 = 文件名去掉 `.gram`**。
- `"off"`：不启用任何外部 gram。

如果你把文件改名成 `my-model.gram`，要么把配置改成 `"my-model"`，要么保持 `"auto"`（自动选最大的）。保存后托盘 →“刷新配置(&R)”生效。

> 不建议用万象 LMDG 替换 essay-bgw：2026-10-17 同口径实测，万象 d3 混合 42.65（essay 54.35）、d4 延续 49.40（essay 51.95），核心场景明显倒退，只有 d5 整句 +1.5。

## 四、为什么没有单独的 x86 / x64 包

因为引擎是**单进程 + 32 位**的通用设计：

- `MoqiDaemon.exe` + `rime.dll`（均为 x86）：唯一引擎，32/64 位系统都能跑
- `MoqiTextService.dll`（x86）给 32 位应用，`x64\MoqiTextService.dll` 给 64 位应用
- 两个客户端都不加载 rime.dll，只通过命名管道连 daemon

所以不需要按系统架构拆包；拆开只会让用户装错。体积大头仍是词库与可选 gram。

## 五、本次更新（2026-10-21）要点

- **数字/小数点直通**：中文模式下打 `4.` 直接出 `4.`（不再丢数字、光标卡在 `.` 上）；
  `4.5` → `4.5`，`12.` → `12.`；单独打 `.` 仍是中文句号 `。`；
  拼音输入过程中按数字仍然用于选择第 N 个候选。
- **候选窗**：首格高亮四边贴边（上下左右都贴到边），直角；默认 7 个候选、每行 9 个。
- **语法模型**：标准包自带 8.6MB 小模型，lite 带 1.1MB 裁剪版；
  essay / wanxiang / wiki 三档各带一个模型；也可单独下载 `.gram` 放入用户目录启用。
