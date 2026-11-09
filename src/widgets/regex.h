/**
 * Raven - widgets/regex.h
 * A small regular expression engine for the search of the text editor.
 *
 * The syntax it knows, and nothing more:
 *
 *   abc        the bytes themselves
 *   .          any byte but a line break
 *   \d \D \w \W \s \S   a digit, a letter or digit or underscore, a blank
 *   \n \t \r \\ \. \*   an escaped character
 *   [abc] [a-z] [^abc]  a set of bytes
 *   ^ $        the start and the end of the text, or of a line with (?m)
 *   \b \B      a word boundary, or the absence of one
 *   (...)      a group, which the replacement can name ($1)
 *   (?:...)    a group that cannot be named
 *   |          one side or the other
 *   * + ?      none or more, one or more, one or none (lazy with a ? after)
 *   {n} {n,} {n,m}
 *
 * The engine is a backtracking one over a compiled list of instructions.
 * It stops after a budget of steps, so a pattern that would take forever
 * (nested repetitions over a long text) ends instead of hanging the
 * interface; the search then reports no match.
 */

#ifndef RVN_REGEX_H
#define RVN_REGEX_H

#include "../core/base.h"

typedef struct rvn_regex_t rvn_regex_t;

/**
 * Compiles a pattern.
 * @param ignore_case ASCII case is ignored.
 * @param multiline ^ and $ match the ends of every line, not of the text.
 * @param error Set to a text saying what is wrong with the pattern (static,
 *              never freed); NULL when the pattern was taken.
 * @return NULL when the pattern is not valid.
 */
rvn_regex_t* rvn_regex_compile(const char* pattern, int ignore_case, int multiline, const char** error);
void rvn_regex_free(rvn_regex_t* regex);
/** The number of groups of the pattern, the whole match included (1 at least). */
int rvn_regex_group_count(const rvn_regex_t* regex);

/**
 * Looks for the first match that starts at or after `start`.
 * @param groups Receives 2 * rvn_regex_group_count offsets: start and end
 *               of the whole match, then of each group; -1 when a group did
 *               not take part.
 * @return The offset where the match starts, or -1.
 */
int rvn_regex_search(const rvn_regex_t* regex, const char* text, int length, int start, int* groups);

/**
 * R_TRUE when the text holds a match. Cheaper than rvn_regex_search: it
 * stops at the first one.
 */
int rvn_regex_matches(const rvn_regex_t* regex, const char* text, int length);

#endif /* RVN_REGEX_H */
