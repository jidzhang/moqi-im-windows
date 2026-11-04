; Moqi IM for Windows — Inno Setup 6 wizard（x64；lite 另有 x86 变体）
; Build: install Inno Setup 6, then run build-installer.ps1 -StageDir <stage root>
; AppId / IME CLSID: keep stable across releases (ARP upgrade path).

#define MyAppName "墨奇输入法"
#define MyAppPublisher "Moqi"
#define MyAppURL "https://github.com/jidzhang/moqi-im-windows"
#define MyAppId "{{C7A6A2D5-16C7-4BE4-8F52-E96D6D6A9E42}"
#define ImeClsid "{{8F204C91-2D7A-4B3E-9E1F-6A5C0D8B2E7F}}"

; Injected by build-installer.ps1 (read from version.txt, or the release tag in CI).
#ifndef MyAppVersion
  #define MyAppVersion "1.0.0"
#endif

#ifndef StageDir
  #define StageDir "..\stage"
#endif

; Installer variants share one staged tree:
;   -MyOutputBaseFilename  output file name
;   -MyExcludeBuild        exclude precompiled Rime data (lite package)
;   -MyGramPath            add an external .gram model (full package)
#ifndef MyOutputBaseFilename
  #define MyOutputBaseFilename "moqi-im-windows-setup"
#endif

[Setup]
AppId={#MyAppId}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
; ARP（卸载程序列表）显示名：显式只写应用名；版本单独存 DisplayVersion，
; 由系统界面自行拼装。默认值会用 NameAndVersion 消息拼出"名称 版本 x.y.z"，
; 与 2026-10-21 定稿的显示口径（不带"版本"字样）冲突。
; 2026-10-25 用户定：版本号必须在卸载面板可见（Win10 设置>应用的列表视图
; 不显示 DisplayVersion 列），拼进 DisplayName；appwiz.cpl 的版本列由
; DisplayVersion（AppVersion 自动写）继续提供。
UninstallDisplayName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf32}\MoqiIM
; 2026-11-02 用户定：覆盖安装不弹"文件夹已存在，是否继续"确认框（默认 auto
; 会在目录页下一步时弹模态框，用户容易懵圈），直接装入既有目录。
DirExistsWarning=no
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64
; C4（台账，2026-10-23 修）：CloseApplications=no 会在结尾弹"关闭应用"交互页
; （VERYSILENT 下也弹，用户实报"点确定才关"）。产品在 ssInstall 已自行
; StopMoqiProcesses，无需 Inno 再介入关应用。
CloseApplications=no
RestartApplications=no
WizardStyle=modern
OutputDir=dist
OutputBaseFilename={#MyOutputBaseFilename}
Compression=lzma2/max
SolidCompression=yes
WizardSizePercent=110,100
DisableWelcomePage=no

[Languages]
; Use the vendored translation file so packaging does not depend on local Inno Setup language packs.
Name: "chinesesimplified"; MessagesFile: ".\Inno-Setup-Chinese-Simplified-Translation\ChineseSimplified.isl"

[Messages]
; Show "墨奇输入法 1.0.0" (no "版本" wording); override here so the third-party
; translation submodule stays pristine.
NameAndVersion=%1 %2
AboutSetupMessage=%1 %2%n%3%n%n%1 主页：%n%4

[Files]
#ifdef MyExcludeBuild
Source: "{#StageDir}\win32\MoqiIM\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs; Excludes: "{#MyExcludeBuild}"
#else
Source: "{#StageDir}\win32\MoqiIM\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
#endif
#ifdef MyGramPath
Source: "{#MyGramPath}"; DestDir: "{app}\moqi-ime\input_methods\rime\data"; Flags: ignoreversion
#endif

[Icons]
Name: "{autoprograms}\{#MyAppName}\Uninstall"; Filename: "{uninstallexe}"
Name: "{autoprograms}\{#MyAppName}\Logs"; Filename: "{win}\explorer.exe"; Parameters: """{localappdata}\MoqiIM\Log"""

[Run]
; 启动器拉起已挪到 [Code] ssPostInstall 尾部（静默模式下 [Run] 的 Check
; 先于 ssPostInstall 评估，读不到标志；见 14-交接 §十四）。此段留空。

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; \
  ValueType: string; ValueName: "MoqiLaunch"; \
  ValueData: """{app}\MoqiDaemon.exe"" --serve"; \
  Flags: uninsdeletevalue

[InstallDelete]
Type: files; Name: "{app}\MoqiLauncher.exe"
; MoqiLaunch.exe 自 2.0.0 退休（引擎在 MoqiDaemon + TSF DLL），旧包装下的
; 副本必须清掉：它被旧版计划任务拉起时会抢管道/单实例锁，让 daemon 退出，
; 32 位应用全瘫（2026-11-03 用户重启后实报）。
Type: files; Name: "{app}\MoqiLaunch.exe"
; 随包编辑器目录：2.0.0 早期包名为 Notepad4，现统一为 Notepad2（exe 改名
; notepad2.exe）；旧目录整体清掉避免两份共存。
Type: filesandordirs; Name: "{app}\Notepad4"
Type: filesandordirs; Name: "{app}\Notepad2"
Type: filesandordirs; Name: "{app}\x64"
; 不再整树删除 {app}\moqi-ime：那会在每次升级重写 110-400MB 预编译数据，
; 并把用户放进安装目录的 .gram 一起删掉。本版真正下线的旧文件逐项删除，
; 其余由 [Files] 的 ignoreversion 覆盖。
Type: files; Name: "{app}\MoqiIME.exe"
Type: files; Name: "{app}\moqi-ime\MoqiIME.exe"
Type: files; Name: "{app}\moqi-ime\input_methods\rime\data\zh-moqi.gram"
Type: files; Name: "{app}\mdaemon.*"

[Code]
const
  SetupHelperExitSuccess = 0;
  SetupHelperExitRestartRequired = 2;

var
  HelperInstallSucceeded: Boolean;
  HelperInstallNeedsRestart: Boolean;
  HelperUninstallNeedsRestart: Boolean;
  HadExistingInstall: Boolean;

function ExistingImeInstallationPresent: Boolean;
begin
  Result :=
    FileExists(ExpandConstant('{app}\MoqiLaunch.exe')) or
    FileExists(ExpandConstant('{app}\MoqiLauncher.exe')) or
    FileExists(ExpandConstant('{syswow64}\MoqiTextService.dll')) or
    FileExists(ExpandConstant('{sys}\MoqiTextService.dll')) or
    RegKeyExists(HKEY_LOCAL_MACHINE, 'SOFTWARE\Microsoft\CTF\TIP\{#ImeClsid}') or
    RegKeyExists(HKEY_CURRENT_USER, 'Software\Microsoft\CTF\TIP\{#ImeClsid}') or
    RegKeyExists(HKEY_CLASSES_ROOT, 'CLSID\{#ImeClsid}') or
    RegKeyExists(HKEY_CURRENT_USER, 'Software\Classes\CLSID\{#ImeClsid}');
end;

procedure RegPurgeMoqiResiduals;
var
  ClsidKey: String;
  TipKey: String;
begin
  ClsidKey := 'CLSID\{#ImeClsid}';
  TipKey := 'SOFTWARE\Microsoft\CTF\TIP\{#ImeClsid}';
  if RegKeyExists(HKEY_CLASSES_ROOT, ClsidKey) then
    RegDeleteKeyIncludingSubkeys(HKEY_CLASSES_ROOT, ClsidKey);
  if RegKeyExists(HKEY_LOCAL_MACHINE, TipKey) then
    RegDeleteKeyIncludingSubkeys(HKEY_LOCAL_MACHINE, TipKey);
  if RegKeyExists(HKEY_CURRENT_USER, 'Software\Microsoft\CTF\TIP\{#ImeClsid}') then
    RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER, 'Software\Microsoft\CTF\TIP\{#ImeClsid}');
  if RegKeyExists(HKEY_CURRENT_USER, 'Software\Classes\CLSID\{#ImeClsid}') then
    RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER, 'Software\Classes\CLSID\{#ImeClsid}');
end;

procedure TryKillProcessImage(const ImageName: String);
var
  R: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /T /IM "' + ImageName + '"',
    '', SW_HIDE, ewWaitUntilTerminated, R);
end;

procedure TryKillBackendServers;
var
  R: Integer;
begin
  // 后端（v1.5.0 起叫 MoqiIME.exe，旧版叫 server.exe）在启动器已退出时是
  // 孤儿进程，不在其进程树里；留着它会继续用旧代码/旧数据服务客户端，
  // 且锁住 exe 使覆盖安装写不进新文件。
  // 按路径过滤只杀本安装目录下的后端，避免误杀其他软件的同名进程。
  Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
    '-NoProfile -ExecutionPolicy Bypass -Command "' +
    'Get-Process MoqiIME,server -ErrorAction SilentlyContinue |' +
    ' Where-Object { $_.Path -like ''' + ExpandConstant('{app}') + '\*'' } |' +
    ' Stop-Process -Force"',
    '', SW_HIDE, ewWaitUntilTerminated, R);
end;

procedure DeleteLegacyAutorunValue;
begin
  // v1.6.0 起运行键值名 MoqiLaunch；旧名留着会变成指向已删 exe 的死启动项。
  RegDeleteValue(HKEY_CURRENT_USER, 'Software\Microsoft\Windows\CurrentVersion\Run',
    'MoqiLauncher');
end;

procedure StopMoqiProcesses;
begin
  TryKillProcessImage('MoqiDaemon.exe');
TryKillProcessImage('MoqiLaunch.exe');
  TryKillProcessImage('MoqiLauncher.exe');
  TryKillBackendServers;
end;

function GetSetupHelperPath: string;
begin
  Result := ExpandConstant('{app}\SetupHelper.exe');
end;

procedure EnsureSetupHelperExists;
begin
  if not FileExists(GetSetupHelperPath) then
    RaiseException('SetupHelper.exe not found: ' + GetSetupHelperPath);
end;

function RunSetupHelper(const Parameters: string; var ResultCode: Integer): Boolean;
begin
  EnsureSetupHelperExists;
  Result := Exec(GetSetupHelperPath, Parameters, ExpandConstant('{app}'),
    SW_HIDE, ewWaitUntilTerminated, ResultCode);
  if not Result then
    ResultCode := -1;
end;

function BuildInstallSetupHelperParameters(const Action: string): string;
begin
  Result := Action;
  if WizardSilent() then
    Result := Result + ' /s';
  Result := Result + ' --appdir "' + ExpandConstant('{app}') + '"';
end;

function BuildUninstallSetupHelperParameters(const Action: string): string;
begin
  Result := Action;
  if UninstallSilent() then
    Result := Result + ' /s';
  Result := Result + ' --appdir "' + ExpandConstant('{app}') + '"';
end;

procedure HandleSetupHelperResult(const Operation: string; const ResultCode: Integer);
begin
  RaiseException(Operation + ' failed (exit code ' + IntToStr(ResultCode) + ').');
end;

// Free a file that may be locked by a running process (loaded DLL).
// Windows allows renaming a loaded DLL: the loading process keeps the old
// file content via its existing handle, while the name is freed for the
// installer to write a fresh copy. Renamed copies are cleaned up by the
// next install's [InstallDelete] or by CleanupStaleOldFiles in SetupHelper.
procedure FreeLockedFile(const FilePath: string);
var
  I: Integer;
  NewName: string;
begin
  if FileExists(FilePath) then
  begin
    // Try delete first (works when not locked).
    if DeleteFile(FilePath) then Exit;
    // Locked: rename out of the way (always succeeds on loaded DLLs).
    I := 0;
    repeat
      Inc(I);
      NewName := FilePath + '.busy' + IntToStr(I);
    until not FileExists(NewName);
    RenameFile(FilePath, NewName);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  FindRec: TFindRec;
  DataDir: string;
begin
  if CurStep = ssInstall then
  begin
    HadExistingInstall := ExistingImeInstallationPresent;
    StopMoqiProcesses;
    DeleteLegacyAutorunValue;
    // rime.dll + gram/bin files are loaded or memory-mapped in-process by
    // every app that typed Chinese (architecture phase 1). Killing
    // MoqiIME is not enough — free each by renaming so the installer can
    // write fresh copies without "access denied".
    FreeLockedFile(ExpandConstant('{app}\moqi-ime\input_methods\rime\rime.dll'));
    // Free all .gram files (memory-mapped by octagram plugin).
    DataDir := ExpandConstant('{app}\moqi-ime\input_methods\rime\data');
    if FindFirst(DataDir + '\*.gram', FindRec) then
    begin
      try
        repeat
          FreeLockedFile(DataDir + '\' + FindRec.Name);
        until not FindNext(FindRec);
      finally
        FindClose(FindRec);
      end;
    end;
    // Free prebuilt build files (memory-mapped by librime dictionaries).
    DataDir := DataDir + '\build';
    if FindFirst(DataDir + '\*.bin', FindRec) then
    begin
      try
        repeat
          FreeLockedFile(DataDir + '\' + FindRec.Name);
        until not FindNext(FindRec);
      finally
        FindClose(FindRec);
      end;
    end;
  end;

  if CurStep = ssPostInstall then
  begin
    if not RunSetupHelper(BuildInstallSetupHelperParameters('/i'), ResultCode) then
      HandleSetupHelperResult('SetupHelper install', ResultCode);

    if ResultCode = SetupHelperExitSuccess then
    begin
      HelperInstallSucceeded := True;
    end
    else if ResultCode = SetupHelperExitRestartRequired then
    begin
      HelperInstallSucceeded := True;
      HelperInstallNeedsRestart := True;
    end;
    if (ResultCode <> SetupHelperExitSuccess) and
       (ResultCode <> SetupHelperExitRestartRequired) then
      HandleSetupHelperResult('SetupHelper install', ResultCode);

    // 装完直接拉起启动器（2026-10-24 定案）。不能走 [Run]+Check：
    // 静默模式下 Inno 先评估 [Run] 的 Check、后触发 ssPostInstall，
    // Check 读到的标志永远还是初始 False（实测 /LOG 时序，14-交接 §十四）。
    // 2026-11-03：DLL 被占用走改名路径时也拉起——SetupHelper 在该路径同样
    // 完成注册+激活（见 SetupHelper RunInstall），不拉起只会让 x86 应用
    // 首键多等守护复活（实测覆盖安装后 daemon 缺席）。
    if HelperInstallSucceeded then
    begin
      if not Exec(ExpandConstant('{app}\MoqiDaemon.exe'), '--serve',
                  ExpandConstant('{app}'), SW_SHOWNORMAL, ewNoWait, ResultCode) then
        Log('Launch MoqiDaemon failed, code ' + IntToStr(ResultCode))
      else
        Log('Launched MoqiDaemon after install');
    end
    else
      Log('Skip launching MoqiDaemon: install failed');
  end;
end;

// 2026-11-02 用户定：不用弹出式对话框；提示写到完成页，且仅限覆盖安装时出现；首次安装不提示重启。
procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpFinished then
  begin
    if HelperUninstallNeedsRestart then
      WizardForm.FinishedLabel.Caption := '部分 TSF DLL 已安排在系统重启后删除。请尽快重启 Windows，以完成卸载清理。'
    else if HelperInstallNeedsRestart or HadExistingInstall then
      WizardForm.FinishedLabel.Caption := '建议安装完成后重启电脑，让新版墨奇输入法立即生效。';
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  // 必须早于 Inno 6.3+ 自动调用的旧版卸载器：旧 SetupHelper /u 在系统 TSF
  // DLL 被记事本等应用占用时会返回"需重启"，新安装器随即退出码 5、一个
  // 文件都不装（2026-11-03 覆盖装实报）。这里先把被占用的 DLL 改名让出
  // 正式路径，旧卸载器就能正常走完；kill daemon 也避免它锁 rime.dll。
  StopMoqiProcesses;
  FreeLockedFile(ExpandConstant('{sys}\MoqiTextService.dll'));
  FreeLockedFile(ExpandConstant('{syswow64}\MoqiTextService.dll'));
  Result := '';
end;

function NeedRestart(): Boolean;
begin
  // 极静默安装时 Inno 会不问直接重启（丢用户未保存工作）。交互安装仍
  // 在完成页提示；静默安装把重启交给调用方 /NORESTART 决定。
  Result := HelperInstallNeedsRestart and not WizardSilent();
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
begin
  if CurUninstallStep = usUninstall then
  begin
    StopMoqiProcesses;
    DeleteLegacyAutorunValue;
    // 2026-11-03：卸载（含 Inno 6.3+ 升级时自动跑的旧卸载器）前先把可能被
    // 运行中进程占用的 rime.dll 改名让路——否则 Inno 自己删不掉会标记
    // "需重启"，新安装器直接退出码 5 什么都不装（覆盖安装实测）。
    FreeLockedFile(ExpandConstant('{app}\moqi-ime\input_methods\rime\rime.dll'));
    FreeLockedFile(ExpandConstant('{app}\rime.dll'));
    if not RunSetupHelper(BuildUninstallSetupHelperParameters('/u'), ResultCode) then
      HandleSetupHelperResult('SetupHelper uninstall', ResultCode);
    if ResultCode = SetupHelperExitRestartRequired then
      HelperUninstallNeedsRestart := True
    else if ResultCode <> SetupHelperExitSuccess then
      HandleSetupHelperResult('SetupHelper uninstall', ResultCode);
  end;
  if CurUninstallStep = usPostUninstall then
  begin
    RegPurgeMoqiResiduals;
  end;
end;
