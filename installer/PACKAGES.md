# 墨奇输入法 Windows 安装包说明

## 一、三个下载怎么选（2026-11-21 重写；同日裁撤 essay / wiki 两档）

实测口径：产品原生栈（管道直驱 MoqiDaemon，C++ 引擎链）· 整句 1888 句（≤10 字）·
每格全新用户目录 · 独立重跑逐位一致。同一引擎同一词典，只换内置语法模型：

| 文件 | 内容 | 整句 d5 | 适合谁 |
|---|---|---|---|
| `moqi-im-windows-setup.exe`（约 71MB） | 预编译词库 + 8.6MB 蒸馏语法模型 | 67.74% | **推荐给绝大多数用户**：装完即用、体积适中 |
| `moqi-im-windows-setup-lite.exe`（约 30MB） | 1.1MB 裁剪模型，无预编译词库 | 精度让步 | 极限小包 / 老机器；首跑现场编译词库（实测约 10 秒） |
| `moqi-im-windows-setup-wanxiang.exe`（约 373MB） | + `wanxiang-lts-zh-hans`（398MB） | **71.35%（+3.61）** | 整句效果优先的重度用户 |

一句话：**日常用标准版**；老机器用 lite；整句效果优先用 wanxiang。

裁撤记录（2026-11-21，用户拍板）：

- **essay 档**（全量八股文 206MB）：同罚分下 67.43%，输给 8.6MB 蒸馏标准档
  （67.74%）——v1.7.0「蒸馏反超全量」在新引擎链上再次复现；
- **wiki 档**（自训 30.5MB）：现行资产打分与标准档逐位一致（等于白带 16MB），
  10-28 老版自训 gram 复测同样不敌标准档。

所有安装包都是**通用包**：引擎固定 32 位（`MoqiDaemon.exe` + `rime.dll`），同时安装 32/64 位两个薄 TSF DLL。32 位 Windows 和 64 位 Windows 用同一个安装包；64 位系统上的 32 位应用走 `SysWOW64` 客户端，64 位应用走 `System32` 客户端，都连同一个 daemon。

## 二、单独安装语法模型（不换包也能升级效果）

1. 拿到一个 `.gram`（例如万象 `wanxiang-lts-zh-hans.gram`）。
2. **推荐：复制到用户目录（不需要管理员权限，升级/重装也不会丢）**：

   ```text
   C:\Users\<你的用户名>\AppData\Roaming\Moqi\Rime\wanxiang-lts-zh-hans.gram
   ```

   资源管理器地址栏粘贴 `%APPDATA%\Moqi\Rime` 回车即可打开。

   也支持放安装目录（需管理员权限，但**覆盖安装时会被删掉**）：

   ```text
   C:\Program Files (x86)\MoqiIM\moqi-ime\input_methods\rime\data\
   ```

3. 让墨奇重新加载：托盘图标右键 → **"刷新配置(&R)"**，或结束 `MoqiDaemon.exe` 后重新启动。
4. 默认配置 `grammar_model` 为 `auto`，会自动选择上述目录里**最大的 `.gram`**
   （2026-11-07 起取消大小门槛，放任意 `.gram` 都能被识别），不需要改配置。

> **gram 现状（2026-11-21）**：标准包自带 8.6MB `zh-moqi.gram`（蒸馏版），
> lite 自带 1.1MB 裁剪版；wanxiang 档自带 398MB 全量模型。用户目录里放更大的
> `.gram` 会覆盖标准档的模型（auto 取最大者）。

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

- `"auto"`（默认）：自动选上面目录里最大的 `.gram`。
- `"wanxiang-lts-zh-hans"`：显式指定。**值 = 文件名去掉 `.gram`**。
- `"off"`：不启用任何外部 gram。

如果你把文件改名成 `my-model.gram`，要么把配置改成 `"my-model"`，要么保持 `"auto"`（自动选最大的）。保存后托盘 →"刷新配置(&R)"生效。

还可以用 `"grammar_penalty"` 手工微调组句罚分（范围 -20~0；不填按模型预设：
标准/精简 -4，万象 0）。

## 四、为什么没有单独的 x86 / x64 包

因为引擎是**单进程 + 32 位**的通用设计：

- `MoqiDaemon.exe` + `rime.dll`（均为 x86）：唯一引擎，32/64 位系统都能跑
- `MoqiTextService.dll`（x86）给 32 位应用，`x64\MoqiTextService.dll` 给 64 位应用
- 两个客户端都不加载 rime.dll，只通过命名管道连 daemon

所以不需要按系统架构拆包；拆开只会让用户装错。体积大头仍是词库与可选 gram。

## 五、裁撤档位的历史依据（2026-11-21）

- essay / wiki 两档与标准档的差异只在内置 gram；产品栈同引擎同词典实测，
  两者均不优于标准档 8.6MB 蒸馏模型（essay −0.31、wiki 逐位同分），
  万象模型 +3.61（其中模型 +2.81、罚分 0 贡献 +0.80）。
- 老的"单独装 essay 模型"文档路径仍然有效（把 gram 放用户目录即可），
  只是不再提供对应安装包。
