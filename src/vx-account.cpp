/*
VX.Stream pour OBS — compte Valerix (connexion / déconnexion)
Copyright (C) 2026 Valerix (Jaime Pires) <support@valerix.stream>
SPDX-License-Identifier: GPL-2.0-or-later
*/

// La session Valerix vit dans le CEF partagé par les docks ; le C++ ne peut pas
// la lire directement. `QCefCookieManager::CheckForCookie` s'est révélé peu
// fiable (le bouton « Se déconnecter » restait grisé même connecté) : la SOURCE
// DE VÉRITÉ est donc la PAGE `/obs/account`, qui publie son état dans son titre
// (« VXAUTH:1 » / « VXAUTH:0 ») — seul canal page → plugin de browser-panel.hpp.
// L'état est mémorisé dans la config du module pour que le menu soit correct dès
// le lancement d'OBS, avant toute ouverture de fenêtre.
//
// Connexion   → /obs/account (bouton Twitch). La page ajoute « CLOSE » à son
//               titre quand elle vient d'un login : la fenêtre se referme seule,
//               sans laisser le streamer devant le dashboard.
// Déconnexion → /obs/account?logout=1 (invalide la session CÔTÉ SERVEUR) ET
//               purge du cookie CEF : sans les deux, soit le jeton resterait
//               valide, soit les docks resteraient connectés localement.

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QAction>
#include <QMenu>
#include <QMessageBox>

#include <browser-panel.hpp>

#include <string>

#include "vx-account.hpp"
#include "vx-webdialog.hpp"

extern QCefCookieManager *vx_get_cookies(void);

namespace {

const char *SITE = "https://valerix.stream";
const char *SESSION_COOKIE = "sf_session";
const char *ACCOUNT_URL = "https://valerix.stream/obs/account";
const char *STATE_FILE = "account-logged.txt";

QAction *g_login = nullptr;
QAction *g_logout = nullptr;
bool g_logged = false;

std::string state_path()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *p = obs_module_config_path(STATE_FILE);
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

bool read_state()
{
	char *c = os_quick_read_utf8_file(state_path().c_str());
	const bool v = c && c[0] == '1';
	bfree(c);
	return v;
}

void write_state(bool v)
{
	const char *s = v ? "1" : "0";
	os_quick_write_utf8_file(state_path().c_str(), s, 1, false);
}

// Reflète l'état dans le menu : un seul bouton pertinent à la fois.
void apply_state(bool logged)
{
	g_logged = logged;
	if (g_login) {
		g_login->setText(logged ? QStringLiteral("👤 Mon compte Valerix…")
					: QStringLiteral("🔑 Se connecter à Valerix…"));
	}
	if (g_logout)
		g_logout->setVisible(logged); // masqué (pas grisé) quand déconnecté
}

} // namespace

bool vx_account_is_logged(void)
{
	return g_logged;
}

void vx_account_open(void)
{
	vx_webdialog_open("VX.Stream — Mon compte", ACCOUNT_URL, 460, 560);
}

void vx_account_add_menu(QMenu *menu)
{
	QMenu *account = menu->addMenu(QStringLiteral("Compte"));

	g_login = account->addAction(QStringLiteral("🔑 Se connecter à Valerix…"));
	QObject::connect(g_login, &QAction::triggered, [] { vx_account_open(); });

	g_logout = account->addAction(QStringLiteral("🚪 Se déconnecter"));
	QObject::connect(g_logout, &QAction::triggered, [menu] {
		if (QMessageBox::question(menu->parentWidget(), QStringLiteral("VX.Stream"),
					  QStringLiteral("Se déconnecter de Valerix ?\n\nVos docks VX.Stream "
							 "demanderont une nouvelle connexion.")) != QMessageBox::Yes)
			return;

		// 1) session invalidée côté serveur par la page ; 2) cookie purgé du
		// CEF pour que les docks déjà ouverts ne gardent pas la session.
		vx_webdialog_open("VX.Stream — Déconnexion", "https://valerix.stream/obs/account?logout=1", 460, 420);
		if (QCefCookieManager *cm = vx_get_cookies()) {
			cm->DeleteCookies(SITE, SESSION_COOKIE);
			cm->FlushStore();
		}
		apply_state(false);
		write_state(false);
		obs_log(LOG_INFO, "compte : déconnexion demandée");
	});

	// État persisté : le menu est juste dès l'ouverture d'OBS.
	apply_state(read_state());

	// La page nous dit l'état réel (et demande la fermeture après un login).
	vx_webdialog_set_title_hook([](const QString &t) {
		if (!t.startsWith(QStringLiteral("VXAUTH:")))
			return;
		const QString rest = t.mid(7);
		const bool logged = rest.startsWith(QLatin1Char('1'));
		if (logged != g_logged) {
			apply_state(logged);
			write_state(logged);
			obs_log(LOG_INFO, "compte : %s", logged ? "connecté" : "déconnecté");
		}
		// « VXAUTH:1CLOSE » → on sort d'un login (ou d'une déconnexion) : la
		// fenêtre a fini son office, on la referme au lieu d'afficher le site.
		if (rest.contains(QStringLiteral("CLOSE")))
			vx_webdialog_close();
	});
}

void vx_account_require_login(void)
{
	if (g_logged)
		return;
	// Connexion OBLIGATOIRE avant d'utiliser le plugin : sans session, les docks
	// n'affichent qu'un écran de connexion — autant l'ouvrir tout de suite.
	obs_log(LOG_INFO, "compte : aucune session connue, ouverture de la connexion");
	vx_account_open();
}
