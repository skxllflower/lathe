; Lathe's Windows installer (the native GUI). Built by tools/build-release-native.ps1, which stages the files and passes
; the defines below; by hand:
;
;   makensis /DVERSION=0.1.8 /DSTAGE=<staged dir> /DOUTFILE=<setup.exe> /DICON=<icon.ico> native/installer/lathe.nsi
;
; Per machine, into Program Files\Vacant Systems\Lathe, under the uninstall key "Lathe", the exe lathe-gui.exe: the
; Tauri app's place, key and name, so it installs over that app in place (the same shortcuts, Explorer verbs, and the
; paths WAVdesk and its installer probe). /S installs silently (WAVdesk's installer runs it so for its companion offer).
; The staged dir:
;
;   lathe-gui.exe            the app (the native GUI)
;   coredist\lathe.exe       the CLI that does every conversion, beside its LGPL libav DLLs and THIRD_PARTY_NOTICES.txt
;                            (the Tauri layout: the app finds it there, WAVdesk through Shared\registry.json)

Unicode true
ManifestDPIAware true
RequestExecutionLevel admin
SetCompressor /SOLID lzma

!ifndef VERSION
  !error "pass /DVERSION=<x.y.z>"
!endif
!ifndef STAGE
  !error "pass /DSTAGE=<staged dir>"
!endif
!ifndef OUTFILE
  !define OUTFILE "Lathe_${VERSION}_x64-setup.exe"
!endif

!define PRODUCT "Lathe"
!define PUBLISHER "Vacant Systems"
!define MAINEXE "lathe-gui.exe"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT}"
!define DIR_KEY "Software\vacantsystems\${PRODUCT}"
!define STARTMENU "$SMPROGRAMS\Vacant Systems"

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"
!include "x64.nsh"
!include "nsDialogs.nsh"

Name "${PRODUCT}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\Vacant Systems\${PRODUCT}"
InstallDirRegKey HKLM "${DIR_KEY}" ""
BrandingText "${PUBLISHER}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${PRODUCT}"
VIAddVersionKey "CompanyName" "${PUBLISHER}"
VIAddVersionKey "FileDescription" "${PRODUCT} Setup"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "${PUBLISHER}"

!ifdef ICON
  !define MUI_ICON "${ICON}"
  !define MUI_UNICON "${ICON}"
!endif
!define MUI_ABORTWARNING

!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Start ${PRODUCT}"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchUnelevated

!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
UninstPage custom un.DataPage un.DataPageLeave
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; SAFE_RMDIR_R <path> <suffix>: a recursive delete only when the path is non-empty and ends with <suffix> (an empty
; variable before \* is the classic NSIS disk wipe)
!macro SAFE_RMDIR_R path suffix
  Push $R7
  Push $R8
  StrCpy $R8 "${path}"
  StrLen $R7 "${suffix}"
  IntOp $R7 0 - $R7
  StrCpy $R7 "$R8" "" $R7
  ${If} $R8 != ""
  ${AndIf} $R7 == "${suffix}"
    RMDir /r "$R8"
  ${EndIf}
  Pop $R8
  Pop $R7
!macroend

; WAIT_UNLOCKED <file>: a running lathe-gui.exe (this app, or the Tauri one) or lathe.exe (a conversion, WAVdesk's
; decode-server) holds its image open; ask the user to quit rather than kill it mid-convert. Silent: abort, unchanged.
!macro WAIT_UNLOCKED file what
  ${If} ${FileExists} "${file}"
    ${Do}
      ClearErrors
      FileOpen $R9 "${file}" a
      ${IfNot} ${Errors}
        FileClose $R9
        ${Break}
      ${EndIf}
      MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "${what} is running. Quit it, then click Retry." /SD IDCANCEL IDRETRY +2
      Abort "${what} is running."
    ${Loop}
  ${EndIf}
!macroend

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "${PRODUCT} needs 64-bit Windows." /SD IDOK
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext all
FunctionEnd

; the finish page's "Start Lathe": through Explorer, so the app runs as the user, not elevated like this installer
Function LaunchUnelevated
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\${MAINEXE}"'
FunctionEnd

; one "Convert with Lathe" sub-verb: <key> orders the submenu (a/b/c + index), <fmt> is the --format value
!macro CONVERT_VERB key label fmt
  WriteRegStr HKLM "Software\Classes\Lathe.Convert\shell\${key}" "MUIVerb" "${label}"
  WriteRegStr HKLM "Software\Classes\Lathe.Convert\shell\${key}\command" "" '"$INSTDIR\${MAINEXE}" --convert "%1" --format ${fmt}'
!macroend

Section "Install"
  SetRegView 64
  SetShellVarContext all

  !insertmacro WAIT_UNLOCKED "$INSTDIR\${MAINEXE}" "Lathe"
  !insertmacro WAIT_UNLOCKED "$INSTDIR\coredist\lathe.exe" "A Lathe conversion (Lathe, or WAVdesk using it)"

  ; a clean coredist\ each install (no DLL left from an older libav), and the Tauri app's WebView2 profile when this
  ; installs over it
  !insertmacro SAFE_RMDIR_R "$INSTDIR\coredist" "\${PRODUCT}\coredist"
  ReadEnvStr $0 "LOCALAPPDATA"
  ${If} $0 != ""
    !insertmacro SAFE_RMDIR_R "$0\com.vacantsystems.lathe" "\com.vacantsystems.lathe"
  ${EndIf}

  SetOutPath "$INSTDIR"
  File /r "${STAGE}\*.*"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  ; the suite's shared folder, writable by the unelevated apps (the ffmpeg fetch, registry.json): BUILTIN\Users by SID
  ; (locale-independent), inherited, Modify
  ReadEnvStr $0 "ProgramData"
  ${If} $0 != ""
    CreateDirectory "$0\Vacant Systems\Shared"
    nsExec::ExecToLog 'icacls "$0\Vacant Systems\Shared" /grant *S-1-5-32-545:(OI)(CI)M /T'
  ${EndIf}

  ; Explorer's "Convert with Lathe" cascade on any file (the app's single instance routes --convert / --format into
  ; the running window)
  WriteRegStr HKLM "Software\Classes\*\shell\LatheConvert" "MUIVerb" "Convert with Lathe"
  WriteRegStr HKLM "Software\Classes\*\shell\LatheConvert" "Icon" "$INSTDIR\${MAINEXE}"
  WriteRegStr HKLM "Software\Classes\*\shell\LatheConvert" "ExtendedSubCommandsKey" "Lathe.Convert"
  !insertmacro CONVERT_VERB a1wav WAV wav
  !insertmacro CONVERT_VERB a2mp3 MP3 mp3
  !insertmacro CONVERT_VERB a3flac FLAC flac
  !insertmacro CONVERT_VERB a4aac AAC aac
  !insertmacro CONVERT_VERB a5m4a M4A m4a
  !insertmacro CONVERT_VERB b1mp4 MP4 mp4
  !insertmacro CONVERT_VERB b2mkv MKV mkv
  !insertmacro CONVERT_VERB b3mov MOV mov
  !insertmacro CONVERT_VERB b4webm WEBM webm
  !insertmacro CONVERT_VERB c1png PNG png
  !insertmacro CONVERT_VERB c2jpg JPG jpg
  !insertmacro CONVERT_VERB c3webp WEBP webp
  !insertmacro CONVERT_VERB c4gif GIF gif

  ; shortcuts made anew, so Explorer re-reads the icon (a kept .lnk keeps a stale cached one)
  CreateDirectory "${STARTMENU}"
  Delete "${STARTMENU}\${PRODUCT}.lnk"
  CreateShortcut "${STARTMENU}\${PRODUCT}.lnk" "$INSTDIR\${MAINEXE}" "" "$INSTDIR\${MAINEXE}" 0
  Delete "$DESKTOP\${PRODUCT}.lnk"
  CreateShortcut "$DESKTOP\${PRODUCT}.lnk" "$INSTDIR\${MAINEXE}" "" "$INSTDIR\${MAINEXE}" 0

  WriteRegStr HKLM "${DIR_KEY}" "" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "${PRODUCT}"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" '"$INSTDIR\${MAINEXE}"'
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "${PUBLISHER}"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" '"$INSTDIR"'
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegStr HKLM "${UNINST_KEY}" "MainBinaryName" "${MAINEXE}"
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" "$0"

  nsExec::ExecToLog 'ie4uinit.exe -show'
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, i 0, i 0)'
SectionEnd

; ---- uninstall ----

Var DeleteData
Var DeleteDataBox

Function un.onInit
  SetRegView 64
  SetShellVarContext all
  StrCpy $DeleteData 0
FunctionEnd

Function un.DataPage
  !insertmacro MUI_HEADER_TEXT "Your settings" "Lathe keeps its settings and presets in your user folder."
  nsDialogs::Create 1018
  Pop $0
  ${NSD_CreateLabel} 0 0 100% 36u "Your media files and converted outputs are never touched. Leave the box clear to keep Lathe's settings and presets for a later install."
  Pop $0
  ${NSD_CreateCheckbox} 0 44u 100% 12u "Also delete Lathe's settings and presets"
  Pop $DeleteDataBox
  nsDialogs::Show
FunctionEnd

Function un.DataPageLeave
  ${NSD_GetState} $DeleteDataBox $DeleteData
FunctionEnd

Section "Uninstall"
  !insertmacro WAIT_UNLOCKED "$INSTDIR\${MAINEXE}" "Lathe"
  !insertmacro WAIT_UNLOCKED "$INSTDIR\coredist\lathe.exe" "A Lathe conversion (Lathe, or WAVdesk using it)"

  DeleteRegKey HKLM "Software\Classes\*\shell\LatheConvert"
  DeleteRegKey HKLM "Software\Classes\Lathe.Convert"

  Delete "${STARTMENU}\${PRODUCT}.lnk"
  RMDir "${STARTMENU}"
  Delete "$DESKTOP\${PRODUCT}.lnk"

  !insertmacro SAFE_RMDIR_R "$INSTDIR" "\${PRODUCT}"
  DeleteRegKey HKLM "${UNINST_KEY}"
  DeleteRegKey HKLM "${DIR_KEY}"
  DeleteRegKey /ifempty HKLM "Software\vacantsystems"

  ; the last Vacant Systems app out takes the suite's Shared folders with it
  StrCpy $2 "1"
  ${If} ${FileExists} "$PROGRAMFILES64\Vacant Systems\WAVdesk\wavdesk.exe"
    StrCpy $2 "0"
  ${EndIf}
  ${If} ${FileExists} "$PROGRAMFILES64\Vacant Systems\Latch\latch-gui.exe"
    StrCpy $2 "0"
  ${EndIf}

  ReadEnvStr $0 "ProgramData"
  ${If} $0 != ""
    !insertmacro SAFE_RMDIR_R "$0\Vacant Systems\Lathe" "\Vacant Systems\Lathe"
    ${If} $2 == "1"
      !insertmacro SAFE_RMDIR_R "$0\Vacant Systems\Shared" "\Vacant Systems\Shared"
    ${EndIf}
    RMDir "$0\Vacant Systems"
  ${EndIf}

  ; the user's data only when asked (the shared cookies are logins: last one out and asked)
  ReadEnvStr $1 "LOCALAPPDATA"
  ${If} $1 != ""
  ${AndIf} $DeleteData == ${BST_CHECKED}
    !insertmacro SAFE_RMDIR_R "$1\Vacant Systems\Lathe" "\Vacant Systems\Lathe"
    ${If} $2 == "1"
      !insertmacro SAFE_RMDIR_R "$1\Vacant Systems\Shared" "\Vacant Systems\Shared"
    ${EndIf}
    RMDir "$1\Vacant Systems"
  ${EndIf}

  RMDir "$PROGRAMFILES64\Vacant Systems"
  System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, i 0, i 0)'
SectionEnd
