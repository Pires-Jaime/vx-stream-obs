/*
VX.Stream pour OBS
Copyright (C) 2026 Valerix (Jaime Pires) <support@valerix.stream>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

// Étape 1 (palier B) : le plugin devient autonome —
//   • docks navigateur créés NATIVEMENT (via le CEF d'obs-browser), plus
//     besoin de l'installeur qui écrivait ExtraBrowserDocks dans user.ini ;
//   • thème « Valerix » copié dans les thèmes utilisateur ;
//   • menu VX.Stream : cases afficher/masquer par dock + raccourcis web.

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QMainWindow>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>

#include "vx-docks.hpp"
#include "vx-multistream-dialog.hpp"
#include "vx-multistream.hpp"
#include "vx-theme.hpp"
#include "vx-updater.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static bool themeInstalled = false;

static void open_url(const char *url)
{
	QDesktopServices::openUrl(QUrl(QString::fromUtf8(url)));
}

static void add_vx_menu(void)
{
	auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!window) {
		obs_log(LOG_WARNING, "fenêtre principale introuvable, menu non ajouté");
		return;
	}

	QMenu *menu = window->menuBar()->addMenu(QStringLiteral("VX.Stream"));

	// ── Docks : cases à cocher natives (toggleViewAction, toujours synchro) ──
	if (vx_docks_available()) {
		size_t n = 0;
		const char *const *ids = vx_dock_ids(&n);
		for (size_t i = 0; i < n; i++) {
			QAction *a = vx_dock_toggle_action(ids[i]);
			if (a)
				menu->addAction(a);
		}
	} else {
		QAction *warn = menu->addAction(QStringLiteral("⚠ Docks indisponibles (obs-browser manquant)"));
		warn->setEnabled(false);
	}

	menu->addSeparator();

	QAction *theme = menu->addAction(QStringLiteral("Activer le thème Valerix…"));
	QObject::connect(theme, &QAction::triggered, [window] {
		QMessageBox::information(
			window, QStringLiteral("Thème Valerix"),
			themeInstalled
				? QStringLiteral(
					  "Le thème est installé.\n\nParamètres → Apparence → Thème : « Valerix »,\npuis appliquez.")
				: QStringLiteral(
					  "Le thème n'a pas pu être installé.\nConsultez le journal OBS (vx-stream)."));
	});

	menu->addSeparator();

	QAction *ms = menu->addAction(QStringLiteral("Multistream…"));
	QObject::connect(ms, &QAction::triggered, [] { vx_ms_show_dialog(); });

	menu->addSeparator();

	QAction *docksCfg = menu->addAction(QStringLiteral("Configurer mes docks…"));
	QObject::connect(docksCfg, &QAction::triggered, [] { open_url("https://valerix.stream/obs"); });

	QAction *dash = menu->addAction(QStringLiteral("Ouvrir mon dashboard Valerix…"));
	QObject::connect(dash, &QAction::triggered, [] { open_url("https://valerix.stream/dashboard"); });

	menu->addSeparator();

	QAction *about = menu->addAction(QStringLiteral("VX.Stream v%1").arg(PLUGIN_VERSION));
	about->setEnabled(false);

	obs_log(LOG_INFO, "menu VX.Stream ajouté à la barre de menus");

	// Vérification de mise à jour en arrière-plan (jamais bloquant).
	vx_updater_check(menu);
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	// FINISHED_LOADING : l'UI est construite. Ordre importe — les docks
	// d'abord (le menu récupère leurs toggleViewAction).
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		vx_create_docks();
		themeInstalled = vx_install_theme();
		add_vx_menu();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		vx_ms_on_streaming_started();
		break;
	// STOPPING (pas STOPPED) : nos sorties partagent les encodeurs du
	// stream principal, elles doivent lâcher prise avant qu'il ne finisse.
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		vx_ms_on_streaming_stopping();
		break;
	default:
		break;
	}
}

bool obs_module_load(void)
{
	vx_ms_load();
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	obs_log(LOG_INFO, "VX.Stream chargé (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	vx_ms_shutdown();
	obs_log(LOG_INFO, "VX.Stream déchargé");
}
