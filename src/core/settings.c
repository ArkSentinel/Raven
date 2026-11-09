/**
 * Raven - core/settings.c
 * The settings store (raven_settings.h): a JSON document of flat keys,
 * a hand-written reader and writer, and the usual folders of an application.
 *
 * The document is kept as a tree. A key with dots is a path in it: setting
 * "editor.font_size" makes { "editor": { "font_size": 13 } }, and reading a
 * file that already holds a literal "editor.font_size" finds it as it is.
 * That way a file may be nested or flat, and a program sees one flat set of
 * keys either way.
 */

#include "context.h"
#include "fs.h"
#include "handle.h"

#include <float.h>
#include <limits.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

/*
 * The stores are handles of their own, in a private pool: no other part of
 * the library resolves them. 14 is the first type tag handle.h leaves free.
 */
#define RVN_SETTINGS_HANDLE_TYPE ((rvn_handle_type_t)14)

/** Deeper than this a document is refused: a fuzzable reader never recurses without a limit. */
#define RVN_SETTINGS_MAX_DEPTH 64
/** Texts handed to the application last until the next call of the same kind. */
#define RVN_SETTINGS_RING 8

/* ---------------------------------------------------------------------------
 *  Values
 * ------------------------------------------------------------------------- */

typedef enum rvn_json_kind_t {
	RVN_JSON_NULL = 0,
	RVN_JSON_BOOL,
	RVN_JSON_INT,
	RVN_JSON_NUMBER,
	RVN_JSON_TEXT,
	RVN_JSON_ARRAY,
	RVN_JSON_OBJECT
} rvn_json_kind_t;

typedef struct rvn_json_t {
	int kind;
	int boolean;
	long long integer;
	double number;
	char* text;
	/** Items of an array, or values of the members of an object. */
	struct rvn_json_t** values;
	/** Names of the members of an object (NULL for an array), as many as `name_cap` allows. */
	char** names;
	uint32_t count;
	uint32_t cap;
	uint32_t name_cap;
} rvn_json_t;

typedef struct rvn_settings_t {
	char* path;
	/** The document. An empty object when there is nothing. */
	rvn_json_t* root;
	/** The document as text, written on demand (Rsettings_get_json). */
	char* json;
} rvn_settings_t;

static rvn_pool_t g_settings_pool;
static int g_settings_pool_ready;
/** Texts returned by the folder functions and by Rsettings_get_key: released by the next calls, and at shutdown. */
static char* g_settings_ring[RVN_SETTINGS_RING];
static uint32_t g_settings_ring_next;
static char* g_settings_key_ring[RVN_SETTINGS_RING];
static uint32_t g_settings_key_ring_next;

static rvn_json_t* rvn_json_new(int kind) {
	rvn_json_t* value = (rvn_json_t*)rvn_calloc(sizeof(rvn_json_t));
	value->kind = kind;
	return value;
}

static void rvn_json_free(rvn_json_t* value) {
	uint32_t i;
	if (!value) return;
	for (i = 0; i < value->count; i++) {
		rvn_json_free(value->values[i]);
		if (value->names) rvn_free(value->names[i]);
	}
	rvn_free(value->values);
	rvn_free(value->names);
	rvn_free(value->text);
	rvn_free(value);
}

static void rvn_json_add(rvn_json_t* container, const char* name, rvn_json_t* value) {
	RVN_ARRAY_RESERVE(container->values, container->cap, container->count + 1);
	if (name) {
		RVN_ARRAY_RESERVE(container->names, container->name_cap, container->count + 1);
		container->names[container->count] = rvn_strdup(name);
	}
	container->values[container->count++] = value;
}

/* ---------------------------------------------------------------------------
 *  Reading
 * ------------------------------------------------------------------------- */

typedef struct rvn_json_reader_t {
	const char* text;
	const char* end;
	int depth;
	int failed;
} rvn_json_reader_t;

static void rvn_json_skip(rvn_json_reader_t* reader) {
	for (;;) {
		while (reader->text < reader->end && (unsigned char)*reader->text <= ' ') reader->text++;
		if (reader->end - reader->text >= 2 && reader->text[0] == '/' && reader->text[1] == '/') {
			/* to the end of the line */
			reader->text += 2;
			while (reader->text < reader->end && *reader->text != '\n') reader->text++;
			continue;
		}
		if (reader->end - reader->text >= 2 && reader->text[0] == '/' && reader->text[1] == '*') {
			reader->text += 2;
			while (reader->end - reader->text >= 2 && !(reader->text[0] == '*' && reader->text[1] == '/')) reader->text++;
			if (reader->end - reader->text >= 2) reader->text += 2;
			else reader->text = reader->end;
			continue;
		}
		return;
	}
}

static int rvn_json_hex4(const char* text, uint32_t* value) {
	uint32_t result = 0;
	int i;
	for (i = 0; i < 4; i++) {
		char c = text[i];
		uint32_t digit;
		if (c >= '0' && c <= '9') digit = (uint32_t)(c - '0');
		else if (c >= 'a' && c <= 'f') digit = (uint32_t)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F') digit = (uint32_t)(c - 'A' + 10);
		else return R_FALSE;
		result = (result << 4) | digit;
	}
	*value = result;
	return R_TRUE;
}

static char* rvn_json_read_string(rvn_json_reader_t* reader) {
	rvn_text_t out;
	if (reader->text >= reader->end || *reader->text != '"') {
		reader->failed = R_TRUE;
		return NULL;
	}
	memset(&out, 0, sizeof(out));
	reader->text++;
	while (reader->text < reader->end && *reader->text != '"') {
		unsigned char c = (unsigned char)*reader->text++;
		if (c == '\\') {
			char escape;
			if (reader->text >= reader->end) break;
			escape = *reader->text++;
			switch (escape) {
				case '"': rvn_text_append(&out, "\"", 1); break;
				case '\\': rvn_text_append(&out, "\\", 1); break;
				case '/': rvn_text_append(&out, "/", 1); break;
				case 'b': rvn_text_append(&out, "\b", 1); break;
				case 'f': rvn_text_append(&out, "\f", 1); break;
				case 'n': rvn_text_append(&out, "\n", 1); break;
				case 'r': rvn_text_append(&out, "\r", 1); break;
				case 't': rvn_text_append(&out, "\t", 1); break;
				case 'u': {
					uint32_t first;
					if (reader->end - reader->text < 4 || !rvn_json_hex4(reader->text, &first)) {
						reader->failed = R_TRUE;
						rvn_free(out.data);
						return NULL;
					}
					reader->text += 4;
					if (first >= 0xD800u && first <= 0xDBFFu) {
						/* a high surrogate: the low one follows, or the pair is broken */
						uint32_t low;
						if (reader->end - reader->text >= 6 && reader->text[0] == '\\' && reader->text[1] == 'u' &&
							rvn_json_hex4(reader->text + 2, &low) && low >= 0xDC00u && low <= 0xDFFFu) {
							reader->text += 6;
							first = 0x10000u + ((first - 0xD800u) << 10) + (low - 0xDC00u);
						} else {
							first = 0xFFFDu;
						}
					} else if (first >= 0xDC00u && first <= 0xDFFFu) {
						first = 0xFFFDu;
					}
					{
						char encoded[4];
						int bytes = rvn_utf8_encode(first, encoded);
						rvn_text_append(&out, encoded, (size_t)bytes);
					}
					break;
				}
				default:
					reader->failed = R_TRUE;
					rvn_free(out.data);
					return NULL;
			}
			continue;
		}
		rvn_text_append(&out, (const char*)&c, 1);
	}
	if (reader->text >= reader->end || *reader->text != '"') {
		reader->failed = R_TRUE;
		rvn_free(out.data);
		return NULL;
	}
	reader->text++;
	if (!out.data) return rvn_strdup("");
	return out.data;
}

/* Validates the grammar of a JSON number; returns its end, or NULL. */
static const char* rvn_json_number_end(const char* text, const char* end) {
	const char* c = text;
	if (c < end && *c == '-') c++;
	if (c >= end) return NULL;
	if (*c == '0') {
		c++;
	} else if (*c >= '1' && *c <= '9') {
		while (c < end && *c >= '0' && *c <= '9') c++;
	} else {
		return NULL;
	}
	if (c < end && *c == '.') {
		c++;
		if (c >= end || *c < '0' || *c > '9') return NULL;
		while (c < end && *c >= '0' && *c <= '9') c++;
	}
	if (c < end && (*c == 'e' || *c == 'E')) {
		c++;
		if (c < end && (*c == '+' || *c == '-')) c++;
		if (c >= end || *c < '0' || *c > '9') return NULL;
		while (c < end && *c >= '0' && *c <= '9') c++;
	}
	return c;
}

/* A double from validated digits, without the locale of strtod (a decimal point is a point). */
static double rvn_json_number_value(const char* text, const char* end) {
	double value = 0.0;
	double fraction = 0.1;
	int exponent = 0, negative = R_FALSE, in_fraction = R_FALSE;
	const char* c = text;
	if (c < end && *c == '-') {
		negative = R_TRUE;
		c++;
	}
	for (; c < end; c++) {
		if (*c >= '0' && *c <= '9') {
			if (in_fraction) {
				value += (double)(*c - '0') * fraction;
				fraction *= 0.1;
			} else {
				value = value * 10.0 + (double)(*c - '0');
			}
		} else if (*c == '.') {
			in_fraction = R_TRUE;
		} else if (*c == 'e' || *c == 'E') {
			int sign = 1;
			c++;
			if (c < end && (*c == '+' || *c == '-')) {
				if (*c == '-') sign = -1;
				c++;
			}
			for (; c < end && *c >= '0' && *c <= '9'; c++) {
				if (exponent < 10000) exponent = exponent * 10 + (*c - '0');
			}
			exponent *= sign;
			break;
		}
	}
	if (exponent != 0) value *= pow(10.0, (double)exponent);
	return negative ? -value : value;
}

static rvn_json_t* rvn_json_read_value(rvn_json_reader_t* reader);

static rvn_json_t* rvn_json_read_container(rvn_json_reader_t* reader, int is_object) {
	rvn_json_t* container = rvn_json_new(is_object ? RVN_JSON_OBJECT : RVN_JSON_ARRAY);
	char closing = is_object ? '}' : ']';
	reader->text++;
	if (++reader->depth > RVN_SETTINGS_MAX_DEPTH) {
		reader->failed = R_TRUE;
		rvn_json_free(container);
		return NULL;
	}
	for (;;) {
		rvn_json_skip(reader);
		if (reader->text >= reader->end) break;
		if (*reader->text == closing) {
			/* (a trailing comma is tolerated) */
			reader->text++;
			reader->depth--;
			return container;
		}
		if (container->count > 0) {
			if (*reader->text != ',') break;
			reader->text++;
			rvn_json_skip(reader);
			if (reader->text < reader->end && *reader->text == closing) {
				reader->text++;
				reader->depth--;
				return container;
			}
		}
		if (is_object) {
			char* name = rvn_json_read_string(reader);
			rvn_json_t* value;
			if (!name) break;
			rvn_json_skip(reader);
			if (reader->text >= reader->end || *reader->text != ':') {
				rvn_free(name);
				break;
			}
			reader->text++;
			value = rvn_json_read_value(reader);
			if (!value) {
				rvn_free(name);
				break;
			}
			rvn_json_add(container, name, value);
			rvn_free(name);
		} else {
			rvn_json_t* value = rvn_json_read_value(reader);
			if (!value) break;
			rvn_json_add(container, NULL, value);
		}
	}
	reader->failed = R_TRUE;
	rvn_json_free(container);
	return NULL;
}

static rvn_json_t* rvn_json_read_value(rvn_json_reader_t* reader) {
	rvn_json_skip(reader);
	if (reader->text >= reader->end) {
		reader->failed = R_TRUE;
		return NULL;
	}
	if (*reader->text == '{') return rvn_json_read_container(reader, R_TRUE);
	if (*reader->text == '[') return rvn_json_read_container(reader, R_FALSE);
	if (*reader->text == '"') {
		char* text = rvn_json_read_string(reader);
		rvn_json_t* value;
		if (!text) return NULL;
		value = rvn_json_new(RVN_JSON_TEXT);
		value->text = text;
		return value;
	}
	if (reader->end - reader->text >= 4 && memcmp(reader->text, "true", 4) == 0) {
		reader->text += 4;
		{
			rvn_json_t* value = rvn_json_new(RVN_JSON_BOOL);
			value->boolean = R_TRUE;
			return value;
		}
	}
	if (reader->end - reader->text >= 5 && memcmp(reader->text, "false", 5) == 0) {
		reader->text += 5;
		return rvn_json_new(RVN_JSON_BOOL);
	}
	if (reader->end - reader->text >= 4 && memcmp(reader->text, "null", 4) == 0) {
		reader->text += 4;
		return rvn_json_new(RVN_JSON_NULL);
	}
	{
		const char* number_end = rvn_json_number_end(reader->text, reader->end);
		rvn_json_t* value;
		int is_integer = R_TRUE;
		const char* c;
		if (!number_end) {
			reader->failed = R_TRUE;
			return NULL;
		}
		for (c = reader->text; c < number_end; c++) {
			if (*c == '.' || *c == 'e' || *c == 'E') is_integer = R_FALSE;
		}
		value = rvn_json_new(is_integer ? RVN_JSON_INT : RVN_JSON_NUMBER);
		if (is_integer) {
			long long integer = 0;
			int negative = *reader->text == '-';
			c = reader->text + (negative ? 1 : 0);
			for (; c < number_end; c++) {
				int digit = *c - '0';
				if (integer > (LLONG_MAX - digit) / 10) {
					/* (larger than the type: kept as a number) */
					value->kind = RVN_JSON_NUMBER;
					value->number = rvn_json_number_value(reader->text, number_end);
					integer = 0;
					break;
				}
				integer = integer * 10 + digit;
			}
			if (value->kind == RVN_JSON_NUMBER) {
				reader->text = number_end;
				return value;
			}
			value->integer = negative ? -integer : integer;
		} else {
			value->number = rvn_json_number_value(reader->text, number_end);
		}
		reader->text = number_end;
		return value;
	}
}

/* A whole document. Returns NULL when it is not valid JSON. */
static rvn_json_t* rvn_json_parse(const char* text, size_t size) {
	rvn_json_reader_t reader;
	rvn_json_t* root;
	if (!text) return NULL;
	/* a byte order mark is not part of the document */
	if (size >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
		text += 3;
		size -= 3;
	}
	memset(&reader, 0, sizeof(reader));
	reader.text = text;
	reader.end = text + size;
	root = rvn_json_read_value(&reader);
	if (!root) return NULL;
	rvn_json_skip(&reader);
	if (reader.failed || reader.text != reader.end) {
		rvn_json_free(root);
		return NULL;
	}
	return root;
}

/* ---------------------------------------------------------------------------
 *  Writing
 * ------------------------------------------------------------------------- */

/* The shortest text that reads back as exactly this double ("%.17g" trimmed). */
static void rvn_json_format_number(double value, char* out, size_t size) {
	char buffer[64];
	int precision;
	if (!isfinite(value)) {
		/* JSON has no NaN nor infinity: a number that cannot be written is 0 */
		snprintf(out, size, "0");
		return;
	}
	for (precision = 15; precision <= 17; precision++) {
		snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
		/* (parsed the way the reader does: no locale can change the decimal point) */
		if (rvn_json_number_value(buffer, buffer + strlen(buffer)) == value) break;
	}
	if (!strpbrk(buffer, ".eE")) {
		/* an integer double still reads as a double: "1.0" */
		size_t length = strlen(buffer);
		if (length + 3 <= size) {
			memcpy(out, buffer, length);
			memcpy(out + length, ".0", 3);
		} else {
			snprintf(out, size, "%s", buffer);
		}
	} else {
		snprintf(out, size, "%s", buffer);
	}
}

static void rvn_json_write_string(const char* text, rvn_text_t* out) {
	const unsigned char* c;
	rvn_text_append(out, "\"", 1);
	for (c = (const unsigned char*)text; *c; c++) {
		switch (*c) {
			case '"': rvn_text_append(out, "\\\"", 2); break;
			case '\\': rvn_text_append(out, "\\\\", 2); break;
			case '\b': rvn_text_append(out, "\\b", 2); break;
			case '\f': rvn_text_append(out, "\\f", 2); break;
			case '\n': rvn_text_append(out, "\\n", 2); break;
			case '\r': rvn_text_append(out, "\\r", 2); break;
			case '\t': rvn_text_append(out, "\\t", 2); break;
			default:
				if (*c < 0x20) {
					char escape[8];
					snprintf(escape, sizeof(escape), "\\u%04x", (unsigned)*c);
					rvn_text_add(out, escape);
				} else {
					rvn_text_append(out, (const char*)c, 1);
				}
				break;
		}
	}
	rvn_text_append(out, "\"", 1);
}

static void rvn_json_write_indent(rvn_text_t* out, int depth) {
	int i;
	rvn_text_append(out, "\n", 1);
	for (i = 0; i < depth; i++) rvn_text_append(out, "  ", 2);
}

/* R_TRUE when every member is a plain value: such an array stays on one line. */
static int rvn_json_inline(const rvn_json_t* value) {
	uint32_t i;
	for (i = 0; i < value->count; i++) {
		int kind = value->values[i]->kind;
		if (kind == RVN_JSON_ARRAY || kind == RVN_JSON_OBJECT) return R_FALSE;
	}
	return R_TRUE;
}

static void rvn_json_write_value(const rvn_json_t* value, rvn_text_t* out, int depth) {
	uint32_t i;
	char number[64];
	switch (value->kind) {
		case RVN_JSON_BOOL: rvn_text_add(out, value->boolean ? "true" : "false"); break;
		case RVN_JSON_INT: snprintf(number, sizeof(number), "%lld", value->integer); rvn_text_add(out, number); break;
		case RVN_JSON_NUMBER: rvn_json_format_number(value->number, number, sizeof(number)); rvn_text_add(out, number); break;
		case RVN_JSON_TEXT: rvn_json_write_string(value->text ? value->text : "", out); break;
		case RVN_JSON_ARRAY:
			if (!value->count) {
				rvn_text_append(out, "[]", 2);
				break;
			}
			rvn_text_append(out, "[", 1);
			if (rvn_json_inline(value)) {
				for (i = 0; i < value->count; i++) {
					if (i) rvn_text_append(out, ", ", 2);
					rvn_json_write_value(value->values[i], out, depth + 1);
				}
			} else {
				for (i = 0; i < value->count; i++) {
					rvn_json_write_indent(out, depth + 1);
					rvn_json_write_value(value->values[i], out, depth + 1);
					if (i + 1 < value->count) rvn_text_append(out, ",", 1);
				}
				rvn_json_write_indent(out, depth);
			}
			rvn_text_append(out, "]", 1);
			break;
		case RVN_JSON_OBJECT:
			if (!value->count) {
				rvn_text_append(out, "{}", 2);
				break;
			}
			rvn_text_append(out, "{", 1);
			for (i = 0; i < value->count; i++) {
				rvn_json_write_indent(out, depth + 1);
				rvn_json_write_string(value->names ? value->names[i] : "", out);
				rvn_text_append(out, ": ", 2);
				rvn_json_write_value(value->values[i], out, depth + 1);
				if (i + 1 < value->count) rvn_text_append(out, ",", 1);
			}
			rvn_json_write_indent(out, depth);
			rvn_text_append(out, "}", 1);
			break;
		case RVN_JSON_NULL:
		default:
			rvn_text_append(out, "null", 4);
			break;
	}
}

/* The document of a store as text, "" when it holds nothing. */
static const char* rvn_settings_json(rvn_settings_t* settings) {
	rvn_text_t out;
	if (!settings) return "";
	if (settings->json) return settings->json;
	if (settings->root && settings->root->kind == RVN_JSON_OBJECT && settings->root->count == 0) return "";
	if (settings->root && settings->root->kind == RVN_JSON_ARRAY && settings->root->count == 0) return "[]";
	memset(&out, 0, sizeof(out));
	rvn_json_write_value(settings->root, &out, 0);
	rvn_text_append(&out, "\n", 1);
	settings->json = out.data;
	return settings->json ? settings->json : "";
}

/* ---------------------------------------------------------------------------
 *  The tree, by dotted key
 * ------------------------------------------------------------------------- */

static int rvn_json_member_index(const rvn_json_t* object, const char* name) {
	uint32_t i;
	if (!object || object->kind != RVN_JSON_OBJECT || !object->names) return -1;
	for (i = 0; i < object->count; i++) {
		if (strcmp(object->names[i], name) == 0) return (int)i;
	}
	return -1;
}

/* The value of a key, or NULL. A dotted key walks into the objects it names. */
static rvn_json_t* rvn_json_lookup_depth(rvn_json_t* node, const char* key, int depth) {
	const char* dot;
	int index;
	if (!node || !key || depth > RVN_SETTINGS_MAX_DEPTH) return NULL;
	if (node->kind != RVN_JSON_OBJECT) return NULL;
	/* a key that is literally there wins over a path */
	index = rvn_json_member_index(node, key);
	if (index >= 0) return node->values[index];
	dot = strchr(key, '.');
	if (!dot || dot == key) return NULL;
	{
		char* head = rvn_strndup(key, (size_t)(dot - key));
		int head_index = rvn_json_member_index(node, head);
		rvn_json_t* result = head_index >= 0 ? rvn_json_lookup_depth(node->values[head_index], dot + 1, depth + 1) : NULL;
		rvn_free(head);
		return result;
	}
}

static rvn_json_t* rvn_json_lookup(rvn_json_t* node, const char* key) {
	return rvn_json_lookup_depth(node, key, 0);
}

/* The slot that holds the value of a key, making the objects on the way when asked to. */
static rvn_json_t** rvn_json_slot_depth(rvn_json_t* root, const char* key, int create, int depth) {
	const char* dot;
	int index;
	if (!root || !key || !key[0] || depth > RVN_SETTINGS_MAX_DEPTH) return NULL;
	if (root->kind != RVN_JSON_OBJECT) return NULL;
	index = rvn_json_member_index(root, key);
	if (index >= 0) return &root->values[index];
	if (!create && !strchr(key, '.')) return NULL;
	dot = strchr(key, '.');
	if (!dot || dot == key) {
		if (!create) return NULL;
		rvn_json_add(root, key, rvn_json_new(RVN_JSON_NULL));
		return &root->values[root->count - 1];
	}
	{
		char* head = rvn_strndup(key, (size_t)(dot - key));
		int head_index = rvn_json_member_index(root, head);
		rvn_json_t** slot;
		if (head_index < 0) {
			if (!create) {
				rvn_free(head);
				return NULL;
			}
			rvn_json_add(root, head, rvn_json_new(RVN_JSON_OBJECT));
			head_index = (int)root->count - 1;
		} else if (root->values[head_index]->kind != RVN_JSON_OBJECT) {
			if (!create) {
				rvn_free(head);
				return NULL;
			}
			rvn_json_free(root->values[head_index]);
			root->values[head_index] = rvn_json_new(RVN_JSON_OBJECT);
		}
		slot = rvn_json_slot_depth(root->values[head_index], dot + 1, create, depth + 1);
		rvn_free(head);
		return slot;
	}
}

static rvn_json_t** rvn_json_slot(rvn_json_t* root, const char* key, int create) {
	return rvn_json_slot_depth(root, key, create, 0);
}

/* Puts a new value where the key is, releasing what was there. */
static void rvn_settings_put(rvn_settings_t* settings, const char* key, rvn_json_t* value) {
	rvn_json_t** slot;
	if (!settings || !key || !key[0]) {
		rvn_json_free(value);
		return;
	}
	if (!settings->root) settings->root = rvn_json_new(RVN_JSON_OBJECT);
	if (settings->root->kind != RVN_JSON_OBJECT) {
		/* (a document that is not an object has no keys to set) */
		rvn_error(R_ERROR_INVALID_OPERATION, R_SEVERITY_ERROR,
			"the document of this settings store is not an object: it has no keys");
		rvn_json_free(value);
		return;
	}
	slot = rvn_json_slot(settings->root, key, R_TRUE);
	if (!slot) {
		rvn_json_free(value);
		return;
	}
	rvn_json_free(*slot);
	*slot = value;
	rvn_free(settings->json);
	settings->json = NULL;
}

/* ---------------------------------------------------------------------------
 *  The stores
 * ------------------------------------------------------------------------- */

static rvn_settings_t* rvn_settings_get(Rid id) {
	if (!g_settings_pool_ready) return NULL;
	return (rvn_settings_t*)rvn_pool_get(&g_settings_pool, id);
}

static rvn_settings_t* rvn_settings_check(Rid id, const char* function) {
	rvn_settings_t* settings;
	if (!rvn_api_check(function)) return NULL;
	settings = rvn_settings_get(id);
	if (!settings) rvn_error(R_ERROR_INVALID_ID, R_SEVERITY_ERROR, "%s: invalid settings handle 0x%08X", function, (unsigned)id);
	return settings;
}

static rvn_settings_t* rvn_settings_alloc(const char* path) {
	rvn_settings_t* settings = (rvn_settings_t*)rvn_calloc(sizeof(rvn_settings_t));
	settings->path = rvn_strdup(path);
	settings->root = rvn_json_new(RVN_JSON_OBJECT);
	return settings;
}

void rvn_settings_shutdown(void) {
	uint32_t i;
	if (!g_settings_pool_ready) return;
	for (i = 0; i < g_settings_pool.used; i++) {
		rvn_settings_t* settings = (rvn_settings_t*)rvn_pool_at(&g_settings_pool, i);
		if (settings) {
			rvn_free(settings->path);
			rvn_free(settings->json);
			rvn_json_free(settings->root);
			rvn_free(settings);
		}
	}
	rvn_pool_free(&g_settings_pool);
	g_settings_pool_ready = R_FALSE;
	for (i = 0; i < RVN_SETTINGS_RING; i++) {
		rvn_free(g_settings_ring[i]);
		g_settings_ring[i] = NULL;
		rvn_free(g_settings_key_ring[i]);
		g_settings_key_ring[i] = NULL;
	}
	g_settings_ring_next = 0;
	g_settings_key_ring_next = 0;
}

/* A text of the folder functions, kept until the next one. */
static const char* rvn_settings_keep(char* text) {
	uint32_t slot = g_settings_ring_next++ % RVN_SETTINGS_RING;
	rvn_free(g_settings_ring[slot]);
	g_settings_ring[slot] = text;
	return text ? text : "";
}

/* A key of Rsettings_get_key, kept until the next one. */
static const char* rvn_settings_keep_key(char* text) {
	uint32_t slot = g_settings_key_ring_next++ % RVN_SETTINGS_RING;
	rvn_free(g_settings_key_ring[slot]);
	g_settings_key_ring[slot] = text;
	return text ? text : "";
}

/* ---------------------------------------------------------------------------
 *  Folders
 * ------------------------------------------------------------------------- */

/* Creates a folder and every folder above it that is missing. */
static void rvn_settings_mkdirs(const char* path) {
	char* copy;
	size_t length, i;
	if (!path || !path[0]) return;
	copy = rvn_strdup(path);
	length = strlen(copy);
	for (i = 1; i <= length; i++) {
		if (i == length || rvn_path_is_separator(copy[i])) {
			char saved = copy[i];
			copy[i] = '\0';
			if (copy[0]) rvn_make_dir(copy);
			copy[i] = saved;
		}
	}
	rvn_free(copy);
}

/* The name of the application as a folder: no separators, no way out. */
static char* rvn_settings_app_folder(const char* app_name) {
	char* name = rvn_strdup(app_name && app_name[0] ? app_name : "Raven");
	char* c;
	for (c = name; *c; c++) {
		if (rvn_path_is_separator(*c) || *c == ':') *c = '_';
	}
	return name;
}

/* The base folder of the application, created, or NULL. */
static char* rvn_settings_base_folder(const char* app_name, int cache) {
	char* app = rvn_settings_app_folder(app_name);
	char* base = NULL;
	char* with_app;
#if defined(_WIN32)
	char* root = cache ? rvn_env_get("LOCALAPPDATA") : rvn_env_get("APPDATA");
	if (!root) root = rvn_env_get("USERPROFILE");
	if (root) {
		/* %APPDATA%\<app>, or %LOCALAPPDATA%\<app>\Cache */
		with_app = rvn_path_child(root, app);
		base = cache ? rvn_path_child(with_app, "Cache") : with_app;
		if (cache) rvn_free(with_app);
		rvn_free(root);
	}
#else
	char* folder = rvn_env_get(cache ? "XDG_CACHE_HOME" : "XDG_CONFIG_HOME");
	/* (an XDG variable must be an absolute path to be used) */
	if (folder && folder[0] == '/') {
		base = rvn_strdup(folder);
	} else {
		char* home = rvn_path_home();
#if defined(__APPLE__)
		base = rvn_path_child(home, cache ? "Library/Caches" : "Library/Application Support");
#else
		base = rvn_path_child(home, cache ? ".cache" : ".config");
#endif
		rvn_free(home);
	}
	rvn_free(folder);
	if (base) {
		with_app = rvn_path_child(base, app);
		rvn_free(base);
		base = with_app;
	}
#endif
	rvn_free(app);
	if (base) rvn_settings_mkdirs(base);
	return base;
}

/* ---------------------------------------------------------------------------
 *  Public API
 * ------------------------------------------------------------------------- */

Rid Rsettings_create(const char* path) {
	rvn_settings_t* settings;
	Rid id;
	if (!rvn_api_check(__func__)) return R_INVALID_ID;
	if (!g_settings_pool_ready) {
		rvn_pool_init(&g_settings_pool, RVN_SETTINGS_HANDLE_TYPE);
		g_settings_pool_ready = R_TRUE;
	}
	settings = rvn_settings_alloc(path && path[0] ? path : NULL);
	id = rvn_pool_add(&g_settings_pool, settings);
	if (id == R_INVALID_ID) {
		rvn_free(settings->path);
		rvn_json_free(settings->root);
		rvn_free(settings);
		rvn_error(R_ERROR_OUT_OF_MEMORY, R_SEVERITY_ERROR, "Rsettings_create: no more stores");
		return R_INVALID_ID;
	}
	if (settings->path) Rsettings_load(id);
	return id;
}

void Rsettings_destroy(Rid id) {
	rvn_settings_t* settings = rvn_settings_check(id, __func__);
	if (!settings) return;
	rvn_pool_remove(&g_settings_pool, id);
	rvn_free(settings->path);
	rvn_free(settings->json);
	rvn_json_free(settings->root);
	rvn_free(settings);
}

int Rsettings_load(Rid id) {
	rvn_settings_t* settings = rvn_settings_check(id, __func__);
	char* text;
	size_t size = 0;
	rvn_json_t* root;
	if (!settings) return R_FALSE;
	rvn_json_free(settings->root);
	settings->root = rvn_json_new(RVN_JSON_OBJECT);
	rvn_free(settings->json);
	settings->json = NULL;
	if (!settings->path || !rvn_path_exists(settings->path)) return R_FALSE;
	text = rvn_read_file(settings->path, &size);
	if (!text) return R_FALSE;
	root = rvn_json_parse(text, size);
	rvn_free(text);
	if (!root) return R_FALSE;
	rvn_json_free(settings->root);
	settings->root = root;
	return R_TRUE;
}

#if defined(_WIN32)
/* Replaces a file with another, atomically where the file system allows it. */
static int rvn_settings_rename(const char* from, const char* to) {
	int length = MultiByteToWideChar(CP_UTF8, 0, from, -1, NULL, 0);
	int target = MultiByteToWideChar(CP_UTF8, 0, to, -1, NULL, 0);
	WCHAR* wide_from;
	WCHAR* wide_to;
	BOOL ok;
	if (length <= 0 || target <= 0) return R_FALSE;
	wide_from = (WCHAR*)rvn_alloc(sizeof(WCHAR) * (size_t)length);
	wide_to = (WCHAR*)rvn_alloc(sizeof(WCHAR) * (size_t)target);
	MultiByteToWideChar(CP_UTF8, 0, from, -1, wide_from, length);
	MultiByteToWideChar(CP_UTF8, 0, to, -1, wide_to, target);
	ok = MoveFileExW(wide_from, wide_to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	rvn_free(wide_from);
	rvn_free(wide_to);
	return ok ? R_TRUE : R_FALSE;
}

static void rvn_settings_delete(const char* path) {
	int length = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
	WCHAR* wide;
	if (length <= 0) return;
	wide = (WCHAR*)rvn_alloc(sizeof(WCHAR) * (size_t)length);
	MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, length);
	DeleteFileW(wide);
	rvn_free(wide);
}
#else
static int rvn_settings_rename(const char* from, const char* to) {
	return rename(from, to) == 0;
}

static void rvn_settings_delete(const char* path) {
	remove(path);
}
#endif

int Rsettings_save(Rid id) {
	rvn_settings_t* settings = rvn_settings_check(id, __func__);
	const char* json;
	char* temporary;
	char* folder;
	int ok;
	if (!settings) return R_FALSE;
	if (!settings->path) {
		rvn_error(R_ERROR_INVALID_OPERATION, R_SEVERITY_ERROR, "Rsettings_save: the store has no file");
		return R_FALSE;
	}
	folder = rvn_path_dir(settings->path);
	if (folder) rvn_settings_mkdirs(folder);
	rvn_free(folder);
	json = rvn_settings_json(settings);
	if (!json[0]) json = "{}";
	temporary = (char*)rvn_alloc(strlen(settings->path) + 5);
	sprintf(temporary, "%s.tmp", settings->path);
	ok = rvn_write_file(temporary, json, strlen(json));
	if (ok) ok = rvn_settings_rename(temporary, settings->path);
	if (!ok) rvn_settings_delete(temporary);
	rvn_free(temporary);
	return ok;
}

const char* Rsettings_get_path(Rid id) {
	rvn_settings_t* settings = rvn_settings_check(id, __func__);
	return settings && settings->path ? settings->path : "";
}

RSettingType Rsettings_get_type(Rid id, const char* key) {
	rvn_settings_t* settings = rvn_settings_check(id, __func__);
	rvn_json_t* value;
	if (!settings) return R_SETTING_NONE;
	value = rvn_json_lookup(settings->root, key);
	if (!value) return R_SETTING_NONE;
	switch (value->kind) {
		case RVN_JSON_BOOL: return R_SETTING_BOOL;
		case RVN_JSON_INT: return R_SETTING_INT;
		case RVN_JSON_NUMBER: return R_SETTING_NUMBER;
		case RVN_JSON_TEXT: return R_SETTING_TEXT;
		default: return R_SETTING_NONE;
	}
}

int Rsettings_get_bool(Rid id, const char* key, int fallback) {
	rvn_settings_t* settings = rvn_settings_check(id, __func__);
	rvn_json_t* value;
	if (!settings) return fallback;
	value = rvn_json_lookup(settings->root, key);
	if (!value) return fallback;
	switch (value->kind) {
		case RVN_JSON_BOOL: return value->boolean ? R_TRUE : R_FALSE;
		case RVN_JSON_INT: return value->integer != 0 ? R_TRUE : R_FALSE;
		case RVN_JSON_NUMBER: return value->number != 0.0 ? R_TRUE : R_FALSE;
		default: return fallback;
	}
}

int Rsettings_get_int(Rid id, const char* key, int fallback) {
	rvn_settings_t* settings = rvn_settings_check(id, __func__);
	rvn_json_t* value;
	long long integer;
	if (!settings) return fallback;
	value = rvn_json_lookup(settings->root, key);
	if (!value) return fallback;
	if (value->kind == RVN_JSON_INT) integer = value->integer;
	else if (value->kind == RVN_JSON_NUMBER) integer = (long long)value->number;
	else if (value->kind == RVN_JSON_BOOL) integer = value->boolean ? 1 : 0;
	else return fallback;
	if (integer > INT_MAX) return INT_MAX;
	if (integer < INT_MIN) return INT_MIN;
	return (int)integer;
}

double Rsettings_get_number(Rid id, const char* key, double fallback) {
	rvn_settings_t* settings = rvn_settings_check(id, __func__);
	rvn_json_t* value;
	if (!settings) return fallback;
	value = rvn_json_lookup(settings->root, key);
	if (!value) return fallback;
	if (value->kind == RVN_JSON_NUMBER) return value->number;
	if (value->kind == RVN_JSON_INT) return (double)value->integer;
	return fallback;
}

const char* Rsettings_get_text(Rid id, const char* key, const char* fallback) {
	rvn_settings_t* settings = rvn_settings_check(id, __func__);
	rvn_json_t* value;
	if (!settings) return fallback;
	value = rvn_json_lookup(settings->root, key);
	if (!value || value->kind != RVN_JSON_TEXT) return fallback;
	return value->text ? value->text : "";
}

void Rsettings_set_bool(Rid id, const char* key, int value) {
	rvn_settings_t* settings = rvn_settings_check(id, __func__);
	rvn_json_t* json;
	if (!settings) return;
	json = rvn_json_new(RVN_JSON_BOOL);
	json->boolean = value != 0;
	rvn_settings_put(settings, key, json);
}

void Rsettings_set_int(Rid id, const char* key, int value) {
	rvn_settings_t* settings = rvn_settings_check(id, __func__);
	rvn_json_t* json;
	if (!settings) return;
	json = rvn_json_new(RVN_JSON_INT);
	json->integer = value;
	rvn_settings_put(settings, key, json);
}

void Rsettings_set_number(Rid id, const char* key, double value) {
	rvn_settings_t* settings = rvn_settings_check(id, __func__);
	rvn_json_t* json;
	if (!settings) return;
	json = rvn_json_new(RVN_JSON_NUMBER);
	json->number = value;
	rvn_settings_put(settings, key, json);
}

void Rsettings_set_text(Rid id, const char* key, const char* value) {
	rvn_settings_t* settings = rvn_settings_check(id, __func__);
	rvn_json_t* json;
	if (!settings) return;
	if (!value) {
		Rsettings_remove(id, key);
		return;
	}
	json = rvn_json_new(RVN_JSON_TEXT);
	json->text = rvn_strdup(value);
	rvn_settings_put(settings, key, json);
}

/* Removes the member of the object that holds the slot. */
static int rvn_json_remove_slot_depth(rvn_json_t* node, const char* key, int depth) {
	const char* dot;
	int index;
	if (!node || node->kind != RVN_JSON_OBJECT || !key || !key[0] || depth > RVN_SETTINGS_MAX_DEPTH) return R_FALSE;
	index = rvn_json_member_index(node, key);
	if (index >= 0) {
		rvn_json_free(node->values[index]);
		rvn_free(node->names[index]);
		memmove(&node->values[index], &node->values[index + 1], sizeof(rvn_json_t*) * (size_t)(node->count - (uint32_t)index - 1));
		memmove(&node->names[index], &node->names[index + 1], sizeof(char*) * (size_t)(node->count - (uint32_t)index - 1));
		node->count--;
		return R_TRUE;
	}
	dot = strchr(key, '.');
	if (!dot || dot == key) return R_FALSE;
	{
		char* head = rvn_strndup(key, (size_t)(dot - key));
		int head_index = rvn_json_member_index(node, head);
		int removed = head_index >= 0 ? rvn_json_remove_slot_depth(node->values[head_index], dot + 1, depth + 1) : R_FALSE;
		rvn_free(head);
		return removed;
	}
}

static int rvn_json_remove_slot(rvn_json_t* node, const char* key) {
	return rvn_json_remove_slot_depth(node, key, 0);
}

int Rsettings_remove(Rid id, const char* key) {
	rvn_settings_t* settings = rvn_settings_check(id, __func__);
	int removed;
	if (!settings) return R_FALSE;
	removed = rvn_json_remove_slot(settings->root, key);
	if (removed) {
		rvn_free(settings->json);
		settings->json = NULL;
	}
	return removed;
}

int Rsettings_has(Rid id, const char* key) {
	rvn_settings_t* settings = rvn_settings_check(id, __func__);
	if (!settings) return R_FALSE;
	return rvn_json_lookup(settings->root, key) != NULL;
}

const char* Rsettings_get_json(Rid id) {
	rvn_settings_t* settings = rvn_settings_check(id, __func__);
	if (!settings) return "";
	return rvn_settings_json(settings);
}

int Rsettings_set_json(Rid id, const char* json) {
	rvn_settings_t* settings = rvn_settings_check(id, __func__);
	rvn_json_t* root;
	if (!settings) return R_FALSE;
	if (!json) return R_FALSE;
	root = rvn_json_parse(json, strlen(json));
	if (!root) return R_FALSE;
	rvn_json_free(settings->root);
	settings->root = root;
	rvn_free(settings->json);
	settings->json = NULL;
	return R_TRUE;
}

int Rsettings_get_key_count(Rid id, const char* prefix) {
	rvn_settings_t* settings = rvn_settings_check(id, __func__);
	rvn_json_t* node;
	if (!settings) return 0;
	node = (prefix && prefix[0]) ? rvn_json_lookup(settings->root, prefix) : settings->root;
	if (!node || (node->kind != RVN_JSON_OBJECT && node->kind != RVN_JSON_ARRAY)) return 0;
	return (int)node->count;
}

const char* Rsettings_get_key(Rid id, const char* prefix, int index) {
	rvn_settings_t* settings = rvn_settings_check(id, __func__);
	rvn_json_t* node;
	if (!settings || index < 0) return NULL;
	node = (prefix && prefix[0]) ? rvn_json_lookup(settings->root, prefix) : settings->root;
	if (!node || (uint32_t)index >= node->count) return NULL;
	if (node->kind == RVN_JSON_ARRAY) {
		char number[16];
		snprintf(number, sizeof(number), "%d", index);
		return rvn_settings_keep_key(rvn_strdup(number));
	}
	if (node->kind != RVN_JSON_OBJECT) return NULL;
	return rvn_settings_keep_key(rvn_strdup(node->names ? node->names[index] : ""));
}

const char* Rsettings_get_config_folder(const char* app_name) {
	RVN_API_GUARD("");
	return rvn_settings_keep(rvn_settings_base_folder(app_name, R_FALSE));
}

const char* Rsettings_get_cache_folder(const char* app_name) {
	RVN_API_GUARD("");
	return rvn_settings_keep(rvn_settings_base_folder(app_name, R_TRUE));
}

Rid Rsettings_open(const char* app_name, const char* file_name) {
	char* folder;
	char* path;
	Rid id;
	RVN_API_GUARD(R_INVALID_ID);
	if (!file_name || !file_name[0] || strchr(file_name, '/') || strchr(file_name, '\\') || strstr(file_name, "..")) {
		rvn_error(R_ERROR_INVALID_VALUE, R_SEVERITY_ERROR, "Rsettings_open: \"%s\" is not a file name", file_name ? file_name : "(null)");
		return R_INVALID_ID;
	}
	folder = rvn_settings_base_folder(app_name, R_FALSE);
	if (!folder) {
		rvn_error(R_ERROR_FILE_NOT_FOUND, R_SEVERITY_ERROR, "Rsettings_open: there is no folder for the settings of \"%s\"", app_name ? app_name : "");
		return R_INVALID_ID;
	}
	path = rvn_path_child(folder, file_name);
	rvn_free(folder);
	id = Rsettings_create(path);
	rvn_free(path);
	return id;
}
