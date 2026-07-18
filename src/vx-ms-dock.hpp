/*
VX.Stream pour OBS — dock natif Multistream (toggles par plateforme)
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

/** Crée le dock « VX Multistream » (après FINISHED_LOADING). */
void vx_ms_dock_create(void);

/** Retire le dock (EXIT). Widget Qt pur, pas de CEF — retrait simple. */
void vx_ms_dock_destroy(void);

/** Rafraîchit la liste (à appeler quand les cibles changent). */
void vx_ms_dock_refresh(void);
