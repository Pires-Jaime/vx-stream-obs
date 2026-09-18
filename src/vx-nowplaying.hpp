/*
VX.Stream pour OBS — musique en cours lue sur le PC du streamer
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <string>

/**
 * Publie vers Valerix le titre joué par le PC, pour le widget « Musique en
 * cours » des overlays.
 *
 * ⚠️ POURQUOI PAS L'API SPOTIFY. Depuis février 2026, une application Spotify
 * en mode développement est plafonnée à 5 utilisateurs déclarés à la main, et
 * l'accès étendu est réservé aux plateformes à grande échelle : une app
 * Valerix unique ne servirait que cinq streamers. Windows, lui, expose la
 * session média de N'IMPORTE quel lecteur — Spotify, Deezer, un navigateur,
 * VLC — sans clé, sans quota et sans compte à connecter.
 */
void vx_nowplaying_start(void);

/** Arrête le fil proprement (appelé au déchargement du module). */
void vx_nowplaying_stop(void);

/** Même jeton que le flux d'évènements ; vide = rien n'est publié. */
void vx_nowplaying_set_token(const std::string &token);
