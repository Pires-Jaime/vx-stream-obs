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

#include <cstring> // strcmp : comparaison du nom de canvas
#include <mutex>
#include <string>

#include "vx-vertical.hpp"

namespace {

std::mutex mtx;
obs_canvas_t *canvas = nullptr;

// Suivi automatique de la scène principale. Allumé par défaut : c'est le
// comportement attendu d'un canvas vertical, et l'éteindre est un choix
// délibéré, pas le contraire.
bool linked = true;

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
	obs_data_set_bool(root, "linked", linked);
	obs_data_save_json_safe(root, config_file().c_str(), "tmp", "bak");
	obs_data_release(root);
}

} // namespace

void vx_vert_init(void)
{
	std::lock_guard<std::mutex> lock(mtx);
	if (canvas)
		return;

	// Le drapeau « scènes liées » vit dans le MÊME fichier que le canvas, mais
	// se relit indépendamment : il doit survivre même quand le canvas vient du
	// frontend et que le reste du fichier n'est plus lu.
	{
		obs_data_t *root = obs_data_create_from_json_file_safe(config_file().c_str(), "bak");
		if (root) {
			obs_data_set_default_bool(root, "linked", true);
			linked = obs_data_get_bool(root, "linked");
			obs_data_release(root);
		}
	}

	struct obs_video_info ovi;
	vertical_ovi(&ovi);

	// 1) Le canvas existe-t-il DÉJÀ côté frontend ? C'est le cas au second
	//    lancement : OBS le restaure avec la collection de scènes. On l'adopte
	//    au lieu d'en créer un second du même nom.
	{
		struct obs_frontend_canvas_list liste = {};
		obs_frontend_get_canvases(&liste);
		for (size_t i = 0; i < liste.canvases.num; i++) {
			obs_canvas_t *c = liste.canvases.array[i];
			const char *nom = obs_canvas_get_name(c);
			if (nom && strcmp(nom, "VX Vertical") == 0) {
				canvas = obs_canvas_get_ref(c);
				break;
			}
		}
		obs_frontend_canvas_list_free(&liste);
	}
	if (canvas)
		obs_log(LOG_INFO, "vertical : canvas frontend existant repris");
	if (!canvas) {
		// ⚠️ obs_frontend_add_canvas, PAS obs_canvas_create.
		//
		// `obs_canvas_create` travaille au niveau de libobs : le canvas existe
		// et s'affiche dans notre dock, mais l'INTERFACE d'OBS l'ignore
		// totalement. Conséquence constatée le 2026-09-30 : « VX Vertical »
		// n'apparaissait pas dans Paramètres → Flux → Canvas supplémentaire,
		// donc le Dual Format de Twitch nous était inaccessible — alors
		// qu'Aitum, lui, y figurait.
		//
		// La version frontend enregistre le canvas auprès de l'UI : il devient
		// sélectionnable pour le Dual Format, listé par obs_frontend_get_canvases
		// et sauvegardé avec la collection de scènes.
		//
		// ACTIVATE : les sources deviennent actives quand visibles (sans quoi
		// une source navigateur ne rend rien). SCENE_REF : le canvas retient
		// ses scènes. PAS de MIX_AUDIO : l'audio de la verticale est celui du
		// stream principal — mixer les sources du canvas doublerait le son.
		canvas = obs_frontend_add_canvas("VX Vertical", &ovi, ACTIVATE | SCENE_REF);

		// Repli : sur une version d'OBS sans l'API frontend des canvas, on
		// retombe sur l'ancien chemin plutôt que de perdre le 9:16 entier.
		// ⚠️ `vertical.json` n'est lu QUE dans ce repli. Un canvas chargé
		// depuis ce fichier n'est PAS enregistré auprès du frontend — c'était
		// toute la cause du problème. Le fichier n'est jamais supprimé : la
		// mise en page d'avant reste récupérable si besoin.
		if (!canvas) {
			obs_log(LOG_WARNING, "vertical : obs_frontend_add_canvas indisponible, repli libobs "
					     "(le Dual Format Twitch ne verra pas ce canvas)");
			obs_data_t *root = obs_data_create_from_json_file_safe(config_file().c_str(), "bak");
			if (root) {
				obs_data_t *cd = obs_data_get_obj(root, "canvas");
				if (cd) {
					canvas = obs_load_canvas(cd);
					obs_data_release(cd);
				}
				obs_data_release(root);
			}
			if (!canvas)
				canvas = obs_canvas_create("VX Vertical", &ovi, ACTIVATE | SCENE_REF);
		}
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
		// ⚠️ On NE retire PAS le canvas du frontend à l'extinction : OBS le
		// sauvegarde avec la collection de scènes et le restaurera au prochain
		// lancement. Le supprimer ici effacerait la mise en page verticale du
		// streamer à chaque fermeture d'OBS.
		obs_canvas_release(canvas);
		canvas = nullptr;
	}
}

// ── Scènes liées ─────────────────────────────────────────────────────────────

bool vx_vert_linked(void)
{
	std::lock_guard<std::mutex> lock(mtx);
	return linked;
}

void vx_vert_set_linked(bool on)
{
	{
		std::lock_guard<std::mutex> lock(mtx);
		if (linked == on)
			return;
		linked = on;
		save_locked();
	}
	// À l'activation, on rattrape tout de suite la scène courante : sinon le
	// streamer coche la case et ne voit rien changer jusqu'à sa prochaine
	// bascule — il croirait la fonction cassée.
	if (on)
		vx_vert_follow_main_scene();
}

void vx_vert_follow_main_scene(void)
{
	obs_canvas_t *c = nullptr;
	{
		std::lock_guard<std::mutex> lock(mtx);
		if (!linked || !canvas)
			return;
		c = canvas;
	}

	// Scène programme d'OBS. `obs_frontend_get_current_scene` renvoie une
	// référence forte : ne pas oublier de la rendre.
	obs_source_t *mainScene = obs_frontend_get_current_scene();
	if (!mainScene)
		return;
	const char *nom = obs_source_get_name(mainScene);

	// Correspondance PAR LE NOM. C'est volontairement simple : aucune table de
	// liaison à maintenir, et la règle s'explique en une phrase au streamer —
	// « donne le même nom aux deux scènes ». Une scène verticale sans jumelle
	// reste donc affichée telle quelle, sans écran noir.
	obs_scene_t *vertScene = nom ? obs_canvas_get_scene_by_name(c, nom) : nullptr;
	if (vertScene) {
		obs_source_t *src = obs_scene_get_source(vertScene);
		// ⚠️ Ne rebrancher que si ça CHANGE : reposer la même scène relance les
		// sources (une vidéo repartirait de zéro à chaque bascule d'OBS).
		obs_source_t *actuelle = obs_canvas_get_channel(c, 0);
		if (actuelle != src)
			obs_canvas_set_channel(c, 0, src);
		if (actuelle)
			obs_source_release(actuelle);
	}
	obs_source_release(mainScene);
}
