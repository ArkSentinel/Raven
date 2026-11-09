/**
 * Raven - core/latex_build.c
 * A LaTeX build: latexmk or an engine, run in the background, then its log
 * read as the diagnostics of the editor.
 *
 * The build owns a process of core/process.c, which streams what the tool
 * prints to the UI thread. When the process ends the log is parsed and the
 * messages are kept until the build runs again or is destroyed. Nothing here
 * blocks: the UI only ever waits for a process when the build is cancelled
 * (or when the whole library shuts down).
 */

#include "base.h"
#include "context.h"
#include "fs.h"
#include "handle.h"

/** Longer than any log an editor shows: the rest of a runaway log is cut. */
#define RVN_BUILD_LOG_MAX (32u * 1024u * 1024u)

typedef struct rvn_build_t {
	Rid id;
	/** The tool that writes the log, and the files it works on. */
	char* root_file;
	char* folder;
	char* base_name;
	char* log_path;
	char* pdf_path;
	/** The .pdf that "Output written on ..." named, when it is not the expected one. */
	char* output_pdf;

	Rid process;
	RProcessOutputCallback output;
	void* output_user;
	RLatexBuildFinishedCallback finished;
	void* finished_user;

	int succeeded;
	int destroyed;

	/** The list of diagnostics of the last build (a handle, raven_latex.h). */
	Rid log;
} rvn_build_t;

static rvn_build_t* rvn_build_get(Rid id) {
	if (!g_rvn.initialized || id == R_INVALID_ID) return NULL;
	return (rvn_build_t*)rvn_pool_get(&g_rvn.builds, id);
}

/* ---------------------------------------------------------------------------
 *  The command line
 * ------------------------------------------------------------------------- */

static const char* rvn_build_engine_program(RLatexEngine engine) {
	switch (engine) {
		case R_LATEX_XELATEX: return "xelatex";
		case R_LATEX_LUALATEX: return "lualatex";
		case R_LATEX_PDFLATEX:
		default: return "pdflatex";
	}
}

/** The name of the file without its folder and without ".tex". */
static char* rvn_build_base_name(const char* root_file) {
	const char* name = rvn_path_name(root_file);
	size_t length = strlen(name);
	if (length > 4 && rvn_strnicmp(name + length - 4, ".tex", 4) == 0) length -= 4;
	return rvn_strndup(name, length);
}

/* ---------------------------------------------------------------------------
 *  Freeing
 * ------------------------------------------------------------------------- */

static void rvn_build_clear_messages(rvn_build_t* build) {
	if (build->log != R_INVALID_ID) Rlatex_log_destroy(build->log);
	build->log = R_INVALID_ID;
}

static void rvn_build_free(rvn_build_t* build) {
	if (!build) return;
	rvn_build_clear_messages(build);
	rvn_free(build->root_file);
	rvn_free(build->folder);
	rvn_free(build->base_name);
	rvn_free(build->log_path);
	rvn_free(build->pdf_path);
	rvn_free(build->output_pdf);
	rvn_free(build);
}

/**
 * Reads the "Output written on <file>.pdf (3 pages, 12345 bytes)." line of a
 * log: an engine told to write somewhere else says where. Relative names are
 * taken from the folder of the build.
 */
static char* rvn_build_output_pdf(const char* log, size_t size, const char* folder) {
	const char* end = log + size;
	const char* cursor = log;
	while (cursor < end) {
		const char* line_end = cursor;
		while (line_end < end && *line_end != '\n') line_end++;
		if (line_end - cursor > 20 && memcmp(cursor, "Output written on ", 18) == 0) {
			const char* name = cursor + 18;
			const char* stop = name;
			while (stop < line_end && *stop != ' ' && *stop != '\r') stop++;
			if (stop - name > 4 && rvn_strnicmp(stop - 4, ".pdf", 4) == 0) {
				char* text = rvn_strndup(name, (size_t)(stop - name));
				char* path = text[0] == '/' ? rvn_strdup(text) : rvn_path_join(folder, text);
				rvn_free(text);
				return path;
			}
		}
		cursor = line_end < end ? line_end + 1 : end;
	}
	return NULL;
}

/* ---------------------------------------------------------------------------
 *  The end of a build
 * ------------------------------------------------------------------------- */

static void rvn_build_finish(rvn_build_t* build, int exit_code, int failed) {
	char* log;
	size_t size = 0;

	rvn_build_clear_messages(build);
	build->succeeded = R_FALSE;

	rvn_free(build->output_pdf);
	build->output_pdf = NULL;
	if (build->log_path) {
		log = rvn_read_file(build->log_path, &size);
		if (log) {
			if (size > RVN_BUILD_LOG_MAX) size = RVN_BUILD_LOG_MAX;
			build->log = Rlatex_log_parse(log, size);
			build->output_pdf = rvn_build_output_pdf(log, size, build->folder);
			rvn_free(log);
		}
	}
	/* a tool that stopped with 0 wrote the documents it is about */
	build->succeeded = !failed && exit_code == 0;
}

/** Runs on the UI thread, from a posted task of the process. */
static void rvn_build_on_exit(int exit_code, int failed, void* user_data) {
	rvn_build_t* build = (rvn_build_t*)user_data;
	if (build->destroyed) return;
	rvn_build_finish(build, exit_code, failed);
	if (build->finished) build->finished(build->id, build->succeeded, build->finished_user);
}

/** Runs on the UI thread: the output of the tool, forwarded to the application. */
static void rvn_build_on_output(const char* text, size_t length, int is_stderr, void* user_data) {
	rvn_build_t* build = (rvn_build_t*)user_data;
	if (build->destroyed || !build->output) return;
	build->output(text, length, is_stderr, build->output_user);
}

/* ---------------------------------------------------------------------------
 *  Starting
 * ------------------------------------------------------------------------- */

static char* rvn_build_join(const char* folder, const char* name, const char* extension) {
	char* path = rvn_path_join(folder, name);
	char* full;
	if (!extension) return path;
	full = (char*)rvn_alloc(strlen(path) + strlen(extension) + 1);
	strcpy(full, path);
	strcat(full, extension);
	rvn_free(path);
	return full;
}

Rid Rlatex_build_start(const RLatexBuildDesc* desc, RProcessOutputCallback output,
	RLatexBuildFinishedCallback finished, void* user_data) {
	rvn_build_t* build;
	RProcessDesc process_desc;
	Rid process = R_INVALID_ID;
	Rid id;
	char* arguments[16];
	int argument_count = 0;
	int i;

	RVN_API_GUARD(R_INVALID_ID);
	if (!desc || !desc->root_file || !desc->root_file[0]) {
		rvn_error(R_ERROR_INVALID_VALUE, R_SEVERITY_ERROR, "Rlatex_build_start: no root file to build");
		return R_INVALID_ID;
	}

	build = (rvn_build_t*)rvn_calloc(sizeof(rvn_build_t));
	build->root_file = rvn_strdup(desc->root_file);
	build->folder = rvn_path_dir(desc->root_file);
	if (!build->folder || !build->folder[0]) {
		rvn_free(build->folder);
		build->folder = rvn_strdup(".");
	}
	build->base_name = rvn_build_base_name(desc->root_file);
	build->output = output;
	build->output_user = user_data;
	build->finished = finished;
	build->finished_user = user_data;
	build->log_path = desc->log_path_override ? rvn_strdup(desc->log_path_override)
		: rvn_build_join(build->folder, build->base_name, ".log");
	build->pdf_path = rvn_build_join(build->folder, build->base_name, ".pdf");

	memset(&process_desc, 0, sizeof(process_desc));
	process_desc.working_directory = build->folder;
	process_desc.merge_stderr = R_TRUE;

	if (desc->command_override) {
		process_desc.program = desc->command_override;
		process_desc.arguments = desc->arguments_override;
		process_desc.argument_count = desc->arguments_override_count;
	} else if (desc->use_latexmk) {
		/* latexmk takes the flags of the engine after -pdf / -xelatex / -lualatex */
		static const char* const latexmk_flags[] = { "-synctex=1", "-interaction=nonstopmode", NULL };
		process_desc.program = "latexmk";
		arguments[argument_count++] = desc->engine == R_LATEX_XELATEX ? "-xelatex" :
			desc->engine == R_LATEX_LUALATEX ? "-lualatex" : "-pdf";
		for (i = 0; latexmk_flags[i]; i++) arguments[argument_count++] = (char*)latexmk_flags[i];
		if (desc->shell_escape) arguments[argument_count++] = "-shell-escape";
		arguments[argument_count++] = (char*)desc->root_file;
		arguments[argument_count] = NULL;
		process_desc.arguments = (const char* const*)arguments;
	} else {
		static const char* const engine_flags[] = { "-synctex=1", "-interaction=nonstopmode", NULL };
		process_desc.program = rvn_build_engine_program(desc->engine);
		for (i = 0; engine_flags[i]; i++) arguments[argument_count++] = (char*)engine_flags[i];
		if (desc->shell_escape) arguments[argument_count++] = "-shell-escape";
		arguments[argument_count++] = (char*)desc->root_file;
		arguments[argument_count] = NULL;
		process_desc.arguments = (const char* const*)arguments;
	}

	/* The build is in the pool before its process exists: the callbacks of
	 * the process find a valid handle, and a failure here has nothing to
	 * cancel. */
	build->process = R_INVALID_ID;
	build->log = R_INVALID_ID;
	id = rvn_pool_add(&g_rvn.builds, build);
	if (id == R_INVALID_ID) {
		rvn_build_free(build);
		return R_INVALID_ID;
	}
	build->id = id;

	process = Rprocess_start(&process_desc, rvn_build_on_output, rvn_build_on_exit, build);
	if (process == R_INVALID_ID) {
		rvn_pool_remove(&g_rvn.builds, id);
		rvn_build_free(build);
		rvn_error(R_ERROR_PLATFORM, R_SEVERITY_ERROR, "Rlatex_build_start: the tool cannot be run: %s",
			process_desc.program);
		return R_INVALID_ID;
	}
	build->process = process;
	return id;
}

/* ---------------------------------------------------------------------------
 *  The public functions
 * ------------------------------------------------------------------------- */

int Rlatex_build_is_running(Rid build_id) {
	rvn_build_t* build = rvn_build_get(build_id);
	if (!build) return R_FALSE;
	return build->process != R_INVALID_ID && Rprocess_is_running(build->process);
}

void Rlatex_build_cancel(Rid build_id) {
	rvn_build_t* build = rvn_build_get(build_id);
	if (!build || build->process == R_INVALID_ID) return;
	Rprocess_cancel(build->process);
	/* the exit callback brings the messages of what the tool wrote */
}

Rid Rlatex_build_get_messages(Rid build_id) {
	rvn_build_t* build = rvn_build_get(build_id);
	RVN_API_GUARD(R_INVALID_ID);
	return build ? build->log : R_INVALID_ID;
}

const char* Rlatex_build_get_pdf_path(Rid build_id) {
	rvn_build_t* build = rvn_build_get(build_id);
	if (!build) return NULL;
	/* what the engine said it wrote wins over what its name suggests */
	if (build->output_pdf && rvn_path_exists(build->output_pdf)) return build->output_pdf;
	if (build->pdf_path && rvn_path_exists(build->pdf_path)) return build->pdf_path;
	return NULL;
}

const char* Rlatex_build_get_log_path(Rid build_id) {
	rvn_build_t* build = rvn_build_get(build_id);
	return build ? build->log_path : NULL;
}

int Rlatex_build_succeeded(Rid build_id) {
	rvn_build_t* build = rvn_build_get(build_id);
	return build ? build->succeeded : R_FALSE;
}

void Rlatex_build_destroy(Rid build_id) {
	rvn_build_t* build = rvn_build_get(build_id);
	if (!build) return;
	build->destroyed = R_TRUE;
	if (build->process != R_INVALID_ID) {
		rvn_process_destroy(build->process);
		build->process = R_INVALID_ID;
	}
	rvn_pool_remove(&g_rvn.builds, build_id);
	rvn_build_free(build);
}

void rvn_builds_shutdown(void) {
	uint32_t index;
	if (!g_rvn.builds.items) return;
	for (index = 0; index < g_rvn.builds.used; index++) {
		rvn_build_t* build = (rvn_build_t*)rvn_pool_at(&g_rvn.builds, index);
		if (build) {
			build->destroyed = R_TRUE;
			if (build->process != R_INVALID_ID) {
				rvn_process_destroy(build->process);
				build->process = R_INVALID_ID;
			}
			rvn_build_free(build);
		}
	}
	rvn_pool_free(&g_rvn.builds);
}

const char* Rlatex_find_tool(const char* name) {
	return Rprocess_find(name);
}
