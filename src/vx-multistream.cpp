/*
VX.Stream pour OBS — multistream local
Copyright (C) 2026 Valerix (Jaime Pires) <support@valerix.stream>
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Multistream « local » : chaque cible est un obs_output rtmp supplémentaire
// qui RÉUTILISE les encodeurs du stream principal — donc zéro encodage en
// plus (même image, même qualité partout) ; le seul coût est l'upload du
// streamer (bitrate × nombre de cibles). Contrepartie assumée : une cible ne
// peut démarrer que si le stream principal est actif (c'est lui qui possède
// les encodeurs) — d'où l'auto-start sur STREAMING_STARTED.
//
// C'est l'architecture d'obs-multi-rtmp (GPL), en version volontairement
// réduite : pas d'encodeurs dédiés par cible, pas de multi-track.

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>

#include "vx-multistream.hpp"
#include "vx-vertical.hpp"

namespace {

struct Live {
	obs_output_t *output = nullptr;
	obs_service_t *service = nullptr;
};

std::mutex mtx;
std::vector<VxTarget> targets;
std::vector<std::pair<std::string, Live>> lives; // id → sorties actives
// Encodeur vidéo du canvas VERTICAL, PARTAGÉ par toutes les destinations 9:16
// (un seul encodage pour TikTok + YouTube vertical + …). Créé à la volée quand
// une cible verticale démarre, libéré à l'arrêt du stream. L'audio, lui, reste
// celui du stream principal (partagé aussi côté horizontal).
obs_encoder_t *vertVenc = nullptr;

void release_vert_encoder()
{
	if (vertVenc) {
		obs_encoder_release(vertVenc);
		vertVenc = nullptr;
	}
}

// Encodeur vidéo pour le canvas vertical : cloné du principal (NVENC reste
// NVENC, mêmes réglages) mais branché sur la vidéo du canvas 9:16. Nullptr si
// le canvas vertical n'est pas prêt. Réutilisé entre destinations verticales.
obs_encoder_t *vertical_video_encoder(obs_encoder_t *mainV)
{
	if (vertVenc)
		return vertVenc;
	obs_canvas_t *canvas = vx_vert_canvas();
	if (!canvas || !obs_canvas_has_video(canvas))
		return nullptr;
	const char *vid = mainV ? obs_encoder_get_id(mainV) : "obs_x264";
	obs_data_t *vs = mainV ? obs_encoder_get_settings(mainV) : obs_data_create();
	vertVenc = obs_video_encoder_create(vid, "vx_ms_vert_venc", vs, nullptr);
	obs_data_release(vs);
	if (!vertVenc && strcmp(vid, "obs_x264") != 0) {
		obs_data_t *fb = obs_data_create();
		vertVenc = obs_video_encoder_create("obs_x264", "vx_ms_vert_venc", fb, nullptr);
		obs_data_release(fb);
	}
	if (vertVenc)
		obs_encoder_set_video(vertVenc, obs_canvas_get_video(canvas));
	return vertVenc;
}

std::string config_file()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *p = obs_module_config_path("multistream.json");
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

void save_locked()
{
	obs_data_t *root = obs_data_create();
	obs_data_array_t *arr = obs_data_array_create();
	for (const VxTarget &t : targets) {
		obs_data_t *o = obs_data_create();
		obs_data_set_string(o, "id", t.id.c_str());
		obs_data_set_string(o, "name", t.name.c_str());
		obs_data_set_string(o, "platform", t.platform.c_str());
		obs_data_set_string(o, "server", t.server.c_str());
		obs_data_set_string(o, "key", t.key.c_str());
		obs_data_set_string(o, "canvas", t.canvas.c_str());
		obs_data_set_bool(o, "enabled", t.enabled);
		obs_data_array_push_back(arr, o);
		obs_data_release(o);
	}
	obs_data_set_array(root, "targets", arr);
	obs_data_array_release(arr);
	obs_data_save_json_safe(root, config_file().c_str(), "tmp", "bak");
	obs_data_release(root);
}

Live *find_live(const std::string &id)
{
	for (auto &p : lives)
		if (p.first == id)
			return &p.second;
	return nullptr;
}

void release_live(Live &l)
{
	if (l.output) {
		obs_output_release(l.output);
		l.output = nullptr;
	}
	if (l.service) {
		obs_service_release(l.service);
		l.service = nullptr;
	}
}

void stop_locked(const std::string &id)
{
	Live *l = find_live(id);
	if (!l)
		return;
	if (l->output && obs_output_active(l->output))
		obs_output_stop(l->output);
	release_live(*l);
	lives.erase(std::remove_if(lives.begin(), lives.end(), [&](auto &p) { return p.first == id; }), lives.end());
}

} // namespace

void vx_ms_load(void)
{
	std::lock_guard<std::mutex> lock(mtx);
	targets.clear();
	obs_data_t *root = obs_data_create_from_json_file_safe(config_file().c_str(), "bak");
	if (!root)
		return;
	obs_data_array_t *arr = obs_data_get_array(root, "targets");
	const size_t n = arr ? obs_data_array_count(arr) : 0;
	for (size_t i = 0; i < n; i++) {
		obs_data_t *o = obs_data_array_item(arr, i);
		VxTarget t;
		t.id = obs_data_get_string(o, "id");
		t.name = obs_data_get_string(o, "name");
		t.platform = obs_data_get_string(o, "platform");
		t.server = obs_data_get_string(o, "server");
		t.key = obs_data_get_string(o, "key");
		// Migration <0.8.0 : « enabled » remplace « auto_start » (défaut true).
		obs_data_set_default_bool(o, "enabled",
					  obs_data_has_user_value(o, "auto_start") ? obs_data_get_bool(o, "auto_start")
										   : true);
		t.enabled = obs_data_get_bool(o, "enabled");
		// Canvas (défaut horizontal ; absent sur les configs d'avant 0.10.0).
		obs_data_set_default_string(o, "canvas", "horizontal");
		t.canvas = obs_data_get_string(o, "canvas");
		if (t.canvas != "vertical")
			t.canvas = "horizontal";
		// Migration : les cibles d'avant 0.8.0 n'ont pas de plateforme → on la
		// devine du serveur/nom pour afficher le bon logo.
		if (t.platform.empty()) {
			auto has = [](const std::string &s, const char *needle) {
				std::string low = s;
				std::transform(low.begin(), low.end(), low.begin(),
					       [](unsigned char c) { return (char)std::tolower(c); });
				return low.find(needle) != std::string::npos;
			};
			if (has(t.server, "youtube") || has(t.name, "youtube"))
				t.platform = "youtube";
			else if (has(t.server, "twitch") || has(t.name, "twitch"))
				t.platform = "twitch";
			else if (has(t.name, "kick") || has(t.server, "kick"))
				t.platform = "kick";
			else if (has(t.name, "tiktok") || has(t.server, "tiktok"))
				t.platform = "tiktok";
			else
				t.platform = "custom";
		}
		if (!t.id.empty())
			targets.push_back(std::move(t));
		obs_data_release(o);
	}
	if (arr)
		obs_data_array_release(arr);
	obs_data_release(root);
	obs_log(LOG_INFO, "multistream : %zu cible(s) chargée(s)", targets.size());
}

std::vector<VxTarget> vx_ms_targets(void)
{
	std::lock_guard<std::mutex> lock(mtx);
	return targets;
}

void vx_ms_upsert(const VxTarget &t)
{
	std::lock_guard<std::mutex> lock(mtx);
	for (VxTarget &e : targets) {
		if (e.id == t.id) {
			e = t;
			save_locked();
			return;
		}
	}
	targets.push_back(t);
	save_locked();
}

void vx_ms_remove(const std::string &id)
{
	std::lock_guard<std::mutex> lock(mtx);
	stop_locked(id);
	targets.erase(std::remove_if(targets.begin(), targets.end(), [&](const VxTarget &t) { return t.id == id; }),
		      targets.end());
	save_locked();
}

bool vx_ms_start(const std::string &id, std::string *whyNot)
{
	std::lock_guard<std::mutex> lock(mtx);

	const VxTarget *t = nullptr;
	for (const VxTarget &e : targets)
		if (e.id == id)
			t = &e;
	if (!t) {
		if (whyNot)
			*whyNot = "cible inconnue";
		return false;
	}
	if (t->server.empty() || t->key.empty()) {
		if (whyNot)
			*whyNot = "serveur ou clé manquant";
		return false;
	}

	Live *existing = find_live(id);
	if (existing && existing->output && obs_output_active(existing->output))
		return true; // déjà en route

	// Les encodeurs appartiennent au stream principal : sans lui, rien à
	// partager (c'est la limite assumée du mode « zéro ré-encodage »).
	if (!obs_frontend_streaming_active()) {
		if (whyNot)
			*whyNot = "démarrez d'abord votre stream principal";
		return false;
	}
	obs_output_t *main = obs_frontend_get_streaming_output();
	if (!main) {
		if (whyNot)
			*whyNot = "sortie principale introuvable";
		return false;
	}
	obs_encoder_t *mainV = obs_output_get_video_encoder(main);
	obs_encoder_t *aenc = obs_output_get_audio_encoder(main, 0);
	obs_output_release(main);

	// Choix de la source vidéo : image du stream principal (horizontal) ou
	// canvas 9:16 (vertical). L'audio est partagé dans les deux cas.
	obs_encoder_t *venc = vx_target_is_vertical(*t) ? vertical_video_encoder(mainV) : mainV;
	if (!venc || !aenc) {
		if (whyNot)
			*whyNot = vx_target_is_vertical(*t)
					  ? "canvas vertical indisponible (ouvrez le dock VX Vertical)"
					  : "encodeurs du stream principal indisponibles";
		return false;
	}

	obs_data_t *ss = obs_data_create();
	obs_data_set_string(ss, "server", t->server.c_str());
	obs_data_set_string(ss, "key", t->key.c_str());
	obs_service_t *svc = obs_service_create("rtmp_custom", ("vx_ms_svc_" + id).c_str(), ss, nullptr);
	obs_data_release(ss);

	obs_output_t *out = obs_output_create("rtmp_output", ("vx_ms_out_" + id).c_str(), nullptr, nullptr);
	if (!svc || !out) {
		if (svc)
			obs_service_release(svc);
		if (out)
			obs_output_release(out);
		if (whyNot)
			*whyNot = "création de la sortie RTMP impossible";
		return false;
	}

	obs_output_set_service(out, svc);
	obs_output_set_video_encoder(out, venc);
	obs_output_set_audio_encoder(out, aenc, 0);
	obs_output_set_reconnect_settings(out, 20, 2); // coupure réseau ≠ fin de multistream

	if (!obs_output_start(out)) {
		const char *err = obs_output_get_last_error(out);
		if (whyNot)
			*whyNot = err && *err ? err : "démarrage refusé";
		obs_output_release(out);
		obs_service_release(svc);
		return false;
	}

	if (existing)
		release_live(*existing);
	lives.push_back({id, {out, svc}});
	obs_log(LOG_INFO, "multistream : « %s » démarré (%s)", t->name.c_str(), t->server.c_str());
	return true;
}

void vx_ms_set_enabled(const std::string &id, bool on)
{
	{
		std::lock_guard<std::mutex> lock(mtx);
		bool found = false;
		for (VxTarget &t : targets) {
			if (t.id == id) {
				t.enabled = on;
				found = true;
			}
		}
		if (!found)
			return;
		save_locked();
	}
	// Bascule à chaud, HORS verrou (vx_ms_start/stop le reprennent) : si le
	// stream principal tourne, le toggle agit tout de suite.
	if (!obs_frontend_streaming_active())
		return;
	if (on) {
		std::string why;
		if (!vx_ms_start(id, &why))
			obs_log(LOG_WARNING, "multistream toggle : %s → %s", id.c_str(), why.c_str());
	} else {
		vx_ms_stop(id);
	}
}

void vx_ms_stop(const std::string &id)
{
	std::lock_guard<std::mutex> lock(mtx);
	stop_locked(id);
}

bool vx_ms_active(const std::string &id)
{
	std::lock_guard<std::mutex> lock(mtx);
	Live *l = find_live(id);
	return l && l->output && obs_output_active(l->output);
}

void vx_ms_on_streaming_started(void)
{
	// Copie des ids d'abord : vx_ms_start reprend le verrou.
	std::vector<std::string> ids;
	{
		std::lock_guard<std::mutex> lock(mtx);
		for (const VxTarget &t : targets)
			if (t.enabled)
				ids.push_back(t.id);
	}
	for (const std::string &id : ids) {
		std::string why;
		if (!vx_ms_start(id, &why))
			obs_log(LOG_WARNING, "multistream auto : %s → %s", id.c_str(), why.c_str());
	}
}

void vx_ms_on_streaming_stopping(void)
{
	// Nos sorties partagent les encodeurs du principal : on les coupe avant
	// qu'il ne s'arrête, sinon il resterait retenu par nos références.
	std::lock_guard<std::mutex> lock(mtx);
	std::vector<std::string> ids;
	for (auto &p : lives)
		ids.push_back(p.first);
	for (const std::string &id : ids)
		stop_locked(id);
	// L'encodeur vertical référence la vidéo du canvas : le libérer AVANT que
	// le canvas ne soit détruit (EXIT) et pour ne pas retenir le GPU hors live.
	release_vert_encoder();
}

void vx_ms_shutdown(void)
{
	vx_ms_on_streaming_stopping();
}
