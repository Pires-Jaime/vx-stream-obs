; VX.Stream — installateur complet « OBS Valerix »
; Copyright (C) 2026 Valerix (Jaime Pires) — GPL-2.0-or-later
;
; Installe, dans l'ordre :
;   1. OBS Studio officiel (téléchargé depuis GitHub SEULEMENT s'il est absent —
;      la plupart des streamers l'ont déjà : dans ce cas, aucune attente) ;
;   2. le plugin VX.Stream (docks natifs, menu, thème, multistream, canvas
;      vertical VX Vertical — 100 % maison, plus aucune dépendance tierce).
;
; Le payload (plugin) est préparé par la CI dans installer/payload/
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

; ── Attente de la fermeture d'OBS ──────────────────────────────────────────────
; Windows VERROUILLE la DLL d'un plugin tant qu'OBS l'a chargée : toute copie
; par-dessus échoue. L'updater ferme OBS, mais sa fermeture prend plusieurs
; secondes (sources, encodeurs, CEF) — sans cette attente l'installation partait
; trop tôt, l'écrasement échouait et le plugin restait à l'ANCIENNE version
; (symptôme : « j'ai mis à jour mais rien n'a changé »).
;
; Sonde du verrou : sur Windows on ne peut ni supprimer ni écraser une DLL
; chargée. On tente donc de supprimer la DLL cible ; tant qu'elle résiste, OBS
; la tient encore. Aucune dépendance à un plugin NSIS tiers.
!define VX_DLL "$APPDATA\obs-studio\plugins\vx-stream\bin\64bit\vx-stream.dll"

Function WaitForObsRelease
  SetShellVarContext all
  ; Rien d'installé encore → aucun verrou possible.
  ${IfNot} ${FileExists} "${VX_DLL}"
    Return
  ${EndIf}

  StrCpy $R1 0 ; tentatives depuis la dernière relance
vx_wait_loop:
  ClearErrors
  Delete "${VX_DLL}"
  ${IfNot} ${FileExists} "${VX_DLL}"
    DetailPrint "OBS a libéré le plugin — installation possible."
    Return
  ${EndIf}

  IntOp $R1 $R1 + 1
  ${If} $R1 >= 20 ; ~10 s d'attente silencieuse
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION \
      "OBS Studio est encore ouvert : le plugin ne peut pas être remplacé.$\r$\n$\r$\n\
Fermez complètement OBS (vérifiez la zone de notification), puis cliquez Réessayer." \
      IDRETRY vx_wait_retry
    ; Annuler : mieux vaut s'arrêter que produire une « fausse » mise à jour.
    MessageBox MB_ICONSTOP "Installation annulée — le plugin n'a pas été modifié."
    Abort
  vx_wait_retry:
    StrCpy $R1 0
  ${EndIf}

  Sleep 500
  Goto vx_wait_loop
FunctionEnd

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

; Les plugins vont dans C:\ProgramData\obs-studio\plugins (layout « plugin
; folder » d'OBS 28+, celui produit par obs-plugintemplate : <nom>/bin/64bit).
; SetShellVarContext all fait pointer $APPDATA sur ProgramData.

Section "Plugin VX.Stream" SecVx
  SectionIn RO ; c'est l'objet même de l'installateur
  SetShellVarContext all
  ; OBS doit avoir relâché la DLL, sinon l'écrasement échoue en silence.
  Call WaitForObsRelease
  ; On veut une VRAIE erreur si la copie échoue, pas un « Ignorer » qui laisse
  ; l'ancienne version en place en faisant croire que tout s'est bien passé.
  SetOverwrite try
  ClearErrors
  SetOutPath "$APPDATA\obs-studio\plugins"
  File /r "payload\vx\"
  ${If} ${Errors}
    MessageBox MB_ICONSTOP \
      "Le plugin VX.Stream n'a PAS pu être remplacé (fichier verrouillé).$\r$\n\
Fermez OBS Studio complètement puis relancez cet installateur."
    Abort
  ${EndIf}
  DetailPrint "Plugin VX.Stream installé."

  ; Purge de l'ancien canvas vertical Aitum : les versions ≤ 0.14.0 le
  ; bundlaient. On le retire pour ne laisser QUE notre dock « VX Vertical »
  ; (sinon deux docks verticaux coexistent et prêtent à confusion).
  RMDir /r "$APPDATA\obs-studio\plugins\vertical-canvas"
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
  !insertmacro MUI_DESCRIPTION_TEXT ${SecVx} "Docks Valerix, menu VX.Stream, thème, multistream et canvas vertical, directement dans OBS."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ── Désinstallation (retire NOS fichiers, jamais OBS) ─────────────────────────
Section "Uninstall"
  SetRegView 64
  SetShellVarContext all
  RMDir /r "$APPDATA\obs-studio\plugins\vx-stream"
  RMDir /r "$APPDATA\obs-studio\plugins\vertical-canvas"
  Delete "$INSTDIR\Uninstall-VXStream.exe"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\VXStream"
SectionEnd
