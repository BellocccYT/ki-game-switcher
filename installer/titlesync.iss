; Windows-Installer für TitleSync (Inno Setup 6)
;
; Bauen (nach "cmake --install build_x64 --prefix release/RelWithDebInfo"):
;   ISCC.exe /DAppVersion=1.5.0 installer\titlesync.iss
; Ergebnis: release\titlesync-<Version>-windows-x64-setup.exe

#ifndef AppVersion
  #define AppVersion "1.5.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\release\RelWithDebInfo\titlesync"
#endif
#ifndef OutputDir
  #define OutputDir "..\release"
#endif

#define AppName "TitleSync"
#define AppUrl "https://github.com/BellocccYT/titlesync"
#define OllamaUrl "https://ollama.com/download/OllamaSetup.exe"
#define OllamaModel "qwen3-vl:4b-instruct"

[Setup]
AppId={{6E0B8F43-2C1D-4C5A-9F7E-8A1B3D5C7E90}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Belloccc
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
UninstallDisplayName={#AppName} (OBS-Plugin, früher KI Game Switcher)
#ifdef TestDir
; Testbau: ohne Adminrechte in einen beliebigen Ordner
DefaultDirName={#TestDir}
PrivilegesRequired=lowest
#else
; OBS lädt Plugins für alle Benutzer aus diesem Ordner
DefaultDirName={commonappdata}\obs-studio\plugins\titlesync
PrivilegesRequired=admin
#endif
DisableDirPage=yes
; Gleiche AppId wie „KI Game Switcher“ (Update), aber neuer Ordner – nicht den alten weiterverwenden
UsePreviousAppDir=no
DisableProgramGroupPage=yes
; Ollama und die Plugin-Einstellungen gehören dem angemeldeten Benutzer
UsedUserAreasWarning=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir={#OutputDir}
OutputBaseFilename=titlesync-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Languages]
Name: "de"; MessagesFile: "compiler:Languages\German.isl"

[Messages]
FinishedHeadingLabel=Fertig!
FinishedLabelNoIcons=TitleSync ist installiert.%n%nStarte OBS: Das Dock „TitleSync“ erscheint automatisch. Deine Einstellungen vom „KI Game Switcher“ werden übernommen. Unter „Einstellungen …“ verbindest du dort dein Twitch-Konto.
FinishedLabel=TitleSync ist installiert.%n%nStarte OBS: Das Dock „TitleSync“ erscheint automatisch. Deine Einstellungen vom „KI Game Switcher“ werden übernommen. Unter „Einstellungen …“ verbindest du dort dein Twitch-Konto.

[Tasks]
Name: "ollama"; Description: "Ollama installieren und Bildmodell laden – kostenlose KI auf deinem PC, ohne API-Key (Download ca. 4,9 GB, braucht ca. 4 GB Grafikspeicher)"; Check: not OllamaInstalled
Name: "ollamamodel"; Description: "Bildmodell {#OllamaModel} für Ollama laden (Download 3,3 GB, falls noch nicht vorhanden)"; Check: OllamaInstalled

[InstallDelete]
; Version unter altem Namen entfernen, sonst lädt OBS das Plugin doppelt
Type: filesandordirs; Name: "{commonappdata}\obs-studio\plugins\ki-game-switcher"
; Reste einer älteren Version (z. B. aus der ZIP-Datei) entfernen
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\data"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "*.pdb"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
Type: dirifempty; Name: "{app}"

[Run]
Filename: "{code:ObsExe}"; WorkingDir: "{code:ObsBinDir}"; Description: "OBS Studio jetzt starten"; Flags: postinstall nowait skipifsilent runasoriginaluser; Check: ObsFound

[Code]
var
  DownloadPage: TDownloadWizardPage;

function ObsInstallDir: String;
begin
  if not RegQueryStringValue(HKLM, 'SOFTWARE\OBS Studio', '', Result) then
    Result := '';
end;

function ObsBinDir(Param: String): String;
begin
  Result := AddBackslash(ObsInstallDir) + 'bin\64bit';
end;

function ObsExe(Param: String): String;
begin
  Result := ObsBinDir('') + '\obs64.exe';
end;

function ObsFound: Boolean;
begin
  Result := (ObsInstallDir <> '') and FileExists(ObsExe(''));
end;

function OllamaExe: String;
begin
  Result := ExpandConstant('{localappdata}\Programs\Ollama\ollama.exe');
end;

function OllamaInstalled: Boolean;
begin
  Result := FileExists(OllamaExe);
end;

function IsProcessRunning(const FileName: String): Boolean;
var
  Locator, Service, Processes: Variant;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2');
    Processes := Service.ExecQuery(Format('SELECT Name FROM Win32_Process WHERE Name = "%s"', [FileName]));
    Result := Processes.Count > 0;
  except
  end;
end;

function WaitForObsClosed: Boolean;
begin
  Result := True;
  while IsProcessRunning('obs64.exe') do begin
    if SuppressibleMsgBox('OBS Studio läuft gerade. Bitte schließe OBS und klicke dann auf „Wiederholen“.',
      mbError, MB_RETRYCANCEL, IDCANCEL) = IDCANCEL then begin
      Result := False;
      Exit;
    end;
  end;
end;

function InitializeSetup: Boolean;
begin
  Result := WaitForObsClosed;
  if Result and not ObsFound then
    Result := SuppressibleMsgBox('OBS Studio wurde auf diesem PC nicht gefunden. Das Plugin braucht OBS Studio 31 oder neuer.' + #13#10#13#10 +
      'Trotzdem installieren?', mbConfirmation, MB_YESNO, IDYES) = IDYES;
end;

function InitializeUninstall: Boolean;
begin
  Result := WaitForObsClosed;
end;

procedure InitializeWizard;
begin
  DownloadPage := CreateDownloadPage('Ollama wird heruntergeladen', 'Das dauert je nach Internetverbindung ein paar Minuten.', nil);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpReady) and WizardIsTaskSelected('ollama') then begin
    DownloadPage.Clear;
    DownloadPage.Add('{#OllamaUrl}', 'OllamaSetup.exe', '');
    DownloadPage.Show;
    try
      try
        DownloadPage.Download;
      except
        if DownloadPage.AbortedByUser then
          Result := False
        else
          Result := SuppressibleMsgBox('Ollama konnte nicht heruntergeladen werden:' + #13#10 + GetExceptionMessage + #13#10#13#10 +
            'Das Plugin trotzdem ohne Ollama installieren?', mbError, MB_YESNO, IDYES) = IDYES;
      end;
    finally
      DownloadPage.Hide;
    end;
  end;
end;

procedure SetStatus(const Text: String);
begin
  WizardForm.StatusLabel.Caption := Text;
  WizardForm.FilenameLabel.Caption := '';
  WizardForm.Refresh;
end;

function OllamaReady: Boolean;
var
  ResultCode: Integer;
begin
  Result := ExecAsOriginalUser(OllamaExe, 'list', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
  Log(Format('ollama list: Ergebnis %d', [ResultCode]));
end;

function StartOllama: Boolean;
var
  ResultCode, I: Integer;
begin
  Result := OllamaReady;
  if Result then
    Exit;
  ExecAsOriginalUser(ExtractFilePath(OllamaExe) + 'ollama app.exe', '', '', SW_HIDE, ewNoWait, ResultCode);
  for I := 1 to 30 do begin
    Sleep(1000);
    if OllamaReady then begin
      Result := True;
      Exit;
    end;
  end;
end;

procedure UseOllamaInPlugin;
var
  ConfigDir: String;
begin
  // Nur bei der ersten Einrichtung: bestehende Einstellungen bleiben unangetastet
  ConfigDir := ExpandConstant('{userappdata}\obs-studio\plugin_config\titlesync');
  // Alte Einstellungen übernimmt das Plugin beim Start selbst
  if FileExists(ConfigDir + '\settings.json') or
     FileExists(ExpandConstant('{userappdata}\obs-studio\plugin_config\ki-game-switcher\settings.json')) then
    Exit;
  ForceDirectories(ConfigDir);
  Log('Plugin auf Ollama eingestellt: ' + ConfigDir);
  SaveStringToFile(ConfigDir + '\settings.json', '{"provider": "ollama"}', False);
end;

procedure SetUpOllama;
var
  ResultCode: Integer;
  Setup: String;
begin
  Setup := ExpandConstant('{tmp}\OllamaSetup.exe');
  Log('Ollama einrichten: ' + OllamaExe);
  if WizardIsTaskSelected('ollama') and FileExists(Setup) then begin
    SetStatus('Ollama wird installiert …');
    if not ExecAsOriginalUser(Setup, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-', '', SW_SHOW, ewWaitUntilTerminated, ResultCode)
      or (ResultCode <> 0) or not OllamaInstalled then begin
      SuppressibleMsgBox('Ollama konnte nicht installiert werden (Code ' + IntToStr(ResultCode) + ').' + #13#10 +
        'Du kannst es später selbst von ollama.com installieren.', mbError, MB_OK, IDOK);
      Exit;
    end;
  end;
  if not OllamaInstalled then
    Exit;

  SetStatus('Ollama wird gestartet …');
  if not StartOllama then begin
    SuppressibleMsgBox('Ollama ist installiert, startet aber nicht. Starte Ollama selbst und lade dann das Bildmodell mit:' + #13#10#13#10 +
      'ollama pull {#OllamaModel}', mbError, MB_OK, IDOK);
    UseOllamaInPlugin;
    Exit;
  end;

  SetStatus('Bildmodell {#OllamaModel} wird geladen (3,3 GB) – Fortschritt im Konsolenfenster …');
  if not ExecAsOriginalUser(OllamaExe, 'pull {#OllamaModel}', '', SW_SHOW, ewWaitUntilTerminated, ResultCode) then
    ResultCode := -1;
  Log(Format('ollama pull: Ergebnis %d', [ResultCode]));
  if ResultCode <> 0 then
    SuppressibleMsgBox('Das Bildmodell konnte nicht geladen werden. Hole das später in einer Eingabeaufforderung nach mit:' + #13#10#13#10 +
      'ollama pull {#OllamaModel}', mbError, MB_OK, IDOK);
  UseOllamaInPlugin;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and (WizardIsTaskSelected('ollama') or WizardIsTaskSelected('ollamamodel')) then
    SetUpOllama;
end;
