/*
VX.Stream pour OBS — vérification de mise à jour
Copyright (C) 2026 Valerix (Jaime Pires) <support@valerix.stream>
SPDX-License-Identifier: GPL-2.0-or-later
*/

// HTTP via WinINet (API native Windows, toujours présente) plutôt que
// Qt6Network : rien ne garantit qu'OBS livre Qt6Network.dll, et une DLL
// manquante empêcherait le plugin ENTIER de charger. Hors Windows le check
// est simplement absent (la cible de l'installateur est Windows).
//
// Deux vérifications distinctes (parité SE.Live) :
//   • le PLUGIN (endpoint /api/vx-stream/version, installe en 1 clic) ;
//   • OBS lui-même (endpoint /api/vx-stream/obs-version → simple lien vers
//     obsproject.com/download : ce n'est pas à nous d'installer OBS).

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QAction>
#include <QDesktopServices>
#include <QMenu>
#include <QMessageBox>
#include <QMetaObject>
#include <QUrl>

#include <atomic>
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

// Extrait le premier « X.Y[.Z] » d'une chaîne libre — obs_get_version_string()
// peut contenir un suffixe (rc, beta) ou un préfixe selon les builds.
std::string extract_semver(const std::string &s)
{
	size_t i = 0;
	while (i < s.size()) {
		if (isdigit((unsigned char)s[i])) {
			size_t j = i;
			int dots = 0;
			while (j < s.size() && (isdigit((unsigned char)s[j]) || (s[j] == '.' && dots < 2))) {
				if (s[j] == '.')
					dots++;
				j++;
			}
			std::string v = s.substr(i, j - i);
			while (!v.empty() && v.back() == '.')
				v.pop_back();
			if (v.find('.') != std::string::npos) {
				if (dots == 1)
					v += ".0"; // "32.1" → "32.1.0"
				return v;
			}
			i = j;
		} else {
			i++;
		}
	}
	return {};
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

// Notes de version renvoyées par l'API (« notes »), pour afficher le CHANGELOG
// à chaque mise à jour. JSON simple que nous produisons → extraction directe,
// avec les échappements que peut contenir un corps de release GitHub.
std::string parse_notes(const std::string &body)
{
	const size_t k = body.find("\"notes\"");
	if (k == std::string::npos)
		return {};
	size_t i = body.find('"', body.find(':', k));
	if (i == std::string::npos)
		return {};
	std::string out;
	for (++i; i < body.size(); i++) {
		const char c = body[i];
		if (c == '\\' && i + 1 < body.size()) {
			const char n = body[++i];
			if (n == 'n')
				out += '\n';
			else if (n == 't')
				out += ' ';
			else if (n == 'u') { // \uXXXX : on saute (rare dans nos notes)
				i += 4;
			} else
				out += n;
			continue;
		}
		if (c == '"')
			break;
		out += c;
	}
	return out;
}

// Dernière version déjà signalée par MessageBox (pour ne le faire qu'une fois).
// `name` distingue le fichier plugin du fichier OBS.
std::string notified_file(const char *name)
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *p = obs_module_config_path(name);
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

std::string read_notified(const char *name)
{
	char *content = os_quick_read_utf8_file(notified_file(name).c_str());
	std::string s = content ? content : "";
	bfree(content);
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
		s.pop_back();
	return s;
}

void write_notified(const char *name, const std::string &v)
{
	os_quick_write_utf8_file(notified_file(name).c_str(), v.c_str(), v.size(), false);
}

const char *PLUGIN_NOTIFIED = "update-notified.txt";
const char *OBS_NOTIFIED = "obs-update-notified.txt";
const char *OBS_DOWNLOAD_URL = "https://obsproject.com/download";

// Version d'OBS en cours, normalisée "X.Y.Z".
std::string local_obs_version(void)
{
	return extract_semver(obs_get_version_string());
}

// Chemins interrogés par les DEUX plateformes (le code commun les passe à
// http_get_body) — les garder hors du #ifdef évite aussi l'erreur
// « const inutilisée » hors _WIN32 (warnings-as-errors).
static const char *VERSION_URL_PATH = "/api/vx-stream/version";
static const char *OBS_VERSION_URL_PATH = "/api/vx-stream/obs-version";

} // namespace

#ifdef _WIN32
#include <windows.h>
#include <wininet.h>
#include <shellapi.h>
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "shell32.lib")

#include <QApplication>
#include <QMainWindow>

#include <cstdio>

static const char *VERSION_URL_HOST = "valerix.stream";
static const char *INSTALLER_PATH = "/downloads/VX.Stream-Installer.exe";

static std::string http_get_body(const char *path)
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
		HINTERNET req = HttpOpenRequestA(conn, "GET", path, nullptr, nullptr, nullptr,
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
// Télécharge l'installateur (~3,5 Mo) dans %TEMP%. Chaîne vide si échec.
static std::string download_installer()
{
	char tmp[MAX_PATH];
	if (!GetTempPathA(sizeof(tmp), tmp))
		return {};
	std::string dest = std::string(tmp) + "VXStream-Update.exe";

	HINTERNET net = InternetOpenA("vx-stream-plugin", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
	if (!net)
		return {};
	bool ok = false;
	HINTERNET conn = InternetConnectA(net, VERSION_URL_HOST, INTERNET_DEFAULT_HTTPS_PORT, nullptr, nullptr,
					  INTERNET_SERVICE_HTTP, 0, 0);
	if (conn) {
		HINTERNET req = HttpOpenRequestA(conn, "GET", INSTALLER_PATH, nullptr, nullptr, nullptr,
						 INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE, 0);
		if (req && HttpSendRequestA(req, nullptr, 0, nullptr, 0)) {
			FILE *f = nullptr;
			if (fopen_s(&f, dest.c_str(), "wb") == 0 && f) {
				char buf[16384];
				DWORD n = 0;
				size_t total = 0;
				while (InternetReadFile(req, buf, sizeof(buf), &n) && n > 0) {
					fwrite(buf, 1, n, f);
					total += n;
				}
				fclose(f);
				ok = total > 500 * 1024; // un vrai installeur fait > 500 Ko
			}
		}
		if (req)
			InternetCloseHandle(req);
		InternetCloseHandle(conn);
	}
	InternetCloseHandle(net);
	return ok ? dest : std::string{};
}

/**
 * « Mettre à jour maintenant » : télécharge l'installateur, le lance, puis
 * ferme OBS proprement — la DLL du plugin est verrouillée tant qu'OBS tourne,
 * l'installation ne peut aboutir qu'après sa fermeture.
 */
static void launch_update(QWidget *parent)
{
	QApplication::setOverrideCursor(Qt::WaitCursor);
	const std::string exe = download_installer();
	QApplication::restoreOverrideCursor();

	if (exe.empty()) {
		QMessageBox::warning(parent, QStringLiteral("VX.Stream"),
				     QStringLiteral("Téléchargement impossible — récupérez la mise à jour "
						    "sur valerix.stream/obs."));
		QDesktopServices::openUrl(QUrl(QStringLiteral("https://valerix.stream/obs")));
		return;
	}

	obs_log(LOG_INFO, "updater : installateur téléchargé (%s), fermeture d'OBS", exe.c_str());
	ShellExecuteA(nullptr, "open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

	auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (window)
		window->close(); // fermeture PROPRE (confirmation si un stream tourne)
}
#else
static std::string http_get_body(const char *)
{
	return {}; // cible Windows — pas de check ailleurs
}

static void launch_update(QWidget *)
{
	QDesktopServices::openUrl(QUrl(QStringLiteral("https://valerix.stream/obs")));
}
#endif

static std::thread checkThread;
static std::thread manualThread;
static std::atomic<bool> manualBusy{false};

void vx_updater_shutdown(void)
{
	// Join (pas detach) : un thread survivant au déchargement de la DLL
	// exécuterait du code disparu → crash à la fermeture d'OBS. Les timeouts
	// WinINet de 5 s bornent l'attente au pire cas.
	if (checkThread.joinable())
		checkThread.join();
	if (manualThread.joinable())
		manualThread.join();
}

// Boîte de dialogue de mise à jour AVEC le changelog. Les notes vont dans le
// « détail » (repliable) : le message principal reste court, et le streamer qui
// veut savoir ce qui change déplie. Renvoie true si l'utilisateur accepte.
static bool ask_update(QWidget *parent, const QString &text, const std::string &notes)
{
	QMessageBox box(parent);
	box.setWindowTitle(QStringLiteral("VX.Stream"));
	box.setIcon(QMessageBox::Question);
	box.setText(text);
	if (!notes.empty()) {
		box.setInformativeText(QStringLiteral("Nouveautés de cette version :"));
		box.setDetailedText(QString::fromStdString(notes));
	}
	box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
	box.setDefaultButton(QMessageBox::Yes);
	return box.exec() == QMessageBox::Yes;
}

// Ajoute au menu l'entrée « ⬆ OBS vX disponible… » (simple lien de
// téléchargement — installer OBS n'est pas notre rôle). Thread UI uniquement.
static void add_obs_update_action(QMenu *menu, const std::string &remote)
{
	QAction *first = menu->actions().isEmpty() ? nullptr : menu->actions().first();
	QAction *up =
		new QAction(QStringLiteral("⬆ OBS Studio v%1 disponible…").arg(QString::fromStdString(remote)), menu);
	QObject::connect(up, &QAction::triggered,
			 [] { QDesktopServices::openUrl(QUrl(QString::fromUtf8(OBS_DOWNLOAD_URL))); });
	menu->insertAction(first, up);
	menu->insertSeparator(first);
}

void vx_updater_check(QMenu *menu)
{
	// Thread joignable : le lancement d'OBS ne doit JAMAIS attendre le réseau.
	checkThread = std::thread([menu] {
		// ── Plugin ──
		const std::string body = http_get_body(VERSION_URL_PATH);
		const std::string remote = parse_version(body);
		const std::string notes = parse_notes(body);
		const bool pluginOutdated = !remote.empty() && is_newer(remote, PLUGIN_VERSION);
		if (!pluginOutdated && !remote.empty())
			obs_log(LOG_INFO, "updater : %s à jour (dernière : %s)", PLUGIN_VERSION, remote.c_str());

		// ── OBS lui-même ──
		const std::string obsBody = http_get_body(OBS_VERSION_URL_PATH);
		const std::string obsRemote = parse_version(obsBody);
		const std::string obsNotes = parse_notes(obsBody);
		const std::string obsLocal = local_obs_version();
		const bool obsOutdated = !obsRemote.empty() && !obsLocal.empty() && is_newer(obsRemote, obsLocal);

		if (!pluginOutdated && !obsOutdated)
			return;

		if (pluginOutdated)
			obs_log(LOG_INFO, "updater : mise à jour disponible %s → %s", PLUGIN_VERSION, remote.c_str());
		if (obsOutdated)
			obs_log(LOG_INFO, "updater : OBS %s → %s disponible", obsLocal.c_str(), obsRemote.c_str());

		const bool pluginNotified = read_notified(PLUGIN_NOTIFIED) == remote;
		const bool obsNotified = read_notified(OBS_NOTIFIED) == obsRemote;

		// Retour sur le thread UI : Qt interdit de toucher aux widgets ailleurs.
		QMetaObject::invokeMethod(
			menu,
			[menu, remote, notes, obsRemote, obsNotes, obsLocal, pluginOutdated, obsOutdated,
			 pluginNotified, obsNotified] {
				if (obsOutdated) {
					add_obs_update_action(menu, obsRemote);
					if (!obsNotified) {
						write_notified(OBS_NOTIFIED, obsRemote);
						if (ask_update(menu->parentWidget(),
							       QStringLiteral("Une mise à jour d'OBS Studio est "
									      "disponible (v%1 → v%2).\n\nOuvrir "
									      "la page de téléchargement ?")
								       .arg(QString::fromStdString(obsLocal),
									    QString::fromStdString(obsRemote)),
							       obsNotes))
							QDesktopServices::openUrl(
								QUrl(QString::fromUtf8(OBS_DOWNLOAD_URL)));
					}
				}

				if (pluginOutdated) {
					QAction *first = menu->actions().isEmpty() ? nullptr : menu->actions().first();
					QAction *up = new QAction(QStringLiteral("⬆ Mettre à jour vers v%1 (auto)…")
									  .arg(QString::fromStdString(remote)),
								  menu);
					QObject::connect(up, &QAction::triggered, [menu] {
						if (QMessageBox::question(
							    menu->parentWidget(), QStringLiteral("VX.Stream"),
							    QStringLiteral("Installer la mise à jour maintenant ?\n\n"
									   "OBS va se fermer, l'installateur s'ouvre, "
									   "puis relancez OBS.")) == QMessageBox::Yes)
							launch_update(menu->parentWidget());
					});
					menu->insertAction(first, up);
					menu->insertSeparator(first);

					if (!pluginNotified) {
						write_notified(PLUGIN_NOTIFIED, remote);
						// PLUGIN_VERSION est une variable extern (pas un littéral)
						// → fromUtf8, pas QStringLiteral.
						if (ask_update(menu->parentWidget(),
							       QStringLiteral("Une mise à jour de VX.Stream est "
									      "disponible (v%1 → v%2).\n\nL'installer "
									      "maintenant ? OBS se fermera, puis "
									      "relancez-le.")
								       .arg(QString::fromUtf8(PLUGIN_VERSION),
									    QString::fromStdString(remote)),
							       notes))
							launch_update(menu->parentWidget());
					}
				}
			},
			Qt::QueuedConnection);
	});
}

void vx_updater_check_manual(QMenu *menu)
{
	if (manualBusy.exchange(true))
		return; // une vérification manuelle tourne déjà
	if (manualThread.joinable())
		manualThread.join();

	manualThread = std::thread([menu] {
		const std::string body = http_get_body(VERSION_URL_PATH);
		const std::string obsBody = http_get_body(OBS_VERSION_URL_PATH);
		const std::string remote = parse_version(body), notes = parse_notes(body);
		const std::string obsRemote = parse_version(obsBody), obsNotes = parse_notes(obsBody);
		const std::string obsLocal = local_obs_version();
		const bool pluginOutdated = !remote.empty() && is_newer(remote, PLUGIN_VERSION);
		const bool obsOutdated = !obsRemote.empty() && !obsLocal.empty() && is_newer(obsRemote, obsLocal);

		QMetaObject::invokeMethod(
			menu,
			[menu, remote, notes, obsRemote, obsNotes, obsLocal, pluginOutdated, obsOutdated] {
				manualBusy.store(false);
				QWidget *parent = menu->parentWidget();

				if (remote.empty() && obsRemote.empty()) {
					QMessageBox::warning(parent, QStringLiteral("VX.Stream"),
							     QStringLiteral(
								     "Vérification impossible (réseau ou serveur "
								     "indisponible). Réessayez plus tard."));
					return;
				}

				const QString pluginLine =
					remote.empty() ? QStringLiteral("VX.Stream : v%1 (vérification impossible)")
								 .arg(QString::fromUtf8(PLUGIN_VERSION))
					: pluginOutdated ? QStringLiteral("VX.Stream : v%1 → v%2 disponible")
								   .arg(QString::fromUtf8(PLUGIN_VERSION),
									QString::fromStdString(remote))
							 : QStringLiteral("VX.Stream : v%1 — à jour ✓")
								   .arg(QString::fromUtf8(PLUGIN_VERSION));
				const QString obsLine =
					obsRemote.empty() ? QStringLiteral("OBS Studio : vérification impossible")
					: obsOutdated     ? QStringLiteral("OBS Studio : v%1 → v%2 disponible")
								.arg(QString::fromStdString(obsLocal),
								     QString::fromStdString(obsRemote))
						      : QStringLiteral("OBS Studio : v%1 — à jour ✓")
								.arg(QString::fromStdString(obsLocal));
				const QString status = pluginLine + QStringLiteral("\n") + obsLine;

				if (pluginOutdated) {
					if (ask_update(parent,
						       status + QStringLiteral("\n\nInstaller la mise à jour de "
									       "VX.Stream maintenant ? OBS va se "
									       "fermer, l'installateur s'ouvre, puis "
									       "relancez OBS."),
						       notes))
						launch_update(parent);
				} else if (obsOutdated) {
					if (ask_update(parent,
						       status + QStringLiteral("\n\nOuvrir la page de "
									       "téléchargement d'OBS Studio ?"),
						       obsNotes))
						QDesktopServices::openUrl(QUrl(QString::fromUtf8(OBS_DOWNLOAD_URL)));
				} else {
					QMessageBox::information(parent, QStringLiteral("VX.Stream"), status);
				}
			},
			Qt::QueuedConnection);
	});
}
