/*
VX.Stream pour OBS — création de scènes Valerix
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

/** Affiche le dialog « Créer mes scènes Valerix ». */
void vx_scenes_show_dialog(void);

/** Ferme/détruit le dialog (à appeler sur EXIT — il contient un widget CEF). */
void vx_scenes_shutdown(void);
