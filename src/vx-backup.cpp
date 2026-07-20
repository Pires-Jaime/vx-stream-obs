/*
VX.Stream pour OBS — VX.Backup : sauvegarde de la configuration OBS
Copyright (C) 2026 Valerix (Jaime Pires) <support@valerix.stream>
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Sauvegarde les COLLECTIONS DE SCÈNES d'OBS (scènes, sources, dispositions,
// filtres) sur le serveur Valerix, pour les retrouver après un changement de PC.
//
// Choix v1 volontairement SÛR : on ne sauvegarde que les collections de scènes
// (basic/scenes/*.json). Elles ne contiennent AUCUNE clé de stream (celles-ci
// vivent dans les profils, basic/profiles/*/service.json, qu'on n'envoie pas).
// Le streamer ressaisit ses clés une fois — le reste (tout son décor) revient.
//
// L'authentification se fait par un « code VX.Backup » (jeton par utilisateur)
// que le streamer copie depuis valerix.stream/obs et colle ici. Le plugin ne
// connaît pas la session web ; ce code est le pont, révocable côté dashboard.
//
// Réseau : WinINet (comme l'updater), synchrone sur clic utilisateur (jamais à
// la fermeture) → aucune rétention à l'unload. Windows uniquement.

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QAction>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <cstring>
#include <string>

#include "vx-backup.hpp"

namespace {

std::string token_file()
{
	char *p = obs_module_config_path("backup-token.txt");
	std::string s = p ? p : "";
	bfree(p);
	return s;
}

std::string load_token()
{
	const std::string f = token_file();
	if (f.empty())
		return {};
	char *c = os_quick_read_utf8_file(f.c_str());
	if (!c)
		return {};
	std::string s(c);
	bfree(c);
	// Retire les espaces/retours parasites d'un copier-coller.
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
		s.pop_back();
	return s;
}

void save_token(const std::string &tok)
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	const std::string f = token_file();
	if (!f.empty())
		os_quick_write_utf8_file(f.c_str(), tok.c_str(), tok.size(), false);
}

// Répertoire des collections de scènes OBS (basic/scenes).
std::string scenes_dir()
{
	char path[1024];
	if (os_get_config_path(path, sizeof(path), "obs-studio/basic/scenes") <= 0)
		return {};
	return path;
}

} // namespace

// ── Réseau (Windows / WinINet) ───────────────────────────────────────────────
#ifdef _WIN32
#include <windows.h>
#include <wininet.h>

namespace {

struct HttpResult {
	bool ok = false;
	long status = 0;
	std::string body;
};

// Requête HTTPS générique. `extraHeaders` = lignes "Clé: valeur\r\n". Le corps
// (facultatif) part tel quel. Timeouts courts : action utilisateur, pas la peine
// de bloquer OBS longtemps.
HttpResult http_request(const char *verb, const std::string &path, const std::string &token,
			const std::string &extraHeaders = {}, const std::string &body = {})
{
	static const char *HOST = "valerix.stream";
	HttpResult res;
	HINTERNET net = InternetOpenA("vx-stream-plugin", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
	if (!net)
		return res;
	DWORD t = 15000;
	InternetSetOptionA(net, INTERNET_OPTION_CONNECT_TIMEOUT, &t, sizeof(t));
	InternetSetOptionA(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &t, sizeof(t));
	InternetSetOptionA(net, INTERNET_OPTION_SEND_TIMEOUT, &t, sizeof(t));
	HINTERNET conn =
		InternetConnectA(net, HOST, INTERNET_DEFAULT_HTTPS_PORT, nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
	if (conn) {
		HINTERNET req = HttpOpenRequestA(conn, verb, path.c_str(), nullptr, nullptr, nullptr,
						 INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_CACHE_WRITE, 0);
		if (req) {
			std::string headers = "Authorization: Bearer " + token + "\r\n" + extraHeaders;
			if (HttpSendRequestA(req, headers.c_str(), (DWORD)headers.size(),
					     body.empty() ? nullptr : (LPVOID)body.data(), (DWORD)body.size())) {
				DWORD code = 0, len = sizeof(code);
				HttpQueryInfoA(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &len,
					       nullptr);
				res.status = (long)code;
				res.ok = code >= 200 && code < 300;
				char buf[4096];
				DWORD n = 0;
				while (InternetReadFile(req, buf, sizeof(buf), &n) && n > 0)
					res.body.append(buf, n);
			}
			InternetCloseHandle(req);
		}
		InternetCloseHandle(conn);
	}
	InternetCloseHandle(net);
	return res;
}

} // namespace
#else
namespace {
struct HttpResult {
	bool ok = false;
	long status = 0;
	std::string body;
};
HttpResult http_request(const char *, const std::string &, const std::string &, const std::string & = {},
			const std::string & = {})
{
	return {};
}
} // namespace
#endif

// ── Assemblage / restauration des collections de scènes ──────────────────────

namespace {

// Construit le blob JSON { obs_version, scenes: [{file, content}] }.
std::string build_backup_payload()
{
	const std::string dir = scenes_dir();
	obs_data_t *root = obs_data_create();
	obs_data_set_string(root, "obs_version", obs_get_version_string());
	obs_data_array_t *arr = obs_data_array_create();

	os_dir_t *d = dir.empty() ? nullptr : os_opendir(dir.c_str());
	if (d) {
		struct os_dirent *ent;
		while ((ent = os_readdir(d)) != nullptr) {
			if (ent->directory)
				continue;
			std::string name = ent->d_name;
			if (name.size() < 6 || name.substr(name.size() - 5) != ".json")
				continue;
			const std::string full = dir + "/" + name;
			char *content = os_quick_read_utf8_file(full.c_str());
			if (!content)
				continue;
			obs_data_t *o = obs_data_create();
			obs_data_set_string(o, "file", name.c_str());
			obs_data_set_string(o, "content", content);
			obs_data_array_push_back(arr, o);
			obs_data_release(o);
			bfree(content);
		}
		os_closedir(d);
	}

	obs_data_set_array(root, "scenes", arr);
	const char *json = obs_data_get_json(root);
	std::string payload = json ? json : "";
	obs_data_array_release(arr);
	obs_data_release(root);
	return payload;
}

// Écrit les collections de scènes reçues dans basic/scenes. Renvoie le nombre
// de fichiers restaurés.
int restore_backup_payload(const std::string &json)
{
	const std::string dir = scenes_dir();
	if (dir.empty())
		return 0;
	os_mkdirs(dir.c_str());

	obs_data_t *root = obs_data_create_from_json(json.c_str());
	if (!root)
		return 0;
	obs_data_array_t *arr = obs_data_get_array(root, "scenes");
	int written = 0;
	const size_t n = arr ? obs_data_array_count(arr) : 0;
	for (size_t i = 0; i < n; i++) {
		obs_data_t *o = obs_data_array_item(arr, i);
		const char *file = obs_data_get_string(o, "file");
		const char *content = obs_data_get_string(o, "content");
		// Garde-fou anti-traversée : un nom de fichier simple, pas de chemin.
		std::string fname = file ? file : "";
		if (!fname.empty() && content && fname.find('/') == std::string::npos &&
		    fname.find('\\') == std::string::npos && fname.find("..") == std::string::npos) {
			const std::string full = dir + "/" + fname;
			if (os_quick_write_utf8_file(full.c_str(), content, strlen(content), false))
				written++;
		}
		obs_data_release(o);
	}
	if (arr)
		obs_data_array_release(arr);
	obs_data_release(root);
	return written;
}

// ── Dialogue ─────────────────────────────────────────────────────────────────

class BackupDialog : public QDialog {
public:
	explicit BackupDialog(QWidget *parent) : QDialog(parent)
	{
		setWindowTitle(QStringLiteral("VX.Backup — sauvegarde de la configuration"));
		setMinimumSize(460, 420);
		auto *root = new QVBoxLayout(this);

		auto *info = new QLabel(
			QStringLiteral("Sauvegardez vos <b>scènes et sources</b> sur le serveur Valerix, "
				       "et restaurez-les sur un autre PC.<br>Collez votre <b>code VX.Backup</b> "
				       "(depuis valerix.stream/obs) :"),
			this);
		info->setWordWrap(true);
		root->addWidget(info);

		auto *tokRow = new QHBoxLayout();
		tokenEdit = new QLineEdit(QString::fromStdString(load_token()), this);
		tokenEdit->setEchoMode(QLineEdit::Password);
		tokenEdit->setPlaceholderText(QStringLiteral("vxb_…"));
		auto *saveTok = new QPushButton(QStringLiteral("Enregistrer"), this);
		connect(saveTok, &QPushButton::clicked, this, [this] {
			save_token(tokenEdit->text().trimmed().toStdString());
			refreshList();
		});
		tokRow->addWidget(tokenEdit, 1);
		tokRow->addWidget(saveTok);
		root->addLayout(tokRow);

		auto *backupBtn = new QPushButton(QStringLiteral("⬆  Sauvegarder ma configuration maintenant"), this);
		connect(backupBtn, &QPushButton::clicked, this, [this] { doBackup(); });
		root->addWidget(backupBtn);

		root->addWidget(new QLabel(QStringLiteral("Sauvegardes disponibles :"), this));
		list = new QListWidget(this);
		root->addWidget(list, 1);

		auto *restoreBtn = new QPushButton(QStringLiteral("⬇  Restaurer la sauvegarde sélectionnée"), this);
		connect(restoreBtn, &QPushButton::clicked, this, [this] { doRestore(); });
		root->addWidget(restoreBtn);

		status = new QLabel(this);
		status->setWordWrap(true);
		status->setStyleSheet(QStringLiteral("color: #888; font-size: 11px;"));
		root->addWidget(status);

		refreshList();
	}

private:
	QLineEdit *tokenEdit = nullptr;
	QListWidget *list = nullptr;
	QLabel *status = nullptr;

	std::string token() { return tokenEdit->text().trimmed().toStdString(); }

	void setStatus(const QString &s, bool err = false)
	{
		status->setText(s);
		status->setStyleSheet(err ? QStringLiteral("color: #ef4444; font-size: 11px;")
					  : QStringLiteral("color: #22c55e; font-size: 11px;"));
	}

	void refreshList()
	{
		list->clear();
		const std::string tok = token();
		if (tok.empty())
			return;
		HttpResult r = http_request("GET", "/api/vx-backup/manifest", tok);
		if (r.status == 401) {
			// Cas vécu : le code a été régénéré côté site → celui d'OBS ne vaut
			// plus rien. Les SAUVEGARDES, elles, restent liées au compte.
			setStatus(QStringLiteral("Code invalide ou périmé. Recopiez celui affiché sur "
						 "valerix.stream/obs — vos sauvegardes existantes seront "
						 "retrouvées, rien n'est perdu."),
				  true);
			return;
		}
		if (r.status == 403) {
			setStatus(QStringLiteral("VX.Backup nécessite un abonnement Valerix (Light minimum)."), true);
			return;
		}
		if (!r.ok) {
			setStatus(QStringLiteral("Serveur injoignable (code %1).").arg(r.status), true);
			return;
		}
		obs_data_t *root = obs_data_create_from_json(r.body.c_str());
		if (!root)
			return;
		obs_data_array_t *arr = obs_data_get_array(root, "backups");
		const size_t n = arr ? obs_data_array_count(arr) : 0;
		for (size_t i = 0; i < n; i++) {
			obs_data_t *o = obs_data_array_item(arr, i);
			QString label = QString::fromUtf8(obs_data_get_string(o, "label"));
			if (label.isEmpty())
				label = QStringLiteral("Sauvegarde OBS");
			QString when = QString::fromUtf8(obs_data_get_string(o, "createdAt"));
			auto *item = new QListWidgetItem(QStringLiteral("%1  —  %2").arg(label, when.left(19)));
			item->setData(Qt::UserRole, QString::fromUtf8(obs_data_get_string(o, "id")));
			list->addItem(item);
			obs_data_release(o);
		}
		if (arr)
			obs_data_array_release(arr);
		obs_data_release(root);
	}

	void doBackup()
	{
		const std::string tok = token();
		if (tok.empty()) {
			setStatus(QStringLiteral("Collez d'abord votre code VX.Backup."), true);
			return;
		}
		save_token(tok);
		const std::string payload = build_backup_payload();
		if (payload.size() < 4) {
			setStatus(QStringLiteral("Aucune collection de scènes à sauvegarder."), true);
			return;
		}

		char host[256] = "cet ordinateur";
#ifdef _WIN32
		DWORD hs = sizeof(host);
		GetComputerNameA(host, &hs);
#endif
		std::string headers = "Content-Type: application/octet-stream\r\n";
		headers += std::string("X-VX-Machine: ") + host + "\r\n";
		headers += std::string("X-VX-OBS: ") + obs_get_version_string() + "\r\n";
		headers += "X-VX-Label: Configuration OBS\r\n";

		setStatus(QStringLiteral("Envoi en cours…"));
		HttpResult r = http_request("POST", "/api/vx-backup/push", tok, headers, payload);
		if (r.ok) {
			setStatus(QStringLiteral("✓ Sauvegarde envoyée."));
			refreshList();
		} else if (r.status == 401) {
			setStatus(QStringLiteral("Code invalide ou périmé — recopiez celui de valerix.stream/obs."),
				  true);
		} else if (r.status == 403) {
			setStatus(QStringLiteral("VX.Backup nécessite un abonnement Valerix (Light minimum)."), true);
		} else if (r.status == 413) {
			setStatus(QStringLiteral("Configuration trop volumineuse (max 25 Mo)."), true);
		} else {
			setStatus(QStringLiteral("Échec de l'envoi (code %1).").arg(r.status), true);
		}
	}

	void doRestore()
	{
		QListWidgetItem *it = list->currentItem();
		if (!it) {
			setStatus(QStringLiteral("Sélectionnez une sauvegarde dans la liste."), true);
			return;
		}
		if (QMessageBox::question(
			    this, QStringLiteral("Restaurer"),
			    QStringLiteral("Restaurer cette sauvegarde ? Vos collections de scènes du même nom "
					   "seront remplacées.\n\nRedémarrez OBS ensuite, puis choisissez la "
					   "collection dans le menu « Collection de scènes ».")) != QMessageBox::Yes)
			return;

		const std::string id = it->data(Qt::UserRole).toString().toStdString();
		HttpResult r = http_request("GET", "/api/vx-backup/pull/" + id, token());
		if (!r.ok) {
			setStatus(QStringLiteral("Téléchargement impossible (code %1).").arg(r.status), true);
			return;
		}
		int written = restore_backup_payload(r.body);
		if (written > 0)
			setStatus(QStringLiteral("✓ %1 collection(s) restaurée(s). Redémarrez OBS.").arg(written));
		else
			setStatus(QStringLiteral("Rien n'a pu être restauré."), true);
	}
};

BackupDialog *dialog = nullptr;

void show_dialog()
{
	if (!dialog) {
		auto *window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		dialog = new BackupDialog(window);
	}
	dialog->show();
	dialog->raise();
	dialog->activateWindow();
}

} // namespace

void vx_backup_add_menu(QMenu *menu)
{
	QAction *a = menu->addAction(QStringLiteral("VX.Backup — sauvegarder ma config…"));
	QObject::connect(a, &QAction::triggered, [] { show_dialog(); });
}
