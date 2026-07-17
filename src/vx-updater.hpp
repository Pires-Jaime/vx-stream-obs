/*
VX.Stream pour OBS — vérification de mise à jour
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

class QMenu;

/**
 * Lance la vérification en arrière-plan (thread dédié, jamais bloquant).
 * Si une version plus récente existe : ajoute une entrée « ⬆ Mise à jour »
 * en tête du menu, et affiche UNE MessageBox par nouvelle version (mémorisé
 * dans la config du module pour ne pas harceler à chaque lancement d'OBS).
 */
void vx_updater_check(QMenu *menu);

/**
 * Attend la fin du thread de vérification (unload du module). Sans ce join,
 * un thread encore vivant survivrait au déchargement de la DLL → crash.
 */
void vx_updater_shutdown(void);
