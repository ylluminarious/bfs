/****************************************************************************
 * bfs                                                                      *
 * Copyright (C) 2016-2017 Tavian Barnes <tavianator@tavianator.com>        *
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

#include "dstring.h"
#include <stdlib.h>
#include <string.h>

/**
 * The memory representation of a dynamic string.  Users get a pointer to data.
 */
struct dstring {
	size_t capacity;
	size_t length;
	char data[];
};

static struct dstring *dstrheader(const char *dstr) {
	return (struct dstring *)(dstr - offsetof(struct dstring, data));
}

static size_t dstrsize(size_t capacity) {
	return sizeof(struct dstring) + capacity + 1;
}

char *dstralloc(size_t capacity) {
	struct dstring *header = malloc(dstrsize(capacity));
	if (!header) {
		return NULL;
	}

	header->capacity = capacity;
	header->length = 0;
	return header->data;
}

size_t dstrlen(const char *dstr) {
	return dstrheader(dstr)->length;
}

int dstreserve(char **dstr, size_t capacity) {
	struct dstring *header = dstrheader(*dstr);

	if (capacity > header->capacity) {
		capacity *= 2;

		header = realloc(header, dstrsize(capacity));
		if (!header) {
			return -1;
		}
		header->capacity = capacity;

		*dstr = header->data;
	}

	return 0;
}

int dstresize(char **dstr, size_t length) {
	if (dstreserve(dstr, length) != 0) {
		return -1;
	}

	struct dstring *header = dstrheader(*dstr);
	header->length = length;
	header->data[length] = '\0';

	return 0;
}

static int dstrcat_impl(char **dest, const char *src, size_t srclen) {
	size_t oldlen = dstrlen(*dest);
	size_t newlen = oldlen + srclen;

	if (dstresize(dest, newlen) != 0) {
		return -1;
	}

	memcpy(*dest + oldlen, src, srclen);
	return 0;
}

int dstrcat(char **dest, const char *src) {
	return dstrcat_impl(dest, src, strlen(src));
}

int dstrncat(char **dest, const char *src, size_t n) {
	return dstrcat_impl(dest, src, strnlen(src, n));
}

int dstrapp(char **str, char c) {
	return dstrcat_impl(str, &c, 1);
}

void dstrfree(char *dstr) {
	if (dstr) {
		free(dstrheader(dstr));
	}
}
