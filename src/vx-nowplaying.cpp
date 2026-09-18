/*
VX.Stream pour OBS — musique en cours lue sur le PC du streamer
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "vx-nowplaying.hpp"

// ⚠️ ORDRE IMPOSÉ : <obs-module.h> AVANT <plugin-support.h>, comme dans tous les
// autres modules. `plugin-support.h` redéclare `blogva` sans le marqueur
// d'export de libobs ; si elle passe en premier, MSVC voit deux déclarations de
// liaison différente et refuse de compiler :
//   util/base.h(77): error C2375: 'blogva': redefinition; different linkage
// C'est ce qui a fait échouer le build Windows de la 0.23.1 — et uniquement lui,
// Clang et GCC tolèrent l'ordre inverse.
#ifdef VX_HAS_OBS
#include <obs-module.h>
#include <plugin-support.h>
#else
#define obs_log(...)
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {

// ⚠️ Seuls le jeton et son verrou vivent hors du bloc Windows : ce sont les
// seuls que `vx_nowplaying_set_token` touche sur toutes les plateformes. Tout
// le reste (fil, boucle, aides JSON) est INUTILE ailleurs, et Xcode compile
// avec -Werror : une variable inutilisée y casse la compilation de tout le
// plugin. C'est ce qui a fait échouer la 0.23.0 sur macOS.
std::mutex g_mtx;
std::string g_token;

} // namespace

#ifdef _WIN32

// ⚠️ C++/WinRT AVANT <windows.h>, et les macros de compatibilité : sans
// NOMINMAX, les macros min/max de Windows cassent les en-têtes WinRT, et
// WIN32_LEAN_AND_MEAN évite la collision GetCurrentTime.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>

#include <windows.h>
#include <wininet.h>

#pragma comment(lib, "wininet.lib")
// ⚠️ Indispensable : les fonctions d'exécution de WinRT (RoInitialize…) vivent
// ici. Sans cette bibliothèque, la compilation passe et c'est l'ÉDITION DE
// LIENS qui échoue, avec des symboles non résolus incompréhensibles.
#pragma comment(lib, "windowsapp.lib")

using namespace winrt;
using namespace winrt::Windows::Media::Control;

namespace {

std::atomic<bool> g_run{false};
std::thread g_thread;

std::string token_copy()
{
	std::lock_guard<std::mutex> lock(g_mtx);
	return g_token;
}

/** Échappement JSON minimal : nos titres contiennent guillemets et backslashes. */
std::string json_escape(const std::string &s)
{
	std::string out;
	out.reserve(s.size() + 8);
	for (unsigned char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			// Les caractères de contrôle doivent être échappés en \u00XX,
			// sinon le JSON est invalide et le serveur rejette tout l'envoi.
			if (c < 0x20) {
				char buf[7];
				snprintf(buf, sizeof(buf), "\\u%04x", c);
				out += buf;
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	return out;
}

/** UTF-16 (Windows) → UTF-8 (notre API). */
std::string utf8(const winrt::hstring &h)
{
	if (h.empty())
		return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, h.c_str(), (int)h.size(), nullptr, 0, nullptr, nullptr);
	if (n <= 0)
		return {};
	std::string out(static_cast<size_t>(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, h.c_str(), (int)h.size(), out.data(), n, nullptr, nullptr);
	return out;
}

void http_post_json(const char *host, const char *path, const std::string &body)
{
	HINTERNET net = InternetOpenA("vx-stream-plugin", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
	if (!net)
		return;
	// Délais courts : ce fil est JOINT au déchargement du module, il ne doit
	// jamais retenir la fermeture d'OBS.
	DWORD t = 4000;
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
			HttpSendRequestA(req, hdrs, (DWORD)strlen(hdrs), (LPVOID)body.data(), (DWORD)body.size());
			InternetCloseHandle(req);
		}
		InternetCloseHandle(conn);
	}
	InternetCloseHandle(net);
}

struct Morceau {
	std::string titre, artiste, album, lecteur;
	bool pause = false;
	long long positionMs = -1, dureeMs = -1;
	bool operator==(const Morceau &o) const { return titre == o.titre && artiste == o.artiste && pause == o.pause; }
};

/**
 * Lit la session média que Windows considère comme ACTIVE.
 *
 * ⚠️ `GetCurrentSession()` et non la liste complète : plusieurs lecteurs
 * peuvent être ouverts (Spotify en pause, un onglet YouTube…), et publier le
 * premier venu afficherait à l'antenne un titre que personne n'écoute.
 */
bool lire_session(Morceau &out)
{
	try {
		auto mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
		auto session = mgr.GetCurrentSession();
		if (!session)
			return false;

		const auto info = session.GetPlaybackInfo();
		const auto statut = info.PlaybackStatus();
		// Arrêté / fermé : rien ne joue, on ne garde pas le dernier titre.
		if (statut != GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing &&
		    statut != GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused)
			return false;

		const auto props = session.TryGetMediaPropertiesAsync().get();
		out.titre = utf8(props.Title());
		out.artiste = utf8(props.Artist());
		out.album = utf8(props.AlbumTitle());
		out.lecteur = utf8(session.SourceAppUserModelId());
		out.pause = (statut == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused);

		// ⚠️ La position n'est PAS rafraîchie en continu par Windows : elle ne
		// bouge qu'aux évènements du lecteur. L'overlay l'avance lui-même entre
		// deux envois — d'où l'intérêt de la transmettre telle quelle.
		const auto tl = session.GetTimelineProperties();
		const auto pos = std::chrono::duration_cast<std::chrono::milliseconds>(tl.Position()).count();
		const auto fin = std::chrono::duration_cast<std::chrono::milliseconds>(tl.EndTime()).count();
		out.positionMs = pos >= 0 ? pos : -1;
		out.dureeMs = fin > 0 ? fin : -1;
		return !out.titre.empty();
	} catch (...) {
		// Session média indisponible (Windows N, service arrêté) : on se tait.
		return false;
	}
}

void boucle()
{
	// ⚠️ Chaque fil qui touche à WinRT doit initialiser son apartment, sinon le
	// premier appel lève et la boucle ne publierait jamais rien.
	winrt::init_apartment(winrt::apartment_type::multi_threaded);

	Morceau precedent;
	bool avaitQuelqueChose = false;
	int tours = 0;

	while (g_run.load()) {
		const std::string tok = token_copy();
		if (!tok.empty()) {
			Morceau m;
			const bool ok = lire_session(m);

			// On publie quand quelque chose change, et de toute façon toutes
			// les ~10 s : le serveur oublie l'état au bout de 15 s sans
			// nouvelle, ce qui évite de figer un titre si OBS est tué.
			const bool change = ok != avaitQuelqueChose || (ok && !(m == precedent));
			if (change || (tours % 5) == 0) {
				std::string body = "{\"token\":\"" + json_escape(tok) + "\"";
				if (ok) {
					body += ",\"title\":\"" + json_escape(m.titre) + "\"";
					body += ",\"artist\":\"" + json_escape(m.artiste) + "\"";
					body += ",\"album\":\"" + json_escape(m.album) + "\"";
					body += ",\"source\":\"" + json_escape(m.lecteur) + "\"";
					body += m.pause ? ",\"paused\":true" : ",\"paused\":false";
					if (m.positionMs >= 0)
						body += ",\"positionMs\":" + std::to_string(m.positionMs);
					if (m.dureeMs > 0)
						body += ",\"durationMs\":" + std::to_string(m.dureeMs);
				} else {
					// Titre vide = « plus rien ne joue » côté serveur.
					body += ",\"title\":\"\"";
				}
				body += "}";
				http_post_json("valerix.stream", "/api/vx-stream/nowplaying", body);

				if (change)
					obs_log(LOG_DEBUG, "musique : %s", ok ? m.titre.c_str() : "(rien)");
			}
			precedent = m;
			avaitQuelqueChose = ok;
		}

		tours++;
		// Sommeil découpé : sans ça, l'arrêt d'OBS attendrait 2 s de plus.
		for (int i = 0; i < 20 && g_run.load(); i++)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	winrt::uninit_apartment();
}

} // namespace

void vx_nowplaying_start(void)
{
	if (g_run.exchange(true))
		return;
	g_thread = std::thread(boucle);
}

void vx_nowplaying_stop(void)
{
	if (!g_run.exchange(false))
		return;
	if (g_thread.joinable())
		g_thread.join();
}

#else // !_WIN32

// La session média globale est une API Windows. Sur macOS et Linux le module
// existe mais ne fait rien : le streamer garde la source SongRequest.
void vx_nowplaying_start(void) {}
void vx_nowplaying_stop(void) {}

#endif

void vx_nowplaying_set_token(const std::string &token)
{
	std::lock_guard<std::mutex> lock(g_mtx);
	g_token = token;
}
