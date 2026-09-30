/*
VX.Stream pour OBS — canvas vertical (VX Vertical)
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <obs.h>

// Le canvas 9:16 est une SOURCE de diffusion : ce sont les destinations du dock
// VX Multistream marquées « vertical » qui l'émettent (encodeur partagé). VX
// Vertical ne fait donc que composer le canvas (scènes/sources) et l'afficher.

/** Crée/recharge le canvas 1080×1920 (FINISHED_LOADING). */
void vx_vert_init(void);

/** Sauvegarde + libération du canvas (EXIT, APRÈS la destruction de l'aperçu
 *  ET l'arrêt des sorties multistream verticales qui référencent sa vidéo). */
void vx_vert_shutdown(void);

/** Canvas courant (pointeur possédé par le module — ne pas release). */
obs_canvas_t *vx_vert_canvas(void);

/** Persiste le canvas tout de suite (après une modification de scène/source). */
void vx_vert_save(void);

// ── Scènes liées ─────────────────────────────────────────────────────────────
// Changer de scène dans OBS bascule AUSSI la scène verticale, quand les deux
// portent le MÊME NOM. Sans ça il faut basculer deux fois à chaque changement,
// en direct — c'est la source d'erreur nº1 d'un setup 9:16 (parité SE.Live).

/** Le suivi automatique est-il actif ? (persisté) */
bool vx_vert_linked(void);

/** Active/désactive le suivi et persiste le choix. */
void vx_vert_set_linked(bool on);

/** À appeler sur OBS_FRONTEND_EVENT_SCENE_CHANGED. Sans effet si le suivi est
 *  éteint, ou si aucune scène verticale ne porte le nom de la scène OBS. */
void vx_vert_follow_main_scene(void);
