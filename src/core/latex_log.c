/**
 * Raven - core/latex_log.c
 * The log of pdflatex, xelatex or lualatex, read as a list of diagnostics.
 *
 * A log is written for a human reading a terminal, not for a program: files
 * are opened with a parenthesis glued to the text before them, a line number
 * is printed as "l.42" followed by the source, and a box that is too wide is
 * reported over two lines. The rules below are the ones the editors that do
 * this job use (TeXShop, LaTeX Workshop):
 *
 *   - a line that starts with '!' is an error ("! Undefined control sequence.",
 *     "! LaTeX Error: File `x.sty' not found."); the line number comes from the
 *     "l.<n> <source>" line that follows it, and the file from the innermost
 *     file still open;
 *   - "LaTeX Warning:", "LaTeX Font Warning:", "Package <name> Warning:" and
 *     "Class <name> Warning:" are warnings, with "on input line <n>." when the
 *     message gives one; a "Package <name> Error:" is an error;
 *   - "Overfull \hbox (12.3pt too wide) in paragraph at lines 10--12" and its
 *     underfull twin are boxes, with the first line of the pair;
 *   - "Emergency stop", "Fatal error occurred" and "No pages of output" are
 *     errors;
 *   - everything from "Here is how much of TeX's memory" on is the summary
 *     that TeX prints at the end, and holds no diagnostic.
 *
 * Files: TeX opens a file with '(' and closes it with ')'. The log has no
 * other delimiter, so a depth counter is kept: a '(' that is not a size, a
 * page number or a word of a message opens a file, and every ')' closes the
 * innermost one. Every log reader does this. It gives up on a log in which a
 * file name itself contains a parenthesis, which is rare enough to accept for
 * the sake of keeping the code readable.
 */

#include "context.h"
#include "handle.h"
#include "../../include/raven/raven_latex.h"

/* ---------------------------------------------------------------------------
 *  Text helpers
 * ------------------------------------------------------------------------- */

static int rvn_log_is_space(char c) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static const char* rvn_log_skip_space(const char* cursor, const char* end) {
	while (cursor < end && rvn_log_is_space(*cursor)) cursor++;
	return cursor;
}

static const char* rvn_log_line_end(const char* cursor, const char* end) {
	while (cursor < end && *cursor != '\n') cursor++;
	return cursor;
}

/** The start of the next line, or `end`. */
static const char* rvn_log_next_line(const char* line_end, const char* end) {
	return line_end < end ? line_end + 1 : end;
}

static int rvn_log_starts_with(const char* cursor, const char* end, const char* text) {
	size_t length = strlen(text);
	return (size_t)(end - cursor) >= length && memcmp(cursor, text, length) == 0;
}

/**
 * Copies a piece of the log, trimmed. The NUL bytes that pdflatex writes in
 * the names of some files are dropped: a message travels through the C API,
 * where a NUL ends a string.
 */
static char* rvn_log_copy(const char* begin, const char* end) {
	const char* cursor;
	size_t length = 0;
	char* text;
	begin = rvn_log_skip_space(begin, end);
	while (end > begin && rvn_log_is_space(end[-1])) end--;
	for (cursor = begin; cursor < end; cursor++) {
		if (*cursor != '\0') length++;
	}
	text = (char*)rvn_alloc(length + 1);
	length = 0;
	for (cursor = begin; cursor < end; cursor++) {
		if (*cursor != '\0') text[length++] = *cursor;
	}
	text[length] = '\0';
	return text;
}

/** Parses a decimal number at *cursor. R_FALSE when there is none. */
static int rvn_log_number(const char** cursor, const char* end, int* value) {
	const char* start = *cursor;
	int number = 0;
	while (start < end && *start >= '0' && *start <= '9') {
		if (number < 100000000) number = number * 10 + (*start - '0');
		start++;
	}
	if (start == *cursor) return R_FALSE;
	*cursor = start;
	*value = number;
	return R_TRUE;
}

/** Finds "on input line <n>" in a warning: it is where its line number lives. */
static int rvn_log_input_line(const char* begin, const char* end, int* line) {
	const char* cursor;
	for (cursor = begin; cursor + 13 <= end; cursor++) {
		if (memcmp(cursor, "on input line", 13) == 0) {
			const char* at = rvn_log_skip_space(cursor + 13, end);
			if (rvn_log_number(&at, end, line)) return R_TRUE;
		}
	}
	return R_FALSE;
}

/** "l.<n> <source>" says where TeX stopped. */
static int rvn_log_line_marker(const char* begin, const char* end, int* line) {
	const char* cursor = rvn_log_skip_space(begin, end);
	if (cursor + 2 > end || cursor[0] != 'l' || cursor[1] != '.') return R_FALSE;
	cursor += 2;
	return rvn_log_number(&cursor, end, line);
}

/** "Overfull \hbox (...) in paragraph at lines 10--12": the first number of "lines". */
static int rvn_log_box_lines(const char* begin, const char* end) {
	const char* cursor;
	int line = 0;
	for (cursor = begin; cursor + 5 <= end; cursor++) {
		if (memcmp(cursor, "lines", 5) == 0 && (cursor == begin || rvn_log_is_space(cursor[-1]))) {
			const char* at = rvn_log_skip_space(cursor + 5, end);
			if (rvn_log_number(&at, end, &line)) return line;
		}
	}
	return 0;
}

/* ---------------------------------------------------------------------------
 *  The files TeX has open
 * ------------------------------------------------------------------------- */

typedef struct rvn_log_files_t {
	char** names;
	uint32_t count;
	uint32_t cap;
} rvn_log_files_t;

/**
 * R_TRUE when what follows a '(' is a file name rather than a size, a page
 * number or a word of a message. A name has a dot or a separator in it and
 * does not start with a digit, a bracket or a dash.
 */
static int rvn_log_is_file_name(const char* begin, const char* end) {
	const char* cursor;
	int has_dot = 0;
	if (begin >= end) return R_FALSE;
	if (*begin == '$' || *begin == '[' || *begin == '-' || *begin == '\\' || *begin == '"' || *begin == '\'') return R_FALSE;
	if (*begin >= '0' && *begin <= '9') return R_FALSE;
	for (cursor = begin; cursor < end; cursor++) {
		if (*cursor == '.') has_dot = 1;
#if defined(_WIN32)
		if (*cursor == '\\' || *cursor == '/') has_dot = 1;
#else
		if (*cursor == '/') has_dot = 1;
#endif
	}
	/* "12.3pt too wide" has a dot but ends in "pt": no file does */
	if (end - begin > 2 && rvn_strnicmp(end - 2, "pt", 2) == 0) return R_FALSE;
	return has_dot;
}

static void rvn_log_files_push(rvn_log_files_t* files, const char* begin, const char* end) {
	char* name;
	while (end > begin && rvn_log_is_space(end[-1])) end--;
	if (end <= begin) return;
	name = rvn_log_copy(begin, end);
	RVN_ARRAY_RESERVE(files->names, files->cap, files->count + 1);
	files->names[files->count++] = name;
}

static void rvn_log_files_pop(rvn_log_files_t* files) {
	if (!files->count) return;
	rvn_free(files->names[--files->count]);
}

static const char* rvn_log_files_current(const rvn_log_files_t* files) {
	return files->count ? files->names[files->count - 1] : NULL;
}

static void rvn_log_files_free(rvn_log_files_t* files) {
	uint32_t i;
	for (i = 0; i < files->count; i++) rvn_free(files->names[i]);
	rvn_free(files->names);
	memset(files, 0, sizeof(*files));
}

/** Reads the parentheses of one line and keeps the list of open files up to date. */
static void rvn_log_track_files(const char* begin, const char* end, rvn_log_files_t* files) {
	const char* cursor = begin;
	while (cursor < end) {
		if (*cursor == '(') {
			const char* name = cursor + 1;
			const char* stop = name;
			while (stop < end && !rvn_log_is_space(*stop) && *stop != '(' && *stop != ')') stop++;
			if (rvn_log_is_file_name(name, stop)) {
				rvn_log_files_push(files, name, stop);
				cursor = stop;
				continue;
			}
			/* A parenthesis that opens no file: a size, a page number, "(see
			 * the...)", "(hyperref)". Its ')' must not close a file, so the
			 * whole pair is skipped. */
			{
				int depth = 1;
				const char* skip = cursor + 1;
				while (skip < end && depth > 0) {
					if (*skip == '(') depth++;
					else if (*skip == ')') depth--;
					skip++;
				}
				cursor = skip;
				continue;
			}
		} else if (*cursor == ')') {
			rvn_log_files_pop(files);
		}
		cursor++;
	}
}

/* ---------------------------------------------------------------------------
 *  Messages
 * ------------------------------------------------------------------------- */

typedef struct rvn_log_list_t {
	RLogMessage* items;
	uint32_t count;
	uint32_t cap;
} rvn_log_list_t;

/** Stores a message; the strings in it are owned by the list. */
static void rvn_log_add(rvn_log_list_t* list, RLogKind kind, const char* file, int line,
	const char* message, const char* context) {
	RLogMessage* item;
	RVN_ARRAY_RESERVE(list->items, list->cap, list->count + 1);
	item = &list->items[list->count++];
	item->kind = kind;
	item->file = rvn_strdup(file);
	item->line = line > 0 ? line : 0;
	item->message = rvn_strdup(message ? message : "");
	item->context = rvn_strdup(context);
}

/**
 * The lines that follow an error: the hint of TeX ("See the LaTeX manual..."),
 * the "l.42 \foo" line with the source of the error, and what comes after, up
 * to a few lines. Stops at a blank line and at the next diagnostic.
 */
static const char* rvn_log_error_context(const char* cursor, const char* end, const char* first,
	const char* first_end, rvn_text_t* context) {
	const char* stop = cursor;
	int lines = 0;
	if (first && first_end > first) {
		rvn_text_append(context, first, (size_t)(first_end - first));
		lines++;
	} else {
		/* TeX puts an empty line between the message and the hint that follows */
		while (stop < end) {
			const char* skip_end = rvn_log_line_end(stop, end);
			if (rvn_log_skip_space(stop, skip_end) < skip_end) break;
			stop = rvn_log_next_line(skip_end, end);
		}
	}
	while (stop < end && lines < 5) {
		const char* line_end = rvn_log_line_end(stop, end);
		const char* trimmed = rvn_log_skip_space(stop, line_end);
		if (trimmed >= line_end) break;
		if (rvn_log_starts_with(trimmed, line_end, "! ") ||
			rvn_log_starts_with(trimmed, line_end, "Overfull ") ||
			rvn_log_starts_with(trimmed, line_end, "Underfull ") ||
			rvn_log_starts_with(trimmed, line_end, "LaTeX Warning:") ||
			rvn_log_starts_with(trimmed, line_end, "LaTeX Font Warning:")) {
			break;
		}
		if (context->length) rvn_text_add(context, "\n");
		rvn_text_append(context, stop, (size_t)(line_end - stop));
		stop = rvn_log_next_line(line_end, end);
		lines++;
	}
	return stop;
}

/**
 * A warning whose sentence may go on over indented lines. The log wraps a
 * long message and indents the continuation, so those lines belong to it
 * until one ends with a period. Returns where the warning ends.
 */
static const char* rvn_log_warning_text(const char* cursor, const char* end, char** text) {
	rvn_text_t full;
	const char* line_end = rvn_log_line_end(cursor, end);
	memset(&full, 0, sizeof(full));
	rvn_text_append(&full, cursor, (size_t)(line_end - cursor));
	while (line_end < end) {
		const char* next = line_end + 1;
		const char* next_end = rvn_log_line_end(next, end);
		const char* trimmed = rvn_log_skip_space(next, next_end);
		size_t length = full.length;
		if (trimmed >= next_end) break;
		/* A wrapped message is indented, or prefixed with "(package)": both
		 * belong to it. Anything else is a new line of the log. */
		if (trimmed == next && *trimmed != '(') break;
		if (trimmed != next && *trimmed == '(') {
			/* "(hyperref)                removing ...": the name is not text */
			const char* close = trimmed;
			while (close < next_end && *close != ')') close++;
			if (close < next_end) trimmed = rvn_log_skip_space(close + 1, next_end);
			else break;
		}
		if (trimmed >= next_end) { line_end = next_end; continue; }
		if (length && full.data[length - 1] == '.') break;  /* the sentence is over */
		if (rvn_log_starts_with(trimmed, next_end, "! ") ||
			rvn_log_starts_with(trimmed, next_end, "Overfull ") ||
			rvn_log_starts_with(trimmed, next_end, "Underfull ") ||
			rvn_log_starts_with(trimmed, next_end, "LaTeX Warning:") ||
			rvn_log_starts_with(trimmed, next_end, "LaTeX Font Warning:") ||
			rvn_log_starts_with(trimmed, next_end, "Package ") ||
			rvn_log_starts_with(trimmed, next_end, "Class ")) {
			break;
		}
		rvn_text_add(&full, " ");
		rvn_text_append(&full, trimmed, (size_t)(next_end - trimmed));
		line_end = next_end;
	}
	*text = full.data ? full.data : rvn_strdup("");
	return line_end;
}

/** R_TRUE when the line is "Package <name> Warning: ...", "Class <name> Warning: ..." or the LaTeX ones. */
static int rvn_log_is_warning(const char* begin, const char* end) {
	const char* cursor;
	if (rvn_log_starts_with(begin, end, "LaTeX Warning:")) return R_TRUE;
	if (rvn_log_starts_with(begin, end, "LaTeX Font Warning:")) return R_TRUE;
	if (!rvn_log_starts_with(begin, end, "Package ") && !rvn_log_starts_with(begin, end, "Class ")) return R_FALSE;
	/* the first colon ends the name: what precedes it must say Warning */
	for (cursor = begin; cursor < end && *cursor != ':'; cursor++) {
		if (cursor + 7 <= end && memcmp(cursor, "Warning", 7) == 0) return R_TRUE;
	}
	return R_FALSE;
}

/** R_TRUE for "Package <name> Error: ..." and "Class <name> Error: ...". */
static int rvn_log_is_package_error(const char* begin, const char* end) {
	const char* cursor;
	if (!rvn_log_starts_with(begin, end, "Package ") && !rvn_log_starts_with(begin, end, "Class ")) return R_FALSE;
	for (cursor = begin; cursor < end && *cursor != ':'; cursor++) {
		if (cursor + 5 <= end && memcmp(cursor, "Error", 5) == 0) return R_TRUE;
	}
	return R_FALSE;
}

/** R_TRUE for the lines that end a failed run. */
static int rvn_log_is_fatal(const char* begin, const char* end) {
	return rvn_log_starts_with(begin, end, "Emergency stop") ||
		rvn_log_starts_with(begin, end, "Fatal error occurred") ||
		rvn_log_starts_with(begin, end, "No pages of output");
}

/* ---------------------------------------------------------------------------
 *  Parsing
 * ------------------------------------------------------------------------- */

static void rvn_log_parse_into(const char* log, size_t size, rvn_log_list_t* list) {
	const char* cursor = log;
	const char* end = log + size;
	rvn_log_files_t files;

	memset(&files, 0, sizeof(files));
	while (cursor < end) {
		const char* line_end = rvn_log_line_end(cursor, end);
		const char* trimmed = rvn_log_skip_space(cursor, line_end);

		/* the summary of memory at the end of every log holds nothing to report */
		if (rvn_log_starts_with(trimmed, line_end, "Here is how much of TeX's memory")) break;

		rvn_log_track_files(cursor, line_end, &files);

		if (rvn_log_starts_with(trimmed, line_end, "! ")) {
			char* text = rvn_log_copy(trimmed + 2, line_end);
			const char* scan = rvn_log_next_line(line_end, end);
			const char* after = scan;
			int line = 0, looked = 0;

			/* the hint and the "l.<n> <source>" line come right after the message */
			{
				const char* marker_begin = NULL, *marker_end = NULL;
				while (scan < end && looked < 4) {
					const char* scan_end = rvn_log_line_end(scan, end);
					const char* scan_trimmed = rvn_log_skip_space(scan, scan_end);
					int marker = 0;
					if (rvn_log_line_marker(scan_trimmed, scan_end, &marker)) {
						line = marker;
						marker_begin = scan;
						marker_end = scan_end;
						after = rvn_log_next_line(scan_end, end);
						break;
					}
					if (rvn_log_starts_with(scan_trimmed, scan_end, "! ")) break;
					scan = rvn_log_next_line(scan_end, end);
					looked++;
				}
				if (scan >= end) after = end;
				{
					rvn_text_t context;
					memset(&context, 0, sizeof(context));
					rvn_log_error_context(after, end, marker_begin, marker_end, &context);
					rvn_log_add(list, R_LOG_ERROR, rvn_log_files_current(&files), line, text,
						context.length ? context.data : NULL);
					rvn_free(context.data);
				}
			}
			rvn_free(text);
			cursor = after;
			continue;
		}

		if (rvn_log_starts_with(trimmed, line_end, "Overfull ") || rvn_log_starts_with(trimmed, line_end, "Underfull ")) {
			char* text = rvn_log_copy(trimmed, line_end);
			rvn_log_add(list, R_LOG_BAD_BOX, rvn_log_files_current(&files), rvn_log_box_lines(trimmed, line_end), text, NULL);
			rvn_free(text);
			cursor = rvn_log_next_line(line_end, end);
			continue;
		}

		if (rvn_log_is_warning(trimmed, line_end)) {
			char* text = NULL;
			const char* warning_end = rvn_log_warning_text(trimmed, end, &text);
			int line = 0;
			rvn_log_input_line(trimmed, warning_end, &line);
			rvn_log_add(list, R_LOG_WARNING, rvn_log_files_current(&files), line, text, NULL);
			rvn_free(text);
			cursor = rvn_log_next_line(warning_end, end);
			continue;
		}

		if (rvn_log_is_package_error(trimmed, line_end)) {
			char* text = rvn_log_copy(trimmed, line_end);
			int line = 0;
			rvn_log_input_line(trimmed, line_end, &line);
			rvn_log_add(list, R_LOG_ERROR, rvn_log_files_current(&files), line, text, NULL);
			rvn_free(text);
			cursor = rvn_log_next_line(line_end, end);
			continue;
		}

		if (rvn_log_is_fatal(trimmed, line_end)) {
			char* text = rvn_log_copy(trimmed, line_end);
			rvn_log_add(list, R_LOG_ERROR, rvn_log_files_current(&files), 0, text, NULL);
			rvn_free(text);
			cursor = rvn_log_next_line(line_end, end);
			continue;
		}

		cursor = rvn_log_next_line(line_end, end);
	}
	rvn_log_files_free(&files);
}

static void rvn_log_free(RLogMessage* messages, int count);

/** The parser, private: the public functions wrap its list in a handle. */
static int rvn_log_parse(const char* log, size_t size, RLogMessage** messages) {
	rvn_log_list_t list;

	if (messages) *messages = NULL;
	if (!log || size == 0) return 0;
	memset(&list, 0, sizeof(list));
	rvn_log_parse_into(log, size, &list);
	if (!list.count) {
		rvn_free(list.items);
		return 0;
	}
	if (messages) *messages = list.items;
	else rvn_log_free(list.items, (int)list.count);
	return (int)list.count;
}

/** Frees a list of messages and their strings. */
static void rvn_log_free(RLogMessage* messages, int count) {
	int i;
	if (!messages) return;
	for (i = 0; i < count; i++) {
		rvn_free((void*)messages[i].file);
		rvn_free((void*)messages[i].message);
		rvn_free((void*)messages[i].context);
	}
	rvn_free(messages);
}

/** Reads a log file and parses it. */
static int rvn_log_parse_file(const char* path, RLogMessage** messages) {
	char* text;
	size_t size = 0;
	int count;

	if (messages) *messages = NULL;
	if (!path) return 0;
	text = rvn_read_file(path, &size);
	if (!text) return 0;
	count = rvn_log_parse(text, size, messages);
	rvn_free(text);
	return count;
}

/* ---------------------------------------------------------------------------
 *  Lists as handles (raven_latex.h)
 * ------------------------------------------------------------------------- */

/** A list of messages in the pool, with the number it holds. */
typedef struct rvn_log_t {
	RLogMessage* items;
	int count;
} rvn_log_t;

static rvn_log_t* rvn_log_get(Rid id) {
	if (!g_rvn.initialized || id == R_INVALID_ID) return NULL;
	return (rvn_log_t*)rvn_pool_get(&g_rvn.logs, id);
}

Rid Rlatex_log_parse(const char* log, size_t size) {
	RLogMessage* items = NULL;
	int count;
	rvn_log_t* entry;
	Rid id;

	RVN_API_GUARD(R_INVALID_ID);
	count = rvn_log_parse(log, size, &items);
	if (count <= 0) return R_INVALID_ID;
	entry = (rvn_log_t*)rvn_calloc(sizeof(rvn_log_t));
	entry->items = items;
	entry->count = count;
	id = rvn_pool_add(&g_rvn.logs, entry);
	if (id == R_INVALID_ID) {
		rvn_log_free(entry->items, entry->count);
		rvn_free(entry);
	}
	return id;
}

Rid Rlatex_log_parse_file(const char* path) {
	RLogMessage* items = NULL;
	int count;
	rvn_log_t* entry;
	Rid id;

	RVN_API_GUARD(R_INVALID_ID);
	count = rvn_log_parse_file(path, &items);
	if (count <= 0) return R_INVALID_ID;
	entry = (rvn_log_t*)rvn_calloc(sizeof(rvn_log_t));
	entry->items = items;
	entry->count = count;
	id = rvn_pool_add(&g_rvn.logs, entry);
	if (id == R_INVALID_ID) {
		rvn_log_free(entry->items, entry->count);
		rvn_free(entry);
	}
	return id;
}

int Rlatex_log_get_count(Rid id) {
	rvn_log_t* entry = rvn_log_get(id);
	RVN_API_GUARD(0);
	return entry ? entry->count : 0;
}

int Rlatex_log_get_message(Rid id, int index, RLogMessage* message) {
	rvn_log_t* entry = rvn_log_get(id);
	RVN_API_GUARD(R_FALSE);
	if (!entry || index < 0 || index >= entry->count) return R_FALSE;
	if (message) *message = entry->items[index];
	return R_TRUE;
}

void Rlatex_log_destroy(Rid id) {
	rvn_log_t* entry;
	RVN_API_GUARD_VOID();
	entry = rvn_log_get(id);
	if (!entry) return;
	rvn_log_free(entry->items, entry->count);
	rvn_free(entry);
	rvn_pool_remove(&g_rvn.logs, id);
}

/** Frees every list the application left alive (Rshutdown). */
void rvn_logs_shutdown(void) {
	uint32_t i;
	for (i = 0; i < g_rvn.logs.used; i++) {
		rvn_log_t* entry = (rvn_log_t*)rvn_pool_at(&g_rvn.logs, i);
		if (!entry) continue;
		rvn_log_free(entry->items, entry->count);
		rvn_free(entry);
	}
}
