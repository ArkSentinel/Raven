/**
 * Raven tests - what an editor of LaTeX needs from the text editor: snippets,
 * folding, search and replace, the languages of TeX and BibTeX, and words.
 */

#include "test.h"

static Rid g_window, g_editor;

static void key(RKey k, Ruint modifiers) {
	Rwindow_inject_key(g_window, k, R_TRUE, modifiers);
	Rwindow_inject_key(g_window, k, R_FALSE, modifiers);
}

static void type(const char* text) {
	Rwindow_inject_text(g_window, text);
}

static void open_editor(const char* language) {
	char xml[256];
	snprintf(xml, sizeof(xml), "<Window Width=\"500\" Height=\"400\"><TextEditor Name=\"e\" Language=\"%s\" LineNumbers=\"true\"/></Window>", language);
	g_window = Rwindow_create_from_xml(xml, strlen(xml));
	CHECK(Rwindow_is_valid(g_window));
	g_editor = Rwidget_find(g_window, "e");
	Rdraw_frame();
	Rwidget_focus(g_editor);
}

static int cursor_line(void) {
	int line = -1, column = 0;
	Rtext_editor_get_cursor(g_editor, &line, &column);
	return line;
}

static RToken token_at(int line, const char* word) {
	const char* text = Rtext_editor_get_line(g_editor, line);
	const char* at = text ? strstr(text, word) : NULL;
	if (!at) return (RToken)-1;
	return Rtext_editor_get_token(g_editor, line, (int)(at - text));
}

/* ---------------------------------------------------------------------------
 *  The languages
 * ------------------------------------------------------------------------- */

static void test_latex_language(void) {
	open_editor("LaTeX");
	CHECK_STR(Rtext_editor_get_language(g_editor), "LaTeX");
	CHECK_STR(Rlanguage_for_file("main.tex"), "LaTeX");
	CHECK_STR(Rlanguage_for_file("refs.bib"), "BibTeX");

	Rtext_editor_set_text(g_editor,
		"\\documentclass{article}\n"
		"\\begin{document}\n"
		"% a comment \\section\n"
		"\\section{Title}\n"
		"Text with $x^2 + y^2$ and 12.\n"
		"\\end{document}\n");
	/* a command is a keyword, and its environment a tag */
	CHECK(token_at(0, "\\documentclass") == R_TOKEN_KEYWORD);
	CHECK(token_at(0, "article") == R_TOKEN_TAG);
	CHECK(token_at(1, "\\begin") == R_TOKEN_KEYWORD);
	CHECK(token_at(1, "document") == R_TOKEN_TAG);
	/* a percent sign starts a comment, to the end of the line */
	CHECK(token_at(2, "% a comment") == R_TOKEN_COMMENT);
	CHECK(token_at(2, "\\section") == R_TOKEN_COMMENT);
	/* a command with an argument */
	CHECK(token_at(3, "\\section") == R_TOKEN_KEYWORD);
	CHECK(token_at(3, "Title") == R_TOKEN_TEXT);
	/* mathematics is a text, and a number a number */
	CHECK(token_at(4, "$x^2") == R_TOKEN_STRING);
	CHECK(token_at(4, "12") == R_TOKEN_NUMBER);
	CHECK(token_at(5, "\\end") == R_TOKEN_KEYWORD);
	CHECK(token_at(5, "document") == R_TOKEN_TAG);
	/* (the % of a line that follows a text is still a comment) */
	Rtext_editor_set_text(g_editor, "50\\% of it % and a comment");
	CHECK(token_at(0, "\\%") == R_TOKEN_KEYWORD);
	CHECK(token_at(0, "% and a comment") == R_TOKEN_COMMENT);
	/* (the arguments of \\begin and \\end of another line) */
	Rtext_editor_set_text(g_editor, "\\begin{verbatim}\n\\section{no}\n\\end{verbatim}");
	CHECK(token_at(1, "\\section") == R_TOKEN_STRING);
	Rwindow_destroy(g_window);
}

static void test_bibtex_language(void) {
	open_editor("BibTeX");
	Rtext_editor_set_text(g_editor,
		"@article{knuth1984,\n"
		"  author = {Donald Knuth},\n"
		"  title = \"Literate Programming\",\n"
		"  year = 1984,\n"
		"}\n");
	CHECK(token_at(0, "@article") == R_TOKEN_KEYWORD);
	CHECK(token_at(0, "knuth1984") == R_TOKEN_TAG);
	CHECK(token_at(1, "author") == R_TOKEN_ATTRIBUTE);
	CHECK(token_at(1, "Donald Knuth") == R_TOKEN_STRING);
	CHECK(token_at(2, "title") == R_TOKEN_ATTRIBUTE);
	CHECK(token_at(3, "year") == R_TOKEN_ATTRIBUTE);
	CHECK(token_at(3, "1984") == R_TOKEN_NUMBER);
	Rwindow_destroy(g_window);
}

/* ---------------------------------------------------------------------------
 *  Words, and the line of the caret
 * ------------------------------------------------------------------------- */

static void test_counts(void) {
	open_editor("LaTeX");
	Rtext_editor_set_text(g_editor, "one two three\n\n  four\n");
	CHECK(Rtext_editor_get_word_count(g_editor) == 4);
	CHECK(Rtext_editor_get_non_empty_line_count(g_editor) == 2);
	Rtext_editor_set_cursor(g_editor, 2, 3);
	CHECK(Rtext_editor_get_current_line(g_editor) == 3);
	CHECK(Rtext_editor_get_character_count(g_editor) == 19);
	/* (a command is not a word of the text) */
	Rtext_editor_set_text(g_editor, "\\section{Hello world}\n% a comment\n");
	CHECK(Rtext_editor_get_word_count(g_editor) == 2);
	Rwindow_destroy(g_window);
}

/* ---------------------------------------------------------------------------
 *  Search and replace
 * ------------------------------------------------------------------------- */

static void test_search(void) {
	int line, column, to_line, to_column;
	open_editor("LaTeX");
	Rtext_editor_set_text(g_editor, "alpha beta gamma\nbeta beta\nGamma");

	CHECK(Rtext_editor_set_search(g_editor, "beta", R_SEARCH_NONE));
	CHECK_STR(Rtext_editor_get_search(g_editor), "beta");
	CHECK(Rtext_editor_count_matches(g_editor) == 3);
	CHECK(Rtext_editor_get_match(g_editor, 0, &line, &column, &to_line, &to_column));
	CHECK(line == 0 && column == 6 && to_line == 0 && to_column == 10);
	CHECK(Rtext_editor_get_match(g_editor, 2, &line, &column, &to_line, &to_column));
	CHECK(line == 1 && column == 5);

	/* the next one, from the end of the text: it wraps */
	Rtext_editor_set_cursor(g_editor, 2, 0);
	CHECK(Rtext_editor_find_next(g_editor, R_FALSE));
	CHECK(Rtext_editor_get_selection(g_editor, &line, &column, &to_line, &to_column));
	CHECK(line == 0 && column == 6);
	/* and backwards, from a caret after the second one of the second line */
	Rtext_editor_set_cursor(g_editor, 1, 10);
	CHECK(Rtext_editor_find_next(g_editor, R_TRUE));
	CHECK(Rtext_editor_get_selection(g_editor, &line, &column, &to_line, &to_column));
	CHECK(line == 1 && column == 0);

	/* case, when it is ignored */
	CHECK(Rtext_editor_set_search(g_editor, "gamma", R_SEARCH_IGNORE_CASE));
	CHECK(Rtext_editor_count_matches(g_editor) == 2);
	/* whole words */
	CHECK(Rtext_editor_set_search(g_editor, "beta", R_SEARCH_WHOLE_WORD));
	CHECK(Rtext_editor_count_matches(g_editor) == 3);
	CHECK(Rtext_editor_set_search(g_editor, "bet", R_SEARCH_WHOLE_WORD));
	CHECK(Rtext_editor_count_matches(g_editor) == 0);

	/* regular expressions, with a group */
	CHECK(Rtext_editor_set_search(g_editor, "(b[a-z]+)", R_SEARCH_REGEX));
	CHECK(Rtext_editor_count_matches(g_editor) == 3);
	{
		int groups[4] = { 0, 4, 0, 4 };
		CHECK_STR(Rtext_editor_expand_replacement("<$1> $& $$ \\n", "beta", 0, 4, groups, 2), "<beta> beta $ \n");
	}
	/* a pattern that is not valid leaves the search as it was */
	EXPECT_ERRORS_BEGIN();
	CHECK(!Rtext_editor_set_search(g_editor, "(", R_SEARCH_REGEX));
	EXPECT_ERRORS_END(1);
	CHECK_STR(Rtext_editor_get_search(g_editor), "(b[a-z]+)");

	/* replace: the selection, then the next one */
	CHECK(Rtext_editor_set_search(g_editor, "beta", R_SEARCH_NONE));
	Rtext_editor_set_cursor(g_editor, 0, 0);
	Rtext_editor_find_next(g_editor, R_FALSE);
	CHECK(Rtext_editor_replace(g_editor, "B"));
	CHECK_STR(Rtext_editor_get_line(g_editor, 0), "alpha B gamma");
	/* replace all */
	Rtext_editor_set_text(g_editor, "x x x");
	CHECK(Rtext_editor_set_search(g_editor, "x", R_SEARCH_NONE));
	CHECK(Rtext_editor_replace_all(g_editor, "yy") == 3);
	CHECK_STR(Rtext_editor_get_text(g_editor), "yy yy yy");
	CHECK(Rtext_editor_can_undo(g_editor));
	Rwindow_destroy(g_window);
}

/* ---------------------------------------------------------------------------
 *  Folding
 * ------------------------------------------------------------------------- */

static void test_folding(void) {
	int before, after;
	open_editor("LaTeX");
	Rtext_editor_set_text(g_editor,
		"\\begin{document}\n"
		"\\begin{itemize}\n"
		"\\item one\n"
		"\\item two\n"
		"\\end{itemize}\n"
		"\\end{document}\n");
	Rdraw_frame();
	CHECK(Rtext_editor_get_folding(g_editor));
	CHECK(Rtext_editor_fold_line(g_editor, 0));
	CHECK(Rtext_editor_is_folded(g_editor, 0));
	CHECK(Rtext_editor_get_folded_count(g_editor) == 1);
	/* (a line inside the folded block is inside it) */
	CHECK(Rtext_editor_get_fold_start(g_editor, 2) == 0);
	CHECK(Rtext_editor_get_fold_start(g_editor, 5) == -1);
	before = Rtext_editor_get_line_count(g_editor);
	Rdraw_frame();
	after = Rtext_editor_get_line_count(g_editor);
	/* (folding does not change the text) */
	CHECK(before == after && before == 7);

	/* the caret on a block folds it, and unfolds it */
	Rtext_editor_set_cursor(g_editor, 1, 2);
	CHECK(Rtext_editor_toggle_fold(g_editor));
	CHECK(Rtext_editor_is_folded(g_editor, 1));
	CHECK(Rtext_editor_toggle_fold(g_editor));
	CHECK(!Rtext_editor_is_folded(g_editor, 1));
	/* (a line inside a block folds the block that holds it, the innermost) */
	Rtext_editor_set_cursor(g_editor, 2, 0);
	CHECK(Rtext_editor_toggle_fold(g_editor));
	CHECK(Rtext_editor_is_folded(g_editor, 1));
	Rtext_editor_toggle_fold(g_editor);
	/* (a document with no block) */
	Rtext_editor_set_text(g_editor, "one\ntwo");
	Rtext_editor_set_cursor(g_editor, 1, 0);
	CHECK(!Rtext_editor_toggle_fold(g_editor));
	Rtext_editor_set_text(g_editor,
		"\\begin{document}\n"
		"\\begin{itemize}\n"
		"\\item one\n"
		"\\item two\n"
		"\\end{itemize}\n"
		"\\end{document}\n");
	/* unfold one, then all */
	Rtext_editor_fold_line(g_editor, 0);
	Rtext_editor_fold_line(g_editor, 1);
	CHECK(Rtext_editor_get_folded_count(g_editor) == 2);
	Rtext_editor_unfold_line(g_editor, 0);
	CHECK(Rtext_editor_get_folded_count(g_editor) == 1);
	Rtext_editor_unfold_all(g_editor);
	CHECK(Rtext_editor_get_folded_count(g_editor) == 0);
	/* the key */
	Rtext_editor_set_cursor(g_editor, 0, 3);
	key(R_KEY_LEFT_BRACKET, R_MOD_CTRL | R_MOD_SHIFT);
	CHECK(Rtext_editor_is_folded(g_editor, 0));
	key(R_KEY_RIGHT_BRACKET, R_MOD_CTRL | R_MOD_SHIFT);
	CHECK(!Rtext_editor_is_folded(g_editor, 0));
	/* (with folding off, nothing is folded) */
	Rtext_editor_fold_line(g_editor, 0);
	Rtext_editor_set_folding(g_editor, R_FALSE);
	CHECK(!Rtext_editor_get_folding(g_editor));
	CHECK(Rtext_editor_get_folded_count(g_editor) == 0);
	Rwindow_destroy(g_window);
}

/* ---------------------------------------------------------------------------
 *  Snippets
 * ------------------------------------------------------------------------- */

static void test_snippets(void) {
	RSnippet section;
	open_editor("LaTeX");
	Rsnippet_clear();
	memset(&section, 0, sizeof(section));
	section.name = "\\section";
	section.description = "Section";
	section.body = "\\section{${1:title}}\n$0";
	CHECK(Rsnippet_add(&section));
	CHECK(Rsnippet_get_count() == 1);
	CHECK_STR(Rsnippet_get_name(0), "\\section");
	CHECK(Rsnippet_find("\\SECTION") == 0);
	CHECK(Rsnippet_find("nope") == -1);

	/* typing its name and choosing it makes the snippet, with its place selected */
	Rtext_editor_set_text(g_editor, "");
	Rtext_editor_set_cursor(g_editor, 0, 0);
	type("\\section");
	CHECK(Rtext_editor_snippet_at_caret(g_editor) == 0);
	CHECK(Rtext_editor_insert_snippet(g_editor, "\\section"));
	/* (the $0 of the body is on the line under the title) */
	CHECK_STR(Rtext_editor_get_text(g_editor), "\\section{title}\n");
	CHECK(Rtext_editor_is_in_snippet(g_editor));
	CHECK_STR(Rtext_editor_get_selected_text(g_editor), "title");
	/* Tab goes to the next place, at the end (the list of proposals is not in the way) */
	Rtext_editor_set_completion(g_editor, R_FALSE);
	key(R_KEY_TAB, 0);
	CHECK(!Rtext_editor_is_in_snippet(g_editor));
	/* the name typed is replaced by the body */
	Rtext_editor_set_text(g_editor, "fig");
	Rtext_editor_set_cursor(g_editor, 0, 3);
	{
		RSnippet figure;
		memset(&figure, 0, sizeof(figure));
		figure.name = "fig";
		figure.body = "\\begin{figure}\n\\centering\n\\includegraphics[width=\\linewidth]{$1}\n\\caption{$2}\n\\end{figure}";
		CHECK(Rsnippet_add(&figure));
	}
	CHECK(Rtext_editor_insert_snippet(g_editor, "fig"));
	CHECK(strstr(Rtext_editor_get_text(g_editor), "\\begin{figure}") != NULL);

	/* an unknown snippet is reported, and nothing changes */
	EXPECT_ERRORS_BEGIN();
	CHECK(!Rtext_editor_insert_snippet(g_editor, "nope"));
	EXPECT_ERRORS_END(0);
	Rsnippet_clear();
	CHECK(Rsnippet_get_count() == 0);
	Rwindow_destroy(g_window);
}

/* ---------------------------------------------------------------------------
 *  Several cursors
 * ------------------------------------------------------------------------- */

static void test_carets(void) {
	int line = -1, column = -1;
	open_editor("LaTeX");
	Rtext_editor_set_text(g_editor, "one two one");
	CHECK(!Rtext_editor_get_multi_cursor(g_editor));
	CHECK(Rtext_editor_get_caret_count(g_editor) == 1);
	CHECK(!Rtext_editor_add_caret(g_editor, 0, 8));
	Rtext_editor_set_multi_cursor(g_editor, R_TRUE);
	CHECK(Rtext_editor_get_multi_cursor(g_editor));
	CHECK(Rtext_editor_add_caret(g_editor, 0, 8));
	CHECK(Rtext_editor_get_caret_count(g_editor) == 2);
	CHECK(Rtext_editor_get_caret(g_editor, 1, &line, &column));
	CHECK(line == 0 && column == 8);
	CHECK(Rtext_editor_get_caret(g_editor, 2, &line, &column) == R_FALSE);
	Rtext_editor_clear_carets(g_editor);
	CHECK(Rtext_editor_get_caret_count(g_editor) == 1);
	Rwindow_destroy(g_window);
}

/* ---------------------------------------------------------------------------
 *  Markers, read back
 * ------------------------------------------------------------------------- */

static void test_markers_read(void) {
	int line = 0, from = 0, to = 0;
	RMarkerKind kind = R_MARKER_INFO;
	open_editor("LaTeX");
	Rtext_editor_set_text(g_editor, "one\ntwo\nthree");
	Rtext_editor_add_marker(g_editor, 1, 0, 3, R_MARKER_ERROR, "undefined control sequence");
	Rtext_editor_add_marker(g_editor, 2, -1, -1, R_MARKER_WARNING, NULL);
	CHECK(Rtext_editor_get_marker_count(g_editor) == 2);
	CHECK_STR(Rtext_editor_get_marker(g_editor, 0, &line, &from, &to, &kind), "undefined control sequence");
	CHECK(line == 1 && from == 0 && to == 3 && kind == R_MARKER_ERROR);
	CHECK(Rtext_editor_get_marker(g_editor, -1, NULL, NULL, NULL, NULL) == NULL);
	Rtext_editor_clear_markers_of_line(g_editor, 1);
	CHECK(Rtext_editor_get_marker_count(g_editor) == 1);
	Rtext_editor_clear_markers(g_editor);
	CHECK(Rtext_editor_get_marker_count(g_editor) == 0);
	Rwindow_destroy(g_window);
}

/* ---------------------------------------------------------------------------
 *  What the language does while typing
 * ------------------------------------------------------------------------- */

static void test_tex_typing(void) {
	open_editor("LaTeX");
	Rtext_editor_set_text(g_editor, "");
	Rtext_editor_set_cursor(g_editor, 0, 0);
	type("\\begin{itemize}");
	/* (the closing brace brings \end on a line of its own, and the caret inside) */
	CHECK_STR(Rtext_editor_get_text(g_editor), "\\begin{itemize}\n\t\n\\end{itemize}");
	CHECK(cursor_line() == 1);
	/* and the comment key comments a line with a % */
	Rtext_editor_set_text(g_editor, "one\ntwo");
	Rtext_editor_select_all(g_editor);
	key(R_KEY_SLASH, R_MOD_CTRL);
	CHECK_STR(Rtext_editor_get_line(g_editor, 0), "% one");
	CHECK_STR(Rtext_editor_get_line(g_editor, 1), "% two");
	/* (in TeX, a new line after \begin is indented too) */
	Rtext_editor_set_text(g_editor, "\\begin{center}");
	Rtext_editor_set_cursor(g_editor, 0, 14);
	key(R_KEY_ENTER, 0);
	type("x");
	CHECK_STR(Rtext_editor_get_line(g_editor, 1), "\tx");
	Rwindow_destroy(g_window);
}

int main(void) {
	if (!test_init(2)) return 1;
	RUN(test_latex_language);
	RUN(test_bibtex_language);
	RUN(test_counts);
	RUN(test_search);
	RUN(test_folding);
	RUN(test_snippets);
	RUN(test_carets);
	RUN(test_markers_read);
	RUN(test_tex_typing);
	Rshutdown();
	return test_report("test_latex_editor");
}
