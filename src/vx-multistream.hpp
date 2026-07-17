/*
VX.Stream pour OBS — multistream local (sorties RTMP additionnelles)
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <string>
#include <vector>

struct VxTarget {
	std::string id;     // identifiant stable (horodatage)
	std::string name;   // libellé affiché ("Kick", "YouTube"…)
	std::string server; // rtmp(s)://…
	std::string key;    // clé de stream
	bool autoStart = true;
};

/** Charge les cibles depuis la config du module (appel au chargement). */
void vx_ms_load(void);

/** Liste courante (copie — la vérité vit dans le module). */
std::vector<VxTarget> vx_ms_targets(void);

void vx_ms_upsert(const VxTarget &t);
void vx_ms_remove(const std::string &id);

/** Démarre une cible. Renvoie false avec raison si impossible. */
bool vx_ms_start(const std::string &id, std::string *whyNot);
void vx_ms_stop(const std::string &id);
bool vx_ms_active(const std::string &id);

/** Hooks frontend : démarrage/arrêt automatique avec le stream principal. */
void vx_ms_on_streaming_started(void);
void vx_ms_on_streaming_stopping(void);

/** Arrêt + libération de tout (unload du module). */
void vx_ms_shutdown(void);
