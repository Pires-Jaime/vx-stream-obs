/*
VX.Stream pour OBS — dock natif Multistream
Copyright (C) 2026 Valerix (Jaime Pires) <support@valerix.stream>
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Le tableau de bord du multistream, toujours sous les yeux : une ligne par
// plateforme (logo + nom + état) et un interrupteur pour dire « celle-ci
// diffuse / celle-ci non ». Le toggle agit à chaud : en plein live, l'activer
// démarre la sortie, le décocher la coupe (vx_ms_set_enabled).
//
// Widget Qt pur (AUCUN CEF) : les règles de destruction synchrone des docks
// navigateur ne s'appliquent pas ici — un simple remove_dock à l'EXIT suffit.

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QAbstractButton>
#include <QFrame>
#include <QPaintEvent>
#include <QPainter>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <string>
#include <vector>

#include "vx-multistream.hpp"
#include "vx-multistream-dialog.hpp"
#include "vx-platform-logo.hpp"
#include "vx-ms-dock.hpp"

namespace {

constexpr const char *DOCK_ID = "vx_multistream";

// Interrupteur peint (Qt n'a pas de « switch » natif) : piste arrondie + pastille
// qui glisse. Réutilise le signal `toggled` de QAbstractButton (aucun nouveau
// membre méta → pas de Q_OBJECT nécessaire).
class ToggleSwitch : public QAbstractButton {
public:
	explicit ToggleSwitch(QWidget *parent = nullptr) : QAbstractButton(parent)
	{
		setCheckable(true);
		setCursor(Qt::PointingHandCursor);
		setFixedSize(38, 20);
	}
	QSize sizeHint() const override { return QSize(38, 20); }

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing);
		const bool on = isChecked();
		p.setPen(Qt::NoPen);
		p.setBrush(on ? QColor("#7c3aed") : QColor("#3a3a44"));
		const int r = height() / 2;
		p.drawRoundedRect(rect(), r, r);
		p.setBrush(Qt::white);
		const int d = height() - 4;
		p.drawEllipse(on ? width() - d - 2 : 2, 2, d, d);
	}
};

class MsDockWidget : public QWidget {
public:
	MsDockWidget()
	{
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(8, 8, 8, 8);
		root->setSpacing(6);

		auto *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		listHost = new QWidget(scroll);
		listLayout = new QVBoxLayout(listHost);
		listLayout->setContentsMargins(0, 0, 0, 0);
		listLayout->setSpacing(4);
		listLayout->addStretch(1);
		scroll->setWidget(listHost);
		root->addWidget(scroll, 1);

		auto *btnRow = new QHBoxLayout();
		auto *manage = new QPushButton(QStringLiteral("＋ Gérer les destinations…"), this);
		manage->setCursor(Qt::PointingHandCursor);
		connect(manage, &QPushButton::clicked, this, [] { vx_ms_show_dialog(); });
		btnRow->addWidget(manage);
		root->addLayout(btnRow);

		auto *timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, [this] { refresh(); });
		timer->start(2000);

		refresh();
	}

	void refresh()
	{
		const auto ts = vx_ms_targets();

		// Reconstruire les lignes seulement si la composition a changé — un
		// rebuild toutes les 2 s ferait clignoter les toggles sous la souris.
		std::string sig;
		for (const VxTarget &t : ts)
			sig += t.id + '|' + t.name + '|' + t.platform + '|' + t.canvas + '|' + (t.enabled ? '1' : '0') +
			       ';';
		if (sig != lastSig) {
			lastSig = sig;
			rebuild(ts);
		}
		updateStates(ts);
	}

private:
	struct Row {
		std::string id;
		QLabel *status = nullptr;
		ToggleSwitch *toggle = nullptr;
	};

	QWidget *listHost = nullptr;
	QVBoxLayout *listLayout = nullptr;
	QLabel *empty = nullptr;
	std::vector<Row> rows;
	std::string lastSig;

	void clearList()
	{
		rows.clear();
		empty = nullptr;
		// Tout sauf le stretch final.
		while (listLayout->count() > 1) {
			QLayoutItem *it = listLayout->takeAt(0);
			if (it->widget())
				it->widget()->deleteLater();
			delete it;
		}
	}

	void rebuild(const std::vector<VxTarget> &ts)
	{
		clearList();

		if (ts.empty()) {
			empty = new QLabel(QStringLiteral("Aucune destination de multistream.\n"
							  "Ajoutez Kick, YouTube, TikTok…\n"
							  "via « Gérer les destinations »."),
					   listHost);
			empty->setAlignment(Qt::AlignCenter);
			empty->setWordWrap(true);
			empty->setStyleSheet(QStringLiteral("color: #888; font-size: 11px; padding: 14px 4px;"));
			listLayout->insertWidget(0, empty);
			return;
		}

		int pos = 0;
		for (const VxTarget &t : ts) {
			auto *card = new QFrame(listHost);
			card->setStyleSheet(QStringLiteral(
				"QFrame { background: rgba(124,58,237,0.07); border: 1px solid rgba(124,58,237,0.25); "
				"border-radius: 8px; } QLabel { background: transparent; border: none; }"));
			auto *h = new QHBoxLayout(card);
			h->setContentsMargins(8, 6, 8, 6);
			h->setSpacing(8);

			auto *logo = new QLabel(card);
			logo->setPixmap(vx_platform_logo(QString::fromStdString(t.platform), 26));
			logo->setFixedSize(26, 26);
			h->addWidget(logo);

			auto *col = new QVBoxLayout();
			col->setSpacing(0);
			// Nom + badge de format (📱 vertical / 🖥 desktop) sur la même ligne.
			auto *nameRow = new QHBoxLayout();
			nameRow->setSpacing(5);
			auto *name = new QLabel(QString::fromStdString(t.name), card);
			name->setStyleSheet(QStringLiteral("font-weight: 600; font-size: 12px;"));
			const bool vertical = vx_target_is_vertical(t);
			auto *badge =
				new QLabel(vertical ? QStringLiteral("📱 9:16") : QStringLiteral("🖥 16:9"), card);
			badge->setToolTip(vertical ? QStringLiteral("Diffuse le canvas vertical (VX Vertical)")
						   : QStringLiteral("Diffuse l'image du stream principal"));
			badge->setStyleSheet(QStringLiteral("color: #9a8cc7; font-size: 9px; font-weight: 700;"));
			nameRow->addWidget(name);
			nameRow->addWidget(badge);
			nameRow->addStretch(1);
			auto *status = new QLabel(card);
			status->setStyleSheet(QStringLiteral("color: #888; font-size: 10px;"));
			col->addLayout(nameRow);
			col->addWidget(status);
			h->addLayout(col, 1);

			auto *toggle = new ToggleSwitch(card);
			toggle->setChecked(t.enabled);
			toggle->setToolTip(QStringLiteral("Diffuser sur cette plateforme (bascule à chaud en live)"));
			const std::string id = t.id;
			connect(toggle, &ToggleSwitch::toggled, this, [this, id](bool on) {
				vx_ms_set_enabled(id, on);
				refresh();
			});
			h->addWidget(toggle);

			listLayout->insertWidget(pos++, card);
			rows.push_back({t.id, status, toggle});
		}
	}

	void updateStates(const std::vector<VxTarget> &ts)
	{
		const bool mainLive = obs_frontend_streaming_active();
		for (Row &r : rows) {
			const VxTarget *t = nullptr;
			for (const VxTarget &e : ts)
				if (e.id == r.id)
					t = &e;
			if (!t)
				continue;
			if (vx_ms_active(r.id)) {
				r.status->setText(QStringLiteral("🔴 En direct"));
				r.status->setStyleSheet(
					QStringLiteral("color: #ef4444; font-size: 10px; font-weight: 600;"));
			} else if (!t->enabled) {
				r.status->setText(QStringLiteral("Désactivé"));
				r.status->setStyleSheet(QStringLiteral("color: #777; font-size: 10px;"));
			} else {
				r.status->setText(mainLive ? QStringLiteral("Démarrage…")
							   : QStringLiteral("Prêt — attend le stream"));
				r.status->setStyleSheet(QStringLiteral("color: #888; font-size: 10px;"));
			}
		}
	}
};

MsDockWidget *dockWidget = nullptr;

} // namespace

void vx_ms_dock_create(void)
{
	if (dockWidget)
		return;
	dockWidget = new MsDockWidget();
	if (!obs_frontend_add_dock_by_id(DOCK_ID, "VX Multistream", dockWidget)) {
		delete dockWidget;
		dockWidget = nullptr;
		obs_log(LOG_WARNING, "dock Multistream : création refusée");
		return;
	}
	obs_log(LOG_INFO, "dock Multistream créé");
}

void vx_ms_dock_destroy(void)
{
	if (!dockWidget)
		return;
	obs_frontend_remove_dock(DOCK_ID); // détruit aussi notre widget (pas de CEF ici)
	dockWidget = nullptr;
}

void vx_ms_dock_refresh(void)
{
	if (dockWidget)
		dockWidget->refresh();
}
