# LocalTeX fork: what it adds to Raven

This branch (`localtex`) carries Raven with the components a LaTeX editor
needs on top of it. Nothing here is specific to one application: every piece
is a general part of the library, documented in the public headers and in
`docs/reference.html`, and usable from any Raven program.

Every entry says what was added, where, and how to use it. Upstream Raven is
untouched apart from the merge that brought the fork level with `main`
(Plot, #53); a diff against upstream is meant to stay readable.

For the exact list of what was implemented, with function counts and test
results, see [IMPLEMENTADO.md](IMPLEMENTADO.md).

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DRAVEN_BUILD_SHARED=ON
cmake --build build
cd build && ctest --output-on-failure -E render
```

`test_render` needs a GPU and a display; it is the only test that does not
run headless. Every other test runs with the null platform and the null
backend, checks for leaks after `Rshutdown`, and passes under AddressSanitizer.

## 1. A PDF preview widget (`raven_pdf.h`, `src/widgets/pdf_view.c`)

Raven does not decode PDF, and this fork does not add a decoder: the
application renders a page (PDFium, Quartz, `pdftoppm`...) and hands the
widget the pixels. What the widget does is the part that is the same for
every application:

- pages laid out one under the other, with a gap, a shadow and a background
  from the theme;
- zoom in pixels per point (Ctrl + wheel, pinch, or from code), and
  `Fit="Width"` / `Fit="Page"` that follow the size of the widget;
- scrolling through the document, page navigation, keyboard, touch;
- a *page needed* callback that says which pages are visible and at what
  scale, so a 400 page thesis renders the three pages that are on screen;
- `Rpdf_view_get_page_at` (page and point under the pointer) and a click
  callback: the half of SyncTeX that goes from the page to the source;
- `Rpdf_view_scroll_to_position` and `Rpdf_view_set_highlight`: the half
  that goes from the source to the page.

```c
Rid view = Rpdf_view_create();
Rpdf_view_set_page_count(view, pages);
for (i = 0; i < pages; i++) Rpdf_view_set_page_size(view, i, 595.0f, 842.0f); /* A4, in points */
Rpdf_view_set_fit(view, R_PDF_FIT_WIDTH);
Rpdf_view_set_page_needed_callback(view, on_page_needed, &document); /* render there */
```

## 2. The editor a LaTeX document needs (`raven_text_editor.h`)

`TextEditor` already had syntax colors, completion, markers and undo. This
fork adds the rest of what an editor of source text is expected to do.

**Languages.** `LaTeX` and `BibTeX` are built in, with their own lexers
(`R_LANGUAGE_TEX`, `R_LANGUAGE_BIBTEX` in `RLanguageFlags`): a backslash
starts a command, `%` a comment, `$...$` mathematics, `\begin{...}` and
`\end{...}` are a pair, and a BibTeX entry is `@type{key, field = {value}}`.
`RLanguageDesc::word_characters` lets a language say that a backslash (TeX)
or an `@` (BibTeX) is part of a name, which is what makes completing `\sec`
propose `\section` and what makes folding `\begin` work.

**Typing.** A `\begin{itemize}` typed in a TeX document gets its
`\end{itemize}` on the line below, with the caret indented inside; Enter
after a `\begin{...}` indents the next line; Ctrl+/ comments lines out with
`%`.

**Search and replace** (`Rtext_editor_set_search`, `Rtext_editor_find_next`,
`Rtext_editor_replace`, `Rtext_editor_replace_all`): plain text or a regular
expression (`R_SEARCH_REGEX`), ignoring case, whole words only. Matches are
computed once per (text, pattern) and highlighted. The engine is
`src/widgets/regex.c`: a small backtracking matcher with a step budget, so a
pathological pattern says "nothing found" instead of freezing the window.

**Folding** (`Rtext_editor_set_folding`, `Rtext_editor_fold_line`,
`Rtext_editor_toggle_fold`, Ctrl+Shift+[ / ]): braces and brackets of a
language of code, `\begin`/`\end`, tags, Markdown titles. The arrow is in the
margin of the line numbers; a folded block shows `...` and its last line
stays visible.

**Snippets** (`Rsnippet_add`, `Rtext_editor_insert_snippet`): `\section`,
`figure`, `table`... Bodies hold `$1`..`$9`, `${1:text}` and `$0`; Tab walks
the places, Escape leaves them. `Rtext_editor_snippet_at_caret` says which
snippet the word being typed would expand.

**Several cursors** (`Rtext_editor_set_multi_cursor`, `Rtext_editor_add_caret`):
Ctrl+click adds a caret; selecting and typing act on all of them. The
replacement of a selection is the base of Ctrl+D (next occurrence).

**Reading back** what an application needs for a status bar and for markers:
`Rtext_editor_get_marker`, `Rtext_editor_clear_markers_of_line`,
`Rtext_editor_get_word_count`, `Rtext_editor_get_character_count`,
`Rtext_editor_get_non_empty_line_count`, `Rtext_editor_get_current_line`.

## 3. Running tools and reading their logs (`raven_process.h`, `raven_latex.h`)

**Processes.** `Rprocess_start` runs a program (`latexmk`, `pdflatex`,
`bibtex`, `hunspell`, `git`) with `fork`/`execvp` on POSIX and
`CreateProcessW` on Windows, reads its output on a thread and posts what it
reads to the UI thread, where the callbacks may touch widgets. No shell is
involved. `Rprocess_cancel` stops it, `Rprocess_run` is the blocking variant
for setup work (bounded output, cut on a character boundary),
`Rprocess_find` looks for a tool in PATH and in the usual folders of TeX Live
and MiKTeX.

**Logs.** `Rlatex_log_parse` turns the wall of text pdflatex writes into the
few lines that matter, as a list of `RLogMessage`: an error with its line
and file, a package warning with `on input line N`, an overfull box. The list
is a handle read with `Rlatex_log_get_message`, like everything else Raven
returns.

**Builds.** `Rlatex_build_start` runs latexmk (or one engine) on a root file,
streams its output, then parses the log and keeps the messages. It never
blocks: the window stays alive while chapters compile. `Rshutdown` stops what
still runs.

```c
RLatexBuildDesc desc = {0};
desc.root_file = "paper/main.tex";
desc.use_latexmk = R_TRUE;
Rid build = Rlatex_build_start(&desc, on_output, on_finished, NULL);
/* in on_finished: Rlatex_build_get_messages(build) -> markers */
```

## 4. What the application remembers (`raven_settings.h`)

A store of settings backed by JSON, in the usual folder of the platform
(`~/.config/<app>`, `~/Library/Application Support/<app>`, `%APPDATA%\<app>`):
booleans, integers, numbers and texts under dotted keys, the whole document
as text, nested objects and arrays, and a reader that tolerates comments and
a trailing comma. `Rsettings_open` makes the folder and the file.

On top of it:

- **Keybindings from the settings** (`Rwindow_set_keybindings`,
  `Rkey_from_name`, `Rkey_to_name`, `Rwindow_get_shortcut`): an application
  can offer a preferences window without a table of keys compiled in.
- **The theme of a project** (`Rtheme_remember_for`,
  `Rtheme_apply_remembered`): the theme in use is remembered per project
  folder and applied when it is opened again.

## 5. An interface that follows the pointer (#20)

The selection highlight of lists, tables, trees and the file browser glided
behind the pointer because it was drawn with a spring. Now a click puts it
where the click is, at once; only the keyboard moves it with the spring, and
`Rset_selection_animation(R_TRUE)` brings the glide back for applications
that want it. A window resize is never animated: the client area follows the
compositor exactly (and on Wayland the surface is committed as soon as the
new size is known, instead of with the next frame). The caret of an editor
still glides, which is what a caret is expected to do.

## What was not done

The rest of `MISSING_COMPONENTS.md` needs the application, not the library,
or cannot be done from a Linux machine:

- the `latexmk` call, the log markers and the page rendering are wired
  together by the application (LocalTeX), which is where the document is;
- SyncTeX is a tool that the application runs; the library has both ends
  (`Rpdf_view_get_page_at`, `Rpdf_view_scroll_to_position`);
- packaging, signing, notarization and the Mac App Store are distribution
  work;
- spell checking is `hunspell` through `Rprocess_start`; marking the words is
  `Rtext_editor_add_marker`;
- a CRDT, a local WebSocket, an embedded terminal and the Ollama integration
  are application features, not widgets;
- Markdown preview of a document, visual diff, autocompletion from a `.bib`
  (the completion callback already takes whatever the application proposes)
  and a project template library belong to the application.
