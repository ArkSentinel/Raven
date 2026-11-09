/**
 * Raven
 * raven_pdf.h : the pages of a document, rendered by the application.
 *
 * Raven has no PDF decoder and wants none: the application renders each page
 * with PDFium, Quartz, pdftoppm or anything else, and hands the widget the
 * RGBA pixels as a texture. The PDF view does the rest of the work: it lays
 * the pages out as they are in points (1/72 inch), zooms, scrolls, tells which
 * page is where, and asks for the pages it needs at the scale they are shown,
 * so that a document of a thousand pages costs what a screenful costs.
 *
 *     static void on_page_needed(Rid view, int page, float scale, void* user) {
 *         Rid texture = render_page(user, page, scale);    // with PDFium
 *         Rpdf_view_set_page_bitmap(view, page, texture, scale);
 *     }
 *
 *     Rid view = Rpdf_view_create();
 *     Rpdf_view_set_page_count(view, 42);
 *     Rpdf_view_set_page_size(view, 0, 595.0f, 842.0f);    // A4, in points
 *     Rpdf_view_set_page_needed_callback(view, on_page_needed, document);
 *     Rpdf_view_set_fit(view, R_PDF_FIT_WIDTH);
 *
 * The view keeps the textures it is given and frees none of them: they belong
 * to the application, which destroys the ones it no longer needs.
 *
 * Keys: the arrows and Page Up / Down scroll, Home and End go to the beginning
 * and to the end of the document; Ctrl + Page Up / Down go to the previous and
 * to the next page. The wheel scrolls, Ctrl + wheel zooms around the pointer,
 * two fingers zoom and a finger drags. In XML:
 *
 *     <PdfView Name="preview" PageCount="42" Page="0" Zoom="1" Fit="Width" PageGap="12"/>
 *
 * The functions are called from the thread that called Rinit, like the other
 * widget functions.
 */

#ifndef RAVEN_PDF_H
#define RAVEN_PDF_H

#include "raven_widget.h"

/** How the view keeps the pages in its place when it is resized. */
typedef enum RPdfFit {
	/** The zoom given is used as it is (the default). */
	R_PDF_FIT_NONE = 0x0570,
	/** The zoom follows the width of the view, with a scrollbar for the height. */
	R_PDF_FIT_WIDTH,
	/** The zoom follows the width or the height, whichever is smaller: the whole page shows. */
	R_PDF_FIT_PAGE
} RPdfFit;

#ifdef __cplusplus
extern "C" {
#endif

	/** @brief Creates an empty view. Give it its pages with Rpdf_view_set_page_count. */
	RAVEN_API Rid Rpdf_view_create(void);

	/* ---- the document ---- */

	/**
	 * @brief Number of pages of the document. 0 pages clears the view: the
	 * bitmaps are forgotten. New pages take the size of US Letter until
	 * Rpdf_view_set_page_size gives them their own.
	 */
	RAVEN_API void Rpdf_view_set_page_count(Rid pdf_view, int count);
	RAVEN_API int Rpdf_view_get_page_count(Rid pdf_view);
	RAVEN_API void Rpdf_view_set_page_size(Rid pdf_view, int page, float width, float height);

	/** @brief The size of a page in points (1/72 inch); 0, 0 when there is no such page. */
	RAVEN_API void Rpdf_view_get_page_size(Rid pdf_view, int page, float* width, float* height);

	/**
	 * @brief The RGBA pixels of a page, rendered at `scale` pixels per point
	 * (2 fills a page drawn at zoom 2). The view keeps the texture and frees
	 * nothing. R_INVALID_ID forgets the bitmap of that page, which then shows
	 * as a placeholder again.
	 */
	RAVEN_API void Rpdf_view_set_page_bitmap(Rid pdf_view, int page, Rid texture, float scale);
	/** @return R_TRUE when that page has a bitmap. */
	RAVEN_API int Rpdf_view_has_page_bitmap(Rid pdf_view, int page);

	/* ---- where the view is ---- */

	/**
	 * @brief The current page, from 0: it becomes the first one in view.
	 * Rpdf_view_set_page scrolls so that the page starts at the top.
	 */
	RAVEN_API void Rpdf_view_set_page(Rid pdf_view, int page);
	RAVEN_API int Rpdf_view_get_page(Rid pdf_view);
	/** @return The first and the last page whose rectangle is in the view, or 0 when there is none. */
	RAVEN_API int Rpdf_view_get_visible_first_page(Rid pdf_view);
	RAVEN_API int Rpdf_view_get_visible_last_page(Rid pdf_view);

	/* ---- zoom ---- */

	/**
	 * @brief Zoom in pixels per point: 1 = 72 dpi (0.05 .. 16, clamped).
	 * Setting it leaves the fit mode. Rpdf_view_set_fit sets a mode that
	 * keeps following the size of the view.
	 */
	RAVEN_API void Rpdf_view_set_zoom(Rid pdf_view, float zoom);
	RAVEN_API float Rpdf_view_get_zoom(Rid pdf_view);
	RAVEN_API void Rpdf_view_set_fit(Rid pdf_view, RPdfFit fit);
	RAVEN_API RPdfFit Rpdf_view_get_fit(Rid pdf_view);

	/** @brief Space between pages, and around them, in points (default 16). */
	RAVEN_API void Rpdf_view_set_page_gap(Rid pdf_view, float gap);
	RAVEN_API float Rpdf_view_get_page_gap(Rid pdf_view);

	/* ---- the application renders the pages ---- */

	/**
	 * @brief Called when a page must be rendered, with `scale` pixels per
	 * point (the zoom, times the scale of the window) and the index of the
	 * page. It may call Rpdf_view_set_page_bitmap from inside. A page is
	 * asked for once while it stays visible at a given scale; it is asked for
	 * again when the scale changes by more than 1 % or when it comes back
	 * into view.
	 */
	typedef void (*RPdfPageNeededCallback)(Rid pdf_view, int page, float scale, void* user_data);
	RAVEN_API void Rpdf_view_set_page_needed_callback(Rid pdf_view, RPdfPageNeededCallback callback, void* user_data);

	/**
	 * @brief The page and the point of the page (in points, from its top-left
	 * corner) under a point of the widget, for SyncTeX inverse search.
	 * @return R_FALSE outside a page.
	 */
	RAVEN_API int Rpdf_view_get_page_at(Rid pdf_view, float x, float y, int* page, float* page_x, float* page_y);

	/** @brief Called when the user clicks on a page. This also fires the action of the widget. */
	typedef void (*RPdfPageClickedCallback)(Rid pdf_view, int page, float page_x, float page_y, void* user_data);
	RAVEN_API void Rpdf_view_set_page_clicked_callback(Rid pdf_view, RPdfPageClickedCallback callback, void* user_data);

	/** @brief Asks for one page to be painted again, once its bitmap has arrived. */
	RAVEN_API void Rpdf_view_invalidate_page(Rid pdf_view, int page);

	/** @brief Scrolls so that a point of a page is in the middle of the view (SyncTeX forward search). */
	RAVEN_API void Rpdf_view_scroll_to_position(Rid pdf_view, int page, float page_x, float page_y);

	/**
	 * @brief A rectangle of a page to show, in points from its top-left corner
	 * (where the source of what is selected is, in a forward search): one per
	 * view. A negative page removes it.
	 */
	RAVEN_API void Rpdf_view_set_highlight(Rid pdf_view, int page, float x, float y, float width, float height);

	/** @brief Color behind the pages; the theme's by default (Rwidget_clear_color brings it back). */
	RAVEN_API void Rpdf_view_set_background_color(Rid pdf_view, float r, float g, float b, float a);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RAVEN_PDF_H */
