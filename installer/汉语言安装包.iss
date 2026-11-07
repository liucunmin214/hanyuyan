; 汉语言安装包（Inno Setup 7）
; 用 ISCC.exe 编译：dist\汉语言安装包.exe

#define MyAppName "汉语言"
#define MyAppVersion "1.0"
#define MyAppExeName "hanedit.exe"

[Setup]
AppId={{8F1A5C2E-4B7D-4E93-9A61-2C8D5E7B3F10}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppName}
DefaultDirName={autopf}\汉语言
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=汉语言安装包
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\icon\han.ico
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}

[Languages]
Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加选项:"; Flags: unchecked
Name: "addtopath"; Description: "把安装目录加入 PATH（命令行可直接使用 han.exe）"; GroupDescription: "附加选项:"; Flags: checkedonce
Name: "associate"; Description: "把 .汉 文件关联到汉语言编辑器"; GroupDescription: "附加选项:"; Flags: checkedonce

[Files]
Source: "..\build\han.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\build\hanedit.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\doc\*"; DestDir: "{app}\doc"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\examples\*"; DestDir: "{app}\examples"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\汉语言编辑器"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\汉语言教程"; Filename: "{app}\doc\教程\教程.html"
Name: "{group}\卸载汉语言"; Filename: "{uninstallexe}"
Name: "{autodesktop}\汉语言编辑器"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; 记录安装目录，编辑器找不到同目录 han.exe 时也能兜底
Root: HKCU; Subkey: "Software\汉语言"; ValueType: string; ValueName: "InstallDir"; \
    ValueData: "{app}"; Flags: uninsdeletekey
; .汉 文件关联
Root: HKCU; Subkey: "Software\Classes\.汉"; ValueType: string; ValueData: "HanLang.Source"; \
    Tasks: associate; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\HanLang.Source"; ValueType: string; ValueData: "汉语言源文件"; \
    Tasks: associate; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\HanLang.Source\DefaultIcon"; ValueType: string; \
    ValueData: "{app}\{#MyAppExeName},0"; Tasks: associate; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\HanLang.Source\shell\open\command"; ValueType: string; \
    ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: associate; Flags: uninsdeletekey

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "启动汉语言编辑器"; Flags: nowait postinstall skipifsilent

[Code]
{ 把安装目录追加到用户 PATH（不覆盖原有内容，重复安装不重复添加） }
procedure AddToUserPath();
var
  PathVal: string;
  AppDir: string;
begin
  if not WizardIsTaskSelected('addtopath') then
    Exit;
  AppDir := ExpandConstant('{app}');
  if RegQueryStringValue(HKCU, 'Environment', 'Path', PathVal) then
  begin
    if Pos(Lowercase(AppDir), Lowercase(PathVal)) = 0 then
      RegWriteExpandStringValue(HKCU, 'Environment', 'Path', PathVal + ';' + AppDir);
  end
  else
    RegWriteExpandStringValue(HKCU, 'Environment', 'Path', AppDir);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    AddToUserPath();
end;

{ 卸载时把安装目录从用户 PATH 中移除，避免残留无效路径 }
procedure RemoveFromUserPath();
var
  PathVal, AppDir: string;
begin
  AppDir := ExpandConstant('{app}');
  if not RegQueryStringValue(HKCU, 'Environment', 'Path', PathVal) then
    Exit;
  if PathVal = AppDir then
  begin
    RegDeleteValue(HKCU, 'Environment', 'Path');
    Exit;
  end;
  StringChangeEx(PathVal, ';' + AppDir, '', False);
  StringChangeEx(PathVal, AppDir + ';', '', False);
  if PathVal = '' then
    RegDeleteValue(HKCU, 'Environment', 'Path')
  else
    RegWriteExpandStringValue(HKCU, 'Environment', 'Path', PathVal);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveFromUserPath();
end;
