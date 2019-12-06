/* Copyright 2019 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "subprocess.h"

static char *program_name;

__attribute__((constructor, used))
static int libinit(int argc, char **argv)
{
	program_name = *argv;
	return 0;
}

static int init_target_private(struct subprocess_target *target)
{
	switch (target->type) {
	case TARGET_BUFFER:
	case TARGET_BUFFER_NULL_TERMINATED:
		return pipe(target->buffer._pipefd);
	default:
		return 0;
	}
}

static int flags_for_fd(int fd)
{
	switch (fd) {
	case STDIN_FILENO:
		return O_RDONLY;
	case STDOUT_FILENO:
	case STDERR_FILENO:
		return O_WRONLY;
	default:
		return -1;
	}
}

static int connect_process_target(struct subprocess_target *target, int fd)
{
	int target_fd;

	switch (target->type) {
	case TARGET_NULL:
		target_fd = open("/dev/null", flags_for_fd(fd));
		break;
	case TARGET_FD:
		target_fd = target->fd;
		break;
	case TARGET_FILE:
		target_fd = fileno(target->file);
		break;
	case TARGET_BUFFER:
	case TARGET_BUFFER_NULL_TERMINATED:
		switch (fd) {
		case STDIN_FILENO:
			target_fd = target->buffer._pipefd[0];
			close(target->buffer._pipefd[1]);
			break;
		case STDOUT_FILENO:
		case STDERR_FILENO:
			target_fd = target->buffer._pipefd[1];
			close(target->buffer._pipefd[0]);
			break;
		default:
			return -1;
		}
		break;
	}

	return dup2(target_fd, fd);
}

static int process_target_input(struct subprocess_target *target)
{
	int rv = 0;
	ssize_t write_rv;
	size_t bytes_to_write;
	char *buf;

	switch (target->type) {
	case TARGET_BUFFER:
		bytes_to_write = target->buffer.size;
		break;
	case TARGET_BUFFER_NULL_TERMINATED:
		bytes_to_write = strlen(target->buffer.buf);
		break;
	default:
		return 0;
	}

	close(target->buffer._pipefd[0]);
	buf = target->buffer.buf;
	while (bytes_to_write) {
		write_rv =
			write(target->buffer._pipefd[1], buf, bytes_to_write);
		if (write_rv <= 0) {
			rv = -1;
			goto cleanup;
		}
		buf += write_rv;
		bytes_to_write -= write_rv;
	}

cleanup:
	close(target->buffer._pipefd[1]);
	return rv;
}

static int process_target_output(struct subprocess_target *target)
{
	int rv = 0;
	ssize_t read_rv;
	size_t bytes_remaining;

	switch (target->type) {
	case TARGET_BUFFER:
		bytes_remaining = target->buffer.size;
		break;
	case TARGET_BUFFER_NULL_TERMINATED:
		if (target->buffer.size == 0)
			return -1;
		bytes_remaining = target->buffer.size - 1;
		break;
	default:
		return 0;
	}

	close(target->buffer._pipefd[1]);
	target->buffer.bytes_consumed = 0;
	while (bytes_remaining) {
		read_rv = read(
			target->buffer._pipefd[0],
			target->buffer.buf + target->buffer.bytes_consumed,
			bytes_remaining);
		if (read_rv < 0) {
			rv = -1;
			goto cleanup;
		}
		if (read_rv == 0)
			break;
		target->buffer.bytes_consumed += read_rv;
		bytes_remaining -= read_rv;
	}

	if (target->type == TARGET_BUFFER_NULL_TERMINATED)
		target->buffer.buf[target->buffer.bytes_consumed] = '\0';

cleanup:
	close(target->buffer._pipefd[0]);
	return rv;
}

struct subprocess_target subprocess_null = {
	.type = TARGET_NULL,
};

struct subprocess_target subprocess_stdin = {
	.type = TARGET_FD,
	.fd = STDIN_FILENO,
};

struct subprocess_target subprocess_stdout = {
	.type = TARGET_FD,
	.fd = STDOUT_FILENO,
};

struct subprocess_target subprocess_stderr = {
	.type = TARGET_FD,
	.fd = STDERR_FILENO,
};

int subprocess_run(const char *const argv[],
		   struct subprocess_target *input,
		   struct subprocess_target *output,
		   struct subprocess_target *error)
{
	int status;
	pid_t pid = -1;

	if (!input)
		input = &subprocess_stdin;
	if (!output)
		output = &subprocess_stdout;
	if (!error)
		error = &subprocess_stderr;

	if (init_target_private(input) < 0)
		goto fail;
	if (init_target_private(output) < 0)
		goto fail;
	if (init_target_private(error) < 0)
		goto fail;

	if ((pid = fork()) < 0)
		goto fail;
	if (pid == 0) {
		/* Child process */
		if (connect_process_target(input, STDIN_FILENO) < 0)
			goto fail;
		if (connect_process_target(output, STDOUT_FILENO) < 0)
			goto fail;
		if (connect_process_target(error, STDERR_FILENO) < 0)
			goto fail;
		execvp(*argv, (char *const *)argv);
		goto fail;
	}

	/* Parent process */
	if (process_target_input(input) < 0)
		goto fail;
	if (process_target_output(output) < 0)
		goto fail;
	if (process_target_output(error) < 0)
		goto fail;

	if (waitpid(pid, &status, 0) < 0)
		goto fail;

	if (WIFEXITED(status))
		return WEXITSTATUS(status);

fail:
	if (program_name)
		perror(program_name);
	else
		perror("subprocess");
	if (pid == 0)
		exit(127);
	return -1;
}