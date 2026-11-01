; LiteView 安裝檔（Inno Setup 6）
; 建置：iscc /DAppVersion=0.1.0 /DSourceExe=..\build\Release\LiteView.exe installer\LiteView.iss
; 只裝給目前使用者，不需要系統管理員權限；檔案關聯寫在 HKCU，預設不勾。

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef SourceExe
  #define SourceExe "..\build\Release\LiteView.exe"
#endif
#ifndef OutputDir
  #define OutputDir "..\build\installer"
#endif

[Setup]
AppId={{6F1E2B3A-8C4D-4E5F-9A0B-1C2D3E4F5A6B}
AppName=LiteView
AppVersion={#AppVersion}
AppVerName=LiteView {#AppVersion}
AppPublisher=LiteView
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\LiteView
DefaultGroupName=LiteView
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=LiteView-{#AppVersion}-setup
SetupIconFile=..\src\res\LiteView.ico
UninstallDisplayIcon={app}\LiteView.exe
UninstallDisplayName=LiteView
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
; 不放精靈圖片，讓安裝檔維持在 2 MB 以內。
WizardImageFile=
WizardSmallImageFile=
ChangesAssociations=yes
CloseApplications=yes

[Languages]
; ChineseTraditional.isl 取自 Inno Setup 原始碼（Files/Languages/Unofficial，tag is-6_4_3）。
Name: "zh_tw"; MessagesFile: "ChineseTraditional.isl"

[CustomMessages]
zh_tw.AssocTask=設為圖片的開啟程式（.jpg、.png、.gif 等）
zh_tw.AssocGroup=檔案關聯：
zh_tw.DesktopGroup=其他捷徑：
zh_tw.LaunchApp=立即執行 LiteView

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:DesktopGroup}"; Flags: unchecked
Name: "assoc"; Description: "{cm:AssocTask}"; GroupDescription: "{cm:AssocGroup}"; Flags: unchecked

[Files]
Source: "{#SourceExe}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\LiteView"; Filename: "{app}\LiteView.exe"
Name: "{autodesktop}\LiteView"; Filename: "{app}\LiteView.exe"; Tasks: desktopicon

[Registry]
; 程式本身不寫登錄檔；以下只在勾選檔案關聯時寫入，解除安裝時移除。
Root: HKCU; Subkey: "Software\Classes\LiteView.Image"; ValueType: string; ValueName: ""; ValueData: "LiteView 圖片"; Flags: uninsdeletekey; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\LiteView.Image\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\LiteView.exe"",0"; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\LiteView.Image\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\LiteView.exe"" ""%1"""; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Applications\LiteView.exe"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "LiteView"; Flags: uninsdeletekey; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Applications\LiteView.exe\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\LiteView.exe"" ""%1"""; Tasks: assoc
Root: HKCU; Subkey: "Software\LiteView\Capabilities"; ValueType: string; ValueName: "ApplicationName"; ValueData: "LiteView"; Flags: uninsdeletekey; Tasks: assoc
Root: HKCU; Subkey: "Software\LiteView\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "輕巧快速的看圖程式"; Tasks: assoc
Root: HKCU; Subkey: "Software\RegisteredApplications"; ValueType: string; ValueName: "LiteView"; ValueData: "Software\LiteView\Capabilities"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\.jpg\OpenWithProgids"; ValueType: none; ValueName: "LiteView.Image"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKCU; Subkey: "Software\LiteView\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jpg"; ValueData: "LiteView.Image"; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Applications\LiteView.exe\SupportedTypes"; ValueType: string; ValueName: ".jpg"; ValueData: ""; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\.jpeg\OpenWithProgids"; ValueType: none; ValueName: "LiteView.Image"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKCU; Subkey: "Software\LiteView\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jpeg"; ValueData: "LiteView.Image"; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Applications\LiteView.exe\SupportedTypes"; ValueType: string; ValueName: ".jpeg"; ValueData: ""; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\.jpe\OpenWithProgids"; ValueType: none; ValueName: "LiteView.Image"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKCU; Subkey: "Software\LiteView\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jpe"; ValueData: "LiteView.Image"; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Applications\LiteView.exe\SupportedTypes"; ValueType: string; ValueName: ".jpe"; ValueData: ""; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\.jfif\OpenWithProgids"; ValueType: none; ValueName: "LiteView.Image"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKCU; Subkey: "Software\LiteView\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jfif"; ValueData: "LiteView.Image"; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Applications\LiteView.exe\SupportedTypes"; ValueType: string; ValueName: ".jfif"; ValueData: ""; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\.png\OpenWithProgids"; ValueType: none; ValueName: "LiteView.Image"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKCU; Subkey: "Software\LiteView\Capabilities\FileAssociations"; ValueType: string; ValueName: ".png"; ValueData: "LiteView.Image"; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Applications\LiteView.exe\SupportedTypes"; ValueType: string; ValueName: ".png"; ValueData: ""; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\.bmp\OpenWithProgids"; ValueType: none; ValueName: "LiteView.Image"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKCU; Subkey: "Software\LiteView\Capabilities\FileAssociations"; ValueType: string; ValueName: ".bmp"; ValueData: "LiteView.Image"; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Applications\LiteView.exe\SupportedTypes"; ValueType: string; ValueName: ".bmp"; ValueData: ""; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\.dib\OpenWithProgids"; ValueType: none; ValueName: "LiteView.Image"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKCU; Subkey: "Software\LiteView\Capabilities\FileAssociations"; ValueType: string; ValueName: ".dib"; ValueData: "LiteView.Image"; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Applications\LiteView.exe\SupportedTypes"; ValueType: string; ValueName: ".dib"; ValueData: ""; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\.gif\OpenWithProgids"; ValueType: none; ValueName: "LiteView.Image"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKCU; Subkey: "Software\LiteView\Capabilities\FileAssociations"; ValueType: string; ValueName: ".gif"; ValueData: "LiteView.Image"; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Applications\LiteView.exe\SupportedTypes"; ValueType: string; ValueName: ".gif"; ValueData: ""; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\.tif\OpenWithProgids"; ValueType: none; ValueName: "LiteView.Image"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKCU; Subkey: "Software\LiteView\Capabilities\FileAssociations"; ValueType: string; ValueName: ".tif"; ValueData: "LiteView.Image"; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Applications\LiteView.exe\SupportedTypes"; ValueType: string; ValueName: ".tif"; ValueData: ""; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\.tiff\OpenWithProgids"; ValueType: none; ValueName: "LiteView.Image"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKCU; Subkey: "Software\LiteView\Capabilities\FileAssociations"; ValueType: string; ValueName: ".tiff"; ValueData: "LiteView.Image"; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Applications\LiteView.exe\SupportedTypes"; ValueType: string; ValueName: ".tiff"; ValueData: ""; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\.ico\OpenWithProgids"; ValueType: none; ValueName: "LiteView.Image"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKCU; Subkey: "Software\LiteView\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ico"; ValueData: "LiteView.Image"; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Applications\LiteView.exe\SupportedTypes"; ValueType: string; ValueName: ".ico"; ValueData: ""; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\.webp\OpenWithProgids"; ValueType: none; ValueName: "LiteView.Image"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKCU; Subkey: "Software\LiteView\Capabilities\FileAssociations"; ValueType: string; ValueName: ".webp"; ValueData: "LiteView.Image"; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Applications\LiteView.exe\SupportedTypes"; ValueType: string; ValueName: ".webp"; ValueData: ""; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\.heic\OpenWithProgids"; ValueType: none; ValueName: "LiteView.Image"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKCU; Subkey: "Software\LiteView\Capabilities\FileAssociations"; ValueType: string; ValueName: ".heic"; ValueData: "LiteView.Image"; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Applications\LiteView.exe\SupportedTypes"; ValueType: string; ValueName: ".heic"; ValueData: ""; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\.heif\OpenWithProgids"; ValueType: none; ValueName: "LiteView.Image"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKCU; Subkey: "Software\LiteView\Capabilities\FileAssociations"; ValueType: string; ValueName: ".heif"; ValueData: "LiteView.Image"; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Applications\LiteView.exe\SupportedTypes"; ValueType: string; ValueName: ".heif"; ValueData: ""; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\.jxr\OpenWithProgids"; ValueType: none; ValueName: "LiteView.Image"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKCU; Subkey: "Software\LiteView\Capabilities\FileAssociations"; ValueType: string; ValueName: ".jxr"; ValueData: "LiteView.Image"; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Applications\LiteView.exe\SupportedTypes"; ValueType: string; ValueName: ".jxr"; ValueData: ""; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\.wdp\OpenWithProgids"; ValueType: none; ValueName: "LiteView.Image"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKCU; Subkey: "Software\LiteView\Capabilities\FileAssociations"; ValueType: string; ValueName: ".wdp"; ValueData: "LiteView.Image"; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Applications\LiteView.exe\SupportedTypes"; ValueType: string; ValueName: ".wdp"; ValueData: ""; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\.dds\OpenWithProgids"; ValueType: none; ValueName: "LiteView.Image"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKCU; Subkey: "Software\LiteView\Capabilities\FileAssociations"; ValueType: string; ValueName: ".dds"; ValueData: "LiteView.Image"; Tasks: assoc
Root: HKCU; Subkey: "Software\Classes\Applications\LiteView.exe\SupportedTypes"; ValueType: string; ValueName: ".dds"; ValueData: ""; Tasks: assoc

[Run]
Filename: "{app}\LiteView.exe"; Description: "{cm:LaunchApp}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; 縮圖快取在 %LOCALAPPDATA%\LiteView\thumbs；設定檔（%APPDATA%\LiteView）保留給重新安裝時使用。
Type: filesandordirs; Name: "{localappdata}\LiteView\thumbs"
