/**
 * Raven - core/process.c
 * Other programs of the computer, run without blocking the interface.
 *
 * One thread watches each process: it reads what the program prints and, on
 * POSIX, waits for the child. Everything it reads is copied and posted to the
 * UI thread (Rtask_post), so the callbacks of the application always run in a
 * frame, where touching a widget is allowed.
 *
 * The processes belong to the library. They are freed by Rshutdown, which
 * stops the ones still running first; a handle stays valid until then, so an
 * exit code can still be read long after the callbacks ran.
 *
 * References: the pool holds one for the handle, the watching thread one
 * while it runs, and each task posted to the UI thread one. The pool's is
 * released by the destroy path, after the watching thread was joined, so the
 * structure outlives both.
 */

#include "context.h"
#include "fs.h"
#include "handle.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/** What Rprocess_run keeps when the caller gives no limit. */
#define RVN_PROCESS_RUN_MAX (4u * 1024u * 1024u)
/** Longest path Rprocess_find writes in its buffer. */
#define RVN_PROCESS_PATH_MAX 4096
/** Longest a cancelled program may take to answer SIGTERM, in 50 ms steps. */
#define RVN_PROCESS_TERM_STEPS 40

/* ---------------------------------------------------------------------------
 *  State of one process
 * ------------------------------------------------------------------------- */

typedef struct rvn_process_t {
	rvn_atomic_t refs;
	Rid id;
	/** Set before the destroy path: the callbacks are not delivered any more. */
	int destroyed;

	rvn_atomic_t running;
	int exit_code;
	int failed;

	RProcessOutputCallback output;
	RProcessExitCallback exited;
	void* user_data;

	rvn_thread_t thread;
	/** Only the UI thread touches these two. */
	int thread_started;
	int thread_joined;

	/** Guards the child, which the watching thread reaps while the UI thread may cancel it. */
	rvn_mutex_t spawn_mutex;
#if defined(_WIN32)
	HANDLE process;
	HANDLE out_read;
	HANDLE err_read;
#else
	pid_t pid;
	int out_fd;
	int err_fd;
#endif
} rvn_process_t;

/** What a spawn produced, before a thread takes it over. */
typedef struct rvn_spawn_t {
	int failed;
#if defined(_WIN32)
	HANDLE process;
	HANDLE out_read;
	HANDLE err_read;
#else
	pid_t pid;
	int out_fd;
	int err_fd;
#endif
} rvn_spawn_t;

typedef struct rvn_pump_result_t {
	int exit_code;
	int failed;
} rvn_pump_result_t;

/** Receives what a program prints, in the order it printed it. */
typedef void (*rvn_process_sink_t)(void* context, const char* data, size_t length, int is_stderr);

/** A posted piece of output, or the notification that the process ended. */
typedef struct rvn_process_task_t {
	rvn_process_t* process;
	int is_exit;
	char* text;
	size_t length;
	int is_stderr;
} rvn_process_task_t;

static void rvn_process_destroy_id(Rid id);

/* ---------------------------------------------------------------------------
 *  Lifetime
 * ------------------------------------------------------------------------- */

static rvn_process_t* rvn_process_get(Rid id) {
	if (!g_rvn.initialized || id == R_INVALID_ID) return NULL;
	return (rvn_process_t*)rvn_pool_get(&g_rvn.processes, id);
}

static void rvn_process_retain(rvn_process_t* process) {
	rvn_atomic_add(&process->refs, 1);
}

static void rvn_process_release(rvn_process_t* process) {
	if (rvn_atomic_add(&process->refs, -1) != 0) return;
	/* The destroy path released the pool's reference only after joining the
	 * thread, so nothing is left to wait for here. The guard is for safety. */
	if (process->thread_started && !process->thread_joined) {
		process->thread_joined = 1;
		rvn_thread_join(&process->thread);
	}
	rvn_mutex_destroy(&process->spawn_mutex);
	rvn_free(process);
}

static void rvn_process_task_run(void* arg) {
	rvn_process_task_t* task = (rvn_process_task_t*)arg;
	rvn_process_t* process = task->process;
	if (!process->destroyed) {
		if (task->is_exit) {
			if (process->exited) process->exited(process->exit_code, process->failed, process->user_data);
		} else if (process->output) {
			process->output(task->text, task->length, task->is_stderr, process->user_data);
		}
	}
	rvn_free(task->text);
	rvn_process_release(process);
	rvn_free(task);
}

static void rvn_process_post(rvn_process_t* process, rvn_process_task_t* task) {
	task->process = process;
	rvn_process_retain(process);
	Rtask_post(rvn_process_task_run, task);
}

static void rvn_process_post_output(rvn_process_t* process, const char* data, size_t length, int is_stderr) {
	rvn_process_task_t* task;
	if (!process->output || length == 0) return;
	task = (rvn_process_task_t*)rvn_calloc(sizeof(rvn_process_task_t));
	task->text = (char*)rvn_alloc(length + 1);
	memcpy(task->text, data, length);
	task->text[length] = '\0';
	task->length = length;
	task->is_stderr = is_stderr;
	rvn_process_post(process, task);
}

static void rvn_process_post_exit(rvn_process_t* process) {
	rvn_process_task_t* task = (rvn_process_task_t*)rvn_calloc(sizeof(rvn_process_task_t));
	task->is_exit = 1;
	rvn_process_post(process, task);
}

/** Waits for the watching thread, once. The caller holds a reference, so the structure stays. */
static void rvn_process_join(rvn_process_t* process) {
	if (!process->thread_started || process->thread_joined) return;
	process->thread_joined = 1;
	rvn_thread_join(&process->thread);
}

/* ---------------------------------------------------------------------------
 *  Finding programs
 * ------------------------------------------------------------------------- */

static char g_process_found[RVN_PROCESS_PATH_MAX];

static int rvn_process_has_separator(const char* name) {
	const char* cursor;
	for (cursor = name; *cursor; cursor++) {
		if (rvn_path_is_separator(*cursor)) return R_TRUE;
	}
#if defined(_WIN32)
	if (name[0] && name[1] == ':') return R_TRUE;
#endif
	return R_FALSE;
}

/** The spellings a program may have in a folder (Windows adds .exe, .cmd...). */
static const char* const rvn_process_suffixes[] = {
#if defined(_WIN32)
	".exe", ".cmd", ".bat", ".com", ""
#else
	""
#endif
};

/** R_TRUE when a file can be run; `suffix` is added to `path` first when it is not empty. */
static int rvn_process_runnable(const char* path, const char* suffix) {
	size_t length = strlen(path), suffix_length = strlen(suffix);
	char* candidate;
	int ok;
	if (suffix_length && length > suffix_length && rvn_strnicmp(path + length - suffix_length, suffix, suffix_length) == 0) {
		suffix_length = 0;
	}
	candidate = (char*)rvn_alloc(length + suffix_length + 1);
	memcpy(candidate, path, length);
	memcpy(candidate + length, suffix, suffix_length + 1);
	ok = !rvn_path_is_dir(candidate) && rvn_path_exists(candidate);
#if !defined(_WIN32)
	if (ok) ok = access(candidate, X_OK) == 0;
#endif
	rvn_free(candidate);
	return ok;
}

/** Remembers the spelling of `path` that exists as the answer of Rprocess_find. */
static const char* rvn_process_remember(const char* path) {
	size_t i;
	for (i = 0; i < RVN_COUNT_OF(rvn_process_suffixes); i++) {
		const char* suffix = rvn_process_suffixes[i];
		size_t length = strlen(path), suffix_length = strlen(suffix);
		char* candidate;
		if (!rvn_process_runnable(path, suffix)) continue;
		if (suffix_length && length > suffix_length && rvn_strnicmp(path + length - suffix_length, suffix, suffix_length) == 0) {
			suffix_length = 0;
		}
		if (length + suffix_length + 1 >= sizeof(g_process_found)) continue;
		candidate = (char*)rvn_alloc(length + suffix_length + 1);
		memcpy(candidate, path, length);
		memcpy(candidate + length, suffix, suffix_length + 1);
		strcpy(g_process_found, candidate);
		rvn_free(candidate);
		return g_process_found;
	}
	return NULL;
}

/** Looks for `program` in one folder. Returns the answer, or NULL. */
static const char* rvn_process_look_in(const char* folder, const char* program) {
	char* path;
	const char* found;
	if (!folder || !folder[0]) return NULL;
	path = rvn_path_child(folder, program);
	found = rvn_process_remember(path);
	rvn_free(path);
	return found;
}

#if !defined(_WIN32)
/** /usr/local/texlive/<year>/bin/<architecture> */
static const char* rvn_process_look_in_texlive(const char* program) {
	static const char* const bases[] = { "/usr/local/texlive", "/opt/texlive" };
	const char* found = NULL;
	uint32_t b;
	for (b = 0; b < RVN_COUNT_OF(bases) && !found; b++) {
		rvn_dir_entry_t* years;
		uint32_t year_count, y;
		if (!rvn_path_is_dir(bases[b])) continue;
		if (!rvn_dir_read(bases[b], &years, &year_count)) continue;
		for (y = 0; y < year_count && !found; y++) {
			char* year_path;
			rvn_dir_entry_t* entries;
			uint32_t entry_count, e;
			if (!years[y].is_dir) continue;
			year_path = rvn_path_child(bases[b], years[y].name);
			if (rvn_dir_read(year_path, &entries, &entry_count)) {
				for (e = 0; e < entry_count && !found; e++) {
					char* bin_path;
					rvn_dir_entry_t* targets;
					uint32_t target_count, t;
					if (!entries[e].is_dir || strcmp(entries[e].name, "bin") != 0) continue;
					bin_path = rvn_path_child(year_path, "bin");
					if (rvn_dir_read(bin_path, &targets, &target_count)) {
						for (t = 0; t < target_count && !found; t++) {
							char* target_path;
							if (!targets[t].is_dir) continue;
							target_path = rvn_path_child(bin_path, targets[t].name);
							found = rvn_process_look_in(target_path, program);
							rvn_free(target_path);
						}
						rvn_dir_free(targets, target_count);
					}
					rvn_free(bin_path);
				}
				rvn_dir_free(entries, entry_count);
			}
			rvn_free(year_path);
		}
		rvn_dir_free(years, year_count);
	}
	return found;
}
#else
/** C:\texlive\<year>\bin\windows, and the folders of MiKTeX */
static const char* rvn_process_look_in_texlive(const char* program) {
	const char* found = NULL;
	rvn_dir_entry_t* years;
	uint32_t year_count, y;

	if (rvn_path_is_dir("C:\\texlive") && rvn_dir_read("C:\\texlive", &years, &year_count)) {
		for (y = 0; y < year_count && !found; y++) {
			char* year_path;
			char* bin_path;
			char* windows;
			if (!years[y].is_dir) continue;
			year_path = rvn_path_child("C:\\texlive", years[y].name);
			bin_path = rvn_path_child(year_path, "bin");
			windows = rvn_path_child(bin_path, "windows");
			found = rvn_process_look_in(windows, program);
			rvn_free(windows);
			rvn_free(bin_path);
			rvn_free(year_path);
		}
		rvn_dir_free(years, year_count);
	}
	if (!found) {
		char* local = rvn_env_get("LOCALAPPDATA");
		char* program_files = rvn_env_get("PROGRAMFILES");
		if (local) {
			char* programs = rvn_path_child(local, "Programs");
			char* miktex = rvn_path_child(programs, "MiKTeX");
			char* bin = rvn_path_child(miktex, "miktex\\bin");
			found = rvn_process_look_in(bin, program);
			rvn_free(bin);
			rvn_free(miktex);
			rvn_free(programs);
			rvn_free(local);
		}
		if (!found && program_files) {
			char* miktex = rvn_path_child(program_files, "MiKTeX");
			char* bin = rvn_path_child(miktex, "miktex\\bin");
			found = rvn_process_look_in(bin, program);
			rvn_free(bin);
			rvn_free(miktex);
		}
		rvn_free(program_files);
	}
	return found;
}
#endif

const char* Rprocess_find(const char* program) {
	char* path;
	const char* found = NULL;

	if (!program || !program[0]) return NULL;
	if (rvn_process_has_separator(program)) {
		char* absolute = rvn_path_absolute(program);
		if (absolute) found = rvn_process_remember(absolute);
		rvn_free(absolute);
		return found;
	}

	path = rvn_env_get("PATH");
	if (path) {
		const char* cursor = path;
		while (*cursor && !found) {
			const char* end = cursor;
			char* folder;
			while (*end && *end != RVN_PATH_LIST_SEPARATOR) end++;
			folder = rvn_strndup(cursor, (size_t)(end - cursor));
			found = rvn_process_look_in(folder, program);
			rvn_free(folder);
			cursor = *end ? end + 1 : end;
		}
		rvn_free(path);
	}
	if (found) return found;

#if !defined(_WIN32)
	found = rvn_process_look_in("/Library/TeX/texbin", program);
	if (!found) found = rvn_process_look_in("/opt/homebrew/bin", program);
	if (!found) found = rvn_process_look_in("/usr/local/bin", program);
	if (!found) found = rvn_process_look_in_texlive(program);
#else
	found = rvn_process_look_in_texlive(program);
#endif
	return found;
}

char* Rprocess_find_copy(const char* program, size_t* size) {
	const char* found = Rprocess_find(program);
	char* copy = found ? rvn_strdup(found) : NULL;
	if (size) *size = copy ? strlen(copy) : 0;
	return copy;
}

int Rprocess_is_available(const char* program) {
	return Rprocess_find(program) != NULL;
}

/* ---------------------------------------------------------------------------
 *  Spawning: POSIX
 *
 *  fork + pipe + execvp, and a status pipe marked close-on-exec: it closes by
 *  itself when the program really runs, so the parent knows at once whether
 *  exec failed (a program that is not there) instead of learning it from the
 *  exit code of a failed child.
 * ------------------------------------------------------------------------- */

#if !defined(_WIN32)

static char** rvn_process_argv(const char* program, const char* const* arguments, int given) {
	int count = given > 0 ? given : 0, i;
	char** argv;
	if (!given) while (arguments && arguments[count]) count++;
	argv = (char**)rvn_alloc(sizeof(char*) * (size_t)(count + 2));
	argv[0] = rvn_strdup(program);
	for (i = 0; i < count; i++) argv[i + 1] = rvn_strdup(arguments[i]);
	argv[count + 1] = NULL;
	return argv;
}

static void rvn_process_argv_free(char** argv) {
	int i;
	if (!argv) return;
	for (i = 0; argv[i]; i++) rvn_free(argv[i]);
	rvn_free(argv);
}

/** Returns R_FALSE when a process cannot be created at all; `spawn->failed` says the program could not be run. */
static int rvn_spawn(const RProcessDesc* desc, rvn_spawn_t* spawn) {
	int out_pipe[2] = { -1, -1 };
	int err_pipe[2] = { -1, -1 };
	int status_pipe[2] = { -1, -1 };
	char** argv;
	pid_t child;
	int report = 0;
	ssize_t got;

	memset(spawn, 0, sizeof(*spawn));
	spawn->out_fd = -1;
	spawn->err_fd = -1;
	if (pipe(out_pipe) != 0) return R_FALSE;
	if (!desc->merge_stderr && pipe(err_pipe) != 0) {
		close(out_pipe[0]);
		close(out_pipe[1]);
		return R_FALSE;
	}
	if (pipe(status_pipe) != 0) {
		close(out_pipe[0]);
		close(out_pipe[1]);
		if (err_pipe[1] >= 0) { close(err_pipe[0]); close(err_pipe[1]); }
		return R_FALSE;
	}
	fcntl(status_pipe[1], F_SETFD, FD_CLOEXEC);

	argv = rvn_process_argv(desc->program, desc->arguments, desc->argument_count);
	child = fork();
	if (child < 0) {
		close(out_pipe[0]); close(out_pipe[1]);
		if (err_pipe[1] >= 0) { close(err_pipe[0]); close(err_pipe[1]); }
		close(status_pipe[0]); close(status_pipe[1]);
		rvn_process_argv_free(argv);
		return R_FALSE;
	}
	if (child == 0) {
		/* Only async-signal-safe calls from here on: no allocator, no error reporting. */
		int problem = 0;
		if (dup2(out_pipe[1], STDOUT_FILENO) < 0) problem = 1;
		/* with one stream the two are the same pipe: the log of a build keeps
		 * the order the tool wrote its lines in */
		if (desc->merge_stderr) {
			if (dup2(out_pipe[1], STDERR_FILENO) < 0) problem = 1;
		} else if (err_pipe[1] >= 0 && dup2(err_pipe[1], STDERR_FILENO) < 0) {
			problem = 1;
		}
		close(out_pipe[0]);
		close(out_pipe[1]);
		if (err_pipe[0] >= 0) close(err_pipe[0]);
		if (err_pipe[1] >= 0) close(err_pipe[1]);
		close(status_pipe[0]);
		if (!problem && desc->working_directory && chdir(desc->working_directory) != 0) problem = 2;
		if (!problem) {
			execvp(argv[0], argv);
			problem = errno ? errno : 3;
		}
		if (write(status_pipe[1], &problem, sizeof(problem)) < 0) { /* the parent sees a closed pipe */ }
		_exit(127);
	}

	close(out_pipe[1]);
	if (err_pipe[1] >= 0) close(err_pipe[1]);
	close(status_pipe[1]);
	rvn_process_argv_free(argv);

	spawn->pid = child;
	spawn->out_fd = out_pipe[0];
	spawn->err_fd = err_pipe[0];

	/* Blocks until the program runs (the pipe closes on exec) or fails. */
	got = read(status_pipe[0], &report, sizeof(report));
	close(status_pipe[0]);
	if (got > 0) {
		int status = 0;
		spawn->failed = R_TRUE;
		spawn->pid = 0;
		close(spawn->out_fd);
		if (spawn->err_fd >= 0) close(spawn->err_fd);
		spawn->out_fd = -1;
		spawn->err_fd = -1;
		while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
	}
	return R_TRUE;
}

static void rvn_spawn_close(rvn_spawn_t* spawn) {
	if (spawn->out_fd >= 0) close(spawn->out_fd);
	if (spawn->err_fd >= 0) close(spawn->err_fd);
	spawn->out_fd = -1;
	spawn->err_fd = -1;
}

/** Asks the child to stop. The mutex keeps the pid from having been reaped and reused meanwhile. */
static void rvn_process_signal(rvn_process_t* process, int signal_number) {
	rvn_mutex_lock(&process->spawn_mutex);
	if (process->pid > 0) kill(process->pid, signal_number);
	rvn_mutex_unlock(&process->spawn_mutex);
}

static void rvn_process_terminate(rvn_process_t* process) {
	rvn_process_signal(process, SIGTERM);
}

/** WNOHANG reap. `*done` is set when the child is gone; `*status` holds what waitpid gave. */
static void rvn_process_reap(rvn_process_t* process, int* status, int* done) {
	rvn_mutex_lock(&process->spawn_mutex);
	if (process->pid <= 0) {
		*done = 1;
	} else if (waitpid(process->pid, status, WNOHANG) == process->pid) {
		process->pid = 0;
		*done = 1;
	}
	rvn_mutex_unlock(&process->spawn_mutex);
}

/* Reads both pipes until they close and the child is reaped. */
static void rvn_pump(rvn_process_t* process, rvn_process_sink_t sink, void* context, rvn_pump_result_t* result) {
	char buffer[8192];
	int status = 0;
	int child_done = 0;
	int out_fd, err_fd;

	rvn_mutex_lock(&process->spawn_mutex);
	out_fd = process->out_fd;
	err_fd = process->err_fd;
	process->out_fd = -1;
	process->err_fd = -1;
	rvn_mutex_unlock(&process->spawn_mutex);

	result->exit_code = -1;
	result->failed = process->failed;
	for (;;) {
		struct pollfd fds[2];
		int is_error[2];
		int count = 0, ready, i;

		if (out_fd >= 0) {
			fds[count].fd = out_fd;
			fds[count].events = POLLIN;
			fds[count].revents = 0;
			is_error[count++] = 0;
		}
		if (err_fd >= 0) {
			fds[count].fd = err_fd;
			fds[count].events = POLLIN;
			fds[count].revents = 0;
			is_error[count++] = 1;
		}
		if (count == 0) {
			rvn_process_reap(process, &status, &child_done);
			if (child_done) break;
			rvn_sleep_ms(20);
			continue;
		}

		/* Once the child is gone, drain what is already in the pipes and stop:
		 * a grandchild of it may keep the pipe open for ever, and a build must
		 * not hang on that. */
		ready = poll(fds, (nfds_t)count, child_done ? 0 : 100);
		if (ready < 0 && errno != EINTR) break;
		if (child_done && ready <= 0) break;
		for (i = 0; ready > 0 && i < count; i++) {
			ssize_t n;
			if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
			n = read(fds[i].fd, buffer, sizeof(buffer));
			if (n > 0) {
				sink(context, buffer, (size_t)n, is_error[i]);
			} else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) {
				close(fds[i].fd);
				if (is_error[i]) err_fd = -1;
				else out_fd = -1;
			}
		}
		if (!child_done) rvn_process_reap(process, &status, &child_done);
	}
	if (out_fd >= 0) close(out_fd);
	if (err_fd >= 0) close(err_fd);
	if (!child_done) {
		/* the loop was cut short: collect the child all the same */
		status = 0;
		rvn_mutex_lock(&process->spawn_mutex);
		if (process->pid > 0) {
			while (waitpid(process->pid, &status, 0) < 0 && errno == EINTR) {}
			process->pid = 0;
		}
		rvn_mutex_unlock(&process->spawn_mutex);
	}
	if (WIFEXITED(status)) result->exit_code = WEXITSTATUS(status);
	else result->exit_code = -1;
}

/* ---------------------------------------------------------------------------
 *  Spawning: Win32
 *
 *  CreateProcessW with a command line quoted the way the C runtime of Visual
 *  C++ reads it back: backslashes are doubled before a quote, and an argument
 *  with a space or a quote in it is wrapped in quotes. The child gets NUL as
 *  its input, the pipes as its output, and no console window.
 * ------------------------------------------------------------------------- */

#else

static WCHAR* rvn_process_wide(const char* text) {
	int length;
	WCHAR* wide;
	if (!text) return NULL;
	length = MultiByteToWideChar(CP_UTF8, 0, text, -1, NULL, 0);
	if (length <= 0) return NULL;
	wide = (WCHAR*)rvn_alloc(sizeof(WCHAR) * (size_t)length);
	if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, length) <= 0) {
		rvn_free(wide);
		return NULL;
	}
	return wide;
}

typedef struct rvn_wide_text_t {
	WCHAR* data;
	size_t length;
	size_t cap;
} rvn_wide_text_t;

static void rvn_wide_push(rvn_wide_text_t* text, WCHAR character) {
	if (text->length + 2 > text->cap) {
		size_t cap = text->cap ? text->cap * 2 : 256;
		if (cap < text->length + 2) cap = text->length + 2;
		text->data = (WCHAR*)rvn_realloc(text->data, sizeof(WCHAR) * cap);
		text->cap = cap;
	}
	text->data[text->length++] = character;
}

static void rvn_process_append_argument(rvn_wide_text_t* line, const WCHAR* argument) {
	size_t index = 0;
	int needs_quotes = argument[0] == 0;
	for (index = 0; argument[index]; index++) {
		if (argument[index] == L' ' || argument[index] == L'\t' || argument[index] == L'"') needs_quotes = 1;
	}
	if (line->length > 0) rvn_wide_push(line, L' ');
	if (!needs_quotes) {
		for (index = 0; argument[index]; index++) rvn_wide_push(line, argument[index]);
		return;
	}
	rvn_wide_push(line, L'"');
	index = 0;
	while (argument[index]) {
		size_t backslashes = 0, k;
		while (argument[index] == L'\\') { index++; backslashes++; }
		if (argument[index] == L'\0') {
			/* a run of backslashes at the end must not escape the closing quote */
			for (k = 0; k < backslashes * 2; k++) rvn_wide_push(line, L'\\');
			break;
		} else if (argument[index] == L'"') {
			for (k = 0; k < backslashes * 2 + 1; k++) rvn_wide_push(line, L'\\');
			rvn_wide_push(line, L'"');
		} else {
			for (k = 0; k < backslashes; k++) rvn_wide_push(line, L'\\');
			rvn_wide_push(line, argument[index]);
		}
		index++;
	}
	rvn_wide_push(line, L'"');
}

static WCHAR* rvn_process_command_line(const RProcessDesc* desc) {
	rvn_wide_text_t line;
	WCHAR* wide;
	size_t i;
	memset(&line, 0, sizeof(line));
	wide = rvn_process_wide(desc->program);
	if (!wide) return NULL;
	rvn_process_append_argument(&line, wide);
	rvn_free(wide);
	for (i = 0; desc->arguments && (desc->argument_count > 0 ? i < (size_t)desc->argument_count : desc->arguments[i]); i++) {
		wide = rvn_process_wide(desc->arguments[i]);
		if (!wide) continue;
		rvn_process_append_argument(&line, wide);
		rvn_free(wide);
	}
	rvn_wide_push(&line, L'\0');
	return line.data;
}

static int rvn_spawn(const RProcessDesc* desc, rvn_spawn_t* spawn) {
	SECURITY_ATTRIBUTES security;
	WCHAR* line = NULL;
	WCHAR* folder = NULL;
	STARTUPINFOW startup;
	PROCESS_INFORMATION info;
	HANDLE null_input = INVALID_HANDLE_VALUE;
	HANDLE out_read = NULL, out_write = NULL;
	HANDLE err_read = NULL, err_write = NULL;
	int ok = R_FALSE;

	memset(spawn, 0, sizeof(*spawn));
	memset(&security, 0, sizeof(security));
	security.nLength = sizeof(security);
	security.bInheritHandle = TRUE;

	if (!CreatePipe(&out_read, &out_write, &security, 0)) return R_FALSE;
	SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
	if (!desc->merge_stderr) {
		if (!CreatePipe(&err_read, &err_write, &security, 0)) {
			CloseHandle(out_read);
			CloseHandle(out_write);
			return R_FALSE;
		}
		SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);
	}
	null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
		OPEN_EXISTING, 0, NULL);

	line = rvn_process_command_line(desc);
	if (!line) goto done;
	if (desc->working_directory) folder = rvn_process_wide(desc->working_directory);

	memset(&startup, 0, sizeof(startup));
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = null_input != INVALID_HANDLE_VALUE ? null_input : GetStdHandle(STD_INPUT_HANDLE);
	startup.hStdOutput = out_write;
	startup.hStdError = desc->merge_stderr ? out_write : err_write;

	memset(&info, 0, sizeof(info));
	if (!CreateProcessW(NULL, line, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, folder, &startup, &info)) {
		/* a program that is not there is an outcome, not a failure of the library */
		spawn->failed = R_TRUE;
		ok = R_TRUE;
		goto done;
	}
	CloseHandle(info.hThread);
	spawn->process = info.hProcess;
	spawn->out_read = out_read;
	spawn->err_read = err_read;
	out_read = NULL;
	err_read = NULL;
	ok = R_TRUE;

done:
	if (out_write) CloseHandle(out_write);
	if (err_write) CloseHandle(err_write);
	if (out_read) CloseHandle(out_read);
	if (err_read) CloseHandle(err_read);
	if (null_input != INVALID_HANDLE_VALUE) CloseHandle(null_input);
	rvn_free(line);
	rvn_free(folder);
	return ok;
}

static void rvn_spawn_close(rvn_spawn_t* spawn) {
	if (spawn->process) { CloseHandle(spawn->process); spawn->process = NULL; }
	if (spawn->out_read) { CloseHandle(spawn->out_read); spawn->out_read = NULL; }
	if (spawn->err_read) { CloseHandle(spawn->err_read); spawn->err_read = NULL; }
}

static void rvn_process_terminate(rvn_process_t* process) {
	rvn_mutex_lock(&process->spawn_mutex);
	if (process->process) TerminateProcess(process->process, 1);
	rvn_mutex_unlock(&process->spawn_mutex);
}

/** Reads what one pipe holds right now. */
static void rvn_process_read_ready(HANDLE pipe, rvn_process_sink_t sink, void* context, int is_stderr) {
	char buffer[8192];
	DWORD available = 0, taken = 0;
	if (!pipe) return;
	if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL)) return;
	while (available > 0) {
		DWORD want = available > (DWORD)sizeof(buffer) ? (DWORD)sizeof(buffer) : available;
		if (!ReadFile(pipe, buffer, want, &taken, NULL) || taken == 0) break;
		sink(context, buffer, (size_t)taken, is_stderr);
		if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL)) break;
	}
}

static void rvn_pump(rvn_process_t* process, rvn_process_sink_t sink, void* context, rvn_pump_result_t* result) {
	char buffer[8192];
	DWORD available = 0, taken = 0;
	DWORD exit_code = 0;
	HANDLE child, out_read, err_read;

	/* The handles stay in the structure while the pump runs: the UI thread
	 * may terminate the process meanwhile, and needs them. The mutex is what
	 * keeps the two from closing a handle under the other. */
	rvn_mutex_lock(&process->spawn_mutex);
	child = process->process;
	out_read = process->out_read;
	err_read = process->err_read;
	rvn_mutex_unlock(&process->spawn_mutex);

	result->exit_code = -1;
	result->failed = process->failed;
	if (!child) return;

	for (;;) {
		int drained = 1;
		HANDLE pipes[2];
		int i;
		rvn_process_read_ready(out_read, sink, context, 0);
		rvn_process_read_ready(err_read, sink, context, 1);
		if (WaitForSingleObject(child, 5) != WAIT_OBJECT_0) continue;
		/* what the program wrote just before it ended may still be in the pipes */
		pipes[0] = out_read;
		pipes[1] = err_read;
		for (i = 0; i < 2; i++) {
			if (!pipes[i]) continue;
			while (PeekNamedPipe(pipes[i], NULL, 0, NULL, &available, NULL) && available > 0) {
				DWORD want = available > (DWORD)sizeof(buffer) ? (DWORD)sizeof(buffer) : available;
				if (!ReadFile(pipes[i], buffer, want, &taken, NULL) || taken == 0) break;
				sink(context, buffer, (size_t)taken, i == 1);
				drained = 0;
			}
		}
		if (drained) break;
	}
	if (GetExitCodeProcess(child, &exit_code)) {
		result->exit_code = exit_code == STILL_ACTIVE ? -1 : (int)exit_code;
	}
	rvn_mutex_lock(&process->spawn_mutex);
	if (process->out_read) CloseHandle(process->out_read);
	if (process->err_read) CloseHandle(process->err_read);
	if (process->process) CloseHandle(process->process);
	process->out_read = NULL;
	process->err_read = NULL;
	process->process = NULL;
	rvn_mutex_unlock(&process->spawn_mutex);
}

#endif

/* ---------------------------------------------------------------------------
 *  Watching a process
 * ------------------------------------------------------------------------- */

static void rvn_process_sink_post(void* context, const char* data, size_t length, int is_stderr) {
	rvn_process_post_output((rvn_process_t*)context, data, length, is_stderr);
}

static void rvn_process_watch(void* arg) {
	rvn_process_t* process = (rvn_process_t*)arg;
	rvn_pump_result_t result;

	rvn_pump(process, rvn_process_sink_post, process, &result);

	process->exit_code = result.exit_code;
	process->failed = result.failed;
	rvn_atomic_store(&process->running, 0);
	rvn_process_post_exit(process);
	rvn_process_release(process);
}

Rid Rprocess_start(const RProcessDesc* desc, RProcessOutputCallback output, RProcessExitCallback exited, void* user_data) {
	rvn_process_t* process;
	rvn_spawn_t spawn;
	Rid id;

	RVN_API_GUARD(R_INVALID_ID);
	if (!desc || !desc->program || !desc->program[0]) {
		rvn_error(R_ERROR_INVALID_VALUE, R_SEVERITY_ERROR, "Rprocess_start: no program to run");
		return R_INVALID_ID;
	}
	memset(&spawn, 0, sizeof(spawn));
	if (!rvn_spawn(desc, &spawn)) {
		rvn_error(R_ERROR_PLATFORM, R_SEVERITY_ERROR, "Rprocess_start: cannot create a process");
		return R_INVALID_ID;
	}

	process = (rvn_process_t*)rvn_calloc(sizeof(rvn_process_t));
	process->output = output;
	process->exited = exited;
	process->user_data = user_data;
	process->exit_code = -1;
	process->failed = spawn.failed;
	rvn_atomic_store(&process->refs, 1);
	rvn_atomic_store(&process->running, 1);
	rvn_mutex_init(&process->spawn_mutex);
	/* the spawn belongs to the process from here on */
#if defined(_WIN32)
	process->process = spawn.process;
	process->out_read = spawn.out_read;
	process->err_read = spawn.err_read;
	spawn.process = NULL;
	spawn.out_read = NULL;
	spawn.err_read = NULL;
#else
	process->pid = spawn.pid;
	process->out_fd = spawn.out_fd;
	process->err_fd = spawn.err_fd;
	spawn.pid = 0;
	spawn.out_fd = -1;
	spawn.err_fd = -1;
#endif

	id = rvn_pool_add(&g_rvn.processes, process);
	if (id == R_INVALID_ID) {
		rvn_process_terminate(process);
		rvn_spawn_close(&spawn);
		rvn_mutex_destroy(&process->spawn_mutex);
		rvn_free(process);
		return R_INVALID_ID;
	}
	process->id = id;

	if (spawn.failed) {
		/* nothing runs: the failure is reported like any other end, from a frame */
		rvn_atomic_store(&process->running, 0);
		rvn_process_post_exit(process);
		return id;
	}

	/* the watching thread owns a reference of its own */
	rvn_process_retain(process);
	if (!rvn_thread_create(&process->thread, rvn_process_watch, process, "raven-process")) {
		/* the extra reference goes first: the destroy path releases the pool's */
		rvn_process_release(process);
		rvn_process_destroy_id(id);
		rvn_error(R_ERROR_PLATFORM, R_SEVERITY_ERROR, "Rprocess_start: cannot start the watching thread");
		return R_INVALID_ID;
	}
	process->thread_started = R_TRUE;
	return id;
}

int Rprocess_is_running(Rid process_id) {
	rvn_process_t* process = rvn_process_get(process_id);
	if (!process) return R_FALSE;
	return rvn_atomic_load(&process->running) != 0;
}

int Rprocess_get_exit_code(Rid process_id) {
	rvn_process_t* process = rvn_process_get(process_id);
	if (!process || rvn_atomic_load(&process->running)) return -1;
	return process->exit_code;
}

RProcessState Rprocess_get_state(Rid process_id) {
	rvn_process_t* process = rvn_process_get(process_id);
	if (!process) return R_PROCESS_FAILED;
	if (rvn_atomic_load(&process->running)) return R_PROCESS_RUNNING;
	return process->failed ? R_PROCESS_FAILED : R_PROCESS_EXITED;
}

void Rprocess_cancel(Rid process_id) {
	rvn_process_t* process = rvn_process_get(process_id);
	int step;

	if (!process) return;
	if (rvn_atomic_load(&process->running)) {
#if defined(_WIN32)
		rvn_process_terminate(process);
#else
		rvn_process_signal(process, SIGTERM);
		/* a program that ignores SIGTERM is killed after a while */
		for (step = 0; step < RVN_PROCESS_TERM_STEPS && rvn_atomic_load(&process->running); step++) {
			rvn_sleep_ms(50);
		}
		if (rvn_atomic_load(&process->running)) rvn_process_signal(process, SIGKILL);
#endif
	}
	rvn_process_join(process);
}

void Rprocess_wait(Rid process_id) {
	rvn_process_t* process = rvn_process_get(process_id);
	if (!process) return;
	rvn_process_join(process);
}

static void rvn_process_destroy_id(Rid id) {
	rvn_process_t* process = rvn_process_get(id);
	if (!process) return;
	process->destroyed = R_TRUE;
	Rprocess_cancel(id);
	rvn_pool_remove(&g_rvn.processes, id);
	rvn_process_release(process);
}

void rvn_process_destroy(Rid process) {
	rvn_process_destroy_id(process);
}

void Rprocess_destroy(Rid process) {
	rvn_process_destroy_id(process);
}

void rvn_process_shutdown(void) {
	uint32_t index;
	if (!g_rvn.processes.items) return;
	for (index = 0; index < g_rvn.processes.used; index++) {
		rvn_process_t* process = (rvn_process_t*)rvn_pool_at(&g_rvn.processes, index);
		if (!process) continue;
		process->destroyed = R_TRUE;
		if (rvn_atomic_load(&process->running)) {
#if defined(_WIN32)
			rvn_process_terminate(process);
#else
			rvn_process_signal(process, SIGKILL);
#endif
		}
		rvn_process_join(process);
	}
	/* The output and exit tasks that the threads posted are dropped with the
	 * processes: they find them destroyed and call nothing of the
	 * application. Running them here would be safer still, but a task of the
	 * application that is queued in front of them may want a widget that the
	 * shutdown already deleted. */
	for (index = 0; index < g_rvn.processes.used; index++) {
		rvn_process_t* process = (rvn_process_t*)rvn_pool_at(&g_rvn.processes, index);
		if (!process) continue;
		rvn_pool_remove(&g_rvn.processes, process->id);
		rvn_process_release(process);
	}
	rvn_pool_free(&g_rvn.processes);
}

/* ---------------------------------------------------------------------------
 *  Running to the end
 * ------------------------------------------------------------------------- */

typedef struct rvn_process_buffer_t {
	char* data;
	size_t length;
	size_t cap;
	size_t max;
	int truncated;
} rvn_process_buffer_t;

static void rvn_process_buffer_append(rvn_process_buffer_t* buffer, const char* data, size_t length) {
	size_t room;
	if (buffer->truncated) return;
	room = buffer->max > buffer->length ? buffer->max - buffer->length : 0;
	if (length > room) {
		length = room;
		buffer->truncated = R_TRUE;
	}
	if (buffer->length + length + 1 > buffer->cap) {
		size_t cap = buffer->cap ? buffer->cap : 4096;
		while (cap < buffer->length + length + 1) cap *= 2;
		buffer->data = (char*)rvn_realloc(buffer->data, cap);
		buffer->cap = cap;
	}
	if (length) memcpy(buffer->data + buffer->length, data, length);
	buffer->length += length;
	buffer->data[buffer->length] = '\0';
}

static void rvn_process_sink_buffer(void* context, const char* data, size_t length, int is_stderr) {
	RVN_UNUSED(is_stderr);
	rvn_process_buffer_append((rvn_process_buffer_t*)context, data, length);
}

/** Drops a character cut in two: the output of a program is UTF-8, and half a glyph is worse than none. */
static void rvn_process_buffer_trim_utf8(rvn_process_buffer_t* buffer) {
	size_t lead = buffer->length;
	if (!buffer->data || buffer->length == 0) return;
	while (lead > 0 && ((unsigned char)buffer->data[lead - 1] & 0xC0u) == 0x80u) lead--;
	if (lead == 0) {
		buffer->length = 0;
	} else {
		unsigned char first = (unsigned char)buffer->data[lead - 1];
		size_t expected = first < 0x80u ? 1 : first < 0xE0u ? 2 : first < 0xF0u ? 3 : 4;
		if (lead - 1 + expected > buffer->length) buffer->length = lead - 1;
	}
	buffer->data[buffer->length] = '\0';
}

char* Rprocess_run(const RProcessDesc* desc, size_t* size, size_t max_bytes) {
	rvn_process_buffer_t buffer;
	rvn_spawn_t spawn;
	rvn_process_t process;
	rvn_pump_result_t result;
	char* text = NULL;

	if (size) *size = 0;
	if (!desc || !desc->program || !desc->program[0]) return NULL;

	memset(&buffer, 0, sizeof(buffer));
	buffer.max = max_bytes ? max_bytes : RVN_PROCESS_RUN_MAX;
	memset(&spawn, 0, sizeof(spawn));
	if (!rvn_spawn(desc, &spawn)) return NULL;
	if (spawn.failed) {
		rvn_spawn_close(&spawn);
		return NULL;
	}

	/* A process of its own, not in the pool: nothing posted, nothing to watch. */
	memset(&process, 0, sizeof(process));
	process.failed = R_FALSE;
	rvn_mutex_init(&process.spawn_mutex);
#if defined(_WIN32)
	process.process = spawn.process;
	process.out_read = spawn.out_read;
	process.err_read = spawn.err_read;
#else
	process.pid = spawn.pid;
	process.out_fd = spawn.out_fd;
	process.err_fd = spawn.err_fd;
#endif
	rvn_pump(&process, rvn_process_sink_buffer, &buffer, &result);
	rvn_mutex_destroy(&process.spawn_mutex);
	if (buffer.truncated) rvn_process_buffer_trim_utf8(&buffer);

	text = buffer.data ? buffer.data : rvn_strdup("");
	buffer.data = NULL;
	if (size) *size = buffer.length;
	rvn_free(buffer.data);
	/* (a NULL text when it could not be run, with nothing to read) */
	if (result.failed) {
		rvn_free(text);
		return NULL;
	}
	return text;
}
