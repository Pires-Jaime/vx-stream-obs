/*
VX.Stream pour OBS — signalement de problème
Copyright (C) 2026 Valerix (Jaime Pires) <support@valerix.stream>
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Le plugin natif n'a PAS la session Valerix (elle vit dans le CEF des docks) :
// il dépose donc le diagnostic sur un endpoint public à id imprévisible
// (report-stash, TTL 24 h), puis ouvre la page /obs/report?d=<id> dans le
// dialog CEF — c'est LA page, avec la session, qui crée le ticket. Réseau via
// WinINet (comme l'updater : Qt6Network non garanti dans OBS).

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QApplication>
#include <QMetaObject>

#include <algorithm>
#include <cstring>
#include <string>
#include <thread>

#include "vx-report.hpp"
#include "vx-webdialog.hpp"

namespace {

// Fin du dernier journal OBS (les noms de fichiers sont horodatés → le max
// lexicographique est le plus récent). Bornée : le corps du ticket est limité.
std::string latest_log_tail(size_t maxChars)
{
	std::string tail;
	char *logsDir = os_get_config_path_ptr("obs-studio/logs");
	if (!logsDir)
		return tail;
	std::string pattern = std::string(logsDir) + "/*.txt";
	bfree(logsDir);

	os_glob_t *glob = nullptr;
	if (os_glob(pattern.c_str(), 0, &glob) == 0 && glob) {
		std::string newest;
		for (size_t i = 0; i < glob->gl_pathc; i++) {
			if (glob->gl_pathv[i].directory)
				continue;
			const std::string p = glob->gl_pathv[i].path;
			if (p > newest)
				newest = p;
		}
		os_globfree(glob);
		if (!newest.empty()) {
			char *content = os_quick_read_utf8_file(newest.c_str());
			if (content) {
				std::string all = content;
				bfree(content);
				tail = all.size() > maxChars ? all.substr(all.size() - maxChars) : all;
			}
		}
	}
	return tail;
}

// Diagnostic complet, sérialisé en JSON par obs_data (échappement garanti).
std::string build_diag_json(void)
{
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "pluginVersion", PLUGIN_VERSION);
	obs_data_set_string(d, "obsVersion", obs_get_version_string());
#if defined(_WIN32)
	obs_data_set_string(d, "os", "Windows x64");
#elif defined(__APPLE__)
	obs_data_set_string(d, "os", "macOS");
#else
	obs_data_set_string(d, "os", "Linux");
#endif
	obs_data_set_string(d, "logTail", latest_log_tail(6000).c_str());
	const char *json = obs_data_get_json(d);
	std::string s = json ? json : "{}";
	obs_data_release(d);
	return s;
}

// "id":"<uuid>" — même extraction minimaliste que l'updater (objet contrôlé).
std::string parse_id(const std::string &body)
{
	const size_t k = body.find("\"id\"");
	if (k == std::string::npos)
		return {};
	const size_t q1 = body.find('"', body.find(':', k));
	if (q1 == std::string::npos)
		return {};
	const size_t q2 = body.find('"', q1 + 1);
	if (q2 == std::string::npos)
		return {};
	std::string id = body.substr(q1 + 1, q2 - q1 - 1);
	// L'id repart dans une URL : uuid strict uniquement.
	const bool ok = id.size() == 36 && std::all_of(id.begin(), id.end(), [](char c) {
				return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-';
			});
	return ok ? id : std::string{};
}

} // namespace

#ifdef _WIN32
#include <windows.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")

static std::string http_post_json(const char *host, const char *path, const std::string &body)
{
	std::string resp;
	HINTERNET net = InternetOpenA("vx-stream-plugin", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
	if (!net)
		return resp;
	DWORD t = 5000; // mêmes timeouts courts que l'updater : thread JOINT à l'unload
	InternetSetOptionA(net, INTERNET_OPTION_CONNECT_TIMEOUT, &t, sizeof(t));
	InternetSetOptionA(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));
	InternetSetOptionA(net, INTERNET_OPTION_SEND_TIMEOUT, &t, sizeof(t));
	HINTERNET conn =
		InternetConnectA(net, host, INTERNET_DEFAULT_HTTPS_PORT, nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
	if (conn) {
		HINTERNET req = HttpOpenRequestA(conn, "POST", path, nullptr, nullptr, nullptr,
						 INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE, 0);
		if (req) {
			const char *hdrs = "Content-Type: application/json\r\n";
			if (HttpSendRequestA(req, hdrs, (DWORD)strlen(hdrs), (LPVOID)body.data(), (DWORD)body.size())) {
				char buf[1024];
				DWORD n = 0;
				while (InternetReadFile(req, buf, sizeof(buf), &n) && n > 0)
					resp.append(buf, n);
			}
			InternetCloseHandle(req);
		}
		InternetCloseHandle(conn);
	}
	InternetCloseHandle(net);
	return resp;
}
#else
static std::string http_post_json(const char *, const char *, const std::string &)
{
	return {}; // cible Windows — le formulaire s'ouvre simplement sans diagnostic
}
#endif

static std::thread reportThread;

void vx_report_shutdown(void)
{
	if (reportThread.joinable())
		reportThread.join();
}

void vx_report_show(void)
{
	if (reportThread.joinable())
		reportThread.join(); // clic précédent terminé (timeouts 5 s le garantissent)

	reportThread = std::thread([] {
		const std::string diag = build_diag_json();
		const std::string resp = http_post_json("valerix.stream", "/api/vx-stream/report-stash", diag);
		const std::string id = parse_id(resp);
		std::string url = "https://valerix.stream/obs/report";
		if (!id.empty())
			url += "?d=" + id;
		else
			obs_log(LOG_WARNING, "report : dépôt du diagnostic impossible, formulaire sans diag");

		// Retour thread UI pour ouvrir le dialog (Qt : widgets = UI thread only).
		QMetaObject::invokeMethod(
			qApp, [url] { vx_webdialog_open("VX.Stream — Signaler un problème", url.c_str(), 600, 680); },
			Qt::QueuedConnection);
	});
}
