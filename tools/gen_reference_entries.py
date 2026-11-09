#!/usr/bin/env python3
"""
Adds to docs/reference.html the entries of functions that are in the headers
but not in the page, in the format the page uses. The page is still written
by hand for its prose; this only keeps the entries from falling behind.

    python3 tools/gen_reference_entries.py [--check]

With --check it says what is missing and writes nothing.
"""
import html
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PAGE = os.path.join(ROOT, "docs", "reference.html")

# (header, section title or None, the functions in order; None = all that are missing)
SECTIONS = [
    ("raven_text_editor.h", "Search and replace", [
        "Rtext_editor_set_search", "Rtext_editor_get_search", "Rtext_editor_get_search_flags",
        "Rtext_editor_find_next", "Rtext_editor_count_matches", "Rtext_editor_get_match",
        "Rtext_editor_replace", "Rtext_editor_replace_all", "Rtext_editor_expand_replacement"]),
    ("raven_text_editor.h", "Folding", [
        "Rtext_editor_set_folding", "Rtext_editor_get_folding", "Rtext_editor_toggle_fold",
        "Rtext_editor_fold_line", "Rtext_editor_unfold_line", "Rtext_editor_is_folded",
        "Rtext_editor_unfold_all", "Rtext_editor_get_folded_count", "Rtext_editor_get_fold_start"]),
    ("raven_text_editor.h", "Snippets", [
        "Rsnippet_add", "Rsnippet_clear", "Rsnippet_get_count", "Rsnippet_get_name", "Rsnippet_find",
        "Rsnippet_set_variable", "Rtext_editor_insert_snippet", "Rtext_editor_snippet_at_caret",
        "Rtext_editor_set_snippet_completion", "Rtext_editor_is_in_snippet"]),
    ("raven_text_editor.h", "Several cursors", [
        "Rtext_editor_set_multi_cursor", "Rtext_editor_get_multi_cursor", "Rtext_editor_get_caret_count",
        "Rtext_editor_get_caret", "Rtext_editor_add_caret", "Rtext_editor_clear_carets",
        "Rtext_editor_select_next_occurrence"]),
    ("raven_text_editor.h", "Words and lines", [
        "Rtext_editor_get_word_count", "Rtext_editor_get_character_count",
        "Rtext_editor_get_non_empty_line_count", "Rtext_editor_get_current_line"]),
    ("raven_text_editor.h", "Markers", [
        "Rtext_editor_get_marker", "Rtext_editor_clear_markers_of_line"]),
    ("raven_style.h", "The theme of a project", [
        "Rtheme_remember_for", "Rtheme_get_remembered", "Rtheme_apply_remembered"]),
    ("raven_core.h", "Animations", ["Rset_selection_animation", "Rget_selection_animation"]),
    ("raven_window.h", "Keybindings", [
        "Rwindow_set_keybindings", "Rkey_from_name", "Rkey_to_name",
        "Rwindow_get_shortcut_count", "Rwindow_get_shortcut"]),
    ("raven_latex.h", "Reading a log", [
        "Rlatex_log_parse", "Rlatex_log_parse_file", "Rlatex_log_get_count",
        "Rlatex_log_get_message", "Rlatex_log_destroy"]),
    ("raven_settings.h", "Settings", None),
]

# Headers whose every function must be in the page: the ones this fork owns
# (the rest of the page documents what it chooses, as it always did).
COMPLETE = ("raven_pdf.h", "raven_process.h", "raven_latex.h", "raven_settings.h", "raven_text_editor.h")

LINKED = ("Rid", "RKey", "Ruint", "RToken", "RLanguageDesc", "RSnippet", "RMarkerKind",
          "RSettingType", "RSearchFlags", "RProcessDesc", "RLogMessage", "RLatexBuildDesc",
          "RLatexEngine", "RLogKind", "RProcessOutputCallback", "RLatexBuildFinishedCallback",
          "RProcessExitCallback", "RProcessState", "RPdfFit", "RPdfPageNeededCallback",
          "RPdfPageClickedCallback", "RXmlAttributeInfo", "RTextEditorChangedCallback")


def header_text(header):
    with open(os.path.join(ROOT, "include", "raven", header), encoding="utf-8") as handle:
        return handle.read()


def functions(text):
    """name -> (return type, [(type, name)], documentation)."""
    found = {}
    pattern = r"RAVEN_API\s+([\w\s\*]+?)\s+(\w+)\s*\(([^;]*?)\);"
    for match in re.finditer(pattern, text, flags=re.S):
        ret, name, params = match.group(1).strip(), match.group(2), match.group(3)
        # (the doc block just above, if the lines between hold no other declaration)
        before = text[:match.start()]
        close = before.rfind("*/")
        opening = before.rfind("/**", 0, close) if close >= 0 else -1
        doc = ""
        if close >= 0 and opening >= 0 and ";" not in before[close:] and "typedef" not in before[opening:match.start()]:
            doc = before[opening + 3:close]
        arguments = []
        for part in [p.strip() for p in params.split(",") if p.strip()]:
            part = re.sub(r"\s+", " ", part.replace("\n", " "))
            if part in ("void", ""):
                continue
            if " " in part:
                ctype, pname = part.rsplit(" ", 1)
            else:
                ctype, pname = part, ""
            arguments.append((ctype.strip(), pname.strip()))
        found[name] = (ret, arguments, doc)
    return found


def link_types(text):
    for name in sorted(LINKED, key=len, reverse=True):
        text = re.sub(r"(?<![\w>])%s(?![\w<])" % name, '<a href="#%s">%s</a>' % (name, name), text)
    return text


def signature(ret, name, arguments):
    parts = ["%s %s" % (ctype, pname) for ctype, pname in arguments]
    text = "%s %s(%s);" % (ret, name, ", ".join(parts) if parts else "void")
    return link_types(html.escape(text))


def documentation(doc):
    lines, params, returns = [], [], None
    for raw in doc.split("\n"):
        line = raw.strip().lstrip("*").strip()
        if not line:
            continue
        match = re.match(r"@param\s+(\w+)\s*(.*)", line)
        if match:
            params.append((match.group(1), match.group(2)))
            continue
        match = re.match(r"@return\s*(.*)", line)
        if match:
            returns = match.group(1)
            continue
        if line.startswith("@"):
            continue
        lines.append(line)
    prose = re.sub(r"\s+", " ", " ".join(lines)).strip()
    out = []
    if prose:
        out.append("<p>%s</p>" % link_types(html.escape(prose, quote=False)))
    if params or returns is not None:
        out.append("<dl>")
        for pname, text in params:
            out.append("<dt>%s</dt><dd>%s</dd>" % (pname, link_types(html.escape(text, quote=False))))
        if returns is not None:
            out.append("<dt>returns</dt><dd>%s</dd>" % link_types(html.escape(returns, quote=False)))
        out.append("</dl>")
    return "".join(out)


def entry(name, found):
    ret, arguments, doc = found[name]
    return '<div class="d" id="%s"><pre class="sig">%s</pre>%s</div>' % (
        name, signature(ret, name, arguments), documentation(doc))


def section_id(header, title):
    slug = re.sub(r"[^a-z0-9]+", "-", title.lower()).strip("-")
    return "%s-%s" % (header_id(header), slug)


def header_id(header):
    """The anchor the page gives a header: raven_text_editor.h -> raven_text_editor_h."""
    return header.replace(".", "_")


def insert_after_section(page, header, title, entries):
    """Puts a <h4> section at the end of a header's part of the page."""
    start = page.find('<h2 id="%s">' % header_id(header))
    if start < 0:
        return page, False
    end = page.find("<h2 ", start + 1)
    if end < 0:
        end = page.find('<div class="pager">', start)
    if end < 0:
        end = len(page)
    cut = end
    while cut > start and page[cut - 1] in "\n\t ":
        cut -= 1
    block = '\n<h4 id="%s">%s</h4>\n%s' % (section_id(header, title), title, "\n".join(entries))
    return page[:cut] + block + page[cut:], True


def add_to_index(page, names):
    """Adds the names to the index of functions at the end of the page."""
    for name in names:
        if 'href="#%s"' % name in page.split('<h2 id="index">')[-1]:
            continue
        letter = name[1].upper()
        # the paragraph of that letter: <p><b>X</b> &nbsp; <a...>
        start = page.find('<p><b>%s</b>' % letter)
        if start < 0:
            continue
        end = page.find("</p>", start)
        page = page[:end] + ' &middot; <a href="#%s">%s</a>' % (name, name) + page[end:]
    return page


def documented_entries(page):
    """name -> the signature text the page shows for it."""
    return dict(re.findall(r'<div class="d" id="(R\w+)"><pre class="sig">(.*?)</pre>', page, flags=re.S))


def signature_text(sig):
    """A signature without its links or its spacing, to compare two of them."""
    return re.sub(r"\s+", " ", html.unescape(re.sub(r"<[^>]+>", "", sig))).strip()


def stale_entries(page, headers):
    """The names of the entries of the page whose signature is not the one of the header any more."""
    found = {}
    for header in headers:
        found.update(functions(header_text(header)))
    stale = []
    for name, shown in documented_entries(page).items():
        if name not in found:
            continue
        ret, arguments, _ = found[name]
        if signature_text(shown) != signature_text(signature(ret, name, arguments)):
            stale.append(name)
    return stale


def rewrite_stale(page, headers):
    """Replaces the entries of the page whose signature changed; returns the page and what changed."""
    entries = documented_entries(page)
    found = {}
    for header in headers:
        found.update(functions(header_text(header)))
    changed = []
    for name, shown in entries.items():
        if name not in found:
            continue
        ret, arguments, doc = found[name]
        if signature_text(shown) == signature_text(signature(ret, name, arguments)):
            continue
        old = '<div class="d" id="%s"><pre class="sig">%s</pre>' % (name, shown)
        start = page.find(old)
        if start < 0:
            continue
        end = page.find("</div>", start)
        if end < 0:
            continue
        page = page[:start] + entry(name, found) + page[end + len("</div>"):]
        changed.append(name)
    return page, changed


def main():
    check = "--check" in sys.argv
    page = open(PAGE, encoding="utf-8").read()
    # (every header of the library: the page may fall behind on any of them, not only on the new ones)
    headers = sorted(name for name in os.listdir(os.path.join(ROOT, "include", "raven")) if name.endswith(".h"))
    cache, missing = {}, []
    for header, title, names in SECTIONS:
        if header not in cache:
            cache[header] = functions(header_text(header))
        found = cache[header]
        if names is None:
            names = sorted(name for name in found if ('id="%s"' % name) not in page)
        else:
            # (a header whose every function belongs to the page: a new one is missing too)
            extra = sorted(name for name in found if ('id="%s"' % name) not in page) if header in COMPLETE else []
            names = list(names) + [name for name in extra if name not in names]
        # (a section added in an earlier pass is already in `page`: the same header may come twice)
        entries = []
        for name in names:
            if name not in found:
                print("  %s: not in %s" % (name, header))
                continue
            if ('id="%s"' % name) in page:
                continue
            if any(name == added for added, _ in entries) or name in missing:
                continue
            entries.append((name, entry(name, found)))
        if not entries:
            continue
        missing.extend(name for name, _ in entries)
        if check:
            continue
        new_page, ok = insert_after_section(page, header, title, [text for _, text in entries])
        if not ok:
            print("  header %s: not in the page" % header)
            continue
        page = new_page
    page, stale = rewrite_stale(page, headers)
    if check:
        if missing:
            print("%d entries missing: %s" % (len(missing), ", ".join(missing)))
            return 1
        if stale:
            print("%d entries out of date (run without --check): %s" % (len(stale), ", ".join(stale)))
            return 1
        print("the page has every function of the headers, with the signature each one has")
        return 0
    if stale:
        print("%d entries brought up to date: %s" % (len(stale), ", ".join(stale)))
    page = add_to_index(page, missing)
    with open(PAGE, "w", encoding="utf-8") as handle:
        handle.write(page)
    print("%d entries added" % len(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
