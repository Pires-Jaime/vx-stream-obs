/*
VX.Stream pour OBS — création de scènes Valerix
Copyright (C) 2026 Valerix (Jaime Pires) <support@valerix.stream>
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Le dialog embarque un QCefWidget (session PARTAGÉE avec les docks : si le
// streamer s'est connecté dans un dock, il l'est ici) pointant sur
// /obs/scene-setup. La page publie la liste de ses overlays dans
// document.title (préfixe VXSETUP:, base64url) — le seul canal page→plugin
// qu'expose browser-panel.hpp est le signal titleChanged, et il suffit :
// l'utilisateur ne copie rien.
//
// À la validation : une scène par overlay coché, contenant une source
// navigateur plein cadre (l'overlay du streamer, avec SON token). L'UI d'OBS
// référence les scènes dès leur création (signal source_create) — on relâche
// nos refs aussitôt.

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QByteArray>
#include <QCheckBox>
#include <QDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <browser-panel.hpp>

#include "vx-scenes.hpp"

extern QCef *vx_get_cef(void);
extern QCefCookieManager *vx_get_cookies(void);

namespace {
// (uniquement l'état du module — la classe Q_OBJECT vit au scope fichier :
// le moc ne traite pas les classes dans un namespace anonyme)
}

struct OverlayInfo {
	QString name;
	QString token;
	int width = 1920;
	int height = 1080;
	int widgets = 0;
};

class ScenesDialog : public QDialog {
	// Q_OBJECT requis : le connect vers titleChanged se fait en syntaxe
	// SIGNAL/SLOT (chaînes, résolution runtime) — la syntaxe moderne par
	// pointeur référencerait le symbole du signal, défini dans le moc
	// d'obs-browser.dll et non exporté → erreur de link.
	Q_OBJECT
public:
	explicit ScenesDialog(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle(QStringLiteral("VX.Stream — Ajouter mes overlays"));
		setMinimumSize(520, 460);

		root = new QVBoxLayout(this);

		QCef *cef = vx_get_cef();
		if (!cef) {
			auto *l = new QLabel(QStringLiteral("obs-browser indisponible."), this);
			root->addWidget(l);
			return;
		}
		web = cef->create_widget(this, "https://valerix.stream/obs/scene-setup", vx_get_cookies());
		web->setMinimumHeight(180);
		root->addWidget(web, 1);

		listBox = new QVBoxLayout();
		root->addLayout(listBox);

		auto *btnRow = new QHBoxLayout();
		createBtn = new QPushButton(QStringLiteral("Ajouter à la scène actuelle"), this);
		createBtn->setEnabled(false);
		connect(createBtn, &QPushButton::clicked, this, &ScenesDialog::createScenes);
		btnRow->addStretch(1);
		btnRow->addWidget(createBtn);
		root->addLayout(btnRow);

		// Le canal page → plugin : la page écrit VXSETUP:<base64url> dans son
		// titre ; CEF relaie via titleChanged (connexion par chaînes, cf. Q_OBJECT).
		connect(web, SIGNAL(titleChanged(QString)), this, SLOT(onTitle(QString)));
	}

	void closeWeb()
	{
		// Détruire le navigateur PENDANT que CEF est vivant (même règle que
		// les docks — cf. le crash de fermeture corrigé en 0.5.2).
		if (web) {
			web->closeBrowser();
			web->deleteLater();
			web = nullptr;
		}
	}

private:
	QVBoxLayout *root = nullptr;
	QVBoxLayout *listBox = nullptr;
	QCefWidget *web = nullptr;
	QPushButton *createBtn = nullptr;
	std::vector<std::pair<QCheckBox *, OverlayInfo>> rows;

private slots:
	void onTitle(const QString &title)
	{
		if (!title.startsWith(QStringLiteral("VXSETUP:")))
			return;
		const QByteArray json = QByteArray::fromBase64(title.mid(8).toUtf8(), QByteArray::Base64UrlEncoding);
		const QJsonObject o = QJsonDocument::fromJson(json).object();

		// Purge de la liste précédente (reconnexion, refresh…).
		for (auto &r : rows)
			r.first->deleteLater();
		rows.clear();

		const QJsonArray arr = o.value(QStringLiteral("overlays")).toArray();
		// Par VALEUR : l'itérateur de QJsonArray produit un proxy temporaire
		// (QJsonValueConstRef) — une référence dessus = -Wrange-loop-bind-reference
		// promu en erreur chez AppleClang (seule plateforme à l'activer).
		for (const auto v : arr) {
			const QJsonObject ov = v.toObject();
			OverlayInfo info;
			info.name = ov.value(QStringLiteral("name")).toString(QStringLiteral("Overlay"));
			info.token = ov.value(QStringLiteral("token")).toString();
			info.width = ov.value(QStringLiteral("width")).toInt(1920);
			info.height = ov.value(QStringLiteral("height")).toInt(1080);
			info.widgets = ov.value(QStringLiteral("widgets")).toInt(0);
			if (info.token.isEmpty())
				continue;
			auto *cb = new QCheckBox(QStringLiteral("%1  (%2×%3, %4 widget%5)")
							 .arg(info.name)
							 .arg(info.width)
							 .arg(info.height)
							 .arg(info.widgets)
							 .arg(info.widgets > 1 ? "s" : ""),
						 this);
			cb->setChecked(true);
			listBox->addWidget(cb);
			rows.push_back({cb, info});
		}
		createBtn->setEnabled(!rows.empty());
	}

private:
	// Ajout à la SCÈNE COURANTE (retour G le Point) : le streamer a déjà ses
	// scènes — l'overlay doit devenir une SOURCE dedans, pas une scène à part.
	void createScenes()
	{
		obs_source_t *sceneSrc = obs_frontend_get_current_scene();
		obs_scene_t *scene = sceneSrc ? obs_scene_from_source(sceneSrc) : nullptr;
		if (!scene) {
			if (sceneSrc)
				obs_source_release(sceneSrc);
			QMessageBox::warning(this, QStringLiteral("VX.Stream"),
					     QStringLiteral("Aucune scène active dans OBS."));
			return;
		}

		int created = 0;
		for (auto &r : rows) {
			if (!r.first->isChecked())
				continue;
			const OverlayInfo &ov = r.second;

			obs_data_t *ss = obs_data_create();
			const QString url = QStringLiteral("https://valerix.stream/overlay/%1").arg(ov.token);
			obs_data_set_string(ss, "url", url.toUtf8().constData());
			obs_data_set_int(ss, "width", ov.width);
			obs_data_set_int(ss, "height", ov.height);
			// L'overlay coupe son rendu quand il n'est pas affiché.
			obs_data_set_bool(ss, "shutdown", true);

			const QString srcName = QStringLiteral("Overlay Valerix — %1").arg(ov.name);
			obs_source_t *src =
				obs_source_create("browser_source", srcName.toUtf8().constData(), ss, nullptr);
			obs_data_release(ss);

			if (src) {
				obs_scene_add(scene, src);
				obs_source_release(src);
				created++;
			}
		}
		obs_source_release(sceneSrc);

		QMessageBox::information(this, QStringLiteral("VX.Stream"),
					 created ? QStringLiteral("%1 source%2 ajoutée%2 à la scène actuelle.")
							   .arg(created)
							   .arg(created > 1 ? "s" : "")
						 : QStringLiteral("Aucune source ajoutée."));
		if (created)
			accept();
	}
};

namespace {
ScenesDialog *dialog = nullptr;
} // namespace

void vx_scenes_show_dialog(void)
{
	if (!dialog) {
		auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		dialog = new ScenesDialog(window);
	}
	dialog->show();
	dialog->raise();
	dialog->activateWindow();
}

void vx_scenes_shutdown(void)
{
	if (dialog) {
		dialog->closeWeb();
		dialog->deleteLater();
		dialog = nullptr;
	}
}

#include "vx-scenes.moc"
