/**
 * Raven
 * raven_latex.h : LaTeX logs read as diagnostics, and a build that runs an
 * engine in the background.
 *
 * A LaTeX log is a wall of text, and what an editor has to show are the few
 * lines that matter: an error with the line it stopped at, a warning with the
 * line it is about, a box that is too wide. Rlatex_log_parse turns the text
 * of a log into a list of RLogMessage, and Rlatex_log_parse_file reads the
 * file first.
 *
 * Rlatex_build_start goes further: it runs latexmk (or one engine) on a root
 * file, streams what it prints to a callback on the UI thread, then parses the
 * log it wrote and keeps the messages. Everything is asynchronous: the window
 * stays alive while chapters compile.
 *
 *     static void on_finished(Rid build, int success, void* user) {
 *         Rid log = Rlatex_build_get_messages(build);
 *         RLogMessage message;
 *         int count = Rlatex_log_get_count(log), i;
 *         for (i = 0; i < count; i++) {
 *             Rlatex_log_get_message(log, i, &message); // markers of the editor
 *         }
 *         Rlatex_build_destroy(build);
 *     }
 *
 *     RLatexBuildDesc desc = {0};
 *     desc.root_file = "paper/main.tex";
 *     desc.engine = R_LATEX_PDFLATEX;
 *     desc.use_latexmk = R_TRUE;
 *     Rlatex_build_start(&desc, on_output, on_finished, NULL);
 *
 * The builds belong to the library: Rshutdown stops the ones still running and
 * frees them. A build destroyed by the application is cancelled first.
 *
 * The functions are for the UI thread unless their comment says otherwise.
 * The strings of an RLogMessage belong to the list it was read from, so a
 * list stays valid until Rlatex_log_destroy, and the one of a build until the
 * build is destroyed or builds again.
 */

#ifndef RAVEN_LATEX_H
#define RAVEN_LATEX_H

#include "raven_core.h"
#include "raven_process.h"

#ifdef __cplusplus
extern "C" {
#endif

	/** What a line of a log is worth to an editor. */
	typedef enum RLogKind {
		/** TeX stopped here ("! Undefined control sequence."). */
		R_LOG_ERROR = 0x0500,
		/** Something that may be wrong (an undefined reference, a font substitution). */
		R_LOG_WARNING,
		/** Neither: a file opened, the pages written, how much memory TeX used. */
		R_LOG_INFO,
		/** Overfull or underfull \hbox or \vbox: not an error, but the page will show it. */
		R_LOG_BAD_BOX
	} RLogKind;

	/** One diagnostic of a log. The strings belong to the list: copy them to keep them. */
	typedef struct RLogMessage {
		RLogKind kind;
		/** The file as the log writes it (often "./chapter.tex"), or NULL. */
		const char* file;
		/** The line, or 0 when the log does not say. */
		int line;
		/** One line, trimmed: "Undefined control sequence." */
		const char* message;
		/**
		 * The lines TeX printed around it (what the error happened in, what
		 * follows), or NULL. Several lines, separated by '\n'.
		 */
		const char* context;
	} RLogMessage;

	/* A list of messages is a handle, as everything else Raven returns:
	   Rlatex_log_parse gives one, Rlatex_log_get_message reads it (its
	   strings are valid until the list is destroyed), Rlatex_log_destroy
	   frees it. */

	/**
	 * @brief Parses the text of a log.
	 * @param log The bytes of the file; need not be NUL-terminated.
	 * @param size Their number.
	 * @return A list of messages, or R_INVALID_ID when the log has nothing
	 * to report (a log with no diagnostic parses to no list at all).
	 */
	RAVEN_API Rid Rlatex_log_parse(const char* log, size_t size);

	/** @brief Reads a log file and parses it. R_INVALID_ID when it cannot be read or has nothing to report. */
	RAVEN_API Rid Rlatex_log_parse_file(const char* path);

	/** @return How many messages the list holds, or 0 for R_INVALID_ID. */
	RAVEN_API int Rlatex_log_get_count(Rid log);

	/**
	 * @brief One message of the list.
	 * @param message Receives the fields; its strings are owned by the list.
	 * @return R_FALSE when `index` is out of range.
	 */
	RAVEN_API int Rlatex_log_get_message(Rid log, int index, RLogMessage* message);

	/** @brief Frees a list given by Rlatex_log_parse. Its messages are no longer valid. */
	RAVEN_API void Rlatex_log_destroy(Rid log);

	/** The engines an editor offers. All of them take the same command line. */
	typedef enum RLatexEngine {
		R_LATEX_PDFLATEX = 0,
		R_LATEX_XELATEX,
		R_LATEX_LUALATEX
	} RLatexEngine;

	/** Called on the UI thread when a build ended. `success` is R_TRUE when the process exited with 0. */
	typedef void (*RLatexBuildFinishedCallback)(Rid build, int success, void* user_data);

	/**
	 * What to build. A zero-initialized structure builds `root_file` with
	 * pdflatex, once; an editor usually sets `use_latexmk` too.
	 */
	typedef struct RLatexBuildDesc {
		/** Path to the main .tex: its folder is the working directory, its name gives the .pdf and the .log. */
		const char* root_file;
		RLatexEngine engine;
		/**
		 * R_TRUE runs latexmk, which does the several passes LaTeX needs (a
		 * table of contents, references); R_FALSE runs the engine once. Since
		 * R_FALSE is 0, a zero-initialized structure runs the engine once.
		 */
		int use_latexmk;
		/** R_TRUE adds -shell-escape: the document may run programs of its own. */
		int shell_escape;
		/**
		 * For tests and unusual tools: the program that runs instead of the
		 * engine and latexmk, with its arguments in `arguments_override`
		 * (NULL-terminated, without argv[0]). The working directory is still
		 * the folder of the root file.
		 */
		const char* command_override;
		const char* const* arguments_override;
		/** How many there are. 0 = count them up to the NULL. */
		int arguments_override_count;
		/** For tests: the log to read when the process ends, instead of the one guessed from the root file. */
		const char* log_path_override;
	} RLatexBuildDesc;

	/**
	 * @brief Starts a build and returns at once.
	 * @param desc What to build; copied.
	 * @param output Called with what the tool prints, as it prints it, on the
	 * UI thread. May be NULL.
	 * @param user_data Passed to `output`.
	 * @return The build, or R_INVALID_ID when it cannot start (no root file).
	 * When the process ends the log is parsed, then `finished` runs.
	 */
	RAVEN_API Rid Rlatex_build_start(const RLatexBuildDesc* desc, RProcessOutputCallback output,
		RLatexBuildFinishedCallback finished, void* user_data);

	/** @return R_TRUE while the tool runs. */
	RAVEN_API int Rlatex_build_is_running(Rid build);

	/** @brief Stops the tool and waits for it. The messages parsed so far stay. */
	RAVEN_API void Rlatex_build_cancel(Rid build);

	/**
	 * @brief The diagnostics of the last finished build, as a list that
	 * belongs to the build: read it with Rlatex_log_get_message, and do not
	 * destroy it. Valid until the build runs again or is destroyed.
	 * @return The list, or R_INVALID_ID before the first build ends.
	 */
	RAVEN_API Rid Rlatex_build_get_messages(Rid build);

	/** @return The .pdf the build wrote, or NULL when it wrote none. Owned by the build. */
	RAVEN_API const char* Rlatex_build_get_pdf_path(Rid build);

	/** @return The .log the build read, or NULL. Owned by the build. */
	RAVEN_API const char* Rlatex_build_get_log_path(Rid build);

	/** @return R_TRUE when the build ended and its tool exited with 0. */
	RAVEN_API int Rlatex_build_succeeded(Rid build);

	/**
	 * @brief Cancels the build if it still runs, frees it and its messages.
	 * The handle is invalid afterwards.
	 */
	RAVEN_API void Rlatex_build_destroy(Rid build);

	/**
	 * @brief Finds a tool of the trade: "latexmk", "pdflatex", "xelatex",
	 * "lualatex", "synctex", "bibtex", "biber", "hunspell", "git"...
	 * Same lookup as Rprocess_find, which it forwards to.
	 * @return An absolute path valid until the next call, or NULL.
	 */
	RAVEN_API const char* Rlatex_find_tool(const char* name);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RAVEN_LATEX_H */
