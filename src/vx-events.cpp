/*
VX.Stream pour OBS — flux d'évènements Valerix
SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "vx-events.hpp"
#include "vx-voicemod.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#ifdef VX_HAS_OBS
#include <obs-module.h>
#include "plugin-support.h"
#else
#define obs_log(...) ((void)0)
#endif

// ── Analyse SSE (portable, testée hors OBS) ──────────────────────────────────
std::vector<std::string> VxSseParser::feed(const std::string &chunk)
{
	std::vector<std::string> out;
	buf += chunk;

	size_t pos;
	while ((pos = buf.find('\n')) != std::string::npos) {
		std::string line = buf.substr(0, pos);
		buf.erase(0, pos + 1);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();

		if (line.empty()) {
			// Ligne vide = fin d'évènement. Un `current` vide arrive après un
			// simple battement : rien à livrer.
			if (!current.empty()) {
				out.push_back(current);
				current.clear();
			}
			continue;
		}
		if (line[0] == ':')
			continue; // commentaire / ping

		if (line.rfind("data:", 0) == 0) {
			std::string d = line.substr(5);
			if (!d.empty() && d[0] == ' ')
				d.erase(0, 1);
			// Plusieurs `data:` dans un même évènement se concatènent.
			if (!current.empty())
				current += "\n";
			current += d;
		}
		// Les champs `event:` / `id:` ne nous servent pas : le type est dans
		// le JSON.
	}
	return out;
}

// ── Extraction JSON minimale ─────────────────────────────────────────────────
// Pas de bibliothèque JSON dans le plugin : nos charges utiles sont plates et
// produites par nous. On lit une chaîne ou un nombre par clé, sans plus.
namespace {

std::string json_str(const std::string &j, const char *key)
{
	const std::string pat = std::string("\"") + key + "\":";
	size_t i = j.find(pat);
	if (i == std::string::npos)
		return {};
	i += pat.size();
	while (i < j.size() && (j[i] == ' ' || j[i] == '\t'))
		++i;
	if (i >= j.size() || j[i] != '"')
		return {};
	++i;
	std::string out;
	for (; i < j.size() && j[i] != '"'; ++i) {
		if (j[i] == '\\' && i + 1 < j.size())
			++i; // échappement : on garde le caractère suivant tel quel
		out += j[i];
	}
	return out;
}

long json_num(const std::string &j, const char *key)
{
	const std::string pat = std::string("\"") + key + "\":";
	size_t i = j.find(pat);
	if (i == std::string::npos)
		return 0;
	i += pat.size();
	while (i < j.size() && (j[i] == ' ' || j[i] == '\t'))
		++i;
	return std::strtol(j.c_str() + i, nullptr, 10);
}

std::mutex g_mu;
std::string g_token;
std::atomic<bool> g_run{false};
std::thread g_thread;

// ── Pont Voicemod ────────────────────────────────────────────────────────────
// Clé applicative de Valerix : elle identifie NOTRE intégration auprès de
// Voicemod, elle est la même pour tous les streamers et vit donc dans le
// binaire. Aucun secret par utilisateur ici.
const char *VOICEMOD_CLIENT_KEY = "controlapi-0keCusmFK";

VxWebSocket g_vm;
std::string g_prevVoice; // voix à restaurer
std::chrono::steady_clock::time_point g_restoreAt{};

bool voicemod_ensure()
{
	if (g_vm.connected())
		return true;
	if (!g_vm.connect_to("127.0.0.1", 59129, "/v1/"))
		return false;

	g_vm.send_text(std::string("{\"id\":\"vx-reg\",\"action\":\"registerClient\",\"payload\":{\"clientKey\":\"") +
		       VOICEMOD_CLIENT_KEY + "\"}}");
	std::string reply;
	if (!g_vm.recv_text(reply) || reply.find("\"code\":") == std::string::npos || json_num(reply, "code") != 200) {
		obs_log(LOG_WARNING, "Voicemod : enregistrement refusé");
		g_vm.close();
		return false;
	}
	obs_log(LOG_INFO, "Voicemod : connecté");
	return true;
}

void voicemod_load(const std::string &voiceId)
{
	if (voiceId.empty() || !voicemod_ensure())
		return;
	g_vm.send_text("{\"id\":\"vx-load\",\"action\":\"loadVoice\",\"payload\":{\"voiceID\":\"" + voiceId + "\"}}");
}

void handle_event(const std::string &json)
{
	const std::string type = json_str(json, "type");
	if (type != "voicemod_voice")
		return;

	const std::string voice = json_str(json, "voiceId");
	const long secs = json_num(json, "durationSec");
	if (voice.empty())
		return;

	voicemod_load(voice);
	if (secs > 0) {
		// « nofx » = voix normale. On ne mémorise pas la voix précédente :
		// Voicemod ne la renvoie pas de façon fiable, et restaurer une voix
		// erronée serait pire que revenir au neutre.
		g_prevVoice = "nofx";
		g_restoreAt = std::chrono::steady_clock::now() + std::chrono::seconds(secs);
	} else {
		g_restoreAt = {};
	}
}

void tick_restore()
{
	if (g_restoreAt.time_since_epoch().count() == 0)
		return;
	if (std::chrono::steady_clock::now() < g_restoreAt)
		return;
	g_restoreAt = {};
	voicemod_load(g_prevVoice);
}

} // namespace

void vx_events_set_token(const std::string &token)
{
	std::lock_guard<std::mutex> lk(g_mu);
	g_token = token;
}

#ifdef _WIN32
#include <windows.h>
#include <wininet.h>
#pragma comment(lib, "wininet.lib")

namespace {

// ⚠️ InternetReadFile BLOQUE jusqu'à l'arrivée d'octets. Baisser `g_run` ne
// suffit donc pas : le fil resterait coincé et `join()` figerait la fermeture
// d'OBS — le symptôme « OBS ne se ferme plus » que ce dépôt connaît déjà.
// On garde la poignée pour pouvoir la fermer depuis l'extérieur, ce qui fait
// échouer la lecture immédiatement.
std::mutex g_reqMu;
void *g_req = nullptr;

void set_req(void *h)
{
	std::lock_guard<std::mutex> lk(g_reqMu);
	g_req = h;
}

void run_stream()
{
	VxSseParser parser;
	while (g_run) {
		std::string token;
		{
			std::lock_guard<std::mutex> lk(g_mu);
			token = g_token;
		}
		if (token.empty()) {
			std::this_thread::sleep_for(std::chrono::seconds(5));
			continue;
		}

		HINTERNET net = InternetOpenA("vx-stream-plugin", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
		if (net) {
			// Filet de sécurité en plus de la fermeture de poignée : même si
			// celle-ci échouait, la lecture rendrait la main. Plus long que le
			// battement de 25 s du serveur, sinon on se reconnecterait sans fin.
			DWORD to = 40000;
			InternetSetOptionA(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
			InternetSetOptionA(net, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
			HINTERNET conn = InternetConnectA(net, "valerix.stream", INTERNET_DEFAULT_HTTPS_PORT, nullptr,
							  nullptr, INTERNET_SERVICE_HTTP, 0, 0);
			if (conn) {
				const std::string path = "/api/obs/events?token=" + token;
				HINTERNET req = HttpOpenRequestA(conn, "GET", path.c_str(), nullptr, nullptr, nullptr,
								 INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE |
									 INTERNET_FLAG_KEEP_CONNECTION,
								 0);
				set_req(req);
				if (req && HttpSendRequestA(req, nullptr, 0, nullptr, 0)) {
					char buf[2048];
					DWORD n = 0;
					// InternetReadFile rend la main dès qu'il y a des octets :
					// c'est ce qui rend le flux exploitable en direct.
					while (g_run && InternetReadFile(req, buf, sizeof(buf), &n) && n > 0) {
						for (const std::string &ev : parser.feed(std::string(buf, n)))
							handle_event(ev);
						tick_restore();
					}
				}
				set_req(nullptr);
				if (req)
					InternetCloseHandle(req);
				InternetCloseHandle(conn);
			}
			InternetCloseHandle(net);
		}
		// Coupure (réseau, veille, redémarrage serveur) : on repart après une
		// pause, sans boucler à vide si le serveur refuse.
		for (int i = 0; i < 10 && g_run; ++i) {
			tick_restore();
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
	}
}

} // namespace
#else
namespace {
// Le flux n'est câblé que sur Windows, comme le reste du réseau du plugin.
void run_stream()
{
	while (g_run) {
		tick_restore();
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}
}
} // namespace
#endif

void vx_events_start(void)
{
	if (g_run)
		return;
	g_run = true;
	g_thread = std::thread(run_stream);
}

void vx_events_stop(void)
{
	if (!g_run)
		return;
	g_run = false;
#ifdef _WIN32
	// Débloque une lecture en cours : sans ça, `join()` attendrait le prochain
	// octet du serveur — jusqu'à 25 s, ou indéfiniment si le réseau est tombé.
	{
		std::lock_guard<std::mutex> lk(g_reqMu);
		if (g_req)
			InternetCloseHandle((HINTERNET)g_req);
		g_req = nullptr;
	}
#endif
	if (g_thread.joinable())
		g_thread.join();
	g_vm.close();
}
