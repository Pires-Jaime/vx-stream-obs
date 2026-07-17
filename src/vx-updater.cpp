/*
VX.Stream pour OBS — vérification de mise à jour
Copyright (C) 2026 Valerix (Jaime Pires) <support@valerix.stream>
SPDX-License-Identifier: GPL-2.0-or-later
*/

// HTTP via WinINet (API native Windows, toujours présente) plutôt que
// Qt6Network : rien ne garantit qu'OBS livre Qt6Network.dll, et une DLL
// manquante empêcherait le plugin ENTIER de charger. Hors Windows le check
// est simplement absent (la cible de l'installateur est Windows).

#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QAction>
#include <QDesktopServices>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QUrl>

#include <cctype>
#include <string>
#include <thread>

#include "vx-updater.hpp"

namespace {

// "X.Y.Z" → [X, Y, Z] ; false si la forme est invalide.
// (Pas de sscanf : déprécié par MSVC → erreur avec warnings-as-errors.)
bool parse_semver(const std::string &s, int out[3])
{
	size_t pos = 0;
	for (int i = 0; i < 3; i++) {
		if (pos >= s.size() || !isdigit((unsigned char)s[pos]))
			return false;
		long v = 0;
		while (pos < s.size() && isdigit((unsigned char)s[pos]))
			v = v * 10 + (s[pos++] - '0');
		out[i] = (int)v;
		if (i < 2) {
			if (pos >= s.size() || s[pos] != '.')
				return false;
			pos++;
		}
	}
	return pos == s.size();
}

// Renvoie true si `remote` est strictement plus récent que `local`.
bool is_newer(const std::string &remote, const std::string &local)
{
	int r[3], l[3] = {0, 0, 0};
	if (!parse_semver(remote, r))
		return false;
	parse_semver(local, l);
	for (int i = 0; i < 3; i++) {
		if (r[i] != l[i])
			return r[i] > l[i];
	}
	return false;
}

// Extraction minimaliste de "version":"X.Y.Z" — pas besoin d'un parseur JSON
// pour un objet à deux clés que nous contrôlons.
std::string parse_version(const std::string &body)
{
	const size_t k = body.find("\"version\"");
	if (k == std::string::npos)
		return {};
	const size_t q1 = body.find('"', body.find(':', k));
	if (q1 == std::string::npos)
		return {};
	const size_t q2 = body.find('"', q1 + 1);
	if (q2 == std::string::npos)
		return {};
	return body.substr(q1 + 1, q2 - q1 - 1);
}

// Dernière version déjà signalée par MessageBox (pour ne le faire qu'une fois).
std::string notified_file()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *p = obs_module_config_path("update-notified.txt");
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

std::string read_notified()
{
	char *content = os_quick_read_utf8_file(notified_file().c_str());
	std::string s = content ? content : "";
	bfree(content);
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
		s.pop_back();
	return s;
}

void write_notified(const std::string &v)
{
	os_quick_write_utf8_file(notified_file().c_str(), v.c_str(), v.size(), false);
}

} // namespace

#ifdef _WIN32
#include <windows.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")

static const char *VERSION_URL_HOST = "valerix.stream";
static const char *VERSION_URL_PATH = "/api/vx-stream/version";

static std::string http_get_version_body()
{
	std::string body;
	HINTERNET net = InternetOpenA("vx-stream-plugin", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
	if (!net)
		return body;
	// Timeouts courts : ce thread est JOINT à l'unload du module — il ne doit
	// jamais retenir la fermeture d'OBS plus de quelques secondes.
	DWORD t = 5000;
	InternetSetOptionA(net, INTERNET_OPTION_CONNECT_TIMEOUT, &t, sizeof(t));
	InternetSetOptionA(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));
	InternetSetOptionA(net, INTERNET_OPTION_SEND_TIMEOUT, &t, sizeof(t));
	HINTERNET conn = InternetConnectA(net, VERSION_URL_HOST, INTERNET_DEFAULT_HTTPS_PORT, nullptr, nullptr,
					  INTERNET_SERVICE_HTTP, 0, 0);
	if (conn) {
		HINTERNET req = HttpOpenRequestA(conn, "GET", VERSION_URL_PATH, nullptr, nullptr, nullptr,
						 INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE, 0);
		if (req) {
			if (HttpSendRequestA(req, nullptr, 0, nullptr, 0)) {
				char buf[1024];
				DWORD n = 0;
				while (InternetReadFile(req, buf, sizeof(buf), &n) && n > 0)
					body.append(buf, n);
			}
			InternetCloseHandle(req);
		}
		InternetCloseHandle(conn);
	}
	InternetCloseHandle(net);
	return body;
}
#else
static std::string http_get_version_body()
{
	return {}; // cible Windows — pas de check ailleurs
}
#endif

static std::thread checkThread;

void vx_updater_shutdown(void)
{
	// Join (pas detach) : un thread survivant au déchargement de la DLL
	// exécuterait du code disparu → crash à la fermeture d'OBS. Les timeouts
	// WinINet de 5 s bornent l'attente au pire cas.
	if (checkThread.joinable())
		checkThread.join();
}

void vx_updater_check(QMenu *menu)
{
	// Thread joignable : le lancement d'OBS ne doit JAMAIS attendre le réseau.
	checkThread = std::thread([menu] {
		const std::string body = http_get_version_body();
		const std::string remote = parse_version(body);
		if (remote.empty() || !is_newer(remote, PLUGIN_VERSION)) {
			if (!remote.empty())
				obs_log(LOG_INFO, "updater : %s à jour (dernière : %s)", PLUGIN_VERSION,
					remote.c_str());
			return;
		}
		obs_log(LOG_INFO, "updater : mise à jour disponible %s → %s", PLUGIN_VERSION, remote.c_str());

		const bool alreadyNotified = read_notified() == remote;

		// Retour sur le thread UI : Qt interdit de toucher aux widgets ailleurs.
		QMetaObject::invokeMethod(
			menu,
			[menu, remote, alreadyNotified] {
				QAction *first = menu->actions().isEmpty() ? nullptr : menu->actions().first();
				QAction *up = new QAction(QStringLiteral("⬆ Mise à jour disponible (v%1)…")
								  .arg(QString::fromStdString(remote)),
							  menu);
				QObject::connect(up, &QAction::triggered, [] {
					QDesktopServices::openUrl(QUrl(QStringLiteral("https://valerix.stream/obs")));
				});
				menu->insertAction(first, up);
				menu->insertSeparator(first);

				if (!alreadyNotified) {
					write_notified(remote);
					// PLUGIN_VERSION est une variable extern (pas un littéral)
					// → fromUtf8, pas QStringLiteral.
					QMessageBox::information(menu->parentWidget(), QStringLiteral("VX.Stream"),
								 QStringLiteral(
									 "Une mise à jour de VX.Stream est disponible "
									 "(v%1 → v%2).\n\nTéléchargez-la depuis "
									 "valerix.stream/obs — ou via le menu "
									 "VX.Stream → « Mise à jour disponible ».")
									 .arg(QString::fromUtf8(PLUGIN_VERSION),
									      QString::fromStdString(remote)));
				}
			},
			Qt::QueuedConnection);
	});
}
