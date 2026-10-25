; Moqi IM for Windows — Inno Setup 6 wizard (x64 only).
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
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64
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
  ValueData: """{app}\MoqiLaunch.exe"""; \
  Flags: uninsdeletevalue

[InstallDelete]
Type: files; Name: "{app}\MoqiLauncher.exe"
Type: filesandordirs; Name: "{app}\moqi-ime"
Type: filesandordirs; Name: "{app}\x64"

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

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssInstall then
  begin
    HadExistingInstall := ExistingImeInstallationPresent;
    StopMoqiProcesses;
    DeleteLegacyAutorunValue;
  end;

  if CurStep = ssPostInstall then
  begin
    if not RunSetupHelper(BuildInstallSetupHelperParameters('/i'), ResultCode) then
      HandleSetupHelperResult('SetupHelper install', ResultCode);

    if ResultCode = SetupHelperExitSuccess then
    begin
      HelperInstallSucceeded := True;
      if HadExistingInstall then
        SuppressibleMsgBox(
          '检测到这是一次覆盖安装。若当前会话里仍有旧的 TSF 实例，墨奇可能要在注销或重启 Windows 后才能立即恢复正常输入。',
          mbInformation, MB_OK, IDOK);
    end
    else if ResultCode = SetupHelperExitRestartRequired then
    begin
      HelperInstallSucceeded := True;
      HelperInstallNeedsRestart := True;
      SuppressibleMsgBox(
        '安装程序已更新应用文件，但 TSF DLL 当前仍被系统占用。' + #13#10#13#10 +
        '请在安装完成后尽快重启 Windows。安装器已安排在系统重启后自动完成 TSF 注册。',
        mbInformation, MB_OK, IDOK);
    end;
    if (ResultCode <> SetupHelperExitSuccess) and
       (ResultCode <> SetupHelperExitRestartRequired) then
      HandleSetupHelperResult('SetupHelper install', ResultCode);

    // 装完直接拉起启动器（2026-10-24 定案）。不能走 [Run]+Check：
    // 静默模式下 Inno 先评估 [Run] 的 Check、后触发 ssPostInstall，
    // Check 读到的标志永远还是初始 False（实测 /LOG 时序，14-交接 §十四）。
    // 需重启场景不拉起（重启后登录任务/Run 键会拉）。
    if HelperInstallSucceeded and (not HelperInstallNeedsRestart) then
    begin
      if not Exec(ExpandConstant('{app}\MoqiLaunch.exe'), '',
                  ExpandConstant('{app}'), SW_SHOWNORMAL, ewNoWait, ResultCode) then
        Log('Launch MoqiLauncher failed, code ' + IntToStr(ResultCode))
      else
        Log('Launched MoqiLauncher after install');
    end
    else
      Log('Skip launching MoqiLauncher: succeeded=' +
        IntToStr(Ord(HelperInstallSucceeded)) + ' needsRestart=' +
        IntToStr(Ord(HelperInstallNeedsRestart)));
  end;
end;

function NeedRestart(): Boolean;
begin
  Result := HelperInstallNeedsRestart;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
begin
  if CurUninstallStep = usUninstall then
  begin
    StopMoqiProcesses;
    DeleteLegacyAutorunValue;
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
    if HelperUninstallNeedsRestart then
      SuppressibleMsgBox(
        '部分 TSF DLL 已安排在系统重启后删除。请尽快重启 Windows，以完成卸载清理。',
        mbInformation, MB_OK, IDOK);
  end;
end;
