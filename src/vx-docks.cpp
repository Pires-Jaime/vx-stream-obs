/*
VX.Stream pour OBS — docks navigateur natifs
Copyright (C) 2026 Valerix (Jaime Pires) <support@valerix.stream>
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Les docks sont créés via l'API « panel » d'obs-browser (le CEF embarqué
// d'OBS, celui du dock chat Twitch officiel) — chargée dynamiquement par
// obs_browser_init_panel() : si obs-browser manque, on dégrade sans crash.
//
// Les URLs ne portent AUCUN token : elles pointent sur /obs/dock/<outil>, qui
// résout la session côté serveur et redirige vers le dock final. C'est ce qui
// évite toute configuration par streamer dans le plugin : une seule connexion
// Valerix dans n'importe quel dock (cookie manager PARTAGÉ et PERSISTANT) et
// les quatre docks fonctionnent, même après relance d'OBS.

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QMainWindow>

#include <vector>
#include <QDockWidget>

#include <browser-panel.hpp>

#include "vx-docks.hpp"

static QCef *cef = nullptr;
static QCefCookieManager *cookies = nullptr;
// Nos widgets CEF : gardés pour pouvoir fermer les navigateurs SYNCHRONEMENT
// sur EXIT — indispensable, cf. vx_destroy_docks.
static std::vector<QCefWidget *> widgets;

struct VxDock {
	const char *id;
	const char *title;
	const char *url;
};

// L'id devient l'objectName du QDockWidget (utile pour toggleViewAction).
static const VxDock DOCKS[] = {
	{"vx_chat", "VX Chat", "https://valerix.stream/obs/dock/chat"},
	{"vx_activity", "VX Activité", "https://valerix.stream/obs/dock/activity"},
	{"vx_music", "VX Musique", "https://valerix.stream/obs/dock/music"},
	{"vx_control", "VX Contrôle", "https://valerix.stream/obs/dock/control"},
};

bool vx_docks_available(void)
{
	return cef != nullptr;
}

// Accès au CEF et au cookie store partagés (dialog Scènes : même session que
// les docks — l'utilisateur connecté dans un dock l'est partout).
QCef *vx_get_cef(void)
{
	return cef;
}

QCefCookieManager *vx_get_cookies(void)
{
	return cookies;
}

bool vx_create_docks(void)
{
	cef = obs_browser_init_panel();
	if (!cef) {
		obs_log(LOG_WARNING, "obs-browser indisponible : docks natifs désactivés");
		return false;
	}
	cef->init_browser(); // async — create_widget attend tout seul que CEF soit prêt

	// Store de cookies dédié et persistant : la session Valerix survit aux
	// relances d'OBS, et elle est partagée par les 4 docks.
	cookies = cef->create_cookie_manager("vx-stream", true);

	for (const VxDock &d : DOCKS) {
		QCefWidget *w = cef->create_widget(nullptr, d.url, cookies);
		if (!w) {
			obs_log(LOG_WARNING, "dock %s : création CEF échouée", d.id);
			continue;
		}
		w->setMinimumSize(240, 200);
		widgets.push_back(w);
		obs_frontend_add_dock_by_id(d.id, d.title, w);
		obs_log(LOG_INFO, "dock natif ajouté : %s", d.title);
	}
	return true;
}

void vx_destroy_docks(void)
{
	if (!cef)
		return;
	// Appelé sur OBS_FRONTEND_EVENT_EXIT — le moment clé : CEF est encore
	// vivant. Deux crash logs de fermeture nous ont appris, dans l'ordre :
	//   1. laisser OBS détruire les docks avec la fenêtre (~OBSBasic) arrive
	//      APRÈS le déchargement d'obs-browser → crash (fix 0.5.2) ;
	//   2. remove_dock seul ne suffit PAS : la destruction Qt du dock (et donc
	//      le closeBrowser) reste différée (deleteLater) et retombe pendant
	//      l'arrêt de CEF → crash encore (log 0.6.1). D'où : fermer chaque
	//      navigateur EXPLICITEMENT et MAINTENANT, puis retirer les docks,
	//      puis détruire le cookie manager — plus aucune ressource CEF à nous
	//      quand obs-browser s'éteint.
	for (QCefWidget *w : widgets)
		w->closeBrowser();
	widgets.clear();

	size_t n = 0;
	const char *const *ids = vx_dock_ids(&n);
	for (size_t i = 0; i < n; i++)
		obs_frontend_remove_dock(ids[i]);

	delete cookies;
	cookies = nullptr;
	cef = nullptr;
	obs_log(LOG_INFO, "navigateurs CEF fermés et docks retirés avant l'arrêt de CEF");
}

QAction *vx_dock_toggle_action(const char *id)
{
	auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!window)
		return nullptr;
	// obs_frontend_add_dock_by_id enveloppe notre widget dans un QDockWidget
	// dont l'objectName est l'id : son toggleViewAction() est une case à
	// cocher native, toujours synchronisée avec l'état réel du dock.
	auto *dock = window->findChild<QDockWidget *>(QString::fromUtf8(id));
	return dock ? dock->toggleViewAction() : nullptr;
}

const char *const *vx_dock_ids(size_t *count)
{
	static const char *ids[] = {"vx_chat", "vx_activity", "vx_music", "vx_control"};
	*count = sizeof(ids) / sizeof(ids[0]);
	return ids;
}
