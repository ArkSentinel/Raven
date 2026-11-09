/**
 * Raven - widgets/pdf_view.c
 * PDF view: the pages of a document, rendered by the application.
 *
 * Raven has no PDF decoder: the application renders a page (PDFium, Quartz,
 * pdftoppm...) and hands the view the RGBA pixels as a texture. What is here
 * is the view: the pages laid out in points at a zoom, the scrolling, the
 * page navigation, the fit modes, and the callback that tells the
 * application which pages are needed at which scale, so that it renders only
 * what is on screen.
 */

#include "common.h"
#include "../render/renderer.h"

#include <stdio.h>

#define RVN_PDF_ZOOM_MIN 0.05f
#define RVN_PDF_ZOOM_MAX 16.0f
/* One notch of the wheel, one press of Ctrl + '+'. */
#define RVN_PDF_ZOOM_STEP 1.1f
/* Size of a page the application gave no size for (US Letter, in points). */
#define RVN_PDF_PAGE_WIDTH 612.0f
#define RVN_PDF_PAGE_HEIGHT 792.0f
/* A page is rendered again when the scale asked for moves by more than this. */
#define RVN_PDF_SCALE_TOLERANCE 0.01f

typedef struct rvn_pdf_page_t {
	/** Size of the page, in points. */
	float width;
	float height;
	/** Pixels of the page, or R_INVALID_ID while the application has not rendered it. */
	Rid texture;
	/** Resolution of the texture, in pixels per point (the scale it was rendered at). */
	float bitmap_scale;
	/** Scale last asked for through the callback; 0 once the page left the view. */
	float asked;
} rvn_pdf_page_t;

typedef struct rvn_pdf_view_t {
	rvn_widget_t base;
	rvn_pdf_page_t* pages;
	int count;
	uint32_t cap;
	/** The page the view was taken to, from 0. */
	int page;
	float zoom;
	float gap;
	RPdfFit fit;
	rvn_scrollbar_t bar;
	/** Bookkeeping of the pointer: the page that was pressed, or -1. */
	int pressed_page;
	RPdfPageNeededCallback needed;
	void* needed_user;
	RPdfPageClickedCallback clicked;
	void* clicked_user;
	int has_highlight;
	int highlight_page;
	float highlight[4];
	/** The pages drawn by the last frame: the ones that left the view are asked for again. */
	int painted_first;
	int painted_last;
} rvn_pdf_view_t;

/** Where the pages are in the widget. */
typedef struct rvn_pdf_layout_t {
	/** Area the pages are drawn in: the widget without the scrollbar. */
	rvn_rect_t body;
	rvn_rect_t track;
	float zoom;
	/** Height of all the pages and the gaps between them. */
	float content;
	int scrolls;
} rvn_pdf_layout_t;

static void rvn_pdf_init(rvn_widget_t* widget) {
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	view->zoom = 1.0f;
	view->gap = 16.0f;
	view->fit = R_PDF_FIT_NONE;
	view->pressed_page = -1;
	view->highlight_page = -1;
	view->painted_first = -1;
	view->painted_last = -1;
}

static void rvn_pdf_destroy(rvn_widget_t* widget) {
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	/* (the textures belong to the application) */
	rvn_free(view->pages);
	view->pages = NULL;
	view->count = view->cap = 0;
}

/* ---------------------------------------------------------------------------
 *  Geometry
 * ------------------------------------------------------------------------- */

static float rvn_pdf_page_width(const rvn_pdf_view_t* view, int page) {
	if (page < 0 || page >= view->count) return RVN_PDF_PAGE_WIDTH;
	return view->pages[page].width > 0.0f ? view->pages[page].width : RVN_PDF_PAGE_WIDTH;
}

static float rvn_pdf_page_height(const rvn_pdf_view_t* view, int page) {
	if (page < 0 || page >= view->count) return RVN_PDF_PAGE_HEIGHT;
	return view->pages[page].height > 0.0f ? view->pages[page].height : RVN_PDF_PAGE_HEIGHT;
}

/* Distance from the top of the content to the top of a page (O(page)). */
static float rvn_pdf_page_top(const rvn_pdf_view_t* view, int page, float zoom) {
	float y = view->gap;
	int i;
	for (i = 0; i < page; i++) y += rvn_pdf_page_height(view, i) * zoom + view->gap;
	return y;
}

static float rvn_pdf_content_height(const rvn_pdf_view_t* view, float zoom) {
	float y = view->gap;
	int i;
	for (i = 0; i < view->count; i++) y += rvn_pdf_page_height(view, i) * zoom + view->gap;
	return view->count > 0 ? y : 0.0f;
}

/** The zoom the fit mode asks for, given the room of the view. */
static float rvn_pdf_fit_zoom(rvn_pdf_view_t* view, float width, float height, float bar) {
	float available_w, available_h, zoom;
	if (view->fit == R_PDF_FIT_NONE) return rvn_clampf(view->zoom, RVN_PDF_ZOOM_MIN, RVN_PDF_ZOOM_MAX);
	available_w = rvn_maxf(1.0f, width - bar - view->gap * 2.0f);
	available_h = rvn_maxf(1.0f, height - view->gap * 2.0f);
	zoom = available_w / rvn_pdf_page_width(view, view->page);
	if (view->fit == R_PDF_FIT_PAGE) zoom = rvn_minf(zoom, available_h / rvn_pdf_page_height(view, view->page));
	return rvn_clampf(zoom, RVN_PDF_ZOOM_MIN, RVN_PDF_ZOOM_MAX);
}

/**
 * Works out the zoom and the room the pages take. Fills what the scrollbar
 * needs, and folds the zoom of a fit mode back into the widget.
 */
static void rvn_pdf_lay(rvn_widget_t* widget, rvn_pdf_layout_t* out) {
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	float width = widget->rect.w > 0.0f ? widget->rect.w : 320.0f;
	float height = widget->rect.h > 0.0f ? widget->rect.h : 480.0f;
	float bar = rvn_widget_metric(widget, R_METRIC_SCROLLBAR_SIZE);
	float zoom = rvn_pdf_fit_zoom(view, width, height, bar);
	float content = rvn_pdf_content_height(view, zoom);
	int scrolls = content > height + 0.5f;

	view->zoom = zoom;
	view->bar.content = content;
	view->bar.viewport = height;
	rvn_scrollbar_clamp(&view->bar);

	out->zoom = zoom;
	out->content = content;
	out->scrolls = scrolls;
	out->body = rvn_rect(0.0f, 0.0f, rvn_maxf(0.0f, width - (scrolls ? bar : 0.0f)), height);
	out->track = rvn_rect(width - bar, 0.0f, bar, height);
}

/**
 * The pages that touch the part [top, bottom] of the content, walking them
 * once. `first` is -1 when there is none.
 */
static void rvn_pdf_visible_range(const rvn_pdf_view_t* view, const rvn_pdf_layout_t* layout,
	float top, float bottom, int* first, int* last) {

	float y = view->gap;
	int i;
	*first = -1;
	*last = -1;
	for (i = 0; i < view->count; i++) {
		float h = rvn_pdf_page_height(view, i) * layout->zoom;
		if (y + h >= top && y <= bottom) {
			if (*first < 0) *first = i;
			*last = i;
		}
		y += h + view->gap;
	}
}

/** The page whose rectangle is at the top of the body, or -1. */
static int rvn_pdf_top_page(const rvn_pdf_view_t* view, const rvn_pdf_layout_t* layout, float offset) {
	float y = view->gap;
	int i;
	if (view->count == 0) return -1;
	for (i = 0; i < view->count; i++) {
		float h = rvn_pdf_page_height(view, i) * layout->zoom;
		if (y <= offset + 0.5f && y + h > offset + 0.5f) return i;
		y += h + view->gap;
	}
	return view->count - 1;
}

/* Keeps the current page in step with what the user scrolled to. */
static void rvn_pdf_track_page(rvn_widget_t* widget, rvn_pdf_layout_t* layout) {
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	int page;
	if (!layout->scrolls) return;
	page = rvn_pdf_top_page(view, layout, view->bar.offset);
	if (page >= 0) view->page = page;
}

static void rvn_pdf_scroll_by(rvn_widget_t* widget, float delta) {
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	rvn_pdf_layout_t layout;
	rvn_pdf_lay(widget, &layout);
	if (rvn_scrollbar_set(&view->bar, view->bar.offset + delta)) {
		rvn_pdf_track_page(widget, &layout);
		rvn_widget_invalidate_paint(widget);
	}
}

/* ---------------------------------------------------------------------------
 *  Zoom
 * ------------------------------------------------------------------------- */

/*
 * Sets the zoom, keeping the point of the page under (x, y) where it is: the
 * pointer for the wheel, the middle of the view for the keys.
 */
static int rvn_pdf_zoom_at(rvn_widget_t* widget, float zoom, float x, float y) {
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	rvn_pdf_layout_t before, after;
	float old_zoom, old_offset, anchor, doc_y, fraction = 0.0f;
	int page = -1, i, top_page;

	rvn_pdf_lay(widget, &before);
	old_zoom = before.zoom;
	old_offset = view->bar.offset;
	zoom = rvn_clampf(zoom, RVN_PDF_ZOOM_MIN, RVN_PDF_ZOOM_MAX);
	if (fabsf(zoom - old_zoom) < 0.0005f) return R_FALSE;

	/* what the pointer is over: a page, and how far down that page it is */
	anchor = y - before.body.y;
	{
		float top = view->gap - old_offset;
		for (i = 0; i < view->count; i++) {
			float width = rvn_pdf_page_width(view, i) * before.zoom;
			float height = rvn_pdf_page_height(view, i) * before.zoom;
			rvn_rect_t rect = rvn_rect(before.body.x + (before.body.w - width) * 0.5f,
				before.body.y + top, width, height);
			if (rvn_rect_contains(rect, x, y)) {
				page = i;
				fraction = height > 0.0f ? (y - rect.y) / height : 0.0f;
				break;
			}
			top += height + view->gap;
		}
	}

	view->fit = R_PDF_FIT_NONE;
	view->zoom = zoom;
	rvn_pdf_lay(widget, &after);

	if (page >= 0) {
		/* the same place of the page stays under the pointer */
		doc_y = rvn_pdf_page_top(view, page, after.zoom) +
			fraction * rvn_pdf_page_height(view, page) * after.zoom;
		rvn_scrollbar_set(&view->bar, doc_y - anchor);
	} else {
		/* no page under the pointer: keep the same part of the scrolling */
		float ratio = before.content > 0.0f ? (old_offset + anchor) / before.content : 0.0f;
		rvn_scrollbar_set(&view->bar, ratio * after.content - anchor);
	}
	/* (drawn where it is at once: it does not glide from the other size) */
	view->bar.shown.started = R_FALSE;
	top_page = rvn_pdf_top_page(view, &after, view->bar.offset);
	if (top_page >= 0 && after.scrolls) view->page = top_page;
	rvn_widget_invalidate_paint(widget);
	return R_TRUE;
}

/* ---------------------------------------------------------------------------
 *  Painting
 * ------------------------------------------------------------------------- */

static void rvn_pdf_paint_page(rvn_widget_t* widget, RPainter* painter, rvn_pdf_view_t* view,
	const rvn_pdf_layout_t* layout, int page, rvn_rect_t rect) {

	rvn_pdf_page_t* entry = &view->pages[page];
	rvn_color_t border = rvn_widget_color(widget, R_COLOR_BORDER);
	rvn_color_t shadow = rvn_widget_color(widget, R_COLOR_SHADOW);
	rvn_color_t paper = rvn_widget_color(widget, R_COLOR_INPUT);
	float radius = rvn_widget_metric(widget, R_METRIC_CORNER_RADIUS) * 0.5f;
	int has_bitmap = entry->texture != R_INVALID_ID && rvn_texture_get(entry->texture) != NULL;

	/* a soft shadow, kept short: the pages are many */
	rvn_paint_shadow(painter, rect, radius, 10.0f, rvn_color_fade(shadow, 0.35f));
	/* the paper, cut out of the background */
	rvn_paint_box(painter, rect, radius, paper, rvn_maxf(1.0f, rvn_widget_metric(widget, R_METRIC_BORDER_WIDTH)), border);

	if (has_bitmap) {
		rvn_paint_image(painter, rect, entry->texture, NULL, 0xFFFFFFFFu, radius);
	} else {
		/* not rendered yet: the number of the page, dimmed, in its middle */
		char number[16];
		float font = rvn_clampf(rect.h * 0.045f, 8.0f, 42.0f);
		snprintf(number, sizeof(number), "%d", page + 1);
		rvn_paint_text_in(painter, R_INVALID_ID, font, rect, 0.5f, number, -1,
			rvn_color_fade(rvn_widget_color(widget, R_COLOR_TEXT_PLACEHOLDER), 0.8f));
	}
	if (view->has_highlight && view->highlight_page == page) {
		rvn_rect_t mark = rvn_rect(rect.x + view->highlight[0] * layout->zoom,
			rect.y + view->highlight[1] * layout->zoom,
			view->highlight[2] * layout->zoom, view->highlight[3] * layout->zoom);
		mark = rvn_rect_intersect(mark, rect);
		if (mark.w > 0.0f && mark.h > 0.0f) {
			rvn_paint_box(painter, mark, 2.0f,
				rvn_color_fade(rvn_widget_color(widget, R_COLOR_SELECTION), 0.35f),
				1.5f, rvn_widget_color(widget, R_COLOR_ACCENT));
		}
	}
}

/*
 * Tells the application which of the pages in view must be rendered, once per
 * scale. The pages that left the view had their `asked` cleared, so they are
 * asked for again when they come back.
 */
static void rvn_pdf_request_pages(rvn_widget_t* widget, rvn_pdf_view_t* view, int first, int last, float scale) {
	int i;
	if (!view->needed || first < 0 || last < first) return;
	for (i = first; i <= last && i < view->count; i++) {
		rvn_pdf_page_t* entry = &view->pages[i];
		int rendered = entry->texture != R_INVALID_ID && entry->bitmap_scale > 0.0f &&
			fabsf(scale - entry->bitmap_scale) <= entry->bitmap_scale * RVN_PDF_SCALE_TOLERANCE;
		if (rendered) {
			entry->asked = scale;
			continue;
		}
		if (entry->asked > 0.0f && fabsf(scale - entry->asked) <= entry->asked * RVN_PDF_SCALE_TOLERANCE) continue;
		/* (remembered before the callback: it may hand the bitmap from inside) */
		entry->asked = scale;
		view->needed(widget->id, i, scale, view->needed_user);
	}
}

static void rvn_pdf_paint(rvn_widget_t* widget, RPainter* painter) {
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	rvn_pdf_layout_t layout;
	rvn_rect_t all = rvn_rect(0.0f, 0.0f, widget->rect.w, widget->rect.h);
	rvn_color_t background = rvn_widget_color(widget, R_COLOR_PANEL);
	rvn_vec2_t origin = rvn_widget_origin(widget);
	float mouse_x = widget->window ? widget->window->mouse_x - origin.x : -1.0f;
	float mouse_y = widget->window ? widget->window->mouse_y - origin.y : -1.0f;
	float scale = widget->window && widget->window->scale > 0.0f ? widget->window->scale : 1.0f;
	float offset;
	int first = -1, last = -1, i, focused = (widget->flags & RVN_WF_FOCUSED) != 0;

	rvn_pdf_lay(widget, &layout);
	if (!painter->enabled) background = rvn_color_fade(background, 0.5f);
	rvn_paint_rect(painter, all, background);
	offset = rvn_scrollbar_shown(&view->bar, painter);

	/* which pages are in view: the first and the last that touch the body */
	rvn_pdf_visible_range(view, &layout, offset, offset + layout.body.h, &first, &last);

	if (view->count > 0 && first >= 0) {
		/* (the pages are walked once, in order: where one starts follows from the previous one) */
		float top = view->gap - offset, zoom = layout.zoom;
		rvn_paint_push_clip(painter, layout.body);
		for (i = 0; i < first; i++) top += rvn_pdf_page_height(view, i) * zoom + view->gap;
		for (i = first; i <= last && i < view->count; i++) {
			float width = rvn_pdf_page_width(view, i) * zoom, height = rvn_pdf_page_height(view, i) * zoom;
			rvn_rect_t rect = rvn_rect(layout.body.x + (layout.body.w - width) * 0.5f,
				layout.body.y + top, width, height);
			rvn_pdf_paint_page(widget, painter, view, &layout, i, rect);
			top += height + view->gap;
		}
		rvn_paint_pop_clip(painter);
	}

	/* the pages that left the view are asked for again when they come back */
	for (i = view->painted_first; i >= 0 && i <= view->painted_last; i++) {
		if (i >= first && i <= last) continue;
		if (i >= 0 && i < view->count) view->pages[i].asked = 0.0f;
	}
	view->painted_first = first;
	view->painted_last = last;

	/* the application renders what is in view, once per scale */
	rvn_pdf_request_pages(widget, view, first, last, layout.zoom * scale);
	if (!rvn_widget_alive(widget)) return;

	if (layout.scrolls) rvn_scrollbar_paint(&view->bar, widget, painter, layout.track, R_TRUE, mouse_x, mouse_y);
	if (focused) rvn_paint_focus(widget, painter, rvn_widget_metric(widget, R_METRIC_CORNER_RADIUS));
}

/* ---------------------------------------------------------------------------
 *  Input
 * ------------------------------------------------------------------------- */

static int rvn_pdf_page_at_point(rvn_widget_t* widget, rvn_pdf_layout_t* layout, float x, float y,
	int* page, float* page_x, float* page_y) {

	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	float zoom = layout->zoom > 0.0f ? layout->zoom : 1.0f;
	float top = view->gap - view->bar.offset;
	int i;
	if (!rvn_rect_contains(layout->body, x, y)) return R_FALSE;
	for (i = 0; i < view->count; i++) {
		float width = rvn_pdf_page_width(view, i) * zoom, height = rvn_pdf_page_height(view, i) * zoom;
		rvn_rect_t rect = rvn_rect(layout->body.x + (layout->body.w - width) * 0.5f,
			layout->body.y + top, width, height);
		if (rvn_rect_contains(rect, x, y)) {
			if (page) *page = i;
			if (page_x) *page_x = (x - rect.x) / zoom;
			if (page_y) *page_y = (y - rect.y) / zoom;
			return R_TRUE;
		}
		top += height + view->gap;
	}
	return R_FALSE;
}

static int rvn_pdf_event(rvn_widget_t* widget, const REvent* event) {
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	rvn_pdf_layout_t layout;
	int changed = R_FALSE, page;

	rvn_pdf_lay(widget, &layout);

	/* the scrollbar first: it lies over the pages */
	if (event->type == R_EVENT_MOUSE_DOWN || event->type == R_EVENT_MOUSE_MOVE || event->type == R_EVENT_MOUSE_UP) {
		if ((layout.track.w > 0.0f || view->bar.dragging) &&
			rvn_scrollbar_event(&view->bar, widget, event, layout.track, R_TRUE, &changed)) {
			rvn_pdf_track_page(widget, &layout);
			rvn_widget_invalidate_paint(widget);
			return R_TRUE;
		}
	}

	switch (event->type) {
		case R_EVENT_MOUSE_DOWN:
			if (event->button != R_MOUSE_LEFT) return R_FALSE;
			rvn_widget_focus(widget);
			view->pressed_page = -1;
			if (rvn_pdf_page_at_point(widget, &layout, event->local_x, event->local_y, &page, NULL, NULL)) {
				view->pressed_page = page;
			}
			return R_TRUE;
		case R_EVENT_MOUSE_UP: {
			int pressed = view->pressed_page;
			float page_x = 0.0f, page_y = 0.0f;
			view->pressed_page = -1;
			if (event->button != R_MOUSE_LEFT || pressed < 0) return R_FALSE;
			if (!rvn_pdf_page_at_point(widget, &layout, event->local_x, event->local_y, &page, &page_x, &page_y)) return R_TRUE;
			if (page != pressed) return R_TRUE;
			if (view->clicked) view->clicked(widget->id, page, page_x, page_y, view->clicked_user);
			if (rvn_widget_alive(widget)) rvn_widget_fire_action(widget);
			return R_TRUE;
		}
		case R_EVENT_MOUSE_WHEEL: {
			float along = event->wheel_y;
			if (event->modifiers & (R_MOD_CTRL | R_MOD_SUPER) && along != 0.0f) {
				float zoom = view->zoom * powf(RVN_PDF_ZOOM_STEP, along);
				return rvn_pdf_zoom_at(widget, zoom, event->local_x, event->local_y);
			}
			if (along == 0.0f) return R_FALSE;
			changed = rvn_scrollbar_wheel(&view->bar, -along * rvn_maxf(48.0f, view->bar.viewport * 0.12f));
			if (changed) rvn_pdf_track_page(widget, &layout);
			rvn_widget_invalidate_paint(widget);
			/* at the end of the range, the wheel goes to what contains the view */
			return changed;
		}
		case R_EVENT_FOCUS_GAINED:
		case R_EVENT_FOCUS_LOST:
			rvn_widget_invalidate_paint(widget);
			return R_FALSE;
		case R_EVENT_KEY_DOWN: {
			float step;
			if (!(widget->flags & RVN_WF_FOCUSED)) return R_FALSE;
			step = rvn_maxf(40.0f, view->bar.viewport - 40.0f);
			if (event->modifiers & (R_MOD_CTRL | R_MOD_SUPER)) {
				switch (event->key) {
					case R_KEY_PAGE_UP:
						Rpdf_view_set_page(widget->id, view->page - 1);
						return R_TRUE;
					case R_KEY_PAGE_DOWN:
						Rpdf_view_set_page(widget->id, view->page + 1);
						return R_TRUE;
					default:
						return R_FALSE;
				}
			}
			switch (event->key) {
				case R_KEY_UP:
				case R_KEY_LEFT:
					rvn_pdf_scroll_by(widget, -step);
					return R_TRUE;
				case R_KEY_DOWN:
				case R_KEY_RIGHT:
					rvn_pdf_scroll_by(widget, step);
					return R_TRUE;
				case R_KEY_PAGE_UP: rvn_pdf_scroll_by(widget, -step); return R_TRUE;
				case R_KEY_PAGE_DOWN: rvn_pdf_scroll_by(widget, step); return R_TRUE;
				case R_KEY_HOME:
					if (view->count > 0) view->page = 0;
					rvn_scrollbar_set(&view->bar, 0.0f);
					rvn_widget_invalidate_paint(widget);
					return R_TRUE;
				case R_KEY_END:
					if (view->count > 0) view->page = view->count - 1;
					rvn_scrollbar_set(&view->bar, rvn_scrollbar_max(&view->bar));
					rvn_widget_invalidate_paint(widget);
					return R_TRUE;
				default:
					return R_FALSE;
			}
		}
		default:
			return R_FALSE;
	}
}

static int rvn_pdf_pan(rvn_widget_t* widget, float dx, float dy, int coasting) {
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	rvn_pdf_layout_t layout;
	int moved = rvn_scrollbar_pan(&view->bar, -dy, coasting);
	if (moved) {
		rvn_pdf_lay(widget, &layout);
		rvn_pdf_track_page(widget, &layout);
	}
	RVN_UNUSED(dx);
	return moved;
}

static int rvn_pdf_zoom(rvn_widget_t* widget, float factor, float x, float y) {
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	rvn_pdf_layout_t layout;
	if (!(factor > 0.0f)) return R_FALSE;
	rvn_pdf_lay(widget, &layout);
	/* (a factor > 1 makes the pages larger) */
	return rvn_pdf_zoom_at(widget, view->zoom * factor, x, y);
}

/* ---------------------------------------------------------------------------
 *  XML
 * ------------------------------------------------------------------------- */

static int rvn_parse_pdf_fit(const char* value, RPdfFit* fit) {
	if (rvn_stricmp(value, "None") == 0) *fit = R_PDF_FIT_NONE;
	else if (rvn_stricmp(value, "Width") == 0) *fit = R_PDF_FIT_WIDTH;
	else if (rvn_stricmp(value, "Page") == 0) *fit = R_PDF_FIT_PAGE;
	else return R_FALSE;
	return R_TRUE;
}

static int rvn_pdf_set_attribute(rvn_widget_t* widget, const char* name, const char* value) {
	int number;
	float real;
	if (RVN_ATTR("PageCount")) {
		if (!rvn_parse_int(value, &number)) return R_FALSE;
		Rpdf_view_set_page_count(widget->id, number);
		return R_TRUE;
	}
	if (RVN_ATTR("Page")) {
		if (!rvn_parse_int(value, &number)) return R_FALSE;
		Rpdf_view_set_page(widget->id, number);
		return R_TRUE;
	}
	if (RVN_ATTR("Zoom")) {
		if (!rvn_parse_float(value, &real)) return R_FALSE;
		Rpdf_view_set_zoom(widget->id, real);
		return R_TRUE;
	}
	if (RVN_ATTR("Fit")) {
		RPdfFit fit;
		if (!rvn_parse_pdf_fit(value, &fit)) return R_FALSE;
		Rpdf_view_set_fit(widget->id, fit);
		return R_TRUE;
	}
	if (RVN_ATTR("PageGap")) {
		if (!rvn_parse_float(value, &real)) return R_FALSE;
		Rpdf_view_set_page_gap(widget->id, real);
		return R_TRUE;
	}
	return R_FALSE;
}

static rvn_vec2_t rvn_pdf_measure(rvn_widget_t* widget) {
	RVN_UNUSED(widget);
	/* a preview is usually given its size by a filling slot; this is a page of A4 */
	return rvn_vec2(320.0f, 480.0f);
}

static void rvn_pdf_arrange(rvn_widget_t* widget) {
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	rvn_pdf_layout_t layout;
	float before = view->zoom;
	rvn_pdf_lay(widget, &layout);
	/*
	 * A fit mode follows the size of the view: when the zoom it gives changes,
	 * the page shown stays at the top, where the reader left it.
	 */
	if (view->fit != R_PDF_FIT_NONE && view->count > 0 && fabsf(layout.zoom - before) > 0.0005f) {
		rvn_scrollbar_set(&view->bar, rvn_pdf_page_top(view, view->page, layout.zoom));
		view->bar.shown.started = R_FALSE;
	}
}

const rvn_widget_class_t rvn_class_pdf_view = {
	.type = R_WIDGET_PDF_VIEW,
	.name = "PdfView",
	.size = sizeof(rvn_pdf_view_t),
	/* the paint calls the application (which may render a page): on the thread of the interface */
	.flags = RVN_CF_NO_CHILDREN | RVN_CF_FOCUSABLE | RVN_CF_UI_PAINT | RVN_CF_SURFACE,
	.init = rvn_pdf_init,
	.destroy = rvn_pdf_destroy,
	.measure = rvn_pdf_measure,
	.arrange = rvn_pdf_arrange,
	.paint = rvn_pdf_paint,
	.event = rvn_pdf_event,
	.set_attribute = rvn_pdf_set_attribute,
	.pan = rvn_pdf_pan,
	.zoom = rvn_pdf_zoom
};

/* ---------------------------------------------------------------------------
 *  Public API
 * ------------------------------------------------------------------------- */

Rid Rpdf_view_create(void) {
	rvn_widget_t* widget;
	RVN_API_GUARD(R_INVALID_ID);
	widget = rvn_widget_create(R_WIDGET_PDF_VIEW);
	if (!widget) return R_INVALID_ID;
	return widget->id;
}

void Rpdf_view_set_page_count(Rid id, int count) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_PDF_VIEW);
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	int i;
	count = rvn_maxi(count, 0);
	if ((uint32_t)count > view->cap) {
		RVN_ARRAY_RESERVE(view->pages, view->cap, count);
		/* the memory of a grown array is not zeroed */
		for (i = view->count; i < (int)view->cap; i++) memset(&view->pages[i], 0, sizeof(view->pages[i]));
	}
	if (count > view->count) {
		for (i = view->count; i < count; i++) {
			view->pages[i].width = RVN_PDF_PAGE_WIDTH;
			view->pages[i].height = RVN_PDF_PAGE_HEIGHT;
			view->pages[i].texture = R_INVALID_ID;
			view->pages[i].bitmap_scale = 1.0f;
			view->pages[i].asked = 0.0f;
		}
	}
	view->count = count;
	view->page = rvn_clampi(view->page, 0, count > 0 ? count - 1 : 0);
	view->pressed_page = -1;
	if (view->highlight_page >= count) view->has_highlight = R_FALSE;
	rvn_widget_invalidate_layout(widget);
}

int Rpdf_view_get_page_count(Rid id) {
	RVN_WIDGET(widget, id, R_WIDGET_PDF_VIEW, 0);
	return ((rvn_pdf_view_t*)widget)->count;
}

void Rpdf_view_set_page_size(Rid id, int page, float width, float height) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_PDF_VIEW);
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	if (page < 0 || page >= view->count) {
		rvn_error(R_ERROR_INVALID_VALUE, R_SEVERITY_ERROR, "Rpdf_view_set_page_size: invalid page %d", page);
		return;
	}
	view->pages[page].width = width > 1.0f ? width : 1.0f;
	view->pages[page].height = height > 1.0f ? height : 1.0f;
	rvn_widget_invalidate_layout(widget);
}

void Rpdf_view_get_page_size(Rid id, int page, float* width, float* height) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_PDF_VIEW);
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	if (width) *width = 0.0f;
	if (height) *height = 0.0f;
	if (page < 0 || page >= view->count) return;
	if (width) *width = view->pages[page].width;
	if (height) *height = view->pages[page].height;
}

void Rpdf_view_set_page_bitmap(Rid id, int page, Rid texture, float scale) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_PDF_VIEW);
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	if (page < 0 || page >= view->count) {
		rvn_error(R_ERROR_INVALID_VALUE, R_SEVERITY_ERROR, "Rpdf_view_set_page_bitmap: invalid page %d", page);
		return;
	}
	if (texture != R_INVALID_ID && !rvn_texture_get(texture)) {
		rvn_error(R_ERROR_INVALID_ID, R_SEVERITY_ERROR, "Rpdf_view_set_page_bitmap: invalid texture");
		return;
	}
	view->pages[page].texture = texture;
	view->pages[page].bitmap_scale = scale > 0.0f ? scale : 1.0f;
	rvn_widget_invalidate_paint(widget);
}

int Rpdf_view_has_page_bitmap(Rid id, int page) {
	RVN_WIDGET(widget, id, R_WIDGET_PDF_VIEW, R_FALSE);
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	if (page < 0 || page >= view->count) return R_FALSE;
	return view->pages[page].texture != R_INVALID_ID;
}

void Rpdf_view_set_page(Rid id, int page) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_PDF_VIEW);
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	rvn_pdf_layout_t layout;
	if (view->count == 0) {
		view->page = 0;
		return;
	}
	view->page = rvn_clampi(page, 0, view->count - 1);
	rvn_pdf_lay(widget, &layout);
	rvn_scrollbar_set(&view->bar, rvn_pdf_page_top(view, view->page, layout.zoom));
	view->bar.shown.started = R_FALSE;
	rvn_widget_invalidate_paint(widget);
}

int Rpdf_view_get_page(Rid id) {
	RVN_WIDGET(widget, id, R_WIDGET_PDF_VIEW, 0);
	return ((rvn_pdf_view_t*)widget)->page;
}

int Rpdf_view_get_visible_first_page(Rid id) {
	RVN_WIDGET(widget, id, R_WIDGET_PDF_VIEW, 0);
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	rvn_pdf_layout_t layout;
	int first, last;
	rvn_pdf_lay(widget, &layout);
	rvn_pdf_visible_range(view, &layout, view->bar.offset, view->bar.offset + layout.body.h, &first, &last);
	return first >= 0 ? first : 0;
}

int Rpdf_view_get_visible_last_page(Rid id) {
	RVN_WIDGET(widget, id, R_WIDGET_PDF_VIEW, 0);
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	rvn_pdf_layout_t layout;
	int first, last;
	rvn_pdf_lay(widget, &layout);
	rvn_pdf_visible_range(view, &layout, view->bar.offset, view->bar.offset + layout.body.h, &first, &last);
	return last >= 0 ? last : 0;
}

void Rpdf_view_set_zoom(Rid id, float zoom) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_PDF_VIEW);
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	view->zoom = rvn_clampf(zoom, RVN_PDF_ZOOM_MIN, RVN_PDF_ZOOM_MAX);
	view->fit = R_PDF_FIT_NONE;
	rvn_widget_invalidate_layout(widget);
}

float Rpdf_view_get_zoom(Rid id) {
	RVN_WIDGET(widget, id, R_WIDGET_PDF_VIEW, 1.0f);
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	rvn_pdf_layout_t layout;
	rvn_pdf_lay(widget, &layout);
	return view->zoom;
}

void Rpdf_view_set_fit(Rid id, RPdfFit fit) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_PDF_VIEW);
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	if (fit != R_PDF_FIT_NONE && fit != R_PDF_FIT_WIDTH && fit != R_PDF_FIT_PAGE) {
		rvn_error(R_ERROR_INVALID_ENUM, R_SEVERITY_ERROR, "Rpdf_view_set_fit: invalid fit mode");
		return;
	}
	view->fit = fit;
	rvn_widget_invalidate_layout(widget);
}

RPdfFit Rpdf_view_get_fit(Rid id) {
	RVN_WIDGET(widget, id, R_WIDGET_PDF_VIEW, R_PDF_FIT_NONE);
	return ((rvn_pdf_view_t*)widget)->fit;
}

void Rpdf_view_set_page_gap(Rid id, float gap) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_PDF_VIEW);
	((rvn_pdf_view_t*)widget)->gap = rvn_clampf(gap, 0.0f, 400.0f);
	rvn_widget_invalidate_layout(widget);
}

float Rpdf_view_get_page_gap(Rid id) {
	RVN_WIDGET(widget, id, R_WIDGET_PDF_VIEW, 16.0f);
	return ((rvn_pdf_view_t*)widget)->gap;
}

void Rpdf_view_set_page_needed_callback(Rid id, RPdfPageNeededCallback callback, void* user_data) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_PDF_VIEW);
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	int i;
	view->needed = callback;
	view->needed_user = user_data;
	/* the pages are asked for again, whatever was asked before */
	for (i = 0; i < view->count; i++) view->pages[i].asked = 0.0f;
	rvn_widget_invalidate_paint(widget);
}

int Rpdf_view_get_page_at(Rid id, float x, float y, int* page, float* page_x, float* page_y) {
	RVN_WIDGET(widget, id, R_WIDGET_PDF_VIEW, R_FALSE);
	rvn_pdf_layout_t layout;
	rvn_pdf_lay(widget, &layout);
	return rvn_pdf_page_at_point(widget, &layout, x, y, page, page_x, page_y);
}

void Rpdf_view_set_page_clicked_callback(Rid id, RPdfPageClickedCallback callback, void* user_data) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_PDF_VIEW);
	((rvn_pdf_view_t*)widget)->clicked = callback;
	((rvn_pdf_view_t*)widget)->clicked_user = user_data;
}

void Rpdf_view_invalidate_page(Rid id, int page) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_PDF_VIEW);
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	if (page >= 0 && page < view->count) rvn_widget_invalidate_paint(widget);
}

void Rpdf_view_scroll_to_position(Rid id, int page, float page_x, float page_y) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_PDF_VIEW);
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	rvn_pdf_layout_t layout;
	RVN_UNUSED(page_x);
	if (page < 0 || page >= view->count) {
		rvn_error(R_ERROR_INVALID_VALUE, R_SEVERITY_ERROR, "Rpdf_view_scroll_to_position: invalid page %d", page);
		return;
	}
	rvn_pdf_lay(widget, &layout);
	rvn_scrollbar_set(&view->bar, rvn_pdf_page_top(view, page, layout.zoom) +
		page_y * layout.zoom - layout.body.h * 0.5f);
	view->bar.shown.started = R_FALSE;
	view->page = page;
	rvn_widget_invalidate_paint(widget);
}

void Rpdf_view_set_highlight(Rid id, int page, float x, float y, float width, float height) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_PDF_VIEW);
	rvn_pdf_view_t* view = (rvn_pdf_view_t*)widget;
	if (page < 0 || page >= view->count) {
		view->has_highlight = R_FALSE;
		view->highlight_page = -1;
		rvn_widget_invalidate_paint(widget);
		return;
	}
	view->has_highlight = R_TRUE;
	view->highlight_page = page;
	view->highlight[0] = x;
	view->highlight[1] = y;
	view->highlight[2] = width > 0.0f ? width : 0.0f;
	view->highlight[3] = height > 0.0f ? height : 0.0f;
	rvn_widget_invalidate_paint(widget);
}

void Rpdf_view_set_background_color(Rid id, float r, float g, float b, float a) {
	RVN_WIDGET_VOID(widget, id, R_WIDGET_PDF_VIEW);
	/* (the color role of the widget: Rwidget_clear_color gives the one of the theme back) */
	rvn_widget_set_color(widget, R_COLOR_PANEL, rvn_rgba(r, g, b, a));
	rvn_widget_invalidate_paint(widget);
}
