; AI 变声器 安装脚本 (Inno Setup 6)
; 编译: iscc installer\setup.iss   (先编译 voice_changer, obs_plugin, driver, 并准备好 voice_changer\dist)
#define AppName "AI 变声器"
#define AppVer "2.0.0"
#define Dist "..\voice_changer\dist"

[Setup]
AppId={{8C4C2A51-6E7B-4F3A-9C1E-AIVC00000002}
AppName={#AppName}
AppVersion={#AppVer}
AppPublisher=AI Voice Changer
DefaultDirName={autopf}\AIVoiceChanger
DefaultGroupName={#AppName}
OutputDir=..\release
OutputBaseFilename=AIVoiceChanger-Setup-{#AppVer}
SetupIconFile=..\voice_changer\src\app.ico
UninstallDisplayIcon={app}\VoiceChanger.exe
Compression=lzma2/fast
SolidCompression=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
WizardStyle=modern
DiskSpanning=no

[Languages]
Name: "cn"; MessagesFile: "ChineseSimplified.isl"

[Tasks]
Name: "desktop"; Description: "创建桌面快捷方式"
Name: "obs"; Description: "安装 OBS 插件（在 OBS 里直接添加「AI 变声器」音频源）"; Check: ObsInstalled
Name: "vmic"; Description: "安装虚拟麦克风「AI变声器 麦克风」（测试版驱动：需开启 Windows 测试模式并重启）"; Flags: unchecked

[Files]
Source: "{#Dist}\VoiceChanger.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Dist}\vc_cli.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Dist}\onnxruntime.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Dist}\onnxruntime_providers_shared.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Dist}\DirectML.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Dist}\models\common\*.onnx"; DestDir: "{app}\models\common"; Flags: ignoreversion
Source: "{#Dist}\voices\*"; DestDir: "{app}\voices"; Flags: ignoreversion recursesubdirs; Excludes: "*_fp16_tmp*"
Source: "..\README.md"; DestDir: "{app}"; DestName: "使用说明.md"
Source: "..\obs_plugin\build\Release\obs-ai-voice.dll"; DestDir: "{code:ObsPluginDir}"; Tasks: obs; Flags: ignoreversion
Source: "..\driver\package\*"; DestDir: "{app}\driver\package"; Flags: ignoreversion
Source: "..\driver\install_test.ps1"; DestDir: "{app}\driver"; Flags: ignoreversion
Source: "..\driver\uninstall.ps1"; DestDir: "{app}\driver"; Flags: ignoreversion
Source: "..\driver\scripts\*"; DestDir: "{app}\driver\scripts"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\VoiceChanger.exe"
Name: "{group}\安装虚拟麦克风 (测试版)"; Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -File ""{app}\driver\install_test.ps1"" -EnableTestSigning"; WorkingDir: "{app}\driver"
Name: "{group}\卸载 {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\VoiceChanger.exe"; Tasks: desktop

[Run]
Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -File ""{app}\driver\install_test.ps1"" -EnableTestSigning"; WorkingDir: "{app}\driver"; Tasks: vmic; Flags: runascurrentuser waituntilterminated; StatusMsg: "正在安装虚拟麦克风驱动..."
Filename: "{app}\VoiceChanger.exe"; Description: "立即启动 {#AppName}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -File ""{app}\driver\uninstall.ps1"""; WorkingDir: "{app}\driver"; Flags: runhidden; RunOnceId: "UninstallVMic"

[Code]
function ObsDir(): String;
begin
  Result := '';
  if not RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', Result) then
    if DirExists(ExpandConstant('{commonpf64}\obs-studio')) then
      Result := ExpandConstant('{commonpf64}\obs-studio');
end;

function ObsInstalled(): Boolean;
begin
  Result := ObsDir() <> '';
end;

function ObsPluginDir(Param: String): String;
begin
  Result := ObsDir() + '\obs-plugins\64bit';
end;
