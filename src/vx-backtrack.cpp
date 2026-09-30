/*
VX.Stream pour OBS — Backtrack du canvas vertical
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Le replay buffer natif d'OBS ne connaît QUE le canvas principal : impossible
// d'en tirer un extrait vertical. On monte donc notre propre sortie
// `replay_buffer` sur la vidéo du canvas 9:16, avec son encodeur dédié et
// l'audio du mix principal.
//
// ⚠️ Un tampon de relecture encode EN CONTINU, même quand rien ne se passe.
// D'où deux garde-fous repris de SE.Live : il ne tourne que pendant le direct,
// et la durée est plafonnée (180 s par défaut, comme leur recommandation).

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs-hotkey.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <mutex>
#include <string>

#include "vx-backtrack.hpp"
#include "vx-vertical.hpp"

namespace {

std::mutex mtx;
obs_output_t *output = nullptr;
obs_encoder_t *venc = nullptr;
obs_encoder_t *aenc = nullptr;
obs_hotkey_id hotkey = OBS_INVALID_HOTKEY_ID;

bool enabled = false; // éteint par défaut : ça consomme, on ne l'impose pas
int seconds = 180;    // plafond conseillé par SE.Live

std::string config_file()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *p = obs_module_config_path("backtrack.json");
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

void save_locked()
{
	obs_data_t *root = obs_data_create();
	obs_data_set_bool(root, "enabled", enabled);
	obs_data_set_int(root, "seconds", seconds);
	obs_data_save_json_safe(root, config_file().c_str(), "tmp", "bak");
	obs_data_release(root);
}

/** Dossier de sortie : celui des enregistrements d'OBS, pour que le streamer
 *  retrouve ses extraits là où il a l'habitude. Repli sur le dossier du module
 *  si OBS ne le donne pas (profil incomplet). */
std::string output_dir()
{
	char *p = obs_frontend_get_current_record_output_path();
	std::string dir = p ? p : "";
	bfree(p);
	if (dir.empty()) {
		char *m = obs_module_config_path("backtrack");
		dir = m ? m : "";
		bfree(m);
		if (!dir.empty())
			os_mkdirs(dir.c_str());
	}
	return dir;
}

/** Libère sortie et encodeurs. À appeler SOUS le verrou. */
void teardown_locked()
{
	if (output) {
		obs_output_stop(output);
		obs_output_release(output);
		output = nullptr;
	}
	// ⚠️ Les encodeurs se libèrent APRÈS la sortie : ils lui sont attachés, et
	// les lâcher d'abord a déjà coûté un crash à la fermeture sur le multistream.
	if (venc) {
		obs_encoder_release(venc);
		venc = nullptr;
	}
	if (aenc) {
		obs_encoder_release(aenc);
		aenc = nullptr;
	}
}

void on_hotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (pressed)
		vx_backtrack_save();
}

} // namespace

void vx_backtrack_init(void)
{
	std::lock_guard<std::mutex> lock(mtx);
	obs_data_t *root = obs_data_create_from_json_file_safe(config_file().c_str(), "bak");
	if (root) {
		obs_data_set_default_bool(root, "enabled", false);
		obs_data_set_default_int(root, "seconds", 180);
		enabled = obs_data_get_bool(root, "enabled");
		seconds = (int)obs_data_get_int(root, "seconds");
		obs_data_release(root);
	}
	seconds = std::clamp(seconds, 10, 600);

	if (hotkey == OBS_INVALID_HOTKEY_ID) {
		hotkey = obs_hotkey_register_frontend(
			"vx_backtrack_save", "VX Backtrack : enregistrer l'extrait vertical", on_hotkey, nullptr);
	}
}

void vx_backtrack_start(void)
{
	std::lock_guard<std::mutex> lock(mtx);
	if (!enabled || output)
		return;

	obs_canvas_t *canvas = vx_vert_canvas();
	if (!canvas || !obs_canvas_has_video(canvas)) {
		obs_log(LOG_WARNING, "backtrack : canvas vertical indisponible");
		return;
	}

	const std::string dir = output_dir();
	if (dir.empty()) {
		obs_log(LOG_WARNING, "backtrack : aucun dossier de sortie");
		return;
	}

	obs_data_t *st = obs_data_create();
	obs_data_set_string(st, "directory", dir.c_str());
	obs_data_set_string(st, "format", "VX-Vertical %CCYY-%MM-%DD %hh-%mm-%ss");
	obs_data_set_string(st, "extension", "mp4");
	obs_data_set_int(st, "max_time_sec", seconds);
	// 0 = pas de plafond en Mo : c'est la DURÉE qui borne, un double plafond
	// tronquerait l'extrait sans que personne comprenne pourquoi.
	obs_data_set_int(st, "max_size_mb", 0);
	output = obs_output_create("replay_buffer", "vx_backtrack", st, nullptr);
	obs_data_release(st);
	if (!output) {
		obs_log(LOG_ERROR, "backtrack : sortie replay_buffer impossible");
		return;
	}

	// Encodeur vidéo dédié, branché sur la vidéo du canvas 9:16. On ne
	// réutilise PAS celui du multistream : il peut être occupé par une
	// destination, et deux sorties sur un même encodeur se marchent dessus.
	obs_data_t *vs = obs_data_create();
	obs_data_set_int(vs, "bitrate", 6000);
	venc = obs_video_encoder_create("obs_x264", "vx_backtrack_venc", vs, nullptr);
	obs_data_release(vs);
	if (venc)
		obs_encoder_set_video(venc, obs_canvas_get_video(canvas));

	// Audio : le mix principal. La verticale n'a pas d'audio propre (le canvas
	// est créé sans MIX_AUDIO, exprès, pour ne pas doubler le son).
	obs_data_t *as = obs_data_create();
	obs_data_set_int(as, "bitrate", 160);
	aenc = obs_audio_encoder_create("ffmpeg_aac", "vx_backtrack_aenc", as, 0, nullptr);
	obs_data_release(as);
	if (aenc)
		obs_encoder_set_audio(aenc, obs_get_audio());

	if (!venc || !aenc) {
		obs_log(LOG_ERROR, "backtrack : encodeurs indisponibles");
		teardown_locked();
		return;
	}
	obs_output_set_video_encoder(output, venc);
	obs_output_set_audio_encoder(output, aenc, 0);

	if (!obs_output_start(output)) {
		obs_log(LOG_ERROR, "backtrack : démarrage refusé — %s", obs_output_get_last_error(output));
		teardown_locked();
		return;
	}
	obs_log(LOG_INFO, "backtrack : tampon vertical actif (%d s)", seconds);
}

void vx_backtrack_stop(void)
{
	std::lock_guard<std::mutex> lock(mtx);
	teardown_locked();
}

void vx_backtrack_save(void)
{
	std::lock_guard<std::mutex> lock(mtx);
	if (!output || !obs_output_active(output)) {
		obs_log(LOG_INFO, "backtrack : rien à enregistrer (tampon inactif)");
		return;
	}
	proc_handler_t *ph = obs_output_get_proc_handler(output);
	if (!ph)
		return;
	calldata_t cd = {};
	proc_handler_call(ph, "save", &cd);
	calldata_free(&cd);
	obs_log(LOG_INFO, "backtrack : extrait vertical enregistré");
}

bool vx_backtrack_enabled(void)
{
	std::lock_guard<std::mutex> lock(mtx);
	return enabled;
}

void vx_backtrack_set_enabled(bool on)
{
	{
		std::lock_guard<std::mutex> lock(mtx);
		if (enabled == on)
			return;
		enabled = on;
		save_locked();
		if (!on) {
			teardown_locked();
			return;
		}
	}
	// Activé en plein direct : on démarre tout de suite plutôt que d'attendre
	// le prochain live — sinon la case cochée ne ferait rien de visible.
	if (obs_frontend_streaming_active())
		vx_backtrack_start();
}

int vx_backtrack_seconds(void)
{
	std::lock_guard<std::mutex> lock(mtx);
	return seconds;
}

void vx_backtrack_set_seconds(int s)
{
	std::lock_guard<std::mutex> lock(mtx);
	seconds = std::clamp(s, 10, 600);
	save_locked();
}
