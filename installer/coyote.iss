; Coyote setup (Inno Setup 6.3+). Build: ISCC /DAppVersion=0.2.0.17 /DStageDir=<stage> coyote.iss
; The version passed in is MAJOR.MINOR.PATCH.BUILD; the build part is the CI run number, so every
; delivery has a different version and an older installation is detected as "needs update".
#ifndef AppVersion
  #define AppVersion "0.0.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\stage"
#endif
#ifndef OutDir
  #define OutDir "..\out"
#endif
#define AppGuid "6F3B2C9E-7A41-4D5B-9E0A-3C2D8B1F7A64"

[Setup]
AppId={{{#AppGuid}}
AppName=Coyote
AppVersion={#AppVersion}
AppVerName=Coyote {#AppVersion}
AppPublisher=Coyote
VersionInfoVersion={#AppVersion}
VersionInfoProductName=Coyote
VersionInfoDescription=Coyote setup
#ifdef Test32
DefaultDirName={userappdata}\CoyoteTest
#else
DefaultDirName={autopf}\Coyote
#endif
DefaultGroupName=Coyote
DisableProgramGroupPage=yes
DisableDirPage=auto
DisableReadyPage=no
UsePreviousAppDir=yes
UsePreviousSetupType=yes
UsePreviousTasks=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
#ifndef Test32
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#endif
MinVersion=10.0
OutputDir={#OutDir}
OutputBaseFilename=Coyote-Setup-{#AppVersion}
SetupIconFile={#StageDir}\coyote.ico
UninstallDisplayIcon={app}\coyote.ico
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
ChangesEnvironment=yes
ChangesAssociations=yes
#ifdef Test32
CloseApplications=no
#else
CloseApplications=yes
#endif
RestartApplications=no
SetupLogging=yes

[Types]
Name: "full"; Description: "Full installation: Coyote and Coyote Studio"
Name: "cli"; Description: "Coyote only (command-line toolchain, no Studio)"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "core"; Description: "Coyote toolchain (coyote, pvm, cpm, cpx, cdt, coyote-llvm), documentation and samples"; Types: full cli custom; Flags: fixed
Name: "studio"; Description: "Coyote Studio (editor, run and debug environment)"; Types: full custom
Name: "vscode"; Description: "VS Code extension package (.vsix, installed by you from VS Code)"; Types: custom

[Tasks]
Name: "addtopath"; Description: "Add Coyote to the PATH (use coyote, pvm, cpm, cpx and cdt from any terminal)"
Name: "desktopicon"; Description: "Create a desktop shortcut for Coyote Studio"; Components: studio; Flags: unchecked
Name: "assoc"; Description: "Open .coy files with Coyote Studio"; Components: studio

[InstallDelete]
; An update replaces the old files of a component completely (nothing stale is left behind).
Type: filesandordirs; Name: "{app}\bin"; Components: core
Type: filesandordirs; Name: "{app}\libs"; Components: core
Type: filesandordirs; Name: "{app}\docs"; Components: core
Type: filesandordirs; Name: "{app}\studio"; Components: studio

[Files]
Source: "{#StageDir}\bin\*"; DestDir: "{app}\bin"; Components: core; Flags: ignoreversion
Source: "{#StageDir}\libs\*"; DestDir: "{app}\libs"; Components: core; Flags: ignoreversion recursesubdirs
Source: "{#StageDir}\docs\*"; DestDir: "{app}\docs"; Components: core; Flags: ignoreversion
Source: "{#StageDir}\samples\*"; DestDir: "{app}\samples"; Components: core; Flags: ignoreversion
Source: "{#StageDir}\assets\brand\*"; DestDir: "{app}\assets\brand"; Components: core; Flags: ignoreversion
Source: "{#StageDir}\coyote.ico"; DestDir: "{app}"; Components: core; Flags: ignoreversion
Source: "{#StageDir}\VERSION"; DestDir: "{app}"; Components: core; Flags: ignoreversion
Source: "{#StageDir}\studio\*"; DestDir: "{app}\studio"; Components: studio; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\extras\*"; DestDir: "{app}\extras"; Components: vscode; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Coyote\Coyote Studio"; Filename: "{app}\studio\CoyoteStudio.exe"; Components: studio
Name: "{autoprograms}\Coyote\Coyote Command Prompt"; Filename: "{cmd}"; Parameters: "/K ""set ""PATH={app}\bin;%PATH%"" && coyote --version"""; WorkingDir: "{userdocs}"; IconFilename: "{app}\coyote.ico"
Name: "{autoprograms}\Coyote\Coyote Documentation"; Filename: "{app}\docs"; IconFilename: "{app}\coyote.ico"
Name: "{autoprograms}\Coyote\Uninstall Coyote"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Coyote Studio"; Filename: "{app}\studio\CoyoteStudio.exe"; Tasks: desktopicon; Components: studio

[Registry]
; .coy files open in Coyote Studio.
Root: HKA; Subkey: "Software\Classes\.coy"; ValueType: string; ValueName: ""; ValueData: "Coyote.Source"; Flags: uninsdeletevalue; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\Coyote.Source"; ValueType: string; ValueName: ""; ValueData: "Coyote source file"; Flags: uninsdeletekey; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\Coyote.Source\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\coyote.ico"; Tasks: assoc
Root: HKA; Subkey: "Software\Classes\Coyote.Source\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\studio\CoyoteStudio.exe"" ""%1"""; Tasks: assoc
; PATH (per-user installs edit the user PATH, all-users installs the system PATH).
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{code:PathWith|HKCU}"; Check: NeedsAddPath(False); Tasks: addtopath; Flags: preservestringtype
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{code:PathWith|HKLM}"; Check: NeedsAddPath(True); Tasks: addtopath; Flags: preservestringtype

[Run]
Filename: "{app}\studio\CoyoteStudio.exe"; Description: "Start Coyote Studio"; Flags: nowait postinstall skipifsilent; Components: studio

[Code]
const
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{' + '{#AppGuid}' + '}_is1';

var
  OldVersion: String;
  UpdateMode: Boolean;

// ---- version handling ----

function NextPart(var S: String): Integer;
var
  P: Integer;
  Part: String;
begin
  P := Pos('.', S);
  if P = 0 then begin
    Part := S;
    S := '';
  end else begin
    Part := Copy(S, 1, P - 1);
    Delete(S, 1, P);
  end;
  Result := StrToIntDef(Part, 0);
end;

// -1: A < B, 0: equal, 1: A > B (numeric, part by part)
function CompareVersions(A, B: String): Integer;
var
  I, X, Y: Integer;
begin
  Result := 0;
  for I := 1 to 4 do begin
    X := NextPart(A);
    Y := NextPart(B);
    if X < Y then begin Result := -1; Exit; end;
    if X > Y then begin Result := 1; Exit; end;
  end;
end;

function InstalledVersion(): String;
begin
  if not RegQueryStringValue(HKLM, UninstallKey, 'DisplayVersion', Result) then
    if not RegQueryStringValue(HKCU, UninstallKey, 'DisplayVersion', Result) then
      Result := '';
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  UpdateMode := False;
  OldVersion := InstalledVersion();
  if OldVersion = '' then Exit;
  case CompareVersions(OldVersion, '{#AppVersion}') of
    -1: UpdateMode := True;
    1: begin
         SuppressibleMsgBox('A newer version of Coyote (' + OldVersion + ') is already installed, and this setup is version {#AppVersion}.' + #13#10#13#10 +
           'Downgrading is not supported. To go back to an older version, uninstall Coyote first.', mbInformation, MB_OK, IDOK);
         Result := False;
       end;
  end;
end;

procedure InitializeWizard();
begin
  if UpdateMode then begin
    WizardForm.Caption := 'Coyote Update';
    WizardForm.WelcomeLabel1.Caption := 'Update Coyote';
    WizardForm.WelcomeLabel2.Caption := 'Coyote ' + OldVersion + ' is installed on this computer.' + #13#10#13#10 +
      'This will update it to version {#AppVersion}, keeping your current location and choices. Your projects and settings are not touched.' + #13#10#13#10 +
      'Click Next to continue.';
  end else if OldVersion = '{#AppVersion}' then begin
    WizardForm.WelcomeLabel1.Caption := 'Repair Coyote';
    WizardForm.WelcomeLabel2.Caption := 'Coyote {#AppVersion} is already installed. This will reinstall it (you can also add or remove components).' + #13#10#13#10 +
      'Click Next to continue.';
  end;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if UpdateMode then
    Result := (PageID = wpSelectDir) or (PageID = wpSelectComponents) or (PageID = wpSelectTasks) or (PageID = wpSelectProgramGroup);
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if UpdateMode then begin
    if CurPageID = wpReady then begin
      WizardForm.NextButton.Caption := 'Update';
      WizardForm.ReadyLabel.Caption := 'Setup is ready to update Coyote from ' + OldVersion + ' to {#AppVersion}.';
    end;
    if CurPageID = wpFinished then
      WizardForm.FinishedHeadingLabel.Caption := 'Coyote has been updated';
  end;
end;

// ---- PATH ----

function NeedsAddPath(Machine: Boolean): Boolean;
var
  Paths, Entry: String;
  Root: Integer;
  Key: String;
begin
  // Only the PATH of the kind of install being done is edited.
  if Machine <> IsAdminInstallMode then begin
    Result := False;
    Exit;
  end;
  if Machine then begin
    Root := HKLM;
    Key := 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment';
  end else begin
    Root := HKCU;
    Key := 'Environment';
  end;
  if not RegQueryStringValue(Root, Key, 'Path', Paths) then Paths := '';
  Entry := ExpandConstant('{app}\bin');
  Result := Pos(';' + Uppercase(Entry) + ';', ';' + Uppercase(Paths) + ';') = 0;
end;

// The current PATH of the given hive with our bin folder appended (no stray ';' when the PATH was empty).
function PathWith(Hive: String): String;
var
  Paths: String;
begin
  if Hive = 'HKLM' then begin
    if not RegQueryStringValue(HKLM, 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment', 'Path', Paths) then Paths := '';
  end else begin
    if not RegQueryStringValue(HKCU, 'Environment', 'Path', Paths) then Paths := '';
  end;
  if (Paths <> '') and (Paths[Length(Paths)] <> ';') then Paths := Paths + ';';
  Result := Paths + ExpandConstant('{app}\bin');
end;

procedure RemoveFromPath(Root: Integer; Key: String);
var
  Paths, Entry, Upper: String;
  P: Integer;
begin
  if not RegQueryStringValue(Root, Key, 'Path', Paths) then Exit;
  Entry := ExpandConstant('{app}\bin');
  Upper := ';' + Uppercase(Paths) + ';';
  P := Pos(';' + Uppercase(Entry) + ';', Upper);
  if P = 0 then Exit;
  Paths := ';' + Paths + ';';
  Delete(Paths, P, Length(Entry) + 1);
  if (Length(Paths) > 0) and (Paths[1] = ';') then Delete(Paths, 1, 1);
  if (Length(Paths) > 0) and (Paths[Length(Paths)] = ';') then Delete(Paths, Length(Paths), 1);
  if Paths = '' then RegDeleteValue(Root, Key, 'Path')
  else RegWriteExpandStringValue(Root, Key, 'Path', Paths);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then begin
    RemoveFromPath(HKCU, 'Environment');
    if IsAdminInstallMode then
      RemoveFromPath(HKLM, 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment');
  end;
end;
