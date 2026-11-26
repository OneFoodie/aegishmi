; =============================================================================
; AegisHMI Windows 安装包脚本（Inno Setup 6）
;
; 前置条件：
;   1. 先执行 scripts\build.bat Release 与 scripts\deploy.bat，生成 dist 目录；
;   2. 安装 Inno Setup 6（https://jrsoftware.org/isinfo.php）；
;   3. 编译：
;        "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" scripts\installer.iss
;      产物：aegishmi\installer\AegisHMI-0.1.0-setup.exe
; =============================================================================

#define MyAppName "AegisHMI"
#define MyAppVersion "0.1.0"
#define MyAppPublisher "AegisHMI Project"
#define MyAppExeName "aegis_hmi.exe"
#define MyAppSimName "aegis_sim.exe"

[Setup]
; AppId 用于识别同一程序的升级安装，发布后不要随意改动。
AppId={{8F2A6C1E-5B7D-4C3A-9E10-2D4B6A8F1C33}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=..\installer
OutputBaseFilename=AegisHMI-{#MyAppVersion}-setup
Compression=lzma
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; 安装包不包含第三方源码，只包含运行所需文件
UninstallDisplayName={#MyAppName}

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务:"

[Files]
; dist 已由 deploy.bat 生成：exe + Qt DLL + 插件 + configs
Source: "..\dist\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
; 示例测试方案（若 dist 已包含则重复复制也无害）
Source: "..\configs\*"; DestDir: "{app}\configs"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\设备模拟器"; Filename: "{app}\{#MyAppSimName}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent
