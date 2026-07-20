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
