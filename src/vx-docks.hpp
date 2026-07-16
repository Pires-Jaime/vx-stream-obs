/*
VX.Stream pour OBS — docks navigateur natifs
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <cstddef>

class QAction;

/** Initialise CEF (via obs-browser) et enregistre les 4 docks VX. */
bool vx_create_docks(void);

/** obs-browser était-il disponible ? (false = docks non créés) */
bool vx_docks_available(void);

/** toggleViewAction du dock (case cochable native), ou nullptr. */
QAction *vx_dock_toggle_action(const char *id);

/** Ids des docks, pour construire le menu. */
const char *const *vx_dock_ids(size_t *count);
