/*
VX.Stream pour OBS — signalement de problème (ticket support Valerix)
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

/**
 * « Signaler un problème… » : collecte un diagnostic (versions plugin/OBS, OS,
 * fin du journal OBS), le dépose sur /api/vx-stream/report-stash (thread réseau,
 * jamais bloquant), puis ouvre le formulaire web /obs/report?d=<id> dans le
 * dialog CEF (session des docks → le ticket est rattaché au bon compte).
 * Si le dépôt échoue, le formulaire s'ouvre sans diagnostic.
 */
void vx_report_show(void);

/** Join du thread réseau (unload du module). */
void vx_report_shutdown(void);
