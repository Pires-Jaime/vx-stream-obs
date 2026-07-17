# VX.Stream pour OBS

Plugin OBS Studio de [Valerix](https://valerix.stream) : ajoute un menu **VX.Stream**
dans la barre de menus d'OBS, à côté de « Aide ».

**Étape 0 (actuelle)** : validation de la chaîne de build. Le menu ouvre le lanceur
de docks et le dashboard Valerix dans le navigateur. Les étapes suivantes ajouteront
la création/bascule des docks VX directement depuis le menu (et remplaceront
l'installeur `vxstream-setup.exe`).

## Installation (Windows)

**Recommandé** : l'installateur complet `VX.Stream-Installer-*.exe` (artefact
`vxstream-installer-*`) — installe OBS s'il manque, le plugin et le canvas
vertical, au bon endroit.

**Manuelle** : récupérer l'artefact `vx-stream-*-windows-x64-*` (onglet Actions),
dézipper : il contient un dossier `vx-stream/` (layout « plugin folder » OBS 28+ :
`bin/64bit` + `data`). Copier ce dossier **entier** dans
`C:\ProgramData\obs-studio\plugins\` (PAS dans Program Files). Relancer OBS :
le menu **VX.Stream** apparaît à droite de « Aide ».

Compilé contre libobs 31.x / Qt 6 (les prebuilts officiels obs-deps) ; testé sur
OBS 32.0.4 Windows.

## Build

Basé sur [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).
GitHub Actions compile Windows (MSVC), macOS et Linux à chaque push — aucune
toolchain locale requise. Voir `buildspec.json` pour les versions épinglées.

## Licence

GPL-2.0 (héritée d'OBS et du template officiel). Voir `LICENSE`.
