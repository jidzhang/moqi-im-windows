# 墨奇输入法 Windows 安装包说明

## 一、五个下载怎么选（2026-10-27 实测重写）

实测口径：产品原生栈（rime.dll + Go 管线，无 GUI）· 整句 120 句 + 混拼 500 对 ·
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

所有安装包都同时安装 32 位和 64 位 TSF DLL，不需要按系统架构二选一。

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

3. 让墨奇重新加载：托盘图标右键 → **“刷新配置(&R)”**，或退出 `MoqiLauncher` 再启动。
4. 默认配置 `grammar_model` 为 `auto`，会自动选择上述两个目录里 **≥50MB 的最大 `.gram`**（2026-10-27 由 64MB 下调，自训模型 64.5MB 由此可被自动识别），不需要改配置。

复制后目录里应该有（二选一即可，推荐用户目录）：

```text
%APPDATA%\Moqi\Rime\zh-hans-t-essay-bgw.gram            ← 推荐（普通权限，升级不丢）
C:\Program Files (x86)\MoqiIM\...\rime\data\zh-hans-t-essay-bgw.gram  ← 备选（管理员）
```

> **2026-10-21 起安装包不再自带 7MB 的 `zh-moqi.gram`**：实测它对准确率没有提升
> （整句 59.92 → 59.95，噪声级），却让每个包白背 7MB，所以直接删掉了。
> 标准包/lite 包里现在**一个 `.gram` 都没有**，这是预期行为，不是装漏了。

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

三个包都带：

- `MoqiTextService.dll`（32 位，1.67 MB）
- `x64\MoqiTextService.dll`（64 位，1.95 MB）

拆开最多省约 2 MB 原始体积（压缩后约 1 MB，不到整包的 1%），但会让另一架构的应用（部分 32 位软件）失去输入法支持，`SetupHelper` 也需要同时拿到两个 DLL 才能完成注册。体积大头是词库（122 MB 预编译数据 + 约 100 MB 源数据）和可选的 196.8 MB gram，所以保持一个包同时装双架构。

## 五、本次更新（2026-10-21）要点

- **数字/小数点直通**：中文模式下打 `4.` 直接出 `4.`（不再丢数字、光标卡在 `.` 上）；
  `4.5` → `4.5`，`12.` → `12.`；单独打 `.` 仍是中文句号 `。`；
  拼音输入过程中按数字仍然用于选择第 N 个候选。
- **候选窗**：首格高亮四边贴边（上下左右都贴到边），直角；默认 7 个候选、每行 9 个。
- **语法模型**：不再自带 7MB 小模型；标准包/lite 包不含任何 `.gram`，
  essay / wanxiang / wiki 三档各带一个模型；也可单独下载 `.gram` 放入用户目录启用。
