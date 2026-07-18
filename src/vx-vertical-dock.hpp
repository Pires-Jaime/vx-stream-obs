/*
VX.Stream pour OBS — dock VX Vertical (aperçu + scènes + sortie 9:16)
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

/** Crée le dock (après vx_vert_init). */
void vx_vert_dock_create(void);

/** Détruit l'aperçu (obs_display) SYNCHRONEMENT puis retire le dock (EXIT,
 *  AVANT vx_vert_shutdown — le draw callback référence le canvas). */
void vx_vert_dock_destroy(void);
