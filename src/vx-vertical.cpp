/*
VX.Stream pour OBS — canvas vertical (VX Vertical)
Copyright (C) 2026 Valerix (Jaime Pires) <support@valerix.stream>
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Notre propre canvas 9:16, bâti sur l'API canvas NATIVE de libobs (31.1+) —
// celle qui n'existait pas quand Aitum a écrit Vertical Canvas et l'a forcé à
// réimplémenter tout un pipeline vidéo à la main. Ici : obs_canvas_create
// (1080×1920, fps/couleurs hérités du principal), scènes attachées au canvas,
// obs_save_canvas/obs_load_canvas pour la persistance.
//
// VX Vertical ne fait QUE composer et afficher le canvas. La DIFFUSION du 9:16
// passe par les destinations du dock VX Multistream marquées « vertical » :
// elles branchent un encodeur partagé sur obs_canvas_get_video(). Une même
// image verticale peut ainsi partir vers TikTok, YouTube vertical, etc.

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <mutex>
#include <string>

#include "vx-vertical.hpp"

namespace {

std::mutex mtx;
obs_canvas_t *canvas = nullptr;

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
	obs_data_save_json_safe(root, config_file().c_str(), "tmp", "bak");
	obs_data_release(root);
}

} // namespace

void vx_vert_init(void)
{
	std::lock_guard<std::mutex> lock(mtx);
	if (canvas)
		return;

	obs_data_t *root = obs_data_create_from_json_file_safe(config_file().c_str(), "bak");
	if (root) {
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

void vx_vert_save(void)
{
	std::lock_guard<std::mutex> lock(mtx);
	save_locked();
}

void vx_vert_shutdown(void)
{
	std::lock_guard<std::mutex> lock(mtx);
	save_locked();
	if (canvas) {
		obs_canvas_set_channel(canvas, 0, nullptr);
		obs_canvas_remove(canvas);
		obs_canvas_release(canvas);
		canvas = nullptr;
	}
}
