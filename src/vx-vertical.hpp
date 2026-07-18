/*
VX.Stream pour OBS — canvas vertical (VX Vertical)
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <obs.h>
#include <string>

struct VxVertSettings {
	std::string server;   // rtmp(s)://… (TikTok, YouTube…)
	std::string key;      // clé de stream
	bool enabled = false; // diffuser automatiquement avec le stream principal
	int bitrate = 4500;   // kbps vidéo de la sortie verticale
};

/** Crée/recharge le canvas 1080×1920 (FINISHED_LOADING). */
void vx_vert_init(void);

/** Sauvegarde + arrêt + libération (EXIT, APRÈS la destruction de l'aperçu). */
void vx_vert_shutdown(void);

/** Hooks stream principal : auto start/stop de la sortie verticale. */
void vx_vert_on_stream(bool started);

/** Canvas courant (pointeur possédé par le module — ne pas release). */
obs_canvas_t *vx_vert_canvas(void);

VxVertSettings vx_vert_get_settings(void);
void vx_vert_set_settings(const VxVertSettings &s);

bool vx_vert_active(void);
bool vx_vert_start(std::string *whyNot);
void vx_vert_stop(void);

/** Persiste canvas + réglages tout de suite (après une modification UI). */
void vx_vert_save(void);
