/*
VX.Stream pour OBS — dock VX Vertical
Copyright (C) 2026 Valerix (Jaime Pires) <support@valerix.stream>
SPDX-License-Identifier: GPL-2.0-or-later
*/

// Le dock du canvas vertical : l'APERÇU 9:16 EN DIRECT (le « visuel du canvas
// mobile »), les scènes verticales, leurs sources (presets Remplir/Adapter —
// pas de drag à la souris en v1), et la sortie RTMP dédiée.
//
// L'aperçu est un obs_display embarqué dans un QWidget à fenêtre NATIVE
// (pattern des projecteurs OBS) : le draw callback tourne sur le thread
// graphique et rend obs_render_canvas_texture dans un viewport letterboxé.
// Destruction : obs_display_destroy AVANT la libération du canvas (EXIT).

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QCheckBox>
#include <QComboBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QResizeEvent>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "vx-vertical.hpp"
#include "vx-vertical-dock.hpp"

namespace {

constexpr const char *DOCK_ID = "vx_vertical";

// ── Aperçu : QWidget natif + obs_display ─────────────────────────────────────

class VertPreview : public QWidget {
public:
	explicit VertPreview(QWidget *parent) : QWidget(parent)
	{
		setAttribute(Qt::WA_NativeWindow);
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_NoSystemBackground);
		setMinimumHeight(240);
	}

	~VertPreview() override { destroyDisplay(); }

	QPaintEngine *paintEngine() const override { return nullptr; }

	void destroyDisplay()
	{
		if (display) {
			obs_display_remove_draw_callback(display, &VertPreview::draw, nullptr);
			obs_display_destroy(display);
			display = nullptr;
		}
	}

protected:
	void showEvent(QShowEvent *e) override
	{
		QWidget::showEvent(e);
		ensureDisplay();
	}

	void resizeEvent(QResizeEvent *e) override
	{
		QWidget::resizeEvent(e);
		if (display) {
			const qreal d = devicePixelRatioF();
			obs_display_resize(display, (uint32_t)(width() * d), (uint32_t)(height() * d));
		}
	}

private:
	obs_display_t *display = nullptr;

	void ensureDisplay()
	{
		if (display)
			return;
		const qreal d = devicePixelRatioF();
		gs_init_data info = {};
		info.cx = (uint32_t)(width() * d);
		info.cy = (uint32_t)(height() * d);
		info.format = GS_BGRA;
		info.zsformat = GS_ZS_NONE;
#if defined(_WIN32)
		info.window.hwnd = (void *)winId();
#elif defined(__APPLE__)
		info.window.view = (id)(uintptr_t)winId();
#elif defined(__linux__) || defined(__FreeBSD__)
		// X11 uniquement (comme OBS : pas d'embarquement sous Wayland).
		auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
		if (!x11)
			return; // Wayland → pas d'aperçu, le reste du dock fonctionne
		info.window.id = (uint32_t)winId();
		info.window.display = x11->display();
#endif
		display = obs_display_create(&info, 0x0C0A14);
		if (display)
			obs_display_add_draw_callback(display, &VertPreview::draw, nullptr);
	}

	static void draw(void *, uint32_t cx, uint32_t cy)
	{
		obs_canvas_t *canvas = vx_vert_canvas();
		if (!canvas)
			return;
		struct obs_video_info ovi;
		if (!obs_canvas_get_video_info(canvas, &ovi))
			return;

		// Letterbox 9:16 centré dans le widget.
		const float cw = (float)ovi.base_width, ch = (float)ovi.base_height;
		const float scale = ((float)cx / cw < (float)cy / ch) ? (float)cx / cw : (float)cy / ch;
		const int vw = (int)(cw * scale), vh = (int)(ch * scale);
		const int vx = ((int)cx - vw) / 2, vy = ((int)cy - vh) / 2;

		gs_viewport_push();
		gs_projection_push();
		gs_ortho(0.0f, cw, 0.0f, ch, -100.0f, 100.0f);
		gs_set_viewport(vx, vy, vw, vh);
		obs_render_canvas_texture(canvas);
		gs_projection_pop();
		gs_viewport_pop();
	}
};

// ── Helpers scènes/sources ───────────────────────────────────────────────────

obs_scene_t *current_scene()
{
	obs_canvas_t *canvas = vx_vert_canvas();
	if (!canvas)
		return nullptr;
	obs_source_t *ch = obs_canvas_get_channel(canvas, 0);
	obs_scene_t *scene = ch ? obs_scene_from_source(ch) : nullptr;
	if (ch)
		obs_source_release(ch); // la scène reste retenue par le canal
	return scene;
}

std::vector<obs_sceneitem_t *> scene_items(obs_scene_t *scene)
{
	std::vector<obs_sceneitem_t *> out;
	if (!scene)
		return out;
	obs_scene_enum_items(
		scene,
		[](obs_scene_t *, obs_sceneitem_t *item, void *p) {
			static_cast<std::vector<obs_sceneitem_t *> *>(p)->push_back(item);
			return true;
		},
		&out);
	return out;
}

void apply_bounds(obs_sceneitem_t *item, bool fill)
{
	obs_transform_info ti;
	obs_sceneitem_get_info2(item, &ti);
	ti.pos.x = 540.0f;
	ti.pos.y = 960.0f;
	ti.rot = 0.0f;
	ti.scale.x = 1.0f;
	ti.scale.y = 1.0f;
	ti.alignment = OBS_ALIGN_CENTER;
	ti.bounds_type = fill ? OBS_BOUNDS_SCALE_OUTER : OBS_BOUNDS_SCALE_INNER;
	ti.bounds_alignment = OBS_ALIGN_CENTER;
	ti.bounds.x = 1080.0f;
	ti.bounds.y = 1920.0f;
	ti.crop_to_bounds = fill; // Remplir : recadre ce qui déborde du 9:16
	obs_sceneitem_set_info2(item, &ti);
}

const char *camera_source_id()
{
#if defined(_WIN32)
	return "dshow_input";
#elif defined(__APPLE__)
	return "macos-avcapture";
#else
	return "v4l2_input";
#endif
}

// ── Le dock ──────────────────────────────────────────────────────────────────

class VertDockWidget : public QWidget {
public:
	VertDockWidget()
	{
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(8, 8, 8, 8);
		root->setSpacing(6);

		preview = new VertPreview(this);
		root->addWidget(preview, 1);

		// Scènes verticales.
		auto *sceneRow = new QHBoxLayout();
		sceneCombo = new QComboBox(this);
		connect(sceneCombo, &QComboBox::activated, this, [this](int) { onSceneSelected(); });
		auto *addScene = new QPushButton(QStringLiteral("＋"), this);
		addScene->setFixedWidth(28);
		addScene->setToolTip(QStringLiteral("Nouvelle scène verticale"));
		connect(addScene, &QPushButton::clicked, this, [this] { onAddScene(); });
		auto *delScene = new QPushButton(QStringLiteral("−"), this);
		delScene->setFixedWidth(28);
		delScene->setToolTip(QStringLiteral("Supprimer la scène verticale"));
		connect(delScene, &QPushButton::clicked, this, [this] { onRemoveScene(); });
		sceneRow->addWidget(new QLabel(QStringLiteral("Scène"), this));
		sceneRow->addWidget(sceneCombo, 1);
		sceneRow->addWidget(addScene);
		sceneRow->addWidget(delScene);
		root->addLayout(sceneRow);

		// Sources de la scène.
		list = new QListWidget(this);
		list->setMaximumHeight(110);
		root->addWidget(list);

		auto *srcRow = new QHBoxLayout();
		auto *addSrc = new QPushButton(QStringLiteral("＋ Source"), this);
		auto *menu = new QMenu(addSrc);
		menu->addAction(QStringLiteral("Scène principale (recadrée)"), [this] { onAddMainScene(); });
		menu->addAction(QStringLiteral("Caméra"), [this] { onAddCamera(); });
		menu->addAction(QStringLiteral("Source navigateur (URL)…"), [this] { onAddBrowser(); });
		addSrc->setMenu(menu);
		auto *delSrc = new QPushButton(QStringLiteral("−"), this);
		delSrc->setFixedWidth(28);
		delSrc->setToolTip(QStringLiteral("Retirer la source sélectionnée"));
		connect(delSrc, &QPushButton::clicked, this, [this] { onRemoveSource(); });
		auto *fill = new QPushButton(QStringLiteral("Remplir"), this);
		fill->setToolTip(QStringLiteral("Remplit le 9:16 (recadre les bords)"));
		connect(fill, &QPushButton::clicked, this, [this] { onPreset(true); });
		auto *fit = new QPushButton(QStringLiteral("Adapter"), this);
		fit->setToolTip(QStringLiteral("Tout visible (bandes possibles)"));
		connect(fit, &QPushButton::clicked, this, [this] { onPreset(false); });
		srcRow->addWidget(addSrc);
		srcRow->addWidget(delSrc);
		srcRow->addStretch(1);
		srcRow->addWidget(fill);
		srcRow->addWidget(fit);
		root->addLayout(srcRow);

		// Sortie RTMP verticale.
		const VxVertSettings s = vx_vert_get_settings();
		server = new QLineEdit(QString::fromStdString(s.server), this);
		server->setPlaceholderText(QStringLiteral("rtmp(s)://… (TikTok Live, YouTube…)"));
		key = new QLineEdit(QString::fromStdString(s.key), this);
		key->setEchoMode(QLineEdit::Password);
		key->setPlaceholderText(QStringLiteral("Clé de stream"));
		root->addWidget(server);
		root->addWidget(key);

		auto *outRow = new QHBoxLayout();
		bitrate = new QSpinBox(this);
		bitrate->setRange(500, 20000);
		bitrate->setSingleStep(500);
		bitrate->setSuffix(QStringLiteral(" kbps"));
		bitrate->setValue(s.bitrate);
		autoStart = new QCheckBox(QStringLiteral("Diffuser avec le stream"), this);
		autoStart->setChecked(s.enabled);
		outRow->addWidget(bitrate);
		outRow->addWidget(autoStart, 1);
		root->addLayout(outRow);

		auto *ctlRow = new QHBoxLayout();
		status = new QLabel(this);
		status->setStyleSheet(QStringLiteral("color: #888; font-size: 10px;"));
		startBtn = new QPushButton(QStringLiteral("▶"), this);
		startBtn->setFixedWidth(34);
		startBtn->setToolTip(QStringLiteral("Démarrer la diffusion verticale"));
		connect(startBtn, &QPushButton::clicked, this, [this] { onStart(); });
		stopBtn = new QPushButton(QStringLiteral("⏹"), this);
		stopBtn->setFixedWidth(34);
		stopBtn->setToolTip(QStringLiteral("Arrêter la diffusion verticale"));
		connect(stopBtn, &QPushButton::clicked, this, [] { vx_vert_stop(); });
		ctlRow->addWidget(status, 1);
		ctlRow->addWidget(startBtn);
		ctlRow->addWidget(stopBtn);
		root->addLayout(ctlRow);

		// Persistance des réglages à chaque changement.
		auto saveSettings = [this] {
			VxVertSettings ns;
			ns.server = server->text().trimmed().toStdString();
			ns.key = key->text().trimmed().toStdString();
			ns.bitrate = bitrate->value();
			ns.enabled = autoStart->isChecked();
			vx_vert_set_settings(ns);
		};
		connect(server, &QLineEdit::editingFinished, this, saveSettings);
		connect(key, &QLineEdit::editingFinished, this, saveSettings);
		connect(bitrate, &QSpinBox::valueChanged, this, saveSettings);
		connect(autoStart, &QCheckBox::toggled, this, saveSettings);

		auto *timer = new QTimer(this);
		connect(timer, &QTimer::timeout, this, [this] { refreshStatus(); });
		timer->start(2000);

		refreshScenes();
		refreshStatus();
	}

	void destroyPreview()
	{
		if (preview)
			preview->destroyDisplay();
	}

private:
	VertPreview *preview = nullptr;
	QComboBox *sceneCombo = nullptr;
	QListWidget *list = nullptr;
	QLineEdit *server = nullptr;
	QLineEdit *key = nullptr;
	QSpinBox *bitrate = nullptr;
	QCheckBox *autoStart = nullptr;
	QLabel *status = nullptr;
	QPushButton *startBtn = nullptr;
	QPushButton *stopBtn = nullptr;

	void refreshScenes()
	{
		obs_canvas_t *canvas = vx_vert_canvas();
		sceneCombo->clear();
		if (!canvas)
			return;
		obs_scene_t *cur = current_scene();
		const char *curName = cur ? obs_source_get_name(obs_scene_get_source(cur)) : nullptr;
		struct Ctx {
			QComboBox *combo;
			const char *cur;
		} ctx{sceneCombo, curName};
		obs_canvas_enum_scenes(
			canvas,
			[](void *p, obs_source_t *src) {
				auto *c = static_cast<Ctx *>(p);
				const char *n = obs_source_get_name(src);
				c->combo->addItem(QString::fromUtf8(n ? n : "?"));
				if (c->cur && n && strcmp(c->cur, n) == 0)
					c->combo->setCurrentIndex(c->combo->count() - 1);
				return true;
			},
			&ctx);
		refreshItems();
	}

	void refreshItems()
	{
		list->clear();
		for (obs_sceneitem_t *item : scene_items(current_scene())) {
			obs_source_t *src = obs_sceneitem_get_source(item);
			const char *n = src ? obs_source_get_name(src) : "?";
			list->addItem(QString::fromUtf8(n ? n : "?"));
		}
	}

	void refreshStatus()
	{
		if (vx_vert_active()) {
			status->setText(QStringLiteral("🔴 Verticale en direct"));
			status->setStyleSheet(QStringLiteral("color: #ef4444; font-size: 10px; font-weight: 600;"));
		} else if (autoStart->isChecked()) {
			status->setText(obs_frontend_streaming_active()
						? QStringLiteral("Démarrage…")
						: QStringLiteral("Prête — démarre avec le stream"));
			status->setStyleSheet(QStringLiteral("color: #888; font-size: 10px;"));
		} else {
			status->setText(QStringLiteral("Sortie verticale désactivée"));
			status->setStyleSheet(QStringLiteral("color: #777; font-size: 10px;"));
		}
	}

	void onSceneSelected()
	{
		obs_canvas_t *canvas = vx_vert_canvas();
		if (!canvas)
			return;
		const QByteArray name = sceneCombo->currentText().toUtf8();
		obs_source_t *src = obs_canvas_get_source_by_name(canvas, name.constData());
		if (src) {
			obs_canvas_set_channel(canvas, 0, src);
			obs_source_release(src);
		}
		refreshItems();
	}

	void onAddScene()
	{
		obs_canvas_t *canvas = vx_vert_canvas();
		if (!canvas)
			return;
		bool ok = false;
		const QString name = QInputDialog::getText(this, QStringLiteral("Nouvelle scène verticale"),
							   QStringLiteral("Nom :"), QLineEdit::Normal,
							   QStringLiteral("Verticale %1").arg(sceneCombo->count() + 1),
							   &ok);
		if (!ok || name.trimmed().isEmpty())
			return;
		obs_scene_t *scene = obs_canvas_scene_create(canvas, name.trimmed().toUtf8().constData());
		if (scene)
			obs_canvas_set_channel(canvas, 0, obs_scene_get_source(scene));
		vx_vert_save();
		refreshScenes();
	}

	void onRemoveScene()
	{
		obs_canvas_t *canvas = vx_vert_canvas();
		obs_scene_t *scene = current_scene();
		if (!canvas || !scene || sceneCombo->count() <= 1)
			return; // toujours garder au moins une scène
		obs_canvas_set_channel(canvas, 0, nullptr);
		obs_canvas_scene_remove(scene);
		// Rebrancher le canal sur la première scène restante.
		struct Ctx {
			obs_source_t *first = nullptr;
		} ctx;
		obs_canvas_enum_scenes(
			canvas,
			[](void *p, obs_source_t *src) {
				auto *c = static_cast<Ctx *>(p);
				if (!c->first)
					c->first = obs_source_get_ref(src);
				return c->first == nullptr;
			},
			&ctx);
		if (ctx.first) {
			obs_canvas_set_channel(canvas, 0, ctx.first);
			obs_source_release(ctx.first);
		}
		vx_vert_save();
		refreshScenes();
	}

	void addSourceToScene(obs_source_t *src, bool fill)
	{
		obs_scene_t *scene = current_scene();
		if (!scene || !src)
			return;
		obs_sceneitem_t *item = obs_scene_add(scene, src);
		if (item)
			apply_bounds(item, fill);
		vx_vert_save();
		refreshItems();
	}

	void onAddMainScene()
	{
		obs_source_t *main = obs_frontend_get_current_scene();
		if (!main)
			return;
		addSourceToScene(main, true); // Remplir : le stream recadré en 9:16
		obs_source_release(main);
	}

	void onAddCamera()
	{
		obs_source_t *cam = obs_source_create(camera_source_id(), "Caméra (verticale)", nullptr, nullptr);
		if (!cam)
			return;
		addSourceToScene(cam, true);
		obs_source_release(cam);
	}

	void onAddBrowser()
	{
		bool ok = false;
		const QString url = QInputDialog::getText(this, QStringLiteral("Source navigateur"),
							  QStringLiteral("URL (ex : votre overlay Valerix) :"),
							  QLineEdit::Normal,
							  QStringLiteral("https://valerix.stream/overlay/"), &ok);
		if (!ok || url.trimmed().isEmpty())
			return;
		obs_data_t *ss = obs_data_create();
		obs_data_set_string(ss, "url", url.trimmed().toUtf8().constData());
		obs_data_set_int(ss, "width", 1080);
		obs_data_set_int(ss, "height", 1920);
		obs_data_set_bool(ss, "shutdown", true);
		obs_source_t *src = obs_source_create("browser_source", "Overlay (verticale)", ss, nullptr);
		obs_data_release(ss);
		if (!src)
			return;
		addSourceToScene(src, false); // déjà au bon format → Adapter
		obs_source_release(src);
	}

	void onRemoveSource()
	{
		const int row = list->currentRow();
		if (row < 0)
			return;
		auto items = scene_items(current_scene());
		if (row < (int)items.size())
			obs_sceneitem_remove(items[row]);
		vx_vert_save();
		refreshItems();
	}

	void onPreset(bool fill)
	{
		const int row = list->currentRow();
		if (row < 0)
			return;
		auto items = scene_items(current_scene());
		if (row < (int)items.size())
			apply_bounds(items[row], fill);
		vx_vert_save();
	}

	void onStart()
	{
		std::string why;
		if (!vx_vert_start(&why))
			status->setText(QStringLiteral("⚠ %1").arg(QString::fromStdString(why)));
		refreshStatus();
	}
};

VertDockWidget *dockWidget = nullptr;

} // namespace

void vx_vert_dock_create(void)
{
	if (dockWidget)
		return;
	dockWidget = new VertDockWidget();
	if (!obs_frontend_add_dock_by_id(DOCK_ID, "VX Vertical", dockWidget)) {
		delete dockWidget;
		dockWidget = nullptr;
		obs_log(LOG_WARNING, "dock Vertical : création refusée");
		return;
	}
	obs_log(LOG_INFO, "dock Vertical créé");
}

void vx_vert_dock_destroy(void)
{
	if (!dockWidget)
		return;
	// L'obs_display est détruit ICI, synchronement : le retrait du dock peut
	// être différé par Qt, or le canvas est libéré juste après (EXIT).
	dockWidget->destroyPreview();
	obs_frontend_remove_dock(DOCK_ID);
	dockWidget = nullptr;
}
