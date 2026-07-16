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

// Étape 0 — validation de la chaîne de build, volontairement minimale.
// Objectif unique : prouver que la DLL charge dans OBS 32.x et qu'un menu
// « VX.Stream » apparaît dans la barre de menus, à côté de « Aide ».
// Aucune logique métier ici : si ce menu s'affiche, la toolchain (MSVC + Qt de
// l'ABI d'OBS + libobs) est validée et tout le reste n'est plus que du code.

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QMainWindow>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QDesktopServices>
#include <QUrl>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static void open_url(const char *url)
{
	QDesktopServices::openUrl(QUrl(QString::fromUtf8(url)));
}

static void add_vx_menu(void)
{
	// La fenêtre principale d'OBS est une QMainWindow : sa menuBar() est la
	// barre Fichier … Aide. Qt ajoute le nouveau menu en dernier, donc à
	// droite de « Aide » — exactement l'emplacement demandé.
	auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!window) {
		obs_log(LOG_WARNING, "fenêtre principale introuvable, menu non ajouté");
		return;
	}

	QMenu *menu = window->menuBar()->addMenu(QStringLiteral("VX.Stream"));

	QAction *docks = menu->addAction(QStringLiteral("Configurer mes docks…"));
	QObject::connect(docks, &QAction::triggered, [] { open_url("https://valerix.stream/obs"); });

	QAction *dash = menu->addAction(QStringLiteral("Ouvrir mon dashboard Valerix…"));
	QObject::connect(dash, &QAction::triggered, [] { open_url("https://valerix.stream/dashboard"); });

	menu->addSeparator();

	QAction *about = menu->addAction(QStringLiteral("VX.Stream v%1 — étape 0").arg(PLUGIN_VERSION));
	about->setEnabled(false);

	obs_log(LOG_INFO, "menu VX.Stream ajouté à la barre de menus");
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	// On attend la fin du chargement de l'UI : ajouter le menu pendant que la
	// fenêtre se construit encore est le crash classique des plugins frontend.
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		add_vx_menu();
}

bool obs_module_load(void)
{
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	obs_log(LOG_INFO, "VX.Stream chargé (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "VX.Stream déchargé");
}
