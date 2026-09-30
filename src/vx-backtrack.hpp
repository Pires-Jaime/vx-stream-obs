/*
VX.Stream pour OBS — Backtrack du canvas vertical
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

// Tampon de relecture (replay buffer) branché sur le canvas 9:16 : un raccourci
// clavier enregistre les N dernières secondes de la VERTICALE, ce que le replay
// buffer natif d'OBS ne sait pas faire (il ne connaît que le canvas principal).
// Parité SE.Live « Backtrack ».
//
// ⚠️ Ne tourne QUE pendant le direct, comme chez eux : un tampon actif encode en
// permanence, y compris à vide. Laisser ça allumé hors live brûle du CPU pour rien.

/** Enregistre le raccourci clavier et relit les réglages. À l'initialisation. */
void vx_backtrack_init(void);

/** Démarre le tampon si la fonction est activée (STREAMING_STARTED). */
void vx_backtrack_start(void);

/** Arrête le tampon et libère encodeurs et sortie (STREAMING_STOPPING, EXIT). */
void vx_backtrack_stop(void);

/** Écrit le contenu du tampon sur le disque. Sans effet s'il ne tourne pas. */
void vx_backtrack_save(void);

/** Activé par le streamer ? (persisté) */
bool vx_backtrack_enabled(void);
void vx_backtrack_set_enabled(bool on);

/** Durée du tampon, en secondes. */
int vx_backtrack_seconds(void);
void vx_backtrack_set_seconds(int s);
