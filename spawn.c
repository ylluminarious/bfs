/****************************************************************************
 * bfs                                                                      *
 * Copyright (C) 2018 Tavian Barnes <tavianator@tavianator.com>             *
 *                                                                          *
 * Permission to use, copy, modify, and/or distribute this software for any *
 * purpose with or without fee is hereby granted.                           *
 *                                                                          *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES *
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF         *
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR  *
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES   *
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN    *
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF  *
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.           *
 ****************************************************************************/

#include "spawn.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/**
 * Types of spawn actions.
 */
enum bfs_spawn_op {
	BFS_SPAWN_FCHDIR,
};

/**
 * A spawn action.
 */
struct bfs_spawn_action {
	struct bfs_spawn_action *next;

	enum bfs_spawn_op op;
	int fd;
};

int bfs_spawn_init(struct bfs_spawn *ctx) {
	ctx->flags = 0;
	ctx->actions = NULL;
	ctx->tail = &ctx->actions;
	return 0;
}

int bfs_spawn_destroy(struct bfs_spawn *ctx) {
	struct bfs_spawn_action *action = ctx->actions;
	while (action) {
		struct bfs_spawn_action *next = action->next;
		free(action);
		action = next;
	}
	return 0;
}

int bfs_spawn_setflags(struct bfs_spawn *ctx, enum bfs_spawn_flags flags) {
	ctx->flags = flags;
	return 0;
}

/** Add a spawn action to the chain. */
static struct bfs_spawn_action *bfs_spawn_add(struct bfs_spawn *ctx, enum bfs_spawn_op op) {
	struct bfs_spawn_action *action = malloc(sizeof(*action));
	if (action) {
		action->next = NULL;
		action->op = op;
		action->fd = -1;

		*ctx->tail = action;
		ctx->tail = &action->next;
	}
	return action;
}

int bfs_spawn_addfchdir(struct bfs_spawn *ctx, int fd) {
	if (fd < 0) {
		errno = EBADF;
		return -1;
	}

	struct bfs_spawn_action *action = bfs_spawn_add(ctx, BFS_SPAWN_FCHDIR);
	if (action) {
		action->fd = fd;
		return 0;
	} else {
		return -1;
	}
}

/** Facade for execvpe() which is non-standard. */
static int bfs_execvpe(const char *exe, char **argv, char **envp) {
#if __GLIBC__ || __linux__ || __NetBSD__ || __OpenBSD__
	return execvpe(exe, argv, envp);
#else
	extern char **environ;
	environ = envp;
	return execvp(exe, argv);
#endif
}

/** Holder for bfs_spawn_exec() arguments. */
struct bfs_spawn_args {
	const char *exe;
	const struct bfs_spawn *ctx;
	char **argv;
	char **envp;
	int *pipefd;
};

/** Actually exec() the new process. */
static int bfs_spawn_exec(void *ptr) {
	const struct bfs_spawn_args *args = ptr;

	const struct bfs_spawn *ctx = args->ctx;
	enum bfs_spawn_flags flags = ctx ? ctx->flags : 0;
	const struct bfs_spawn_action *actions = ctx ? ctx->actions : NULL;

	close(args->pipefd[0]);
	int pipefd = args->pipefd[1];

	for (const struct bfs_spawn_action *action = actions; action; action = action->next) {
		switch (action->op) {
		case BFS_SPAWN_FCHDIR:
			if (fchdir(action->fd) != 0) {
				goto fail;
			}
			break;
		}
	}

	if (flags & BFS_SPAWN_USEPATH) {
		bfs_execvpe(args->exe, args->argv, args->envp);
	} else {
		execve(args->exe, args->argv, args->envp);
	}

	int error;
fail:
	error = errno;
	while (write(pipefd, &error, sizeof(error)) < sizeof(error));
	close(pipefd);
	return 127;
}

/** fork() or clone() a process, whatever's available. */
static pid_t bfs_clone(int (*fn)(void *ptr), void *ptr) {
#if __linux__
	char stack[4096];
	int flags = CLONE_VFORK | CLONE_VM | SIGCHLD;

#if __ia64__
	pid_t ret = __clone2(fn, stack, sizeof(stack), flags, ptr, NULL, NULL, NULL);
#else
	pid_t ret = clone(fn, stack + sizeof(stack), flags, ptr, NULL, NULL, NULL);
#endif
	return ret;

#else // !__linux__

	pid_t ret = fork();
	if (ret < 0) {
		return -1;
	} else if (ret == 0) {
		// Child
		_Exit(fn(ptr));
	} else {
		// Parent
		return ret;
	}
#endif
}

pid_t bfs_spawn(const char *exe, const struct bfs_spawn *ctx, char **argv, char **envp) {
	// Use a pipe to report errors from the child
	int pipefd[2];
	if (pipe_cloexec(pipefd) != 0) {
		return -1;
	}

	struct bfs_spawn_args args = {
		.exe = exe,
		.ctx = ctx,
		.argv = argv,
		.envp = envp,
		.pipefd = pipefd,
	};
	pid_t pid = bfs_clone(bfs_spawn_exec, &args);

	int error;
	if (pid < 0) {
		error = errno;
		close(pipefd[1]);
		close(pipefd[0]);
		errno = error;
		return -1;
	}

	close(pipefd[1]);

	ssize_t nbytes = read(pipefd[0], &error, sizeof(error));
	close(pipefd[0]);
	if (nbytes == sizeof(error)) {
		int wstatus;
		waitpid(pid, &wstatus, 0);
		errno = error;
		return -1;
	}

	return pid;
}
