/*
VX.Stream pour OBS — flux d'évènements Valerix (actions à exécuter localement)
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <functional>
#include <string>
#include <vector>

/**
 * Analyseur de flux SSE, isolé du transport pour être testable hors Windows.
 *
 * Le format est simple mais piégeux : un évènement se termine par une LIGNE
 * VIDE, un même évènement peut s'étaler sur plusieurs lignes `data:`, et les
 * lignes commençant par `:` sont des commentaires (nos battements de cœur).
 * Un analyseur naïf « une ligne = un évènement » perd les messages longs et
 * traite les pings comme des données.
 */
class VxSseParser {
public:
	/** Absorbe un fragment reçu du réseau ; renvoie les évènements complets. */
	std::vector<std::string> feed(const std::string &chunk);

private:
	std::string buf;     // octets pas encore terminés par un saut de ligne
	std::string current; // données de l'évènement en cours d'assemblage
};

/**
 * Démarre le fil qui maintient la connexion au flux et exécute les actions.
 * Sans jeton, ne fait rien : on ne peut pas deviner de quel streamer il s'agit.
 */
void vx_events_start(void);

/** Arrête le fil proprement (appelé au déchargement du module). */
void vx_events_stop(void);

/** Enregistre le jeton d'évènements transmis par la page « Mon compte ». */
void vx_events_set_token(const std::string &token);
