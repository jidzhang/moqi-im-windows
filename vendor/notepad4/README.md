# 随包文本编辑器（Notepad4，文件名为 notepad2.exe）

- 上游：<https://github.com/zufuliu/notepad4>，版本 `v26.10r6392`
- 取用资产：`Notepad4_zh-Hans_Win32_v26.10r6392.zip`（32 位、简体中文内建，
  单文件 2.0MB；matepath 等其余内容不带）
- **文件名 `notepad2.exe` 是本地改名**：用户 2026-11-03 定（"4"在汉语文化里
  寓意不好），与上游 Notepad2 项目无关，程序本身仍是 Notepad4。
- 许可：BSD-3-Clause（见 `License.txt`，随安装包分发到 `{app}\Notepad2\`）
- 用途：语言栏菜单"打开置顶短语/超级简拼/成对符号设置/配置AI助手/WebDAV
  配置"等命令用它打开配置文件（JSON/YAML 高亮）。加载顺序：
  `{app}\Notepad2\notepad2.exe` → 系统 notepad.exe → 系统关联。
- 选 Win32 版：MoqiDaemon 是 x86 进程，32 位编辑器在任意安装位上都可直接
  拉起，且体积最小。
- 升级版本时同步更新本文件与 NOTICE.md，并保持 exe 文件名不变。