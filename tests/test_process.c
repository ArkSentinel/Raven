/**
 * Raven tests - running other programs (core/process.c) and reading LaTeX
 * logs (core/latex_log.c, core/latex_build.c).
 *
 * The programs run here are the ones every system has: /bin/sh on POSIX, the
 * command interpreter of Windows. Nothing needs a TeX installation: the log
 * parser works on text, and the build runs a shell that writes a log of its
 * own.
 */

#include "test.h"

#include <stdio.h>

/* ---------------------------------------------------------------------------
 *  Helpers
 * ------------------------------------------------------------------------- */

/**
 * A file name of this run: the tests run in the folder of the build, where
 * several of them may run at the same time, so the name carries the process
 * and the moment.
 */
static void test_file_name(char* buffer, size_t cap, const char* suffix) {
	static unsigned counter;
	unsigned serial = ++counter;
	snprintf(buffer, cap, "raven_process_%lu_%u_%s",
		(unsigned long)(uintptr_t)buffer, serial, suffix);
}

static void pump_frames_until(int* flag, double seconds) {
	double deadline = Rget_time() + seconds;
	while (!*flag && Rget_time() < deadline) {
		Rwait_events(0.01);
		Rdraw_frame();
	}
	/* the last tasks posted by the process are delivered here */
	{
		int i;
		for (i = 0; i < 5; i++) Rdraw_frame();
	}
}

typedef struct Capture {
	char text[8192];
	size_t length;
	int calls;
	int stderr_calls;
	int exit_calls;
	int exit_code;
	int failed;
} Capture;

static void on_capture(const char* text, size_t length, int is_stderr, void* user_data) {
	Capture* capture = (Capture*)user_data;
	size_t room = sizeof(capture->text) - capture->length - 1;
	if (length > room) length = room;
	if (length) memcpy(capture->text + capture->length, text, length);
	capture->length += length;
	capture->text[capture->length] = '\0';
	capture->calls++;
	if (is_stderr) capture->stderr_calls++;
}

static void on_capture_exit(int exit_code, int failed, void* user_data) {
	Capture* capture = (Capture*)user_data;
	capture->exit_calls++;
	capture->exit_code = exit_code;
	capture->failed = failed;
}

/* ---------------------------------------------------------------------------
 *  Finding a program
 * ------------------------------------------------------------------------- */

static void test_find(void) {
#if defined(_WIN32)
	CHECK(Rprocess_find("cmd") != NULL);
	CHECK(Rprocess_is_available("cmd"));
#else
	CHECK(Rprocess_find("sh") != NULL);
	CHECK(Rprocess_is_available("sh"));
	CHECK(Rprocess_find("/bin/sh") != NULL);
#endif
	CHECK(!Rprocess_is_available("raven-no-such-program-4f1c"));
	CHECK(Rprocess_find("raven-no-such-program-4f1c") == NULL);
	/* a tool of the trade is absent here: the call only has to answer */
	printf("  latexmk: %s\n", Rlatex_find_tool("latexmk") ? Rlatex_find_tool("latexmk") : "(not installed)");
}

/* ---------------------------------------------------------------------------
 *  Running to the end
 * ------------------------------------------------------------------------- */

static void test_run(void) {
	RProcessDesc desc;
	char* output = NULL;
	size_t length = 0;

	memset(&desc, 0, sizeof(desc));
#if defined(_WIN32)
	{
		static const char* const arguments[] = { "/c", "echo hello", NULL };
		desc.program = "cmd";
		desc.arguments = arguments;
	}
#else
	{
		static const char* const arguments[] = { "-c", "printf hello", NULL };
		desc.program = "/bin/sh";
		desc.arguments = arguments;
	}
#endif
	output = Rprocess_run(&desc, &length, 0);
	CHECK(output != NULL);
	if (output) {
		CHECK(strstr(output, "hello") != NULL);
		CHECK(length == strlen(output));
		Rmemory_free(output);
	}

	/* a program that is not there cannot be run */
	desc.program = "raven-no-such-program-4f1c";
	output = NULL;
	length = 0;
	output = Rprocess_run(&desc, &length, 0);
	CHECK(output == NULL);

#if !defined(_WIN32)
	/* the output is cut at max_bytes, cleanly (no half of a character) */
	{
		static const char* const arguments[] = { "-c", "printf '0123456789abcdef'", NULL };
		desc.program = "/bin/sh";
		desc.arguments = arguments;
		output = Rprocess_run(&desc, &length, 8);
		CHECK(output != NULL);
		CHECK(length == 8);
		CHECK_STR(output, "01234567");
		Rmemory_free(output);

		/* a UTF-8 character cut in two is dropped, not left broken */
		{
			static const char* const accents[] = { "-c", "printf '\\303\\251\\303\\251'", NULL };
			desc.arguments = accents;
			output = Rprocess_run(&desc, &length, 3);
			CHECK(output != NULL);
			CHECK(length == 2);
			Rmemory_free(output);
		}

		/* a program that prints a lot does not overflow the buffer */
		{
			static const char* const words[] = { "-c", "i=0; while [ $i -lt 20000 ]; do echo 0123456789; i=$((i+1)); done", NULL };
			desc.arguments = words;
			output = Rprocess_run(&desc, &length, 1024);
			CHECK(output != NULL);
			CHECK(length == 1024);
			CHECK(output && strlen(output) == 1024);
			Rmemory_free(output);
		}
	}
#endif
}

/* ---------------------------------------------------------------------------
 *  Running asynchronously
 * ------------------------------------------------------------------------- */

static void test_async(void) {
	Capture capture;
	RProcessDesc desc;
	Rid process;

	memset(&capture, 0, sizeof(capture));
	memset(&desc, 0, sizeof(desc));
	desc.merge_stderr = R_TRUE;
#if defined(_WIN32)
	{
		static const char* const arguments[] = { "/c", "echo one & echo two & exit 3", NULL };
		desc.program = "cmd";
		desc.arguments = arguments;
	}
#else
	{
		static const char* const arguments[] = { "-c", "echo one; echo two; exit 3", NULL };
		desc.program = "/bin/sh";
		desc.arguments = arguments;
	}
#endif
	process = Rprocess_start(&desc, on_capture, on_capture_exit, &capture);
	CHECK(process != R_INVALID_ID);
	pump_frames_until(&capture.exit_calls, 10.0);
	CHECK(capture.exit_calls == 1);
	CHECK(capture.exit_code == 3);
	CHECK(!capture.failed);
	CHECK(strstr(capture.text, "one") != NULL);
	CHECK(strstr(capture.text, "two") != NULL);
	CHECK(!Rprocess_is_running(process));
	CHECK(Rprocess_get_exit_code(process) == 3);
	CHECK(Rprocess_get_state(process) == R_PROCESS_EXITED);

	/* a program that is not there ends at once, with failed set */
	memset(&capture, 0, sizeof(capture));
	desc.program = "raven-no-such-program-4f1c";
	desc.arguments = NULL;
	process = Rprocess_start(&desc, on_capture, on_capture_exit, &capture);
	CHECK(process != R_INVALID_ID);
	pump_frames_until(&capture.exit_calls, 5.0);
	CHECK(capture.exit_calls == 1);
	CHECK(capture.failed);
	CHECK(capture.exit_code == -1);
	CHECK(Rprocess_get_state(process) == R_PROCESS_FAILED);
}

/* stderr is its own stream unless the description merges it */
static void test_streams(void) {
	Capture capture;
	RProcessDesc desc;
	Rid process;

	memset(&capture, 0, sizeof(capture));
	memset(&desc, 0, sizeof(desc));
#if defined(_WIN32)
	{
		static const char* const arguments[] = { "/c", "echo out & echo err 1>&2 & exit 0", NULL };
		desc.program = "cmd";
		desc.arguments = arguments;
	}
#else
	{
		static const char* const arguments[] = { "-c", "echo out; echo err 1>&2; exit 0", NULL };
		desc.program = "/bin/sh";
		desc.arguments = arguments;
	}
#endif
	desc.merge_stderr = R_FALSE;
	process = Rprocess_start(&desc, on_capture, on_capture_exit, &capture);
	CHECK(process != R_INVALID_ID);
	pump_frames_until(&capture.exit_calls, 10.0);
	CHECK(capture.exit_calls == 1);
	CHECK(capture.exit_code == 0);
	CHECK(capture.stderr_calls >= 1);
	CHECK(strstr(capture.text, "out") != NULL);
	CHECK(strstr(capture.text, "err") != NULL);

	/* merged, everything arrives as one stream */
	memset(&capture, 0, sizeof(capture));
	desc.merge_stderr = R_TRUE;
	process = Rprocess_start(&desc, on_capture, on_capture_exit, &capture);
	CHECK(process != R_INVALID_ID);
	pump_frames_until(&capture.exit_calls, 10.0);
	CHECK(capture.exit_calls == 1);
	CHECK(capture.stderr_calls == 0);
	CHECK(strstr(capture.text, "out") != NULL);
	CHECK(strstr(capture.text, "err") != NULL);
}

static void test_cancel(void) {
	Capture capture;
	RProcessDesc desc;
	Rid process;

	memset(&capture, 0, sizeof(capture));
	memset(&desc, 0, sizeof(desc));
#if defined(_WIN32)
	{
		static const char* const arguments[] = { "/c", "ping -n 30 127.0.0.1 > nul", NULL };
		desc.program = "cmd";
		desc.arguments = arguments;
	}
#else
	{
		static const char* const arguments[] = { "-c", "sleep 30", NULL };
		desc.program = "/bin/sh";
		desc.arguments = arguments;
	}
#endif
	process = Rprocess_start(&desc, on_capture, on_capture_exit, &capture);
	CHECK(process != R_INVALID_ID);
	CHECK(Rprocess_is_running(process));
	Rprocess_cancel(process);
	CHECK(!Rprocess_is_running(process));
	pump_frames_until(&capture.exit_calls, 5.0);
	CHECK(capture.exit_calls == 1);
}

/* ---------------------------------------------------------------------------
 *  The log parser
 * ------------------------------------------------------------------------- */

static const char* const SAMPLE_LOG =
	"This is pdfTeX, Version 3.141592653-2.6-1.40.25 (TeX Live 2024)\n"
	"entering extended mode\n"
	"(./main.tex\n"
	"LaTeX2e <2023-11-01> patch level 1\n"
	"(./chapter.tex\n"
	"! Undefined control sequence.\n"
	"l.42 \\foo\n"
	"         \n"
	"? \n"
	"LaTeX Warning: Reference `fig:one' on page 1 undefined on input line 7.\n"
	"\n"
	"Overfull \\hbox (12.3pt too wide) in paragraph at lines 10--12\n"
	"[]\\OT1/cmr/m/n/10 some text\n"
	")\n"
	")\n"
	" (see the transcript file for additional information)\n"
	"Here is how much of TeX's memory you used:\n"
	"  1234 strings out of 480000\n";

static void test_log(void) {
	RLogMessage message;
	Rid log = Rlatex_log_parse(SAMPLE_LOG, strlen(SAMPLE_LOG));
	int count = Rlatex_log_get_count(log);

	CHECK(count == 3);
	if (count == 3) {
		CHECK(Rlatex_log_get_message(log, 0, &message));
		CHECK(message.kind == R_LOG_ERROR);
		CHECK(message.line == 42);
		CHECK_STR(message.message, "Undefined control sequence.");
		CHECK(message.file != NULL && strstr(message.file, "chapter.tex") != NULL);
		CHECK(message.context != NULL);

		CHECK(Rlatex_log_get_message(log, 1, &message));
		CHECK(message.kind == R_LOG_WARNING);
		CHECK(message.line == 7);
		CHECK(message.message != NULL && strstr(message.message, "Reference") != NULL);
		CHECK(message.file != NULL && strstr(message.file, "chapter.tex") != NULL);

		CHECK(Rlatex_log_get_message(log, 2, &message));
		CHECK(message.kind == R_LOG_BAD_BOX);
		CHECK(message.line == 10);
		CHECK(message.message != NULL && strstr(message.message, "Overfull") != NULL);
		CHECK(!Rlatex_log_get_message(log, 3, &message));
	}
	Rlatex_log_destroy(log);

	/* the same, from a file */
	{
		char path[256];
		FILE* file;
		test_file_name(path, sizeof(path), "sample.log");
		file = fopen(path, "wb");
		CHECK(file != NULL);
		if (file) {
			fwrite(SAMPLE_LOG, 1, strlen(SAMPLE_LOG), file);
			fclose(file);
			log = Rlatex_log_parse_file(path);
			CHECK(Rlatex_log_get_count(log) == 3);
			Rlatex_log_destroy(log);
			remove(path);
		}
	}

	/* a log with nothing to report gives nothing, and an unreadable file too */
	log = Rlatex_log_parse("This is pdfTeX\n(./main.tex)\n", 29);
	CHECK(log == R_INVALID_ID);
	CHECK(Rlatex_log_get_count(log) == 0);
	log = Rlatex_log_parse_file("raven-no-such-log-4f1c.log");
	CHECK(log == R_INVALID_ID);
}

static void test_log_package_warning(void) {
	/* a warning of a package, wrapped over two lines, with its line number */
	static const char* const log =
		"(./main.tex\n"
		"Package hyperref Warning: Token not allowed in a PDF string (Unicode):\n"
		"(hyperref)                removing `math shift' on input line 12.\n"
		"Package foo Error: something went wrong on input line 3.\n"
		"No pages of output.\n";
	RLogMessage message;
	Rid list = Rlatex_log_parse(log, strlen(log));
	int count = Rlatex_log_get_count(list);

	CHECK(count == 3);
	if (count == 3) {
		CHECK(Rlatex_log_get_message(list, 0, &message));
		CHECK(message.kind == R_LOG_WARNING);
		CHECK(message.line == 12);
		CHECK(strstr(message.message, "Token not allowed") != NULL);
		CHECK(Rlatex_log_get_message(list, 1, &message));
		CHECK(message.kind == R_LOG_ERROR);
		CHECK(message.line == 3);
		CHECK(strstr(message.message, "something went wrong") != NULL);
		CHECK(Rlatex_log_get_message(list, 2, &message));
		CHECK(message.kind == R_LOG_ERROR);
		CHECK(strstr(message.message, "No pages of output") != NULL);
	}
	Rlatex_log_destroy(list);
}

/* ---------------------------------------------------------------------------
 *  A build
 * ------------------------------------------------------------------------- */

static int g_build_finished;
static int g_build_success;
static Rid g_build_id;

static void on_build_finished(Rid build, int success, void* user_data) {
	(void)user_data;
	g_build_finished++;
	g_build_success = success;
	g_build_id = build;
}

static void test_build(void) {
#if !defined(_WIN32)
	RLatexBuildDesc desc;
	RLogMessage message;

	char root_name[256];
	char log_name[256];
	char command[1024];
	const char* arguments[3];

	test_file_name(root_name, sizeof(root_name), "main.tex");
	test_file_name(log_name, sizeof(log_name), "build.log");
	snprintf(command, sizeof(command),
		"printf '! Undefined control sequence.\\nl.42 \\\\foo\\n' > %s; echo done", log_name);

	g_build_finished = 0;
	g_build_success = -1;
	memset(&desc, 0, sizeof(desc));
	desc.root_file = root_name;
	desc.engine = R_LATEX_PDFLATEX;
	desc.use_latexmk = R_TRUE;
	desc.command_override = "/bin/sh";
	arguments[0] = "-c";
	arguments[1] = command;
	arguments[2] = NULL;
	desc.arguments_override = arguments;
	desc.log_path_override = log_name;

	{
		Rid build = Rlatex_build_start(&desc, NULL, on_build_finished, NULL);
		CHECK(build != R_INVALID_ID);
		pump_frames_until(&g_build_finished, 10.0);
		CHECK(g_build_finished == 1);
		CHECK(g_build_id == build);
		CHECK(g_build_success == R_TRUE);
		CHECK(Rlatex_build_succeeded(build));
		CHECK(!Rlatex_build_is_running(build));
		CHECK_STR(Rlatex_build_get_log_path(build), log_name);

		{
			Rid log = Rlatex_build_get_messages(build);
			CHECK(Rlatex_log_get_count(log) == 1);
			if (Rlatex_log_get_count(log) == 1) {
				CHECK(Rlatex_log_get_message(log, 0, &message));
				CHECK(message.kind == R_LOG_ERROR);
				CHECK(message.line == 42);
				CHECK_STR(message.message, "Undefined control sequence.");
			}
		}
		/* no pdf was written by the shell */
		CHECK(Rlatex_build_get_pdf_path(build) == NULL);
		Rlatex_build_destroy(build);
		CHECK(!Rlatex_build_is_running(build));
		CHECK(Rlatex_build_get_pdf_path(build) == NULL);
	}

	/* a build destroyed while it runs stops its process */
	{
		Rid build;
		g_build_finished = 0;
		arguments[0] = "-c";
		arguments[1] = "sleep 30";
		arguments[2] = NULL;
		desc.log_path_override = NULL;
		build = Rlatex_build_start(&desc, NULL, on_build_finished, NULL);
		CHECK(build != R_INVALID_ID);
		CHECK(Rlatex_build_is_running(build));
		Rlatex_build_destroy(build);
	}
	remove(log_name);
#else
	printf("  (skipped: the build test uses a POSIX shell)\n");
#endif
}

/* ---------------------------------------------------------------------------
 *  Main
 * ------------------------------------------------------------------------- */

int main(void) {
	if (!test_init(2)) {
		printf("cannot initialize Raven\n");
		return 1;
	}
	RUN(test_find);
	RUN(test_run);
	RUN(test_async);
	RUN(test_streams);
	RUN(test_cancel);
	/* everything still running when the library stops must be freed by Rshutdown */
	{
		RProcessDesc desc;
		memset(&desc, 0, sizeof(desc));
#if defined(_WIN32)
		static const char* const arguments[] = { "/c", "ping -n 30 127.0.0.1 > nul", NULL };
		desc.program = "cmd";
#else
		static const char* const arguments[] = { "-c", "sleep 30", NULL };
		desc.program = "/bin/sh";
#endif
		desc.arguments = arguments;
		CHECK(Rprocess_start(&desc, NULL, NULL, NULL) != R_INVALID_ID);
	}
	RUN(test_log);
	RUN(test_log_package_warning);
	RUN(test_build);
	Rshutdown();
	return test_report("test_process");
}
