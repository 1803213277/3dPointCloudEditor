; 3D 点云编辑器 —— Inno Setup 安装包脚本
;
; 编译：ISCC.exe Setup.iss /DAppVersion=1.0.0
; 或直接用仓库根的 tools\make-installer.ps1（会先构建再调用本脚本）。
;
; 默认 per-user 安装（%LocalAppData%\Programs\3dPointCloudEditor），不需要管理员权限；
; 用户在安装界面点“为所有用户安装”时才走 UAC。

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif

#ifndef BuildDir
  #define BuildDir "..\..\x64\Release"
#endif

; 需要排查安装过程时传 /DSetupLogging=yes，日志会写到 %TEMP%\Setup Log*.txt
#ifndef SetupLogging
  #define SetupLogging "no"
#endif

#define AppName "3D 点云编辑器"
#define AppNameEn "3D Point Cloud Editor"
#define AppPublisher "zhengzhx"
#define AppExeName "3dPointCloudEditor.exe"
#define AppUrl "https://github.com/1803213277/3dPointCloudEditor"

[Setup]
AppId={{A906E836-8724-4D77-8965-47A5374511AB}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
VersionInfoVersion={#AppVersion}
VersionInfoDescription={#AppNameEn} Setup
VersionInfoProductName={#AppNameEn}
DefaultDirName={localappdata}\Programs\3dPointCloudEditor
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
; 默认永远装到当前用户目录：不需要管理员权限，也不会因为写 C:\Program Files 失败而报“无法创建目录”。
; 需要为所有用户安装时，用命令行显式指定：Setup.exe /ALLUSERS（会走 UAC）
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=commandline
; 不沿用注册表里记录的旧安装目录，避免上一次失败的安装把路径带到 Program Files
UsePreviousAppDir=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir=..\..\dist
OutputBaseFilename=3dPointCloudEditor-{#AppVersion}-setup
SetupIconFile=..\..\3dPointCloudEditor\app.ico
UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName={#AppName}
Compression=lzma2/max
SolidCompression=yes
SetupLogging={#SetupLogging}
WizardStyle=modern
ShowLanguageDialog=auto
CloseApplications=yes
RestartApplications=no
AllowNoIcons=yes

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
; 整个免安装目录（exe + pri + 运行时 DLL + samples + 图标）；
; 调试符号/链接中间文件不打包进安装包
Source: "{#BuildDir}\*"; DestDir: "{app}"; Excludes: "*.pdb,*.lib,*.exp,*.ilk,*.appxrecipe,*.obj"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(AppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; 示例点云和运行时 DLL 都是安装时写入的，卸载时由 Inno 自动清理；
; 这里只清理用户可能生成在安装目录里的缓存文件
Type: filesandordirs; Name: "{app}\.cache"
