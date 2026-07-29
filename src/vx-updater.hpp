/*
VX.Stream pour OBS — vérification de mise à jour
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

class QMenu;

/**
 * Lance la vérification en arrière-plan (thread dédié, jamais bloquant).
 * Vérifie le PLUGIN et OBS lui-même : pour chacun, si une version plus récente
 * existe, ajoute une entrée « ⬆ … » en tête du menu et affiche UNE MessageBox
 * par nouvelle version (mémorisé dans la config du module pour ne pas harceler
 * à chaque lancement d'OBS).
 */
void vx_updater_check(QMenu *menu);

/**
 * « Vérifier les mises à jour… » (action utilisateur) : re-vérifie plugin + OBS
 * et affiche TOUJOURS le résultat (à jour ou pas), contrairement au check de
 * démarrage qui reste silencieux quand tout est à jour.
 */
void vx_updater_check_manual(QMenu *menu);

/**
 * Attend la fin des threads de vérification (unload du module). Sans ce join,
 * un thread encore vivant survivrait au déchargement de la DLL → crash.
 */
void vx_updater_shutdown(void);
