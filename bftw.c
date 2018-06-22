/****************************************************************************
 * bfs                                                                      *
 * Copyright (C) 2015-2017 Tavian Barnes <tavianator@tavianator.com>        *
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

/**
 * The bftw() implementation consists of the following components:
 *
 * - struct bftw_dir: A directory that has been encountered during the
 *   traversal.  They have reference-counted links to their parents in the
 *   directory tree.
 *
 * - struct bftw_cache: Holds bftw_dir's with open file descriptors, used for
 *   openat() to minimize the amount of path re-traversals that need to happen.
 *   Currently implemented as a priority queue based on depth and reference
 *   count.
 *
 * - struct bftw_queue: The queue of bftw_dir's left to explore.  Implemented as
 *   a simple circular buffer.
 *
 * - struct bftw_state: Represents the current state of the traversal, allowing
 *   bftw() to be factored into various helper functions.
 */

#include "bftw.h"
#include "dstring.h"
#include "stat.h"
#include "util.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int bftw_mutex_lock(pthread_mutex_t *mutex) {
	int ret = pthread_mutex_lock(mutex);
	assert(ret == 0);
	return ret;
}

static int bftw_mutex_unlock(pthread_mutex_t *mutex) {
	int ret = pthread_mutex_unlock(mutex);
	assert(ret == 0);
	return ret;
}

static int bftw_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
	int ret = pthread_cond_wait(cond, mutex);

	if (ret != 0) {
		assert(false);
		bftw_mutex_unlock(mutex);
		bftw_mutex_lock(mutex);
	}

	return ret;
}

static int bftw_cond_signal(pthread_cond_t *cond) {
	int ret = pthread_cond_signal(cond);
	assert(ret == 0);
	return ret;
}

static int bftw_cond_broadcast(pthread_cond_t *cond) {
	int ret = pthread_cond_broadcast(cond);
	assert(ret == 0);
	return ret;
}

static int bftw_rwlock_rdlock(pthread_rwlock_t *rwlock) {
	int ret = pthread_rwlock_rdlock(rwlock);
	assert(ret == 0);
	return ret;
}

static int bftw_rwlock_wrlock(pthread_rwlock_t *rwlock) {
	int ret = pthread_rwlock_wrlock(rwlock);
	assert(ret == 0);
	return ret;
}

static int bftw_rwlock_unlock(pthread_rwlock_t *rwlock) {
	int ret = pthread_rwlock_unlock(rwlock);
	assert(ret == 0);
	return ret;
}

static int bftw_join(pthread_t thread, void **retval) {
	int ret = pthread_join(thread, retval);
	assert(ret == 0);
	return ret;
}

/**
 * A directory.
 */
struct bftw_dir {
	/** The parent directory, if any. */
	struct bftw_dir *parent;
	/** This directory's depth in the walk. */
	size_t depth;

	/** The next directory in the queue, if any. */
	struct bftw_dir *next;

	/** Reference count. */
	size_t refcount;
	/** Index in the bftw_cache priority queue. */
	size_t heap_index;

	/** An open file descriptor to this directory, or -1. */
	int fd;

	/** The device number, for cycle detection. */
	dev_t dev;
	/** The inode number, for cycle detection. */
	ino_t ino;

	/** The offset of this directory in the full path. */
	size_t nameoff;
	/** The length of the directory's name. */
	size_t namelen;
	/** The directory's name. */
	char name[];
};

/**
 * A cache of open directories.
 */
struct bftw_cache {
	/** A min-heap of open directories. */
	struct bftw_dir **heap;
	/** Current heap size. */
	size_t size;
	/** Maximum heap size. */
	size_t capacity;

	/** Lock protecting the cache from concurrent modification. */
	pthread_rwlock_t rwlock;
};

/** Initialize a cache. */
static int bftw_cache_init(struct bftw_cache *cache, size_t capacity) {
	cache->heap = malloc(capacity*sizeof(*cache->heap));
	if (!cache->heap) {
		return -1;
	}

	cache->size = 0;
	cache->capacity = capacity;

	if (pthread_rwlock_init(&cache->rwlock, NULL) != 0) {
		free(cache->heap);
		return -1;
	}

	return 0;
}

/** Destroy a cache. */
static void bftw_cache_destroy(struct bftw_cache *cache) {
	assert(cache->size == 0);
	pthread_rwlock_destroy(&cache->rwlock);
	free(cache->heap);
}

/** Check if two heap entries are in heap order. */
static bool bftw_heap_check(const struct bftw_dir *parent, const struct bftw_dir *child) {
	if (parent->depth > child->depth) {
		return true;
	} else if (parent->depth < child->depth) {
		return false;
	} else {
		return parent->refcount <= child->refcount;
	}
}

/** Move a bftw_dir to a particular place in the heap. */
static void bftw_heap_move(struct bftw_cache *cache, struct bftw_dir *dir, size_t i) {
	cache->heap[i] = dir;
	dir->heap_index = i;
}

/** Bubble an entry up the heap. */
static void bftw_heap_bubble_up(struct bftw_cache *cache, struct bftw_dir *dir) {
	size_t i = dir->heap_index;

	while (i > 0) {
		size_t pi = (i - 1)/2;
		struct bftw_dir *parent = cache->heap[pi];
		if (bftw_heap_check(parent, dir)) {
			break;
		}

		bftw_heap_move(cache, parent, i);
		i = pi;
	}

	bftw_heap_move(cache, dir, i);
}

/** Bubble an entry down the heap. */
static void bftw_heap_bubble_down(struct bftw_cache *cache, struct bftw_dir *dir) {
	size_t i = dir->heap_index;

	while (true) {
		size_t ci = 2*i + 1;
		if (ci >= cache->size) {
			break;
		}

		struct bftw_dir *child = cache->heap[ci];

		size_t ri = ci + 1;
		if (ri < cache->size) {
			struct bftw_dir *right = cache->heap[ri];
			if (!bftw_heap_check(child, right)) {
				ci = ri;
				child = right;
			}
		}

		if (bftw_heap_check(dir, child)) {
			break;
		}

		bftw_heap_move(cache, child, i);
		i = ci;
	}

	bftw_heap_move(cache, dir, i);
}

/** Bubble an entry up or down the heap. */
static void bftw_heap_bubble(struct bftw_cache *cache, struct bftw_dir *dir) {
	size_t i = dir->heap_index;

	if (i > 0) {
		size_t pi = (i - 1)/2;
		struct bftw_dir *parent = cache->heap[pi];
		if (!bftw_heap_check(parent, dir)) {
			bftw_heap_bubble_up(cache, dir);
			return;
		}
	}

	bftw_heap_bubble_down(cache, dir);
}

/** Increment a bftw_dir's reference count. */
static void bftw_dir_incref(struct bftw_cache *cache, struct bftw_dir *dir) {
	++dir->refcount;

	if (dir->fd >= 0) {
		bftw_heap_bubble_down(cache, dir);
	}
}

/** Decrement a bftw_dir's reference count. */
static void bftw_dir_decref(struct bftw_cache *cache, struct bftw_dir *dir) {
	--dir->refcount;

	if (dir->fd >= 0) {
		bftw_heap_bubble_up(cache, dir);
	}
}

/** Add a bftw_dir to the cache. */
static void bftw_cache_add(struct bftw_cache *cache, struct bftw_dir *dir) {
	assert(cache->size < cache->capacity);
	assert(dir->fd >= 0);

	size_t size = cache->size++;
	dir->heap_index = size;
	bftw_heap_bubble_up(cache, dir);
}

/** Remove a bftw_dir from the cache. */
static void bftw_cache_remove(struct bftw_cache *cache, struct bftw_dir *dir) {
	assert(cache->size > 0);
	assert(dir->fd >= 0);

	size_t size = --cache->size;
	size_t i = dir->heap_index;
	if (i != size) {
		struct bftw_dir *end = cache->heap[size];
		end->heap_index = i;
		bftw_heap_bubble(cache, end);
	}
}

/** Close a bftw_dir. */
static void bftw_dir_close(struct bftw_cache *cache, struct bftw_dir *dir) {
	assert(dir->fd >= 0);

	bftw_cache_remove(cache, dir);

	close(dir->fd);
	dir->fd = -1;
}

/** Pop a directory from the cache. */
static void bftw_cache_pop(struct bftw_cache *cache) {
	assert(cache->size > 0);
	bftw_dir_close(cache, cache->heap[0]);
}

/**
 * Shrink the cache, to recover from EMFILE.
 *
 * @param cache
 *         The cache in question.
 * @param saved
 *         A bftw_dir that must be preserved.
 * @return
 *         0 if successfully shrunk, otherwise -1.
 */
static int bftw_cache_shrink(struct bftw_cache *cache, const struct bftw_dir *saved) {
	int ret = -1;
	struct bftw_dir *dir = NULL;

	if (cache->size >= 1) {
		dir = cache->heap[0];
		if (dir == saved && cache->size >= 2) {
			dir = cache->heap[1];
		}
	}

	if (dir && dir != saved) {
		bftw_dir_close(cache, dir);
		ret = 0;
	}

	cache->capacity = cache->size;
	return ret;
}

/** Create a new bftw_dir. */
static struct bftw_dir *bftw_dir_new(struct bftw_cache *cache, struct bftw_dir *parent, const char *name) {
	size_t namelen = strlen(name);
	size_t size = sizeof(struct bftw_dir) + namelen + 1;

	bool needs_slash = false;
	if (namelen == 0 || name[namelen - 1] != '/') {
		needs_slash = true;
		++size;
	}

	struct bftw_dir *dir = malloc(size);
	if (!dir) {
		return NULL;
	}

	dir->parent = parent;

	if (parent) {
		dir->depth = parent->depth + 1;
		dir->nameoff = parent->nameoff + parent->namelen;

		bftw_rwlock_wrlock(&cache->rwlock);
		bftw_dir_incref(cache, parent);
		bftw_rwlock_unlock(&cache->rwlock);
	} else {
		dir->depth = 0;
		dir->nameoff = 0;
	}

	dir->next = NULL;

	dir->refcount = 1;
	dir->fd = -1;

	dir->dev = -1;
	dir->ino = -1;

	memcpy(dir->name, name, namelen);
	if (needs_slash) {
		dir->name[namelen++] = '/';
	}
	dir->name[namelen] = '\0';
	dir->namelen = namelen;

	return dir;
}

/**
 * Compute the path to a bftw_dir.
 */
static int bftw_dir_path(const struct bftw_dir *dir, char **path) {
	size_t pathlen = dir->nameoff + dir->namelen;
	if (dstresize(path, pathlen) != 0) {
		return -1;
	}
	char *dest = *path;

	// Build the path backwards
	while (dir) {
		memcpy(dest + dir->nameoff, dir->name, dir->namelen);
		dir = dir->parent;
	}

	return 0;
}

/**
 * Get the appropriate (fd, path) pair for the *at() family of functions.
 *
 * @param dir
 *         The directory being accessed.
 * @param path
 *         The full path to that directory.
 * @param[out] at_fd
 *         Will hold the appropriate file descriptor to use.
 * @param[in,out] at_path
 *         Will hold the appropriate path to use.
 * @return The closest open ancestor file.
 */
static struct bftw_dir *bftw_dir_base(struct bftw_dir *dir, const char *path, int *at_fd, const char **at_path) {
	// XXX: Don't take at_fd?  It's just base ? AT_FDCWD : base->fd

	struct bftw_dir *base = dir;

	do {
		base = base->parent;
	} while (base && base->fd < 0);

	if (base) {
		*at_fd = base->fd;
		*at_path = path + base->nameoff + base->namelen;
	} else {
		*at_fd = AT_FDCWD;
		*at_path = path;
	}

	return base;
}

/**
 * Open a bftw_dir relative to another one.
 *
 * @param cache
 *         The cache containing the dir.
 * @param dir
 *         The directory to open.
 * @param base
 *         The base directory for the relative path (may be NULL).
 * @param at_path
 *         The relative path to base.
 * @param exclusive
 *         Whether cache->rwlock is already locked in exclusive (write) mode.  If
 *         it isn't, it must be read-locked.  Then this function will unlock it
 *         and acquire a write lock during its operation.
 * @return
 *         The opened file descriptor, or negative on error.
 */
static int bftw_dir_openat(struct bftw_cache *cache, struct bftw_dir *dir, const struct bftw_dir *base, const char *at_path, bool exclusive) {
	assert(dir->fd < 0);

	int at_fd = base ? base->fd : AT_FDCWD;
	int flags = O_RDONLY | O_CLOEXEC | O_DIRECTORY;
	int fd = openat(at_fd, at_path, flags);
	int error = errno;

	if (!exclusive) {
		bftw_rwlock_unlock(&cache->rwlock);
		bftw_rwlock_wrlock(&cache->rwlock);
	}

	// XXX: There should not be races to open the same directory?
	assert(dir->fd < 0);

	if (fd < 0 && error == EMFILE && base && base->fd >= 0) {
		if (bftw_cache_shrink(cache, base) == 0) {
			fd = openat(base->fd, at_path, flags);
			error = errno;
		}
	}

	if (fd >= 0) {
		if (cache->size == cache->capacity) {
			bftw_cache_pop(cache);
		}

		dir->fd = fd;
		bftw_cache_add(cache, dir);
	}

	errno = error;
	return fd;
}

/**
 * Open a bftw_dir.
 *
 * This function should be called with cache->rwlock read-locked.  It returns
 * with cache->rwlock write-locked!
 *
 * @param cache
 *         The cache containing the directory.
 * @param dir
 *         The directory to open.
 * @param path
 *         The full path to the dir.
 * @return
 *         The opened file descriptor, or negative on error.
 */
static int bftw_dir_open(struct bftw_cache *cache, struct bftw_dir *dir, const char *path) {
	int at_fd;
	const char *at_path;
	struct bftw_dir *base = bftw_dir_base(dir, path, &at_fd, &at_path);

	int fd = bftw_dir_openat(cache, dir, base, at_path, false);

	if (fd < 0 && errno == EMFILE) {
		// bftw_dir_openat() has already tried to handle EMFILE if it
		// could, but base may have been closed in the meantime, or we
		// may need to close it and try a different base
		if (bftw_cache_shrink(cache, NULL) == 0) {
			base = bftw_dir_base(dir, path, &at_fd, &at_path);
			fd = bftw_dir_openat(cache, dir, base, at_path, true);
		}
	}

	if (fd >= 0 || errno != ENAMETOOLONG) {
		return fd;
	}

	// Handle ENAMETOOLONG by manually traversing the path component-by-component

	if (base && base->fd < 0) {
		base = bftw_dir_base(dir, path, &at_fd, &at_path);
	}

	// -1 to include the root, which has depth == 0
	size_t offset = base ? base->depth : -1;
	size_t levels = dir->depth - offset;
	if (levels < 2) {
		return fd;
	}

	struct bftw_dir **parents = malloc(levels * sizeof(*parents));
	if (!parents) {
		return fd;
	}

	struct bftw_dir *parent = dir;
	for (size_t i = levels; i-- > 0;) {
		parents[i] = parent;
		parent = parent->parent;
	}

	for (size_t i = 0; i < levels; ++i) {
		fd = bftw_dir_openat(cache, parents[i], base, parents[i]->name, true);
		if (fd < 0) {
			break;
		}

		base = parents[i];
	}

	free(parents);
	return fd;
}

/**
 * Open a DIR* for a bftw_dir.
 *
 * @param cache
 *         The cache containing the directory.
 * @param dir
 *         The directory to open.
 * @param path
 *         The full path to the directory.
 * @return
 *         The opened DIR *, or NULL on error.
 */
static DIR *bftw_dir_opendir(struct bftw_cache *cache, struct bftw_dir *dir, const char *path) {
	if (bftw_rwlock_rdlock(&cache->rwlock) != 0) {
		return NULL;
	}

	int error;
	int fd = bftw_dir_open(cache, dir, path);
	if (fd < 0) {
		error = errno;
		bftw_rwlock_unlock(&cache->rwlock);
		errno = error;
		return NULL;
	}

	// Now we dup() the fd and pass it to fdopendir().  This way we can
	// close the DIR* as soon as we're done with it, reducing the memory
	// footprint significantly, while keeping the fd around for future
	// openat() calls.

	int dfd = dup_cloexec(fd);

	if (dfd < 0 && errno == EMFILE) {
		if (bftw_cache_shrink(cache, dir) == 0) {
			dfd = dup_cloexec(fd);
		}
	}

	// Unlock here since fdopendir() may do I/O
	error = errno;
	bftw_rwlock_unlock(&cache->rwlock);

	if (dfd < 0) {
		errno = error;
		return NULL;
	}

	DIR *ret = fdopendir(dfd);
	if (!ret) {
		error = errno;
		close(dfd);
		errno = error;
	}

	return ret;
}

/** Free a bftw_dir. */
static void bftw_dir_free(struct bftw_cache *cache, struct bftw_dir *dir) {
	assert(dir->refcount == 0);

	bftw_rwlock_wrlock(&cache->rwlock);
	if (dir->fd >= 0) {
		bftw_dir_close(cache, dir);
	}
	bftw_rwlock_unlock(&cache->rwlock);

	free(dir);
}

/**
 * A directory reader.
 */
struct bftw_reader {
	/** The next reader in the chain. */
	struct bftw_reader *next;

	/** The directory object. */
	struct bftw_dir *dir;
	/** The path to the directory. */
	char *path;
	/** The open handle to the directory. */
	DIR *handle;
	/** The current directory entry. */
	struct dirent *de;
	/** Any error code that has occurred. */
	int error;
	/** Whether this reader is ready to be accessed by the main thread. */
	bool ready;
};

/** Create a reader. */
static struct bftw_reader *bftw_reader_new(void) {
	struct bftw_reader *reader = malloc(sizeof(*reader));
	if (!reader) {
		return NULL;
	}

	reader->path = dstralloc(0);
	if (!reader->path) {
		free(reader);
		return NULL;
	}

	reader->next = NULL;
	reader->dir = NULL;
	reader->handle = NULL;
	reader->de = NULL;
	reader->error = 0;
	reader->ready = false;
	return reader;
}

/** Read a directory entry. */
static int bftw_reader_read(struct bftw_reader *reader) {
	if (xreaddir(reader->handle, &reader->de) != 0) {
		reader->error = errno;
		return -1;
	}

	return 0;
}

// XXX: HACK
static struct bftw_cache *global_cache;

/** Open a directory for reading. */
static int bftw_reader_open(struct bftw_reader *reader) {
	assert(!reader->handle);

	if (bftw_dir_path(reader->dir, &reader->path) != 0) {
		// XXX: Make sure dstrlen(reader->path) == 0
		reader->error = errno;
		return -1;
	}

	reader->handle = bftw_dir_opendir(global_cache, reader->dir, reader->path);
	if (!reader->handle) {
		reader->error = errno;
		return -1;
	}

	// Read the first entry as soon as we open the directory, so that we can
	// parallelize both opening and reading
	return bftw_reader_read(reader);
}

/** Close a directory. */
static int bftw_reader_close(struct bftw_reader *reader) {
	assert(reader->handle);

	int ret = 0;
	if (closedir(reader->handle) != 0) {
		reader->error = errno;
		ret = -1;
	}

	reader->handle = NULL;
	reader->de = NULL;

	return ret;
}

/** Refresh a reader before reading a new directory. */
static void bftw_reader_refresh(struct bftw_reader *reader) {
	assert(reader->next == NULL);
	assert(reader->handle == NULL);
	assert(reader->de == NULL);

	reader->dir = NULL;
	dstresize(&reader->path, 0);
	reader->error = 0;
	reader->ready = false;
}

/** Free a reader. */
static void bftw_reader_free(struct bftw_reader *reader) {
	if (reader->handle) {
		bftw_reader_close(reader);
	}

	dstrfree(reader->path);
	free(reader);
}

/**
 * A queue of bftw_dir's to examine.
 */
struct bftw_queue {
	/** The head of the queue. */
	struct bftw_dir *head;
	/** The next directory to read (in the background). */
	struct bftw_dir *next;
	/** The tail of the queue. */
	struct bftw_dir *tail;

	/** Chain of readers in the same order is the main queue. */
	struct bftw_reader *read_head;
	/** Tail of the reader chain. */
	struct bftw_reader *read_tail;
	/** Chain of readers waiting to be used. */
	struct bftw_reader *read_next;
	/** Whether the background readers should stay alive. */
	bool alive;

	/** Mutex guarding this queue. */
	pthread_mutex_t mutex;
	/** Condition variable to wake the main thread. */
	pthread_cond_t fg_cond;
	/** Condition variable to wake reader threads. */
	pthread_cond_t bg_cond;

	/** The number of background threads. */
	unsigned int nthreads;
	/** The background threads themselves. */
	pthread_t *threads;
};

/** Start reading the next directory from the queue. */
static struct bftw_reader *bftw_queue_read(struct bftw_queue *queue) {
	assert(queue->next);
	assert(queue->read_next);

	struct bftw_reader *reader = queue->read_next;
	queue->read_next = reader->next;
	reader->next = NULL;

	reader->dir = queue->next;
	queue->next = reader->dir->next;

	return reader;
}

/** Background thread function. */
static void *bftw_reader_thread(void *ptr) {
	struct bftw_queue *queue = ptr;

	if (bftw_mutex_lock(&queue->mutex) != 0) {
		return NULL;
	}

	while (queue->alive) {
		while (!queue->next || !queue->read_next) {
			bftw_cond_wait(&queue->bg_cond, &queue->mutex);
			if (!queue->alive) {
				goto done;
			}
		}

		struct bftw_reader *reader = bftw_queue_read(queue);

		if (!queue->read_head) {
			queue->read_head = reader;
		}
		if (queue->read_tail) {
			queue->read_tail->next = reader;
		}
		queue->read_tail = reader;

		bftw_mutex_unlock(&queue->mutex);
		bftw_reader_open(reader);
		bftw_mutex_lock(&queue->mutex);

		reader->ready = true;
		if (reader == queue->read_head) {
			bftw_cond_signal(&queue->fg_cond);
		}
	}

done:
	bftw_mutex_unlock(&queue->mutex);

	return NULL;
}

/** Stop and join the background threads. */
static void bftw_queue_join(struct bftw_queue *queue) {
	bftw_mutex_lock(&queue->mutex);

	assert(queue->alive);
	queue->alive = false;
	bftw_cond_broadcast(&queue->bg_cond);

	bftw_mutex_unlock(&queue->mutex);

	for (unsigned int i = 0; i < queue->nthreads; ++i) {
		bftw_join(queue->threads[i], NULL);
	}
}

/** Free a chain of readers. */
// XXX: Name?
static void bftw_free_readers(struct bftw_reader *reader) {
	while (reader) {
		struct bftw_reader *next = reader->next;
		bftw_reader_free(reader);
		reader = next;
	}
}

/** Initialize a bftw_queue. */
static int bftw_queue_init(struct bftw_queue *queue) {
	queue->head = NULL;
	queue->next = NULL;
	queue->tail = NULL;

	queue->read_head = NULL;
	queue->read_tail = NULL;
	queue->read_next = NULL;
	queue->alive = true;

	unsigned int nthreads = 1; // XXX
	unsigned int nreaders = nthreads + 1;
	for (unsigned int i = 0; i < nreaders; ++i) {
		struct bftw_reader *reader = bftw_reader_new();
		if (!reader) {
			goto fail_readers;
		}
		reader->next = queue->read_next;
		queue->read_next = reader;
	}

	if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
		goto fail_readers;
	}
	if (pthread_cond_init(&queue->fg_cond, NULL) != 0) {
		goto fail_mutex;
	}
	if (pthread_cond_init(&queue->bg_cond, NULL) != 0) {
		goto fail_fg_cond;
	}

	queue->threads = malloc(nthreads*sizeof(*queue->threads));
	if (!queue->threads) {
		goto fail_bg_cond;
	}

	queue->nthreads = 0;
	for (unsigned int i = 0; i < nthreads; ++i) {
		if (pthread_create(queue->threads + i, NULL, bftw_reader_thread, queue) != 0) {
			goto fail_threads;
		}
		++queue->nthreads;
	}

	return 0;

fail_threads:
	bftw_queue_join(queue);
	free(queue->threads);
fail_bg_cond:
	pthread_cond_destroy(&queue->bg_cond);
fail_fg_cond:
	pthread_cond_destroy(&queue->fg_cond);
fail_mutex:
	pthread_mutex_destroy(&queue->mutex);
fail_readers:
	bftw_free_readers(queue->read_next);
	return -1;
}

/** Destroy a bftw_queue. */
static void bftw_queue_destroy(struct bftw_queue *queue) {
	assert(queue->head == NULL);
	assert(queue->tail == NULL);
	assert(!queue->alive);

	free(queue->threads);

	pthread_cond_destroy(&queue->bg_cond);
	pthread_cond_destroy(&queue->fg_cond);
	pthread_mutex_destroy(&queue->mutex);

	bftw_free_readers(queue->read_head);
	bftw_free_readers(queue->read_next);
}

/** Add a directory to the bftw_queue. */
static void bftw_queue_push(struct bftw_queue *queue, struct bftw_dir *dir) {
	bftw_mutex_lock(&queue->mutex);

	assert(dir->next == NULL);

	if (!queue->head) {
		queue->head = dir;
	}
	if (!queue->next) {
		queue->next = dir;
	}
	if (queue->tail) {
		queue->tail->next = dir;
	}
	queue->tail = dir;

	if (queue->read_next) {
		bftw_cond_signal(&queue->bg_cond);
	}

	bftw_mutex_unlock(&queue->mutex);
}

/** Pop the next directory from the queue. */
static struct bftw_dir *bftw_queue_pop(struct bftw_queue *queue) {
	struct bftw_dir *dir = queue->head;
	queue->head = dir->next;
	if (queue->tail == dir) {
		queue->tail = NULL;
	}
	dir->next = NULL;
	return dir;
}

/** Pop the next directory and start reading it. */
static struct bftw_reader *bftw_queue_poll(struct bftw_queue *queue) {
	bftw_mutex_lock(&queue->mutex);

	while (!queue->read_head || !queue->read_head->ready) {
		if (!queue->read_head && queue->read_next) {
			// We can open it ourselves without waiting for a
			// background thread
			struct bftw_reader *reader = bftw_queue_read(queue);
			bftw_queue_pop(queue);
			bftw_mutex_unlock(&queue->mutex);
			bftw_reader_open(reader);
			return reader;
		} else {
			bftw_cond_wait(&queue->fg_cond, &queue->mutex);
		}
	}

	struct bftw_reader *reader = queue->read_head;
	queue->read_head = reader->next;
	if (queue->read_tail == reader) {
		queue->read_tail = NULL;
	}
	reader->next = NULL;

	bftw_queue_pop(queue);
	bftw_mutex_unlock(&queue->mutex);
	return reader;
}

/** Return a reader to the queue. */
// XXX: Name?
static void bftw_queue_done_reading(struct bftw_queue *queue, struct bftw_reader *reader) {
	bftw_reader_refresh(reader);

	bftw_mutex_lock(&queue->mutex);

	reader->next = queue->read_next;
	queue->read_next = reader;

	bftw_mutex_unlock(&queue->mutex);
}

/** Call stat() and use the results. */
static int bftw_stat(struct BFTW *ftwbuf, struct bfs_stat *sb) {
	int ret = bfs_stat(ftwbuf->at_fd, ftwbuf->at_path, ftwbuf->at_flags, BFS_STAT_BROKEN_OK, sb);
	if (ret == 0) {
		ftwbuf->statbuf = sb;
		ftwbuf->typeflag = mode_to_typeflag(sb->mode);
	}
	return ret;
}

/**
 * Holds the current state of the bftw() traversal.
 */
struct bftw_state {
	/** bftw() callback. */
	bftw_fn *fn;
	/** bftw() flags. */
	int flags;
	/** bftw() callback data. */
	void *ptr;

	/** The appropriate errno value, if any. */
	int error;

	/** The cache of open directories. */
	struct bftw_cache cache;
	/** The queue of directories left to explore. */
	struct bftw_queue queue;

	/** The reader for the current directory. */
	struct bftw_reader *reader;
	/** Whether the current visit is pre- or post-order. */
	enum bftw_visit visit;

	/** The root path of the walk. */
	const char *root;

	/** Extra data about the current file. */
	struct BFTW ftwbuf;
	/** bfs_stat() buffer for the current file. */
	struct bfs_stat statbuf;
};

/**
 * Initialize the bftw() state.
 */
static int bftw_state_init(struct bftw_state *state, const char *root, bftw_fn *fn, int nopenfd, int flags, void *ptr) {
	state->root = root;
	state->fn = fn;
	state->flags = flags;
	state->ptr = ptr;

	state->error = 0;

	if (nopenfd < 2) {
		errno = EMFILE;
		return -1;
	}

	// -1 to account for dup()
	if (bftw_cache_init(&state->cache, nopenfd - 1) != 0) {
		goto fail;
	}
	global_cache = &state->cache;

	if (bftw_queue_init(&state->queue) != 0) {
		goto fail_cache;
	}

	state->reader = NULL;
	state->visit = BFTW_PRE;

	return 0;

fail_cache:
	bftw_cache_destroy(&state->cache);
fail:
	return -1;
}

/**
 * Update the path for the current file.
 */
static int bftw_update_path(struct bftw_state *state) {
	struct bftw_reader *reader = state->reader;
	struct bftw_dir *dir = reader->dir;
	struct dirent *de = reader->de;

	size_t length;
	if (de) {
		length = dir->nameoff + dir->namelen;
	} else if (dir->depth == 0) {
		// Use exactly the string passed to bftw(), including any trailing slashes
		length = strlen(state->root);
	} else {
		// -1 to trim the trailing slash
		length = dir->nameoff + dir->namelen - 1;
	}

	if (dstrlen(reader->path) < length) {
		errno = reader->error;
		return -1;
	}
	dstresize(&reader->path, length);

	if (de) {
		if (dstrcat(&reader->path, reader->de->d_name) != 0) {
			return -1;
		}
	}

	return 0;
}

/**
 * Initialize the buffers with data about the current path.
 */
static void bftw_prepare_visit(struct bftw_state *state) {
	struct bftw_reader *reader = state->reader;
	struct bftw_dir *dir = reader ? reader->dir : NULL;
	struct dirent *de = reader ? reader->de : NULL;

	struct BFTW *ftwbuf = &state->ftwbuf;
	ftwbuf->path = reader ? reader->path : state->root;
	ftwbuf->root = state->root;
	ftwbuf->depth = 0;
	ftwbuf->error = reader ? reader->error : 0;
	ftwbuf->visit = state->visit;
	ftwbuf->statbuf = NULL;
	ftwbuf->at_fd = AT_FDCWD;
	ftwbuf->at_path = ftwbuf->path;
	ftwbuf->at_flags = AT_SYMLINK_NOFOLLOW;

	if (dir) {
		ftwbuf->nameoff = dir->nameoff;
		ftwbuf->depth = dir->depth;

		if (de) {
			ftwbuf->nameoff += dir->namelen;
			++ftwbuf->depth;

			ftwbuf->at_fd = dirfd(reader->handle);
			ftwbuf->at_path += ftwbuf->nameoff;
		} else {
			// XXX: A race may close this
			bftw_dir_base(dir, ftwbuf->path, &ftwbuf->at_fd, &ftwbuf->at_path);
		}
	}

	if (ftwbuf->depth == 0) {
		// Compute the name offset for root paths like "foo/bar"
		ftwbuf->nameoff = xbasename(ftwbuf->path) - ftwbuf->path;
	}

	if (ftwbuf->error != 0) {
		ftwbuf->typeflag = BFTW_ERROR;
		return;
	} else if (de) {
		ftwbuf->typeflag = dirent_to_typeflag(de);
	} else if (ftwbuf->depth > 0) {
		ftwbuf->typeflag = BFTW_DIR;
	} else {
		ftwbuf->typeflag = BFTW_UNKNOWN;
	}

	int follow_flags = BFTW_LOGICAL;
	if (ftwbuf->depth == 0) {
		follow_flags |= BFTW_COMFOLLOW;
	}
	bool follow = state->flags & follow_flags;
	if (follow) {
		ftwbuf->at_flags = 0;
	}

	bool detect_cycles = (state->flags & BFTW_DETECT_CYCLES) && de;

	bool xdev = state->flags & BFTW_XDEV;

	if ((state->flags & BFTW_STAT)
	    || ftwbuf->typeflag == BFTW_UNKNOWN
	    || (ftwbuf->typeflag == BFTW_LNK && follow)
	    || (ftwbuf->typeflag == BFTW_DIR && (detect_cycles || xdev))) {
		if (bftw_stat(ftwbuf, &state->statbuf) != 0) {
			ftwbuf->error = errno;
			ftwbuf->typeflag = BFTW_ERROR;
			return;
		}

		if (ftwbuf->typeflag == BFTW_DIR && detect_cycles) {
			dev_t dev = ftwbuf->statbuf->dev;
			ino_t ino = ftwbuf->statbuf->ino;
			for (const struct bftw_dir *parent = dir; parent; parent = parent->parent) {
				if (dev == parent->dev && ino == parent->ino) {
					ftwbuf->error = ELOOP;
					ftwbuf->typeflag = BFTW_ERROR;
					return;
				}
			}
		}
	}
}

/**
 * Invoke the callback.
 */
static int bftw_visit_path(struct bftw_state *state) {
	if (state->reader) {
		if (bftw_update_path(state) != 0) {
			state->error = errno;
			return BFTW_STOP;
		}
	}

	bftw_prepare_visit(state);

	// Never give the callback BFTW_ERROR unless BFTW_RECOVER is specified
	if (state->ftwbuf.typeflag == BFTW_ERROR && !(state->flags & BFTW_RECOVER)) {
		state->error = state->ftwbuf.error;
		return BFTW_STOP;
	}

	// Defensive copy
	struct BFTW ftwbuf = state->ftwbuf;

	enum bftw_action action = state->fn(&ftwbuf, state->ptr);
	switch (action) {
	case BFTW_CONTINUE:
	case BFTW_SKIP_SIBLINGS:
	case BFTW_SKIP_SUBTREE:
	case BFTW_STOP:
		return action;

	default:
		state->error = EINVAL;
		return BFTW_STOP;
	}
}

/**
 * Push a new directory onto the queue.
 */
static int bftw_push(struct bftw_state *state, const char *name) {
	struct bftw_dir *parent = NULL;
	if (state->reader) {
		parent = state->reader->dir;
	}

	struct bftw_dir *dir = bftw_dir_new(&state->cache, parent, name);
	if (!dir) {
		state->error = errno;
		return -1;
	}

	const struct bfs_stat *statbuf = state->ftwbuf.statbuf;
	if (statbuf) {
		dir->dev = statbuf->dev;
		dir->ino = statbuf->ino;
	}

	bftw_queue_push(&state->queue, dir);
	return 0;
}

/**
 * Finalize and free a directory we're done with.
 */
static int bftw_release_dir(struct bftw_state *state, struct bftw_dir *dir, bool do_visit) {
	int ret = BFTW_CONTINUE;

	if (!(state->flags & BFTW_DEPTH)) {
		do_visit = false;
	}

	state->visit = BFTW_POST;

	while (dir) {
		bftw_rwlock_wrlock(&state->cache.rwlock);
		bftw_dir_decref(&state->cache, dir);
		bftw_rwlock_unlock(&state->cache.rwlock);

		if (dir->refcount > 0) {
			break;
		}

		if (do_visit) {
			state->reader->dir = dir;
			if (bftw_visit_path(state) == BFTW_STOP) {
				ret = BFTW_STOP;
				do_visit = false;
			}
		}

		struct bftw_dir *parent = dir->parent;
		bftw_dir_free(&state->cache, dir);
		dir = parent;
	}

	state->visit = BFTW_PRE;
	return ret;
}

/**
 * Close and release the reader.
 */
static int bftw_release_reader(struct bftw_state *state, bool do_visit) {
	int ret = BFTW_CONTINUE;

	struct bftw_reader *reader = state->reader;
	if (reader->handle) {
		bftw_reader_close(reader);
	}

	if (reader->error != 0) {
		if (do_visit) {
			if (bftw_visit_path(state) == BFTW_STOP) {
				ret = BFTW_STOP;
				do_visit = false;
			}
		} else {
			state->error = reader->error;
		}
		reader->error = 0;
	}

	if (bftw_release_dir(state, reader->dir, do_visit) == BFTW_STOP) {
		ret = BFTW_STOP;
	}

	bftw_queue_done_reading(&state->queue, reader);
	state->reader = NULL;

	return ret;
}

/**
 * Dispose of the bftw() state.
 *
 * @return
 *         The bftw() return value.
 */
static int bftw_state_destroy(struct bftw_state *state) {
	if (state->reader) {
		bftw_release_reader(state, false);
	}

	struct bftw_queue *queue = &state->queue;
	bftw_queue_join(&state->queue);
	while (queue->head) {
		bftw_release_dir(state, bftw_queue_pop(queue), false);
	}
	bftw_queue_destroy(queue);

	bftw_cache_destroy(&state->cache);

	errno = state->error;
	if (state->error == 0) {
		return 0;
	} else {
		return -1;
	}
}

int bftw(const char *path, bftw_fn *fn, int nopenfd, enum bftw_flags flags, void *ptr) {
	struct bftw_state state;
	if (bftw_state_init(&state, path, fn, nopenfd, flags, ptr) != 0) {
		return -1;
	}

	// Handle 'path' itself first
	switch (bftw_visit_path(&state)) {
	case BFTW_SKIP_SUBTREE:
	case BFTW_STOP:
		goto done;
	}

	if (state.ftwbuf.typeflag != BFTW_DIR) {
		goto done;
	}

	// Now start the breadth-first search

	if (bftw_push(&state, path) != 0) {
		goto done;
	}

	while (state.queue.head) {
		state.reader = bftw_queue_poll(&state.queue);
		struct bftw_reader *reader = state.reader;

		while (reader->de && reader->error == 0) {
			const char *name = reader->de->d_name;
			if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
				goto read;
			}

			switch (bftw_visit_path(&state)) {
			case BFTW_CONTINUE:
				break;

			case BFTW_SKIP_SIBLINGS:
				goto next;

			case BFTW_SKIP_SUBTREE:
				goto read;

			case BFTW_STOP:
				goto done;
			}

			if (state.ftwbuf.typeflag == BFTW_DIR) {
				const struct bfs_stat *statbuf = state.ftwbuf.statbuf;
				if ((flags & BFTW_XDEV)
				    && statbuf
				    && statbuf->dev != reader->dir->dev) {
					goto read;
				}

				if (bftw_push(&state, name) != 0) {
					goto done;
				}
			}

		read:
			bftw_reader_read(reader);
		}

	next:
		if (bftw_release_reader(&state, true) == BFTW_STOP) {
			goto done;
		}
	}

done:
	return bftw_state_destroy(&state);
}
