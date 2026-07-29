/*
VX.Stream pour OBS — compte Valerix (connexion / déconnexion)
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

class QMenu;

/**
 * Ajoute au menu VX.Stream le sous-menu « Compte » : « Se connecter… » toujours
 * présent, « Se déconnecter » visible uniquement quand une session est connue.
 * L'état vient de la page /obs/account (via le titre) et est persisté dans la
 * config du module, pour que le menu soit correct dès le lancement d'OBS.
 */
void vx_account_add_menu(QMenu *menu);

/** Ouvre la fenêtre « Mon compte » (connexion Twitch si pas de session). */
void vx_account_open(void);

/** Dernier état connu de la session Valerix. */
bool vx_account_is_logged(void);

/**
 * Au démarrage : si aucune session n'est connue, ouvre la fenêtre de connexion.
 * Sans compte, les docks n'affichent de toute façon qu'un écran de connexion.
 */
void vx_account_require_login(void);
