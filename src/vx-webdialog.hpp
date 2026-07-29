/*
VX.Stream pour OBS — dialog web générique (CEF partagé des docks)
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

/**
 * Ouvre (ou remplace) LE dialog web du plugin : un QDialog contenant un
 * QCefWidget sur `url`, avec la session partagée des docks (si le streamer est
 * connecté dans un dock, il l'est ici). Un seul dialog à la fois — rouvrir
 * remplace le précédent (titre/URL frais).
 */
void vx_webdialog_open(const char *title, const char *url, int w, int h);

/**
 * Détruit le dialog et son navigateur SYNCHRONEMENT. À appeler sur
 * OBS_FRONTEND_EVENT_EXIT, pendant que CEF est encore vivant — tout widget CEF
 * détruit en différé recrée le crash de fermeture (leçon 0.6.1/0.7.1).
 */
void vx_webdialog_shutdown(void);
