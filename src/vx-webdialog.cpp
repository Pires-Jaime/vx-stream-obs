/*
VX.Stream pour OBS — dialog web générique
Copyright (C) 2026 Valerix (Jaime Pires) <support@valerix.stream>
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Fenêtre web réutilisable (compte, émulateur d'événements, signalement…) :
// même pattern CEF que le dialog Scènes. Elle ÉCOUTE le titre de la page —
// seul canal page → plugin exposé par browser-panel.hpp — pour que nos pages
// puissent remonter leur état (connexion) et demander la fermeture après un
// login, sans laisser le streamer devant une page inutile.

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QMainWindow>
#include <QTimer>
#include <QVBoxLayout>

#include <browser-panel.hpp>

#include "vx-webdialog.hpp"

extern QCef *vx_get_cef(void);
extern QCefCookieManager *vx_get_cookies(void);

namespace {
std::function<void(const QString &)> titleHook;
}

// Q_OBJECT requis : la connexion vers titleChanged se fait en syntaxe
// SIGNAL/SLOT (chaînes, résolution runtime) — la syntaxe moderne par pointeur
// référencerait le symbole du signal, défini dans le moc d'obs-browser.dll et
// non exporté → erreur de link. (Même contrainte que vx-scenes.)
class WebDialog : public QDialog {
	Q_OBJECT
public:
	WebDialog(QWidget *parent, const char *title, const char *url, int w, int h) : QDialog(parent)
	{
		setWindowTitle(QString::fromUtf8(title));
		resize(w, h);
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(0, 0, 0, 0);

		QCef *cef = vx_get_cef();
		if (!cef) {
			auto *l = new QLabel(QStringLiteral("obs-browser indisponible."), this);
			l->setMargin(14);
			root->addWidget(l);
			return;
		}
		web = cef->create_widget(this, url, vx_get_cookies());
		root->addWidget(web, 1);
		connect(web, SIGNAL(titleChanged(QString)), this, SLOT(onTitle(QString)));
	}

	void closeWeb()
	{
		// Fermeture + destruction SYNCHRONES pendant que CEF vit encore
		// (un deleteLater retomberait après l'arrêt de CEF → crash exit).
		if (web) {
			web->closeBrowser();
			delete web;
			web = nullptr;
		}
	}

private slots:
	void onTitle(const QString &t)
	{
		if (titleHook)
			titleHook(t);
	}

private:
	QCefWidget *web = nullptr;
};

namespace {
WebDialog *dlg = nullptr;
}

void vx_webdialog_set_title_hook(std::function<void(const QString &)> cb)
{
	titleHook = std::move(cb);
}

void vx_webdialog_open(const char *title, const char *url, int w, int h)
{
	vx_webdialog_shutdown(); // un seul dialog — remplace l'éventuel précédent
	auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	dlg = new WebDialog(window, title, url, w, h);
	dlg->show();
	dlg->raise();
	dlg->activateWindow();
}

void vx_webdialog_close(void)
{
	// Différé d'un tour de boucle : on est appelé DEPUIS un signal du widget
	// CEF (titleChanged) — le détruire pendant son propre callback planterait.
	// Le closeWeb() reste synchrone une fois la pile déroulée.
	QTimer::singleShot(0, qApp ? static_cast<QObject *>(qApp) : nullptr, [] { vx_webdialog_shutdown(); });
}

void vx_webdialog_shutdown(void)
{
	if (dlg) {
		dlg->closeWeb();
		delete dlg; // synchrone — même règle que les docks CEF
		dlg = nullptr;
	}
}

#include "vx-webdialog.moc"
