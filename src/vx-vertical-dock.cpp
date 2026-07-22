/*
VX.Stream pour OBS — dock VX Vertical
Copyright (C) 2026 Valerix (Jaime Pires) <support@valerix.stream>
SPDX-License-Identifier: GPL-2.0-or-later
*/

// L'éditeur du canvas vertical, éclaté en TROIS docks à la manière d'Aitum :
//   • « VX Vertical »  : l'aperçu 9:16 interactif (petit) — clic, déplacement,
//                        8 poignées, magnétisme ;
//   • « VX Scènes »    : la liste des scènes verticales (créer/choisir) ;
//   • « VX Sources »   : les sources de la scène courante (ajouter/clic droit).
// Ils partagent le même canvas ; un notifieur les tient synchronisés.
// La DIFFUSION, elle, se règle dans VX Multistream (destinations « vertical »).
//
// L'aperçu est un obs_display embarqué dans un QWidget à fenêtre NATIVE
// (pattern des projecteurs OBS) : le draw callback tourne sur le thread
// graphique et rend obs_render_canvas_texture dans un viewport letterboxé.
// Le letterbox est publié dans des atomiques pour que les événements souris
// (thread UI) puissent convertir px écran → coordonnées canvas.
// Destruction : obs_display_destroy AVANT la libération du canvas (EXIT).

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

#include <graphics/matrix4.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "vx-vertical.hpp"
#include "vx-vertical-dock.hpp"

namespace {

constexpr const char *DOCK_ID = "vx_vertical";

obs_scene_t *current_scene();

// État partagé entre le thread graphique (draw) et le thread UI (souris).
// Scalaires atomiques : le letterbox est écrit par draw, lu par la souris ;
// la sélection l'inverse. Un décalage d'une frame est sans conséquence.
std::atomic<int> s_vpX{0}, s_vpY{0}, s_vpW{1}, s_vpH{1}; // viewport en px physiques
std::atomic<int> s_baseW{1080}, s_baseH{1920};
std::atomic<long long> s_selectedId{0}; // obs_sceneitem id sélectionné (0 = aucun)
// Guides de magnétisme actives (coordonnées canvas ; < 0 = aucune).
std::atomic<float> s_snapV{-1.0f}, s_snapH{-1.0f};

// Boîte englobante d'un item en coordonnées CANVAS (via sa transformation box).
struct Box {
	float x0, y0, x1, y1;
};
bool sceneitem_box(obs_sceneitem_t *item, Box *out)
{
	matrix4 m;
	obs_sceneitem_get_box_transform(item, &m);
	float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
	const float pts[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
	for (auto &p : pts) {
		vec3 v, r;
		vec3_set(&v, p[0], p[1], 0.0f);
		vec3_transform(&r, &v, &m);
		minx = r.x < minx ? r.x : minx;
		miny = r.y < miny ? r.y : miny;
		maxx = r.x > maxx ? r.x : maxx;
		maxy = r.y > maxy ? r.y : maxy;
	}
	out->x0 = minx;
	out->y0 = miny;
	out->x1 = maxx;
	out->y1 = maxy;
	return maxx > minx && maxy > miny;
}

// Applique une boîte englobante voulue à un item. Modèle unifié : on passe tout
// le monde en « bounds » centrés — déplacer = changer pos, redimensionner =
// changer bounds. Ça marche pour toutes les sources, quelle que soit leur
// taille native, et c'est exactement ce que fait la boîte de délimitation d'OBS.
void apply_aabb(obs_sceneitem_t *item, const Box &b)
{
	obs_transform_info ti;
	obs_sceneitem_get_info2(item, &ti);
	ti.pos.x = (b.x0 + b.x1) / 2;
	ti.pos.y = (b.y0 + b.y1) / 2;
	ti.alignment = OBS_ALIGN_CENTER;
	ti.bounds_alignment = OBS_ALIGN_CENTER;
	if (ti.bounds_type == OBS_BOUNDS_NONE)
		ti.bounds_type = OBS_BOUNDS_SCALE_INNER; // conserve le ratio du contenu
	ti.bounds.x = b.x1 - b.x0;
	ti.bounds.y = b.y1 - b.y0;
	obs_sceneitem_set_info2(item, &ti);
}

// ── Aperçu INTERACTIF : QWidget natif + obs_display + souris ─────────────────

class VertPreview : public QWidget {
public:
	explicit VertPreview(QWidget *parent) : QWidget(parent)
	{
		setAttribute(Qt::WA_NativeWindow);
		setAttribute(Qt::WA_PaintOnScreen);
		setAttribute(Qt::WA_NoSystemBackground);
		setMinimumHeight(160);
		setMouseTracking(true);
		setCursor(Qt::OpenHandCursor);
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

	// Sélection depuis la liste des sources (thread UI).
	void selectExternally(long long id) { s_selectedId = id; }

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

	// Coordonnées widget (logiques) → coordonnées canvas.
	bool toCanvas(const QPointF &pos, float *cxOut, float *cyOut)
	{
		const int vw = s_vpW.load(), vh = s_vpH.load();
		if (vw <= 1 || vh <= 1)
			return false;
		const qreal d = devicePixelRatioF();
		const float px = (float)(pos.x() * d), py = (float)(pos.y() * d);
		const float u = (px - s_vpX.load()) / vw, v = (py - s_vpY.load()) / vh;
		if (u < -0.2f || u > 1.2f || v < -0.2f || v > 1.2f)
			return false; // un peu de marge : on garde le geste hors cadre
		*cxOut = u * s_baseW.load();
		*cyOut = v * s_baseH.load();
		return true;
	}

	// Rayon de préhension d'une poignée, constant À L'ÉCRAN (converti en unités
	// canvas) : sinon il rétrécirait quand l'aperçu est petit.
	float grabRadius() const
	{
		const int vw = s_vpW.load();
		return vw > 1 ? 10.0f * (float)s_baseW.load() / vw : 12.0f;
	}

	void mousePressEvent(QMouseEvent *e) override
	{
		if (e->button() != Qt::LeftButton)
			return;
		float cx, cy;
		if (!toCanvas(e->position(), &cx, &cy))
			return;

		obs_scene_t *scene = current_scene();
		if (!scene)
			return;

		// 1) Une poignée de l'item déjà sélectionné a-t-elle été saisie ?
		const long long sel = s_selectedId.load();
		obs_sceneitem_t *selItem = sel ? obs_scene_find_sceneitem_by_id(scene, sel) : nullptr;
		Box sb;
		if (selItem && sceneitem_box(selItem, &sb)) {
			const int h = handleAt(sb, cx, cy);
			if (h >= 0) {
				beginDrag(selItem, (DragMode)(Resize0 + h), sb, cx, cy);
				return;
			}
		}

		// 2) Sinon, sélection de l'item sous le curseur (du dessus vers le fond).
		std::vector<obs_sceneitem_t *> items;
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *it, void *p) {
				static_cast<std::vector<obs_sceneitem_t *> *>(p)->push_back(it);
				return true;
			},
			&items);
		obs_sceneitem_t *hit = nullptr;
		Box hb{};
		for (auto rit = items.rbegin(); rit != items.rend(); ++rit) {
			Box b;
			if (sceneitem_box(*rit, &b) && cx >= b.x0 && cx <= b.x1 && cy >= b.y0 && cy <= b.y1) {
				hit = *rit;
				hb = b;
				break;
			}
		}
		if (!hit) {
			s_selectedId = 0;
			return;
		}
		s_selectedId = (long long)obs_sceneitem_get_id(hit);
		beginDrag(hit, Move, hb, cx, cy);
	}

	void mouseMoveEvent(QMouseEvent *e) override
	{
		float cx, cy;
		if (!toCanvas(e->position(), &cx, &cy))
			return;

		// Curseur indicatif quand on survole une poignée (sans bouton pressé).
		if (mode == None) {
			updateHoverCursor(cx, cy);
			return;
		}
		if (!dragItem)
			return;

		const float W = (float)s_baseW.load(), H = (float)s_baseH.load();
		const float dx = cx - grabCx, dy = cy - grabCy;
		Box b = startBox;
		if (mode == Move) {
			b.x0 += dx;
			b.x1 += dx;
			b.y0 += dy;
			b.y1 += dy;
			snapMove(&b, W, H);
		} else {
			// Redimensionnement : le bord/coin OPPOSÉ reste ancré.
			const int h = mode - Resize0;
			const bool left = (h == HTL || h == HL || h == HBL);
			const bool right = (h == HTR || h == HR || h == HBR);
			const bool top = (h == HTL || h == HT || h == HTR);
			const bool bottom = (h == HBL || h == HB || h == HBR);
			if (left)
				b.x0 = startBox.x0 + dx;
			if (right)
				b.x1 = startBox.x1 + dx;
			if (top)
				b.y0 = startBox.y0 + dy;
			if (bottom)
				b.y1 = startBox.y1 + dy;
			snapResize(&b, W, H, left, right, top, bottom);
			// Taille minimale + bords non croisés.
			if (b.x1 - b.x0 < 16.0f) {
				if (left)
					b.x0 = b.x1 - 16.0f;
				else
					b.x1 = b.x0 + 16.0f;
			}
			if (b.y1 - b.y0 < 16.0f) {
				if (top)
					b.y0 = b.y1 - 16.0f;
				else
					b.y1 = b.y0 + 16.0f;
			}
		}
		apply_aabb(dragItem, b);
	}

	void mouseReleaseEvent(QMouseEvent *) override
	{
		if (mode != None) {
			mode = None;
			dragItem = nullptr;
			s_snapV = s_snapH = -1.0f;
			setCursor(Qt::OpenHandCursor);
			vx_vert_save();
		}
	}

	// Molette = redimensionner autour du centre (geste rapide, sans poignée).
	void wheelEvent(QWheelEvent *e) override
	{
		const long long id = s_selectedId.load();
		obs_scene_t *scene = current_scene();
		obs_sceneitem_t *item = (id && scene) ? obs_scene_find_sceneitem_by_id(scene, id) : nullptr;
		Box b;
		if (!item || !sceneitem_box(item, &b))
			return;
		const float f = e->angleDelta().y() > 0 ? 1.08f : 1.0f / 1.08f;
		const float ccx = (b.x0 + b.x1) / 2, ccy = (b.y0 + b.y1) / 2;
		const float hw = (b.x1 - b.x0) * f / 2, hh = (b.y1 - b.y0) * f / 2;
		apply_aabb(item, Box{ccx - hw, ccy - hh, ccx + hw, ccy + hh});
		vx_vert_save();
	}

private:
	// Ordre des poignées : coins puis milieux de bords.
	enum { HTL = 0, HT, HTR, HR, HBR, HB, HBL, HL, HCOUNT };
	enum DragMode { None = 0, Move, Resize0 };

	obs_display_t *display = nullptr;
	DragMode mode = None;
	obs_sceneitem_t *dragItem = nullptr; // valide uniquement le temps d'un geste
	Box startBox{};
	float grabCx = 0, grabCy = 0;

	static void handlePoint(const Box &b, int h, float *x, float *y)
	{
		const float mx = (b.x0 + b.x1) / 2, my = (b.y0 + b.y1) / 2;
		switch (h) {
		case HTL:
			*x = b.x0;
			*y = b.y0;
			break;
		case HT:
			*x = mx;
			*y = b.y0;
			break;
		case HTR:
			*x = b.x1;
			*y = b.y0;
			break;
		case HR:
			*x = b.x1;
			*y = my;
			break;
		case HBR:
			*x = b.x1;
			*y = b.y1;
			break;
		case HB:
			*x = mx;
			*y = b.y1;
			break;
		case HBL:
			*x = b.x0;
			*y = b.y1;
			break;
		default:
			*x = b.x0;
			*y = my;
			break;
		}
	}

	int handleAt(const Box &b, float cx, float cy) const
	{
		const float r = grabRadius();
		for (int h = 0; h < HCOUNT; h++) {
			float hx, hy;
			handlePoint(b, h, &hx, &hy);
			if (cx >= hx - r && cx <= hx + r && cy >= hy - r && cy <= hy + r)
				return h;
		}
		return -1;
	}

	void beginDrag(obs_sceneitem_t *item, DragMode m, const Box &b, float cx, float cy)
	{
		mode = m;
		dragItem = item;
		startBox = b;
		grabCx = cx;
		grabCy = cy;
		setCursor(m == Move ? Qt::ClosedHandCursor : cursorForHandle(m - Resize0));
	}

	static Qt::CursorShape cursorForHandle(int h)
	{
		switch (h) {
		case HTL:
		case HBR:
			return Qt::SizeFDiagCursor;
		case HTR:
		case HBL:
			return Qt::SizeBDiagCursor;
		case HT:
		case HB:
			return Qt::SizeVerCursor;
		default:
			return Qt::SizeHorCursor;
		}
	}

	void updateHoverCursor(float cx, float cy)
	{
		obs_scene_t *scene = current_scene();
		const long long sel = s_selectedId.load();
		obs_sceneitem_t *item = (sel && scene) ? obs_scene_find_sceneitem_by_id(scene, sel) : nullptr;
		Box b;
		if (item && sceneitem_box(item, &b)) {
			const int h = handleAt(b, cx, cy);
			if (h >= 0) {
				setCursor(cursorForHandle(h));
				return;
			}
		}
		setCursor(Qt::OpenHandCursor);
	}

	// Magnétisme : bords et centre du canvas. Publie la guide à dessiner.
	static float snapTo(float v, const float *targets, int n, float tol, bool *snapped)
	{
		for (int i = 0; i < n; i++)
			if (v > targets[i] - tol && v < targets[i] + tol) {
				*snapped = true;
				return targets[i];
			}
		return v;
	}

	void snapMove(Box *b, float W, float H)
	{
		const float tol = grabRadius() * 0.8f;
		const float w = b->x1 - b->x0, h = b->y1 - b->y0;
		const float tx[3] = {0.0f, W / 2 - w / 2, W - w};
		const float ty[3] = {0.0f, H / 2 - h / 2, H - h};
		bool sx = false, sy = false;
		b->x0 = snapTo(b->x0, tx, 3, tol, &sx);
		b->y0 = snapTo(b->y0, ty, 3, tol, &sy);
		b->x1 = b->x0 + w;
		b->y1 = b->y0 + h;
		s_snapV = sx ? (b->x0 + b->x1) / 2 : -1.0f;
		s_snapH = sy ? (b->y0 + b->y1) / 2 : -1.0f;
	}

	void snapResize(Box *b, float W, float H, bool left, bool right, bool top, bool bottom)
	{
		const float tol = grabRadius() * 0.8f;
		const float tx[3] = {0.0f, W / 2, W};
		const float ty[3] = {0.0f, H / 2, H};
		bool sx = false, sy = false;
		if (left)
			b->x0 = snapTo(b->x0, tx, 3, tol, &sx);
		if (right)
			b->x1 = snapTo(b->x1, tx, 3, tol, &sx);
		if (top)
			b->y0 = snapTo(b->y0, ty, 3, tol, &sy);
		if (bottom)
			b->y1 = snapTo(b->y1, ty, 3, tol, &sy);
		s_snapV = sx ? (left ? b->x0 : b->x1) : -1.0f;
		s_snapH = sy ? (top ? b->y0 : b->y1) : -1.0f;
	}

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

	// Contour + 8 poignées + guides de magnétisme, en coordonnées canvas.
	static void draw_selection(float cw, float ch)
	{
		const long long id = s_selectedId.load();
		if (!id)
			return;
		obs_scene_t *scene = current_scene();
		obs_sceneitem_t *item = scene ? obs_scene_find_sceneitem_by_id(scene, id) : nullptr;
		Box b;
		if (!item || !sceneitem_box(item, &b))
			return;

		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
		gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
		auto setColor = [&](float r, float g, float bl) {
			struct vec4 c;
			vec4_set(&c, r, g, bl, 1.0f);
			gs_effect_set_vec4(color, &c);
		};
		auto rect = [](float x, float y, float w, float h) {
			gs_matrix_push();
			gs_matrix_translate3f(x, y, 0.0f);
			gs_matrix_scale3f(w, h, 1.0f);
			gs_draw_sprite(nullptr, 0, 1, 1);
			gs_matrix_pop();
		};

		const int vw = s_vpW.load();
		const float perPx = vw > 1 ? cw / vw : 2.0f; // 1 px écran en unités canvas
		const float t = 2.0f * perPx;                // contour ~2 px à l'écran
		const float hs = 5.0f * perPx;               // demi-côté d'une poignée

		gs_technique_begin(tech);
		gs_technique_begin_pass(tech, 0);

		// Guides de magnétisme (jaune) sur toute la hauteur/largeur du canvas.
		const float sv = s_snapV.load(), sh = s_snapH.load();
		setColor(0.98f, 0.75f, 0.14f);
		if (sv >= 0.0f)
			rect(sv - t / 2, 0.0f, t, ch);
		if (sh >= 0.0f)
			rect(0.0f, sh - t / 2, cw, t);

		// Contour de l'item.
		setColor(0.486f, 0.227f, 0.929f); // #7c3aed
		rect(b.x0, b.y0, b.x1 - b.x0, t);
		rect(b.x0, b.y1 - t, b.x1 - b.x0, t);
		rect(b.x0, b.y0, t, b.y1 - b.y0);
		rect(b.x1 - t, b.y0, t, b.y1 - b.y0);

		// Poignées (blanches, bien visibles sur n'importe quel fond).
		setColor(1.0f, 1.0f, 1.0f);
		const float mx = (b.x0 + b.x1) / 2, my = (b.y0 + b.y1) / 2;
		const float hx[HCOUNT] = {b.x0, mx, b.x1, b.x1, b.x1, mx, b.x0, b.x0};
		const float hy[HCOUNT] = {b.y0, b.y0, b.y0, my, b.y1, b.y1, b.y1, my};
		for (int i = 0; i < HCOUNT; i++)
			rect(hx[i] - hs, hy[i] - hs, hs * 2, hs * 2);

		gs_technique_end_pass(tech);
		gs_technique_end(tech);
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

		// Publie le letterbox + résolution pour le mapping souris.
		s_vpX = vx;
		s_vpY = vy;
		s_vpW = vw;
		s_vpH = vh;
		s_baseW = (int)ovi.base_width;
		s_baseH = (int)ovi.base_height;

		gs_viewport_push();
		gs_projection_push();
		gs_ortho(0.0f, cw, 0.0f, ch, -100.0f, 100.0f);
		gs_set_viewport(vx, vy, vw, vh);
		obs_render_canvas_texture(canvas);
		draw_selection(cw, ch);
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

// ── Docks : aperçu / scènes / sources (modèle « à la Aitum ») ────────────────
// Trois docks séparés qui partagent le MÊME canvas vertical. Un notifieur simple
// (thread UI uniquement) fait qu'une action dans un dock rafraîchit les autres :
// choisir une scène recharge la liste des sources ; ajouter une source la fait
// apparaître dans l'aperçu (qui, lui, se rend en direct — rien à rafraîchir).

VertPreview *g_preview = nullptr;                // référence partagée (sélection)
std::vector<std::function<void()>> g_refreshers; // rebuilders des listes

void vx_register_refresher(std::function<void()> f)
{
	g_refreshers.push_back(std::move(f));
}
void vx_refresh_all()
{
	for (auto &f : g_refreshers)
		if (f)
			f();
}
void vx_select_in_preview(long long id)
{
	if (g_preview)
		g_preview->selectExternally(id);
}

// ── Dock 1 : aperçu (petit) ──────────────────────────────────────────────────

class VertPreviewDock : public QWidget {
public:
	VertPreviewDock()
	{
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(6, 6, 6, 6);
		root->setSpacing(4);
		preview = new VertPreview(this);
		root->addWidget(preview, 1);
		auto *hint = new QLabel(QStringLiteral("Scènes & Sources : docks <b>VX Scènes</b> / <b>VX Sources</b>. "
						       "Diffusion : <b>VX Multistream</b> (destination Vertical)."),
					this);
		hint->setWordWrap(true);
		hint->setStyleSheet(QStringLiteral("color: #888; font-size: 9px;"));
		root->addWidget(hint);
		g_preview = preview;
	}
	void destroyPreview()
	{
		if (preview)
			preview->destroyDisplay();
		if (g_preview == preview)
			g_preview = nullptr;
	}

private:
	VertPreview *preview = nullptr;
};

// ── Dock 2 : scènes verticales ───────────────────────────────────────────────

class VertScenesDock : public QWidget {
public:
	VertScenesDock()
	{
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(6, 6, 6, 6);
		root->setSpacing(4);

		list = new QListWidget(this);
		connect(list, &QListWidget::currentRowChanged, this, [this](int) {
			if (!refreshing)
				onSelect();
		});
		root->addWidget(list, 1);

		auto *row = new QHBoxLayout();
		auto *add = new QPushButton(QStringLiteral("＋ Scène"), this);
		connect(add, &QPushButton::clicked, this, [this] { onAdd(); });
		auto *del = new QPushButton(QStringLiteral("−"), this);
		del->setFixedWidth(28);
		del->setToolTip(QStringLiteral("Supprimer la scène"));
		connect(del, &QPushButton::clicked, this, [this] { onRemove(); });
		row->addWidget(add);
		row->addWidget(del);
		row->addStretch(1);
		root->addLayout(row);

		vx_register_refresher([this] { refresh(); });
		refresh();
	}

private:
	QListWidget *list = nullptr;
	bool refreshing = false;

	void refresh()
	{
		refreshing = true;
		QSignalBlocker block(list); // ne pas déclencher onSelect en rebâtissant
		list->clear();
		obs_canvas_t *canvas = vx_vert_canvas();
		if (canvas) {
			obs_scene_t *cur = current_scene();
			const char *curName = cur ? obs_source_get_name(obs_scene_get_source(cur)) : nullptr;
			struct Ctx {
				QListWidget *list;
				const char *cur;
				int curRow;
				int row;
			} ctx{list, curName, -1, 0};
			obs_canvas_enum_scenes(
				canvas,
				[](void *p, obs_source_t *src) {
					auto *c = static_cast<Ctx *>(p);
					const char *n = obs_source_get_name(src);
					c->list->addItem(QString::fromUtf8(n ? n : "?"));
					if (c->cur && n && strcmp(c->cur, n) == 0)
						c->curRow = c->row;
					c->row++;
					return true;
				},
				&ctx);
			if (ctx.curRow >= 0)
				list->setCurrentRow(ctx.curRow);
		}
		refreshing = false;
	}

	void onSelect()
	{
		obs_canvas_t *canvas = vx_vert_canvas();
		QListWidgetItem *it = list->currentItem();
		if (!canvas || !it)
			return;
		const QByteArray name = it->text().toUtf8();
		obs_source_t *src = obs_canvas_get_source_by_name(canvas, name.constData());
		if (src) {
			obs_canvas_set_channel(canvas, 0, src);
			obs_source_release(src);
		}
		vx_refresh_all(); // la liste des sources suit la scène courante
	}

	void onAdd()
	{
		obs_canvas_t *canvas = vx_vert_canvas();
		if (!canvas)
			return;
		bool ok = false;
		const QString name = QInputDialog::getText(this, QStringLiteral("Nouvelle scène verticale"),
							   QStringLiteral("Nom :"), QLineEdit::Normal,
							   QStringLiteral("Verticale %1").arg(list->count() + 1), &ok);
		if (!ok || name.trimmed().isEmpty())
			return;
		obs_scene_t *scene = obs_canvas_scene_create(canvas, name.trimmed().toUtf8().constData());
		if (scene)
			obs_canvas_set_channel(canvas, 0, obs_scene_get_source(scene));
		vx_vert_save();
		vx_refresh_all();
	}

	void onRemove()
	{
		obs_canvas_t *canvas = vx_vert_canvas();
		obs_scene_t *scene = current_scene();
		if (!canvas || !scene || list->count() <= 1)
			return; // toujours garder au moins une scène
		obs_canvas_set_channel(canvas, 0, nullptr);
		obs_canvas_scene_remove(scene);
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
		vx_refresh_all();
	}
};

// ── Dock 3 : sources de la scène courante ────────────────────────────────────

class VertSourcesDock : public QWidget {
public:
	VertSourcesDock()
	{
		auto *root = new QVBoxLayout(this);
		root->setContentsMargins(6, 6, 6, 6);
		root->setSpacing(4);

		list = new QListWidget(this);
		list->setContextMenuPolicy(Qt::CustomContextMenu);
		connect(list, &QListWidget::customContextMenuRequested, this,
			[this](const QPoint &p) { showMenu(list->mapToGlobal(p)); });
		// Sélectionner une source la surligne dans l'aperçu (pas de notify).
		connect(list, &QListWidget::currentItemChanged, this, [](QListWidgetItem *it) {
			if (it)
				vx_select_in_preview(it->data(Qt::UserRole).toLongLong());
		});
		root->addWidget(list, 1);

		auto *row = new QHBoxLayout();
		auto *add = new QPushButton(QStringLiteral("＋ Source"), this);
		auto *menu = new QMenu(add);
		menu->addAction(QStringLiteral("Scène principale (recadrée)"), [this] { onAddMainScene(); });
		menu->addAction(QStringLiteral("Caméra"), [this] { onAddCamera(); });
		menu->addAction(QStringLiteral("Source navigateur (URL)…"), [this] { onAddBrowser(); });
		add->setMenu(menu);
		auto *del = new QPushButton(QStringLiteral("−"), this);
		del->setFixedWidth(28);
		del->setToolTip(QStringLiteral("Retirer la source"));
		connect(del, &QPushButton::clicked, this, [this] { onRemove(); });
		auto *fill = new QPushButton(QStringLiteral("Remplir"), this);
		connect(fill, &QPushButton::clicked, this, [this] { onPreset(true); });
		auto *fit = new QPushButton(QStringLiteral("Adapter"), this);
		connect(fit, &QPushButton::clicked, this, [this] { onPreset(false); });
		row->addWidget(add);
		row->addWidget(del);
		row->addStretch(1);
		row->addWidget(fill);
		row->addWidget(fit);
		root->addLayout(row);

		vx_register_refresher([this] { refresh(); });
		refresh();
	}

private:
	QListWidget *list = nullptr;

	obs_sceneitem_t *selectedItem()
	{
		QListWidgetItem *it = list->currentItem();
		obs_scene_t *scene = current_scene();
		if (!it || !scene)
			return nullptr;
		return obs_scene_find_sceneitem_by_id(scene, it->data(Qt::UserRole).toLongLong());
	}

	void refresh()
	{
		QSignalBlocker block(list);
		const long long keep = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toLongLong() : 0;
		list->clear();
		int selRow = -1, row = 0;
		for (obs_sceneitem_t *item : scene_items(current_scene())) {
			obs_source_t *src = obs_sceneitem_get_source(item);
			const char *n = src ? obs_source_get_name(src) : "?";
			QString label = QString::fromUtf8(n ? n : "?");
			if (!obs_sceneitem_visible(item))
				label.prepend(QStringLiteral("👁 "));
			if (obs_sceneitem_locked(item))
				label.prepend(QStringLiteral("🔒 "));
			auto *it = new QListWidgetItem(label);
			const long long id = (long long)obs_sceneitem_get_id(item);
			it->setData(Qt::UserRole, (qlonglong)id);
			list->addItem(it);
			if (id == keep)
				selRow = row;
			row++;
		}
		if (selRow >= 0)
			list->setCurrentRow(selRow);
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
		vx_refresh_all();
	}

	void onAddMainScene()
	{
		obs_source_t *main = obs_frontend_get_current_scene();
		if (!main)
			return;
		addSourceToScene(main, true);
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
		addSourceToScene(src, false);
		obs_source_release(src);
	}

	void onRemove()
	{
		obs_sceneitem_t *item = selectedItem();
		if (!item)
			return;
		obs_sceneitem_remove(item);
		vx_vert_save();
		vx_refresh_all();
	}

	void onPreset(bool fill)
	{
		obs_sceneitem_t *item = selectedItem();
		if (!item)
			return;
		apply_bounds(item, fill);
		vx_vert_save();
	}

	void showMenu(const QPoint &globalPos)
	{
		obs_sceneitem_t *item = selectedItem();
		if (!item)
			return;
		obs_source_t *src = obs_sceneitem_get_source(item);
		QMenu m(this);

		m.addAction(QStringLiteral("Renommer…"), [this, src] {
			if (!src)
				return;
			bool ok = false;
			const QString cur = QString::fromUtf8(obs_source_get_name(src));
			const QString n = QInputDialog::getText(this, QStringLiteral("Renommer la source"),
								QStringLiteral("Nom :"), QLineEdit::Normal, cur, &ok);
			if (ok && !n.trimmed().isEmpty()) {
				obs_source_set_name(src, n.trimmed().toUtf8().constData());
				vx_vert_save();
				vx_refresh_all();
			}
		});

		const bool visible = obs_sceneitem_visible(item);
		m.addAction(visible ? QStringLiteral("Masquer") : QStringLiteral("Afficher"), [item, visible] {
			obs_sceneitem_set_visible(item, !visible);
			vx_vert_save();
			vx_refresh_all();
		});
		const bool locked = obs_sceneitem_locked(item);
		m.addAction(locked ? QStringLiteral("Déverrouiller") : QStringLiteral("Verrouiller"), [item, locked] {
			obs_sceneitem_set_locked(item, !locked);
			vx_vert_save();
			vx_refresh_all();
		});

		m.addSeparator();
		m.addAction(QStringLiteral("Monter d'un plan"), [item] {
			obs_sceneitem_set_order(item, OBS_ORDER_MOVE_UP);
			vx_vert_save();
			vx_refresh_all();
		});
		m.addAction(QStringLiteral("Descendre d'un plan"), [item] {
			obs_sceneitem_set_order(item, OBS_ORDER_MOVE_DOWN);
			vx_vert_save();
			vx_refresh_all();
		});
		m.addAction(QStringLiteral("Mettre au premier plan"), [item] {
			obs_sceneitem_set_order(item, OBS_ORDER_MOVE_TOP);
			vx_vert_save();
			vx_refresh_all();
		});
		m.addAction(QStringLiteral("Mettre à l'arrière-plan"), [item] {
			obs_sceneitem_set_order(item, OBS_ORDER_MOVE_BOTTOM);
			vx_vert_save();
			vx_refresh_all();
		});

		m.addSeparator();
		m.addAction(QStringLiteral("Remplir le cadre 9:16"), [item] {
			apply_bounds(item, true);
			vx_vert_save();
		});
		m.addAction(QStringLiteral("Adapter au cadre 9:16"), [item] {
			apply_bounds(item, false);
			vx_vert_save();
		});
		m.addAction(QStringLiteral("Centrer"), [item] {
			Box b;
			if (!sceneitem_box(item, &b))
				return;
			const float w = b.x1 - b.x0, h = b.y1 - b.y0;
			apply_aabb(item, Box{540.0f - w / 2, 960.0f - h / 2, 540.0f + w / 2, 960.0f + h / 2});
			vx_vert_save();
		});

		if (src) {
			m.addSeparator();
			m.addAction(QStringLiteral("Propriétés…"), [src] { obs_frontend_open_source_properties(src); });
			m.addAction(QStringLiteral("Filtres…"), [src] { obs_frontend_open_source_filters(src); });
		}

		m.addSeparator();
		m.addAction(QStringLiteral("Supprimer"), [this] { onRemove(); });

		m.exec(globalPos);
	}
};

VertPreviewDock *previewDock = nullptr;
VertScenesDock *scenesDock = nullptr;
VertSourcesDock *sourcesDock = nullptr;

} // namespace

void vx_vert_dock_create(void)
{
	if (previewDock)
		return;
	previewDock = new VertPreviewDock();
	if (!obs_frontend_add_dock_by_id(DOCK_ID, "VX Vertical", previewDock)) {
		delete previewDock;
		previewDock = nullptr;
		obs_log(LOG_WARNING, "dock Vertical : création refusée");
		return;
	}
	scenesDock = new VertScenesDock();
	obs_frontend_add_dock_by_id("vx_vertical_scenes", "VX Scènes", scenesDock);
	sourcesDock = new VertSourcesDock();
	obs_frontend_add_dock_by_id("vx_vertical_sources", "VX Sources", sourcesDock);
	obs_log(LOG_INFO, "docks Vertical créés (aperçu + scènes + sources)");
}

void vx_vert_dock_destroy(void)
{
	if (!previewDock)
		return;
	// Plus aucun rafraîchisseur ne doit tourner : les widgets vont disparaître.
	g_refreshers.clear();
	// L'obs_display est détruit ICI, synchronement : le canvas est libéré juste
	// après (EXIT), et le retrait d'un dock peut être différé par Qt.
	previewDock->destroyPreview();
	obs_frontend_remove_dock(DOCK_ID);
	obs_frontend_remove_dock("vx_vertical_scenes");
	obs_frontend_remove_dock("vx_vertical_sources");
	previewDock = nullptr;
	scenesDock = nullptr;
	sourcesDock = nullptr;
}
