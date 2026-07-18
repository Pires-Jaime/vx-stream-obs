/*
VX.Stream pour OBS — fenêtre de gestion du multistream
Copyright (C) 2026 Valerix (Jaime Pires) <support@valerix.stream>
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <obs-module.h>
#include <obs-frontend-api.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <cctype>
#include <ctime>

#include "vx-multistream.hpp"
#include "vx-multistream-dialog.hpp"
#include "vx-ms-dock.hpp"
#include "vx-platform-logo.hpp"

namespace {

// Serveur prérempli par plateforme. Vide = l'utilisateur colle l'URL de son
// dashboard (Kick et TikTok fournissent une URL propre à chaque compte).
struct Preset {
	const char *label;
	const char *platform; // id du logo (vx_platform_logo)
	const char *server;
	const char *hint;
};
const Preset PRESETS[] = {
	{"YouTube", "youtube", "rtmp://a.rtmp.youtube.com/live2", "Clé : YouTube Studio → Diffusion en direct"},
	{"Kick", "kick", "", "Collez l'URL ET la clé depuis kick.com → Creator Dashboard → Stream key"},
	{"TikTok", "tiktok", "", "Nécessite l'accès RTMP TikTok Live (URL + clé fournies par TikTok)"},
	{"Twitch (2e compte)", "twitch", "rtmp://live.twitch.tv/app", "Clé : dashboard Twitch → Stream"},
	{"Autre (RTMP perso)", "custom", "", "Serveur RTMP quelconque"},
};

class EditDialog : public QDialog {
public:
	VxTarget target;

	explicit EditDialog(QWidget *parent, const VxTarget &t) : QDialog(parent), target(t)
	{
		setWindowTitle(t.id.empty() ? QStringLiteral("Ajouter une destination")
					    : QStringLiteral("Modifier la destination"));
		setMinimumWidth(460);
		auto *form = new QFormLayout();

		platform = new QComboBox(this);
		for (const Preset &p : PRESETS)
			platform->addItem(QIcon(vx_platform_logo(QString::fromUtf8(p.platform), 18)),
					  QString::fromUtf8(p.label));
		form->addRow(QStringLiteral("Plateforme"), platform);

		hint = new QLabel(this);
		hint->setWordWrap(true);
		hint->setStyleSheet(QStringLiteral("color: #999; font-size: 11px;"));
		form->addRow(QString(), hint);

		name = new QLineEdit(QString::fromStdString(t.name), this);
		name->setPlaceholderText(QStringLiteral("Nom affiché (ex : Kick)"));
		form->addRow(QStringLiteral("Nom"), name);

		server = new QLineEdit(QString::fromStdString(t.server), this);
		server->setPlaceholderText(QStringLiteral("rtmp(s)://…"));
		form->addRow(QStringLiteral("Serveur"), server);

		key = new QLineEdit(QString::fromStdString(t.key), this);
		key->setEchoMode(QLineEdit::Password);
		key->setPlaceholderText(QStringLiteral("Clé de stream"));
		form->addRow(QStringLiteral("Clé"), key);

		enabled =
			new QCheckBox(QStringLiteral("Diffuser sur cette destination (démarre avec le stream)"), this);
		enabled->setChecked(t.enabled);
		form->addRow(QString(), enabled);

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		connect(buttons, &QDialogButtonBox::accepted, this, &EditDialog::validate);
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

		auto *root = new QVBoxLayout(this);
		root->addLayout(form);
		root->addWidget(buttons);

		connect(platform, &QComboBox::currentIndexChanged, this, &EditDialog::applyPreset);
		// En édition : présélectionner la plateforme de la cible.
		int startIdx = 0;
		for (int i = 0; i < (int)(sizeof(PRESETS) / sizeof(PRESETS[0])); i++)
			if (t.platform == PRESETS[i].platform)
				startIdx = i;
		platform->setCurrentIndex(startIdx);
		applyPreset(startIdx);
		// En édition on garde les valeurs existantes.
		if (!t.name.empty())
			name->setText(QString::fromStdString(t.name));
		if (!t.server.empty())
			server->setText(QString::fromStdString(t.server));
	}

private:
	QComboBox *platform;
	QLabel *hint;
	QLineEdit *name;
	QLineEdit *server;
	QLineEdit *key;
	QCheckBox *enabled;

	void applyPreset(int idx)
	{
		if (idx < 0 || idx >= (int)(sizeof(PRESETS) / sizeof(PRESETS[0])))
			return;
		const Preset &p = PRESETS[idx];
		hint->setText(QString::fromUtf8(p.hint));
		if (name->text().isEmpty())
			name->setText(QString::fromUtf8(p.label));
		if (*p.server)
			server->setText(QString::fromUtf8(p.server));
	}

	void validate()
	{
		if (server->text().trimmed().isEmpty() || key->text().trimmed().isEmpty()) {
			QMessageBox::warning(this, QStringLiteral("Champs manquants"),
					     QStringLiteral("Le serveur et la clé sont obligatoires."));
			return;
		}
		target.name = name->text().trimmed().toStdString();
		if (target.name.empty())
			target.name = "Destination";
		const int idx = platform->currentIndex();
		target.platform = (idx >= 0 && idx < (int)(sizeof(PRESETS) / sizeof(PRESETS[0])))
					  ? PRESETS[idx].platform
					  : "custom";
		target.server = server->text().trimmed().toStdString();
		target.key = key->text().trimmed().toStdString();
		target.enabled = enabled->isChecked();
		if (target.id.empty())
			target.id = std::to_string((long long)time(nullptr));
		accept();
	}
};

class MsDialog : public QDialog {
public:
	explicit MsDialog(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle(QStringLiteral("VX.Stream — Multistream"));
		setMinimumSize(620, 340);

		table = new QTableWidget(0, 4, this);
		table->setHorizontalHeaderLabels({QStringLiteral("Nom"), QStringLiteral("Serveur"),
						  QStringLiteral("Actif"), QStringLiteral("État")});
		table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
		table->setSelectionBehavior(QAbstractItemView::SelectRows);
		table->setSelectionMode(QAbstractItemView::SingleSelection);
		table->setEditTriggers(QAbstractItemView::NoEditTriggers);
		table->verticalHeader()->setVisible(false);

		auto *add = new QPushButton(QStringLiteral("＋ Ajouter"), this);
		auto *edit = new QPushButton(QStringLiteral("Modifier"), this);
		auto *del = new QPushButton(QStringLiteral("Supprimer"), this);
		startBtn = new QPushButton(QStringLiteral("▶ Démarrer"), this);
		stopBtn = new QPushButton(QStringLiteral("⏹ Arrêter"), this);

		connect(add, &QPushButton::clicked, this, [this] { editTarget(VxTarget{}); });
		connect(edit, &QPushButton::clicked, this, [this] {
			const std::string id = selectedId();
			if (id.empty())
				return;
			for (const VxTarget &t : vx_ms_targets())
				if (t.id == id)
					editTarget(t);
		});
		connect(del, &QPushButton::clicked, this, [this] {
			const std::string id = selectedId();
			if (id.empty())
				return;
			if (QMessageBox::question(this, QStringLiteral("Supprimer"),
						  QStringLiteral("Supprimer cette destination ?")) ==
			    QMessageBox::Yes) {
				vx_ms_remove(id);
				refresh();
				vx_ms_dock_refresh();
			}
		});
		connect(startBtn, &QPushButton::clicked, this, [this] {
			const std::string id = selectedId();
			if (id.empty())
				return;
			std::string why;
			if (!vx_ms_start(id, &why))
				QMessageBox::warning(this, QStringLiteral("Impossible de démarrer"),
						     QString::fromStdString(why));
			refresh();
		});
		connect(stopBtn, &QPushButton::clicked, this, [this] {
			vx_ms_stop(selectedId());
			refresh();
		});

		auto *btnRow = new QHBoxLayout();
		btnRow->addWidget(add);
		btnRow->addWidget(edit);
		btnRow->addWidget(del);
		btnRow->addStretch(1);
		btnRow->addWidget(startBtn);
		btnRow->addWidget(stopBtn);

		auto *note = new QLabel(
			QStringLiteral("Les destinations réutilisent l'encodeur du stream principal : aucune charge "
				       "CPU en plus, même qualité partout. En revanche votre connexion doit fournir "
				       "l'upload de CHAQUE destination (bitrate × nombre). Les destinations « auto » "
				       "démarrent avec votre stream."),
			this);
		note->setWordWrap(true);
		note->setStyleSheet(QStringLiteral("color: #999; font-size: 11px;"));

		auto *root = new QVBoxLayout(this);
		root->addWidget(table);
		root->addLayout(btnRow);
		root->addWidget(note);

		auto *timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, &MsDialog::refreshStates);
		timer->start(2000);

		refresh();
	}

	void refresh()
	{
		const auto ts = vx_ms_targets();
		table->setRowCount((int)ts.size());
		int row = 0;
		for (const VxTarget &t : ts) {
			auto *n = new QTableWidgetItem(QString::fromStdString(t.name));
			n->setData(Qt::UserRole, QString::fromStdString(t.id));
			n->setIcon(QIcon(vx_platform_logo(QString::fromStdString(t.platform), 18)));
			table->setItem(row, 0, n);
			table->setItem(row, 1, new QTableWidgetItem(QString::fromStdString(t.server)));
			table->setItem(row, 2, new QTableWidgetItem(t.enabled ? QStringLiteral("✓") : QString()));
			table->setItem(row, 3, new QTableWidgetItem(QString()));
			row++;
		}
		refreshStates();
	}

private:
	QTableWidget *table;
	QPushButton *startBtn;
	QPushButton *stopBtn;

	std::string selectedId()
	{
		const int row = table->currentRow();
		if (row < 0)
			return {};
		QTableWidgetItem *it = table->item(row, 0);
		return it ? it->data(Qt::UserRole).toString().toStdString() : std::string{};
	}

	void refreshStates()
	{
		const bool mainLive = obs_frontend_streaming_active();
		for (int row = 0; row < table->rowCount(); row++) {
			QTableWidgetItem *n = table->item(row, 0);
			QTableWidgetItem *st = table->item(row, 3);
			if (!n || !st)
				continue;
			const std::string id = n->data(Qt::UserRole).toString().toStdString();
			if (vx_ms_active(id)) {
				st->setText(QStringLiteral("🔴 En direct"));
			} else {
				st->setText(mainLive ? QStringLiteral("Arrêté")
						     : QStringLiteral("En attente du stream"));
			}
		}
	}

	void editTarget(const VxTarget &t)
	{
		EditDialog d(this, t);
		if (d.exec() == QDialog::Accepted) {
			vx_ms_upsert(d.target);
			refresh();
			vx_ms_dock_refresh();
		}
	}
};

MsDialog *dialog = nullptr;

} // namespace

void vx_ms_show_dialog(void)
{
	if (!dialog) {
		auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		dialog = new MsDialog(window);
	}
	dialog->refresh();
	dialog->show();
	dialog->raise();
	dialog->activateWindow();
}
