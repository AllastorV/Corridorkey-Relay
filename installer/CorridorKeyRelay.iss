#define AppVersion "1.0.0"

[Setup]
AppId={{6B0E8C1A-4F2D-4C3B-9E57-2A1D5C7F3B90}
AppName=CorridorKey Relay
AppVersion={#AppVersion}
AppPublisher=AllastorV
AppPublisherURL=https://github.com/AllastorV/Corridorkey-Relay
DefaultDirName={autopf}\CorridorKey Relay
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\LICENSE
OutputDir=..\build\dist
OutputBaseFilename=CorridorKey-Relay-{#AppVersion}-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
UninstallDisplayName=CorridorKey Relay

[Tasks]
Name: runtime; Description: "Download and install CorridorKey 2.0 from its official repository (about 10 GB, needs Git and Python 3.12)"

[Files]
Source: "..\build\CorridorKey2.ofx.bundle\*"; DestDir: "{commoncf64}\OFX\Plugins\CorridorKey2.ofx.bundle"; Flags: recursesubdirs ignoreversion
Source: "..\scripts\install_runtime.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD_PARTY.md"; DestDir: "{app}"; Flags: ignoreversion

[UninstallDelete]
Type: filesandordirs; Name: "{commoncf64}\OFX\Plugins\CorridorKey2.ofx.bundle"

[Code]
function Works(const Cmd: String): Boolean;
var
  Code: Integer;
begin
  Result := Exec(ExpandConstant('{cmd}'), '/c ' + Cmd + ' >nul 2>&1', '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
  Script, Params: String;
begin
  if (CurStep <> ssPostInstall) or not WizardIsTaskSelected('runtime') then
    Exit;
  WizardForm.StatusLabel.Caption := 'Installing CorridorKey 2.0 (this can take a while)...';
  Script := ExpandConstant('{app}\install_runtime.ps1');
  Params := '-NoProfile -ExecutionPolicy Bypass -Command "try { & ''' + Script + ''' } catch { Write-Host $_ -ForegroundColor Red; ' +
            'Read-Host ''CorridorKey 2.0 install failed. Press Enter to close''; exit 1 }"';
  Log('Runtime install: powershell.exe ' + Params);
  if not ExecAsOriginalUser('powershell.exe', Params, '', SW_SHOW, ewWaitUntilTerminated, Code) then
    Code := -1;
  Log('Runtime install exit code: ' + IntToStr(Code));
  if Code <> 0 then
    MsgBox('The plugin is installed, but CorridorKey 2.0 could not be installed (exit code ' + IntToStr(Code) + ').' + #13#10#13#10 +
           'Fix the problem shown in the PowerShell window, then run:' + #13#10 + Script, mbError, MB_OK);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Missing: String;
begin
  Result := True;
  if (CurPageID = wpSelectTasks) and WizardIsTaskSelected('runtime') then
  begin
    Missing := '';
    if not Works('git --version') then
      Missing := Missing + #13#10 + '- Git for Windows: https://git-scm.com/download/win';
    if not Works('py -3.12 -c "import sys"') then
      Missing := Missing + #13#10 + '- Python 3.12 (with the py launcher): https://www.python.org/downloads/';
    if Missing <> '' then
    begin
      MsgBox('CorridorKey 2.0 needs these programs, which were not found:' + Missing + #13#10#13#10 +
             'Install them and run this setup again, or untick the CorridorKey 2.0 task to install only the plugin.',
             mbError, MB_OK);
      Result := False;
    end;
  end;
end;
