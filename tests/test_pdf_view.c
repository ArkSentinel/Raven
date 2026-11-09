/**
 * Raven tests - PdfView: the pages of a document, rendered by the application.
 */

#include "test.h"

static Rid g_window;
static int g_needed_calls;
static int g_needed_page;
static float g_needed_scale;
static int g_clicked_page;
static float g_clicked_x, g_clicked_y;
static int g_clicked_calls;

/* A 4 x 2 red page, ready to be handed to the view. */
static Rid make_bitmap(void) {
	uint8_t pixels[4 * 4 * 2 * 4];
	memset(pixels, 0, sizeof(pixels));
	pixels[0] = 255;
	pixels[3] = 255;
	return Rtexture_create_from_pixels(pixels, 4, 2);
}

static Rid content(const char* xml) {
	Rid root = Rwidget_create_from_xml(xml, strlen(xml));
	Rwindow_set_content(g_window, root);
	Rdraw_frame();
	return root;
}

static void on_page_needed(Rid view, int page, float scale, void* user_data) {
	(void)view;
	(void)user_data;
	g_needed_calls++;
	g_needed_page = page;
	g_needed_scale = scale;
}

/* A page needed callback that draws the bitmap from inside, as an application does. */
static Rid g_render_texture;
static int g_rendered;
static void on_page_render(Rid view, int page, float scale, void* user_data) {
	(void)user_data;
	g_rendered++;
	Rpdf_view_set_page_bitmap(view, page, g_render_texture, scale);
}

static void on_page_clicked(Rid view, int page, float page_x, float page_y, void* user_data) {
	(void)view;
	(void)user_data;
	g_clicked_calls++;
	g_clicked_page = page;
	g_clicked_x = page_x;
	g_clicked_y = page_y;
}

static void test_pdf_view(void) {
	Rid view, texture, root;
	float x, y, w, h, pw, ph, zoom, page_x, page_y;
	int page, i;

	root = content("<VBox><PdfView Name=\"pdf\" Height=\"400\" PageCount=\"3\" Zoom=\"1\" Fit=\"None\" PageGap=\"10\"/></VBox>");
	view = Rwidget_find(root, "pdf");
	CHECK(view != R_INVALID_ID);
	CHECK(Rwidget_get_type(view) == R_WIDGET_PDF_VIEW);
	CHECK(Rpdf_view_get_page_count(view) == 3);
	CHECK(Rpdf_view_get_page(view) == 0);
	CHECK(Rpdf_view_get_fit(view) == R_PDF_FIT_NONE);
	CHECK(Rpdf_view_get_page_gap(view) == 10.0f);
	/* a document for a screen reader, named by the application */
	CHECK(Rwidget_get_accessible_role(view) == R_ROLE_DOCUMENT);
	Rwidget_set_accessible_name(view, "Thesis");
	CHECK_STR(Rwidget_get_accessible_name(view), "Thesis");

	/* pages of US Letter, in points, each one on its own */
	Rpdf_view_set_page_size(view, 0, 612.0f, 792.0f);
	Rpdf_view_set_page_size(view, 1, 612.0f, 792.0f);
	Rpdf_view_set_page_size(view, 2, 612.0f, 792.0f);
	Rpdf_view_get_page_size(view, 1, &pw, &ph);
	CHECK(pw == 612.0f && ph == 792.0f);
	Rpdf_view_get_page_size(view, 99, &pw, &ph);
	CHECK(pw == 0.0f && ph == 0.0f);
	EXPECT_ERRORS_BEGIN();
	Rpdf_view_set_page_size(view, 99, 10.0f, 10.0f);
	EXPECT_ERRORS_END(1);

	/* a bitmap from the application */
	texture = make_bitmap();
	CHECK(texture != R_INVALID_ID);
	CHECK(!Rpdf_view_has_page_bitmap(view, 0));
	Rpdf_view_set_page_bitmap(view, 0, texture, 1.0f);
	CHECK(Rpdf_view_has_page_bitmap(view, 0));
	CHECK(!Rpdf_view_has_page_bitmap(view, 1));
	EXPECT_ERRORS_BEGIN();
	Rpdf_view_set_page_bitmap(view, 42, texture, 1.0f);
	EXPECT_ERRORS_END(1);
	Rdraw_frame();

	/* the widget is at least as tall as it was told, and 320 x 480 by default */
	Rwidget_get_rect(view, &x, &y, &w, &h);
	CHECK(w > 0.0f && h > 0.0f);

	/* a zoom that fits the page, to know where the pages are */
	Rpdf_view_set_zoom(view, 0.4f);
	Rpdf_view_set_page(view, 0);
	Rdraw_frame();
	zoom = Rpdf_view_get_zoom(view);
	CHECK_NEAR(zoom, 0.4);
	Rwidget_get_rect(view, &x, &y, &w, &h);
	{
		/* where the first page is drawn, found by probing: left, right, top and bottom */
		float left = -1.0f, right = -1.0f, top = -1.0f, bottom = -1.0f, px, cx, cy;
		for (px = 0.0f; px <= w; px += 1.0f) {
			if (Rpdf_view_get_page_at(view, px, 40.0f, &page, &page_x, &page_y)) {
				if (left < 0.0f) left = px;
				right = px;
			}
		}
		CHECK(left >= 0.0f && right > left);
		CHECK_NEAR(right - left + 1.0f, 612.0f * zoom);
		cx = (left + right) * 0.5f;

		/* (the first page starts at the top of the view: set_page scrolled its top there) */
		CHECK(Rpdf_view_get_page_at(view, cx, 1.0f, &page, &page_x, &page_y));
		CHECK(page == 0);
		CHECK_NEAR(page_y, 2.5);
		for (px = 0.0f; px <= h; px += 1.0f) {
			int found = Rpdf_view_get_page_at(view, cx, px, &page, &page_x, &page_y);
			if (found && page == 0) {
				if (top < 0.0f) top = px;
				bottom = px;
			} else if (found && page > 0 && top >= 0.0f) {
				break;
			}
		}
		CHECK(top >= 0.0f && bottom > top);
		CHECK_NEAR(bottom - top + 1.0f, 792.0f * zoom);

		/* the middle of the first page is the middle of the page, in points */
		cy = (top + bottom) * 0.5f;
		CHECK(Rpdf_view_get_page_at(view, cx, cy, &page, &page_x, &page_y));
		CHECK(page == 0);
		/* (within a couple of points: the pixel probe rounds) */
		CHECK(page_x > 300.0f && page_x < 312.0f);
		CHECK(page_y > 390.0f && page_y < 402.0f);
		/* the gap under the page is on no page */
		CHECK(!Rpdf_view_get_page_at(view, cx, bottom + 6.0f, &page, &page_x, &page_y));

		/* a click on the page tells where it was, in points */
		Rpdf_view_set_page_clicked_callback(view, on_page_clicked, NULL);
		g_clicked_calls = 0;
		test_click_at(g_window, x + cx, y + cy);
		CHECK(g_clicked_calls == 1);
		CHECK(g_clicked_page == 0);
		CHECK(g_clicked_x > 300.0f && g_clicked_x < 312.0f);
		CHECK(g_clicked_y > 390.0f && g_clicked_y < 402.0f);
	}

	/* the callback of the pages needed fires during a frame, with the scale in pixels per point */
	Rpdf_view_set_page_needed_callback(view, on_page_needed, NULL);
	g_needed_calls = 0;
	Rdraw_frame();
	CHECK(g_needed_calls > 0);
	CHECK(g_needed_page >= 0 && g_needed_page < 3);
	CHECK(g_needed_scale > 0.0f);
	/* asked once: another frame at the same scale does not ask again */
	{
		int before = g_needed_calls;
		Rdraw_frame();
		CHECK(g_needed_calls == before);
	}

	/* a zoom asks for the pages again, at the new scale */
	{
		int before = g_needed_calls;
		float asked = g_needed_scale;
		Rpdf_view_set_zoom(view, 0.8f);
		Rdraw_frame();
		CHECK(g_needed_calls > before);
		CHECK(fabsf(g_needed_scale - asked) > 0.01f);
		Rpdf_view_set_zoom(view, 0.4f);
		Rdraw_frame();
	}

	/* fit width takes the width of the view */
	Rpdf_view_set_fit(view, R_PDF_FIT_WIDTH);
	CHECK(Rpdf_view_get_fit(view) == R_PDF_FIT_WIDTH);
	Rwidget_get_rect(view, &x, &y, &w, &h);
	{
		float fitted = Rpdf_view_get_zoom(view);
		CHECK(fitted > 0.0f);
		/* (the pages are narrower than the view, or just as wide) */
		CHECK(612.0f * fitted <= w + 1.0f);
		CHECK(Rpdf_view_get_fit(view) == R_PDF_FIT_WIDTH);
	}
	/* setting the zoom leaves the fit mode */
	Rpdf_view_set_zoom(view, 2.0f);
	CHECK(Rpdf_view_get_fit(view) == R_PDF_FIT_NONE);
	CHECK_NEAR(Rpdf_view_get_zoom(view), 2.0);
	EXPECT_ERRORS_BEGIN();
	Rpdf_view_set_fit(view, (RPdfFit)0x7FFF);
	EXPECT_ERRORS_END(1);
	/* the zoom is clamped */
	Rpdf_view_set_zoom(view, 1000.0f);
	CHECK(Rpdf_view_get_zoom(view) == 16.0f);
	Rpdf_view_set_zoom(view, 0.0f);
	CHECK(Rpdf_view_get_zoom(view) == 0.05f);
	Rpdf_view_set_zoom(view, 1.0f);

	/* going to a page scrolls it to the top */
	Rpdf_view_set_page(view, 2);
	CHECK(Rpdf_view_get_page(view) == 2);
	CHECK(Rpdf_view_get_visible_first_page(view) == 2);
	Rdraw_frame();
	CHECK(Rpdf_view_get_visible_last_page(view) >= 2);
	/* set_page clamps */
	Rpdf_view_set_page(view, 99);
	CHECK(Rpdf_view_get_page(view) == 2);
	Rpdf_view_set_page(view, -5);
	CHECK(Rpdf_view_get_page(view) == 0);
	Rdraw_frame();
	CHECK(Rpdf_view_get_visible_first_page(view) == 0);

	/* scroll_to_position brings a point of a page to the middle of the view */
	Rpdf_view_scroll_to_position(view, 1, 100.0f, 400.0f);
	CHECK(Rpdf_view_get_page(view) == 1);
	Rdraw_frame();
	CHECK(Rpdf_view_get_visible_first_page(view) >= 0);
	CHECK(Rpdf_view_get_visible_first_page(view) <= 1);
	CHECK(Rpdf_view_get_visible_last_page(view) >= 1);

	/* the keyboard: Page Up / Page Down with Ctrl go from page to page */
	Rwidget_focus(view);
	Rdraw_frame();
	test_key(g_window, R_KEY_PAGE_DOWN, R_MOD_CTRL);
	CHECK(Rpdf_view_get_page(view) == 2);
	Rdraw_frame();
	test_key(g_window, R_KEY_PAGE_UP, R_MOD_CTRL);
	CHECK(Rpdf_view_get_page(view) == 1);
	Rdraw_frame();
	test_key(g_window, R_KEY_HOME, 0);
	CHECK(Rpdf_view_get_page(view) == 0);
	test_key(g_window, R_KEY_END, 0);
	Rdraw_frame();
	CHECK(Rpdf_view_get_page(view) == 2);

	/* a page with no bitmap draws a placeholder, and the highlight does not break anything */
	Rpdf_view_set_highlight(view, 1, 20.0f, 30.0f, 200.0f, 40.0f);
	Rdraw_frame();
	Rpdf_view_set_highlight(view, -1, 0.0f, 0.0f, 0.0f, 0.0f);
	Rdraw_frame();
	Rpdf_view_set_background_color(view, 0.1f, 0.1f, 0.1f, 1.0f);
	Rdraw_frame();

	/* aliases of the tag */
	{
		Rid alias, alias_root;
		alias_root = content("<VBox><Pdf Name=\"a\" PageCount=\"2\"/><PdfPreview Name=\"b\" PageCount=\"5\"/></VBox>");
		alias = Rwidget_find(alias_root, "a");
		CHECK(alias != R_INVALID_ID);
		CHECK(Rwidget_get_type(alias) == R_WIDGET_PDF_VIEW);
		CHECK(Rpdf_view_get_page_count(alias) == 2);
		alias = Rwidget_find(alias_root, "b");
		CHECK(alias != R_INVALID_ID);
		CHECK(Rwidget_get_type(alias) == R_WIDGET_PDF_VIEW);
		CHECK(Rpdf_view_get_page_count(alias) == 5);
		/* (back to the view of the test) */
		root = content("<VBox><PdfView Name=\"pdf\" Height=\"400\" PageCount=\"3\" Zoom=\"1\" Fit=\"None\" PageGap=\"10\"/></VBox>");
		view = Rwidget_find(root, "pdf");
		CHECK(view != R_INVALID_ID);
		Rpdf_view_set_page_size(view, 0, 612.0f, 792.0f);
		Rpdf_view_set_page_size(view, 1, 612.0f, 792.0f);
		Rpdf_view_set_page_size(view, 2, 612.0f, 792.0f);
		Rpdf_view_set_page(view, 0);
		Rdraw_frame();
	}

	/* the attributes of the tag, through the loader and one by one */
	{
		Rid attr_root = content("<VBox><PdfView Name=\"q\" PageCount=\"4\" Page=\"2\" Zoom=\"2.5\" Fit=\"Page\" PageGap=\"4\"/></VBox>");
		Rid q = Rwidget_find(attr_root, "q");
		CHECK(Rpdf_view_get_page_count(q) == 4);
		CHECK(Rpdf_view_get_page(q) == 2);
		CHECK(Rpdf_view_get_fit(q) == R_PDF_FIT_PAGE);
		CHECK(Rpdf_view_get_page_gap(q) == 4.0f);
		CHECK(Rwidget_set_attribute(q, "Fit", "Width"));
		CHECK(Rpdf_view_get_fit(q) == R_PDF_FIT_WIDTH);
		CHECK(Rwidget_set_attribute(q, "Zoom", "1.5"));
		CHECK_NEAR(Rpdf_view_get_zoom(q), 1.5);
		CHECK(Rpdf_view_get_fit(q) == R_PDF_FIT_NONE);
		CHECK(Rwidget_set_attribute(q, "PageGap", "6"));
		CHECK(Rpdf_view_get_page_gap(q) == 6.0f);
		CHECK(Rwidget_set_attribute(q, "Page", "3"));
		CHECK(Rpdf_view_get_page(q) == 3);
		CHECK(!Rwidget_set_attribute(q, "Fit", "Diagonal"));
		CHECK(!Rwidget_set_attribute(q, "Nope", "1"));
		root = content("<VBox><PdfView Name=\"pdf\" Height=\"400\" PageCount=\"3\" Zoom=\"1\" Fit=\"None\" PageGap=\"10\"/></VBox>");
		view = Rwidget_find(root, "pdf");
		Rpdf_view_set_page_size(view, 0, 612.0f, 792.0f);
		Rpdf_view_set_page_size(view, 1, 612.0f, 792.0f);
		Rpdf_view_set_page_size(view, 2, 612.0f, 792.0f);
		Rpdf_view_set_page(view, 0);
		Rdraw_frame();
	}

	/* a bitmap is forgotten when R_INVALID_ID is given */
	Rpdf_view_invalidate_page(view, 0);
	Rpdf_view_set_page_bitmap(view, 0, R_INVALID_ID, 0.0f);
	CHECK(!Rpdf_view_has_page_bitmap(view, 0));
	Rdraw_frame();
	Rpdf_view_invalidate_page(view, 99);

	/* the wheel scrolls, Ctrl + wheel zooms */
	Rwidget_get_rect(view, &x, &y, &w, &h);
	Rwindow_inject_mouse_move(g_window, x + w * 0.5f, y + h * 0.5f);
	Rpdf_view_set_zoom(view, 1.0f);
	Rwindow_inject_key(g_window, R_KEY_LEFT_CTRL, R_TRUE, R_MOD_CTRL);
	Rwindow_inject_mouse_wheel(g_window, 0.0f, 3.0f);
	Rwindow_inject_key(g_window, R_KEY_LEFT_CTRL, R_FALSE, 0);
	Rdraw_frame();
	CHECK(Rpdf_view_get_zoom(view) > 1.0f);
	{
		float zoomed = Rpdf_view_get_zoom(view);
		Rwindow_inject_mouse_wheel(g_window, 0.0f, -2.0f);
		Rdraw_frame();
		CHECK(fabsf(Rpdf_view_get_zoom(view) - zoomed) < 0.001f);
	}

	/* a finger drags the pages; two fingers zoom them */
	Rpdf_view_set_zoom(view, 1.0f);
	Rpdf_view_set_page(view, 0);
	Rdraw_frame();
	{
		float before_page_y = -1.0f, after_page_y = -1.0f;
		Rwidget_get_rect(view, &x, &y, &w, &h);
		CHECK(Rpdf_view_get_page_at(view, w * 0.5f, 100.0f, &page, &page_x, &before_page_y));
		Rwindow_inject_touch(g_window, 1, R_TOUCH_DOWN, x + w * 0.5f, y + h * 0.7f);
		Rwindow_advance_touch(g_window, 0.016);
		Rwindow_inject_touch(g_window, 1, R_TOUCH_MOVE, x + w * 0.5f, y + h * 0.3f);
		Rwindow_advance_touch(g_window, 0.016);
		Rwindow_inject_touch(g_window, 1, R_TOUCH_MOVE, x + w * 0.5f, y + h * 0.1f);
		Rwindow_advance_touch(g_window, 1.0);
		Rwindow_inject_touch(g_window, 1, R_TOUCH_UP, x + w * 0.5f, y + h * 0.1f);
		Rdraw_frame();
		/* (the pages went up with the finger: what is at that point is further down the page) */
		CHECK(Rpdf_view_get_page_at(view, w * 0.5f, 100.0f, &page, &page_x, &after_page_y));
		CHECK(after_page_y > before_page_y + 100.0f);

		/* two fingers zoom */
		{
			float zoom = Rpdf_view_get_zoom(view);
			Rwindow_inject_touch(g_window, 2, R_TOUCH_DOWN, x + w * 0.4f, y + h * 0.5f);
			Rwindow_inject_touch(g_window, 3, R_TOUCH_DOWN, x + w * 0.6f, y + h * 0.5f);
			Rwindow_advance_touch(g_window, 0.016);
			Rwindow_inject_touch(g_window, 2, R_TOUCH_MOVE, x + w * 0.25f, y + h * 0.5f);
			Rwindow_inject_touch(g_window, 3, R_TOUCH_MOVE, x + w * 0.75f, y + h * 0.5f);
			Rwindow_advance_touch(g_window, 0.016);
			Rwindow_inject_touch(g_window, 2, R_TOUCH_UP, x + w * 0.25f, y + h * 0.5f);
			Rwindow_inject_touch(g_window, 3, R_TOUCH_UP, x + w * 0.75f, y + h * 0.5f);
			Rdraw_frame();
			CHECK(Rpdf_view_get_zoom(view) > zoom);
		}
	}

	/* the callback may hand the bitmap from inside the frame, and is not asked again */
	{
		Rid render_view = Rpdf_view_create();
		Rid host = Rwidget_create(R_WIDGET_VBOX);
		Rwidget_set_size(render_view, 300.0f, 300.0f);
		Rwidget_add_child(host, render_view);
		Rwindow_set_content(g_window, host);
		Rpdf_view_set_page_count(render_view, 2);
		Rpdf_view_set_page_size(render_view, 0, 612.0f, 792.0f);
		Rpdf_view_set_page_size(render_view, 1, 612.0f, 792.0f);
		Rpdf_view_set_page_needed_callback(render_view, on_page_render, NULL);
		g_render_texture = make_bitmap();
		CHECK(g_render_texture != R_INVALID_ID);
		g_rendered = 0;
		Rdraw_frame();
		CHECK(g_rendered > 0);
		CHECK(Rpdf_view_has_page_bitmap(render_view, 0));
		{
			int before = g_rendered;
			Rdraw_frame();
			CHECK(g_rendered == before);
		}
		Rtexture_destroy(g_render_texture);

		/* (the view of the rest of the test replaced the one the window had) */
		root = content("<VBox><PdfView Name=\"pdf\" Height=\"400\" PageCount=\"3\" Zoom=\"1\" Fit=\"None\" PageGap=\"10\"/></VBox>");
		view = Rwidget_find(root, "pdf");
		CHECK(view != R_INVALID_ID);
		Rpdf_view_set_page_size(view, 0, 612.0f, 792.0f);
		Rpdf_view_set_page_size(view, 1, 612.0f, 792.0f);
		Rpdf_view_set_page_size(view, 2, 612.0f, 792.0f);
		Rdraw_frame();
	}

	/* 0 pages clears the view */
	Rpdf_view_set_page_count(view, 0);
	CHECK(Rpdf_view_get_page_count(view) == 0);
	CHECK(Rpdf_view_get_page(view) == 0);
	CHECK(!Rpdf_view_get_page_at(view, w * 0.5f, h * 0.5f, &page, &page_x, &page_y));
	Rdraw_frame();
	Rpdf_view_set_page_count(view, 3);
	CHECK(Rpdf_view_get_page_count(view) == 3);

	/* every allocation of the view goes with it */
	for (i = 0; i < 1000; i++) Rpdf_view_set_page_count(view, i);
	CHECK(Rpdf_view_get_page_count(view) == 999);
	Rpdf_view_set_page_count(view, 3);

	Rtexture_destroy(texture);
	Rwindow_set_content(g_window, R_INVALID_ID);
	Rdraw_frame();
}

int main(void) {
	if (!test_init(0)) {
		printf("cannot initialize Raven\n");
		return 1;
	}
	g_window = Rwindow_create("pdf view", 500, 400, R_WINDOW_DEFAULT);

	RUN(test_pdf_view);

	Rdraw_frame();
	Rshutdown();
	return test_report("test_pdf_view");
}
