/* Copyright 2019 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Library for creating subprocesses in a high level manner. */

#ifndef VBOOT_REFERENCE_SUBPROCESS_H_
#define VBOOT_REFERENCE_SUBPROCESS_H_

#include <stdio.h>
#include <stdlib.h>

/**
 * subprocess_target is the "mini language" of the subprocess
 * library. It describes where to read or write data from the process.
 *
 * There are currently five target of targets:
 *
 * - TARGET_NULL: /dev/null, no need to describe any other fields.
 *
 * - TARGET_FD: file descriptor, put the fd in the fd field.
 *
 * - TARGET_FILE: FILE *, put the FILE pointer in the file field.
 *
 * - TARGET_BUFFER: read to, or write from, a buffer. Fields:
 *   - buffer->buf: the buffer
 *   - buffer->size: the size of that buffer
 *   - buffer->bytes_consumed: do not fill out this field.
 *     subprocess_run will set it to the number of bytes read from the
 *     process (if writing to a buffer). Goes unused when reading from
 *     a buffer.
 *
 * - TARGET_BUFFER_NULL_TERMINATED: when reading from a buffer, don't
 *   fill out the size field and subprocess_run will strlen for you.
 *   When writing to a buffer, subprocess_run will reserve one byte of
 *   the size for a null terminator and guarantee that the output is
 *   always NULL terminated.
 */
struct subprocess_target {
	enum {
		TARGET_NULL,
		TARGET_FD,
		TARGET_FILE,
		TARGET_BUFFER,
		TARGET_BUFFER_NULL_TERMINATED,
	} type;
	union {
		int fd;
		FILE *file;
		struct {
			char *buf;
			size_t size;

			/* This variable is used internally by "run" and
			 * shouldn't be operated on by the caller.
			 */
			int _pipefd[2];

			/* This variable is the output of the number of bytes
			 * read or written. It should be read by the caller, not
			 * set.
			 */
			size_t bytes_consumed;
		} buffer;
	};
};

/**
 * A convenience subprocess target which uses TARGET_NULL.
 */
struct subprocess_target subprocess_null;

/**
 * A convenience subprocess target which uses TARGET_FD to
 * STDIN_FILENO.
 */
struct subprocess_target subprocess_stdin;

/**
 * A convenience subprocess target which uses TARGET_FD to
 * STDOUT_FILENO.
 */
struct subprocess_target subprocess_stdout;

/**
 * A convenience subprocess target which uses TARGET_FD to
 * STDERR_FILENO.
 */
struct subprocess_target subprocess_stderr;

/**
 * Call a process described by argv and run until completion. Provide
 * input from the subprocess target input, output to the subprocess
 * target output, and error to the subprocess target error.
 *
 * If either input, output, or error are set to NULL, the will be
 * &subprocess_stdin, &subprocess_stdout, or &subprocess_stderr
 * respectively.
 *
 * Returns the exit status on success, or negative values on error.
 */
int subprocess_run(const char *const argv[],
		   struct subprocess_target *input,
		   struct subprocess_target *output,
		   struct subprocess_target *error);

#endif  /* VBOOT_REFERENCE_SUBPROCESS_H_ */