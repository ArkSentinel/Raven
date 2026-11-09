/**
 * Raven - widgets/regex.c
 * The regular expressions of the search of the text editor (regex.h).
 *
 * The pattern becomes a tree (alternation, sequence, repetition, atom),
 * and the tree becomes a list of instructions: the classic construction,
 * where what is not yet known (where a branch goes) is kept in a list of
 * patches and filled in when the other branch is emitted.
 *
 * The matcher walks the instructions, trying one branch after the other:
 * backtracking. A budget of steps keeps a pathological pattern from
 * hanging the interface; when it runs out the search says that nothing
 * matches, instead of freezing the window.
 *
 * Everything works on bytes: a text in UTF-8 matches itself, and a `.`
 * takes one byte. That is enough for a search box, and keeps the engine
 * small and free of any dependency.
 */

#include "regex.h"

/* ---------------------------------------------------------------------------
 *  Instructions
 * ------------------------------------------------------------------------- */

typedef enum rvn_re_op_t {
	RVN_RE_CHAR,
	RVN_RE_ANY,
	RVN_RE_CLASS,
	RVN_RE_SPLIT,
	RVN_RE_JUMP,
	RVN_RE_SAVE,
	RVN_RE_BOL,
	RVN_RE_EOL,
	RVN_RE_WORD_BOUNDARY,
	RVN_RE_NOT_WORD_BOUNDARY,
	RVN_RE_MATCH
} rvn_re_op_t;

/** A set of bytes, as 32 bytes of bits. */
typedef struct rvn_re_class_t {
	uint8_t bits[32];
} rvn_re_class_t;

typedef struct rvn_re_inst_t {
	rvn_re_op_t op;
	/** The byte of a CHAR, the group of a SAVE (2 * group + which), the target of a JUMP. */
	int a;
	/** The second target of a SPLIT. */
	int b;
	/** The set of a CLASS. */
	int cls;
} rvn_re_inst_t;

/** An offset of a list of patches: where an instruction must point when it is known. */
typedef struct rvn_re_patch_t {
	int inst;
	/** 0: the field `a`; 1: the field `b`. */
	int field;
	int next;
} rvn_re_patch_t;

typedef struct rvn_re_builder_t {
	rvn_re_inst_t* code;
	int count;
	uint32_t cap;
	rvn_re_patch_t* patches;
	int patch_count;
	uint32_t patch_cap;
	rvn_re_class_t* classes;
	int class_count;
	uint32_t class_cap;
	const char* error;
} rvn_re_builder_t;

/** What a piece of pattern compiled to: where it starts, and what is not patched yet. */
typedef struct rvn_re_frag_t {
	int start;
	/** Head of the list of patches; -1 when it is empty. */
	int out;
} rvn_re_frag_t;

struct rvn_regex_t {
	rvn_re_inst_t* code;
	int count;
	rvn_re_class_t* classes;
	int class_count;
	int groups;
	int ignore_case;
	int multiline;
};

#define RVN_RE_MAX_STEPS 2000000L
#define RVN_RE_MAX_DEPTH 512
#define RVN_RE_MAX_GROUPS 32

static int rvn_re_emit(rvn_re_builder_t* b, rvn_re_op_t op, int a, int b2, int cls) {
	rvn_re_inst_t* inst;
	RVN_ARRAY_RESERVE(b->code, b->cap, b->count + 1);
	inst = &b->code[b->count];
	inst->op = op;
	inst->a = a;
	inst->b = b2;
	inst->cls = cls;
	return b->count++;
}

static int rvn_re_patch_add(rvn_re_builder_t* b, int inst, int field) {
	rvn_re_patch_t* patch;
	RVN_ARRAY_RESERVE(b->patches, b->patch_cap, b->patch_count + 1);
	patch = &b->patches[b->patch_count];
	patch->inst = inst;
	patch->field = field;
	patch->next = -1;
	return b->patch_count++;
}

/** Joins two lists of patches (the second after the first). */
static int rvn_re_patch_join(rvn_re_builder_t* b, int first, int second) {
	int at;
	if (first < 0) return second;
	if (second < 0) return first;
	for (at = first; b->patches[at].next >= 0; at = b->patches[at].next) {}
	b->patches[at].next = second;
	return first;
}

/** Makes every patch of a list point to `target`. */
static void rvn_re_patch_apply(rvn_re_builder_t* b, int list, int target) {
	while (list >= 0) {
		rvn_re_patch_t* patch = &b->patches[list];
		if (patch->field) b->code[patch->inst].b = target;
		else b->code[patch->inst].a = target;
		list = patch->next;
	}
}

static int rvn_re_is_word(int c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}

static int rvn_re_is_digit(int c) {
	return c >= '0' && c <= '9';
}

static int rvn_re_is_space(int c) {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static void rvn_re_class_set(rvn_re_class_t* cls, int c) {
	if (c < 0 || c > 255) return;
	cls->bits[c >> 3] |= (uint8_t)(1u << (c & 7));
}

static int rvn_re_class_has(const rvn_re_class_t* cls, int c) {
	if (c < 0 || c > 255) return 0;
	return (cls->bits[c >> 3] >> (c & 7)) & 1;
}

static void rvn_re_class_shorthand(rvn_re_class_t* cls, char kind) {
	int c, inverse = kind >= 'A' && kind <= 'Z';
	char lower = (char)(kind | 0x20);
	for (c = 0; c < 256; c++) {
		int inside = lower == 'd' ? rvn_re_is_digit(c) : lower == 'w' ? rvn_re_is_word(c) : rvn_re_is_space(c);
		if (inside != inverse) rvn_re_class_set(cls, c);
	}
}

/* ---------------------------------------------------------------------------
 *  Tree
 * ------------------------------------------------------------------------- */

typedef enum rvn_re_kind_t {
	RVN_NODE_CHAR,
	RVN_NODE_ANY,
	RVN_NODE_CLASS,
	RVN_NODE_BOL,
	RVN_NODE_EOL,
	RVN_NODE_WORD_BOUNDARY,
	RVN_NODE_NOT_WORD_BOUNDARY,
	RVN_NODE_GROUP,
	RVN_NODE_CONCAT,
	RVN_NODE_ALT,
	RVN_NODE_REPEAT,
	RVN_NODE_EMPTY
} rvn_re_kind_t;

typedef struct rvn_re_node_t {
	rvn_re_kind_t kind;
	/** The byte, the set, or the number of the group. */
	int value;
	/** The fewest and the most repetitions: -1 for no limit. */
	int min;
	int max;
	int greedy;
	struct rvn_re_node_t* left;
	struct rvn_re_node_t* right;
} rvn_re_node_t;

static rvn_re_node_t* rvn_re_node(rvn_re_kind_t kind) {
	rvn_re_node_t* node = (rvn_re_node_t*)rvn_calloc(sizeof(rvn_re_node_t));
	node->kind = kind;
	node->greedy = R_TRUE;
	node->max = 1;
	return node;
}

static void rvn_re_node_free(rvn_re_node_t* node) {
	if (!node) return;
	rvn_re_node_free(node->left);
	rvn_re_node_free(node->right);
	rvn_free(node);
}

/* ---------------------------------------------------------------------------
 *  Parser
 * ------------------------------------------------------------------------- */

typedef struct rvn_re_parser_t {
	const char* pattern;
	int at;
	int length;
	rvn_re_builder_t* builder;
	int groups;
	const char* error;
} rvn_re_parser_t;

static rvn_re_node_t* rvn_re_parse_alt(rvn_re_parser_t* parser);

/* One character of an escape: its byte, or -1 when it is a class (added to `cls`). */
static int rvn_re_escape(rvn_re_parser_t* parser, rvn_re_class_t* cls) {
	char c;
	if (parser->at >= parser->length) {
		parser->error = "\\ at the end of the pattern";
		return -2;
	}
	c = parser->pattern[parser->at++];
	switch (c) {
		case 'd': case 'D': case 'w': case 'W': case 's': case 'S':
			if (cls) rvn_re_class_shorthand(cls, c);
			return -1;
		case 'n': return '\n';
		case 't': return '\t';
		case 'r': return '\r';
		case 'f': return '\f';
		case 'v': return '\v';
		case '0': return '\0';
		default: return (unsigned char)c;
	}
}

/* Reads a set; the '[' is behind, the ']' is left behind. */
static rvn_re_node_t* rvn_re_parse_set(rvn_re_parser_t* parser) {
	rvn_re_class_t cls;
	rvn_re_node_t* node;
	int inverse = R_FALSE, first = R_TRUE;
	memset(&cls, 0, sizeof(cls));
	if (parser->at < parser->length && parser->pattern[parser->at] == '^') {
		inverse = R_TRUE;
		parser->at++;
	}
	for (;;) {
		int c, escaped;
		if (parser->at >= parser->length) {
			parser->error = "a set is not closed";
			return NULL;
		}
		c = (unsigned char)parser->pattern[parser->at];
		if (c == ']' && !first) {
			parser->at++;
			break;
		}
		first = R_FALSE;
		if (c == '\\') {
			parser->at++;
			escaped = rvn_re_escape(parser, &cls);
			if (escaped == -2) return NULL;
			if (escaped >= 0) rvn_re_class_set(&cls, escaped);
			continue;
		}
		parser->at++;
		if (parser->at + 1 < parser->length && parser->pattern[parser->at] == '-' && parser->pattern[parser->at + 1] != ']') {
			int to = (unsigned char)parser->pattern[parser->at + 1], k;
			parser->at += 2;
			if (to < c) {
				parser->error = "a range goes backwards";
				return NULL;
			}
			for (k = c; k <= to; k++) rvn_re_class_set(&cls, k);
			continue;
		}
		rvn_re_class_set(&cls, c);
	}
	if (inverse) {
		int k;
		for (k = 0; k < 32; k++) cls.bits[k] = (uint8_t)~cls.bits[k];
	}
	RVN_ARRAY_RESERVE(parser->builder->classes, parser->builder->class_cap, parser->builder->class_count + 1);
	parser->builder->classes[parser->builder->class_count] = cls;
	node = rvn_re_node(RVN_NODE_CLASS);
	node->value = parser->builder->class_count++;
	return node;
}

static rvn_re_node_t* rvn_re_parse_atom(rvn_re_parser_t* parser) {
	char c;
	rvn_re_node_t* node;
	if (parser->at >= parser->length) return NULL;
	c = parser->pattern[parser->at];
	switch (c) {
		case '.':
			parser->at++;
			return rvn_re_node(RVN_NODE_ANY);
		case '^':
			parser->at++;
			return rvn_re_node(RVN_NODE_BOL);
		case '$':
			parser->at++;
			return rvn_re_node(RVN_NODE_EOL);
		case '[':
			parser->at++;
			return rvn_re_parse_set(parser);
		case '(': {
			rvn_re_node_t* inner;
			int group = -1;
			parser->at++;
			if (parser->at + 1 < parser->length && parser->pattern[parser->at] == '?' && parser->pattern[parser->at + 1] == ':') {
				parser->at += 2;
			} else if (parser->at < parser->length && parser->pattern[parser->at] == '?') {
				parser->error = "only (?:...) is known after (";
				return NULL;
			} else {
				group = parser->groups++;
				if (group >= RVN_RE_MAX_GROUPS) {
					parser->error = "too many groups";
					return NULL;
				}
			}
			inner = rvn_re_parse_alt(parser);
			if (!inner) return NULL;
			if (parser->at >= parser->length || parser->pattern[parser->at] != ')') {
				parser->error = "a group is not closed";
				rvn_re_node_free(inner);
				return NULL;
			}
			parser->at++;
			if (group < 0) return inner;
			node = rvn_re_node(RVN_NODE_GROUP);
			node->value = group;
			node->left = inner;
			return node;
		}
		case '\\': {
			int escaped;
			parser->at++;
			if (parser->at < parser->length && (parser->pattern[parser->at] == 'b' || parser->pattern[parser->at] == 'B')) {
				char kind = parser->pattern[parser->at++];
				return rvn_re_node(kind == 'b' ? RVN_NODE_WORD_BOUNDARY : RVN_NODE_NOT_WORD_BOUNDARY);
			}
			if (parser->at < parser->length && strchr("dDwWsS", parser->pattern[parser->at]) != NULL) {
				rvn_re_class_t cls;
				char kind = parser->pattern[parser->at++];
				memset(&cls, 0, sizeof(cls));
				rvn_re_class_shorthand(&cls, kind);
				RVN_ARRAY_RESERVE(parser->builder->classes, parser->builder->class_cap, parser->builder->class_count + 1);
				parser->builder->classes[parser->builder->class_count] = cls;
				node = rvn_re_node(RVN_NODE_CLASS);
				node->value = parser->builder->class_count++;
				return node;
			}
			escaped = rvn_re_escape(parser, NULL);
			if (escaped == -2) return NULL;
			node = rvn_re_node(RVN_NODE_CHAR);
			node->value = escaped < 0 ? 0 : escaped;
			return node;
		}
		case ')':
		case '|':
		case '*':
		case '+':
		case '?':
			parser->error = "nothing to repeat";
			return NULL;
		default:
			parser->at++;
			node = rvn_re_node(RVN_NODE_CHAR);
			node->value = (unsigned char)c;
			return node;
	}
}

/* The bounds of a repetition: {n}, {n,}, {n,m}. */
static int rvn_re_bounds(rvn_re_parser_t* parser, int* min, int* max) {
	int at = parser->at + 1, a = 0, b = -1, digits = 0;
	while (at < parser->length && rvn_re_is_digit((unsigned char)parser->pattern[at])) {
		a = a * 10 + (parser->pattern[at] - '0');
		if (a > 1000) a = 1000;
		at++;
		digits++;
	}
	if (!digits) return R_FALSE;
	if (at < parser->length && parser->pattern[at] == ',') {
		at++;
		if (at < parser->length && rvn_re_is_digit((unsigned char)parser->pattern[at])) {
			b = 0;
			while (at < parser->length && rvn_re_is_digit((unsigned char)parser->pattern[at])) {
				b = b * 10 + (parser->pattern[at] - '0');
				if (b > 1000) b = 1000;
				at++;
			}
		}
	} else {
		b = a;
	}
	if (at >= parser->length || parser->pattern[at] != '}') return R_FALSE;
	if (b >= 0 && b < a) return R_FALSE;
	parser->at = at + 1;
	*min = a;
	*max = b;
	return R_TRUE;
}

static rvn_re_node_t* rvn_re_parse_piece(rvn_re_parser_t* parser) {
	rvn_re_node_t* atom = rvn_re_parse_atom(parser);
	int min = 1, max = 1, greedy = R_TRUE;
	rvn_re_node_t* repeat;
	if (!atom) return NULL;
	if (parser->at >= parser->length) return atom;
	switch (parser->pattern[parser->at]) {
		case '*': min = 0; max = -1; parser->at++; break;
		case '+': min = 1; max = -1; parser->at++; break;
		case '?': min = 0; max = 1; parser->at++; break;
		case '{':
			if (!rvn_re_bounds(parser, &min, &max)) return atom;
			break;
		default:
			return atom;
	}
	if (parser->at < parser->length && parser->pattern[parser->at] == '?') {
		greedy = R_FALSE;
		parser->at++;
	}
	repeat = rvn_re_node(RVN_NODE_REPEAT);
	repeat->min = min;
	repeat->max = max;
	repeat->greedy = greedy;
	repeat->left = atom;
	return repeat;
}

/* Pieces back to back, as a tree of concatenations on the left. */
static rvn_re_node_t* rvn_re_parse_seq(rvn_re_parser_t* parser) {
	rvn_re_node_t* left = NULL;
	while (parser->at < parser->length) {
		char c = parser->pattern[parser->at];
		rvn_re_node_t* piece;
		rvn_re_node_t* concat;
		if (c == '|' || c == ')') break;
		piece = rvn_re_parse_piece(parser);
		if (!piece) {
			rvn_re_node_free(left);
			return NULL;
		}
		if (!left) {
			left = piece;
			continue;
		}
		concat = rvn_re_node(RVN_NODE_CONCAT);
		concat->left = left;
		concat->right = piece;
		left = concat;
	}
	if (!left) left = rvn_re_node(RVN_NODE_EMPTY);
	return left;
}

static rvn_re_node_t* rvn_re_parse_alt(rvn_re_parser_t* parser) {
	rvn_re_node_t* left = rvn_re_parse_seq(parser);
	if (!left) return NULL;
	while (parser->at < parser->length && parser->pattern[parser->at] == '|') {
		rvn_re_node_t* right;
		rvn_re_node_t* alt;
		parser->at++;
		right = rvn_re_parse_seq(parser);
		if (!right) {
			rvn_re_node_free(left);
			return NULL;
		}
		alt = rvn_re_node(RVN_NODE_ALT);
		alt->left = left;
		alt->right = right;
		left = alt;
	}
	return left;
}

/* ---------------------------------------------------------------------------
 *  Compiler
 * ------------------------------------------------------------------------- */

static rvn_re_frag_t rvn_re_compile(rvn_re_builder_t* b, const rvn_re_node_t* node, int depth);

static rvn_re_frag_t rvn_re_frag_empty(rvn_re_builder_t* b) {
	rvn_re_frag_t frag;
	frag.start = rvn_re_emit(b, RVN_RE_JUMP, 0, 0, 0);
	frag.out = rvn_re_patch_add(b, frag.start, 0);
	return frag;
}

/* A copy of a node, so that a repetition compiles it as many times as it runs. */
static rvn_re_node_t* rvn_re_node_copy(const rvn_re_node_t* node) {
	rvn_re_node_t* copy;
	if (!node) return NULL;
	copy = rvn_re_node(node->kind);
	copy->value = node->value;
	copy->min = node->min;
	copy->max = node->max;
	copy->greedy = node->greedy;
	copy->left = rvn_re_node_copy(node->left);
	copy->right = rvn_re_node_copy(node->right);
	return copy;
}

static rvn_re_frag_t rvn_re_compile_star(rvn_re_builder_t* b, const rvn_re_node_t* node, int greedy, int depth) {
	rvn_re_frag_t frag, body;
	int split = rvn_re_emit(b, RVN_RE_SPLIT, 0, 0, 0);
	int jump;
	body = rvn_re_compile(b, node, depth + 1);
	jump = rvn_re_emit(b, RVN_RE_JUMP, split, 0, 0);
	rvn_re_patch_apply(b, body.out, jump);
	/* greedy: the body first, then out; lazy: the other way */
	b->code[split].a = greedy ? body.start : split + 1;
	b->code[split].b = greedy ? jump + 1 : body.start;
	frag.start = split;
	frag.out = greedy ? rvn_re_patch_add(b, split, 1) : rvn_re_patch_add(b, split, 0);
	return frag;
}

static rvn_re_frag_t rvn_re_compile_optional(rvn_re_builder_t* b, const rvn_re_node_t* node, int greedy, int depth) {
	rvn_re_frag_t frag, body;
	int split = rvn_re_emit(b, RVN_RE_SPLIT, 0, 0, 0);
	body = rvn_re_compile(b, node, depth + 1);
	b->code[split].a = greedy ? body.start : split + 1;
	b->code[split].b = greedy ? split + 1 : body.start;
	frag.start = split;
	frag.out = rvn_re_patch_join(b, body.out, rvn_re_patch_add(b, split, greedy ? 1 : 0));
	return frag;
}

/** One copy of a piece of a repetition, joined after `into`. */
static void rvn_re_compile_join(rvn_re_builder_t* b, rvn_re_frag_t* into, rvn_re_frag_t piece) {
	if (into->out >= 0) {
		rvn_re_patch_apply(b, into->out, piece.start);
		into->out = piece.out;
	} else {
		into->out = rvn_re_patch_join(b, into->out, piece.out);
	}
}

static rvn_re_frag_t rvn_re_compile_repeat(rvn_re_builder_t* b, const rvn_re_node_t* node, int depth) {
	rvn_re_frag_t frag;
	int i;
	if (depth > 200) {
		b->error = "the pattern is too deep";
		return rvn_re_frag_empty(b);
	}
	frag.start = -1;
	frag.out = -1;
	for (i = 0; i < node->min; i++) {
		rvn_re_frag_t piece = rvn_re_compile(b, node->left, depth + 1);
		if (frag.start < 0) {
			frag.start = piece.start;
			frag.out = piece.out;
		} else {
			rvn_re_compile_join(b, &frag, piece);
		}
	}
	if (node->max < 0) {
		rvn_re_frag_t star = rvn_re_compile_star(b, node->left, node->greedy, depth + 1);
		if (frag.start < 0) return star;
		rvn_re_compile_join(b, &frag, star);
		return frag;
	}
	for (i = node->min; i < node->max; i++) {
		rvn_re_frag_t piece = rvn_re_compile_optional(b, node->left, node->greedy, depth + 1);
		if (frag.start < 0) {
			frag.start = piece.start;
			frag.out = piece.out;
		} else {
			rvn_re_compile_join(b, &frag, piece);
		}
	}
	if (frag.start < 0) return rvn_re_frag_empty(b);
	return frag;
}

static rvn_re_frag_t rvn_re_compile(rvn_re_builder_t* b, const rvn_re_node_t* node, int depth) {
	rvn_re_frag_t frag;
	if (!node) return rvn_re_frag_empty(b);
	/* (a piece that goes on with what comes after it: nothing to patch) */
	frag.start = -1;
	frag.out = -1;
	switch (node->kind) {
		case RVN_NODE_CHAR:
			frag.start = rvn_re_emit(b, RVN_RE_CHAR, node->value, 0, 0);
			break;
		case RVN_NODE_ANY:
			frag.start = rvn_re_emit(b, RVN_RE_ANY, 0, 0, 0);
			break;
		case RVN_NODE_CLASS:
			frag.start = rvn_re_emit(b, RVN_RE_CLASS, 0, 0, node->value);
			break;
		case RVN_NODE_BOL:
			frag.start = rvn_re_emit(b, RVN_RE_BOL, 0, 0, 0);
			break;
		case RVN_NODE_EOL:
			frag.start = rvn_re_emit(b, RVN_RE_EOL, 0, 0, 0);
			break;
		case RVN_NODE_WORD_BOUNDARY:
			frag.start = rvn_re_emit(b, RVN_RE_WORD_BOUNDARY, 0, 0, 0);
			break;
		case RVN_NODE_NOT_WORD_BOUNDARY:
			frag.start = rvn_re_emit(b, RVN_RE_NOT_WORD_BOUNDARY, 0, 0, 0);
			break;
		case RVN_NODE_GROUP: {
			rvn_re_frag_t inner;
			int save = rvn_re_emit(b, RVN_RE_SAVE, node->value * 2, 0, 0);
			int close, next;
			inner = rvn_re_compile(b, node->left, depth + 1);
			close = rvn_re_emit(b, RVN_RE_SAVE, node->value * 2 + 1, 0, 0);
			rvn_re_patch_apply(b, inner.out, close);
			/* (a jump of its own: the field `a` of a SAVE is its group) */
			next = rvn_re_emit(b, RVN_RE_JUMP, 0, 0, 0);
			frag.start = save;
			frag.out = rvn_re_patch_add(b, next, 0);
			return frag;
		}
		case RVN_NODE_CONCAT: {
			rvn_re_frag_t left = rvn_re_compile(b, node->left, depth + 1);
			rvn_re_frag_t right;
			right = rvn_re_compile(b, node->right, depth + 1);
			rvn_re_patch_apply(b, left.out, right.start);
			frag.start = left.start;
			frag.out = right.out;
			return frag;
		}
		case RVN_NODE_ALT: {
			rvn_re_frag_t left, right;
			int split = rvn_re_emit(b, RVN_RE_SPLIT, 0, 0, 0);
			int jump;
			left = rvn_re_compile(b, node->left, depth + 1);
			jump = rvn_re_emit(b, RVN_RE_JUMP, 0, 0, 0);
			right = rvn_re_compile(b, node->right, depth + 1);
			b->code[split].a = left.start;
			b->code[split].b = right.start;
			rvn_re_patch_apply(b, left.out, jump);
			frag.start = split;
			frag.out = rvn_re_patch_join(b, rvn_re_patch_add(b, jump, 0), right.out);
			return frag;
		}
		case RVN_NODE_REPEAT: {
			rvn_re_node_t* copy = rvn_re_node_copy(node);
			frag = rvn_re_compile_repeat(b, copy, depth + 1);
			rvn_re_node_free(copy);
			return frag;
		}
		case RVN_NODE_EMPTY:
		default:
			return rvn_re_frag_empty(b);
	}
	return frag;
}

rvn_regex_t* rvn_regex_compile(const char* pattern, int ignore_case, int multiline, const char** error) {
	rvn_re_builder_t builder;
	rvn_re_parser_t parser;
	rvn_re_node_t* tree;
	rvn_re_frag_t frag;
	rvn_regex_t* regex;
	int match;

	if (error) *error = NULL;
	if (!pattern) {
		if (error) *error = "no pattern";
		return NULL;
	}
	memset(&builder, 0, sizeof(builder));
	memset(&parser, 0, sizeof(parser));
	parser.pattern = pattern;
	parser.length = (int)strlen(pattern);
	parser.builder = &builder;
	parser.groups = 1;
	tree = rvn_re_parse_alt(&parser);
	if (!tree || parser.at < parser.length) {
		if (error) *error = parser.error ? parser.error : "the pattern is not valid";
		rvn_re_node_free(tree);
		rvn_free(builder.code);
		rvn_free(builder.patches);
		rvn_free(builder.classes);
		return NULL;
	}
	{
		/* (the whole match is group 0: the pattern is wrapped in its own SAVE, start and end) */
		int save = rvn_re_emit(&builder, RVN_RE_SAVE, 0, 0, 0);
		rvn_re_frag_t inner = rvn_re_compile(&builder, tree, 0);
		int close = rvn_re_emit(&builder, RVN_RE_SAVE, 1, 0, 0);
		rvn_re_patch_apply(&builder, inner.out, close);
		frag.start = save;
		/* (a jump of its own: the patch of a SAVE would write over its group) */
		frag.out = rvn_re_patch_add(&builder, rvn_re_emit(&builder, RVN_RE_JUMP, 0, 0, 0), 0);
	}
	rvn_re_node_free(tree);
	if (builder.error) {
		if (error) *error = builder.error;
		rvn_free(builder.code);
		rvn_free(builder.patches);
		rvn_free(builder.classes);
		return NULL;
	}
	match = rvn_re_emit(&builder, RVN_RE_MATCH, 0, 0, 0);
	rvn_re_patch_apply(&builder, frag.out, match);

	regex = (rvn_regex_t*)rvn_calloc(sizeof(rvn_regex_t));
	regex->code = builder.code;
	regex->count = builder.count;
	regex->classes = builder.classes;
	regex->class_count = builder.class_count;
	regex->groups = parser.groups;
	regex->ignore_case = ignore_case ? R_TRUE : R_FALSE;
	regex->multiline = multiline ? R_TRUE : R_FALSE;
	rvn_free(builder.patches);
	return regex;
}

void rvn_regex_free(rvn_regex_t* regex) {
	if (!regex) return;
	rvn_free(regex->code);
	rvn_free(regex->classes);
	rvn_free(regex);
}

int rvn_regex_group_count(const rvn_regex_t* regex) {
	return regex ? regex->groups : 0;
}

/* ---------------------------------------------------------------------------
 *  Matcher
 * ------------------------------------------------------------------------- */

typedef struct rvn_re_run_t {
	const rvn_regex_t* regex;
	const char* text;
	int length;
	int* groups;
	long steps;
	int failed;
} rvn_re_run_t;

static int rvn_re_match_at(rvn_re_run_t* run, int pc, int at, int depth);

/* R_TRUE when the instruction takes the byte at `at`. */
static int rvn_re_takes(rvn_re_run_t* run, const rvn_re_inst_t* inst, int at) {
	int c;
	if (at >= run->length) return R_FALSE;
	c = (unsigned char)run->text[at];
	switch (inst->op) {
		case RVN_RE_CHAR:
			if (run->regex->ignore_case) {
				int a = c >= 'A' && c <= 'Z' ? c + 32 : c;
				int b = inst->a >= 'A' && inst->a <= 'Z' ? inst->a + 32 : inst->a;
				return a == b;
			}
			return c == inst->a;
		case RVN_RE_ANY:
			return c != '\n';
		case RVN_RE_CLASS:
			if (rvn_re_class_has(&run->regex->classes[inst->cls], c)) return R_TRUE;
			if (run->regex->ignore_case) {
				int other = c >= 'A' && c <= 'Z' ? c + 32 : c >= 'a' && c <= 'z' ? c - 32 : -1;
				return other >= 0 && rvn_re_class_has(&run->regex->classes[inst->cls], other);
			}
			return R_FALSE;
		default:
			return R_FALSE;
	}
}

static int rvn_re_boundary(rvn_re_run_t* run, int at) {
	int before = at > 0 ? (unsigned char)run->text[at - 1] : -1;
	int after = at < run->length ? (unsigned char)run->text[at] : -1;
	return rvn_re_is_word(before) != rvn_re_is_word(after);
}

static int rvn_re_match_at(rvn_re_run_t* run, int pc, int at, int depth) {
	if (--run->steps <= 0 || depth > RVN_RE_MAX_DEPTH) {
		run->failed = R_TRUE;
		return R_FALSE;
	}
	for (;;) {
		const rvn_re_inst_t* inst = &run->regex->code[pc];
		switch (inst->op) {
			case RVN_RE_CHAR:
			case RVN_RE_ANY:
			case RVN_RE_CLASS:
				if (!rvn_re_takes(run, inst, at)) return R_FALSE;
				at++;
				pc++;
				continue;
			case RVN_RE_SPLIT:
				if (rvn_re_match_at(run, inst->a, at, depth + 1)) return R_TRUE;
				if (run->failed) return R_FALSE;
				pc = inst->b;
				continue;
			case RVN_RE_JUMP:
				pc = inst->a;
				continue;
			case RVN_RE_SAVE: {
				int group = inst->a / 2, which = inst->a % 2, old;
				if (group >= run->regex->groups) return R_FALSE;
				old = run->groups[group * 2 + which];
				run->groups[group * 2 + which] = at;
				if (rvn_re_match_at(run, pc + 1, at, depth + 1)) return R_TRUE;
				run->groups[group * 2 + which] = old;
				return R_FALSE;
			}
			case RVN_RE_BOL:
				if (at == 0 || (run->regex->multiline && run->text[at - 1] == '\n')) {
					pc++;
					continue;
				}
				return R_FALSE;
			case RVN_RE_EOL:
				if (at == run->length || (run->regex->multiline && run->text[at] == '\n')) {
					pc++;
					continue;
				}
				return R_FALSE;
			case RVN_RE_WORD_BOUNDARY:
				if (!rvn_re_boundary(run, at)) return R_FALSE;
				pc++;
				continue;
			case RVN_RE_NOT_WORD_BOUNDARY:
				if (rvn_re_boundary(run, at)) return R_FALSE;
				pc++;
				continue;
			case RVN_RE_MATCH:
				return R_TRUE;
			default:
				return R_FALSE;
		}
	}
}

int rvn_regex_search(const rvn_regex_t* regex, const char* text, int length, int start, int* groups) {
	rvn_re_run_t run;
	int at, i;
	if (!regex || !text || start < 0) return -1;
	run.regex = regex;
	run.text = text;
	run.length = length;
	run.groups = groups;
	for (at = start; at <= length; at++) {
		for (i = 0; i < regex->groups * 2; i++) groups[i] = -1;
		run.steps = RVN_RE_MAX_STEPS;
		run.failed = R_FALSE;
		if (rvn_re_match_at(&run, 0, at, 0)) return at;
		if (run.failed) return -1;
	}
	return -1;
}

int rvn_regex_matches(const rvn_regex_t* regex, const char* text, int length) {
	int groups[RVN_RE_MAX_GROUPS * 2];
	return rvn_regex_search(regex, text, length, 0, groups) >= 0;
}
