/*
VX.Stream pour OBS — dialog web générique
Copyright (C) 2026 Valerix (Jaime Pires) <support@valerix.stream>
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Fenêtre web réutilisable (Émulateur d'événements, Signaler un problème…) :
// même pattern CEF que le dialog Scènes, sans écoute de titre — la page fait
// tout côté web (session partagée avec les docks). Pas de Q_OBJECT : aucun
// signal custom, donc pas de moc.

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QDialog>
#include <QLabel>
#include <QMainWindow>
#include <QVBoxLayout>

#include <browser-panel.hpp>

#include "vx-webdialog.hpp"

extern QCef *vx_get_cef(void);
extern QCefCookieManager *vx_get_cookies(void);

namespace {

class WebDialog : public QDialog {
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

private:
	QCefWidget *web = nullptr;
};

WebDialog *dlg = nullptr;

} // namespace

void vx_webdialog_open(const char *title, const char *url, int w, int h)
{
	vx_webdialog_shutdown(); // un seul dialog — remplace l'éventuel précédent
	auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	dlg = new WebDialog(window, title, url, w, h);
	dlg->show();
	dlg->raise();
	dlg->activateWindow();
}

void vx_webdialog_shutdown(void)
{
	if (dlg) {
		dlg->closeWeb();
		delete dlg; // synchrone — même règle que les docks CEF
		dlg = nullptr;
	}
}
