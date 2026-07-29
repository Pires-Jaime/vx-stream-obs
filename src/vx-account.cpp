/*
VX.Stream pour OBS — compte Valerix (connexion / déconnexion)
Copyright (C) 2026 Valerix (Jaime Pires) <support@valerix.stream>
SPDX-License-Identifier: GPL-2.0-or-later
*/

// La session Valerix vit dans le cookie `sf_session` du gestionnaire de cookies
// CEF partagé par tous les docks — le C++ n'y accède qu'indirectement :
//   • lecture  : QCefCookieManager::CheckForCookie (asynchrone, callback sur un
//                thread CEF → il FAUT repasser par le thread UI pour Qt) ;
//   • purge    : DeleteCookies + FlushStore.
//
// Connexion   → dialog web /obs/account (bouton Twitch ; le cookie posé y est
//               celui des docks, donc ils sont connectés dans la foulée).
// Déconnexion → /obs/account?logout=1 (invalide la session CÔTÉ SERVEUR) ET
//               purge du cookie CEF : sans les deux, soit le jeton resterait
//               valide, soit les docks resteraient connectés localement.

#include <obs-module.h>
#include <plugin-support.h>

#include <QAction>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>

#include <browser-panel.hpp>

#include "vx-account.hpp"
#include "vx-webdialog.hpp"

extern QCefCookieManager *vx_get_cookies(void);

namespace {

const char *SITE = "https://valerix.stream";
const char *SESSION_COOKIE = "sf_session";

// Dernier état connu — sert d'affichage immédiat à l'ouverture du menu, le
// temps que la vérification asynchrone réponde (elle rafraîchira les libellés).
bool lastKnownLogged = false;

void refresh_actions(QAction *login, QAction *logout, bool logged)
{
	lastKnownLogged = logged;
	login->setText(logged ? QStringLiteral("👤 Mon compte Valerix…")
			      : QStringLiteral("🔑 Se connecter à Valerix…"));
	logout->setEnabled(logged);
}

} // namespace

void vx_account_add_menu(QMenu *menu)
{
	QMenu *account = menu->addMenu(QStringLiteral("Compte"));

	QAction *login = account->addAction(QStringLiteral("🔑 Se connecter à Valerix…"));
	QObject::connect(login, &QAction::triggered, [] {
		vx_webdialog_open("VX.Stream — Mon compte", "https://valerix.stream/obs/account", 460, 520);
	});

	QAction *logout = account->addAction(QStringLiteral("🚪 Se déconnecter"));
	logout->setEnabled(false); // activé dès qu'une session est détectée
	QObject::connect(logout, &QAction::triggered, [menu] {
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
		lastKnownLogged = false;
		obs_log(LOG_INFO, "compte : déconnexion demandée");
	});

	// À chaque ouverture du menu : relecture réelle du cookie (l'utilisateur a
	// pu se connecter dans un dock entre-temps).
	QObject::connect(account, &QMenu::aboutToShow, [login, logout] {
		refresh_actions(login, logout, lastKnownLogged); // affichage immédiat
		QCefCookieManager *cm = vx_get_cookies();
		if (!cm)
			return;
		cm->CheckForCookie(SITE, SESSION_COOKIE, [login, logout](bool exists) {
			// Callback sur un thread CEF → Qt exige le thread UI.
			QMetaObject::invokeMethod(
				login, [login, logout, exists] { refresh_actions(login, logout, exists); },
				Qt::QueuedConnection);
		});
	});
}
