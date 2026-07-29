/*
VX.Stream pour OBS — compte Valerix (connexion / déconnexion)
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

class QMenu;

/**
 * Ajoute au menu VX.Stream les entrées « Se connecter… » / « Se déconnecter ».
 * L'état (connecté ou non) est relu à chaque ouverture du menu via le cookie
 * de session du CEF partagé — le plugin natif n'a pas d'autre moyen de le
 * savoir, la session vivant dans le navigateur intégré, pas côté C++.
 */
void vx_account_add_menu(QMenu *menu);
