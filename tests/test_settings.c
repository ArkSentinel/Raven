/**
 * Raven tests - the settings store (a JSON file), keybindings read from a
 * text, the theme remembered for a project, and the animation of the
 * selection highlight of lists.
 */

#include "test.h"

#include <stdlib.h>
#if !defined(_WIN32)
#include <unistd.h>
#endif

static char* temp_path(const char* name) {
#if defined(_WIN32)
	const char* base = getenv("TEMP");
	char* path;
	if (!base || !base[0]) base = ".";
	path = (char*)Rmemory_alloc(strlen(base) + strlen(name) + 2);
	sprintf(path, "%s\\%s", base, name);
#else
	const char* base = getenv("TMPDIR");
	char* path;
	if (!base || !base[0]) base = "/tmp";
	path = (char*)Rmemory_alloc(strlen(base) + strlen(name) + 2);
	sprintf(path, "%s/%s", base, name);
#endif
	return path;
}

/* ---------------------------------------------------------------------------
 *  Values
 * ------------------------------------------------------------------------- */

static void test_values(void) {
	Rid settings = Rsettings_create(NULL);
	CHECK(Rsettings_get_path(settings)[0] == '\0');
	CHECK(Rsettings_get_type(settings, "nothing") == R_SETTING_NONE);
	CHECK(!Rsettings_has(settings, "nothing"));

	/* the defaults */
	CHECK(Rsettings_get_bool(settings, "missing", R_TRUE) == R_TRUE);
	CHECK(Rsettings_get_int(settings, "missing", 42) == 42);
	CHECK(Rsettings_get_number(settings, "missing", 1.5) == 1.5);
	CHECK_STR(Rsettings_get_text(settings, "missing", "fallback"), "fallback");

	/* the four types */
	Rsettings_set_bool(settings, "editor.wrap", R_TRUE);
	Rsettings_set_int(settings, "editor.font_size", 13);
	Rsettings_set_number(settings, "editor.line_height", 1.25);
	Rsettings_set_text(settings, "editor.font", "Iosevka");
	CHECK(Rsettings_get_type(settings, "editor.wrap") == R_SETTING_BOOL);
	CHECK(Rsettings_get_type(settings, "editor.font_size") == R_SETTING_INT);
	CHECK(Rsettings_get_type(settings, "editor.line_height") == R_SETTING_NUMBER);
	CHECK(Rsettings_get_type(settings, "editor.font") == R_SETTING_TEXT);
	CHECK(Rsettings_get_bool(settings, "editor.wrap", R_FALSE) == R_TRUE);
	CHECK(Rsettings_get_int(settings, "editor.font_size", 0) == 13);
	CHECK(Rsettings_get_number(settings, "editor.line_height", 0.0) == 1.25);
	CHECK_STR(Rsettings_get_text(settings, "editor.font", ""), "Iosevka");
	/* an integer reads as a number, a number as an integer */
	CHECK(Rsettings_get_number(settings, "editor.font_size", 0.0) == 13.0);
	CHECK(Rsettings_get_int(settings, "editor.line_height", 0) == 1);
	/* a type that does not match gives the fallback */
	CHECK(Rsettings_get_int(settings, "editor.font", 7) == 7);
	CHECK_STR(Rsettings_get_text(settings, "editor.font_size", "none"), "none");

	/* removing */
	CHECK(Rsettings_remove(settings, "editor.wrap"));
	CHECK(!Rsettings_has(settings, "editor.wrap"));
	CHECK(!Rsettings_remove(settings, "editor.wrap"));
	/* the others stay */
	CHECK(Rsettings_get_int(settings, "editor.font_size", 0) == 13);
	/* a NULL text removes the key, an empty one stores "" */
	Rsettings_set_text(settings, "editor.font", NULL);
	CHECK(!Rsettings_has(settings, "editor.font"));
	Rsettings_set_text(settings, "editor.font", "");
	CHECK(Rsettings_has(settings, "editor.font"));
	CHECK_STR(Rsettings_get_text(settings, "editor.font", "none"), "");
	/* a text without a fallback is NULL when it is not there */
	CHECK(Rsettings_get_text(settings, "editor.missing", NULL) == NULL);

	/* a path of objects */
	Rsettings_set_int(settings, "tools.latexmk.timeout", 30);
	CHECK(Rsettings_get_int(settings, "tools.latexmk.timeout", 0) == 30);

	Rsettings_destroy(settings);
}

/* ---------------------------------------------------------------------------
 *  JSON
 * ------------------------------------------------------------------------- */

static void test_json(void) {
	Rid settings = Rsettings_create(NULL);
	/* escaping: quotes, a backslash, a newline, a tab, a Unicode char */
	const char* tricky = "a \"quoted\" \\ path\nsecond\tline \xC3\xA9 \xE2\x9C\x93";
	char json_copy[512];
	strcpy(json_copy, tricky);
	Rsettings_set_text(settings, "text", tricky);
	CHECK_STR(Rsettings_get_text(settings, "text", ""), json_copy);

	/* doubles: no trailing zeros, an integer has no decimal point */
	Rsettings_set_number(settings, "pi", 3.1400000000000001);
	Rsettings_set_number(settings, "one", 1.0);
	Rsettings_set_int(settings, "count", 7);
	Rsettings_set_int(settings, "negative", -12);
	{
		const char* json = Rsettings_get_json(settings);
		CHECK(strstr(json, "\"pi\": 3.14") != NULL);
		CHECK(strstr(json, "\"count\": 7") != NULL);
		CHECK(strstr(json, "\"negative\": -12") != NULL);
		CHECK(strstr(json, "\"text\": \"a \\\"quoted\\\" \\\\ path\\nsecond\\tline") != NULL);
	}

	/* the whole document, an array included */
	CHECK(Rsettings_set_json(settings, "{\"recent\": [\"a.tex\", \"b.tex\"], \"n\": 2}"));
	CHECK_STR(Rsettings_get_text(settings, "recent", "none"), "none"); /* an array is not a text */
	CHECK(Rsettings_get_key_count(settings, "recent") == 2);
	CHECK_STR(Rsettings_get_key(settings, "recent", 0), "0");
	CHECK_STR(Rsettings_get_key(settings, "recent", 1), "1");
	CHECK(Rsettings_get_key(settings, "recent", 2) == NULL);
	CHECK(Rsettings_get_int(settings, "n", 0) == 2);

	/* the keys of an object */
	CHECK(Rsettings_get_key_count(settings, "") == 2);
	{
		const char* first = Rsettings_get_key(settings, "", 0);
		const char* second = Rsettings_get_key(settings, "", 1);
		CHECK(first != NULL && second != NULL);
		CHECK((strcmp(first, "recent") == 0 && strcmp(second, "n") == 0) ||
			(strcmp(first, "n") == 0 && strcmp(second, "recent") == 0));
	}
	CHECK(Rsettings_get_key(settings, "", 2) == NULL);

	/* nested objects are flattened to dotted keys */
	CHECK(Rsettings_get_json(settings)[0] != '\0');
	CHECK(Rsettings_get_key_count(settings, "recent") == 2);
	Rsettings_set_json(settings, "{\"editor\": {\"font_size\": 12, \"font\": \"Mono\"}}");
	CHECK(Rsettings_get_int(settings, "editor.font_size", 0) == 12);
	CHECK_STR(Rsettings_get_text(settings, "editor.font", ""), "Mono");
	CHECK(Rsettings_get_key_count(settings, "editor") == 2);
	CHECK(Rsettings_get_key_count(settings, "") == 1);

	/* a literal key with dots wins over a path */
	Rsettings_set_json(settings, "{\"a.b\": 1, \"a\": {\"b\": 2}}");
	CHECK(Rsettings_get_int(settings, "a.b", 0) == 1);

	/* comments and trailing commas are tolerated */
	CHECK(Rsettings_set_json(settings, "{\n // a comment\n \"a\": 1, /* block */ \"b\": [1, 2,],\n}"));
	CHECK(Rsettings_get_int(settings, "a", 0) == 1);
	CHECK(Rsettings_get_key_count(settings, "b") == 2);

	/* Unicode escapes, a surrogate pair included */
	CHECK(Rsettings_set_json(settings, "{\"c\": \"\\u00e9\\u2713\\ud83d\\ude00\"}"));
	CHECK_STR(Rsettings_get_text(settings, "c", ""), "\xC3\xA9\xE2\x9C\x93\xF0\x9F\x98\x80");

	/* malformed JSON is refused and the store keeps what it had */
	Rsettings_set_int(settings, "keep", 5);
	EXPECT_ERRORS_BEGIN();
	CHECK(!Rsettings_set_json(settings, "{\"a\": }"));
	CHECK(!Rsettings_set_json(settings, "{,}"));
	CHECK(!Rsettings_set_json(settings, "{\"a\": 1"));
	CHECK(!Rsettings_set_json(settings, "[1, 2"));
	CHECK(!Rsettings_set_json(settings, "{\"a\": 01}"));
	CHECK(!Rsettings_set_json(settings, "{\"a\": \"unterminated}"));
	CHECK(!Rsettings_set_json(settings, ""));
	CHECK(!Rsettings_set_json(settings, NULL));
	/* (a deeply nested document is refused instead of recursing without a limit) */
	{
		char deep[4096];
		int i;
		size_t used = 0;
		for (i = 0; i < 200; i++) used += (size_t)snprintf(deep + used, sizeof(deep) - used, "{\"a\":");
		used += (size_t)snprintf(deep + used, sizeof(deep) - used, "1");
		for (i = 0; i < 200; i++) used += (size_t)snprintf(deep + used, sizeof(deep) - used, "}");
		CHECK(!Rsettings_set_json(settings, deep));
	}
	EXPECT_ERRORS_END(0);
	CHECK(Rsettings_get_int(settings, "keep", 0) == 5);

	Rsettings_destroy(settings);
}

/* ---------------------------------------------------------------------------
 *  Files
 * ------------------------------------------------------------------------- */

static void test_files(void) {
	char* path = temp_path("raven_test_settings.json");
	char* second = temp_path("raven_test_settings_2.json");
	Rid first, reloaded;

	remove(path);
	remove(second);
	first = Rsettings_create(path);
	CHECK(first != R_INVALID_ID);
	CHECK_STR(Rsettings_get_path(first), path);
	/* (a file that does not exist yet: nothing is read) */
	CHECK(!Rsettings_load(first));
	CHECK(Rsettings_get_int(first, "a", 0) == 0);

	Rsettings_set_int(first, "a", 1);
	Rsettings_set_text(first, "b", "x \"y\"\n\xC3\xA9");
	Rsettings_set_number(first, "c", 0.5);
	Rsettings_set_bool(first, "d", R_TRUE);
	CHECK(Rsettings_save(first));
	CHECK(Rsettings_load(first));
	CHECK(Rsettings_get_int(first, "a", 0) == 1);
	CHECK(Rsettings_get_text(first, "b", "")[0] != '\0');
	CHECK(Rsettings_get_number(first, "c", 0.0) == 0.5);
	CHECK(Rsettings_get_bool(first, "d", R_FALSE) == R_TRUE);

	/* a second store reads the same file */
	reloaded = Rsettings_create(path);
	CHECK(reloaded != R_INVALID_ID);
	CHECK(Rsettings_get_int(reloaded, "a", 0) == 1);
	CHECK_STR(Rsettings_get_text(reloaded, "b", ""), "x \"y\"\n\xC3\xA9");
	CHECK(Rsettings_get_number(reloaded, "c", 0.0) == 0.5);

	/* a change is not saved by itself */
	Rsettings_set_int(reloaded, "a", 2);
	CHECK(Rsettings_save(reloaded));
	CHECK(Rsettings_load(first));
	CHECK(Rsettings_get_int(first, "a", 0) == 2);

	/* a store in memory only cannot be saved */
	{
		Rid memory_only = Rsettings_create(NULL);
		EXPECT_ERRORS_BEGIN();
		CHECK(!Rsettings_save(memory_only));
		EXPECT_ERRORS_END(1);
		Rsettings_destroy(memory_only);
	}

	/* (a file that is not JSON: load reports it and the store ends empty) */
	{
		Rid broken;
		FILE* file = fopen(second, "wb");
		CHECK(file != NULL);
		if (file) {
			fputs("{ this is not json", file);
			fclose(file);
		}
		broken = Rsettings_create(second);
		CHECK(Rsettings_get_int(broken, "a", 7) == 7);
		remove(second);
		/* (a file that was there and does not parse: R_FALSE, and nothing is kept) */
		file = fopen(second, "wb");
		if (file) {
			fputs("{ this is not json", file);
			fclose(file);
		}
		CHECK(!Rsettings_load(broken));
		CHECK(Rsettings_get_int(broken, "a", 7) == 7);
		Rsettings_destroy(broken);
		remove(second);
	}

	Rsettings_destroy(first);
	Rsettings_destroy(reloaded);
	remove(path);

	/* the usual folders exist and hold the name of the application
	 * (the test sends them to a folder of the system temporary directory) */
	{
		char* config_root = temp_path("raven_test_settings_config");
		char* path_in_folder;
		Rid opened;
#if defined(_WIN32)
		_putenv_s("APPDATA", config_root);
#else
		setenv("XDG_CONFIG_HOME", config_root, 1);
#endif
		{
			const char* config = Rsettings_get_config_folder("raven_test_settings");
			CHECK(config != NULL && config[0] != '\0');
			CHECK(strstr(config, "raven_test_settings") != NULL);
			CHECK(strstr(config, config_root) == config);
		}
		/* the cache folder is somewhere else */
		CHECK(Rsettings_get_cache_folder("raven_test_settings")[0] != '\0');
		/* Rsettings_open makes the folder and the file */
		opened = Rsettings_open("raven_test_settings", "sessions.json");
		CHECK(opened != R_INVALID_ID);
		Rsettings_set_text(opened, "last_project", "/tmp/x");
		CHECK(Rsettings_save(opened));
		path_in_folder = (char*)Rmemory_alloc(strlen(config_root) + 64);
		sprintf(path_in_folder, "%s/raven_test_settings/sessions.json", config_root);
		CHECK(Rsettings_create(path_in_folder) != R_INVALID_ID);
		remove(path_in_folder);
		Rmemory_free(path_in_folder);
#if !defined(_WIN32)
		{
			char* folder = (char*)Rmemory_alloc(strlen(config_root) + 32);
			sprintf(folder, "%s/raven_test_settings", config_root);
			rmdir(folder);
			Rmemory_free(folder);
			rmdir(config_root);
		}
#endif
		/* (left alive on purpose: Rshutdown must free it) */
		Rmemory_free(config_root);
	}

	Rmemory_free(path);
	Rmemory_free(second);
}

/* ---------------------------------------------------------------------------
 *  Keybindings
 * ------------------------------------------------------------------------- */

static int g_fired;
static char g_fired_name[64];

static void on_action(Rid sender, const char* action, void* user_data) {
	(void)sender;
	(void)user_data;
	g_fired++;
	snprintf(g_fired_name, sizeof(g_fired_name), "%s", action);
}

static void test_key_names(void) {
	RKey key = R_KEY_UNKNOWN;
	Ruint modifiers = 0;

	CHECK(Rkey_from_name("Ctrl+S", &key, &modifiers));
	CHECK(key == R_KEY_S);
	CHECK(modifiers == R_MOD_CTRL);
	CHECK(Rkey_from_name("Ctrl+Shift+S", &key, &modifiers));
	CHECK(key == R_KEY_S && modifiers == (R_MOD_CTRL | R_MOD_SHIFT));
	CHECK(Rkey_from_name("F5", &key, &modifiers));
	CHECK(key == R_KEY_F5 && modifiers == 0);
	CHECK(Rkey_from_name("Escape", &key, &modifiers));
	CHECK(key == R_KEY_ESCAPE);
	CHECK(Rkey_from_name("PageUp", &key, &modifiers));
	CHECK(key == R_KEY_PAGE_UP);
	CHECK(Rkey_from_name("A", &key, &modifiers));
	CHECK(key == R_KEY_A);
	CHECK(Rkey_from_name("Alt+Shift+F1", &key, &modifiers));
	CHECK(key == R_KEY_F1 && modifiers == (R_MOD_ALT | R_MOD_SHIFT));
	/* Cmd is Super, as it is on macOS */
	CHECK(Rkey_from_name("Cmd+K", &key, &modifiers));
	CHECK(key == R_KEY_K && modifiers == R_MOD_SUPER);

	EXPECT_ERRORS_BEGIN();
	CHECK(!Rkey_from_name("Nonsense", &key, &modifiers));
	CHECK(!Rkey_from_name("Ctrl+", &key, &modifiers));
	CHECK(!Rkey_from_name("", &key, &modifiers));
	CHECK(!Rkey_from_name(NULL, &key, &modifiers));
	EXPECT_ERRORS_END(0);

	/* and back */
	CHECK_STR(Rkey_to_name(R_KEY_S, R_MOD_CTRL), "Ctrl+S");
	CHECK_STR(Rkey_to_name(R_KEY_S, R_MOD_CTRL | R_MOD_SHIFT), "Ctrl+Shift+S");
	CHECK_STR(Rkey_to_name(R_KEY_F5, 0), "F5");
	CHECK_STR(Rkey_to_name(R_KEY_ESCAPE, 0), "Escape");
	CHECK(Rkey_to_name(R_KEY_UNKNOWN, 0) == NULL);

	/* the round trip */
	{
		static const struct { RKey key; Ruint modifiers; } keys[] = {
			{ R_KEY_S, R_MOD_CTRL }, { R_KEY_P, R_MOD_CTRL | R_MOD_SHIFT },
			{ R_KEY_F5, 0 }, { R_KEY_PAGE_DOWN, R_MOD_ALT }, { R_KEY_Z, R_MOD_SUPER }
		};
		size_t i;
		for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
			const char* name = Rkey_to_name(keys[i].key, keys[i].modifiers);
			RKey parsed;
			Ruint parsed_modifiers;
			CHECK(name != NULL);
			CHECK(Rkey_from_name(name, &parsed, &parsed_modifiers));
			CHECK(parsed == keys[i].key);
			CHECK(parsed_modifiers == keys[i].modifiers);
		}
	}
}

static void test_window_bindings(void) {
	Rid window = Rwindow_create("keybindings", 300, 200, R_WINDOW_DEFAULT);
	Rid content = Rwidget_create(R_WIDGET_LABEL);
	int count;
	RKey key;
	Ruint modifiers;
	const char* action = NULL;

	Raction_bind("save", on_action, NULL);
	Raction_bind("save_as", on_action, NULL);
	Raction_bind("build", on_action, NULL);
	Raction_bind("clear", on_action, NULL);
	Rwindow_set_content(window, content);
	Rdraw_frame();

	CHECK(Rwindow_get_shortcut_count(window) == 0);
	count = Rwindow_set_keybindings(window, "Ctrl+S save | Ctrl+Shift+S save_as | F5 build | Ctrl+K,Ctrl+B clear");
	CHECK(count == 4);
	CHECK(Rwindow_get_shortcut_count(window) == 4);

	g_fired = 0;
	test_key(window, R_KEY_S, R_MOD_CTRL);
	CHECK(g_fired == 1);
	CHECK_STR(g_fired_name, "save");
	test_key(window, R_KEY_S, R_MOD_CTRL | R_MOD_SHIFT);
	CHECK(g_fired == 2);
	CHECK_STR(g_fired_name, "save_as");
	test_key(window, R_KEY_F5, 0);
	CHECK(g_fired == 3);
	CHECK_STR(g_fired_name, "build");
	/* a sequence binds its first chord for now */
	test_key(window, R_KEY_K, R_MOD_CTRL);
	CHECK(g_fired == 4);
	CHECK_STR(g_fired_name, "clear");
	test_key(window, R_KEY_B, R_MOD_CTRL);
	CHECK(g_fired == 4);

	/* the listing of a preferences window */
	CHECK(Rwindow_get_shortcut(window, 0, &key, &modifiers, &action));
	CHECK(key != R_KEY_UNKNOWN);
	CHECK(action != NULL && action[0] != '\0');
	CHECK(!Rwindow_get_shortcut(window, 4, &key, &modifiers, &action));
	CHECK(!Rwindow_get_shortcut(window, -1, &key, &modifiers, &action));

	/* an entry with no action unbinds its key */
	CHECK(Rwindow_set_keybindings(window, "Ctrl+S  | F5 build") == 2);
	CHECK(Rwindow_get_shortcut_count(window) == 3);
	g_fired = 0;
	test_key(window, R_KEY_S, R_MOD_CTRL);
	CHECK(g_fired == 0);
	test_key(window, R_KEY_F5, 0);
	CHECK(g_fired == 1);

	/* a name that is not a key is skipped */
	EXPECT_ERRORS_BEGIN();
	CHECK(Rwindow_set_keybindings(window, "Ctrl+S save | NotAKey nope") == 1);
	EXPECT_ERRORS_END(1);
	CHECK(Rwindow_get_shortcut_count(window) == 4);

	Raction_unbind("save");
	Raction_unbind("save_as");
	Raction_unbind("build");
	Raction_unbind("clear");
	Rwindow_destroy(window);
}

/* ---------------------------------------------------------------------------
 *  The theme remembered for a project
 * ------------------------------------------------------------------------- */

static void test_theme_memory(void) {
	Rid settings = Rsettings_create(NULL);
	const char* theme = Rtheme_get_name_at(0);
	const char* other = Rtheme_get_name_at(1);
	const char* applied;

	CHECK(theme != NULL);
	CHECK(other != NULL);
	CHECK(Rtheme_get_remembered(settings, "/tmp/raven_project") == NULL);
	/* nothing remembered: nothing is applied */
	EXPECT_ERRORS_BEGIN();
	CHECK(!Rtheme_apply_remembered(settings, "/tmp/raven_project"));
	EXPECT_ERRORS_END(0);

	CHECK(Rtheme_remember_for(settings, "/tmp/raven_project", theme));
	CHECK_STR(Rtheme_get_remembered(settings, "/tmp/raven_project"), theme);
	/* relative and absolute paths, and backslashes, name the same project */
	CHECK(Rtheme_remember_for(settings, "/tmp/raven_project/../raven_project", other));
	CHECK_STR(Rtheme_get_remembered(settings, "/tmp/raven_project"), other);
	CHECK_STR(Rtheme_get_remembered(settings, "/tmp/raven_project/"), other);

	/* applying one that Rtheme_use knows */
	CHECK(Rtheme_use(theme));
	CHECK(Rtheme_apply_remembered(settings, "/tmp/raven_project"));
	CHECK_STR(Rtheme_get_name(), other);
	CHECK(Rtheme_use(theme));

	/* an unknown theme leaves the theme in use as it was */
	Rtheme_remember_for(settings, "/tmp/raven_other", "NoSuchTheme");
	EXPECT_ERRORS_BEGIN();
	CHECK(!Rtheme_apply_remembered(settings, "/tmp/raven_other"));
	EXPECT_ERRORS_END(1);
	applied = Rtheme_get_name();
	CHECK(applied != NULL);
	CHECK_STR(applied, theme);

	Rsettings_destroy(settings);
}

/* ---------------------------------------------------------------------------
 *  The animation of the selection
 * ------------------------------------------------------------------------- */

static void test_selection_animation(void) {
	Rid window;
	Rid list;
	float x, y, w, h;

	/* (test_init already turned the animations off; this is the switch of the selection) */
	Rset_selection_animation(R_TRUE);
	CHECK(Rget_selection_animation() == R_TRUE);
	Rset_selection_animation(R_FALSE);
	CHECK(Rget_selection_animation() == R_FALSE);
	/* the switch does not depend on the other animations */
	Rset_selection_animation(R_TRUE);
	CHECK(Rget_selection_animation() == R_TRUE && !Rget_animations_enabled());
	Rset_selection_animation(R_FALSE);

	{
		static const char xml[] =
			"<Window Width=\"240\" Height=\"200\">"
			"<ListView Name=\"list\"><Item Text=\"a\"/><Item Text=\"b\"/><Item Text=\"c\"/><Item Text=\"d\"/></ListView>"
			"</Window>";
		window = Rwindow_create_from_xml(xml, strlen(xml));
	}
	CHECK(Rwindow_is_valid(window));
	list = Rwidget_find(window, "list");
	CHECK(list != R_INVALID_ID);
	Rdraw_frame();
	Rwidget_get_rect(list, &x, &y, &w, &h);
	{
		float row = Rtheme_get_metric(R_METRIC_ROW_HEIGHT);
		/* a click selects the third row; with no glide it is there at once */
		test_click_at(window, x + w * 0.5f, y + 1.0f + row * 2.5f);
		CHECK(Rlist_view_get_selected(list) == 2);
		Rdraw_frame();
		CHECK(Rlist_view_get_selected(list) == 2);
		/* and the keyboard still moves it */
		Rwidget_focus(list);
		test_key(window, R_KEY_DOWN, 0);
		Rdraw_frame();
		CHECK(Rlist_view_get_selected(list) == 3);
	}
	Rwindow_destroy(window);

	/* with the glide on, a frame runs and the selection is right */
	Rset_selection_animation(R_TRUE);
	Rset_animations_enabled(R_TRUE);
	{
		static const char xml[] =
			"<Window Width=\"240\" Height=\"200\">"
			"<ListView Name=\"list\"><Item Text=\"a\"/><Item Text=\"b\"/></ListView>"
			"</Window>";
		window = Rwindow_create_from_xml(xml, strlen(xml));
	}
	list = Rwidget_find(window, "list");
	Rdraw_frame();
	Rwidget_get_rect(list, &x, &y, &w, &h);
	test_click_at(window, x + w * 0.5f, y + 1.0f + Rtheme_get_metric(R_METRIC_ROW_HEIGHT) * 1.5f);
	CHECK(Rlist_view_get_selected(list) == 1);
	Rdraw_frame();
	Rdraw_frame();
	CHECK(Rlist_view_get_selected(list) == 1);
	Rwindow_destroy(window);
	Rset_animations_enabled(R_FALSE);
	Rset_selection_animation(R_FALSE);
}

/* A resize is the new size, not an animation: one frame after it, the content has it. */
static void test_resize_immediate(void) {
	Rid window = Rwindow_create("resize", 200, 120, R_WINDOW_DEFAULT);
	Rid content;
	float x, y, w, h;

	Rset_animations_enabled(R_TRUE);
	content = Rwidget_create(R_WIDGET_VBOX);
	Rwindow_set_content(window, content);
	Rdraw_frame();
	Rwidget_get_rect(content, &x, &y, &w, &h);
	CHECK_NEAR(w, 200);
	CHECK_NEAR(h, 120);

	Rwindow_set_size(window, 420, 310);
	Rdraw_frame();
	Rwidget_get_rect(content, &x, &y, &w, &h);
	CHECK_NEAR(w, 420);
	CHECK_NEAR(h, 310);

	/* the same with the animations off */
	Rset_animations_enabled(R_FALSE);
	Rwindow_set_size(window, 260, 180);
	Rdraw_frame();
	Rwidget_get_rect(content, &x, &y, &w, &h);
	CHECK_NEAR(w, 260);
	CHECK_NEAR(h, 180);

	Rwindow_destroy(window);
	Rset_animations_enabled(R_FALSE);
}

int main(void) {
	if (!test_init(0)) {
		printf("test_settings: cannot initialize Raven\n");
		return 1;
	}
	RUN(test_values);
	RUN(test_json);
	RUN(test_files);
	RUN(test_key_names);
	RUN(test_window_bindings);
	RUN(test_theme_memory);
	RUN(test_resize_immediate);
	RUN(test_selection_animation);
	Rshutdown();
	return test_report("test_settings");
}
