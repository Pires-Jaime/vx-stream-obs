/*
VX.Stream pour OBS — logos des plateformes de multistream
Copyright (C) 2026 Valerix (Jaime Pires) <support@valerix.stream>
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Logos simplifiés dessinés au QPainter sur une grille 24×24 (mise à l'échelle
// par le painter). Volontairement pas d'assets : rien à installer, rien qui
// dépende du plugin d'images SVG de Qt, et le rendu suit le devicePixelRatio.

#include "vx-platform-logo.hpp"

#include <QPainter>
#include <QPainterPath>

namespace {

void draw_twitch(QPainter &p)
{
	p.setBrush(QColor("#9146FF"));
	p.drawRoundedRect(QRectF(0, 0, 24, 24), 5, 5);
	// Bulle Twitch simplifiée : rectangle à coin coupé + pointe en bas.
	QPainterPath glyph;
	glyph.moveTo(7, 4.5);
	glyph.lineTo(19.5, 4.5);
	glyph.lineTo(19.5, 13.5);
	glyph.lineTo(16, 17);
	glyph.lineTo(13, 17);
	glyph.lineTo(10.5, 19.5);
	glyph.lineTo(10.5, 17);
	glyph.lineTo(4.5, 17);
	glyph.lineTo(4.5, 7);
	glyph.closeSubpath();
	p.setBrush(Qt::white);
	p.drawPath(glyph);
	// Les deux « yeux ».
	p.setBrush(QColor("#9146FF"));
	p.drawRect(QRectF(11, 8, 1.8, 5));
	p.drawRect(QRectF(15.2, 8, 1.8, 5));
}

void draw_youtube(QPainter &p)
{
	p.setBrush(QColor("#FF0000"));
	p.drawRoundedRect(QRectF(0, 0, 24, 24), 5, 5);
	QPainterPath tri;
	tri.moveTo(9.5, 7.5);
	tri.lineTo(17.5, 12);
	tri.lineTo(9.5, 16.5);
	tri.closeSubpath();
	p.setBrush(Qt::white);
	p.drawPath(tri);
}

void draw_kick(QPainter &p)
{
	p.setBrush(QColor("#0E0E10"));
	p.drawRoundedRect(QRectF(0, 0, 24, 24), 5, 5);
	// « K » en blocs, façon pixel du logo Kick.
	p.setBrush(QColor("#53FC18"));
	p.drawRect(QRectF(6, 4, 4, 16));  // barre verticale
	p.drawRect(QRectF(10, 10, 3, 4)); // jonction centrale
	p.drawRect(QRectF(13, 7, 3, 3));  // diagonale haute
	p.drawRect(QRectF(16, 4, 3, 3));
	p.drawRect(QRectF(13, 14, 3, 3)); // diagonale basse
	p.drawRect(QRectF(16, 17, 3, 3));
}

void draw_tiktok(QPainter &p)
{
	p.setBrush(QColor("#010101"));
	p.drawRoundedRect(QRectF(0, 0, 24, 24), 5, 5);
	// Note de musique : hampe + crochet + tête, avec les ombres cyan/rose de
	// la marque, décalées en diagonale.
	auto note = [&p](qreal dx, qreal dy, const QColor &c) {
		p.setBrush(c);
		p.drawRect(QRectF(12 + dx, 5 + dy, 2.4, 11)); // hampe
		QPainterPath hook;                            // crochet
		hook.moveTo(14.4 + dx, 5 + dy);
		hook.quadTo(17.5 + dx, 6.5 + dy, 18.5 + dx, 9.5 + dy);
		hook.quadTo(16.5 + dx, 8.8 + dy, 14.4 + dx, 8.2 + dy);
		hook.closeSubpath();
		p.drawPath(hook);
		p.drawEllipse(QRectF(8.2 + dx, 14 + dy, 5.2, 4)); // tête
	};
	note(-0.9, -0.9, QColor("#25F4EE"));
	note(0.9, 0.9, QColor("#FE2C55"));
	note(0, 0, Qt::white);
}

void draw_custom(QPainter &p)
{
	p.setBrush(QColor("#4B5563"));
	p.drawRoundedRect(QRectF(0, 0, 24, 24), 5, 5);
	// Icône « diffusion » : point + deux ondes.
	p.setBrush(Qt::white);
	p.drawEllipse(QRectF(10.5, 13, 3.5, 3.5));
	QPen pen(Qt::white, 1.8);
	pen.setCapStyle(Qt::RoundCap);
	p.setPen(pen);
	p.setBrush(Qt::NoBrush);
	p.drawArc(QRectF(7.5, 9.5, 9, 9), 30 * 16, 120 * 16);
	p.drawArc(QRectF(4.5, 6.5, 15, 15), 30 * 16, 120 * 16);
}

} // namespace

QPixmap vx_platform_logo(const QString &platform, int size)
{
	// ×2 puis devicePixelRatio : net aussi sur les écrans haute densité.
	QPixmap pm(size * 2, size * 2);
	pm.fill(Qt::transparent);
	{
		QPainter p(&pm);
		p.setRenderHint(QPainter::Antialiasing);
		p.scale(size * 2 / 24.0, size * 2 / 24.0);
		p.setPen(Qt::NoPen);

		const QString id = platform.toLower();
		if (id == QStringLiteral("youtube"))
			draw_youtube(p);
		else if (id == QStringLiteral("kick"))
			draw_kick(p);
		else if (id == QStringLiteral("tiktok"))
			draw_tiktok(p);
		else if (id == QStringLiteral("twitch"))
			draw_twitch(p);
		else
			draw_custom(p);
	}
	pm.setDevicePixelRatio(2.0);
	return pm;
}
