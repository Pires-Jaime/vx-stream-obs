; VX.Stream — installateur complet « OBS Valerix »
; Copyright (C) 2026 Valerix (Jaime Pires) — GPL-2.0-or-later
;
; Installe, dans l'ordre :
;   1. OBS Studio officiel (téléchargé depuis GitHub SEULEMENT s'il est absent —
;      la plupart des streamers l'ont déjà : dans ce cas, aucune attente) ;
;   2. le plugin VX.Stream (docks natifs, menu, thème, multistream) ;
;   3. Aitum Vertical Canvas (canvas vertical 9:16 pour TikTok/YT mobile, GPL).
;
; Le payload (plugin + vertical) est préparé par la CI dans installer/payload/
; AVANT makensis — voir le job « installer » du workflow. Compilé sous Linux
; (makensis 3.x) ; le plugin INetC (téléchargement avec progression) est déposé
; par la CI dans installer/nsis-plugins/x86-unicode/.

Unicode true
!include "MUI2.nsh"
!include "LogicLib.nsh"
!addplugindir "nsis-plugins\x86-unicode"

!ifndef VERSION
  !define VERSION "0.0.0"
!endif

Name "VX.Stream pour OBS"
OutFile "VX.Stream-Installer-${VERSION}.exe"
RequestExecutionLevel admin
InstallDir "$PROGRAMFILES64\obs-studio"
BrandingText "Valerix — valerix.stream"

!define OBS_URL "https://github.com/obsproject/obs-studio/releases/download/32.0.4/OBS-Studio-32.0.4-Windows-x64-Installer.exe"

; ── Apparence ──────────────────────────────────────────────────────────────────
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TITLE "VX.Stream pour OBS"
!define MUI_WELCOMEPAGE_TEXT "Cet assistant installe vos outils Valerix dans OBS Studio :$\r$\n$\r$\n  • OBS Studio (uniquement s'il n'est pas déjà installé)$\r$\n  • le plugin VX.Stream (docks, menu, thème, multistream)$\r$\n  • le canvas vertical (TikTok / YouTube mobile)$\r$\n$\r$\nFermez OBS avant de continuer."
!define MUI_FINISHPAGE_TITLE "C'est prêt !"
!define MUI_FINISHPAGE_TEXT "Lancez OBS : le menu VX.Stream apparaît à côté de « Aide ».$\r$\n$\r$\nConnectez votre compte Valerix dans un des docks VX — une seule connexion suffit pour tous.$\r$\n$\r$\nThème Valerix : Paramètres → Apparence → « Valerix »."

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "French"

; ── Sections ───────────────────────────────────────────────────────────────────

Section "OBS Studio (si absent)" SecObs
  SectionIn RO ; obligatoire — sans OBS, rien à installer
  SetRegView 64

  ; OBS déjà là ? (chemin par défaut, puis chemin d'une installation existante)
  ${If} ${FileExists} "$INSTDIR\bin\64bit\obs64.exe"
    DetailPrint "OBS Studio déjà installé — étape sautée."
    Goto obs_done
  ${EndIf}
  ReadRegStr $0 HKLM "SOFTWARE\OBS Studio" ""
  ${If} $0 != ""
  ${AndIf} ${FileExists} "$0\bin\64bit\obs64.exe"
    StrCpy $INSTDIR $0
    DetailPrint "OBS Studio trouvé : $0 — étape sautée."
    Goto obs_done
  ${EndIf}

  DetailPrint "Téléchargement d'OBS Studio (~150 Mo)…"
  INetC::get /CAPTION "OBS Studio" /BANNER "Téléchargement d'OBS Studio…" \
    "${OBS_URL}" "$TEMP\obs-installer.exe" /END
  Pop $0
  ${If} $0 != "OK"
    MessageBox MB_ICONSTOP "Le téléchargement d'OBS a échoué ($0).$\r$\nInstallez OBS depuis obsproject.com puis relancez cet assistant."
    Abort
  ${EndIf}

  DetailPrint "Installation silencieuse d'OBS Studio…"
  ExecWait '"$TEMP\obs-installer.exe" /S' $0
  Delete "$TEMP\obs-installer.exe"
  ${IfNot} ${FileExists} "$INSTDIR\bin\64bit\obs64.exe"
    MessageBox MB_ICONSTOP "L'installation d'OBS semble avoir échoué (code $0)."
    Abort
  ${EndIf}

obs_done:
SectionEnd

Section "Plugin VX.Stream" SecVx
  SectionIn RO ; c'est l'objet même de l'installateur
  SetOutPath "$INSTDIR"
  File /r "payload\vx\"
  DetailPrint "Plugin VX.Stream installé."
SectionEnd

Section "Canvas vertical (TikTok / mobile)" SecVertical
  SetOutPath "$INSTDIR"
  File /r "payload\vertical\"
  DetailPrint "Aitum Vertical Canvas installé."
SectionEnd

Section "-post"
  WriteUninstaller "$INSTDIR\Uninstall-VXStream.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VXStream" \
    "DisplayName" "VX.Stream pour OBS"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VXStream" \
    "UninstallString" "$INSTDIR\Uninstall-VXStream.exe"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VXStream" \
    "Publisher" "Valerix"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VXStream" \
    "DisplayVersion" "${VERSION}"
SectionEnd

; ── Descriptions des composants ───────────────────────────────────────────────
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecObs} "OBS Studio officiel — téléchargé et installé uniquement s'il est absent de cette machine."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecVx} "Docks Valerix, menu VX.Stream, thème et multistream, directement dans OBS."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecVertical} "Second canvas 9:16 avec ses propres sorties, pour diffuser en vertical (Aitum Vertical, GPL)."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ── Désinstallation (retire NOS fichiers, jamais OBS) ─────────────────────────
Section "Uninstall"
  SetRegView 64
  Delete "$INSTDIR\obs-plugins\64bit\vx-stream.dll"
  Delete "$INSTDIR\obs-plugins\64bit\vx-stream.pdb"
  RMDir /r "$INSTDIR\data\obs-plugins\vx-stream"
  Delete "$INSTDIR\obs-plugins\64bit\vertical-canvas.dll"
  Delete "$INSTDIR\obs-plugins\64bit\vertical-canvas.pdb"
  RMDir /r "$INSTDIR\data\obs-plugins\vertical-canvas"
  Delete "$INSTDIR\Uninstall-VXStream.exe"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VXStream"
SectionEnd
