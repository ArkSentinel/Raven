/**
 * Raven
 * raven_process.h : running other programs of the computer without blocking
 * the interface.
 *
 * An editor needs latexmk, pdflatex, bibtex, hunspell or git, and none of them
 * may freeze the window while it runs. A process started here is watched by a
 * thread of the library that reads its output and posts what it reads to the
 * UI thread: the callbacks below run during a frame, where touching widgets is
 * allowed.
 *
 *     static void on_output(const char* text, size_t length, int is_stderr, void* user) {
 *         Rtext_editor_insert_text(log_view, text, length);   // the UI thread
 *     }
 *     static void on_exit(int code, int failed, void* user) {
 *         ...
 *     }
 *
 *     const char* arguments[] = { "-synctex=1", "main.tex", NULL };
 *     RProcessDesc desc = {0};
 *     desc.program = "pdflatex";
 *     desc.arguments = arguments;
 *     desc.merge_stderr = R_TRUE;
 *     Rprocess_start(&desc, on_output, on_exit, NULL);
 *
 * No shell is involved: the program is run directly with the arguments given,
 * so spaces and quotes in a path or a file name are safe.
 *
 * The processes a program starts belong to the library. Rshutdown asks every
 * process still running to stop, waits for it and frees it; there is nothing
 * to destroy. A process that ended keeps its exit code until the shutdown, so
 * Rprocess_get_exit_code stays readable after the callbacks.
 */

#ifndef RAVEN_PROCESS_H
#define RAVEN_PROCESS_H

#include "raven_core.h"

#ifdef __cplusplus
extern "C" {
#endif

	typedef enum RProcessState {
		/** Started, its output is still being read. */
		R_PROCESS_RUNNING = 0x0400,
		/** Ended by itself: Rprocess_get_exit_code says how. */
		R_PROCESS_EXITED,
		/** Could not be run at all (not found, no permission): the exit code is -1. */
		R_PROCESS_FAILED
	} RProcessState;

	/** What to run and how. A zero-initialized structure is a valid, empty description. */
	typedef struct RProcessDesc {
		/** "latexmk", or an absolute path. Looked up in PATH when it has no separator. */
		const char* program;
		/** NULL-terminated argv, NOT including argv[0]. May be NULL. */
		const char* const* arguments;
		/** How many arguments there are, argv[0] not counted. 0 = count them up to the NULL. */
		int argument_count;
		/** NULL = the current one. */
		const char* working_directory;
		/**
		 * R_TRUE: stderr goes to the same stream as stdout, which is what a
		 * build log wants (the warnings of a compiler are as interesting as
		 * its errors, and the order between them is kept).
		 */
		int merge_stderr;
	} RProcessDesc;

	/** A piece of output, as it arrives. `text` is not null-terminated. Runs on the UI thread. */
	typedef void (*RProcessOutputCallback)(const char* text, size_t length, int is_stderr, void* user_data);
	/** The process ended. `failed` is R_TRUE when it could not be run at all. Runs on the UI thread. */
	typedef void (*RProcessExitCallback)(int exit_code, int failed, void* user_data);

	/**
	 * @brief Starts a program and returns at once.
	 * @param desc What to run. Copied: the strings may be freed after the call.
	 * @param output Called for every piece of output, or NULL. May be called
	 * several times, and not at all for a program that prints nothing.
	 * @param exited Called once when the program ends, or NULL.
	 * @param user_data Passed to both callbacks.
	 * @return The process, or R_INVALID_ID when it could not even be started
	 * (an empty description, no Raven). A program that does not exist is
	 * reported through `exited` with failed set, not here.
	 */
	RAVEN_API Rid Rprocess_start(const RProcessDesc* desc, RProcessOutputCallback output,
		RProcessExitCallback exited, void* user_data);

	/** @return R_TRUE while the program runs. */
	RAVEN_API int Rprocess_is_running(Rid process);

	/** @return The exit code, or -1 while the program runs and when it could not be run. */
	RAVEN_API int Rprocess_get_exit_code(Rid process);

	/** @return R_PROCESS_RUNNING, R_PROCESS_EXITED or R_PROCESS_FAILED. */
	RAVEN_API RProcessState Rprocess_get_state(Rid process);

	/**
	 * @brief Asks the program to stop (SIGTERM on POSIX, TerminateProcess on
	 * Windows), waits a moment, then kills it, and returns once its output has
	 * been read to the end. The exit callback still runs, during a frame.
	 * Does nothing when the program already ended.
	 */
	RAVEN_API void Rprocess_cancel(Rid process);

	/**
	 * @brief Blocks until the program ends. For tests and shutdown: never call
	 * it from the UI thread of a running application.
	 */
	RAVEN_API void Rprocess_wait(Rid process);

	/**
	 * @brief Frees a process that ended. Its callbacks are not called again.
	 * A process still running is stopped first, as Rprocess_cancel does. The
	 * library keeps every process until Rshutdown, so an application that
	 * starts many of them (a check after each pause) calls this when it no
	 * longer needs the exit code.
	 */
	RAVEN_API void Rprocess_destroy(Rid process);

	/** @return R_TRUE when the program is found (the same lookup as Rprocess_find). */
	RAVEN_API int Rprocess_is_available(const char* program);

	/**
	 * @brief Finds a program without running it.
	 * @return An absolute path (in a buffer of the library, valid until the
	 * next call), or NULL when it is nowhere. A program with a separator in it
	 * is checked as given. The usual install folders of TeX are searched too,
	 * after PATH: /Library/TeX/texbin, /usr/local/texlive/ * /bin/ *,
	 * /opt/homebrew/bin, C:\\texlive\\ * \\bin\\windows and the bin folder of
	 * MiKTeX (%LOCALAPPDATA% and %PROGRAMFILES%).
	 */
	RAVEN_API const char* Rprocess_find(const char* program);
	/**
	 * @brief Same as Rprocess_find, but the path is a copy the application owns: it stays the same
	 * after the next call. Its length (without the terminating zero) is written to `size` when it is
	 * not NULL. Free it with Rmemory_free. NULL when the program is not found.
	 */
	RAVEN_API char* Rprocess_find_copy(const char* program, size_t* size);

	/**
	 * @brief Runs a program to the end and collects what it printed.
	 * Blocking: for setup work (asking git for the state of a file, a tool for
	 * its version), never for a build. The window is frozen meanwhile.
	 * @param output Receives the output, NUL-terminated, allocated with the
	 * allocator of Raven: free it with Rmemory_free. May be NULL.
	 * @param size Receives its length, without the NUL. May be NULL.
	 * @param max_bytes Longest output kept; 0 selects 4 MiB. What comes after
	 * is read and dropped, so the program never blocks on a full pipe. A cut
	 * never leaves half of a UTF-8 character.
	 * @return The exit code, or -1 when the program could not be run.
	 */
	RAVEN_API char* Rprocess_run(const RProcessDesc* desc, size_t* size, size_t max_bytes);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RAVEN_PROCESS_H */
