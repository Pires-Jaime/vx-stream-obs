/*
VX.Stream pour OBS — installation du thème Valerix
Copyright (C) 2026 Valerix (Jaime Pires) <support@valerix.stream>
SPDX-License-Identifier: GPL-2.0-or-later
*/

// OBS charge les thèmes utilisateur depuis <config>/obs-studio/themes (vu dans
// OBSApp_Themes.cpp). Un plugin ne peut pas enregistrer un thème par API : on
// COPIE donc notre .ovt depuis le data du module vers ce dossier. Copie à
// chaque chargement (écrase) : une mise à jour du plugin met à jour le thème.
// L'activation reste un choix utilisateur (Paramètres → Apparence → Valerix).

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include "vx-theme.hpp"

bool vx_install_theme(void)
{
	char *src = obs_module_file("themes/Valerix.ovt");
	if (!src) {
		obs_log(LOG_WARNING, "thème : Valerix.ovt absent du data du module");
		return false;
	}

	char *dstDir = os_get_config_path_ptr("obs-studio/themes");
	if (!dstDir) {
		bfree(src);
		return false;
	}
	os_mkdirs(dstDir);

	char dst[512];
	snprintf(dst, sizeof(dst), "%s/Valerix.ovt", dstDir);

	// os_copyfile refuse d'écraser une destination existante → on la retire
	// d'abord (c'est ce qui permet au thème de suivre les mises à jour).
	os_unlink(dst);
	const int rc = os_copyfile(src, dst);
	if (rc == 0)
		obs_log(LOG_INFO, "thème Valerix installé : %s", dst);
	else
		obs_log(LOG_WARNING, "thème : copie échouée (%d) vers %s", rc, dst);

	bfree(src);
	bfree(dstDir);
	return rc == 0;
}
