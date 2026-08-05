; AudioLens インストーラー (Inno Setup 6)
;
; 直接コンパイルせず、scripts\make_installer.ps1 から呼ぶこと。
; バージョンと入力先を /D で渡す必要があり、バージョンの出所は
; CMakeLists.txt ただ一つでなければならない(README の方針)。
;
;   ISCC.exe /DAppVersion=0.2.0 /DSourceDir=...\bin /DOutputDir=... AudioLens.iss

#ifndef AppVersion
  #error AppVersion が指定されていません。scripts\make_installer.ps1 から実行してください。
#endif
#ifndef SourceDir
  #error SourceDir が指定されていません。scripts\make_installer.ps1 から実行してください。
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

#define AppName "AudioLens"
#define AppPublisher "Noriaki Fukuyori"
#define AppExe "AudioLens.exe"

[Setup]
; 固定の AppId。これが変わると、更新ではなく二つ目の製品として並んでしまう。
AppId={{7D4E1C2A-3F58-4B96-9E0D-6C1A8F2B5E37}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
VersionInfoVersion={#AppVersion}

DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName} {#AppVersion}

OutputDir={#OutputDir}
OutputBaseFilename={#AppName}-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

; 要件 N-06。x64 のみ、Windows 10 2004 (build 19041) 以降。
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041

; 管理者権限を要求しない。既定では %LOCALAPPDATA%\Programs\AudioLens に入り、
; UAC が出ない。個人利用が前提(requirements.md §4.1)なので、Program Files に
; 入れる必然性がない。ダイアログで昇格を選ぶこともできる。
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

; 動作中の AudioLens を再起動マネージャー経由で閉じる。
;
; 強制終了ではなく「閉じてくれ」と頼むのが重要である。AudioLens は動作中、
; 仮想ケーブルをシステム既定の出力にしている。正常終了すれば出力先デバイスへ
; 戻すが(N-04)、強制終了されると既定がケーブルのまま残り、システム全体が
; 無音になる。次回起動時に修復する仕掛けはあるものの、アンインストール後には
; 「次回起動」が来ない。
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "japanese"; MessagesFile: "compiler:Languages\Japanese.isl"

[Tasks]
Name: "desktopicon"; Description: "デスクトップにショートカットを作成する"; Flags: unchecked

; 「Windows 起動時に開始」はここには置かない。AudioLens 自身が画面の
; チェックボックスで HKCU の Run キーを管理しており、インストーラーが同じ
; ものを書くと、設定が二か所にあって食い違う。

[Files]
; ビルド生成物のうち配布に不要なものを除く。.pdb と .ilk だけで 187 MB あり、
; 全体の 7 割を占める。
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion; \
    Excludes: "*.ilk,*.pdb,*.exp,*.lib,audiolens_tests.exe,audiolens_procloop.exe,vc_redist.x64.exe"

; Visual C++ ランタイム。入っていないときだけ展開して実行する。
Source: "{#SourceDir}\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall; \
    Check: NeedsVCRuntime

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{group}\{#AppName} をアンインストール"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
; shellexec で起動するため、必要なときだけ UAC が出る。インストーラー本体は
; 昇格しない。
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; \
    StatusMsg: "Visual C++ ランタイムを導入しています..."; \
    Check: NeedsVCRuntime; Flags: shellexec waituntilterminated

Filename: "{app}\{#AppExe}"; Description: "AudioLens を起動する"; \
    Flags: nowait postinstall skipifsilent

[Code]
function NeedsVCRuntime: Boolean;
var
  Installed: Cardinal;
begin
  // 64 ビット側のレジストリを見る。32 ビットのインストーラープロセスから
  // 素直に読むと WOW6432Node に飛ばされて、入っているのに入っていないと
  // 判定される。
  Result := True;
  if RegQueryDWordValue(HKEY_LOCAL_MACHINE_64,
       'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', Installed) then
    Result := (Installed = 0);
end;

function VBCableInstalled: Boolean;
begin
  // VB-Cable のドライバサービス。AudioLens はこれを「取り込み元」にして
  // システム音声を受け取るので、無ければ何も聞こえない。
  Result := RegKeyExists(HKEY_LOCAL_MACHINE,
    'SYSTEM\CurrentControlSet\Services\VBAudioVACMME');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  // 導入そのものは止めない。VB-Cable は別途入れるものであり、あとから
  // 入れても AudioLens 側は何も変える必要がない。知らせるだけにする。
  if (CurStep = ssPostInstall) and (not VBCableInstalled()) then
    MsgBox('仮想オーディオデバイス (VB-Cable) が見つかりませんでした。' + #13#10 + #13#10 +
           'AudioLens はシステムの音を仮想ケーブル経由で受け取ります。' + #13#10 +
           'VB-Cable を導入し、AudioLens の「取り込み元」に指定してください。' + #13#10 +
           'https://vb-audio.com/Cable/',
           mbInformation, MB_OK);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  DataDir: String;
begin
  if CurUninstallStep <> usPostUninstall then
    Exit;

  // 設定・プリセット・ログは既定では残す。作り直すのが面倒なものであり、
  // 再インストールしたときにそのまま使えるほうがよい。
  DataDir := ExpandConstant('{userappdata}\AudioLens');
  if not DirExists(DataDir) then
    Exit;

  if MsgBox('設定・プリセット・ログも削除しますか?' + #13#10 + #13#10 +
            DataDir + #13#10 + #13#10 +
            '「いいえ」を選ぶと残ります。再インストールすればそのまま使えます。',
            mbConfirmation, MB_YESNO) = IDYES then
    DelTree(DataDir, True, True, True);
end;
