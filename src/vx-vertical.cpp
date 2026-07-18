/*
VX.Stream pour OBS — canvas vertical (VX Vertical)
Copyright (C) 2026 Valerix (Jaime Pires) <support@valerix.stream>
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Notre propre canvas 9:16, bâti sur l'API canvas NATIVE de libobs (31.1+) —
// celle qui n'existait pas quand Aitum a écrit Vertical Canvas et l'a forcé à
// réimplémenter tout un pipeline vidéo à la main. Ici : obs_canvas_create
// (1080×1920, fps/couleurs hérités du principal), scènes attachées au canvas,
// obs_save_canvas/obs_load_canvas pour la persistance, obs_canvas_get_video
// pour brancher l'encodeur de la sortie RTMP verticale.
//
// Contrepartie assumée (physique, Aitum pareil) : résolution différente ⇒ la
// sortie verticale a SON ENCODEUR VIDÉO (2e encodage). On duplique le type et
// les réglages de l'encodeur du stream principal (même NVENC/x264, même
// qualité) avec notre bitrate ; l'AUDIO, lui, est partagé avec le principal —
// d'où la même règle que le multistream : la verticale ne tourne QUE pendant
// le stream principal, et se coupe sur STREAMING_STOPPING.

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <cstring>
#include <mutex>

#include "vx-vertical.hpp"

namespace {

std::mutex mtx;
obs_canvas_t *canvas = nullptr;
obs_output_t *output = nullptr;
obs_service_t *service = nullptr;
obs_encoder_t *venc = nullptr;
VxVertSettings settings;

std::string config_file()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *p = obs_module_config_path("vertical.json");
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

void vertical_ovi(struct obs_video_info *ovi)
{
	// fps + espace colorimétrique du principal, résolution 1080×1920.
	obs_get_video_info(ovi);
	ovi->base_width = 1080;
	ovi->base_height = 1920;
	ovi->output_width = 1080;
	ovi->output_height = 1920;
}

void ensure_scene_locked()
{
	// Au moins une scène, et le canal 0 (programme) branché dessus.
	obs_source_t *ch = obs_canvas_get_channel(canvas, 0);
	if (ch) {
		obs_source_release(ch);
		return;
	}
	struct Ctx {
		obs_source_t *first = nullptr;
	} ctx;
	obs_canvas_enum_scenes(
		canvas,
		[](void *p, obs_source_t *src) {
			auto *c = static_cast<Ctx *>(p);
			if (!c->first)
				c->first = obs_source_get_ref(src);
			return c->first == nullptr;
		},
		&ctx);
	if (!ctx.first) {
		obs_scene_t *scene = obs_canvas_scene_create(canvas, "Verticale 1");
		if (scene)
			ctx.first = obs_source_get_ref(obs_scene_get_source(scene));
	}
	if (ctx.first) {
		obs_canvas_set_channel(canvas, 0, ctx.first);
		obs_source_release(ctx.first);
	}
}

void save_locked()
{
	if (!canvas)
		return;
	obs_data_t *root = obs_data_create();
	obs_data_t *cd = obs_save_canvas(canvas);
	if (cd) {
		obs_data_set_obj(root, "canvas", cd);
		obs_data_release(cd);
	}
	obs_data_set_string(root, "server", settings.server.c_str());
	obs_data_set_string(root, "key", settings.key.c_str());
	obs_data_set_bool(root, "enabled", settings.enabled);
	obs_data_set_int(root, "bitrate", settings.bitrate);
	obs_data_save_json_safe(root, config_file().c_str(), "tmp", "bak");
	obs_data_release(root);
}

void release_output_locked()
{
	if (output) {
		if (obs_output_active(output))
			obs_output_stop(output);
		obs_output_release(output);
		output = nullptr;
	}
	if (service) {
		obs_service_release(service);
		service = nullptr;
	}
	if (venc) {
		obs_encoder_release(venc);
		venc = nullptr;
	}
}

} // namespace

void vx_vert_init(void)
{
	std::lock_guard<std::mutex> lock(mtx);
	if (canvas)
		return;

	obs_data_t *root = obs_data_create_from_json_file_safe(config_file().c_str(), "bak");
	if (root) {
		settings.server = obs_data_get_string(root, "server");
		settings.key = obs_data_get_string(root, "key");
		settings.enabled = obs_data_get_bool(root, "enabled");
		obs_data_set_default_int(root, "bitrate", 4500);
		settings.bitrate = (int)obs_data_get_int(root, "bitrate");
		obs_data_t *cd = obs_data_get_obj(root, "canvas");
		if (cd) {
			canvas = obs_load_canvas(cd);
			obs_data_release(cd);
		}
		obs_data_release(root);
	}

	struct obs_video_info ovi;
	vertical_ovi(&ovi);
	if (!canvas) {
		// ACTIVATE : les sources deviennent actives quand visibles (sans quoi
		// une source navigateur ne rend rien). SCENE_REF : le canvas retient
		// ses scènes. PAS de MIX_AUDIO : l'audio de la verticale est celui du
		// stream principal — mixer les sources du canvas doublerait le son.
		canvas = obs_canvas_create("VX Vertical", &ovi, ACTIVATE | SCENE_REF);
	}
	if (!canvas) {
		obs_log(LOG_ERROR, "vertical : création du canvas impossible");
		return;
	}
	if (!obs_canvas_has_video(canvas))
		obs_canvas_reset_video(canvas, &ovi);
	ensure_scene_locked();
	obs_log(LOG_INFO, "vertical : canvas 1080×1920 prêt");
}

obs_canvas_t *vx_vert_canvas(void)
{
	return canvas;
}

VxVertSettings vx_vert_get_settings(void)
{
	std::lock_guard<std::mutex> lock(mtx);
	return settings;
}

void vx_vert_set_settings(const VxVertSettings &s)
{
	std::lock_guard<std::mutex> lock(mtx);
	settings = s;
	save_locked();
}

void vx_vert_save(void)
{
	std::lock_guard<std::mutex> lock(mtx);
	save_locked();
}

bool vx_vert_active(void)
{
	std::lock_guard<std::mutex> lock(mtx);
	return output && obs_output_active(output);
}

bool vx_vert_start(std::string *whyNot)
{
	std::lock_guard<std::mutex> lock(mtx);
	if (!canvas) {
		if (whyNot)
			*whyNot = "canvas vertical indisponible";
		return false;
	}
	if (settings.server.empty() || settings.key.empty()) {
		if (whyNot)
			*whyNot = "serveur ou clé manquant (réglages du dock VX Vertical)";
		return false;
	}
	if (output && obs_output_active(output))
		return true;

	// L'audio vient du stream principal (encodeur partagé) : il doit tourner.
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
	obs_encoder_t *mainA = obs_output_get_audio_encoder(main, 0);
	if (!mainA) {
		obs_output_release(main);
		if (whyNot)
			*whyNot = "encodeur audio du stream principal indisponible";
		return false;
	}

	release_output_locked();

	// Encodeur vidéo DÉDIÉ : même type/réglages que le principal (NVENC reste
	// NVENC), bitrate à nous — mais branché sur la vidéo du canvas vertical.
	const char *vid = mainV ? obs_encoder_get_id(mainV) : "obs_x264";
	obs_data_t *vs = mainV ? obs_encoder_get_settings(mainV) : obs_data_create();
	obs_data_set_int(vs, "bitrate", settings.bitrate);
	venc = obs_video_encoder_create(vid, "vx_vert_venc", vs, nullptr);
	obs_data_release(vs);
	if (!venc && strcmp(vid, "obs_x264") != 0) {
		obs_data_t *fb = obs_data_create();
		obs_data_set_int(fb, "bitrate", settings.bitrate);
		venc = obs_video_encoder_create("obs_x264", "vx_vert_venc", fb, nullptr);
		obs_data_release(fb);
	}
	obs_output_release(main);
	if (!venc) {
		if (whyNot)
			*whyNot = "création de l'encodeur vertical impossible";
		return false;
	}
	obs_encoder_set_video(venc, obs_canvas_get_video(canvas));

	obs_data_t *ss = obs_data_create();
	obs_data_set_string(ss, "server", settings.server.c_str());
	obs_data_set_string(ss, "key", settings.key.c_str());
	service = obs_service_create("rtmp_custom", "vx_vert_svc", ss, nullptr);
	obs_data_release(ss);
	output = obs_output_create("rtmp_output", "vx_vert_out", nullptr, nullptr);
	if (!service || !output) {
		release_output_locked();
		if (whyNot)
			*whyNot = "création de la sortie RTMP impossible";
		return false;
	}
	obs_output_set_service(output, service);
	obs_output_set_video_encoder(output, venc);
	obs_output_set_audio_encoder(output, mainA, 0);
	obs_output_set_reconnect_settings(output, 20, 2);

	if (!obs_output_start(output)) {
		const char *err = obs_output_get_last_error(output);
		if (whyNot)
			*whyNot = err && *err ? err : "démarrage refusé";
		release_output_locked();
		return false;
	}
	obs_log(LOG_INFO, "vertical : diffusion 9:16 démarrée (%s)", settings.server.c_str());
	return true;
}

void vx_vert_stop(void)
{
	std::lock_guard<std::mutex> lock(mtx);
	release_output_locked();
}

void vx_vert_on_stream(bool started)
{
	if (started) {
		if (!vx_vert_get_settings().enabled)
			return;
		std::string why;
		if (!vx_vert_start(&why))
			obs_log(LOG_WARNING, "vertical auto : %s", why.c_str());
	} else {
		// STOPPING : notre sortie partage l'encodeur AUDIO du principal.
		vx_vert_stop();
	}
}

void vx_vert_shutdown(void)
{
	std::lock_guard<std::mutex> lock(mtx);
	save_locked();
	release_output_locked();
	if (canvas) {
		obs_canvas_set_channel(canvas, 0, nullptr);
		obs_canvas_remove(canvas);
		obs_canvas_release(canvas);
		canvas = nullptr;
	}
}
