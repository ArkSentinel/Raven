/**
 * Raven - widgets/text_editor.c
 * TextEditor: multi-line text editing.
 *
 * The text is an array of lines, each one a small UTF-8 buffer: inserting or
 * deleting touches one line (and moves pointers when lines are added), which
 * stays fast for texts of tens of thousands of lines.
 *
 * A position is (line, column), the column being a byte offset in the line.
 * With word wrap a line is shown on several rows; the places where it is cut
 * are computed once per line and per width, and kept until the line changes.
 * Only the rows that are visible are measured, colored and painted.
 *
 * Every change goes through one function, rvn_ed_replace, which also records
 * what is needed to undo it.
 */

#include "common.h"
#include "regex.h"

#define RVN_ED_PAD_X 8.0f
#define RVN_ED_PAD_Y 6.0f
#define RVN_ED_MAX_SPANS 192
/** Characters typed in a row are undone together, up to this many. */
#define RVN_ED_TYPING_GROUP 48
#define RVN_ED_MAX_UNDO 512

typedef struct rvn_ed_pos_t {
	int line;
	int column;
} rvn_ed_pos_t;

typedef struct rvn_ed_line_t {
	char* text;
	int length;
	int cap;
	/** Width of the line on one row, or a negative value when it is not known. */
	float width;
	/** Word wrap: number of rows (0 = not computed) and where the rows after the first begin. */
	int row_count;
	int* breaks;
	/** What the lexer is in at the start of the line (syntax colors). */
	uint8_t lex_in;
} rvn_ed_line_t;

/** A part of a line underlined with a message (Rtext_editor_add_marker). */
typedef struct rvn_ed_marker_t {
	int line;
	int from;
	/** Negative: to the end of the line. */
	int to;
	RMarkerKind kind;
	char* message;
	/** Who added it (0 by default): the application's own sources of markers are told apart by it. */
	int source;
} rvn_ed_marker_t;

/** A block that can be folded: its first line, its last line, and whether it is folded now. */
typedef struct rvn_ed_fold_t {
	int start;
	int end;
	int folded;
} rvn_ed_fold_t;

/** A match of the search, as two positions. */
typedef struct rvn_ed_match_t {
	rvn_ed_pos_t from;
	rvn_ed_pos_t to;
} rvn_ed_match_t;

/** A caret of a selection, besides the main one (Rtext_editor_add_caret). */
typedef struct rvn_ed_caret_t {
	rvn_ed_pos_t caret;
	rvn_ed_pos_t anchor;
} rvn_ed_caret_t;

/** A place of a snippet that the caret walks through ($1, $2...). */
typedef struct rvn_ed_place_t {
	rvn_ed_pos_t at;
	/** Bytes of the text that was put there, selected while the caret is on it. */
	int length;
} rvn_ed_place_t;

/** A snippet, as Rsnippet_add gave it. */
typedef struct rvn_ed_snippet_t {
	char* name;
	char* description;
	char* prefix;
	char* body;
} rvn_ed_snippet_t;

/** A variable that a snippet body may name with `$NAME`. */
typedef struct rvn_ed_variable_t {
	char* name;
	char* value;
} rvn_ed_variable_t;

/** One step of the history: `removed` was replaced by `inserted` at `start`. */
typedef struct rvn_ed_edit_t {
	rvn_ed_pos_t start;
	char* removed;
	int removed_length;
	char* inserted;
	int inserted_length;
	rvn_ed_pos_t caret_before, anchor_before, caret_after;
	/** A run of typed characters that the next one may join. */
	int typing;
	/**
	 * The change this edit belongs to, when several edits make one step of
	 * the history (typing at every caret, Replace All): 0 is a step of its own.
	 */
	uint32_t group;
} rvn_ed_edit_t;

typedef struct rvn_text_editor_t {
	rvn_widget_t base;
	rvn_ed_line_t* lines;
	uint32_t line_count;
	uint32_t line_cap;

	/** The selection is the range between the two; the caret is where typing goes. */
	rvn_ed_pos_t caret;
	rvn_ed_pos_t anchor;
	/** Horizontal position kept while moving up and down; negative = take it from the caret. */
	float goal_x;

	int read_only;
	int wrap;
	int line_numbers;
	int tab_size;
	int tab_spaces;
	/** Name of the language, or NULL; what it was found to be, and when it was looked for. */
	char* language_name;
	const rvn_language_t* language;
	uint32_t language_generation;
	int no_pairs;
	int no_completion;
	/** Counts the changes of the text: what was computed for a version is forgotten with it. */
	uint32_t version;
	/** The version the application last saved or loaded: the text is modified when they differ. */
	uint32_t saved_version;
	/** The bracket next to the caret and the one that closes it, found for (match_version, match_caret). */
	uint32_t match_version;
	rvn_ed_pos_t match_caret;
	int match_found;
	rvn_ed_pos_t match_a, match_b;
	/** Parts of the text underlined with a message. */
	rvn_ed_marker_t* markers;
	uint32_t marker_count;
	uint32_t marker_cap;
	/** The tooltip of the widget is the message of a marker. */
	int marker_tip;
	/** Folded blocks, by the line they start on. */
	rvn_ed_fold_t* folds;
	uint32_t fold_count;
	uint32_t fold_cap;
	int no_folding;
	/** The version of the text the folds were found in. */
	uint32_t fold_version;
	/** Rows hidden by the folds, and the scrollbar as if they were not there. */
	uint32_t hidden_rows;
	int32_t* hidden_before;
	uint32_t hidden_cap;
	/** The search: what is looked for, and where it matches (computed again when the text or the search changes). */
	char* search_pattern;
	Ruint search_flags;
	rvn_ed_match_t* matches;
	uint32_t match_count;
	uint32_t match_cap;
	uint32_t search_version;
	Ruint match_flags;
	char* match_pattern;
	/** A snippet whose places the caret is walking through. */
	rvn_ed_place_t* places;
	uint32_t place_count;
	uint32_t place_cap;
	int place_at;
	/** Where the caret ends after the places, when the snippet has a $0. */
	rvn_ed_pos_t place_end;
	int has_place_end;
	/** Carets besides the main one, and whether there may be some. */
	rvn_ed_caret_t* carets;
	uint32_t caret_count;
	uint32_t caret_cap;
	int multi_cursor;
	/** Alt is held while the pointer drags: a caret on every line it crosses. */
	int column_drag;
	int no_snippets;
	/** The list of proposals (a popup of the window), created on first use. */
	rvn_widget_t* complete;
	RCompletionCallback completion_callback;
	void* completion_user;
	Rid font;
	float font_size;
	/** Zoom of the text, a factor of the font size (0: 1). Ctrl + wheel, Ctrl + '+' / '-' / 0. */
	float zoom;
	int no_zoom;
	RTextEditorZoomCallback zoom_callback;
	void* zoom_user;
	char* placeholder;

	rvn_scrollbar_t vbar;
	rvn_scrollbar_t hbar;
	/** Word wrap: width the rows were cut for, and the first row of each line. */
	float wrap_width;
	uint32_t* row_starts;
	uint32_t row_starts_cap;
	uint32_t total_rows;
	int rows_dirty;
	/** Without word wrap: width of the longest line. */
	float max_width;
	/** Lines whose lex_in is up to date (always a prefix of the text). */
	uint32_t lex_valid;
	/** The caret must be scrolled into view at the next layout. */
	int reveal_pending;

	rvn_ed_edit_t* undo;
	uint32_t undo_count;
	uint32_t undo_cap;
	rvn_ed_edit_t* redo;
	uint32_t redo_count;
	uint32_t redo_cap;
	/** The group the next recorded edits belong to (0: one step of its own). */
	uint32_t edit_group;
	/** How many rvn_ed_group_begin calls are open. */
	int edit_group_depth;
	/** Numbers the groups, so that a group is never 0. */
	uint32_t edit_group_serial;

	int selecting;
	Rid blink_timer;
	double blink_start;
	/** Where the caret is drawn, in the coordinates of the text: it glides to its place. */
	RSpring caret_x;
	RSpring caret_y;

	/** Results of Rtext_editor_get_text / get_selected_text. */
	char* text_cache;
	int text_cache_length;
	int text_cache_valid;
	char* selection_cache;

	RTextEditorChangedCallback callback;
	void* user_data;
} rvn_text_editor_t;

/** What is needed to measure and place text, computed once per function. */
typedef struct rvn_ed_metrics_t {
	rvn_font_t* font;
	Rid font_id;
	float size;
	float line_height;
	float space;
	float tab;
	/** Width of the margin of the line numbers, 0 without it. */
	float gutter;
	/** Where the text is drawn, in the widget. */
	rvn_rect_t area;
	float bar;
	/** Theme of the editor: the colors of the code come from it. */
	const rvn_theme_t* theme;
	/** The bold and the italic of the font, for formatted text; the font itself otherwise. */
	Rid bold_id;
	Rid italic_id;
} rvn_ed_metrics_t;

/* What the editor computes for its rows, its folds and its search (in the order the file defines them). */
static void rvn_ed_update_rows(rvn_text_editor_t* ed, const rvn_ed_metrics_t* m);
static void rvn_ed_folds_refresh(rvn_text_editor_t* ed);
static int rvn_ed_fold_line_at(const rvn_text_editor_t* ed, int line);
static void rvn_ed_update_matches(rvn_text_editor_t* ed);
static void rvn_ed_snippet_leave(rvn_text_editor_t* ed);
static int rvn_ed_is_word_char(char c);
static void rvn_ed_relayout(rvn_text_editor_t* ed);

/* ---------------------------------------------------------------------------
 *  Positions
 * ------------------------------------------------------------------------- */

static rvn_ed_pos_t rvn_ed_pos(int line, int column) {
	rvn_ed_pos_t pos;
	pos.line = line;
	pos.column = column;
	return pos;
}

static int rvn_ed_pos_cmp(rvn_ed_pos_t a, rvn_ed_pos_t b) {
	if (a.line != b.line) return a.line < b.line ? -1 : 1;
	if (a.column != b.column) return a.column < b.column ? -1 : 1;
	return 0;
}

static rvn_ed_pos_t rvn_ed_clamp(const rvn_text_editor_t* ed, rvn_ed_pos_t pos) {
	pos.line = rvn_clampi(pos.line, 0, (int)ed->line_count - 1);
	pos.column = rvn_clampi(pos.column, 0, ed->lines[pos.line].length);
	/* never inside a character */
	while (pos.column > 0 && pos.column < ed->lines[pos.line].length &&
		((unsigned char)ed->lines[pos.line].text[pos.column] & 0xC0) == 0x80) pos.column--;
	return pos;
}

static rvn_ed_pos_t rvn_ed_sel_min(const rvn_text_editor_t* ed) {
	return rvn_ed_pos_cmp(ed->caret, ed->anchor) <= 0 ? ed->caret : ed->anchor;
}

static rvn_ed_pos_t rvn_ed_sel_max(const rvn_text_editor_t* ed) {
	return rvn_ed_pos_cmp(ed->caret, ed->anchor) <= 0 ? ed->anchor : ed->caret;
}

static int rvn_ed_has_selection(const rvn_text_editor_t* ed) {
	return rvn_ed_pos_cmp(ed->caret, ed->anchor) != 0;
}

static rvn_ed_pos_t rvn_ed_end(const rvn_text_editor_t* ed) {
	return rvn_ed_pos((int)ed->line_count - 1, ed->lines[ed->line_count - 1].length);
}

/* Position reached after inserting `text` at `start` (carriage returns before a line break are dropped, as rvn_ed_replace does). */
static rvn_ed_pos_t rvn_ed_pos_after(rvn_ed_pos_t start, const char* text, int length) {
	int i, begin = 0;
	for (i = 0; i <= length; i++) {
		if (i == length || text[i] == '\n') {
			int piece = i - begin;
			while (piece > 0 && text[begin + piece - 1] == '\r') piece--;
			start.column += piece;
			if (i < length) {
				start.line++;
				start.column = 0;
			}
			begin = i + 1;
		}
	}
	return start;
}

/* ---------------------------------------------------------------------------
 *  Lines
 * ------------------------------------------------------------------------- */

static void rvn_ed_line_dirty(rvn_ed_line_t* line) {
	line->width = -1.0f;
	line->row_count = 0;
}

static void rvn_ed_line_reserve(rvn_ed_line_t* line, int length) {
	if (length + 1 > line->cap) {
		int cap = line->cap ? line->cap : 16;
		while (cap < length + 1) cap *= 2;
		line->text = (char*)rvn_realloc(line->text, (size_t)cap);
		line->cap = cap;
	}
}

static void rvn_ed_line_set(rvn_ed_line_t* line, const char* text, int length) {
	rvn_ed_line_reserve(line, length);
	if (length > 0) memcpy(line->text, text, (size_t)length);
	line->text[length] = '\0';
	line->length = length;
	rvn_ed_line_dirty(line);
}

static void rvn_ed_line_free(rvn_ed_line_t* line) {
	rvn_free(line->text);
	rvn_free(line->breaks);
}

/* Opens `count` empty lines at `index`. */
static void rvn_ed_lines_insert(rvn_text_editor_t* ed, uint32_t index, uint32_t count) {
	uint32_t i;
	if (count == 0) return;
	RVN_ARRAY_RESERVE(ed->lines, ed->line_cap, ed->line_count + count);
	memmove(ed->lines + index + count, ed->lines + index, sizeof(rvn_ed_line_t) * (ed->line_count - index));
	memset(ed->lines + index, 0, sizeof(rvn_ed_line_t) * count);
	for (i = 0; i < count; i++) rvn_ed_line_set(&ed->lines[index + i], "", 0);
	ed->line_count += count;
}

static void rvn_ed_lines_remove(rvn_text_editor_t* ed, uint32_t index, uint32_t count) {
	uint32_t i;
	if (count == 0) return;
	for (i = 0; i < count; i++) rvn_ed_line_free(&ed->lines[index + i]);
	memmove(ed->lines + index, ed->lines + index + count, sizeof(rvn_ed_line_t) * (ed->line_count - index - count));
	ed->line_count -= count;
}

/* The text between two ordered positions, in a new block. */
static char* rvn_ed_range(const rvn_text_editor_t* ed, rvn_ed_pos_t from, rvn_ed_pos_t to, int* length_out) {
	size_t length = 0;
	char* out;
	char* write;
	int line;

	for (line = from.line; line <= to.line; line++) {
		int begin = line == from.line ? from.column : 0;
		int end = line == to.line ? to.column : ed->lines[line].length;
		length += (size_t)(end - begin) + (line < to.line ? 1u : 0u);
	}
	out = (char*)rvn_alloc(length + 1);
	write = out;
	for (line = from.line; line <= to.line; line++) {
		int begin = line == from.line ? from.column : 0;
		int end = line == to.line ? to.column : ed->lines[line].length;
		memcpy(write, ed->lines[line].text + begin, (size_t)(end - begin));
		write += end - begin;
		if (line < to.line) *write++ = '\n';
	}
	*write = '\0';
	if (length_out) *length_out = (int)length;
	return out;
}

/* ---------------------------------------------------------------------------
 *  History
 * ------------------------------------------------------------------------- */

static void rvn_ed_edit_free(rvn_ed_edit_t* edit) {
	rvn_free(edit->removed);
	rvn_free(edit->inserted);
}

static void rvn_ed_history_clear(rvn_ed_edit_t* edits, uint32_t* count) {
	uint32_t i;
	for (i = 0; i < *count; i++) rvn_ed_edit_free(&edits[i]);
	*count = 0;
}

static void rvn_ed_changed_state(rvn_text_editor_t* ed, int first_line) {
	ed->rows_dirty = R_TRUE;
	ed->text_cache_valid = R_FALSE;
	ed->version++;
	if ((uint32_t)first_line + 1 < ed->lex_valid) ed->lex_valid = (uint32_t)first_line + 1;
	rvn_widget_invalidate_paint(&ed->base);
}

/* Drops the oldest steps, whole groups at a time, until the history fits in RVN_ED_MAX_UNDO. */
static void rvn_ed_history_trim(rvn_text_editor_t* ed) {
	while (ed->undo_count > RVN_ED_MAX_UNDO) {
		uint32_t oldest = ed->undo[0].group, drop = 1;
		/* (the edits of a group go together: no undo is left half a change) */
		while (drop < ed->undo_count && oldest && ed->undo[drop].group == oldest) drop++;
		/* (the only group there is stays, however long it is: losing it would lose the change) */
		if (drop >= ed->undo_count) break;
		{
			uint32_t i;
			for (i = 0; i < drop; i++) rvn_ed_edit_free(&ed->undo[i]);
			memmove(ed->undo, ed->undo + drop, sizeof(rvn_ed_edit_t) * (size_t)(ed->undo_count - drop));
			ed->undo_count -= drop;
		}
	}
}

/*
 * The one function that changes the text: replaces what lies between two
 * positions by `text` (either may be empty) and returns the position after
 * the inserted text. `record` stores the step in the history.
 */
static rvn_ed_pos_t rvn_ed_replace(rvn_text_editor_t* ed, rvn_ed_pos_t from, rvn_ed_pos_t to,
	const char* text, int length, int record) {

	rvn_ed_pos_t end;
	rvn_ed_line_t* line;
	char* tail;
	int tail_length, pieces = 1, i, begin, index;

	if (rvn_ed_pos_cmp(from, to) > 0) {
		rvn_ed_pos_t swap = from;
		from = to;
		to = swap;
	}
	if (length < 0) length = (int)strlen(text);

	if (record) {
		rvn_ed_edit_t* last = ed->undo_count ? &ed->undo[ed->undo_count - 1] : NULL;
		int typed = length > 0 && rvn_ed_pos_cmp(from, to) == 0 && !memchr(text, '\n', (size_t)length) &&
			rvn_utf8_count(text, length) == 1;
		/* (edits of the same group are one step: what was undone or redone is gone for good only when a new change starts) */
		if (!ed->edit_group || !last || last->group != ed->edit_group) rvn_ed_history_clear(ed->redo, &ed->redo_count);
		if (typed && last && last->typing && last->removed_length == 0 && last->inserted_length < RVN_ED_TYPING_GROUP &&
			last->group == ed->edit_group && rvn_ed_pos_cmp(last->caret_after, from) == 0) {
			/* one more character of a run that is being typed: same step */
			last->inserted = (char*)rvn_realloc(last->inserted, (size_t)(last->inserted_length + length) + 1);
			memcpy(last->inserted + last->inserted_length, text, (size_t)length);
			last->inserted_length += length;
			last->inserted[last->inserted_length] = '\0';
			last->caret_after = rvn_ed_pos(from.line, from.column + length);
		} else {
			rvn_ed_edit_t* edit;
			/* (at the limit the oldest steps go, whole groups at a time) */
			rvn_ed_history_trim(ed);
			RVN_ARRAY_RESERVE(ed->undo, ed->undo_cap, ed->undo_count + 1);
			edit = &ed->undo[ed->undo_count++];
			memset(edit, 0, sizeof(*edit));
			edit->start = from;
			edit->removed = rvn_ed_range(ed, from, to, &edit->removed_length);
			edit->inserted = rvn_strndup(text ? text : "", (size_t)length);
			edit->inserted_length = length;
			edit->caret_before = ed->caret;
			edit->anchor_before = ed->anchor;
			edit->caret_after = rvn_ed_pos_after(from, text, length);
			edit->typing = typed;
			edit->group = ed->edit_group;
		}
	}

	for (i = 0; i < length; i++) {
		if (text[i] == '\n') pieces++;
	}
	/* the markers below follow their lines; those of the lines that change are out of date */
	{
		int delta = (pieces - 1) - (to.line - from.line);
		uint32_t m = 0;
		while (m < ed->marker_count) {
			rvn_ed_marker_t* marker = &ed->markers[m];
			if (marker->line > to.line) {
				marker->line += delta;
				m++;
			} else if (marker->line >= from.line) {
				rvn_free(marker->message);
				*marker = ed->markers[--ed->marker_count];
			} else {
				m++;
			}
		}
	}

	/* what follows the range on its last line is kept aside, and put back after the new text */
	tail_length = ed->lines[to.line].length - to.column;
	tail = rvn_strndup(ed->lines[to.line].text + to.column, (size_t)tail_length);
	rvn_ed_lines_remove(ed, (uint32_t)from.line + 1, (uint32_t)(to.line - from.line));
	line = &ed->lines[from.line];
	line->length = from.column;
	line->text[line->length] = '\0';

	rvn_ed_lines_insert(ed, (uint32_t)from.line + 1, (uint32_t)(pieces - 1));

	/* the pieces between line breaks, without carriage returns */
	begin = 0;
	index = from.line;
	end = from;
	for (i = 0; i <= length; i++) {
		if (i == length || text[i] == '\n') {
			int piece = i - begin;
			line = &ed->lines[index];
			while (piece > 0 && text[begin + piece - 1] == '\r') piece--;
			rvn_ed_line_reserve(line, line->length + piece + (i == length ? tail_length : 0));
			if (piece > 0) memcpy(line->text + line->length, text + begin, (size_t)piece);
			line->length += piece;
			line->text[line->length] = '\0';
			rvn_ed_line_dirty(line);
			end = rvn_ed_pos(index, line->length);
			begin = i + 1;
			index++;
		}
	}
	line = &ed->lines[end.line];
	if (tail_length > 0) memcpy(line->text + line->length, tail, (size_t)tail_length);
	line->length += tail_length;
	line->text[line->length] = '\0';
	rvn_free(tail);

	rvn_ed_changed_state(ed, from.line);
	return end;
}

/* Tells the application that the user changed the text. */
static void rvn_ed_notify(rvn_widget_t* widget) {	rvn_text_editor_t* ed = (rvn_text_editor_t*)widget;
	if (ed->callback) ed->callback(widget->id, ed->user_data);
	if (rvn_widget_alive(widget)) rvn_widget_fire_action(widget);
}

static void rvn_ed_break_typing(rvn_text_editor_t* ed) {
	if (ed->undo_count) ed->undo[ed->undo_count - 1].typing = R_FALSE;
}

/*
 * Several edits as one step of the history: what is recorded between these
 * two calls is undone and redone together (typing at every caret, Replace
 * All). The calls nest; the outermost one makes the step.
 */
static void rvn_ed_group_begin(rvn_text_editor_t* ed) {
	if (ed->edit_group_depth++) return;
	if (++ed->edit_group_serial == 0) ed->edit_group_serial = 1;
	ed->edit_group = ed->edit_group_serial;
	rvn_ed_break_typing(ed);
}

static void rvn_ed_group_end(rvn_text_editor_t* ed) {
	if (ed->edit_group_depth == 0 || --ed->edit_group_depth > 0) return;
	ed->edit_group = 0;
	rvn_ed_break_typing(ed);
}

static void rvn_ed_move_edit(rvn_ed_edit_t** to, uint32_t* to_count, uint32_t* to_cap, rvn_ed_edit_t* from, uint32_t* from_count) {
	RVN_ARRAY_RESERVE(*to, *to_cap, *to_count + 1);
	(*to)[(*to_count)++] = from[--(*from_count)];
}

/* Takes the edits of the last group off `from` and returns their count (0: one edit). */
static uint32_t rvn_ed_group_take(const rvn_ed_edit_t* edits, uint32_t count) {
	uint32_t group, taken = 1;
	if (count == 0) return 0;
	group = edits[count - 1].group;
	if (!group) return 1;
	while (taken < count && edits[count - 1 - taken].group == group) taken++;
	return taken;
}

static int rvn_ed_undo(rvn_text_editor_t* ed) {
	rvn_ed_pos_t caret, anchor;
	uint32_t taken, k;
	if (ed->undo_count == 0 || ed->read_only) return R_FALSE;
	taken = rvn_ed_group_take(ed->undo, ed->undo_count);
	caret = ed->undo[ed->undo_count - taken].caret_before;
	anchor = ed->undo[ed->undo_count - taken].anchor_before;
	/* (the edits of a group were made from the end of the text to its start: undoing them goes the other way) */
	for (k = 0; k < taken; k++) {
		rvn_ed_edit_t* edit = &ed->undo[ed->undo_count - 1 - k];
		edit->typing = R_FALSE;
		rvn_ed_replace(ed, edit->start, rvn_ed_pos_after(edit->start, edit->inserted, edit->inserted_length),
			edit->removed, edit->removed_length, R_FALSE);
	}
	while (taken-- > 0) rvn_ed_move_edit(&ed->redo, &ed->redo_count, &ed->redo_cap, ed->undo, &ed->undo_count);
	ed->caret = rvn_ed_clamp(ed, caret);
	ed->anchor = rvn_ed_clamp(ed, anchor);
	return R_TRUE;
}

static int rvn_ed_redo(rvn_text_editor_t* ed) {
	rvn_ed_pos_t caret;
	uint32_t taken, k;
	if (ed->redo_count == 0 || ed->read_only) return R_FALSE;
	taken = rvn_ed_group_take(ed->redo, ed->redo_count);
	/* (the undo moved the edits the other way round: the last one of the array was the first that was made) */
	caret = ed->redo[ed->redo_count - taken].caret_after;
	for (k = 0; k < taken; k++) {
		rvn_ed_edit_t* edit = &ed->redo[ed->redo_count - 1 - k];
		rvn_ed_replace(ed, edit->start, rvn_ed_pos_after(edit->start, edit->removed, edit->removed_length),
			edit->inserted, edit->inserted_length, R_FALSE);
	}
	while (taken-- > 0) rvn_ed_move_edit(&ed->undo, &ed->undo_count, &ed->undo_cap, ed->redo, &ed->redo_count);
	ed->caret = ed->anchor = rvn_ed_clamp(ed, caret);
	return R_TRUE;
}

/* ---------------------------------------------------------------------------
 *  Metrics and horizontal positions
 * ------------------------------------------------------------------------- */

static Rid rvn_ed_font_id(const rvn_text_editor_t* ed) {
	if (ed->font != R_INVALID_ID && Rfont_is_valid(ed->font)) return ed->font;
	/* text that has a language is code: it lines up better with a fixed width */
	return ed->language_name ? g_rvn.mono_font : R_INVALID_ID;
}

/* The language of the editor, looked for again when the languages changed. */
static const rvn_language_t* rvn_ed_language(rvn_text_editor_t* ed) {
	if (ed->language_generation != rvn_language_generation()) {
		ed->language = ed->language_name ? rvn_language_find(ed->language_name) : NULL;
		ed->language_generation = rvn_language_generation();
		ed->lex_valid = 0;
	}
	return ed->language;
}

static const RLanguageDesc* rvn_ed_language_desc(rvn_text_editor_t* ed) {
	const rvn_language_t* language = rvn_ed_language(ed);
	return language ? rvn_language_desc(language) : NULL;
}

static void rvn_ed_metrics(const rvn_text_editor_t* ed, rvn_ed_metrics_t* m) {
	const rvn_widget_t* widget = &ed->base;
	rvn_font_metrics_t font_metrics;
	float digit;
	int digits = 1;
	uint32_t count;

	m->theme = rvn_widget_theme(widget);
	m->font_id = rvn_ed_font_id(ed);
	m->font = rvn_font_resolve(m->font_id);
	m->bold_id = m->italic_id = m->font_id;
	{
		/* (the styles of a font are made when first asked for, here: the editor is measured before it is painted) */
		const rvn_language_t* language = rvn_ed_language((rvn_text_editor_t*)(uintptr_t)ed);
		if (language && (rvn_language_desc(language)->flags & R_LANGUAGE_FORMATTED_TEXT)) {
			m->bold_id = rvn_font_variant(m->font_id, RVN_FONT_BOLD);
			m->italic_id = rvn_font_variant(m->font_id, RVN_FONT_ITALIC);
		}
	}
	m->size = rvn_widget_font_size(widget, ed->font_size) * (ed->zoom > 0.0f ? ed->zoom : 1.0f);
	rvn_font_metrics(m->font, m->size, &font_metrics);
	m->line_height = ceilf(font_metrics.line_height + 2.0f);
	m->space = rvn_text_width(m->font, m->size, " ", 1);
	if (m->space <= 0.0f) m->space = m->size * 0.3f;
	m->tab = m->space * (float)rvn_maxi(ed->tab_size, 1);
	m->bar = rvn_widget_metric(widget, R_METRIC_SCROLLBAR_SIZE);

	m->gutter = 0.0f;
	if (ed->line_numbers) {
		digit = rvn_text_width(m->font, m->size, "0", 1);
		for (count = ed->line_count; count >= 10; count /= 10) digits++;
		m->gutter = ceilf(digit * (float)rvn_maxi(digits, 2) + 18.0f);
	}
	m->area.x = m->gutter + RVN_ED_PAD_X;
	m->area.y = RVN_ED_PAD_Y;
	/* with word wrap the room of the scrollbar is always kept: the text does not flow again when it appears */
	m->area.w = rvn_maxf(0.0f, widget->rect.w - m->area.x - RVN_ED_PAD_X -
		((ed->wrap || rvn_scrollbar_needed(&ed->vbar)) ? m->bar : 0.0f));
	m->area.h = rvn_maxf(0.0f, widget->rect.h - RVN_ED_PAD_Y * 2.0f -
		((!ed->wrap && rvn_scrollbar_needed(&ed->hbar)) ? m->bar : 0.0f));
}

/* Horizontal position reached after text[from..to), starting at `x` on the row: tabs jump to the next stop. */
static float rvn_ed_advance(const rvn_ed_metrics_t* m, const char* text, int from, int to, float x) {
	int i, run = from;
	for (i = from; i < to; i++) {
		if (text[i] != '\t') continue;
		if (i > run) x += rvn_text_width(m->font, m->size, text + run, i - run);
		x = (floorf(x / m->tab + 0.001f) + 1.0f) * m->tab;
		run = i + 1;
	}
	if (to > run) x += rvn_text_width(m->font, m->size, text + run, to - run);
	return x;
}

/* Byte offset in text[from..to) nearest to the horizontal position `target`. */
static int rvn_ed_offset_at(const rvn_ed_metrics_t* m, const char* text, int from, int to, float target) {
	float x = 0.0f;
	int i = from;
	while (i < to) {
		int next = rvn_utf8_next(text, to, i);
		float after = rvn_ed_advance(m, text, i, next, x);
		if (target < (x + after) * 0.5f) return i;
		x = after;
		i = next;
	}
	return to;
}

/* ---------------------------------------------------------------------------
 *  Rows (word wrap)
 * ------------------------------------------------------------------------- */

/* Cuts one line into rows no wider than `width`, at spaces when it can. */
static void rvn_ed_wrap_line(const rvn_ed_metrics_t* m, rvn_ed_line_t* line, float width) {
	int breaks_cap = 0, count = 0, i = 0, row_start = 0, last_space = -1;
	float x = 0.0f;
	int* breaks = NULL;

	while (i < line->length) {
		int next = rvn_utf8_next(line->text, line->length, i);
		float after = rvn_ed_advance(m, line->text, i, next, x);
		if (after > width && i > row_start) {
			int cut = last_space > row_start ? last_space : i;
			if (count + 1 > breaks_cap) {
				breaks_cap = breaks_cap ? breaks_cap * 2 : 4;
				breaks = (int*)rvn_realloc(breaks, sizeof(int) * (size_t)breaks_cap);
			}
			breaks[count++] = cut;
			row_start = cut;
			last_space = -1;
			x = rvn_ed_advance(m, line->text, cut, i, 0.0f);
			continue;
		}
		if (line->text[i] == ' ' || line->text[i] == '\t') last_space = next;
		x = after;
		i = next;
	}
	rvn_free(line->breaks);
	line->breaks = breaks;
	line->row_count = count + 1;
}

/* Bytes of a line shown on one of its rows. */
static void rvn_ed_row_range(const rvn_text_editor_t* ed, int line, int row, int* begin, int* end) {
	const rvn_ed_line_t* l = &ed->lines[line];
	if (!ed->wrap || l->row_count <= 1) {
		*begin = 0;
		*end = l->length;
		return;
	}
	*begin = row > 0 ? l->breaks[row - 1] : 0;
	*end = row + 1 < l->row_count ? l->breaks[row] : l->length;
}

/* Row of a line that shows a column: a column on a cut is at the start of the row after it. */
static int rvn_ed_row_of(const rvn_text_editor_t* ed, int line, int column) {
	const rvn_ed_line_t* l = &ed->lines[line];
	int row = 0;
	if (!ed->wrap || l->row_count <= 1) return 0;
	while (row + 1 < l->row_count && l->breaks[row] <= column) row++;
	return row;
}

/* R_TRUE when the line is inside a block folded on a line above it. */
static int rvn_ed_line_hidden(const rvn_text_editor_t* ed, int line) {
	uint32_t i;
	if (ed->no_folding) return R_FALSE;
	for (i = 0; i < ed->fold_count; i++) {
		if (ed->folds[i].folded && line > ed->folds[i].start && line < ed->folds[i].end) return R_TRUE;
	}
	return R_FALSE;
}

/* The fold that hides a line, or NULL (the block itself, when `line` is its first line and it is folded). */
static const rvn_ed_fold_t* rvn_ed_fold_hiding(const rvn_text_editor_t* ed, int line) {
	uint32_t i;
	for (i = 0; i < ed->fold_count; i++) {
		if (ed->folds[i].folded && line > ed->folds[i].start && line < ed->folds[i].end) return &ed->folds[i];
	}
	return NULL;
}

/* Brings the row tables and the size of the content up to date. */
static void rvn_ed_update_rows(rvn_text_editor_t* ed, const rvn_ed_metrics_t* m) {
	uint32_t i, rows = 0;
	float widest = 0.0f;

	rvn_ed_folds_refresh(ed);
	if (ed->wrap && ed->wrap_width != m->area.w) {
		ed->wrap_width = m->area.w;
		for (i = 0; i < ed->line_count; i++) ed->lines[i].row_count = 0;
		ed->rows_dirty = R_TRUE;
	}
	if (!ed->rows_dirty) return;
	ed->rows_dirty = R_FALSE;

	/* every line has its first row, folded lines share the one of the line that hides them */
	RVN_ARRAY_RESERVE(ed->row_starts, ed->row_starts_cap, ed->line_count + 1);
	ed->hidden_rows = 0;
	if (ed->wrap) {
		for (i = 0; i < ed->line_count; i++) {
			if (rvn_ed_line_hidden(ed, i)) {
				ed->row_starts[i] = rows;
				continue;
			}
			if (ed->lines[i].row_count == 0) rvn_ed_wrap_line(m, &ed->lines[i], rvn_maxf(ed->wrap_width, m->space * 4.0f));
			ed->row_starts[i] = rows;
			rows += (uint32_t)ed->lines[i].row_count;
		}
		ed->row_starts[ed->line_count] = rows;
		ed->total_rows = rows;
		ed->max_width = 0.0f;
	} else {
		for (i = 0; i < ed->line_count; i++) {
			rvn_ed_line_t* line = &ed->lines[i];
			if (rvn_ed_line_hidden(ed, i)) {
				ed->row_starts[i] = rows;
				continue;
			}
			if (line->width < 0.0f) line->width = rvn_ed_advance(m, line->text, 0, line->length, 0.0f);
			if (line->width > widest) widest = line->width;
			ed->row_starts[i] = rows++;
		}
		ed->row_starts[ed->line_count] = rows;
		ed->total_rows = rows;
		ed->max_width = widest;
	}
	if (ed->total_rows == 0) {
		/* (every line is folded: the first one is shown, so that there is something to unfold) */
		ed->total_rows = 1;
		ed->row_starts[0] = 0;
	}
}

static uint32_t rvn_ed_first_row(const rvn_text_editor_t* ed, int line) {
	if (!ed->row_starts || line < 0 || line >= (int)ed->line_count) return (uint32_t)(line < 0 ? 0 : line);
	return ed->row_starts[line];
}

/* Line and row in the line of a row counted from the top of the text. */
static void rvn_ed_locate_row(const rvn_text_editor_t* ed, uint32_t global_row, int* line, int* row) {
	uint32_t low = 0, high = ed->line_count - 1;
	if (global_row >= ed->total_rows) global_row = ed->total_rows - 1;
	if (!ed->wrap) {
		/* (skipping hidden lines: row_starts maps every line to its row) */
		while (low < high) {
			uint32_t middle = (low + high + 1) / 2;
			if (ed->row_starts[middle] <= global_row) low = middle;
			else high = middle - 1;
		}
		*line = (int)low;
		*row = 0;
		return;
	}
	while (low < high) {
		uint32_t middle = (low + high + 1) / 2;
		if (ed->row_starts[middle] <= global_row) low = middle;
		else high = middle - 1;
	}
	*line = (int)low;
	*row = (int)(global_row - ed->row_starts[low]);
}

/* Where a position is drawn, in the coordinates of the text (before scrolling). */
static rvn_vec2_t rvn_ed_pos_point(const rvn_text_editor_t* ed, const rvn_ed_metrics_t* m, rvn_ed_pos_t pos) {
	int row = rvn_ed_row_of(ed, pos.line, pos.column), begin, end;
	rvn_ed_row_range(ed, pos.line, row, &begin, &end);
	return rvn_vec2(rvn_ed_advance(m, ed->lines[pos.line].text, begin, pos.column, 0.0f),
		(float)(rvn_ed_first_row(ed, pos.line) + (uint32_t)row) * m->line_height);
}

/* Position under a point given in the coordinates of the text. */
static rvn_ed_pos_t rvn_ed_point_pos(const rvn_text_editor_t* ed, const rvn_ed_metrics_t* m, float x, float y) {
	int line, row, begin, end, column;
	float global = floorf(y / m->line_height);
	if (global < 0.0f) global = 0.0f;
	if (global > (float)ed->total_rows) global = (float)ed->total_rows;
	rvn_ed_locate_row(ed, (uint32_t)global, &line, &row);
	rvn_ed_row_range(ed, line, row, &begin, &end);
	column = rvn_ed_offset_at(m, ed->lines[line].text, begin, end, x);
	/* the end of a wrapped row is the position before the cut, not the start of the next row */
	if (ed->wrap && column == end && row + 1 < ed->lines[line].row_count && column > begin) {
		column = rvn_utf8_prev(ed->lines[line].text, column);
	}
	return rvn_ed_pos(line, column);
}

static void rvn_ed_update_bars(rvn_text_editor_t* ed, const rvn_ed_metrics_t* m) {
	ed->vbar.content = (float)ed->total_rows * m->line_height;
	ed->vbar.viewport = m->area.h;
	rvn_scrollbar_clamp(&ed->vbar);
	/* room for the caret after the longest line */
	ed->hbar.content = ed->wrap ? 0.0f : ed->max_width + m->space * 2.0f;
	ed->hbar.viewport = m->area.w;
	rvn_scrollbar_clamp(&ed->hbar);
}

/* Scrolls as little as needed to show the caret. Before the editor has its size, this waits for the layout. */
static void rvn_ed_reveal(rvn_text_editor_t* ed, int in_layout) {
	rvn_ed_metrics_t m;
	rvn_vec2_t point;
	if (!in_layout && (ed->base.rect.w <= 0.0f || ed->base.rect.h <= 0.0f || (ed->base.flags & RVN_WF_LAYOUT_DIRTY))) {
		ed->reveal_pending = R_TRUE;
		return;
	}
	rvn_ed_metrics(ed, &m);
	rvn_ed_update_rows(ed, &m);
	rvn_ed_update_bars(ed, &m);
	point = rvn_ed_pos_point(ed, &m, ed->caret);
	if (point.y < ed->vbar.offset) rvn_scrollbar_set(&ed->vbar, point.y);
	else if (point.y + m.line_height > ed->vbar.offset + ed->vbar.viewport) {
		rvn_scrollbar_set(&ed->vbar, point.y + m.line_height - ed->vbar.viewport);
	}
	if (!ed->wrap) {
		if (point.x < ed->hbar.offset + m.space) rvn_scrollbar_set(&ed->hbar, point.x - m.space * 4.0f);
		else if (point.x > ed->hbar.offset + ed->hbar.viewport - m.space * 2.0f) {
			rvn_scrollbar_set(&ed->hbar, point.x - ed->hbar.viewport + m.space * 6.0f);
		}
	}
	ed->reveal_pending = R_FALSE;
}

/* The caret moved: keep it visible and solid for a moment. */
static void rvn_ed_caret_moved(rvn_text_editor_t* ed, int keep_goal) {
	if (!keep_goal) ed->goal_x = -1.0f;
	ed->blink_start = rvn_clock_seconds();
	rvn_ed_reveal(ed, R_FALSE);
	rvn_widget_invalidate_paint(&ed->base);
}

/* ---------------------------------------------------------------------------
 *  Layout
 * ------------------------------------------------------------------------- */

static rvn_vec2_t rvn_text_editor_measure(rvn_widget_t* widget) {
	rvn_text_editor_t* ed = (rvn_text_editor_t*)widget;
	rvn_ed_metrics_t m;
	rvn_ed_metrics(ed, &m);
	/* an editor is given its size by its parent; by itself it asks for a few lines */
	return rvn_vec2(220.0f, m.line_height * 5.0f + RVN_ED_PAD_Y * 2.0f);
}

static void rvn_text_editor_arrange(rvn_widget_t* widget) {
	rvn_text_editor_t* ed = (rvn_text_editor_t*)widget;
	rvn_ed_metrics_t m;
	int pass;
	/* the scrollbars take room, which changes what is needed: twice is enough to settle */
	for (pass = 0; pass < 2; pass++) {
		rvn_ed_metrics(ed, &m);
		rvn_ed_update_rows(ed, &m);
		rvn_ed_update_bars(ed, &m);
	}
	if (ed->reveal_pending) rvn_ed_reveal(ed, R_TRUE);
}

static void rvn_ed_tracks(const rvn_text_editor_t* ed, const rvn_ed_metrics_t* m, rvn_rect_t* vertical, rvn_rect_t* horizontal) {
	const rvn_widget_t* widget = &ed->base;
	int h_needed = !ed->wrap && rvn_scrollbar_needed(&ed->hbar);
	*vertical = rvn_rect(widget->rect.w - m->bar - 1.0f, 1.0f, m->bar, widget->rect.h - 2.0f - (h_needed ? m->bar : 0.0f));
	*horizontal = rvn_rect(m->gutter + 1.0f, widget->rect.h - m->bar - 1.0f,
		widget->rect.w - m->gutter - 2.0f - (rvn_scrollbar_needed(&ed->vbar) ? m->bar : 0.0f), m->bar);
}

/* ---------------------------------------------------------------------------
 *  Paint
 * ------------------------------------------------------------------------- */

/* Makes the lexer state of the lines up to `line` valid. */
static void rvn_ed_lex_until(rvn_text_editor_t* ed, uint32_t line) {
	const rvn_language_t* language = rvn_ed_language(ed);
	if (ed->lex_valid == 0) {
		ed->lines[0].lex_in = 0;
		ed->lex_valid = 1;
	}
	while (ed->lex_valid <= line && ed->lex_valid < ed->line_count) {
		const rvn_ed_line_t* previous = &ed->lines[ed->lex_valid - 1];
		ed->lines[ed->lex_valid].lex_in = rvn_syntax_line(language, previous->text, previous->length, previous->lex_in, NULL, 0, NULL);
		ed->lex_valid++;
	}
}

/* ---------------------------------------------------------------------------
 *  Brackets
 * ------------------------------------------------------------------------- */

/* R_TRUE when a column of a line is code: not inside a comment or a text. */
static int rvn_ed_is_code(rvn_text_editor_t* ed, int line, int column) {
	const rvn_language_t* language = rvn_ed_language(ed);
	rvn_span_t spans[RVN_ED_MAX_SPANS];
	int count = 0, i;
	if (!language) return R_TRUE;
	rvn_ed_lex_until(ed, (uint32_t)line);
	rvn_syntax_line(language, ed->lines[line].text, ed->lines[line].length, ed->lines[line].lex_in, spans, RVN_ED_MAX_SPANS, &count);
	for (i = 0; i < count; i++) {
		if (column >= spans[i].start && column < spans[i].end) {
			return spans[i].token != RVN_TOKEN_COMMENT && spans[i].token != RVN_TOKEN_STRING;
		}
	}
	return R_TRUE;
}

/* The bracket that closes (or opens) the one at `at`. Looks no further than a few thousand lines. */
static int rvn_ed_match_bracket(rvn_text_editor_t* ed, rvn_ed_pos_t at, rvn_ed_pos_t* match) {
	static const char open[] = "([{", close[] = ")]}";
	const rvn_language_t* language = rvn_ed_language(ed);
	const rvn_ed_line_t* l = &ed->lines[at.line];
	rvn_span_t spans[RVN_ED_MAX_SPANS];
	const char* found;
	char bracket, other;
	int forward, depth = 1, line = at.line, column, budget = 3000, span_count = 0, i;

	if (at.column < 0 || at.column >= l->length) return R_FALSE;
	bracket = l->text[at.column];
	if ((found = strchr(open, bracket)) != NULL && bracket) {
		forward = R_TRUE;
		other = close[found - open];
	} else if ((found = strchr(close, bracket)) != NULL && bracket) {
		forward = R_FALSE;
		other = open[found - close];
	} else {
		return R_FALSE;
	}
	if (!rvn_ed_is_code(ed, at.line, at.column)) return R_FALSE;

	column = at.column + (forward ? 1 : -1);
	while (line >= 0 && line < (int)ed->line_count && budget-- > 0) {
		l = &ed->lines[line];
		if (language) {
			rvn_ed_lex_until(ed, (uint32_t)line);
			rvn_syntax_line(language, l->text, l->length, l->lex_in, spans, RVN_ED_MAX_SPANS, &span_count);
		}
		for (; column >= 0 && column < l->length; column += forward ? 1 : -1) {
			char c = l->text[column];
			int code = R_TRUE;
			if (c != bracket && c != other) continue;
			for (i = 0; i < span_count && code; i++) {
				if (column >= spans[i].start && column < spans[i].end) {
					code = spans[i].token != RVN_TOKEN_COMMENT && spans[i].token != RVN_TOKEN_STRING;
				}
			}
			if (!code) continue;
			depth += c == bracket ? 1 : -1;
			if (depth == 0) {
				*match = rvn_ed_pos(line, column);
				return R_TRUE;
			}
		}
		line += forward ? 1 : -1;
		if (line >= 0 && line < (int)ed->line_count) column = forward ? 0 : ed->lines[line].length - 1;
	}
	return R_FALSE;
}

/* The pair of brackets to show for the place of the caret: the one after it, else the one before it. */
static void rvn_ed_update_match(rvn_text_editor_t* ed) {
	rvn_ed_pos_t at = ed->caret;
	if (ed->match_version == ed->version && rvn_ed_pos_cmp(ed->match_caret, ed->caret) == 0) return;
	ed->match_version = ed->version;
	ed->match_caret = ed->caret;
	ed->match_found = rvn_ed_match_bracket(ed, at, &ed->match_b);
	if (!ed->match_found && at.column > 0) {
		at.column--;
		ed->match_found = rvn_ed_match_bracket(ed, at, &ed->match_b);
	}
	ed->match_a = at;
}

/* Draws text[begin..end) of a line at (x, y), tabs expanded, each span in its color. */
static void rvn_ed_paint_text(RPainter* painter, const rvn_ed_metrics_t* m, const char* text, int begin, int end,
	float x, float y, const rvn_span_t* spans, int span_count, rvn_color_t color) {

	float pen = 0.0f;
	int i = begin, span = 0;

	while (i < end) {
		int stop = end;
		rvn_color_t run_color = color;
		Rid font = m->font_id;
		const char* tab;
		while (span < span_count && spans[span].end <= i) span++;
		if (span < span_count) {
			if (spans[span].start <= i) {
				rvn_token_t token = (rvn_token_t)spans[span].token;
				run_color = rvn_syntax_color(m->theme, token, color);
				/* formatted text: the styles have the advances of the font, so the columns do not move */
				if (token == RVN_TOKEN_STRONG || token == RVN_TOKEN_HEADING) font = m->bold_id;
				else if (token == RVN_TOKEN_EMPHASIS) font = m->italic_id;
				stop = rvn_mini(stop, spans[span].end);
			} else {
				stop = rvn_mini(stop, spans[span].start);
			}
		}
		if (text[i] == '\t') {
			pen = (floorf(pen / m->tab + 0.001f) + 1.0f) * m->tab;
			i++;
			continue;
		}
		tab = (const char*)memchr(text + i, '\t', (size_t)(stop - i));
		if (tab) stop = (int)(tab - text);
		rvn_paint_text(painter, font, m->size, x + pen, y + 1.0f, text + i, stop - i, run_color);
		pen += rvn_text_width(m->font, m->size, text + i, stop - i);
		i = stop;
	}
}

static rvn_color_t rvn_ed_marker_color(RMarkerKind kind, const rvn_widget_t* widget) {
	switch (kind) {
		case R_MARKER_ERROR: return rvn_widget_color(widget, R_COLOR_DANGER);
		case R_MARKER_WARNING: return rvn_widget_color(widget, R_COLOR_WARNING);
		default: return rvn_widget_color(widget, R_COLOR_ACCENT);
	}
}

static void rvn_text_editor_paint(rvn_widget_t* widget, RPainter* painter) {
	rvn_text_editor_t* ed = (rvn_text_editor_t*)widget;
	rvn_ed_metrics_t m;
	rvn_rect_t rect = rvn_rect(0.0f, 0.0f, widget->rect.w, widget->rect.h);
	rvn_rect_t vertical, horizontal;
	rvn_vec2_t origin = rvn_widget_origin(widget);
	int focused = (widget->flags & RVN_WF_FOCUSED) != 0;
	int has_selection = rvn_ed_has_selection(ed);
	rvn_ed_pos_t sel_min = rvn_ed_sel_min(ed), sel_max = rvn_ed_sel_max(ed);
	rvn_color_t text = rvn_widget_text_color(widget, painter);
	rvn_color_t selection = rvn_widget_color(widget, R_COLOR_SELECTION);
	rvn_color_t dim = rvn_widget_color(widget, R_COLOR_TEXT_PLACEHOLDER);
	float radius = rvn_widget_metric(widget, R_METRIC_CORNER_RADIUS);
	float focus = rvn_clampf(rvn_paint_spring(painter, &widget->press, focused ? 1.0f : 0.0f, R_MOTION_SNAPPY), 0.0f, 1.0f);
	float scroll_x, scroll_y, y;
	uint32_t row, first_row, last_row, marker;
	rvn_span_t spans[RVN_ED_MAX_SPANS];
	int span_count = 0, span_line = -1;
	const rvn_language_t* language = rvn_ed_language(ed);

	rvn_ed_metrics(ed, &m);
	rvn_ed_update_rows(ed, &m);
	if (!focused) selection = rvn_color_fade(selection, 0.55f);

	/* frame */
	if (rvn_widget_glass(widget)) {
		rvn_paint_surface(painter, widget, rect, rvn_minf(radius, 14.0f), RVN_SURFACE_INPUT, 0.0f, 0.0f);
		if (focus > 0.01f) {
			rvn_paint_box(painter, rect, rvn_minf(radius, 14.0f), 0, 1.5f, rvn_color_fade(rvn_widget_color(widget, R_COLOR_FOCUS), focus));
		}
	} else {
		rvn_paint_box(painter, rect, radius, rvn_widget_color(widget, R_COLOR_INPUT), rvn_widget_metric(widget, R_METRIC_BORDER_WIDTH),
			rvn_color_mix(rvn_widget_color(widget, R_COLOR_BORDER), rvn_widget_color(widget, R_COLOR_FOCUS), focus));
	}

	scroll_y = rvn_scrollbar_shown(&ed->vbar, painter);
	scroll_x = ed->wrap ? 0.0f : rvn_scrollbar_shown(&ed->hbar, painter);
	/* whole pixels: the text stays sharp while it scrolls */
	scroll_y = floorf(scroll_y * painter->scale + 0.5f) / painter->scale;
	scroll_x = floorf(scroll_x * painter->scale + 0.5f) / painter->scale;

	first_row = scroll_y > 0.0f ? (uint32_t)(scroll_y / m.line_height) : 0;
	last_row = (uint32_t)rvn_maxf(0.0f, (scroll_y + m.area.h) / m.line_height) + 1;
	if (last_row > ed->total_rows) last_row = ed->total_rows;

	/* margin of the line numbers */
	if (ed->line_numbers) {
		rvn_paint_push_clip(painter, rvn_rect(1.0f, 1.0f, m.gutter, rect.h - 2.0f));
		rvn_paint_rect(painter, rvn_rect(m.gutter - 1.0f, 4.0f, 1.0f, rect.h - 8.0f), rvn_widget_color(widget, R_COLOR_SEPARATOR));
		for (row = first_row; row < last_row; row++) {
			int line, line_row, fold_at;
			char number[16];
			rvn_ed_locate_row(ed, row, &line, &line_row);
			if (line_row != 0) continue;
			snprintf(number, sizeof(number), "%d", line + 1);
			rvn_paint_text_in(painter, m.font_id, m.size,
				rvn_rect(0.0f, m.area.y + (float)row * m.line_height - scroll_y, m.gutter - 9.0f, m.line_height), 1.0f, number, -1,
				line == ed->caret.line ? text : dim);
			/* a dot in the margin of the lines that have a marker */
			for (marker = 0; marker < ed->marker_count; marker++) {
				if (ed->markers[marker].line != line) continue;
				rvn_paint_box(painter, rvn_rect(4.0f, m.area.y + (float)row * m.line_height - scroll_y + m.line_height * 0.5f - 3.0f, 6.0f, 6.0f),
					3.0f, rvn_ed_marker_color(ed->markers[marker].kind, widget), 0.0f, 0);
				break;
			}
			/* the arrow of a block that can be folded, and the "..." of one that is */
			if (!ed->no_folding && ed->fold_count > 0) {
				fold_at = rvn_ed_fold_line_at(ed, line);
				if (fold_at >= 0) {
					float arrow_x = m.gutter - 16.0f, arrow_y = m.area.y + (float)row * m.line_height - scroll_y + m.line_height * 0.5f;
					if (ed->folds[fold_at].folded) {
						/* a triangle pointing right */
						rvn_paint_line(painter, arrow_x - 2.0f, arrow_y - 3.5f, arrow_x + 2.5f, arrow_y, 1.4f, dim);
						rvn_paint_line(painter, arrow_x + 2.5f, arrow_y, arrow_x - 2.0f, arrow_y + 3.5f, 1.4f, dim);
						rvn_paint_line(painter, arrow_x - 2.0f, arrow_y + 3.5f, arrow_x - 2.0f, arrow_y - 3.5f, 1.4f, dim);
					} else {
						/* a triangle pointing down */
						rvn_paint_line(painter, arrow_x - 3.5f, arrow_y - 2.0f, arrow_x, arrow_y + 2.5f, 1.4f, dim);
						rvn_paint_line(painter, arrow_x, arrow_y + 2.5f, arrow_x + 3.5f, arrow_y - 2.0f, 1.4f, dim);
						rvn_paint_line(painter, arrow_x + 3.5f, arrow_y - 2.0f, arrow_x - 3.5f, arrow_y - 2.0f, 1.4f, dim);
					}
				}
			}
		}
		rvn_paint_pop_clip(painter);
	}

	rvn_paint_push_clip(painter, rvn_rect(m.gutter + 1.0f, 1.0f, rect.w - m.gutter - 2.0f, rect.h - 2.0f));

	if (ed->line_count == 1 && ed->lines[0].length == 0 && ed->placeholder) {
		rvn_paint_text(painter, m.font_id, m.size, m.area.x, m.area.y + 1.0f, ed->placeholder, -1, dim);
	}

	if (language && last_row > 0) {
		int line, line_row;
		rvn_ed_locate_row(ed, last_row - 1, &line, &line_row);
		rvn_ed_lex_until(ed, (uint32_t)line);
	}
	/* the bracket next to the caret and the one that goes with it */
	if (focused && !has_selection && language) rvn_ed_update_match(ed);
	else ed->match_found = R_FALSE;
	/* the places the search finds, if there is one */
	if (ed->search_pattern && ed->search_pattern[0]) rvn_ed_update_matches(ed);

	for (row = first_row; row < last_row; row++) {
		const rvn_ed_line_t* l;
		int line, line_row, begin, end;
		rvn_ed_locate_row(ed, row, &line, &line_row);
		rvn_ed_row_range(ed, line, line_row, &begin, &end);
		l = &ed->lines[line];
		y = m.area.y + (float)row * m.line_height - scroll_y;

		/* the line of the caret, lightly marked */
		if (focused && !has_selection && line == ed->caret.line && !ed->read_only) {
			rvn_paint_rect(painter, rvn_rect(m.gutter + 1.0f, y, rect.w - m.gutter - 2.0f, m.line_height),
				rvn_color_fade(rvn_widget_color(widget, R_COLOR_ROW_HOVERED), 0.6f));
		}
		/* the part of the selection that lies on this row */
		if (has_selection && line >= sel_min.line && line <= sel_max.line) {
			int from = line == sel_min.line ? rvn_maxi(sel_min.column, begin) : begin;
			int to = line == sel_max.line ? rvn_mini(sel_max.column, end) : end;
			if (from <= to && !(line == sel_max.line && sel_max.column < begin) && !(line == sel_min.line && sel_min.column > end)) {
				float x0 = rvn_ed_advance(&m, l->text, begin, from, 0.0f);
				float x1 = rvn_ed_advance(&m, l->text, from, to, x0);
				/* a selected line break shows as a little more than the text */
				if (line < sel_max.line && end == l->length) x1 += m.space;
				if (x1 > x0) {
					rvn_paint_box(painter, rvn_rect(m.area.x + x0 - scroll_x, y, x1 - x0, m.line_height), 3.0f, selection, 0.0f, 0);
				}
			}
		}
		if (ed->match_found) {
			int which;
			for (which = 0; which < 2; which++) {
				rvn_ed_pos_t at = which ? ed->match_b : ed->match_a;
				float x0;
				if (at.line != line || at.column < begin || at.column >= end) continue;
				x0 = rvn_ed_advance(&m, l->text, begin, at.column, 0.0f);
				rvn_paint_box(painter, rvn_rect(m.area.x + x0 - scroll_x - 1.0f, y + 1.0f,
					rvn_ed_advance(&m, l->text, at.column, at.column + 1, x0) - x0 + 2.0f, m.line_height - 2.0f), 3.0f,
					rvn_color_fade(rvn_widget_color(widget, R_COLOR_ACCENT), 0.22f), 1.0f, rvn_color_fade(rvn_widget_color(widget, R_COLOR_ACCENT), 0.7f));
			}
		}
		if (end > begin) {
			if (language && span_line != line) {
				rvn_syntax_line(language, l->text, l->length, l->lex_in, spans, RVN_ED_MAX_SPANS, &span_count);
				span_line = line;
			}
			rvn_ed_paint_text(painter, &m, l->text, begin, end, m.area.x - scroll_x, y,
				spans, language ? span_count : 0, text);
		}
		/* markers: a wavy line under what they are about */
		for (marker = 0; marker < ed->marker_count; marker++) {
			const rvn_ed_marker_t* mark = &ed->markers[marker];
			int from, to;
			float x0, x1, wave;
			rvn_color_t color;
			if (mark->line != line) continue;
			from = rvn_clampi(mark->from, begin, end);
			to = mark->to < 0 ? end : rvn_clampi(mark->to, begin, end);
			if (to < from || (mark->from >= end && end > begin && line_row + 1 < l->row_count)) continue;
			x0 = rvn_ed_advance(&m, l->text, begin, from, 0.0f);
			x1 = rvn_ed_advance(&m, l->text, from, to, x0);
			/* (something to see even for an empty range) */
			if (x1 - x0 < m.space * 1.5f) x1 = x0 + m.space * 1.5f;
			color = rvn_ed_marker_color(mark->kind, widget);
			for (wave = x0; wave < x1; wave += 4.0f) {
				float next = rvn_minf(wave + 4.0f, x1);
				int up = ((int)((wave - x0) / 4.0f) & 1) != 0;
				rvn_paint_line(painter, m.area.x + wave - scroll_x, y + m.line_height - (up ? 3.5f : 1.5f),
					m.area.x + next - scroll_x, y + m.line_height - (up ? 1.5f : 3.5f), 1.2f, color);
			}
		}
		/* the matches of the search: a light box under the text */
		if (ed->match_count > 0) {
			uint32_t match;
			for (match = 0; match < ed->match_count; match++) {
				const rvn_ed_match_t* found = &ed->matches[match];
				int from, to;
				float x0, x1;
				if (found->from.line > line || found->to.line < line) continue;
				from = found->from.line == line ? rvn_clampi(found->from.column, begin, end) : begin;
				to = found->to.line == line ? rvn_clampi(found->to.column, begin, end) : end;
				if (to < from) continue;
				x0 = rvn_ed_advance(&m, l->text, begin, from, 0.0f);
				x1 = rvn_ed_advance(&m, l->text, from, to, x0);
				if (x1 - x0 < 2.0f) x1 = x0 + m.space * 0.8f;
				rvn_paint_box(painter, rvn_rect(m.area.x + x0 - scroll_x - 1.0f, y + 1.0f, x1 - x0 + 2.0f, m.line_height - 2.0f),
					2.0f, rvn_color_fade(rvn_widget_color(widget, R_COLOR_WARNING), 0.30f), 0.0f, 0);
			}
		}
		/* folded block: the "..." after the first line of the block */
		if (!ed->no_folding && ed->fold_count > 0 && line_row == 0) {
			int fold_at = rvn_ed_fold_line_at(ed, line);
			if (fold_at >= 0 && ed->folds[fold_at].folded) {
				float x0 = rvn_ed_advance(&m, l->text, begin, l->length, 0.0f);
				rvn_paint_text(painter, m.font_id, m.size, m.area.x + x0 - scroll_x + m.space, y + 1.0f, "...", -1,
					rvn_color_fade(rvn_widget_color(widget, R_COLOR_ACCENT), 0.8f));
			}
		}
	}

	/* the caret glides to its place and blinks once it rests there */
	if (focused && !ed->read_only) {
		rvn_vec2_t point = rvn_ed_pos_point(ed, &m, ed->caret);
		float x = rvn_paint_spring(painter, &ed->caret_x, point.x, R_MOTION_SNAPPY);
		float caret_y = rvn_paint_spring(painter, &ed->caret_y, point.y, R_MOTION_SNAPPY);
		int moving = rvn_spring_moving(&ed->caret_x, point.x) || rvn_spring_moving(&ed->caret_y, point.y);
		rvn_rect_t caret = rvn_rect(m.area.x + x - scroll_x - 1.0f, m.area.y + caret_y - scroll_y + 1.0f, 2.0f, m.line_height - 2.0f);
		/* the other carets, each with what it has selected, and a caret of its own */
		if (ed->caret_count > 0) {
			uint32_t which;
			for (which = 0; which < ed->caret_count; which++) {
				rvn_ed_pos_t at = ed->carets[which].caret, anchor = ed->carets[which].anchor;
				rvn_vec2_t other = rvn_ed_pos_point(ed, &m, at);
				rvn_paint_box(painter, rvn_rect(m.area.x + other.x - scroll_x - 1.0f, m.area.y + other.y - scroll_y + 1.0f,
					2.0f, m.line_height - 2.0f), 1.0f, rvn_widget_color(widget, R_COLOR_ACCENT_HOVERED), 0.0f, 0);
				if (rvn_ed_pos_cmp(anchor, at) != 0) {
					rvn_ed_pos_t from = rvn_ed_pos_cmp(anchor, at) < 0 ? anchor : at;
					rvn_ed_pos_t to = rvn_ed_pos_cmp(anchor, at) < 0 ? at : anchor;
					int other_line;
					for (other_line = from.line; other_line <= to.line && other_line < (int)ed->line_count; other_line++) {
						int line_row, begin, end, a, b;
						float x0, x1, top;
						line_row = rvn_ed_row_of(ed, other_line, other_line == from.line ? from.column : 0);
						rvn_ed_row_range(ed, other_line, line_row, &begin, &end);
						a = other_line == from.line ? rvn_maxi(from.column, begin) : begin;
						b = other_line == to.line ? rvn_mini(to.column, end) : end;
						if (b <= a) continue;
						x0 = rvn_ed_advance(&m, ed->lines[other_line].text, begin, a, 0.0f);
						x1 = rvn_ed_advance(&m, ed->lines[other_line].text, a, b, x0);
						top = m.area.y + (float)rvn_ed_first_row(ed, other_line) * m.line_height - scroll_y;
						rvn_paint_box(painter, rvn_rect(m.area.x + x0 - scroll_x, top, x1 - x0, m.line_height), 3.0f,
							rvn_color_fade(selection, 0.75f), 0.0f, 0);
					}
				}
			}
		}
		rvn_text_input_rect(widget, painter, caret);
		if (moving || fmod(rvn_clock_seconds() - ed->blink_start, 1.0) < 0.55) {
			rvn_paint_box(painter, caret,
				1.0f, rvn_widget_color(widget, R_COLOR_ACCENT_HOVERED), 0.0f, 0);
		}
	} else {
		ed->caret_x.started = R_FALSE;
		ed->caret_y.started = R_FALSE;
	}
	rvn_paint_pop_clip(painter);

	rvn_ed_tracks(ed, &m, &vertical, &horizontal);
	rvn_scrollbar_paint(&ed->vbar, widget, painter, vertical, R_TRUE,
		widget->window ? widget->window->mouse_x - origin.x : -1.0f, widget->window ? widget->window->mouse_y - origin.y : -1.0f);
	if (!ed->wrap) {
		rvn_scrollbar_paint(&ed->hbar, widget, painter, horizontal, R_FALSE,
			widget->window ? widget->window->mouse_x - origin.x : -1.0f, widget->window ? widget->window->mouse_y - origin.y : -1.0f);
	}
}

/* ---------------------------------------------------------------------------
 *  Editing commands
 * ------------------------------------------------------------------------- */

typedef enum rvn_ed_class_t {
	RVN_ED_SPACE,
	RVN_ED_WORD,
	RVN_ED_PUNCTUATION
} rvn_ed_class_t;

static rvn_ed_class_t rvn_ed_class(char c) {
	if (c == ' ' || c == '\t') return RVN_ED_SPACE;
	if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || (unsigned char)c >= 0x80) return RVN_ED_WORD;
	return RVN_ED_PUNCTUATION;
}

static rvn_ed_pos_t rvn_ed_char_left(const rvn_text_editor_t* ed, rvn_ed_pos_t pos) {
	if (pos.column > 0) return rvn_ed_pos(pos.line, rvn_utf8_prev(ed->lines[pos.line].text, pos.column));
	if (pos.line > 0) return rvn_ed_pos(pos.line - 1, ed->lines[pos.line - 1].length);
	return pos;
}

static rvn_ed_pos_t rvn_ed_char_right(const rvn_text_editor_t* ed, rvn_ed_pos_t pos) {
	const rvn_ed_line_t* line = &ed->lines[pos.line];
	if (pos.column < line->length) return rvn_ed_pos(pos.line, rvn_utf8_next(line->text, line->length, pos.column));
	if (pos.line + 1 < (int)ed->line_count) return rvn_ed_pos(pos.line + 1, 0);
	return pos;
}

static rvn_ed_pos_t rvn_ed_word_left(const rvn_text_editor_t* ed, rvn_ed_pos_t pos) {
	const char* text = ed->lines[pos.line].text;
	rvn_ed_class_t kind;
	if (pos.column == 0) return rvn_ed_char_left(ed, pos);
	while (pos.column > 0 && rvn_ed_class(text[pos.column - 1]) == RVN_ED_SPACE) pos.column--;
	if (pos.column == 0) return pos;
	kind = rvn_ed_class(text[pos.column - 1]);
	while (pos.column > 0 && rvn_ed_class(text[pos.column - 1]) == kind) pos.column--;
	return pos;
}

static rvn_ed_pos_t rvn_ed_word_right(const rvn_text_editor_t* ed, rvn_ed_pos_t pos) {
	const rvn_ed_line_t* line = &ed->lines[pos.line];
	rvn_ed_class_t kind;
	if (pos.column >= line->length) return rvn_ed_char_right(ed, pos);
	kind = rvn_ed_class(line->text[pos.column]);
	while (pos.column < line->length && rvn_ed_class(line->text[pos.column]) == kind) pos.column++;
	while (pos.column < line->length && rvn_ed_class(line->text[pos.column]) == RVN_ED_SPACE) pos.column++;
	return pos;
}

/* Up or down by `rows` rows, keeping the horizontal position. */
static rvn_ed_pos_t rvn_ed_move_rows(rvn_text_editor_t* ed, const rvn_ed_metrics_t* m, rvn_ed_pos_t pos, int rows) {
	rvn_vec2_t point = rvn_ed_pos_point(ed, m, pos);
	int global = (int)(rvn_ed_first_row(ed, pos.line) + (uint32_t)rvn_ed_row_of(ed, pos.line, pos.column)) + rows;
	if (ed->goal_x < 0.0f) ed->goal_x = point.x;
	if (global < 0) return rvn_ed_pos(0, 0);
	if (global >= (int)ed->total_rows) return rvn_ed_end(ed);
	return rvn_ed_point_pos(ed, m, ed->goal_x, ((float)global + 0.5f) * m->line_height);
}

static void rvn_ed_copy(const rvn_text_editor_t* ed) {
	char* text;
	if (!rvn_ed_has_selection(ed)) return;
	text = rvn_ed_range(ed, rvn_ed_sel_min(ed), rvn_ed_sel_max(ed), NULL);
	Rclipboard_set_text(text);
	rvn_free(text);
}

/* Replaces the selection by a text, as one step of the history. */
static void rvn_ed_insert(rvn_text_editor_t* ed, const char* text, int length) {
	ed->caret = ed->anchor = rvn_ed_replace(ed, rvn_ed_sel_min(ed), rvn_ed_sel_max(ed), text, length, R_TRUE);
}

/** Where a position is in the text, as a byte offset from its start (lines are joined by '\n'). */
static int rvn_ed_offset(const rvn_text_editor_t* ed, rvn_ed_pos_t pos) {
	int line, offset = 0;
	for (line = 0; line < pos.line && line < (int)ed->line_count; line++) offset += ed->lines[line].length + 1;
	return offset + pos.column;
}

/** The position of a byte offset in the text, after an edit. */
static rvn_ed_pos_t rvn_ed_pos_of(const rvn_text_editor_t* ed, int offset) {
	int line = 0;
	while (line < (int)ed->line_count - 1 && offset > ed->lines[line].length) {
		offset -= ed->lines[line].length + 1;
		line++;
	}
	return rvn_ed_pos(line, rvn_clampi(offset, 0, ed->lines[line].length));
}

/* One caret and what it is about, while several are edited at once. */
typedef struct rvn_ed_caret_edit_t {
	/** The start and the end of the edit, as offsets: the selection of the caret, or the character it erases. */
	int from;
	int to;
	/** The caret, by index: 0 is the main one. */
	int index;
} rvn_ed_caret_edit_t;

/* From the end of the text to its start: an edit never moves the bytes still to be edited. */
static int rvn_ed_caret_edit_compare(const void* a, const void* b) {
	const rvn_ed_caret_edit_t* x = (const rvn_ed_caret_edit_t*)a;
	const rvn_ed_caret_edit_t* y = (const rvn_ed_caret_edit_t*)b;
	return x->from == y->from ? (x->to == y->to ? 0 : (x->to < y->to ? 1 : -1)) : (x->from < y->from ? 1 : -1);
}

/*
 * Types (or deletes) at every caret at once, as one step of the history.
 * mode 0: typing, which replaces what each caret has selected; 1: backspace;
 * 2: delete.
 *
 * The carets are edited from the end of the text to its start, so an edit
 * leaves the bytes that come before it where they are. Each caret remembers
 * where its own edit ended, as an offset, and the offsets that come after an
 * edit move with it: the places are exact even when an edit adds lines.
 */
static void rvn_ed_multi_edit(rvn_text_editor_t* ed, const char* text, int length, int mode) {
	uint32_t total = 1 + ed->caret_count, i, k;
	rvn_ed_caret_edit_t* edits = (rvn_ed_caret_edit_t*)rvn_alloc(sizeof(rvn_ed_caret_edit_t) * total);
	int* after = (int*)rvn_alloc(sizeof(int) * total);

	for (i = 0; i < total; i++) {
		rvn_ed_pos_t caret = i == 0 ? ed->caret : ed->carets[i - 1].caret;
		rvn_ed_pos_t anchor = i == 0 ? ed->anchor : ed->carets[i - 1].anchor;
		rvn_ed_pos_t from = rvn_ed_pos_cmp(anchor, caret) <= 0 ? anchor : caret;
		rvn_ed_pos_t to = rvn_ed_pos_cmp(anchor, caret) <= 0 ? caret : anchor;
		rvn_ed_caret_edit_t* edit = &edits[i];
		if (mode == 1 && rvn_ed_pos_cmp(from, to) == 0) from = rvn_ed_char_left(ed, from);
		if (mode == 2 && rvn_ed_pos_cmp(from, to) == 0) to = rvn_ed_char_right(ed, to);
		edit->from = rvn_ed_offset(ed, from);
		edit->to = rvn_ed_offset(ed, to);
		edit->index = (int)i;
		after[i] = edit->from;
	}
	qsort(edits, total, sizeof(rvn_ed_caret_edit_t), rvn_ed_caret_edit_compare);

	/* (every caret is typed at once: one step of the history, not one per caret) */
	rvn_ed_group_begin(ed);
	for (i = 0; i < total; i++) {
		rvn_ed_caret_edit_t* edit = &edits[i];
		uint32_t slot = (uint32_t)edit->index;
		int delta;
		/* (two carets on the same place, or one inside the selection of another, are edited once) */
		if (i > 0) {
			const rvn_ed_caret_edit_t* previous = &edits[i - 1];
			if (edit->from == previous->from && edit->to == previous->to) continue;
			if (edit->to > previous->from) continue;
		}
		/* (where the edit ended, as an offset in the text as it is now) */
		after[slot] = rvn_ed_offset(ed, rvn_ed_replace(ed, rvn_ed_pos_of(ed, edit->from), rvn_ed_pos_of(ed, edit->to),
			text, length, R_TRUE));
		delta = length - (edit->to - edit->from);
		/* (every other caret that lies after this edit moves with what it added or removed:
		   the ones still to be edited, which come before it in the text, and the ones already
		   edited, which come after it) */
		for (k = 0; k < total; k++) {
			uint32_t other = (uint32_t)edits[k].index;
			if (other == slot) continue;
			if (after[other] > edit->from) after[other] += delta;
		}
	}
	rvn_ed_group_end(ed);

	{
		rvn_ed_pos_t caret = rvn_ed_pos_of(ed, after[0]);
		ed->caret = ed->anchor = caret;
		for (k = 0; k < ed->caret_count; k++) {
			rvn_ed_pos_t place = rvn_ed_pos_of(ed, after[k + 1]);
			ed->carets[k].caret = ed->carets[k].anchor = place;
		}
	}
	/* (a caret that came to rest on another one is one caret) */
	for (k = 0; k < ed->caret_count; ) {
		uint32_t other;
		int same = R_FALSE;
		if (rvn_ed_pos_cmp(ed->carets[k].caret, ed->caret) == 0) same = R_TRUE;
		for (other = 0; !same && other < ed->caret_count; other++) {
			if (other != k && rvn_ed_pos_cmp(ed->carets[k].caret, ed->carets[other].caret) == 0) same = R_TRUE;
		}
		if (same) ed->carets[k] = ed->carets[--ed->caret_count];
		else k++;
	}
	rvn_free(edits);
	rvn_free(after);
	rvn_ed_caret_moved(ed, R_FALSE);
}

static int rvn_ed_delete_selection(rvn_text_editor_t* ed) {
	if (!rvn_ed_has_selection(ed)) return R_FALSE;
	rvn_ed_insert(ed, "", 0);
	return R_TRUE;
}

/* Tab and Shift+Tab over a selection of several lines: one level in or out for each of them. */
static void rvn_ed_indent_lines(rvn_text_editor_t* ed, int outdent) {
	rvn_ed_pos_t from = rvn_ed_sel_min(ed), to = rvn_ed_sel_max(ed);
	int first = from.line, last = to.line, line, length = 0, cap = 0;
	char* out = NULL;
	char unit[17];
	int unit_length = 1;

	/* a selection that ends at the start of a line does not include that line */
	if (last > first && to.column == 0) last--;
	unit[0] = '\t';
	if (ed->tab_spaces) {
		unit_length = rvn_clampi(ed->tab_size, 1, 16);
		memset(unit, ' ', (size_t)unit_length);
	}
	for (line = first; line <= last; line++) {
		const rvn_ed_line_t* l = &ed->lines[line];
		int skip = 0, add = 0;
		if (outdent) {
			if (l->length > 0 && l->text[0] == '\t') skip = 1;
			else while (skip < l->length && skip < ed->tab_size && l->text[skip] == ' ') skip++;
		} else if (l->length > 0) {
			add = unit_length;
		}
		if (length + add + (l->length - skip) + 2 > cap) {
			cap = (length + add + l->length + 2) * 2;
			out = (char*)rvn_realloc(out, (size_t)cap);
		}
		memcpy(out + length, unit, (size_t)add);
		length += add;
		memcpy(out + length, l->text + skip, (size_t)(l->length - skip));
		length += l->length - skip;
		if (line < last) out[length++] = '\n';
	}
	rvn_ed_break_typing(ed);
	rvn_ed_replace(ed, rvn_ed_pos(first, 0), rvn_ed_pos(last, ed->lines[last].length), out ? out : "", length, R_TRUE);
	rvn_ed_break_typing(ed);
	rvn_free(out);
	/* the lines stay selected, to indent again */
	ed->anchor = rvn_ed_pos(first, 0);
	ed->caret = last + 1 < (int)ed->line_count && to.column == 0 && to.line > first ? rvn_ed_pos(last + 1, 0)
		: rvn_ed_pos(last, ed->lines[last].length);
}

/* ---------------------------------------------------------------------------
 *  Help while typing: indentation, pairs, comments
 * ------------------------------------------------------------------------- */

/* One level of indentation, as the Tab key writes it. Returns its length. */
static int rvn_ed_indent_unit(const rvn_text_editor_t* ed, char unit[17]) {
	int length = 1;
	unit[0] = '\t';
	if (ed->tab_spaces) {
		length = rvn_clampi(ed->tab_size, 1, 16);
		memset(unit, ' ', (size_t)length);
	}
	unit[length] = '\0';
	return length;
}

static int rvn_ed_is_blank(char c) {
	return c == ' ' || c == '\t';
}

/* The character that closes an opening bracket or quote, or 0. */
static char rvn_ed_closer(char c) {
	switch (c) {
		case '(': return ')';
		case '[': return ']';
		case '{': return '}';
		case '"': return '"';
		case '\'': return '\'';
		case '`': return '`';
		default: return 0;
	}
}

/* R_TRUE when the word that ends at `end` of a line is begin, with its backslash before it. */
static int rvn_ed_ends_with_begin(const rvn_ed_line_t* line, int end) {
	int k = end;
	while (k > 0 && ((line->text[k - 1] >= 'a' && line->text[k - 1] <= 'z') ||
		(line->text[k - 1] >= 'A' && line->text[k - 1] <= 'Z'))) k--;
	if (end - k != 5 || memcmp(line->text + k, "begin", 5) != 0) return R_FALSE;
	return k > 0 && line->text[k - 1] == '\\';
}

/*
 * Enter: the new line starts with the indentation of the one it comes from,
 * one level deeper after an opening bracket (or a colon in Python, or an
 * opening tag). Between a bracket and the one that closes it, the closing
 * one goes down to a line of its own.
 */
static void rvn_ed_new_line(rvn_text_editor_t* ed) {
	rvn_ed_pos_t from = rvn_ed_sel_min(ed);
	const rvn_ed_line_t* line = &ed->lines[from.line];
	const RLanguageDesc* desc = ed->no_pairs ? NULL : rvn_ed_language_desc(ed);
	char buffer[320], unit[17];
	int indent = 0, length = 0, unit_length = rvn_ed_indent_unit(ed, unit);
	int deeper = R_FALSE, split = R_FALSE;

	while (indent < line->length && indent < from.column && indent < 128 && rvn_ed_is_blank(line->text[indent])) indent++;
	if (desc && !rvn_ed_has_selection(ed)) {
		int last = from.column;
		char before, after = from.column < line->length ? line->text[from.column] : '\0';
		while (last > 0 && rvn_ed_is_blank(line->text[last - 1])) last--;
		before = last > 0 ? line->text[last - 1] : '\0';
		if (before == '{' || before == '[' || before == '(') {
			deeper = R_TRUE;
			split = after != '\0' && after == rvn_ed_closer(before);
		} else if (before == ':' && (desc->flags & R_LANGUAGE_INDENT_ON_COLON)) {
			deeper = R_TRUE;
		} else if (before == '}' && (desc->flags & R_LANGUAGE_TEX)) {
			/* \begin{...}: the next line is inside the environment */
			int open = last - 2;
			while (open > 0 && line->text[open] != '{') open--;
			if (line->text[open] == '{' && rvn_ed_ends_with_begin(line, open)) deeper = R_TRUE;
		} else if (before == '>' && (desc->flags & R_LANGUAGE_MARKUP) && last >= 2 && line->text[last - 2] != '/') {
			/* after an opening tag, not after a closing one */
			int open = last - 2;
			while (open > 0 && line->text[open] != '<') open--;
			if (line->text[open] == '<' && line->text[open + 1] != '/' && line->text[open + 1] != '!' && line->text[open + 1] != '?') {
				deeper = R_TRUE;
				split = after == '<' && from.column + 1 < line->length && line->text[from.column + 1] == '/';
			}
		}
	}
	buffer[length++] = '\n';
	memcpy(buffer + length, line->text, (size_t)indent);
	length += indent;
	if (deeper) {
		memcpy(buffer + length, unit, (size_t)unit_length);
		length += unit_length;
	}
	if (split) {
		buffer[length++] = '\n';
		memcpy(buffer + length, line->text, (size_t)indent);
		length += indent;
	}
	rvn_ed_break_typing(ed);
	rvn_ed_insert(ed, buffer, length);
	rvn_ed_break_typing(ed);
	if (split) ed->caret = ed->anchor = rvn_ed_pos(from.line + 1, indent + unit_length);
}

/* Is a quote character one that the language writes its texts with? */
static int rvn_ed_is_quote(const RLanguageDesc* desc, char c) {
	if (c != '"' && c != '\'' && c != '`') return R_FALSE;
	if (desc->flags & R_LANGUAGE_MARKUP) return c != '`';
	/* (an apostrophe is rarely the start of a text where it holds a single character) */
	if (c == '\'' && (desc->flags & R_LANGUAGE_CHAR_LITERALS)) return R_FALSE;
	if (desc->string_quotes && strchr(desc->string_quotes, c)) return R_TRUE;
	/* or one of the marks of its texts that span lines, when it is this character alone (a backquote) */
	if (desc->multiline_strings) {
		const char* mark = desc->multiline_strings;
		while (*mark) {
			if (mark[0] == c && (mark == desc->multiline_strings || mark[-1] == ' ') && (mark[1] == ' ' || mark[1] == '\0')) return R_TRUE;
			mark++;
		}
	}
	return R_FALSE;
}

/* Elements of HTML that have no closing tag. */
static int rvn_ed_is_void_tag(const char* name, int length) {
	static const char* const tags[] = { "br", "hr", "img", "input", "meta", "link", "area", "base", "col", "embed", "source", "track", "wbr" };
	size_t i;
	for (i = 0; i < RVN_COUNT_OF(tags); i++) {
		if ((int)strlen(tags[i]) == length && rvn_strnicmp(tags[i], name, (size_t)length) == 0) return R_TRUE;
	}
	return R_FALSE;
}

/* A character typed by the user: with a language, brackets and quotes come in pairs. */
static void rvn_ed_type(rvn_text_editor_t* ed, uint32_t codepoint) {
	const RLanguageDesc* desc = ed->no_pairs ? NULL : rvn_ed_language_desc(ed);
	const rvn_ed_line_t* line = &ed->lines[ed->caret.line];
	int selection = rvn_ed_has_selection(ed);
	char encoded[4];
	int length = rvn_utf8_encode(codepoint, encoded);
	char c = codepoint < 128 ? (char)codepoint : '\0';
	char next = (!selection && ed->caret.column < line->length) ? line->text[ed->caret.column] : '\0';
	char previous = (!selection && ed->caret.column > 0) ? line->text[ed->caret.column - 1] : '\0';
	char closer = rvn_ed_closer(c);
	int is_quote = desc && rvn_ed_is_quote(desc, c);
	int room = next == '\0' || rvn_ed_is_blank(next) || strchr(")]}>,;", next) != NULL;

	if (!desc || c == '\0') {
		rvn_ed_insert(ed, encoded, length);
		return;
	}
	/* the closing character is already there (the one the editor added): step over it */
	if (!selection && next == c && (c == ')' || c == ']' || c == '}' || is_quote)) {
		if (c == '}' && (desc->flags & R_LANGUAGE_TEX)) {
			int open = ed->caret.column - 1;
			while (open >= 0 && line->text[open] != '{' && line->text[open] != '\\') open--;
			if (open >= 0 && line->text[open] == '{' && rvn_ed_ends_with_begin(line, open)) {
				int name = open + 1, indent = 0, name_length = ed->caret.column - name;
				char buffer[256], unit[17];
				int unit_length = rvn_ed_indent_unit(ed, unit);
				while (indent < line->length && rvn_ed_is_blank(line->text[indent])) indent++;
				if (name_length > 0 && name_length < 200 && indent + unit_length < 100) {
					int written = snprintf(buffer, sizeof(buffer), "\n%.*s%.*s\n%.*s\\end{%.*s}",
						indent, line->text, unit_length, unit, rvn_mini(indent, 100), line->text, name_length, line->text + name);
					if (written > 0 && written < (int)sizeof(buffer)) {
						/* (the caret is before the brace the editor added: it goes after it on its own line) */
						rvn_ed_pos_t after = rvn_ed_pos(ed->caret.line, ed->caret.column + 1);
						rvn_ed_break_typing(ed);
						rvn_ed_replace(ed, after, after, buffer, written, R_TRUE);
						rvn_ed_break_typing(ed);
						ed->caret = ed->anchor = rvn_ed_pos(after.line + 1, indent + unit_length);
						return;
					}
				}
			}
		}
		ed->caret = ed->anchor = rvn_ed_pos(ed->caret.line, ed->caret.column + 1);
		return;
	}
	if (closer && (is_quote || c == '(' || c == '[' || c == '{')) {
		if (selection) {
			/* around what is selected */
			char* inner = rvn_ed_range(ed, rvn_ed_sel_min(ed), rvn_ed_sel_max(ed), NULL);
			size_t inner_length = strlen(inner);
			char* wrapped = (char*)rvn_alloc(inner_length + 3);
			wrapped[0] = c;
			memcpy(wrapped + 1, inner, inner_length);
			wrapped[inner_length + 1] = closer;
			wrapped[inner_length + 2] = '\0';
			rvn_ed_break_typing(ed);
			rvn_ed_insert(ed, wrapped, (int)inner_length + 2);
			rvn_ed_break_typing(ed);
			rvn_free(wrapped);
			rvn_free(inner);
			return;
		}
		/* in a comment or in a text, a bracket is only a character */
		if (room && rvn_ed_is_code(ed, ed->caret.line, rvn_maxi(ed->caret.column - 1, 0)) &&
			(!is_quote || (rvn_ed_class(previous) != RVN_ED_WORD && previous != c && previous != '\\'))) {
			char pair[2];
			pair[0] = c;
			pair[1] = closer;
			rvn_ed_break_typing(ed);
			rvn_ed_insert(ed, pair, 2);
			rvn_ed_break_typing(ed);
			ed->caret = ed->anchor = rvn_ed_pos(ed->caret.line, ed->caret.column - 1);
			return;
		}
	}
	if (c == '}' && !selection && ed->caret.column > 0) {
		/* alone on its line, a closing brace comes back one level */
		int column = ed->caret.column, cut = 0;
		int blank = R_TRUE, i;
		for (i = 0; i < column && blank; i++) blank = rvn_ed_is_blank(line->text[i]);
		if (blank) {
			if (line->text[column - 1] == '\t') cut = 1;
			else while (cut < ed->tab_size && cut < column && line->text[column - 1 - cut] == ' ') cut++;
			rvn_ed_break_typing(ed);
			ed->caret = ed->anchor = rvn_ed_replace(ed, rvn_ed_pos(ed->caret.line, column - cut), ed->caret, "}", 1, R_TRUE);
			rvn_ed_break_typing(ed);
			return;
		}
		/* \begin{name} typed with its closing brace: the line of what is inside, and \end{name} below */
		if (desc->flags & R_LANGUAGE_TEX) {
			int open = ed->caret.column - 1;
			while (open >= 0 && line->text[open] != '{' && line->text[open] != '\\') open--;
			if (open >= 0 && line->text[open] == '{' && rvn_ed_ends_with_begin(line, open)) {
				int name = open + 1;
				if (name < ed->caret.column) {
					char buffer[256], unit[17];
					int indent = 0, name_length = ed->caret.column - name;
					int unit_length = rvn_ed_indent_unit(ed, unit);
					while (indent < line->length && rvn_ed_is_blank(line->text[indent])) indent++;
					if (name_length > 0 && name_length < 200 && indent + unit_length < 100) {
						int written = snprintf(buffer, sizeof(buffer), "}\n%.*s%.*s\n%.*s\\end{%.*s}",
							indent, line->text, unit_length, unit, rvn_mini(indent, 100), line->text, name_length, line->text + name);
						if (written > 0 && written < (int)sizeof(buffer)) {
							rvn_ed_break_typing(ed);
							rvn_ed_replace(ed, ed->caret, ed->caret, buffer, written, R_TRUE);
							rvn_ed_break_typing(ed);
							ed->caret = ed->anchor = rvn_ed_pos(ed->caret.line + 1, indent + unit_length);
							return;
						}
					}
				}
			}
		}
	}
	if (c == '>' && !selection && (desc->flags & R_LANGUAGE_MARKUP) && previous != '/' && previous != '-' && previous != '?') {
		/* the end of an opening tag: its closing tag comes with it */
		int open = ed->caret.column - 1, name_end;
		while (open >= 0 && line->text[open] != '<' && line->text[open] != '>') open--;
		if (open >= 0 && line->text[open] == '<' && open + 1 < ed->caret.column && rvn_ed_class(line->text[open + 1]) == RVN_ED_WORD) {
			name_end = open + 1;
			while (name_end < ed->caret.column && !rvn_ed_is_blank(line->text[name_end])) name_end++;
			if (!((desc->flags & R_LANGUAGE_EMBEDS) && rvn_ed_is_void_tag(line->text + open + 1, name_end - open - 1)) && name_end - open < 80) {
				char closing[96];
				int closing_length = snprintf(closing, sizeof(closing), "></%.*s>", name_end - open - 1, line->text + open + 1);
				rvn_ed_pos_t caret = ed->caret;
				rvn_ed_break_typing(ed);
				rvn_ed_insert(ed, closing, closing_length);
				rvn_ed_break_typing(ed);
				ed->caret = ed->anchor = rvn_ed_pos(caret.line, caret.column + 1);
				return;
			}
		}
	}
	rvn_ed_insert(ed, encoded, length);
}

/* Ctrl+/: comments the selected lines out, or back in when they all are. Returns R_FALSE when the language has no such comment. */
static int rvn_ed_toggle_comment(rvn_text_editor_t* ed) {
	const RLanguageDesc* desc = rvn_ed_language_desc(ed);
	rvn_ed_pos_t from = rvn_ed_sel_min(ed), to = rvn_ed_sel_max(ed);
	int first = from.line, last = to.line, line, mark_length, min_indent = 1 << 20, all = R_TRUE, any = R_FALSE;
	int length = 0, cap = 0;
	char* out = NULL;

	if (!desc || !desc->line_comment) return R_FALSE;
	mark_length = (int)strlen(desc->line_comment);
	if (last > first && to.column == 0) last--;
	for (line = first; line <= last; line++) {
		const rvn_ed_line_t* l = &ed->lines[line];
		int indent = 0;
		while (indent < l->length && rvn_ed_is_blank(l->text[indent])) indent++;
		if (indent == l->length) continue;
		any = R_TRUE;
		if (indent < min_indent) min_indent = indent;
		if (l->length - indent < mark_length || memcmp(l->text + indent, desc->line_comment, (size_t)mark_length) != 0) all = R_FALSE;
	}
	if (!any) return R_TRUE;
	for (line = first; line <= last; line++) {
		const rvn_ed_line_t* l = &ed->lines[line];
		int indent = 0;
		while (indent < l->length && rvn_ed_is_blank(l->text[indent])) indent++;
		if (length + l->length + mark_length + 4 > cap) {
			cap = (length + l->length + mark_length + 4) * 2;
			out = (char*)rvn_realloc(out, (size_t)cap);
		}
		if (indent == l->length) {
			memcpy(out + length, l->text, (size_t)l->length);
			length += l->length;
		} else if (all) {
			int skip = indent + mark_length;
			if (skip < l->length && l->text[skip] == ' ') skip++;
			memcpy(out + length, l->text, (size_t)indent);
			length += indent;
			memcpy(out + length, l->text + skip, (size_t)(l->length - skip));
			length += l->length - skip;
		} else {
			/* the marks line up, at the indentation of the least indented line */
			memcpy(out + length, l->text, (size_t)min_indent);
			length += min_indent;
			memcpy(out + length, desc->line_comment, (size_t)mark_length);
			length += mark_length;
			out[length++] = ' ';
			memcpy(out + length, l->text + min_indent, (size_t)(l->length - min_indent));
			length += l->length - min_indent;
		}
		if (line < last) out[length++] = '\n';
	}
	rvn_ed_break_typing(ed);
	rvn_ed_replace(ed, rvn_ed_pos(first, 0), rvn_ed_pos(last, ed->lines[last].length), out ? out : "", length, R_TRUE);
	rvn_ed_break_typing(ed);
	rvn_free(out);
	if (first == last && rvn_ed_pos_cmp(from, to) == 0) {
		ed->caret = ed->anchor = rvn_ed_clamp(ed, rvn_ed_pos(first, from.column + (all ? -(mark_length + 1) : mark_length + 1)));
	} else {
		ed->anchor = rvn_ed_pos(first, 0);
		ed->caret = rvn_ed_pos(last, ed->lines[last].length);
	}
	return R_TRUE;
}

/* ---------------------------------------------------------------------------
 *  Folding
 *
 *  A block is a pair of positions in the text whose lines can be collapsed:
 *  { ... } and [ ... ] in the languages that use them, \begin{...} with its
 *  \end{...}, <tag> with </tag>, and a Markdown title with what is under it.
 *  The folds are found again when the text changes; the folded ones are
 *  remembered by the line they start on.
 * ------------------------------------------------------------------------- */

/** What opens and closes a block, as offsets in a line. */
typedef struct rvn_ed_pair_t {
	int open_line;
	int open_column;
	int close_line;
	int close_column;
} rvn_ed_pair_t;

/* The word of a \command at `at` of a line ("begin", "end"): its length, or 0. */
static int rvn_ed_command_at(const rvn_ed_line_t* line, int at, char* name, int size) {
	int i = at + 1, length = 0;
	while (i < line->length && length + 1 < size &&
		((line->text[i] >= 'a' && line->text[i] <= 'z') || (line->text[i] >= 'A' && line->text[i] <= 'Z') || line->text[i] == '*')) {
		name[length++] = line->text[i++];
	}
	name[length] = '\0';
	return length;
}

/* The braces of a \begin{...} at `at`: the opening and closing braces of its argument, or -1. */
static void rvn_ed_tex_argument(const rvn_ed_line_t* line, int at, int* begin, int* end) {
	int i = at;
	*begin = *end = -1;
	while (i < line->length && line->text[i] != '{' && line->text[i] != '\\') i++;
	if (i >= line->length || line->text[i] != '{') return;
	*begin = i;
	while (i < line->length && line->text[i] != '}') i++;
	if (i >= line->length) return;
	*end = i;
}

/* One block that was opened and not closed yet, while the pairs are looked for. */
typedef struct rvn_ed_open_t {
	int line;
	int column;
	char* name;
} rvn_ed_open_t;

/** The blocks opened and not closed yet, as the pairs are looked for. */
typedef struct rvn_ed_stack_t {
	rvn_ed_open_t* items;
	uint32_t count;
	uint32_t cap;
} rvn_ed_stack_t;

/** The blocks found, and the place for them. */
typedef struct rvn_ed_pair_list_t {
	rvn_ed_pair_t* items;
	uint32_t count;
	uint32_t cap;
} rvn_ed_pair_list_t;

static void rvn_ed_stack_push(rvn_ed_stack_t* stack, int line, int column, const char* name, int name_length) {
	rvn_ed_open_t* item;
	RVN_ARRAY_RESERVE(stack->items, stack->cap, stack->count + 1);
	item = &stack->items[stack->count++];
	item->line = line;
	item->column = column;
	item->name = rvn_strndup(name, (size_t)name_length);
}

static void rvn_ed_stack_drop(rvn_ed_stack_t* stack, uint32_t index) {
	rvn_free(stack->items[index].name);
	memmove(stack->items + index, stack->items + index + 1, sizeof(rvn_ed_open_t) * (size_t)(stack->count - index - 1));
	stack->count--;
}

static void rvn_ed_stack_free(rvn_ed_stack_t* stack) {
	uint32_t i;
	for (i = 0; i < stack->count; i++) rvn_free(stack->items[i].name);
	rvn_free(stack->items);
}

/* Closes the innermost block whose name is `name`, and keeps the pair when it spans more than one line. */
static void rvn_ed_stack_close(rvn_ed_stack_t* stack, rvn_ed_pair_list_t* pairs, const char* name, int name_length,
	int close_line, int close_column, int ignore_case) {
	uint32_t k;
	for (k = stack->count; k > 0; k--) {
		const char* opened = stack->items[k - 1].name;
		int length = (int)strlen(opened);
		int same = length == name_length && (ignore_case ? rvn_strnicmp(opened, name, (size_t)length) == 0
			: memcmp(opened, name, (size_t)length) == 0);
		rvn_ed_pair_t* pair;
		if (!same) continue;
		if (stack->items[k - 1].line < close_line) {
			RVN_ARRAY_RESERVE(pairs->items, pairs->cap, pairs->count + 1);
			pair = &pairs->items[pairs->count++];
			pair->open_line = stack->items[k - 1].line;
			pair->open_column = stack->items[k - 1].column;
			pair->close_line = close_line;
			pair->close_column = close_column;
		}
		rvn_ed_stack_drop(stack, k - 1);
		return;
	}
}

/*
 * Finds the blocks of the text: the pairs of braces, of \begin and \end, of
 * tags, and the titles of Markdown with what is under them.
 */
static void rvn_ed_find_pairs(rvn_text_editor_t* ed, rvn_ed_pair_t** pairs_out, uint32_t* count_out) {
	const rvn_language_t* language = rvn_ed_language(ed);
	const RLanguageDesc* desc = language ? rvn_language_desc(language) : NULL;
	rvn_ed_pair_list_t pairs;
	rvn_ed_stack_t stack;
	int tex = desc && (desc->flags & R_LANGUAGE_TEX);
	int markup = desc && (desc->flags & R_LANGUAGE_MARKUP);
	int formatted = desc && (desc->flags & R_LANGUAGE_FORMATTED_TEXT);
	int braces = desc && !tex && !markup && !(desc->flags & R_LANGUAGE_BIBTEX) && !formatted;
	int line;

	memset(&pairs, 0, sizeof(pairs));
	memset(&stack, 0, sizeof(stack));
	*pairs_out = NULL;
	*count_out = 0;
	if (!desc || (desc->flags & R_LANGUAGE_STYLE_SHEET)) return;

	for (line = 0; line < (int)ed->line_count; line++) {
		const rvn_ed_line_t* l = &ed->lines[line];
		int i;
		if (tex) {
			/* \begin{name} ... \end{name}, counted by name */
			for (i = 0; i + 1 < l->length; i++) {
				char name[48];
				int length, begin, end;
				if (l->text[i] != '\\') continue;
				length = rvn_ed_command_at(l, i, name, sizeof(name));
				if (length == 0) continue;
				if (strcmp(name, "begin") == 0) {
					rvn_ed_tex_argument(l, i + length, &begin, &end);
					if (begin < 0) continue;
					rvn_ed_stack_push(&stack, line, i, l->text + begin + 1, end - begin - 1);
					i = end;
					continue;
				}
				if (strcmp(name, "end") == 0) {
					rvn_ed_tex_argument(l, i + length, &begin, &end);
					if (begin < 0) continue;
					rvn_ed_stack_close(&stack, &pairs, l->text + begin + 1, end - begin - 1, line, i, R_FALSE);
					i = end;
					continue;
				}
				i += length - 1;
			}
			continue;
		}
		if (formatted) {
			/* a title, with what is under it until the next title of the same level or a higher one */
			int level = 0, next;
			i = 0;
			while (i < l->length && l->text[i] == '#') level++, i++;
			if (level == 0 || level > 6 || (i < l->length && l->text[i] != ' ')) continue;
			for (next = line + 1; next < (int)ed->line_count; next++) {
				const rvn_ed_line_t* n = &ed->lines[next];
				int k = 0, other = 0;
				while (k < n->length && n->text[k] == '#') k++, other++;
				if (other > 0 && other <= level && (k >= n->length || n->text[k] == ' ')) break;
			}
			if (next - 1 > line) {
				rvn_ed_pair_t* pair;
				RVN_ARRAY_RESERVE(pairs.items, pairs.cap, pairs.count + 1);
				pair = &pairs.items[pairs.count++];
				pair->open_line = line;
				pair->open_column = 0;
				pair->close_line = next - 1;
				pair->close_column = ed->lines[next - 1].length;
			}
			continue;
		}
		if (markup) {
			/* a tag and the one that closes it, by name */
			for (i = 0; i < l->length; i++) {
				int name_at, name_end, closing;
				if (l->text[i] != '<' || i + 1 >= l->length) continue;
				closing = l->text[i + 1] == '/';
				name_at = i + 1 + (closing ? 1 : 0);
				name_end = name_at;
				while (name_end < l->length && (rvn_ed_is_word_char(l->text[name_end]) || l->text[name_end] == '-' ||
					l->text[name_end] == ':' || l->text[name_end] == '.')) name_end++;
				if (name_end == name_at) continue;
				if (closing) {
					rvn_ed_stack_close(&stack, &pairs, l->text + name_at, name_end - name_at, line, i, R_TRUE);
				} else {
					/* (a tag that closes itself, or that has no '>' on its line, is no block) */
					int end_tag = name_end, k;
					while (end_tag < l->length && l->text[end_tag] != '>') end_tag++;
					if (end_tag >= l->length) continue;
					for (k = end_tag; k > i && (l->text[k] == ' ' || l->text[k] == '\t' || l->text[k] == '>'); k--) {}
					if (k > i && l->text[k] == '/' && k == end_tag - 1) {
						i = name_end - 1;
						continue;
					}
					rvn_ed_stack_push(&stack, line, i, l->text + name_at, name_end - name_at);
				}
				i = name_end - 1;
			}
			continue;
		}
		if (braces) {
			/* the braces, brackets and parentheses of a language of code */
			int inside_string = 0;
			char quote = 0;
			for (i = 0; i < l->length; i++) {
				char c = l->text[i];
				if (inside_string) {
					if (c == '\\' && i + 1 < l->length) i++;
					else if (c == quote) inside_string = 0;
					continue;
				}
				if (c == '"' || c == '\'') {
					inside_string = 1;
					quote = c;
					continue;
				}
				if (c == '{' || c == '[' || c == '(') {
					rvn_ed_stack_push(&stack, line, i, &c, 1);
				} else if (c == '}' || c == ']' || c == ')') {
					char open = c == '}' ? '{' : c == ']' ? '[' : '(';
					rvn_ed_stack_close(&stack, &pairs, &open, 1, line, i, R_FALSE);
				}
			}
		}
	}
	rvn_ed_stack_free(&stack);
	*pairs_out = pairs.items;
	*count_out = pairs.count;
}

/* Finds the blocks again, keeping the ones that were folded (by their first line). */
static void rvn_ed_update_folds(rvn_text_editor_t* ed) {
	rvn_ed_pair_t* pairs = NULL;
	uint32_t count = 0, i;
	rvn_ed_fold_t* folds;
	uint32_t fold_count = 0;

	if (ed->no_folding) return;
	if (ed->fold_version == ed->version && ed->folds) return;
	ed->fold_version = ed->version;
	rvn_ed_find_pairs(ed, &pairs, &count);
	folds = count ? (rvn_ed_fold_t*)rvn_calloc(sizeof(rvn_ed_fold_t) * count) : NULL;
	for (i = 0; i < count; i++) {
		uint32_t k;
		int folded = R_FALSE;
		/* the same block as before, if it started on the same line and ended on the same one */
		for (k = 0; k < ed->fold_count; k++) {
			if (ed->folds[k].folded && ed->folds[k].start == pairs[i].open_line && ed->folds[k].end == pairs[i].close_line) {
				folded = R_TRUE;
				break;
			}
		}
		folds[fold_count].start = pairs[i].open_line;
		folds[fold_count].end = pairs[i].close_line;
		folds[fold_count].folded = folded;
		fold_count++;
	}
	rvn_free(pairs);
	rvn_free(ed->folds);
	ed->folds = folds;
	ed->fold_count = fold_count;
	ed->fold_cap = count;
	ed->rows_dirty = R_TRUE;
}

/* The folds must be found again before the rows are laid out. */
static void rvn_ed_folds_refresh(rvn_text_editor_t* ed) {
	if (ed->no_folding) return;
	if (ed->fold_version != ed->version || !ed->folds) rvn_ed_update_folds(ed);
}

/* The block of the caret to fold: the innermost one that starts on its line, or that holds it. */
static int rvn_ed_fold_at_caret(rvn_text_editor_t* ed) {
	int line = ed->caret.line, i, best = -1;
	rvn_ed_update_folds(ed);
	for (i = 0; i < (int)ed->fold_count; i++) {
		if (ed->folds[i].start == line) return i;
		if (ed->folds[i].start <= line && ed->folds[i].end >= line) {
			/* (the innermost: the one that starts last) */
			if (best < 0 || ed->folds[i].start > ed->folds[best].start) best = i;
		}
	}
	return best;
}

static int rvn_ed_fold_line_at(const rvn_text_editor_t* ed, int line) {
	int i;
	for (i = 0; i < (int)ed->fold_count; i++) {
		if (ed->folds[i].start == line) return i;
	}
	return -1;
}

/* ---------------------------------------------------------------------------
 *  Search
 *
 *  Every match of the text, computed once per (text, pattern) pair. A plain
 *  pattern is looked for as bytes; a regular expression goes through the
 *  small engine of regex.c. The places of a match are (line, column), and
 *  the list is what Replace and Replace All walk.
 * ------------------------------------------------------------------------- */

static void rvn_ed_search_clear(rvn_text_editor_t* ed) {
	rvn_free(ed->matches);
	ed->matches = NULL;
	ed->match_count = 0;
	ed->match_cap = 0;
	ed->search_version = 0;
	rvn_free(ed->match_pattern);
	ed->match_pattern = NULL;
	ed->match_flags = 0;
}

static int rvn_ed_is_word_char(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$' || (unsigned char)c >= 0x80;
}

/* A byte of a name in the language of the editor: TeX commands start with a backslash, which is part of them. */
static int rvn_ed_word_char(rvn_text_editor_t* ed, char c) {
	const rvn_language_t* language = rvn_ed_language(ed);
	if (language) return rvn_language_is_word_char(language, c);
	return rvn_ed_is_word_char(c);
}


/* R_TRUE when the byte at `at` of a line is a word character, for whole words. */
static int rvn_ed_is_word_byte(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || (unsigned char)c >= 0x80;
}

static void rvn_ed_match_add(rvn_text_editor_t* ed, rvn_ed_pos_t from, rvn_ed_pos_t to) {
	RVN_ARRAY_RESERVE(ed->matches, ed->match_cap, ed->match_count + 1);
	ed->matches[ed->match_count].from = from;
	ed->matches[ed->match_count].to = to;
	ed->match_count++;
}

/* A plain text looked for in the lines of the editor, with the flags of the search. */
static void rvn_ed_search_plain(rvn_text_editor_t* ed, const char* pattern) {
	int length = (int)strlen(pattern), line;
	int ignore_case = (ed->search_flags & R_SEARCH_IGNORE_CASE) != 0;
	int whole = (ed->search_flags & R_SEARCH_WHOLE_WORD) != 0;
	if (length == 0) return;
	for (line = 0; line < (int)ed->line_count; line++) {
		const rvn_ed_line_t* l = &ed->lines[line];
		int i;
		for (i = 0; i + length <= l->length; i++) {
			int equal = ignore_case ? rvn_strnicmp(l->text + i, pattern, (size_t)length) == 0
				: memcmp(l->text + i, pattern, (size_t)length) == 0;
			if (!equal) continue;
			if (whole) {
				if (i > 0 && rvn_ed_is_word_byte(l->text[i - 1])) continue;
				if (i + length < l->length && rvn_ed_is_word_byte(l->text[i + length])) continue;
			}
			rvn_ed_match_add(ed, rvn_ed_pos(line, i), rvn_ed_pos(line, i + length));
			i += length - 1;
		}
	}
}

/*
 * A regular expression looked for in the text, which is joined into one
 * block of lines separated by '\n': a match may then span lines, as `^` and
 * `$` in a multi-line pattern need.
 */
static void rvn_ed_search_regex(rvn_text_editor_t* ed, const char* pattern) {
	const char* error = NULL;
	rvn_regex_t* regex = rvn_regex_compile(pattern, (ed->search_flags & R_SEARCH_IGNORE_CASE) != 0, R_TRUE, &error);
	char* text;
	int* starts;
	int length, at = 0, line = 0, i;
	int whole = (ed->search_flags & R_SEARCH_WHOLE_WORD) != 0;
	int groups[64];

	if (!regex) {
		if (error) rvn_error(R_ERROR_INVALID_VALUE, R_SEVERITY_WARNING, "Rtext_editor_set_search: %s", error);
		return;
	}
	if (rvn_regex_group_count(regex) * 2 > (int)(sizeof(groups) / sizeof(groups[0]))) {
		rvn_regex_free(regex);
		return;
	}
	text = rvn_ed_range(ed, rvn_ed_pos(0, 0), rvn_ed_end(ed), &length);
	starts = (int*)rvn_alloc(sizeof(int) * ((size_t)ed->line_count + 1));
	starts[0] = 0;
	for (i = 1; i < (int)ed->line_count; i++) starts[i] = starts[i - 1] + ed->lines[i - 1].length + 1;
	starts[ed->line_count] = length;

	while (at <= length) {
		int found = rvn_regex_search(regex, text, length, at, groups);
		int from_line, to_line, from_column, to_column;
		if (found < 0) break;
		if (groups[1] < groups[0]) break; /* (an empty match at the end: stop) */
		/* the line of an offset */
		while (line + 1 < (int)ed->line_count && starts[line + 1] <= groups[0]) line++;
		from_line = line;
		from_column = groups[0] - starts[line];
		to_line = from_line;
		while (to_line + 1 < (int)ed->line_count && starts[to_line + 1] <= groups[1]) to_line++;
		to_column = groups[1] - starts[to_line];
		/* (a whole word is one whose ends are not next to a letter or a digit: the same rule as the plain search) */
		if (whole && ((groups[0] > 0 && rvn_ed_is_word_byte(text[groups[0] - 1])) ||
			(groups[1] < length && rvn_ed_is_word_byte(text[groups[1]])))) {
			at = groups[1] > groups[0] ? groups[1] : groups[0] + 1;
			continue;
		}
		rvn_ed_match_add(ed, rvn_ed_pos(from_line, from_column), rvn_ed_pos(to_line, to_column));
		at = groups[1] > groups[0] ? groups[1] : groups[0] + 1;
	}
	rvn_free(starts);
	rvn_free(text);
	rvn_regex_free(regex);
}

/* The matches of the current search, computed again when the text or the search changed. */
static void rvn_ed_update_matches(rvn_text_editor_t* ed) {
	int flags = (int)ed->search_flags;
	if (!ed->search_pattern || !ed->search_pattern[0]) {
		rvn_ed_search_clear(ed);
		return;
	}
	if (ed->search_version == ed->version && ed->match_pattern && strcmp(ed->match_pattern, ed->search_pattern) == 0 &&
		ed->match_flags == ed->search_flags) return;
	rvn_free(ed->matches);
	ed->matches = NULL;
	ed->match_count = 0;
	ed->match_cap = 0;
	rvn_str_set(&ed->match_pattern, ed->search_pattern);
	ed->match_flags = ed->search_flags;
	ed->search_version = ed->version;
	if (flags & R_SEARCH_REGEX) rvn_ed_search_regex(ed, ed->search_pattern);
	else rvn_ed_search_plain(ed, ed->search_pattern);
}

/* ---------------------------------------------------------------------------
 *  Snippets
 * ------------------------------------------------------------------------- */

static rvn_ed_snippet_t* g_snippets;
static uint32_t g_snippet_count;
static uint32_t g_snippet_cap;
static rvn_ed_variable_t* g_variables;
static uint32_t g_variable_count;
static uint32_t g_variable_cap;

static void rvn_ed_snippet_free(rvn_ed_snippet_t* snippet) {
	rvn_free(snippet->name);
	rvn_free(snippet->description);
	rvn_free(snippet->prefix);
	rvn_free(snippet->body);
}

/* The value of a $NAME a snippet body names, or NULL. */
static const char* rvn_snippet_variable(const char* name, int length) {
	uint32_t i;
	for (i = 0; i < g_variable_count; i++) {
		if ((int)strlen(g_variables[i].name) == length && memcmp(g_variables[i].name, name, (size_t)length) == 0) {
			return g_variables[i].value;
		}
	}
	return NULL;
}

void rvn_snippets_shutdown(void) {
	uint32_t i;
	for (i = 0; i < g_snippet_count; i++) rvn_ed_snippet_free(&g_snippets[i]);
	rvn_free(g_snippets);
	g_snippets = NULL;
	g_snippet_count = 0;
	g_snippet_cap = 0;
	for (i = 0; i < g_variable_count; i++) {
		rvn_free(g_variables[i].name);
		rvn_free(g_variables[i].value);
	}
	rvn_free(g_variables);
	g_variables = NULL;
	g_variable_count = 0;
	g_variable_cap = 0;
}

int Rsnippet_add(const RSnippet* snippet) {
	uint32_t i;
	RVN_API_GUARD(R_FALSE);
	if (!snippet || !snippet->name || !snippet->name[0] || !snippet->body) {
		rvn_error(R_ERROR_INVALID_VALUE, R_SEVERITY_ERROR, "Rsnippet_add: a snippet needs a name and a body");
		return R_FALSE;
	}
	/* a name may also be given already with its prefix, as it is typed: "\sec" */
	for (i = 0; i < g_snippet_count; i++) {
		if (rvn_stricmp(g_snippets[i].name, snippet->name) != 0) continue;
		rvn_ed_snippet_free(&g_snippets[i]);
		g_snippet_count--;
		memmove(g_snippets + i, g_snippets + i + 1, sizeof(rvn_ed_snippet_t) * (size_t)(g_snippet_count - i));
		break;
	}
	RVN_ARRAY_RESERVE(g_snippets, g_snippet_cap, g_snippet_count + 1);
	memset(&g_snippets[g_snippet_count], 0, sizeof(rvn_ed_snippet_t));
	rvn_str_set(&g_snippets[g_snippet_count].name, snippet->name);
	rvn_str_set(&g_snippets[g_snippet_count].description, snippet->description);
	rvn_str_set(&g_snippets[g_snippet_count].prefix, snippet->prefix);
	rvn_str_set(&g_snippets[g_snippet_count].body, snippet->body);
	g_snippet_count++;
	return R_TRUE;
}

void Rsnippet_clear(void) {
	RVN_API_GUARD_VOID();
	rvn_snippets_shutdown();
}

int Rsnippet_get_count(void) {
	RVN_API_GUARD(0);
	return (int)g_snippet_count;
}

const char* Rsnippet_get_name(int index) {
	RVN_API_GUARD(NULL);
	if (index < 0 || index >= (int)g_snippet_count) return NULL;
	return g_snippets[index].name;
}

int Rsnippet_find(const char* name) {
	int i;
	RVN_API_GUARD(-1);
	if (!name) return -1;
	for (i = 0; i < (int)g_snippet_count; i++) {
		if (rvn_stricmp(g_snippets[i].name, name) == 0) return i;
	}
	return -1;
}

int Rsnippet_set_variable(const char* name, const char* value) {
	uint32_t i;
	RVN_API_GUARD(R_FALSE);
	if (!name || !name[0]) {
		rvn_error(R_ERROR_INVALID_VALUE, R_SEVERITY_ERROR, "Rsnippet_set_variable: a variable needs a name");
		return R_FALSE;
	}
	/* (the value may hold any text: it is typed as it is) */
	for (i = 0; i < g_variable_count; i++) {
		if (rvn_stricmp(g_variables[i].name, name) != 0) continue;
		rvn_str_set(&g_variables[i].value, value && value[0] ? value : NULL);
		if (!g_variables[i].value) {
			rvn_free(g_variables[i].name);
			g_variables[i] = g_variables[--g_variable_count];
		}
		return R_TRUE;
	}
	if (!value || !value[0]) return R_TRUE; /* (removing one that is not there) */
	RVN_ARRAY_RESERVE(g_variables, g_variable_cap, g_variable_count + 1);
	memset(&g_variables[g_variable_count], 0, sizeof(rvn_ed_variable_t));
	rvn_str_set(&g_variables[g_variable_count].name, name);
	rvn_str_set(&g_variables[g_variable_count].value, value);
	g_variable_count++;
	return R_TRUE;
}

/* The snippet whose name is the longest one that ends at a position, or -1. */
static int rvn_ed_snippet_ending_at(const rvn_text_editor_t* ed, rvn_ed_pos_t at, int* length_out) {
	const rvn_ed_line_t* line = &ed->lines[at.line];
	int best = -1, best_length = 0;
	uint32_t i;
	for (i = 0; i < g_snippet_count; i++) {
		int length = (int)strlen(g_snippets[i].name);
		if (length == 0 || length > at.column || length <= best_length) continue;
		if (rvn_strnicmp(line->text + at.column - length, g_snippets[i].name, (size_t)length) != 0) continue;
		best = (int)i;
		best_length = length;
	}
	if (length_out) *length_out = best_length;
	return best;
}

/*
 * The body of a snippet into text: the places ($1..$9, ${1:text}, $0) are
 * remembered, with where they are in the text that is written.
 */
static char* rvn_ed_snippet_body(const char* body, rvn_ed_pos_t at, rvn_ed_place_t** places, uint32_t* place_count,
	rvn_ed_pos_t* end_at, int* has_end) {
	rvn_text_t text;
	int i;
	uint32_t count = 0, cap = 0;
	rvn_ed_place_t* list = NULL;
	rvn_ed_pos_t current = at;

	memset(&text, 0, sizeof(text));
	*places = NULL;
	*place_count = 0;
	*has_end = R_FALSE;
	*end_at = at;
	for (i = 0; body[i]; ) {
		if (body[i] == '\\' && body[i + 1] == 'n') {
			rvn_text_add(&text, "\n");
			current.line++;
			current.column = 0;
			i += 2;
			continue;
		}
		if (body[i] == '\\' && body[i + 1] == 't') {
			rvn_text_add(&text, "\t");
			current.column++;
			i += 2;
			continue;
		}
		if (body[i] == '\\' && body[i + 1] == '$') {
			rvn_text_add(&text, "$");
			current.column++;
			i += 2;
			continue;
		}
		if (body[i] == '$' && body[i + 1] == '$') {
			rvn_text_add(&text, "$");
			current.column++;
			i += 2;
			continue;
		}
		if (body[i] == '$' && body[i + 1] == '{') {
			/* ${n:text}: a place with a text, selected while the caret is on it */
			int number = 0, begin, length;
			rvn_ed_place_t place;
			i += 2;
			while (body[i] >= '0' && body[i] <= '9') {
				number = number * 10 + (body[i] - '0');
				i++;
			}
			if (body[i] == ':') i++;
			begin = i;
			while (body[i] && body[i] != '}') i++;
			length = i - begin;
			place.at = current;
			place.length = length;
			if (number > 0) {
				while ((int)count < number) {
					RVN_ARRAY_RESERVE(list, cap, count + 1);
					memset(&list[count], 0, sizeof(rvn_ed_place_t));
					list[count].at = current;
					count++;
				}
				list[number - 1] = place;
				*place_count = count;
			}
			if (length > 0) rvn_text_append(&text, body + begin, (size_t)length);
			current.column += length;
			if (body[i]) i++;
			continue;
		}
		if (body[i] == '$' && body[i + 1] >= '0' && body[i + 1] <= '9') {
			int number = body[i + 1] - '0';
			i += 2;
			if (number == 0) {
				*end_at = current;
				*has_end = R_TRUE;
				continue;
			}
			if (number > 0) {
				while ((int)count < number) {
					RVN_ARRAY_RESERVE(list, cap, count + 1);
					memset(&list[count], 0, sizeof(rvn_ed_place_t));
					list[count].at = current;
					count++;
				}
				*place_count = count;
			}
			continue;
		}
		/* "$NAME": a variable the application gave (Rsnippet_set_variable); unknown ones are left as typed */
		if (body[i] == '$' && ((body[i + 1] >= 'A' && body[i + 1] <= 'Z') || (body[i + 1] >= 'a' && body[i + 1] <= 'z') ||
			body[i + 1] == '_')) {
			int begin = i + 1, end = begin;
			const char* value = NULL;
			while ((body[end] >= 'A' && body[end] <= 'Z') || (body[end] >= 'a' && body[end] <= 'z') ||
				(body[end] >= '0' && body[end] <= '9') || body[end] == '_') end++;
			value = rvn_snippet_variable(body + begin, end - begin);
			if (value) {
				int length = (int)strlen(value);
				rvn_text_append(&text, value, (size_t)length);
				current.column += length;
				i = end;
				continue;
			}
		}
		rvn_text_append(&text, body + i, 1);
		current.column++;
		i++;
	}
	if (!text.data) text.data = rvn_strdup("");
	*places = list;
	*place_count = count;
	return text.data;
}

/* Alt+drag: a selection on every line the pointer crosses, from the column where it was pressed. */
static void rvn_ed_column_drag(rvn_text_editor_t* ed, rvn_ed_pos_t anchor, rvn_ed_pos_t to) {
	int first = rvn_mini(anchor.line, to.line), last = rvn_maxi(anchor.line, to.line);
	int line;
	ed->caret_count = 0;
	ed->anchor = rvn_ed_pos(anchor.line, rvn_mini(anchor.column, ed->lines[anchor.line].length));
	for (line = first; line <= last; line++) {
		const rvn_ed_line_t* l = &ed->lines[line];
		/* (a line shorter than the column reaches only as far as it goes) */
		int from_column = rvn_mini(anchor.column, l->length);
		int to_column = rvn_mini(to.column, l->length);
		rvn_ed_pos_t from = rvn_ed_pos(line, rvn_mini(from_column, to_column));
		rvn_ed_pos_t stop = rvn_ed_pos(line, rvn_maxi(from_column, to_column));
		if (line == anchor.line) {
			ed->caret = stop;
			continue;
		}
		if (ed->caret_count >= 255) break;
		RVN_ARRAY_RESERVE(ed->carets, ed->caret_cap, ed->caret_count + 1);
		ed->carets[ed->caret_count].anchor = from;
		ed->carets[ed->caret_count].caret = stop;
		ed->caret_count++;
	}
}


static void rvn_ed_snippet_goto_place(rvn_text_editor_t* ed, int index) {
	rvn_ed_place_t* place;
	if (index < 0 || index >= (int)ed->place_count) return;
	place = &ed->places[index];
	ed->caret = rvn_ed_clamp(ed, place->at);
	ed->anchor = rvn_ed_clamp(ed, rvn_ed_pos(place->at.line, place->at.column + (place->length > 0 ? place->length : 0)));
	/* ($0 is where the caret ends: there is nothing to select) */
	ed->place_at = index;
	ed->goal_x = -1.0f;
	rvn_ed_break_typing(ed);
	rvn_ed_caret_moved(ed, R_FALSE);
}

static void rvn_ed_snippet_leave(rvn_text_editor_t* ed) {
	rvn_free(ed->places);
	ed->places = NULL;
	ed->place_count = 0;
	ed->place_at = -1;
	ed->has_place_end = R_FALSE;
}

/* Goes to where a snippet ends ($0), or to the end of its last place, and leaves it. */
static void rvn_ed_snippet_finish(rvn_text_editor_t* ed) {
	if (ed->has_place_end) {
		ed->caret = ed->anchor = rvn_ed_clamp(ed, ed->place_end);
		ed->goal_x = -1.0f;
	}
	rvn_ed_snippet_leave(ed);
	rvn_ed_caret_moved(ed, R_FALSE);
}

/* Tab (and Shift+Tab) inside a snippet: the next place, or the end. */
static int rvn_ed_snippet_walk(rvn_text_editor_t* ed, int backwards) {
	int next;
	if (ed->place_count == 0) {
		rvn_ed_snippet_leave(ed);
		return R_TRUE;
	}
	next = ed->place_at + (backwards ? -1 : 1);
	if (next < 0 || next >= (int)ed->place_count) {
		rvn_ed_snippet_finish(ed);
		return R_TRUE;
	}
	rvn_ed_snippet_goto_place(ed, next);
	return R_TRUE;
}

static int rvn_ed_snippet_insert(rvn_text_editor_t* ed, int which) {
	rvn_ed_place_t* places = NULL;
	uint32_t place_count = 0;
	char* body;
	int word_length = 0;
	rvn_ed_pos_t from, place_end;
	int has_place_end = R_FALSE;
	int snippet = which;

	if (snippet < 0 || snippet >= (int)g_snippet_count || ed->read_only) return R_FALSE;
	/* (the name typed before the caret is replaced by the snippet) */
	rvn_ed_snippet_ending_at(ed, ed->caret, &word_length);
	from = rvn_ed_pos(ed->caret.line, ed->caret.column - word_length);
	body = rvn_ed_snippet_body(g_snippets[snippet].body, from, &places, &place_count, &place_end, &has_place_end);
	rvn_ed_break_typing(ed);
	ed->caret = ed->anchor = rvn_ed_replace(ed, from, ed->caret, body, -1, R_TRUE);
	rvn_ed_break_typing(ed);
	rvn_free(body);
	rvn_ed_snippet_leave(ed);
	ed->places = places;
	ed->place_count = place_count;
	ed->place_at = -1;
	ed->place_end = place_end;
	ed->has_place_end = has_place_end;
	/* (the places were found from where the snippet starts: they are already absolute) */
	if (place_count > 0) rvn_ed_snippet_goto_place(ed, 0);
	else {
		ed->caret = ed->anchor = from;
		ed->goal_x = -1.0f;
		rvn_ed_caret_moved(ed, R_FALSE);
		rvn_ed_snippet_leave(ed);
	}
	rvn_ed_notify(&ed->base);
	return R_TRUE;
}

/* The snippet whose name ends just before the caret, or -1. */
static int rvn_ed_snippet_at_caret(rvn_text_editor_t* ed) {
	const rvn_ed_line_t* line = &ed->lines[ed->caret.line];
	int start = ed->caret.column;
	if (ed->read_only || ed->no_snippets) return -1;
	/* (the name is a run of letters or backslashes: "\sec", "fig") */
	while (start > 0 && rvn_ed_word_char(ed, line->text[start - 1])) start--;
	if (start == ed->caret.column) return -1;
	return rvn_ed_snippet_ending_at(ed, ed->caret, NULL);
}

/* ---------------------------------------------------------------------------
 *  Proposals (completion)
 *
 *  The list is a popup of the window, owned by the editor. The editor keeps
 *  the keyboard: the popup only takes the keys that move in the list or
 *  choose from it, and every character typed narrows the list down.
 * ------------------------------------------------------------------------- */

#define RVN_EDC_MAX_ITEMS 400
#define RVN_EDC_MAX_ROWS 8

typedef struct rvn_edc_item_t {
	char* text;
	char* detail;
	/** 0: given by the application; 1: starts as what was typed; 2: starts so, whatever the case. */
	int rank;
	/** When it was added: the proposals of the application stay in the order it gave them. */
	uint32_t order;
} rvn_edc_item_t;

typedef struct rvn_ed_complete_t {
	rvn_widget_t base;
	rvn_text_editor_t* editor;
	rvn_edc_item_t* items;
	uint32_t count;
	uint32_t cap;
	int selected;
	rvn_scrollbar_t bar;
	RSpring select_y;
	/** Where the word being completed starts, and how many bytes of it are typed. */
	rvn_ed_pos_t start;
	int prefix_length;
	/** R_TRUE while the callback of the application may add proposals. */
	int collecting;
} rvn_ed_complete_t;

static void rvn_edc_clear(rvn_ed_complete_t* list) {
	uint32_t i;
	for (i = 0; i < list->count; i++) {
		rvn_free(list->items[i].text);
		rvn_free(list->items[i].detail);
	}
	list->count = 0;
}

static void rvn_edc_add(rvn_ed_complete_t* list, const char* text, int length, const char* detail, int rank) {
	rvn_edc_item_t* item;
	if (list->count >= RVN_EDC_MAX_ITEMS || length <= 0) return;
	RVN_ARRAY_RESERVE(list->items, list->cap, list->count + 1);
	item = &list->items[list->count++];
	item->text = rvn_strndup(text, (size_t)length);
	item->detail = detail ? rvn_strdup(detail) : NULL;
	item->rank = rank;
	item->order = list->count;
}

static int rvn_edc_compare(const void* a, const void* b) {
	const rvn_edc_item_t* x = (const rvn_edc_item_t*)a;
	const rvn_edc_item_t* y = (const rvn_edc_item_t*)b;
	int order;
	if (x->rank != y->rank) return x->rank < y->rank ? -1 : 1;
	if (x->rank == 0) return x->order < y->order ? -1 : x->order > y->order;
	order = rvn_stricmp(x->text, y->text);
	if (order == 0) order = strcmp(x->text, y->text);
	/* the same word twice: the one that says what it is comes first, and is the one kept */
	if (order == 0) order = (x->detail ? 0 : 1) - (y->detail ? 0 : 1);
	return order;
}

static float rvn_edc_row_height(const rvn_widget_t* widget) {
	return rvn_widget_metric(widget, R_METRIC_ROW_HEIGHT);
}

static float rvn_edc_pad(const rvn_widget_t* widget) {
	return rvn_widget_glass(widget) ? 5.0f : 1.0f;
}

static rvn_vec2_t rvn_edc_measure(rvn_widget_t* widget) {
	rvn_ed_complete_t* list = (rvn_ed_complete_t*)widget;
	rvn_font_t* font = rvn_font_resolve(g_rvn.mono_font);
	float size = rvn_widget_font_size(widget, 0.0f), width = 0.0f;
	uint32_t i, rows = list->count < RVN_EDC_MAX_ROWS ? list->count : RVN_EDC_MAX_ROWS;
	for (i = 0; i < list->count && i < 64; i++) {
		float item = rvn_text_width(font, size, list->items[i].text, -1);
		if (list->items[i].detail) item += rvn_widget_text_width(widget, R_INVALID_ID, 0.0f, list->items[i].detail) + 28.0f;
		if (item > width) width = item;
	}
	width = rvn_clampf(width + rvn_widget_metric(widget, R_METRIC_PADDING_X) * 2.0f + 14.0f, 180.0f, 460.0f);
	return rvn_vec2(ceilf(width), rvn_edc_row_height(widget) * (float)(rows ? rows : 1) + rvn_edc_pad(widget) * 2.0f);
}

static void rvn_edc_reveal(rvn_widget_t* widget) {
	rvn_ed_complete_t* list = (rvn_ed_complete_t*)widget;
	float row = rvn_edc_row_height(widget), top = row * (float)list->selected;
	if (top < list->bar.offset) rvn_scrollbar_set(&list->bar, top);
	else if (top + row > list->bar.offset + list->bar.viewport) rvn_scrollbar_set(&list->bar, top + row - list->bar.viewport);
}

static void rvn_edc_arrange(rvn_widget_t* widget) {
	rvn_ed_complete_t* list = (rvn_ed_complete_t*)widget;
	list->bar.content = rvn_edc_row_height(widget) * (float)list->count;
	list->bar.viewport = rvn_maxf(0.0f, widget->rect.h - rvn_edc_pad(widget) * 2.0f);
	rvn_scrollbar_clamp(&list->bar);
	rvn_edc_reveal(widget);
}

static rvn_rect_t rvn_edc_track(const rvn_widget_t* widget) {
	float bar = rvn_widget_metric(widget, R_METRIC_SCROLLBAR_SIZE), pad = rvn_edc_pad(widget);
	return rvn_rect(widget->rect.w - bar - 1.0f, pad, bar, rvn_maxf(0.0f, widget->rect.h - pad * 2.0f));
}

static void rvn_edc_paint(rvn_widget_t* widget, RPainter* painter) {
	rvn_ed_complete_t* list = (rvn_ed_complete_t*)widget;
	rvn_rect_t rect = rvn_rect(0.0f, 0.0f, widget->rect.w, widget->rect.h);
	float row = rvn_edc_row_height(widget), padding = rvn_widget_metric(widget, R_METRIC_PADDING_X) * 0.8f;
	float radius = rvn_widget_metric(widget, R_METRIC_POPUP_RADIUS), pad = rvn_edc_pad(widget);
	float size = rvn_widget_font_size(widget, 0.0f);
	float offset = rvn_scrollbar_shown(&list->bar, painter);
	rvn_font_t* font = rvn_font_resolve(g_rvn.mono_font);
	rvn_color_t text = rvn_widget_color(widget, R_COLOR_TEXT);
	rvn_color_t accent = rvn_widget_color(widget, R_COLOR_ACCENT_HOVERED);
	rvn_vec2_t origin = rvn_widget_origin(widget);
	int first = (int)floorf(offset / row), last, i;

	if (rvn_widget_glass(widget)) {
		rvn_paint_surface(painter, widget, rect, radius, RVN_SURFACE_POPUP, 0.0f, 0.0f);
	} else {
		rvn_paint_shadow(painter, rvn_rect(0.0f, 3.0f, rect.w, rect.h), radius, 12.0f, rvn_widget_color(widget, R_COLOR_SHADOW));
		rvn_paint_box(painter, rect, radius, rvn_widget_color(widget, R_COLOR_POPUP), 1.0f, rvn_widget_color(widget, R_COLOR_POPUP_BORDER));
	}
	rvn_paint_push_clip(painter, rvn_rect(1.0f, pad, rect.w - 2.0f, rect.h - pad * 2.0f));
	if (list->selected >= 0 && list->selected < (int)list->count) {
		float y = rvn_paint_spring(painter, &list->select_y, row * (float)list->selected, R_MOTION_SNAPPY);
		rvn_paint_row(painter, widget, rvn_rect(1.0f, pad + y - offset, rect.w - 2.0f, row), rvn_widget_color(widget, R_COLOR_SELECTION));
	}
	last = rvn_mini((int)list->count, first + (int)(list->bar.viewport / row) + 3);
	for (i = rvn_maxi(first, 0); i < last; i++) {
		const rvn_edc_item_t* item = &list->items[i];
		rvn_rect_t line = rvn_rect(padding, pad + row * (float)i - offset, rect.w - padding * 2.0f, row);
		int typed = rvn_mini(list->prefix_length, (int)strlen(item->text));
		float typed_width = rvn_text_width(font, size, item->text, typed);
		/* what is already typed stands out from what choosing the word would add */
		rvn_paint_text_in(painter, g_rvn.mono_font, size, line, 0.0f, item->text, typed, accent);
		rvn_paint_text_in(painter, g_rvn.mono_font, size, rvn_rect(line.x + typed_width, line.y, line.w - typed_width, row), 0.0f,
			item->text + typed, -1, text);
		if (item->detail) {
			rvn_paint_text_in(painter, R_INVALID_ID, size - 1.0f, rvn_rect(line.x, line.y, line.w - 6.0f, row), 1.0f, item->detail, -1,
				rvn_color_fade(text, 0.5f));
		}
	}
	rvn_paint_pop_clip(painter);
	rvn_scrollbar_paint(&list->bar, widget, painter, rvn_edc_track(widget), R_TRUE,
		widget->window ? widget->window->mouse_x - origin.x : -1.0f, widget->window ? widget->window->mouse_y - origin.y : -1.0f);
}

static int rvn_ed_completing(const rvn_text_editor_t* ed) {
	return ed->complete && (ed->complete->flags & RVN_WF_POPUP);
}

static void rvn_ed_complete_close(rvn_text_editor_t* ed) {
	if (rvn_ed_completing(ed) && ed->complete->window) rvn_window_close_popup(ed->complete->window, ed->complete);
}

/* Puts the chosen word in place of what was typed of it. */
static void rvn_ed_complete_accept(rvn_text_editor_t* ed, int index) {
	rvn_ed_complete_t* list = (rvn_ed_complete_t*)ed->complete;
	rvn_widget_t* widget = &ed->base;
	char* text;
	if (!list || index < 0 || index >= (int)list->count) return;
	text = rvn_strdup(list->items[index].text);
	rvn_ed_complete_close(ed);
	if (list->start.line == ed->caret.line && list->start.column <= ed->caret.column && !ed->read_only) {
		rvn_ed_break_typing(ed);
		ed->caret = ed->anchor = rvn_ed_replace(ed, list->start, ed->caret, text, -1, R_TRUE);
		rvn_ed_break_typing(ed);
		rvn_ed_caret_moved(ed, R_FALSE);
		rvn_free(text);
		rvn_ed_notify(widget);
		return;
	}
	rvn_free(text);
}

static int rvn_edc_event(rvn_widget_t* widget, const REvent* event) {
	rvn_ed_complete_t* list = (rvn_ed_complete_t*)widget;
	int count = (int)list->count, changed = R_FALSE, row;
	float pad = rvn_edc_pad(widget);

	if (event->type == R_EVENT_MOUSE_DOWN || event->type == R_EVENT_MOUSE_MOVE || event->type == R_EVENT_MOUSE_UP) {
		if (rvn_scrollbar_event(&list->bar, widget, event, rvn_edc_track(widget), R_TRUE, &changed)) {
			rvn_widget_invalidate_paint(widget);
			return R_TRUE;
		}
	}
	switch (event->type) {
		case R_EVENT_MOUSE_DOWN:
			if (event->button != R_MOUSE_LEFT) return R_TRUE;
			row = (int)floorf((event->local_y - pad + list->bar.offset) / rvn_edc_row_height(widget));
			if (row >= 0 && row < count) rvn_ed_complete_accept(list->editor, row);
			return R_TRUE;
		case R_EVENT_MOUSE_WHEEL:
			rvn_scrollbar_wheel(&list->bar, -event->wheel_y * rvn_edc_row_height(widget) * 3.0f);
			rvn_widget_invalidate_paint(widget);
			return R_TRUE;
		case R_EVENT_KEY_DOWN:
			if (count == 0 || (event->modifiers & (R_MOD_CTRL | R_MOD_ALT | R_MOD_SUPER))) return R_FALSE;
			switch (event->key) {
				case R_KEY_UP: row = list->selected <= 0 ? count - 1 : list->selected - 1; break;
				case R_KEY_DOWN: row = list->selected + 1 >= count ? 0 : list->selected + 1; break;
				case R_KEY_PAGE_UP: row = rvn_maxi(list->selected - RVN_EDC_MAX_ROWS + 1, 0); break;
				case R_KEY_PAGE_DOWN: row = rvn_mini(list->selected + RVN_EDC_MAX_ROWS - 1, count - 1); break;
				case R_KEY_ENTER:
				case R_KEY_KP_ENTER:
				case R_KEY_TAB:
					rvn_ed_complete_accept(list->editor, list->selected);
					return R_TRUE;
				default:
					/* everything else is for the editor: typing goes on */
					return R_FALSE;
			}
			list->selected = row;
			rvn_edc_reveal(widget);
			rvn_widget_invalidate_paint(widget);
			return R_TRUE;
		default:
			return R_FALSE;
	}
}

static void rvn_edc_destroy(rvn_widget_t* widget) {
	rvn_ed_complete_t* list = (rvn_ed_complete_t*)widget;
	rvn_edc_clear(list);
	rvn_free(list->items);
}

static const rvn_widget_class_t rvn_class_ed_complete = {
	.type = RVN_WIDGET_INTERNAL_COMPLETION,
	.name = "TextEditorCompletion",
	.size = sizeof(rvn_ed_complete_t),
	.flags = RVN_CF_NO_CHILDREN | RVN_CF_SURFACE | RVN_CF_NO_MIRROR,
	.destroy = rvn_edc_destroy,
	.measure = rvn_edc_measure,
	.arrange = rvn_edc_arrange,
	.paint = rvn_edc_paint,
	.event = rvn_edc_event
};


/* Adds the names of the text that start with `prefix`, from the lines around the caret. */
static void rvn_ed_complete_document(rvn_text_editor_t* ed, rvn_ed_complete_t* list, const char* prefix, int prefix_length) {
	int first = rvn_maxi(ed->caret.line - 4000, 0), last = rvn_mini(ed->caret.line + 4000, (int)ed->line_count - 1), line;
	for (line = first; line <= last && list->count < RVN_EDC_MAX_ITEMS; line++) {
		const rvn_ed_line_t* l = &ed->lines[line];
		int i = 0;
		while (i < l->length) {
			int start;
			if (!rvn_ed_word_char(ed, l->text[i])) {
				i++;
				continue;
			}
			start = i;
			while (i < l->length && rvn_ed_word_char(ed, l->text[i])) i++;
			/* long enough to be worth choosing, not a number, and not the word being typed itself */
			if (i - start < 3 || i - start <= prefix_length || (l->text[start] >= '0' && l->text[start] <= '9')) continue;
			if (line == ed->caret.line && start == list->start.column) continue;
			if (rvn_strnicmp(l->text + start, prefix, (size_t)prefix_length) != 0) continue;
			rvn_edc_add(list, l->text + start, i - start, NULL, memcmp(l->text + start, prefix, (size_t)prefix_length) == 0 ? 1 : 2);
		}
	}
}

/*
 * Gathers the proposals for the word before the caret and shows them, or
 * hides the list when there is nothing to propose.
 * mode 0: after a character was typed (two characters at least are needed)
 * mode 1: asked for (Ctrl+Space): any start, even none
 * mode 2: the list is shown and the word changed
 */
static void rvn_ed_complete_update(rvn_widget_t* widget, int mode) {
	rvn_text_editor_t* ed = (rvn_text_editor_t*)widget;
	const rvn_language_t* language = rvn_ed_language(ed);
	const rvn_ed_line_t* line = &ed->lines[ed->caret.line];
	static const char* const details[3] = { "keyword", "type", "constant" };
	rvn_ed_complete_t* list;
	char prefix[128];
	int start = ed->caret.column, prefix_length, k, i;
	uint32_t write, read;

	if (!widget->window || ed->read_only || rvn_ed_has_selection(ed) || (mode == 0 && (ed->no_completion || !language))) {
		rvn_ed_complete_close(ed);
		return;
	}
	while (start > 0 && rvn_ed_word_char(ed, line->text[start - 1])) start--;
	prefix_length = ed->caret.column - start;
	if (prefix_length >= (int)sizeof(prefix) || (mode == 0 && prefix_length < 2) || (mode == 2 && prefix_length < 1) ||
		(prefix_length > 0 && line->text[start] >= '0' && line->text[start] <= '9')) {
		rvn_ed_complete_close(ed);
		return;
	}
	memcpy(prefix, line->text + start, (size_t)prefix_length);
	prefix[prefix_length] = '\0';

	if (!ed->complete) {
		ed->complete = rvn_widget_create_class(&rvn_class_ed_complete);
		if (!ed->complete) return;
		ed->complete->flags |= RVN_WF_INTERNAL;
		((rvn_ed_complete_t*)ed->complete)->editor = ed;
	}
	list = (rvn_ed_complete_t*)ed->complete;
	/* another word than the one the list was shown for: it moves there */
	if (rvn_ed_completing(ed) && (list->start.line != ed->caret.line || list->start.column != start)) rvn_ed_complete_close(ed);
	rvn_edc_clear(list);
	list->start = rvn_ed_pos(ed->caret.line, start);
	list->prefix_length = prefix_length;

	if (ed->completion_callback) {
		Rid id = widget->id;
		list->collecting = R_TRUE;
		ed->completion_callback(id, prefix, ed->completion_user);
		if (!rvn_widget_get(id)) return;
		list->collecting = R_FALSE;
	}
	if (language) {
		for (k = 0; k < 3; k++) {
			int count = rvn_language_word_count(language, k);
			for (i = 0; i < count; i++) {
				const char* word = rvn_language_word(language, k, i);
				if ((int)strlen(word) <= prefix_length || rvn_strnicmp(word, prefix, (size_t)prefix_length) != 0) continue;
				rvn_edc_add(list, word, (int)strlen(word), details[k], memcmp(word, prefix, (size_t)prefix_length) == 0 ? 1 : 2);
			}
		}
	}
	if (prefix_length > 0) rvn_ed_complete_document(ed, list, prefix, prefix_length);

	if (list->count > 1) qsort(list->items, list->count, sizeof(rvn_edc_item_t), rvn_edc_compare);
	for (write = 0, read = 0; read < list->count; read++) {
		int twice = write > 0 && strcmp(list->items[write - 1].text, list->items[read].text) == 0;
		uint32_t given;
		/* (what the application proposed is not listed a second time among the words of the language or of the text) */
		for (given = 0; !twice && given < write && list->items[given].rank == 0; given++) {
			twice = strcmp(list->items[given].text, list->items[read].text) == 0;
		}
		if (twice) {
			rvn_free(list->items[read].text);
			rvn_free(list->items[read].detail);
			continue;
		}
		list->items[write++] = list->items[read];
	}
	list->count = write;
	if (list->count == 0) {
		rvn_ed_complete_close(ed);
		return;
	}
	list->selected = 0;
	list->bar.offset = 0.0f;
	list->select_y.started = R_FALSE;
	list->base.material = widget->material;

	if (!rvn_ed_completing(ed)) {
		rvn_ed_metrics_t m;
		rvn_vec2_t origin = rvn_widget_origin(widget), point;
		rvn_ed_metrics(ed, &m);
		rvn_ed_update_rows(ed, &m);
		point = rvn_ed_pos_point(ed, &m, list->start);
		rvn_window_open_popup(widget->window, ed->complete, widget,
			rvn_rect(origin.x + m.area.x + point.x - ed->hbar.offset - 6.0f, origin.y + m.area.y + point.y - ed->vbar.offset,
				10.0f, m.line_height + 3.0f), RVN_POPUP_BELOW, 0.0f);
	}
	rvn_widget_invalidate_layout(ed->complete);
	widget->window->needs_layout = R_TRUE;
}

/* The marker under a point of the text, or NULL. */
static const rvn_ed_marker_t* rvn_ed_marker_at(const rvn_text_editor_t* ed, rvn_ed_pos_t pos) {
	uint32_t i;
	for (i = 0; i < ed->marker_count; i++) {
		const rvn_ed_marker_t* marker = &ed->markers[i];
		if (marker->line != pos.line || pos.column < marker->from) continue;
		if (marker->to < 0 || pos.column <= marker->to) return marker;
	}
	return NULL;
}

/* Is the caret between an opening bracket or quote and the one that closes it, with nothing in between? */
static int rvn_ed_between_pair(rvn_text_editor_t* ed) {
	const RLanguageDesc* desc = ed->no_pairs ? NULL : rvn_ed_language_desc(ed);
	const rvn_ed_line_t* line = &ed->lines[ed->caret.line];
	char before, after;
	if (!desc || ed->caret.column <= 0 || ed->caret.column >= line->length) return R_FALSE;
	before = line->text[ed->caret.column - 1];
	after = line->text[ed->caret.column];
	if (!rvn_ed_closer(before) || rvn_ed_closer(before) != after) return R_FALSE;
	return before == '(' || before == '[' || before == '{' || rvn_ed_is_quote(desc, before);
}

/* ---------------------------------------------------------------------------
 *  Events
 * ------------------------------------------------------------------------- */

static void rvn_ed_blink(Rid timer, void* user_data) {
	rvn_widget_t* widget = rvn_widget_get((Rid)(uintptr_t)user_data);
	RVN_UNUSED(timer);
	if (widget) rvn_widget_invalidate_paint(widget);
}

static void rvn_ed_set_focused(rvn_widget_t* widget, int focused) {
	rvn_text_editor_t* ed = (rvn_text_editor_t*)widget;
	if (focused) {
		ed->blink_start = rvn_clock_seconds();
		if (ed->blink_timer == R_INVALID_ID) ed->blink_timer = Rtimer_create(0.5, R_TRUE, rvn_ed_blink, (void*)(uintptr_t)widget->id);
	} else {
		if (ed->blink_timer != R_INVALID_ID) {
			if (Rtimer_is_valid(ed->blink_timer)) Rtimer_destroy(ed->blink_timer);
			ed->blink_timer = R_INVALID_ID;
		}
		ed->selecting = R_FALSE;
		rvn_ed_break_typing(ed);
	}
	rvn_widget_invalidate_paint(widget);
}

/* Moves the caret to `target`, extending the selection with Shift. */
static void rvn_ed_go(rvn_text_editor_t* ed, rvn_ed_pos_t target, int extend, int keep_goal) {
	ed->caret = rvn_ed_clamp(ed, target);
	if (!extend) ed->anchor = ed->caret;
	rvn_ed_break_typing(ed);
	rvn_ed_complete_close(ed);
	rvn_ed_caret_moved(ed, keep_goal);
}

/* ---------------------------------------------------------------------------
 *  Zoom
 * ------------------------------------------------------------------------- */

#define RVN_ED_ZOOM_MIN 0.5f
#define RVN_ED_ZOOM_MAX 4.0f
/* one notch of the wheel, one press of Ctrl + '+' */
#define RVN_ED_ZOOM_STEP 1.1f

static void rvn_ed_relayout(rvn_text_editor_t* ed);

/*
 * Zooms the text, the point (anchor_x, anchor_y) of the widget staying over
 * the same text: under the pointer for the wheel, at the caret for the keys.
 * Returns R_TRUE when the zoom changed; `notify` calls the zoom callback.
 */
static int rvn_ed_set_zoom(rvn_text_editor_t* ed, float zoom, float anchor_x, float anchor_y, int notify) {
	rvn_widget_t* widget = &ed->base;
	float before_zoom = ed->zoom > 0.0f ? ed->zoom : 1.0f, ax, ay, tx, ty, fx, fy;
	rvn_ed_metrics_t before, after;
	rvn_ed_pos_t pos;
	rvn_vec2_t point;
	zoom = rvn_clampf(zoom, RVN_ED_ZOOM_MIN, RVN_ED_ZOOM_MAX);
	/* (back to 100 % when it passes near it: the wheel finds it again) */
	if (fabsf(zoom - 1.0f) < 0.03f) zoom = 1.0f;
	zoom = floorf(zoom * 100.0f + 0.5f) / 100.0f;
	if (fabsf(zoom - before_zoom) < 0.001f) return R_FALSE;

	rvn_ed_metrics(ed, &before);
	rvn_ed_update_rows(ed, &before);
	ax = rvn_clampf(anchor_x - before.area.x, 0.0f, before.area.w);
	ay = rvn_clampf(anchor_y - before.area.y, 0.0f, before.area.h);
	/*
	 * What is under the anchor: a place in the text, and where in its row (a
	 * fraction of the row, a width in font sizes). Rows would not do: with
	 * word wrap, the lines do not have the same rows at another size.
	 */
	tx = ed->hbar.offset + ax;
	ty = ed->vbar.offset + ay;
	pos = rvn_ed_point_pos(ed, &before, tx, ty);
	point = rvn_ed_pos_point(ed, &before, pos);
	fy = (ty - point.y) / before.line_height;
	fx = (tx - point.x) / before.size;

	ed->zoom = zoom;
	rvn_ed_relayout(ed);
	rvn_ed_metrics(ed, &after);
	rvn_ed_update_rows(ed, &after);
	/* (the gutter of the line numbers grows with the text: measured again) */
	rvn_ed_metrics(ed, &after);
	rvn_ed_update_bars(ed, &after);
	/* the same place of the text under the anchor again */
	point = rvn_ed_pos_point(ed, &after, pos);
	rvn_scrollbar_set(&ed->vbar, point.y + fy * after.line_height - ay);
	if (!ed->wrap) rvn_scrollbar_set(&ed->hbar, point.x + fx * after.size - ax);
	/* no glide from where things were at the other size: they are drawn in their place at once */
	ed->vbar.shown.started = R_FALSE;
	ed->hbar.shown.started = R_FALSE;
	ed->caret_x.started = R_FALSE;
	ed->caret_y.started = R_FALSE;
	rvn_ed_complete_close(ed);
	rvn_widget_invalidate_paint(widget);
	if (notify && ed->zoom_callback) ed->zoom_callback(widget->id, zoom, ed->zoom_user);
	return R_TRUE;
}

/* Ctrl + '+' (or '='), Ctrl + '-', Ctrl + 0, also on the keypad: around the caret, or the middle when it is out of view. */
static int rvn_ed_zoom_key(rvn_text_editor_t* ed, const REvent* event) {
	float zoom = ed->zoom > 0.0f ? ed->zoom : 1.0f, x, y;
	rvn_ed_metrics_t m;
	rvn_vec2_t point;
	int in = event->key == R_KEY_EQUAL || event->key == R_KEY_KP_ADD || event->codepoint == '+' || event->codepoint == '=';
	int out = event->key == R_KEY_MINUS || event->key == R_KEY_KP_SUBTRACT || event->codepoint == '-';
	int reset = event->key == R_KEY_0 || event->key == R_KEY_KP_0 || event->codepoint == '0';
	if (ed->no_zoom || !(event->modifiers & (R_MOD_CTRL | R_MOD_SUPER)) || (event->modifiers & R_MOD_ALT)) return R_FALSE;
	if (!in && !out && !reset) return R_FALSE;
	rvn_ed_metrics(ed, &m);
	rvn_ed_update_rows(ed, &m);
	point = rvn_ed_pos_point(ed, &m, ed->caret);
	x = m.area.x + point.x - ed->hbar.offset;
	y = m.area.y + point.y - ed->vbar.offset;
	if (y < m.area.y || y > m.area.y + m.area.h) {
		x = m.area.x;
		y = m.area.y + m.area.h * 0.5f;
	}
	rvn_ed_set_zoom(ed, reset ? 1.0f : in ? zoom * RVN_ED_ZOOM_STEP : zoom / RVN_ED_ZOOM_STEP, x, y, R_TRUE);
	return R_TRUE;
}

/* Ctrl+Shift+[ folds the block of the caret; Ctrl+Shift+] unfolds the one on its line. */
static void rvn_ed_fold_key(rvn_text_editor_t* ed, int unfold) {
	int fold;
	if (unfold) {
		fold = rvn_ed_fold_line_at(ed, ed->caret.line);
		if (fold < 0) return;
		ed->folds[fold].folded = R_FALSE;
	} else {
		fold = rvn_ed_fold_at_caret(ed);
		if (fold < 0) return;
		ed->folds[fold].folded = R_TRUE;
		ed->caret = ed->anchor = rvn_ed_pos(ed->folds[fold].start, 0);
	}
	rvn_ed_relayout(ed);
	rvn_ed_caret_moved(ed, R_FALSE);
}

static int rvn_ed_key(rvn_widget_t* widget, const REvent* event) {
	rvn_text_editor_t* ed = (rvn_text_editor_t*)widget;
	int shift = (event->modifiers & R_MOD_SHIFT) != 0;
	int command = (event->modifiers & (R_MOD_CTRL | R_MOD_SUPER)) != 0;
	int alt = (event->modifiers & R_MOD_ALT) != 0;
	int changed = R_FALSE;
	Rid id = widget->id;
	rvn_ed_metrics_t m;
	rvn_ed_pos_t target;

	if (rvn_ed_zoom_key(ed, event)) return R_TRUE;

	rvn_ed_metrics(ed, &m);
	rvn_ed_update_rows(ed, &m);

	switch (event->key) {
		case R_KEY_SPACE:
			if (!command) goto printable;
			rvn_ed_complete_update(widget, 1);
			return R_TRUE;
		case R_KEY_SLASH:
			if (!command) goto printable;
			if (ed->read_only || !rvn_ed_toggle_comment(ed)) return R_TRUE;
			changed = R_TRUE;
			break;
		case R_KEY_LEFT_BRACKET:
			/* Ctrl+Shift+[ folds the block of the caret, Ctrl+Shift+] unfolds it */
			if (!command) goto printable;
			if (ed->no_folding) return R_TRUE;
			rvn_ed_fold_key(ed, R_FALSE);
			return R_TRUE;
		case R_KEY_RIGHT_BRACKET:
			if (!command) goto printable;
			if (ed->no_folding) return R_TRUE;
			rvn_ed_fold_key(ed, R_TRUE);
			return R_TRUE;
		case R_KEY_LEFT:
			if (!shift && !command && rvn_ed_has_selection(ed)) target = rvn_ed_sel_min(ed);
			else target = command ? rvn_ed_word_left(ed, ed->caret) : rvn_ed_char_left(ed, ed->caret);
			rvn_ed_go(ed, target, shift, R_FALSE);
			return R_TRUE;
		case R_KEY_RIGHT:
			if (!shift && !command && rvn_ed_has_selection(ed)) target = rvn_ed_sel_max(ed);
			else target = command ? rvn_ed_word_right(ed, ed->caret) : rvn_ed_char_right(ed, ed->caret);
			rvn_ed_go(ed, target, shift, R_FALSE);
			return R_TRUE;
		case R_KEY_UP:
		case R_KEY_DOWN:
			/* Alt+Up / Alt+Down are left to the window shortcuts (previous / next...) */
			if (alt) return R_FALSE;
			if (command) {
				/* Ctrl + arrows scroll without moving the caret */
				rvn_scrollbar_wheel(&ed->vbar, event->key == R_KEY_UP ? -m.line_height : m.line_height);
				rvn_widget_invalidate_paint(widget);
				return R_TRUE;
			}
			rvn_ed_go(ed, rvn_ed_move_rows(ed, &m, ed->caret, event->key == R_KEY_UP ? -1 : 1), shift, R_TRUE);
			return R_TRUE;
		case R_KEY_PAGE_UP:
		case R_KEY_PAGE_DOWN: {
			int page = rvn_maxi((int)(m.area.h / m.line_height) - 1, 1);
			rvn_ed_go(ed, rvn_ed_move_rows(ed, &m, ed->caret, event->key == R_KEY_PAGE_UP ? -page : page), shift, R_TRUE);
			return R_TRUE;
		}
		case R_KEY_HOME:
			if (command) {
				target = rvn_ed_pos(0, 0);
			} else {
				/* the start of the row, or the first character that is not a space when already there */
				const rvn_ed_line_t* line = &ed->lines[ed->caret.line];
				int begin, end, indent;
				rvn_ed_row_range(ed, ed->caret.line, rvn_ed_row_of(ed, ed->caret.line, ed->caret.column), &begin, &end);
				indent = begin;
				while (indent < end && (line->text[indent] == ' ' || line->text[indent] == '\t')) indent++;
				target = rvn_ed_pos(ed->caret.line, ed->caret.column == indent || indent == end ? begin : indent);
			}
			rvn_ed_go(ed, target, shift, R_FALSE);
			return R_TRUE;
		case R_KEY_END:
			if (command) {
				target = rvn_ed_end(ed);
			} else {
				int begin, end, row = rvn_ed_row_of(ed, ed->caret.line, ed->caret.column);
				rvn_ed_row_range(ed, ed->caret.line, row, &begin, &end);
				/* (the last position of a wrapped row is before its cut) */
				if (ed->wrap && row + 1 < ed->lines[ed->caret.line].row_count && end > begin) {
					end = rvn_utf8_prev(ed->lines[ed->caret.line].text, end);
				}
				target = rvn_ed_pos(ed->caret.line, end);
			}
			rvn_ed_go(ed, target, shift, R_FALSE);
			return R_TRUE;
		case R_KEY_ESCAPE:
			/* leaves the places of a snippet */
			if (ed->place_count > 0) {
				rvn_ed_snippet_leave(ed);
				rvn_widget_invalidate_paint(widget);
				return R_TRUE;
			}
			return R_FALSE;
		case R_KEY_BACKSPACE:
		case R_KEY_DELETE:
			if (ed->read_only) return R_TRUE;
			if (ed->multi_cursor && ed->caret_count > 0) {
				rvn_ed_multi_edit(ed, "", 0, event->key == R_KEY_BACKSPACE ? 1 : 2);
				changed = R_TRUE;
				break;
			}
			if (!rvn_ed_has_selection(ed)) {
				if (event->key == R_KEY_BACKSPACE) ed->anchor = command ? rvn_ed_word_left(ed, ed->caret) : rvn_ed_char_left(ed, ed->caret);
				else ed->anchor = command ? rvn_ed_word_right(ed, ed->caret) : rvn_ed_char_right(ed, ed->caret);
				if (event->key == R_KEY_BACKSPACE && !command && rvn_ed_between_pair(ed)) {
					/* a pair with nothing in it goes away as it came: together */
					ed->caret = rvn_ed_char_right(ed, ed->caret);
				}
			}
			/* (typing elsewhere ends the places of a snippet) */
			rvn_ed_snippet_leave(ed);
			rvn_ed_break_typing(ed);
			changed = rvn_ed_delete_selection(ed);
			break;
		case R_KEY_ENTER:
		case R_KEY_KP_ENTER:
			/* Ctrl/Alt+Enter is left to the window shortcuts (send, run...) */
			if (command || alt) return R_FALSE;
			if (ed->read_only) return R_TRUE;
			rvn_ed_snippet_leave(ed);
			rvn_ed_new_line(ed);
			changed = R_TRUE;
			break;
		case R_KEY_TAB:
			/* (inside a snippet, Tab walks its places) */
			if (!command && !alt && ed->place_count > 0) {
				rvn_ed_snippet_walk(ed, shift);
				return R_TRUE;
			}
			if (command || alt) return R_FALSE;
			if (ed->read_only) return R_FALSE;
			rvn_ed_snippet_leave(ed);
			if (shift || rvn_ed_sel_min(ed).line != rvn_ed_sel_max(ed).line) {
				rvn_ed_indent_lines(ed, shift);
			} else if (ed->tab_spaces) {
				/* up to the next tab stop */
				char spaces[16];
				const char* line_text = ed->lines[rvn_ed_sel_min(ed).line].text;
				int count = rvn_clampi(ed->tab_size, 1, 16);
				int columns = 0, at = 0, stop = rvn_ed_sel_min(ed).column;
				/* the column on screen: a tab before the caret counts for what it shows */
				while (at < stop) {
					if (line_text[at] == '\t') columns += count - columns % count;
					else columns++;
					at = rvn_utf8_next(line_text, stop, at);
				}
				count -= columns % count;
				memset(spaces, ' ', (size_t)count);
				rvn_ed_break_typing(ed);
				rvn_ed_insert(ed, spaces, count);
			} else {
				rvn_ed_break_typing(ed);
				rvn_ed_insert(ed, "\t", 1);
			}
			changed = R_TRUE;
			break;
		case R_KEY_A:
			if (!command) goto printable;
			ed->anchor = rvn_ed_pos(0, 0);
			ed->caret = rvn_ed_end(ed);
			rvn_ed_break_typing(ed);
			rvn_widget_invalidate_paint(widget);
			return R_TRUE;
		case R_KEY_C:
			if (!command) goto printable;
			rvn_ed_copy(ed);
			return R_TRUE;
		case R_KEY_X:
			if (!command) goto printable;
			rvn_ed_copy(ed);
			if (ed->read_only) return R_TRUE;
			rvn_ed_break_typing(ed);
			changed = rvn_ed_delete_selection(ed);
			break;
		case R_KEY_V: {
			const char* clip;
			if (!command) goto printable;
			if (ed->read_only) return R_TRUE;
			clip = Rclipboard_get_text();
			if (!clip || !clip[0]) return R_TRUE;
			rvn_ed_break_typing(ed);
			rvn_ed_insert(ed, clip, -1);
			rvn_ed_break_typing(ed);
			changed = R_TRUE;
			break;
		}
		case R_KEY_Z:
			if (!command) goto printable;
			changed = shift ? rvn_ed_redo(ed) : rvn_ed_undo(ed);
			if (!changed) return R_TRUE;
			break;
		case R_KEY_Y:
			if (!command) goto printable;
			changed = rvn_ed_redo(ed);
			if (!changed) return R_TRUE;
			break;
		case R_KEY_D:
			/* Ctrl+D: the next occurrence of the selection gets a caret of its own */
			if (!command || !ed->multi_cursor) goto printable;
			if (ed->read_only) return R_TRUE;
			if (!Rtext_editor_select_next_occurrence(id)) return R_TRUE;
			rvn_widget_invalidate_paint(widget);
			return R_TRUE;
		default:
		printable:
			/* keys that type a character are consumed so that they never trigger
			 * a window shortcut; the character itself arrives as a text event */
			if (command || alt || (int)event->key >= 256 || event->key == R_KEY_UNKNOWN) return R_FALSE;
			return R_TRUE;
	}
	rvn_ed_caret_moved(ed, R_FALSE);
	if (changed) rvn_ed_notify(widget);
	if (!rvn_widget_get(id)) return R_TRUE;
	/* erasing narrows the list of proposals back; anything else is another matter */
	if (event->key == R_KEY_BACKSPACE || event->key == R_KEY_DELETE) {
		if (rvn_ed_completing(ed)) rvn_ed_complete_update(widget, 2);
	} else {
		rvn_ed_complete_close(ed);
	}
	return R_TRUE;
}

static int rvn_text_editor_event(rvn_widget_t* widget, const REvent* event) {
	rvn_text_editor_t* ed = (rvn_text_editor_t*)widget;
	rvn_ed_metrics_t m;
	rvn_rect_t vertical, horizontal;
	int changed = R_FALSE;

	switch (event->type) {
		case R_EVENT_FOCUS_GAINED:
			rvn_ed_set_focused(widget, R_TRUE);
			return R_TRUE;
		case R_EVENT_FOCUS_LOST:
			rvn_ed_complete_close(ed);
			rvn_ed_set_focused(widget, R_FALSE);
			return R_TRUE;
		case R_EVENT_KEY_DOWN:
			if (!(widget->flags & RVN_WF_FOCUSED)) return R_FALSE;
			return rvn_ed_key(widget, event);
		case R_EVENT_TEXT: {
			Rid id = widget->id;
			uint32_t version = ed->version;
			if (!(widget->flags & RVN_WF_FOCUSED) || ed->read_only) return R_FALSE;
			if (event->codepoint < 32 || event->codepoint == 127) return R_FALSE;
			if (ed->multi_cursor && ed->caret_count > 0) {
				/* (every caret types the character, at once) */
				char encoded[4];
				rvn_ed_multi_edit(ed, encoded, rvn_utf8_encode(event->codepoint, encoded), 0);
			} else {
				rvn_ed_type(ed, event->codepoint);
				rvn_ed_caret_moved(ed, R_FALSE);
			}
			if (ed->version != version) rvn_ed_notify(widget);
			if (!rvn_widget_get(id)) return R_TRUE;
			/* each character of a word narrows the proposals down; anything else ends the word */
			if (event->codepoint >= 128 || rvn_ed_is_word_char((char)event->codepoint)) {
				rvn_ed_complete_update(widget, rvn_ed_completing(ed) ? 2 : 0);
			} else {
				rvn_ed_complete_close(ed);
			}
			return R_TRUE;
		}
		case R_EVENT_MOUSE_WHEEL:
			rvn_ed_complete_close(ed);
			/* Ctrl + wheel zooms, around the pointer; it never scrolls what contains the editor */
			if ((event->modifiers & R_MOD_CTRL) && !ed->no_zoom) {
				float zoom = ed->zoom > 0.0f ? ed->zoom : 1.0f;
				if (event->wheel_y != 0.0f) rvn_ed_set_zoom(ed, zoom * powf(RVN_ED_ZOOM_STEP, event->wheel_y), event->local_x, event->local_y, R_TRUE);
				return R_TRUE;
			}
			rvn_ed_metrics(ed, &m);
			if ((event->modifiers & R_MOD_SHIFT) && !ed->wrap) {
				changed = rvn_scrollbar_wheel(&ed->hbar, -event->wheel_y * m.space * 12.0f);
			} else {
				changed = rvn_scrollbar_wheel(&ed->vbar, -event->wheel_y * m.line_height * 3.0f);
				if (event->wheel_x != 0.0f && !ed->wrap) changed |= rvn_scrollbar_wheel(&ed->hbar, -event->wheel_x * m.space * 12.0f);
			}
			rvn_widget_invalidate_paint(widget);
			/* at the end of its range, the wheel goes to what contains the editor */
			return changed;
		case R_EVENT_MOUSE_DOWN:
		case R_EVENT_MOUSE_MOVE:
		case R_EVENT_MOUSE_UP: {
			rvn_ed_pos_t pos;
			int used;
			rvn_ed_metrics(ed, &m);
			rvn_ed_update_rows(ed, &m);
			rvn_ed_tracks(ed, &m, &vertical, &horizontal);
			if (!ed->selecting) {
				used = rvn_scrollbar_event(&ed->vbar, widget, event, vertical, R_TRUE, &changed);
				if (!used && !ed->wrap) used = rvn_scrollbar_event(&ed->hbar, widget, event, horizontal, R_FALSE, &changed);
				if (used) {
					rvn_widget_invalidate_paint(widget);
					return R_TRUE;
				}
			}
			if (event->type == R_EVENT_MOUSE_UP) {
				if (event->button != R_MOUSE_LEFT || !ed->selecting) return R_FALSE;
				ed->selecting = R_FALSE;
				ed->column_drag = R_FALSE;
				rvn_widget_release_mouse(widget);
				return R_TRUE;
			}
			if (event->type == R_EVENT_MOUSE_MOVE && !ed->selecting) {
				if (ed->marker_count > 0 || ed->marker_tip) {
					/* the message of the marker under the pointer is the tooltip of the editor */
					const rvn_ed_marker_t* marker = NULL;
					const char* message;
					if (rvn_rect_contains(m.area, event->local_x, event->local_y)) {
						float tx = event->local_x - m.area.x + ed->hbar.offset, ty = event->local_y - m.area.y + ed->vbar.offset;
						rvn_vec2_t point;
						pos = rvn_ed_point_pos(ed, &m, tx, ty);
						point = rvn_ed_pos_point(ed, &m, pos);
						/* (the nearest position is not under the pointer when it is past the end of the line, or below the text) */
						if (ty >= point.y && ty < point.y + m.line_height && fabsf(tx - point.x) <= m.space) marker = rvn_ed_marker_at(ed, pos);
					}
					message = marker ? marker->message : NULL;
					if (message ? (!widget->tooltip || strcmp(widget->tooltip, message) != 0) : ed->marker_tip) {
						rvn_str_set(&widget->tooltip, message);
						ed->marker_tip = message != NULL;
						if (widget->window) rvn_window_rearm_tooltip(widget->window);
					}
				}
				/* the scrollbars react to the pointer */
				rvn_widget_invalidate_paint(widget);
				return R_FALSE;
			}
			if (event->type == R_EVENT_MOUSE_DOWN && event->button != R_MOUSE_LEFT) return R_FALSE;

			/* the arrow of a folded block in the margin, when the line numbers are shown */
			if (event->type == R_EVENT_MOUSE_DOWN && ed->line_numbers && !ed->no_folding && event->local_x < m.gutter) {
				int row_at = (int)((event->local_y - m.area.y + ed->vbar.offset) / m.line_height);
				int hit_line, hit_row, fold;
				rvn_ed_locate_row(ed, (uint32_t)rvn_maxi(row_at, 0), &hit_line, &hit_row);
				fold = rvn_ed_fold_line_at(ed, hit_line);
				if (fold >= 0) {
					ed->folds[fold].folded = !ed->folds[fold].folded;
					rvn_ed_relayout(ed);
					return R_TRUE;
				}
			}

			pos = rvn_ed_point_pos(ed, &m, event->local_x - m.area.x + ed->hbar.offset, event->local_y - m.area.y + ed->vbar.offset);
			if (event->type == R_EVENT_MOUSE_DOWN) {
				if (event->clicks >= 3) {
					/* the whole line, with its line break */
					ed->anchor = rvn_ed_pos(pos.line, 0);
					ed->caret = pos.line + 1 < (int)ed->line_count ? rvn_ed_pos(pos.line + 1, 0) : rvn_ed_pos(pos.line, ed->lines[pos.line].length);
				} else if (event->clicks == 2) {
					const rvn_ed_line_t* line = &ed->lines[pos.line];
					int begin = pos.column, end = pos.column;
					if (line->length > 0) {
						rvn_ed_class_t kind;
						if (begin >= line->length) begin = line->length - 1;
						kind = rvn_ed_class(line->text[begin]);
						end = begin;
						while (begin > 0 && rvn_ed_class(line->text[begin - 1]) == kind) begin--;
						while (end < line->length && rvn_ed_class(line->text[end]) == kind) end++;
					}
					ed->anchor = rvn_ed_pos(pos.line, begin);
					ed->caret = rvn_ed_pos(pos.line, end);
				} else {
					/* Ctrl+click adds a caret when several are allowed */
					if (ed->multi_cursor && (event->modifiers & (R_MOD_CTRL | R_MOD_SUPER)) && ed->caret_count < 255) {
						rvn_ed_caret_t* caret;
						RVN_ARRAY_RESERVE(ed->carets, ed->caret_cap, ed->caret_count + 1);
						caret = &ed->carets[ed->caret_count++];
						caret->caret = caret->anchor = pos;
						rvn_widget_invalidate_paint(widget);
						return R_TRUE;
					}
					/* Alt+drag: a caret on every line, from the column pressed (a column of text) */
					if (ed->multi_cursor && (event->modifiers & R_MOD_ALT)) {
						ed->anchor = pos;
						ed->caret = pos;
						ed->caret_count = 0;
						ed->column_drag = R_TRUE;
					} else {
						ed->caret = pos;
						if (!(event->modifiers & R_MOD_SHIFT)) ed->anchor = pos;
						ed->column_drag = R_FALSE;
					}
					ed->selecting = R_TRUE;
					rvn_widget_capture_mouse(widget);
				}
				rvn_ed_complete_close(ed);
				rvn_ed_break_typing(ed);
			} else if (ed->column_drag) {
				/* (Alt+drag: the line the pointer is on gets a selection in the column that was pressed) */
				rvn_ed_column_drag(ed, ed->anchor, pos);
			} else {
				ed->caret = pos;
			}
			rvn_ed_caret_moved(ed, R_FALSE);
			return R_TRUE;
		}
		default:
			return R_FALSE;
	}
}

/* ---------------------------------------------------------------------------
 *  Class
 * ------------------------------------------------------------------------- */

static void rvn_ed_set_text(rvn_text_editor_t* ed, const char* text) {
	rvn_ed_history_clear(ed->undo, &ed->undo_count);
	rvn_ed_history_clear(ed->redo, &ed->redo_count);
	rvn_ed_replace(ed, rvn_ed_pos(0, 0), rvn_ed_end(ed), text ? text : "", -1, R_FALSE);
	ed->caret = ed->anchor = rvn_ed_pos(0, 0);
	ed->vbar.offset = 0.0f;
	ed->hbar.offset = 0.0f;
	ed->goal_x = -1.0f;
	rvn_widget_invalidate_layout(&ed->base);
}

static void rvn_text_editor_init(rvn_widget_t* widget) {
	rvn_text_editor_t* ed = (rvn_text_editor_t*)widget;
	ed->tab_size = 4;
	ed->font = R_INVALID_ID;
	ed->goal_x = -1.0f;
	ed->blink_timer = R_INVALID_ID;
	ed->rows_dirty = R_TRUE;
	rvn_ed_lines_insert(ed, 0, 1);
	widget->cursor = R_CURSOR_IBEAM;
}

static void rvn_text_editor_destroy(rvn_widget_t* widget) {
	rvn_text_editor_t* ed = (rvn_text_editor_t*)widget;
	uint32_t i;
	if (ed->blink_timer != R_INVALID_ID && Rtimer_is_valid(ed->blink_timer)) Rtimer_destroy(ed->blink_timer);
	for (i = 0; i < ed->line_count; i++) rvn_ed_line_free(&ed->lines[i]);
	rvn_free(ed->lines);
	rvn_free(ed->row_starts);
	rvn_ed_history_clear(ed->undo, &ed->undo_count);
	rvn_ed_history_clear(ed->redo, &ed->redo_count);
	rvn_free(ed->undo);
	rvn_free(ed->redo);
	rvn_free(ed->placeholder);
	rvn_free(ed->text_cache);
	rvn_free(ed->selection_cache);
	rvn_free(ed->language_name);
	for (i = 0; i < ed->marker_count; i++) rvn_free(ed->markers[i].message);
	rvn_free(ed->markers);
	rvn_free(ed->folds);
	rvn_free(ed->hidden_before);
	rvn_free(ed->matches);
	rvn_free(ed->match_pattern);
	rvn_free(ed->search_pattern);
	rvn_ed_snippet_leave(ed);
	rvn_free(ed->carets);
	if (ed->complete) rvn_widget_destroy(ed->complete);
}

/* Gives the editor a language by name; NULL, "" or "None" removes it. Returns R_FALSE when the name is unknown. */
static int rvn_ed_set_language(rvn_text_editor_t* ed, const char* name) {
	const rvn_language_t* language = NULL;
	if (name && name[0] && rvn_stricmp(name, "None") != 0) {
		language = rvn_language_find(name);
		if (!language) return R_FALSE;
	}
	rvn_str_set(&ed->language_name, language ? rvn_language_desc(language)->name : NULL);
	/* (looked for again, and everything that was computed with the other one with it) */
	ed->language_generation = rvn_language_generation() - 1u;
	ed->lex_valid = 0;
	ed->match_version = ed->version - 1u;
	rvn_ed_complete_close(ed);
	return R_TRUE;
}

static int rvn_text_editor_set_attribute(rvn_widget_t* widget, const char* name, const char* value) {
	rvn_text_editor_t* ed = (rvn_text_editor_t*)widget;
	float number;
	int flag;
	if (RVN_ATTR("Text")) {
		rvn_ed_set_text(ed, value);
		return R_TRUE;
	}
	if (RVN_ATTR("Placeholder")) {
		rvn_str_set(&ed->placeholder, value);
	} else if (RVN_ATTR("ReadOnly")) {
		if (!rvn_parse_bool(value, &ed->read_only)) return R_FALSE;
	} else if (RVN_ATTR("Wrap") || RVN_ATTR("WordWrap")) {
		if (!rvn_parse_bool(value, &flag)) return R_FALSE;
		Rtext_editor_set_word_wrap(widget->id, flag);
	} else if (RVN_ATTR("LineNumbers")) {
		if (!rvn_parse_bool(value, &ed->line_numbers)) return R_FALSE;
	} else if (RVN_ATTR("TabSize")) {
		if (!rvn_parse_float(value, &number) || number < 1.0f || number > 16.0f) return R_FALSE;
		ed->tab_size = (int)number;
	} else if (RVN_ATTR("TabSpaces")) {
		if (!rvn_parse_bool(value, &ed->tab_spaces)) return R_FALSE;
	} else if (RVN_ATTR("Language") || RVN_ATTR("Syntax")) {
		if (!rvn_ed_set_language(ed, value)) return R_FALSE;
	} else if (RVN_ATTR("Markdown")) {
		if (!rvn_parse_bool(value, &flag)) return R_FALSE;
		if (!rvn_ed_set_language(ed, flag ? "Markdown" : "")) return R_FALSE;
	} else if (RVN_ATTR("Completion")) {
		if (!rvn_parse_bool(value, &flag)) return R_FALSE;
		ed->no_completion = !flag;
	} else if (RVN_ATTR("AutoPairs")) {
		if (!rvn_parse_bool(value, &flag)) return R_FALSE;
		ed->no_pairs = !flag;
	} else if (RVN_ATTR("Folding")) {
		if (!rvn_parse_bool(value, &flag)) return R_FALSE;
		ed->no_folding = !flag;
		if (!ed->no_folding) rvn_ed_update_folds(ed);
	} else if (RVN_ATTR("Snippets")) {
		if (!rvn_parse_bool(value, &flag)) return R_FALSE;
		ed->no_snippets = !flag;
	} else if (RVN_ATTR("MultiCursor")) {
		if (!rvn_parse_bool(value, &flag)) return R_FALSE;
		ed->multi_cursor = flag;
	} else if (RVN_ATTR("Font")) {
		Rid font = rvn_xml_font(value);
		if (font == R_INVALID_ID) return R_FALSE;
		ed->font = font;
	} else if (RVN_ATTR("FontSize") || RVN_ATTR("TextSize")) {
		if (!rvn_parse_float(value, &number) || number < 0.0f) return R_FALSE;
		ed->font_size = number;
	} else if (RVN_ATTR("Zoom")) {
		if (!rvn_parse_float(value, &number) || number <= 0.0f) return R_FALSE;
		ed->zoom = rvn_clampf(number, RVN_ED_ZOOM_MIN, RVN_ED_ZOOM_MAX);
	} else if (RVN_ATTR("Zoomable")) {
		if (!rvn_parse_bool(value, &flag)) return R_FALSE;
		ed->no_zoom = !flag;
	} else {
		return R_FALSE;
	}
	/* anything above may change the size of the rows */
	{
		uint32_t i;
		for (i = 0; i < ed->line_count; i++) rvn_ed_line_dirty(&ed->lines[i]);
		ed->rows_dirty = R_TRUE;
	}
	rvn_widget_invalidate_layout(widget);
	return R_TRUE;
}

/* A finger scrolls the text; two fingers zoom it, as Ctrl + wheel does. */
static int rvn_text_editor_pan(rvn_widget_t* widget, float dx, float dy, int coasting) {
	rvn_text_editor_t* ed = (rvn_text_editor_t*)widget;
	int moved = rvn_scrollbar_pan(&ed->vbar, -dy, coasting);
	if (!ed->wrap) moved |= rvn_scrollbar_pan(&ed->hbar, -dx, coasting);
	if (moved) rvn_ed_complete_close(ed);
	return moved;
}

static int rvn_text_editor_zoom(rvn_widget_t* widget, float factor, float x, float y) {
	rvn_text_editor_t* ed = (rvn_text_editor_t*)widget;
	float zoom = ed->zoom > 0.0f ? ed->zoom : 1.0f;
	if (ed->no_zoom) return R_FALSE;
	rvn_ed_complete_close(ed);
	return rvn_ed_set_zoom(ed, zoom * factor, x, y, R_TRUE);
}

const rvn_widget_class_t rvn_class_text_editor = {
	.type = R_WIDGET_TEXT_EDITOR,
	.name = "TextEditor",
	.size = sizeof(rvn_text_editor_t),
	.flags = RVN_CF_NO_CHILDREN | RVN_CF_FOCUSABLE | RVN_CF_SURFACE | RVN_CF_NO_MIRROR,
	.init = rvn_text_editor_init,
	.destroy = rvn_text_editor_destroy,
	.measure = rvn_text_editor_measure,
	.arrange = rvn_text_editor_arrange,
	.paint = rvn_text_editor_paint,
	.event = rvn_text_editor_event,
	.set_attribute = rvn_text_editor_set_attribute,
	.pan = rvn_text_editor_pan,
	.zoom = rvn_text_editor_zoom
};

/* ---------------------------------------------------------------------------
 *  Public API
 * ------------------------------------------------------------------------- */

#define RVN_EDITOR(variable, id, result) \
	rvn_widget_t* widget = rvn_widget_check((id), R_WIDGET_TEXT_EDITOR, __func__); \
	rvn_text_editor_t* variable = (rvn_text_editor_t*)widget; \
	if (!widget) return result

#define RVN_EDITOR_VOID(variable, id) \
	rvn_widget_t* widget = rvn_widget_check((id), R_WIDGET_TEXT_EDITOR, __func__); \
	rvn_text_editor_t* variable = (rvn_text_editor_t*)widget; \
	if (!widget) return

/* An option that changes how the lines are laid out. */
static void rvn_ed_relayout(rvn_text_editor_t* ed) {
	uint32_t i;
	for (i = 0; i < ed->line_count; i++) rvn_ed_line_dirty(&ed->lines[i]);
	ed->rows_dirty = R_TRUE;
	rvn_widget_invalidate_layout(&ed->base);
}

Rid Rtext_editor_create(const char* text) {
	rvn_widget_t* widget;
	RVN_API_GUARD(R_INVALID_ID);
	widget = rvn_widget_create(R_WIDGET_TEXT_EDITOR);
	if (!widget) return R_INVALID_ID;
	if (text && text[0]) rvn_ed_set_text((rvn_text_editor_t*)widget, text);
	return widget->id;
}

void Rtext_editor_set_text(Rid id, const char* text) {
	RVN_EDITOR_VOID(ed, id);
	rvn_ed_set_text(ed, text);
}

/* Loads new text (a file changed by another program), keeping the caret and the scroll where they were
 * as far as the new text reaches. The text loaded counts as saved. */
void Rtext_editor_reload_text(Rid id, const char* text) {
	int line, column;
	float vertical, horizontal;
	RVN_EDITOR_VOID(ed, id);
	line = ed->caret.line;
	column = ed->caret.column;
	vertical = ed->vbar.offset;
	horizontal = ed->hbar.offset;
	rvn_ed_set_text(ed, text);
	if (line >= (int)ed->line_count) line = (int)ed->line_count - 1;
	if (line < 0) line = 0;
	if (column > ed->lines[line].length) column = ed->lines[line].length;
	ed->caret = ed->anchor = rvn_ed_pos(line, column);
	ed->vbar.offset = vertical;
	ed->hbar.offset = horizontal;
	ed->goal_x = -1.0f;
	ed->saved_version = ed->version;
	rvn_widget_invalidate_layout(&ed->base);
}

const char* Rtext_editor_get_text(Rid id) {
	RVN_EDITOR(ed, id, "");
	if (!ed->text_cache_valid) {
		rvn_free(ed->text_cache);
		ed->text_cache = rvn_ed_range(ed, rvn_ed_pos(0, 0), rvn_ed_end(ed), &ed->text_cache_length);
		ed->text_cache_valid = R_TRUE;
	}
	return ed->text_cache;
}

int Rtext_editor_get_length(Rid id) {
	if (!rvn_widget_check(id, R_WIDGET_TEXT_EDITOR, __func__)) return 0;
	Rtext_editor_get_text(id);
	return ((rvn_text_editor_t*)rvn_widget_get(id))->text_cache_length;
}

int Rtext_editor_get_line_count(Rid id) {
	RVN_EDITOR(ed, id, 0);
	return (int)ed->line_count;
}

const char* Rtext_editor_get_line(Rid id, int line) {
	RVN_EDITOR(ed, id, NULL);
	if (line < 0 || line >= (int)ed->line_count) return NULL;
	return ed->lines[line].text;
}

void Rtext_editor_insert_text(Rid id, const char* text) {
	RVN_EDITOR_VOID(ed, id);
	if (!text) return;
	rvn_ed_break_typing(ed);
	rvn_ed_insert(ed, text, -1);
	rvn_ed_break_typing(ed);
	rvn_ed_caret_moved(ed, R_FALSE);
	rvn_ed_notify(widget);
}

void Rtext_editor_set_cursor(Rid id, int line, int column) {
	RVN_EDITOR_VOID(ed, id);
	ed->caret = ed->anchor = rvn_ed_clamp(ed, rvn_ed_pos(line, column));
	rvn_ed_break_typing(ed);
	rvn_ed_caret_moved(ed, R_FALSE);
}

void Rtext_editor_get_cursor(Rid id, int* line, int* column) {
	rvn_widget_t* widget;
	if (line) *line = 0;
	if (column) *column = 0;
	widget = rvn_widget_check(id, R_WIDGET_TEXT_EDITOR, __func__);
	if (!widget) return;
	if (line) *line = ((rvn_text_editor_t*)widget)->caret.line;
	if (column) *column = ((rvn_text_editor_t*)widget)->caret.column;
}

void Rtext_editor_select(Rid id, int from_line, int from_column, int to_line, int to_column) {
	RVN_EDITOR_VOID(ed, id);
	ed->anchor = rvn_ed_clamp(ed, rvn_ed_pos(from_line, from_column));
	ed->caret = rvn_ed_clamp(ed, rvn_ed_pos(to_line, to_column));
	rvn_ed_break_typing(ed);
	rvn_ed_caret_moved(ed, R_FALSE);
}

void Rtext_editor_select_all(Rid id) {
	RVN_EDITOR_VOID(ed, id);
	ed->anchor = rvn_ed_pos(0, 0);
	ed->caret = rvn_ed_end(ed);
	rvn_ed_break_typing(ed);
	rvn_widget_invalidate_paint(widget);
}

int Rtext_editor_get_selection(Rid id, int* from_line, int* from_column, int* to_line, int* to_column) {
	rvn_ed_pos_t from, to;
	RVN_EDITOR(ed, id, R_FALSE);
	from = rvn_ed_sel_min(ed);
	to = rvn_ed_sel_max(ed);
	if (from_line) *from_line = from.line;
	if (from_column) *from_column = from.column;
	if (to_line) *to_line = to.line;
	if (to_column) *to_column = to.column;
	return rvn_ed_has_selection(ed);
}

const char* Rtext_editor_get_selected_text(Rid id) {
	RVN_EDITOR(ed, id, "");
	rvn_free(ed->selection_cache);
	ed->selection_cache = rvn_ed_range(ed, rvn_ed_sel_min(ed), rvn_ed_sel_max(ed), NULL);
	return ed->selection_cache;
}

void Rtext_editor_scroll_to_line(Rid id, int line) {
	rvn_ed_metrics_t m;
	rvn_vec2_t point;
	RVN_EDITOR_VOID(ed, id);
	if (widget->window && widget->window->needs_layout) rvn_window_layout(widget->window);
	rvn_ed_metrics(ed, &m);
	rvn_ed_update_rows(ed, &m);
	rvn_ed_update_bars(ed, &m);
	point = rvn_ed_pos_point(ed, &m, rvn_ed_clamp(ed, rvn_ed_pos(line, 0)));
	/* a third of the way down: the lines before it give some context */
	rvn_scrollbar_set(&ed->vbar, point.y - ed->vbar.viewport * 0.33f);
	rvn_widget_invalidate_paint(widget);
}

void Rtext_editor_undo(Rid id) {
	RVN_EDITOR_VOID(ed, id);
	if (!rvn_ed_undo(ed)) return;
	rvn_ed_caret_moved(ed, R_FALSE);
	rvn_ed_notify(widget);
}

void Rtext_editor_redo(Rid id) {
	RVN_EDITOR_VOID(ed, id);
	if (!rvn_ed_redo(ed)) return;
	rvn_ed_caret_moved(ed, R_FALSE);
	rvn_ed_notify(widget);
}

int Rtext_editor_can_undo(Rid id) {
	RVN_EDITOR(ed, id, R_FALSE);
	return ed->undo_count > 0 && !ed->read_only;
}

int Rtext_editor_can_redo(Rid id) {
	RVN_EDITOR(ed, id, R_FALSE);
	return ed->redo_count > 0 && !ed->read_only;
}

void Rtext_editor_set_read_only(Rid id, int read_only) {
	RVN_EDITOR_VOID(ed, id);
	ed->read_only = read_only != 0;
	rvn_widget_invalidate_paint(widget);
}

void Rtext_editor_set_word_wrap(Rid id, int enabled) {
	RVN_EDITOR_VOID(ed, id);
	if (ed->wrap == (enabled != 0)) return;
	ed->wrap = enabled != 0;
	ed->wrap_width = -1.0f;
	ed->hbar.offset = 0.0f;
	rvn_ed_relayout(ed);
}

void Rtext_editor_set_line_numbers(Rid id, int enabled) {
	RVN_EDITOR_VOID(ed, id);
	ed->line_numbers = enabled != 0;
	rvn_ed_relayout(ed);
}

void Rtext_editor_set_tab_size(Rid id, int spaces) {
	RVN_EDITOR_VOID(ed, id);
	if (spaces < 1 || spaces > 16) {
		rvn_error(R_ERROR_INVALID_VALUE, R_SEVERITY_ERROR, "Rtext_editor_set_tab_size: the size must be between 1 and 16");
		return;
	}
	ed->tab_size = spaces;
	rvn_ed_relayout(ed);
}

void Rtext_editor_set_tab_inserts_spaces(Rid id, int enabled) {
	RVN_EDITOR_VOID(ed, id);
	ed->tab_spaces = enabled != 0;
}

void Rtext_editor_set_syntax(Rid id, RSyntax syntax) {
	RVN_EDITOR_VOID(ed, id);
	if (syntax < R_SYNTAX_NONE || syntax > R_SYNTAX_C) {
		rvn_error(R_ERROR_INVALID_ENUM, R_SEVERITY_ERROR, "Rtext_editor_set_syntax: invalid syntax");
		return;
	}
	rvn_ed_set_language(ed, syntax == R_SYNTAX_XML ? "XML" : syntax == R_SYNTAX_C ? "C" : NULL);
	/* (the font may change with it) */
	rvn_ed_relayout(ed);
}

void Rtext_editor_set_language(Rid id, const char* name) {
	RVN_EDITOR_VOID(ed, id);
	if (!rvn_ed_set_language(ed, name)) {
		rvn_error(R_ERROR_INVALID_VALUE, R_SEVERITY_WARNING, "Rtext_editor_set_language: unknown language \"%s\"", name);
		return;
	}
	rvn_ed_relayout(ed);
}

void Rtext_editor_set_markdown(Rid id, int enabled) {
	RVN_EDITOR_VOID(ed, id);
	rvn_ed_set_language(ed, enabled ? "Markdown" : "");
	rvn_ed_relayout(ed);
}

int Rtext_editor_get_markdown(Rid id) {
	const rvn_language_t* language;
	RVN_EDITOR(ed, id, R_FALSE);
	language = rvn_ed_language(ed);
	return language && (rvn_language_desc(language)->flags & R_LANGUAGE_FORMATTED_TEXT) != 0;
}

const char* Rtext_editor_get_language(Rid id) {
	RVN_EDITOR(ed, id, "");
	return ed->language_name ? ed->language_name : "";
}

RToken Rtext_editor_get_token(Rid id, int line, int column) {
	rvn_span_t spans[RVN_ED_MAX_SPANS];
	const rvn_language_t* language;
	int count = 0, i;
	RVN_EDITOR(ed, id, R_TOKEN_TEXT);
	language = rvn_ed_language(ed);
	if (!language || line < 0 || line >= (int)ed->line_count || column < 0 || column >= ed->lines[line].length) return R_TOKEN_TEXT;
	rvn_ed_lex_until(ed, (uint32_t)line);
	rvn_syntax_line(language, ed->lines[line].text, ed->lines[line].length, ed->lines[line].lex_in, spans, RVN_ED_MAX_SPANS, &count);
	for (i = 0; i < count; i++) {
		if (column >= spans[i].start && column < spans[i].end) return rvn_token_public((rvn_token_t)spans[i].token);
	}
	return R_TOKEN_TEXT;
}

void Rtext_editor_set_completion(Rid id, int enabled) {
	RVN_EDITOR_VOID(ed, id);
	ed->no_completion = !enabled;
	if (!enabled) rvn_ed_complete_close(ed);
}

void Rtext_editor_show_completions(Rid id) {
	RVN_EDITOR_VOID(ed, id);
	RVN_UNUSED(ed);
	if (widget->window && widget->window->needs_layout) rvn_window_layout(widget->window);
	rvn_ed_complete_update(widget, 1);
}

int Rtext_editor_is_completing(Rid id) {
	RVN_EDITOR(ed, id, R_FALSE);
	return rvn_ed_completing(ed);
}

void Rtext_editor_set_completion_callback(Rid id, RCompletionCallback callback, void* user_data) {
	RVN_EDITOR_VOID(ed, id);
	ed->completion_callback = callback;
	ed->completion_user = user_data;
}

void Rtext_editor_add_completion(Rid id, const char* text, const char* detail) {
	rvn_ed_complete_t* list;
	RVN_EDITOR_VOID(ed, id);
	list = (rvn_ed_complete_t*)ed->complete;
	if (!list || !list->collecting) {
		rvn_error(R_ERROR_INVALID_OPERATION, R_SEVERITY_ERROR, "Rtext_editor_add_completion: only from a completion callback");
		return;
	}
	if (!text || !text[0]) return;
	/* (only what continues the word being typed) */
	if ((int)strlen(text) < list->prefix_length || rvn_strnicmp(text, ed->lines[list->start.line].text + list->start.column,
		(size_t)list->prefix_length) != 0) return;
	rvn_edc_add(list, text, (int)strlen(text), detail && detail[0] ? detail : NULL, 0);
}

void Rtext_editor_set_auto_pairs(Rid id, int enabled) {
	RVN_EDITOR_VOID(ed, id);
	ed->no_pairs = !enabled;
}

void Rtext_editor_add_marker(Rid id, int line, int from_column, int to_column, RMarkerKind kind, const char* message) {
	Rtext_editor_add_marker_from(id, 0, line, from_column, to_column, kind, message);
}

void Rtext_editor_add_marker_from(Rid id, int source, int line, int from_column, int to_column, RMarkerKind kind, const char* message) {
	rvn_ed_marker_t* marker;
	RVN_EDITOR_VOID(ed, id);
	if (kind < R_MARKER_ERROR || kind > R_MARKER_INFO) {
		rvn_error(R_ERROR_INVALID_ENUM, R_SEVERITY_ERROR, "Rtext_editor_add_marker: invalid kind");
		return;
	}
	line = rvn_clampi(line, 0, (int)ed->line_count - 1);
	from_column = rvn_clampi(from_column, 0, ed->lines[line].length);
	if (to_column >= 0) to_column = rvn_clampi(to_column, from_column, ed->lines[line].length);
	RVN_ARRAY_RESERVE(ed->markers, ed->marker_cap, ed->marker_count + 1);
	marker = &ed->markers[ed->marker_count++];
	marker->line = line;
	marker->from = from_column;
	marker->to = to_column < 0 ? -1 : to_column;
	marker->kind = kind;
	marker->source = source;
	marker->message = message && message[0] ? rvn_strdup(message) : NULL;
	rvn_widget_invalidate_paint(widget);
}

void Rtext_editor_clear_markers_of_source(Rid id, int source) {
	uint32_t i = 0;
	RVN_EDITOR_VOID(ed, id);
	while (i < ed->marker_count) {
		if (ed->markers[i].source == source) {
			rvn_free(ed->markers[i].message);
			ed->markers[i] = ed->markers[--ed->marker_count];
		} else {
			i++;
		}
	}
	rvn_widget_invalidate_paint(widget);
}

void Rtext_editor_clear_markers(Rid id) {
	uint32_t i;
	RVN_EDITOR_VOID(ed, id);
	for (i = 0; i < ed->marker_count; i++) rvn_free(ed->markers[i].message);
	ed->marker_count = 0;
	if (ed->marker_tip) {
		rvn_str_set(&widget->tooltip, NULL);
		ed->marker_tip = R_FALSE;
		if (widget->window) rvn_window_rearm_tooltip(widget->window);
	}
	rvn_widget_invalidate_paint(widget);
}

int Rtext_editor_get_marker_count(Rid id) {
	RVN_EDITOR(ed, id, 0);
	return (int)ed->marker_count;
}

const char* Rtext_editor_get_marker(Rid id, int index, int* line, int* from_column, int* to_column, RMarkerKind* kind) {
	RVN_EDITOR(ed, id, NULL);
	const rvn_ed_marker_t* marker;
	if (index < 0 || index >= (int)ed->marker_count) return NULL;
	marker = &ed->markers[index];
	if (line) *line = marker->line;
	if (from_column) *from_column = marker->from;
	if (to_column) *to_column = marker->to;
	if (kind) *kind = marker->kind;
	return marker->message;
}

void Rtext_editor_clear_markers_of_line(Rid id, int line) {
	uint32_t i = 0;
	RVN_EDITOR_VOID(ed, id);
	while (i < ed->marker_count) {
		if (ed->markers[i].line == line) {
			rvn_free(ed->markers[i].message);
			ed->markers[i] = ed->markers[--ed->marker_count];
		} else {
			i++;
		}
	}
	rvn_widget_invalidate_paint(widget);
}

/* ---------------------------------------------------------------------------
 *  Search, replace, folding, snippets: the public functions
 * ------------------------------------------------------------------------- */

int Rtext_editor_set_search(Rid id, const char* pattern, Ruint flags) {
	const char* error = NULL;
	rvn_regex_t* check = NULL;
	RVN_EDITOR(ed, id, R_FALSE);
	if (pattern && pattern[0] && (flags & R_SEARCH_REGEX)) {
		check = rvn_regex_compile(pattern, (flags & R_SEARCH_IGNORE_CASE) != 0, R_TRUE, &error);
		if (!check) {
			rvn_error(R_ERROR_INVALID_VALUE, R_SEVERITY_ERROR, "Rtext_editor_set_search: %s", error ? error : "the pattern is not valid");
			return R_FALSE;
		}
		rvn_regex_free(check);
	}
	rvn_str_set(&ed->search_pattern, pattern && pattern[0] ? pattern : NULL);
	ed->search_flags = flags & (R_SEARCH_REGEX | R_SEARCH_IGNORE_CASE | R_SEARCH_WHOLE_WORD);
	ed->search_version = 0;
	rvn_ed_update_matches(ed);
	rvn_widget_invalidate_paint(widget);
	return R_TRUE;
}

const char* Rtext_editor_get_search(Rid id) {
	RVN_EDITOR(ed, id, "");
	return ed->search_pattern ? ed->search_pattern : "";
}

Ruint Rtext_editor_get_search_flags(Rid id) {
	RVN_EDITOR(ed, id, 0);
	return ed->search_flags;
}

int Rtext_editor_count_matches(Rid id) {
	RVN_EDITOR(ed, id, 0);
	rvn_ed_update_matches(ed);
	return (int)ed->match_count;
}

int Rtext_editor_get_match(Rid id, int index, int* from_line, int* from_column, int* to_line, int* to_column) {
	RVN_EDITOR(ed, id, R_FALSE);
	rvn_ed_update_matches(ed);
	if (index < 0 || index >= (int)ed->match_count) return R_FALSE;
	if (from_line) *from_line = ed->matches[index].from.line;
	if (from_column) *from_column = ed->matches[index].from.column;
	if (to_line) *to_line = ed->matches[index].to.line;
	if (to_column) *to_column = ed->matches[index].to.column;
	return R_TRUE;
}

int Rtext_editor_find_next(Rid id, int backwards) {
	uint32_t i;
	int found = -1;
	RVN_EDITOR(ed, id, R_FALSE);
	rvn_ed_update_matches(ed);
	if (ed->match_count == 0) return R_FALSE;
	if (backwards) {
		/* the last match that ends before the caret, else the last one of all */
		for (i = ed->match_count; i > 0; i--) {
			if (rvn_ed_pos_cmp(ed->matches[i - 1].to, ed->caret) < 0) {
				found = (int)i - 1;
				break;
			}
		}
		if (found < 0) found = (int)ed->match_count - 1;
	} else {
		/* the first match that starts after the end of the selection, else the first one of all */
		rvn_ed_pos_t after = rvn_ed_has_selection(ed) ? rvn_ed_sel_max(ed) : ed->caret;
		for (i = 0; i < ed->match_count; i++) {
			if (rvn_ed_pos_cmp(ed->matches[i].from, after) >= 0) {
				found = (int)i;
				break;
			}
		}
		if (found < 0) found = 0;
	}
	ed->anchor = ed->matches[found].from;
	ed->caret = ed->matches[found].to;
	ed->goal_x = -1.0f;
	rvn_ed_caret_moved(ed, R_FALSE);
	return R_TRUE;
}

/* Where `$1`, `$&` and the escapes of a replacement go, into a text. */
static void rvn_ed_expand(const char* replacement, const char* text, int from, int to, const int* groups, int group_count,
	rvn_text_t* out) {
	int i;
	for (i = 0; replacement[i]; ) {
		char c = replacement[i];
		if (c == '$' && replacement[i + 1] >= '0' && replacement[i + 1] <= '9') {
			int group = replacement[i + 1] - '0';
			i += 2;
			if (group * 2 + 1 < group_count * 2 && groups[group * 2] >= 0) {
				rvn_text_append(out, text + from + groups[group * 2], (size_t)(groups[group * 2 + 1] - groups[group * 2]));
			}
			continue;
		}
		/* "$<n>": a group whose number is written between angle brackets */
		if (c == '$' && replacement[i + 1] == '<') {
			int k = i + 2, group = 0, digits = 0;
			while (replacement[k] >= '0' && replacement[k] <= '9') {
				group = group * 10 + (replacement[k] - '0');
				k++;
				digits++;
			}
			if (digits > 0 && replacement[k] == '>') {
				if (group * 2 + 1 < group_count * 2 && groups[group * 2] >= 0) {
					rvn_text_append(out, text + from + groups[group * 2], (size_t)(groups[group * 2 + 1] - groups[group * 2]));
				}
				i = k + 1;
				continue;
			}
		}
		/* "${n}" (a group) and "${n:text}" (the group, or that text when it did not take part) */
		if (c == '$' && replacement[i + 1] == '{') {
			int k = i + 2, group = 0, digits = 0, close;
			while (replacement[k] >= '0' && replacement[k] <= '9') {
				group = group * 10 + (replacement[k] - '0');
				k++;
				digits++;
			}
			close = k;
			while (replacement[close] && replacement[close] != '}') close++;
			if (digits > 0 && replacement[close] == '}' && (replacement[k] == '}' || replacement[k] == ':')) {
				if (group * 2 + 1 < group_count * 2 && groups[group * 2] >= 0) {
					rvn_text_append(out, text + from + groups[group * 2], (size_t)(groups[group * 2 + 1] - groups[group * 2]));
				} else if (replacement[k] == ':') {
					int begin = k + 1;
					while (begin < close && replacement[begin] == ' ') begin++;
					while (close > begin && replacement[close - 1] == ' ') close--;
					rvn_text_append(out, replacement + begin, (size_t)(close - begin));
				}
				i = close + 1;
				continue;
			}
		}
		if (c == '$' && replacement[i + 1] == '&') {
			rvn_text_append(out, text + from, (size_t)(to - from));
			i += 2;
			continue;
		}
		if (c == '$' && replacement[i + 1] == '$') {
			rvn_text_add(out, "$");
			i += 2;
			continue;
		}
		if (c == '\\' && (replacement[i + 1] == 'n' || replacement[i + 1] == 't' || replacement[i + 1] == '\\')) {
			rvn_text_add(out, replacement[i + 1] == 'n' ? "\n" : replacement[i + 1] == 't' ? "\t" : "\\");
			i += 2;
			continue;
		}
		rvn_text_append(out, replacement + i, 1);
		i++;
	}
}

static rvn_text_t g_expand;

const char* Rtext_editor_expand_replacement(const char* replacement, const char* text, int from, int to,
	const int* groups, int group_count) {
	RVN_API_GUARD(NULL);
	if (!replacement) return NULL;
	g_expand.length = 0;
	if (g_expand.data) g_expand.data[0] = '\0';
	if (!text) text = "";
	rvn_ed_expand(replacement, text, from, to, groups, group_count, &g_expand);
	return g_expand.data ? g_expand.data : "";
}

/* Frees what the editor keeps between calls (Rshutdown). */
void rvn_text_editor_shutdown(void) {
	rvn_free(g_expand.data);
	g_expand.data = NULL;
	g_expand.length = 0;
	g_expand.cap = 0;
	rvn_snippets_shutdown();
}

/* The replacement of the current selection, with the groups of the regex that made it. */
static char* rvn_ed_replacement_text(rvn_text_editor_t* ed, const char* replacement, rvn_ed_pos_t from, rvn_ed_pos_t to) {
	char* selected = rvn_ed_range(ed, from, to, NULL);
	rvn_text_t out;
	int groups[2] = { 0, (int)strlen(selected) };
	memset(&out, 0, sizeof(out));
	rvn_ed_expand(replacement ? replacement : "", selected, 0, (int)strlen(selected), groups, 1, &out);
	rvn_free(selected);
	return out.data ? out.data : rvn_strdup("");
}

int Rtext_editor_replace(Rid id, const char* text) {
	char* replacement;
	rvn_ed_pos_t from, to;
	RVN_EDITOR(ed, id, R_FALSE);
	if (ed->read_only) return R_FALSE;
	/* (the selection, when the last Find put it there) */
	if (!rvn_ed_has_selection(ed)) {
		rvn_ed_update_matches(ed);
		if (ed->match_count == 0) return R_FALSE;
		from = ed->matches[0].from;
		to = ed->matches[0].to;
	} else {
		from = rvn_ed_sel_min(ed);
		to = rvn_ed_sel_max(ed);
	}
	replacement = rvn_ed_replacement_text(ed, text, from, to);
	{
		Rid wid = widget->id;
		rvn_ed_break_typing(ed);
		ed->caret = ed->anchor = rvn_ed_replace(ed, from, to, replacement, -1, R_TRUE);
		rvn_ed_break_typing(ed);
		rvn_free(replacement);
		rvn_ed_update_matches(ed);
		rvn_ed_caret_moved(ed, R_FALSE);
		rvn_ed_notify(widget);
		if (!rvn_widget_get(wid)) return R_TRUE;
	}
	Rtext_editor_find_next(id, R_FALSE);
	return R_TRUE;
}

int Rtext_editor_replace_all(Rid id, const char* text) {
	int replaced = 0;
	RVN_EDITOR(ed, id, 0);
	if (ed->read_only) return 0;
	rvn_ed_update_matches(ed);
	if (ed->match_count == 0) return 0;
	/* from the end to the start, so that the places of the matches do not move */
	rvn_ed_group_begin(ed);
	while (ed->match_count > 0) {
		rvn_ed_match_t match = ed->matches[ed->match_count - 1];
		char* replacement = rvn_ed_replacement_text(ed, text, match.from, match.to);
		rvn_ed_replace(ed, match.from, match.to, replacement, -1, R_TRUE);
		rvn_free(replacement);
		ed->match_count--;
		replaced++;
	}
	rvn_ed_group_end(ed);
	ed->caret = ed->anchor = rvn_ed_pos(0, 0);
	rvn_ed_update_matches(ed);
	rvn_ed_caret_moved(ed, R_FALSE);
	rvn_ed_notify(widget);
	return replaced;
}

/* Comments or uncomments the lines of the selection, as one step of the history. */
int Rtext_editor_toggle_comment(Rid id) {
	int toggled;
	RVN_EDITOR(ed, id, 0);
	if (ed->read_only) return 0;
	rvn_ed_group_begin(ed);
	toggled = rvn_ed_toggle_comment(ed);
	rvn_ed_group_end(ed);
	if (toggled) rvn_ed_notify(widget);
	return toggled;
}

void Rtext_editor_begin_edit(Rid id) {
	RVN_EDITOR_VOID(ed, id);
	rvn_ed_group_begin(ed);
}

void Rtext_editor_end_edit(Rid id) {
	RVN_EDITOR_VOID(ed, id);
	rvn_ed_group_end(ed);
}

int Rtext_editor_is_modified(Rid id) {
	RVN_EDITOR(ed, id, 0);
	return ed->version != ed->saved_version;
}

void Rtext_editor_set_modified(Rid id, int modified) {
	RVN_EDITOR_VOID(ed, id);
	ed->saved_version = modified ? ed->version - 1 : ed->version;
}

void Rtext_editor_set_folding(Rid id, int enabled) {
	RVN_EDITOR_VOID(ed, id);
	ed->no_folding = !enabled;
	if (ed->no_folding) {
		uint32_t i;
		for (i = 0; i < ed->fold_count; i++) ed->folds[i].folded = R_FALSE;
	} else {
		rvn_ed_update_folds(ed);
	}
	rvn_ed_relayout(ed);
}

int Rtext_editor_get_folding(Rid id) {
	RVN_EDITOR(ed, id, R_FALSE);
	return !ed->no_folding;
}

int Rtext_editor_toggle_fold(Rid id) {
	int fold;
	RVN_EDITOR(ed, id, R_FALSE);
	if (ed->no_folding) return R_FALSE;
	fold = rvn_ed_fold_at_caret(ed);
	if (fold < 0) return R_FALSE;
	ed->folds[fold].folded = !ed->folds[fold].folded;
	/* (the caret goes to the line of the block when it was inside it) */
	ed->caret = ed->anchor = rvn_ed_pos(ed->folds[fold].start, ed->caret.line == ed->folds[fold].start ? ed->caret.column : 0);
	rvn_ed_relayout(ed);
	rvn_ed_caret_moved(ed, R_FALSE);
	return R_TRUE;
}

int Rtext_editor_fold_line(Rid id, int line) {
	int fold;
	RVN_EDITOR(ed, id, R_FALSE);
	if (ed->no_folding) return R_FALSE;
	rvn_ed_update_folds(ed);
	fold = rvn_ed_fold_line_at(ed, line);
	if (fold < 0) return R_FALSE;
	ed->folds[fold].folded = R_TRUE;
	rvn_ed_relayout(ed);
	return R_TRUE;
}

int Rtext_editor_unfold_line(Rid id, int line) {
	int fold;
	RVN_EDITOR(ed, id, R_FALSE);
	fold = rvn_ed_fold_line_at(ed, line);
	if (fold < 0) return R_FALSE;
	ed->folds[fold].folded = R_FALSE;
	rvn_ed_relayout(ed);
	return R_TRUE;
}

int Rtext_editor_is_folded(Rid id, int line) {
	int fold;
	RVN_EDITOR(ed, id, R_FALSE);
	fold = rvn_ed_fold_line_at(ed, line);
	return fold >= 0 && ed->folds[fold].folded;
}

void Rtext_editor_unfold_all(Rid id) {
	uint32_t i;
	RVN_EDITOR_VOID(ed, id);
	for (i = 0; i < ed->fold_count; i++) ed->folds[i].folded = R_FALSE;
	rvn_ed_relayout(ed);
}

int Rtext_editor_get_folded_count(Rid id) {
	uint32_t i;
	int folded = 0;
	RVN_EDITOR(ed, id, 0);
	for (i = 0; i < ed->fold_count; i++) folded += ed->folds[i].folded ? 1 : 0;
	return folded;
}

int Rtext_editor_get_fold_start(Rid id, int line) {
	const rvn_ed_fold_t* fold;
	RVN_EDITOR(ed, id, -1);
	fold = rvn_ed_fold_hiding(ed, line);
	return fold ? fold->start : -1;
}

int Rtext_editor_insert_snippet(Rid id, const char* name) {
	int which;
	RVN_EDITOR(ed, id, R_FALSE);
	which = Rsnippet_find(name);
	if (which < 0) return R_FALSE;
	return rvn_ed_snippet_insert(ed, which);
}

int Rtext_editor_snippet_at_caret(Rid id) {
	RVN_EDITOR(ed, id, -1);
	return rvn_ed_snippet_at_caret(ed);
}

void Rtext_editor_set_snippet_completion(Rid id, int enabled) {
	RVN_EDITOR_VOID(ed, id);
	ed->no_snippets = !enabled;
}

int Rtext_editor_is_in_snippet(Rid id) {
	RVN_EDITOR(ed, id, R_FALSE);
	return ed->place_count > 0;
}

void Rtext_editor_set_multi_cursor(Rid id, int enabled) {
	RVN_EDITOR_VOID(ed, id);
	ed->multi_cursor = enabled ? R_TRUE : R_FALSE;
	if (!ed->multi_cursor) ed->caret_count = 0;
}

int Rtext_editor_get_multi_cursor(Rid id) {
	RVN_EDITOR(ed, id, R_FALSE);
	return ed->multi_cursor;
}

int Rtext_editor_get_caret_count(Rid id) {
	RVN_EDITOR(ed, id, 0);
	return 1 + (int)ed->caret_count;
}

int Rtext_editor_get_caret(Rid id, int index, int* line, int* column) {
	rvn_ed_pos_t at;
	RVN_EDITOR(ed, id, R_FALSE);
	if (index < 0 || index > (int)ed->caret_count) return R_FALSE;
	at = index == 0 ? ed->caret : ed->carets[index - 1].caret;
	if (line) *line = at.line;
	if (column) *column = at.column;
	return R_TRUE;
}

int Rtext_editor_add_caret(Rid id, int line, int column) {
	rvn_ed_caret_t* caret;
	RVN_EDITOR(ed, id, R_FALSE);
	if (!ed->multi_cursor) return R_FALSE;
	if (ed->caret_count >= 255) return R_FALSE;
	RVN_ARRAY_RESERVE(ed->carets, ed->caret_cap, ed->caret_count + 1);
	caret = &ed->carets[ed->caret_count++];
	caret->caret = rvn_ed_clamp(ed, rvn_ed_pos(line, column));
	caret->anchor = caret->caret;
	return R_TRUE;
}

void Rtext_editor_clear_carets(Rid id) {
	RVN_EDITOR_VOID(ed, id);
	ed->caret_count = 0;
}

int Rtext_editor_select_next_occurrence(Rid id) {
	char* selected;
	rvn_ed_pos_t from, to, after;
	RVN_EDITOR(ed, id, R_FALSE);
	if (!ed->multi_cursor || !rvn_ed_has_selection(ed)) return R_FALSE;
	from = rvn_ed_sel_min(ed);
	to = rvn_ed_sel_max(ed);
	selected = rvn_ed_range(ed, from, to, NULL);
	if (selected[0]) {
		rvn_ed_match_t* matches = NULL;
		uint32_t count = 0, cap = 0, i;
		int length = (int)strlen(selected);
		int line;
		/* (the next one comes after what every caret already selects, so that pressing again goes on) */
		after = to;
		for (i = 0; i < ed->caret_count; i++) {
			rvn_ed_pos_t end = rvn_ed_pos_cmp(ed->carets[i].caret, ed->carets[i].anchor) > 0 ? ed->carets[i].caret : ed->carets[i].anchor;
			if (rvn_ed_pos_cmp(end, after) > 0) after = end;
		}
		for (line = after.line; line < (int)ed->line_count; line++) {
			const rvn_ed_line_t* l = &ed->lines[line];
			int begin = line == after.line ? after.column : 0, k;
			for (k = begin; k + length <= l->length; k++) {
				if (memcmp(l->text + k, selected, (size_t)length) != 0) continue;
				RVN_ARRAY_RESERVE(matches, cap, count + 1);
				matches[count].from = rvn_ed_pos(line, k);
				matches[count].to = rvn_ed_pos(line, k + length);
				count++;
				break;
			}
			if (count > 0) break;
		}
		if (count > 0 && ed->caret_count < 255) {
			rvn_ed_caret_t* caret;
			/* (what was selected stays selected, with a caret of its own, and the new one is the main selection) */
			RVN_ARRAY_RESERVE(ed->carets, ed->caret_cap, ed->caret_count + 1);
			caret = &ed->carets[ed->caret_count++];
			caret->anchor = from;
			caret->caret = to;
			ed->anchor = matches[0].from;
			ed->caret = matches[0].to;
		}
		rvn_free(matches);
	}
	rvn_free(selected);
	return R_TRUE;
}

int Rtext_editor_get_word_count(Rid id) {
	int line, words = 0;
	const RLanguageDesc* desc;
	RVN_EDITOR(ed, id, 0);
	desc = rvn_ed_language_desc(ed);
	for (line = 0; line < (int)ed->line_count; line++) {
		const rvn_ed_line_t* l = &ed->lines[line];
		const char* comment = desc && desc->line_comment && desc->line_comment[0] ? desc->line_comment : NULL;
		int tex = desc && (desc->flags & R_LANGUAGE_TEX);
		int comment_length = comment ? (int)strlen(comment) : 0;
		int i = 0;
		while (i < l->length) {
			char c = l->text[i];
			/* (a command of TeX, with its name, is not a word of the text; neither is what a backslash escapes) */
			if (tex && c == '\\' && i + 1 < l->length) {
				i++;
				while (i < l->length && ((l->text[i] >= 'a' && l->text[i] <= 'z') || (l->text[i] >= 'A' && l->text[i] <= 'Z'))) i++;
				if (i < l->length && !rvn_ed_is_blank(l->text[i])) i++;
				continue;
			}
			/* (in a comment there are no words, in the comment of the language this editor shows) */
			if (comment && i + comment_length <= l->length &&
				memcmp(l->text + i, comment, (size_t)comment_length) == 0) break;
			if (!rvn_ed_is_word_byte(c)) {
				i++;
				continue;
			}
			words++;
			while (i < l->length && rvn_ed_is_word_byte(l->text[i])) i++;
		}
	}
	return words;
}

int Rtext_editor_get_character_count(Rid id) {
	int line, characters = 0;
	RVN_EDITOR(ed, id, 0);
	for (line = 0; line < (int)ed->line_count; line++) characters += rvn_utf8_count(ed->lines[line].text, ed->lines[line].length);
	return characters;
}

int Rtext_editor_get_non_empty_line_count(Rid id) {
	int line, count = 0;
	RVN_EDITOR(ed, id, 0);
	for (line = 0; line < (int)ed->line_count; line++) {
		const rvn_ed_line_t* l = &ed->lines[line];
		int i;
		for (i = 0; i < l->length && (l->text[i] == ' ' || l->text[i] == '\t'); i++) {}
		if (i < l->length) count++;
	}
	return count;
}

int Rtext_editor_get_current_line(Rid id) {
	RVN_EDITOR(ed, id, 0);
	return ed->caret.line + 1;
}

void Rtext_editor_set_font(Rid id, Rid font, float size) {
	RVN_EDITOR_VOID(ed, id);
	ed->font = font;
	ed->font_size = size > 0.0f ? size : 0.0f;
	rvn_ed_relayout(ed);
}

void Rtext_editor_set_zoom(Rid id, float zoom) {
	RVN_EDITOR_VOID(ed, id);
	if (zoom <= 0.0f) zoom = 1.0f;
	/* (from the program: the top of the view stays where it is, nobody is told) */
	if (widget->rect.w > 0.0f && widget->rect.h > 0.0f) {
		rvn_ed_set_zoom(ed, zoom, 0.0f, 0.0f, R_FALSE);
	} else {
		ed->zoom = rvn_clampf(zoom, RVN_ED_ZOOM_MIN, RVN_ED_ZOOM_MAX);
		rvn_ed_relayout(ed);
	}
}

float Rtext_editor_get_zoom(Rid id) {
	RVN_EDITOR(ed, id, 1.0f);
	return ed->zoom > 0.0f ? ed->zoom : 1.0f;
}

void Rtext_editor_set_zoomable(Rid id, int zoomable) {
	RVN_EDITOR_VOID(ed, id);
	ed->no_zoom = !zoomable;
}

void Rtext_editor_set_zoom_callback(Rid id, RTextEditorZoomCallback callback, void* user_data) {
	RVN_EDITOR_VOID(ed, id);
	ed->zoom_callback = callback;
	ed->zoom_user = user_data;
}

void Rtext_editor_set_placeholder(Rid id, const char* text) {
	RVN_EDITOR_VOID(ed, id);
	rvn_str_set(&ed->placeholder, text);
	rvn_widget_invalidate_paint(widget);
}

void Rtext_editor_set_changed_callback(Rid id, RTextEditorChangedCallback callback, void* user_data) {
	RVN_EDITOR_VOID(ed, id);
	ed->callback = callback;
	ed->user_data = user_data;
}
