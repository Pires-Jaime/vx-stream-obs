/*
VX.Stream pour OBS — pont Voicemod (Control API locale)
SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <string>

/**
 * Client WebSocket minimal, sans TLS ni dépendance.
 *
 * Écrit à la main parce que Qt6 WebSockets n'est PAS fourni par obs-deps, et
 * que la cible est 127.0.0.1 : ni chiffrement ni vérification de pair à gérer.
 * Ne couvre que ce dont Voicemod a besoin — trames texte, ping/pong, fermeture.
 */
class VxWebSocket {
public:
	~VxWebSocket() { close(); }

	bool connect_to(const char *host, unsigned short port, const char *path);
	bool send_text(const std::string &payload);
	/** true = `out` contient une trame texte. false = connexion perdue. */
	bool recv_text(std::string &out);
	void close();
	bool connected() const { return sock != -1; }

private:
	bool pump();
	long long sock = -1;
	std::string rx; // octets reçus pas encore consommés
};
