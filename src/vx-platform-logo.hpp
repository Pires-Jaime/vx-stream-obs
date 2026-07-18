/*
VX.Stream pour OBS — logos des plateformes de multistream
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QPixmap>
#include <QString>

/**
 * Logo carré arrondi d'une plateforme ("youtube", "kick", "tiktok", "twitch",
 * autre = générique RTMP), dessiné en QPainter — aucun asset embarqué, aucune
 * dépendance image (le plugin SVG de Qt n'est pas garanti partout).
 */
QPixmap vx_platform_logo(const QString &platform, int size);
