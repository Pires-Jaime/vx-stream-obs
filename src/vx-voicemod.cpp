/*
VX.Stream pour OBS — pont Voicemod (Control API locale)
SPDX-License-Identifier: GPL-2.0-or-later

POURQUOI DU WEBSOCKET FAIT MAIN : la Control API de Voicemod écoute sur
ws://localhost:59129/v1/ — donc sur le PC du streamer, hors de portée de nos
serveurs. Seul un composant local peut l'atteindre : une page https ne le peut
pas (contenu mixte), et Qt6 WebSockets n'est PAS fourni par obs-deps (vérifié
dans deps.qt/qt6.ps1 : qtbase, qtimageformats, qtshadertools, qtmultimedia,
qtsvg, qttools — pas de qtwebsockets). D'où ce client minimal, ~200 lignes,
sans dépendance, qui ne gère que ce dont on a besoin : un handshake, des trames
texte, et un ping/pong.

Pas de TLS : la cible est 127.0.0.1, en clair, par conception de Voicemod.
*/
#include "vx-voicemod.hpp"

#include <cstring>
#include <cstdio>
#include <random>
#include <sstream>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
// MSVC : même méthode que vx-updater.cpp pour WinInet — la directive de
// liaison vit dans la source, pas dans CMakeLists.
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET vx_sock_t;
#define VX_INVALID_SOCK INVALID_SOCKET
#define vx_close_sock closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int vx_sock_t;
#define VX_INVALID_SOCK (-1)
#define vx_close_sock ::close
#endif

namespace {

// ── Base64 (pour Sec-WebSocket-Key) ───────────────────────────────────────────
const char *B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const unsigned char *data, size_t len)
{
	std::string out;
	for (size_t i = 0; i < len; i += 3) {
		unsigned v = data[i] << 16;
		if (i + 1 < len)
			v |= data[i + 1] << 8;
		if (i + 2 < len)
			v |= data[i + 2];
		out += B64[(v >> 18) & 63];
		out += B64[(v >> 12) & 63];
		out += (i + 1 < len) ? B64[(v >> 6) & 63] : '=';
		out += (i + 2 < len) ? B64[v & 63] : '=';
	}
	return out;
}

bool send_all(vx_sock_t s, const char *buf, size_t len)
{
	size_t sent = 0;
	while (sent < len) {
		int n = (int)::send(s, buf + sent, (int)(len - sent), 0);
		if (n <= 0)
			return false;
		sent += (size_t)n;
	}
	return true;
}

} // namespace

// ── Connexion + handshake ────────────────────────────────────────────────────
bool VxWebSocket::connect_to(const char *host, unsigned short port, const char *path)
{
#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return false;
#endif
	sock = (long long)::socket(AF_INET, SOCK_STREAM, 0);
	if ((vx_sock_t)sock == VX_INVALID_SOCK)
		return false;

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		close();
		return false;
	}
	if (::connect((vx_sock_t)sock, (sockaddr *)&addr, sizeof(addr)) != 0) {
		close();
		return false;
	}
	int one = 1;
	::setsockopt((vx_sock_t)sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));

	// Clé aléatoire : le serveur renvoie son accept, qu'on ne vérifie pas —
	// on parle à 127.0.0.1, l'enjeu n'est pas l'authenticité du pair.
	unsigned char key[16];
	std::random_device rd;
	for (unsigned char &b : key)
		b = (unsigned char)(rd() & 0xff);

	std::ostringstream req;
	req << "GET " << path << " HTTP/1.1\r\n"
	    << "Host: " << host << ":" << port << "\r\n"
	    << "Upgrade: websocket\r\n"
	    << "Connection: Upgrade\r\n"
	    << "Sec-WebSocket-Key: " << base64(key, 16) << "\r\n"
	    << "Sec-WebSocket-Version: 13\r\n\r\n";
	const std::string r = req.str();
	if (!send_all((vx_sock_t)sock, r.data(), r.size())) {
		close();
		return false;
	}

	// Réponse : on lit jusqu'à la fin des en-têtes. Tout octet lu au-delà
	// appartient déjà aux trames — le garder est INDISPENSABLE, Voicemod peut
	// envoyer son premier message dans le même paquet TCP.
	std::string head;
	char buf[1024];
	while (head.find("\r\n\r\n") == std::string::npos) {
		int n = (int)::recv((vx_sock_t)sock, buf, sizeof(buf), 0);
		if (n <= 0) {
			close();
			return false;
		}
		head.append(buf, (size_t)n);
		if (head.size() > 16384) {
			close();
			return false;
		}
	}
	if (head.find(" 101") == std::string::npos) {
		close();
		return false;
	}
	const size_t end = head.find("\r\n\r\n") + 4;
	rx.assign(head.begin() + (long)end, head.end());
	return true;
}

void VxWebSocket::close()
{
	if ((vx_sock_t)sock != VX_INVALID_SOCK) {
		vx_close_sock((vx_sock_t)sock);
		sock = (long long)VX_INVALID_SOCK;
	}
	rx.clear();
}

// ── Envoi d'une trame texte (masquée : obligatoire côté client) ──────────────
bool VxWebSocket::send_text(const std::string &payload)
{
	if ((vx_sock_t)sock == VX_INVALID_SOCK)
		return false;
	std::string f;
	f += (char)0x81; // FIN + opcode texte

	const size_t n = payload.size();
	if (n < 126) {
		f += (char)(0x80 | n);
	} else if (n <= 0xffff) {
		f += (char)(0x80 | 126);
		f += (char)((n >> 8) & 0xff);
		f += (char)(n & 0xff);
	} else {
		f += (char)(0x80 | 127);
		for (int i = 7; i >= 0; --i)
			f += (char)((n >> (i * 8)) & 0xff);
	}

	std::random_device rd;
	unsigned char mask[4];
	for (unsigned char &m : mask)
		m = (unsigned char)(rd() & 0xff);
	f.append((const char *)mask, 4);

	for (size_t i = 0; i < n; ++i)
		f += (char)(payload[i] ^ mask[i % 4]);

	return send_all((vx_sock_t)sock, f.data(), f.size());
}

// ── Réception d'une trame texte complète ─────────────────────────────────────
// Renvoie false si la connexion est tombée. `out` vide = rien d'exploitable
// (ping, trame de contrôle) : l'appelant boucle.
bool VxWebSocket::recv_text(std::string &out)
{
	out.clear();
	for (;;) {
		// Assez d'octets pour l'en-tête minimal ?
		while (rx.size() < 2) {
			if (!pump())
				return false;
		}
		const unsigned char b0 = (unsigned char)rx[0];
		const unsigned char b1 = (unsigned char)rx[1];
		const unsigned opcode = b0 & 0x0f;
		const bool masked = (b1 & 0x80) != 0; // un serveur ne masque pas
		size_t len = b1 & 0x7f;
		size_t off = 2;

		if (len == 126) {
			while (rx.size() < off + 2)
				if (!pump())
					return false;
			len = ((unsigned char)rx[off] << 8) | (unsigned char)rx[off + 1];
			off += 2;
		} else if (len == 127) {
			while (rx.size() < off + 8)
				if (!pump())
					return false;
			len = 0;
			for (int i = 0; i < 8; ++i)
				len = (len << 8) | (unsigned char)rx[off + (size_t)i];
			off += 8;
		}
		if (masked)
			off += 4;

		while (rx.size() < off + len)
			if (!pump())
				return false;

		std::string payload = rx.substr(off, len);
		if (masked) {
			const char *m = rx.data() + off - 4;
			for (size_t i = 0; i < payload.size(); ++i)
				payload[i] = (char)(payload[i] ^ m[i % 4]);
		}
		rx.erase(0, off + len);

		if (opcode == 0x1) { // texte
			out = payload;
			return true;
		}
		if (opcode == 0x8) { // fermeture
			close();
			return false;
		}
		if (opcode == 0x9) { // ping → pong, sinon Voicemod nous coupe
			std::string p;
			p += (char)0x8a;
			p += (char)(0x80 | payload.size());
			unsigned char mk[4] = {0, 0, 0, 0};
			p.append((const char *)mk, 4);
			p += payload;
			if (!send_all((vx_sock_t)sock, p.data(), p.size()))
				return false;
		}
		// autres opcodes (pong, binaire) : ignorés, on reboucle
	}
}

bool VxWebSocket::pump()
{
	char buf[4096];
	const int n = (int)::recv((vx_sock_t)sock, buf, sizeof(buf), 0);
	if (n <= 0) {
		close();
		return false;
	}
	rx.append(buf, (size_t)n);
	return true;
}
