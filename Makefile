############################################################################
# bfs                                                                      #
# Copyright (C) 2015-2018 Tavian Barnes <tavianator@tavianator.com>        #
#                                                                          #
# Permission to use, copy, modify, and/or distribute this software for any #
# purpose with or without fee is hereby granted.                           #
#                                                                          #
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES #
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF         #
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR  #
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES   #
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN    #
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF  #
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.           #
############################################################################

ifeq ($(wildcard .git),)
VERSION := 1.2.4
else
VERSION := $(shell git describe --always)
endif

ifndef OS
OS := $(shell uname)
endif

CC ?= gcc
INSTALL ?= install
MKDIR ?= mkdir -p
RM ?= rm -f

WFLAGS ?= -Wall -Wmissing-declarations
CFLAGS ?= -g $(WFLAGS)
LDFLAGS ?=
DEPFLAGS ?= -MD -MP -MF $(@:.o=.d)

DESTDIR ?=
PREFIX ?= /usr

LOCAL_CPPFLAGS := \
    -D__EXTENSIONS__ \
    -D_ATFILE_SOURCE \
    -D_BSD_SOURCE \
    -D_DARWIN_C_SOURCE \
    -D_DEFAULT_SOURCE \
    -D_FILE_OFFSET_BITS=64 \
    -D_GNU_SOURCE \
    -DBFS_VERSION=\"$(VERSION)\"

LOCAL_CFLAGS := -std=c99
LOCAL_LDFLAGS :=
LOCAL_LDLIBS :=

ifeq ($(OS),Linux)
LOCAL_LDFLAGS += -Wl,--as-needed
LOCAL_LDLIBS += -lacl -lcap -lrt
endif

ALL_CPPFLAGS = $(LOCAL_CPPFLAGS) $(CPPFLAGS)
ALL_CFLAGS = $(ALL_CPPFLAGS) $(LOCAL_CFLAGS) $(CFLAGS) $(DEPFLAGS)
ALL_LDFLAGS = $(ALL_CFLAGS) $(LOCAL_LDFLAGS) $(LDFLAGS)
ALL_LDLIBS = $(LOCAL_LDLIBS) $(LDLIBS)

all: bfs

bfs: \
    bftw.o \
    color.o \
    diag.o \
    dstring.o \
    eval.o \
    exec.o \
    main.o \
    mtab.o \
    opt.o \
    parse.o \
    posix1e.o \
    printf.o \
    spawn.o \
    stat.o \
    typo.o \
    util.o
	$(CC) $(ALL_LDFLAGS) $^ $(ALL_LDLIBS) -o $@

sanitized: CFLAGS := -g $(WFLAGS) -fsanitize=address -fsanitize=undefined
sanitized: bfs

release: CFLAGS := -g $(WFLAGS) -O3 -flto -DNDEBUG
release: bfs

%.o: %.c
	$(CC) $(ALL_CFLAGS) -c $< -o $@

check: bfs
	./tests.sh --bfs="$(realpath $<)"
	./tests.sh --bfs="$(realpath $<) -dfs"

clean:
	$(RM) bfs *.o *.d

install:
	$(MKDIR) $(DESTDIR)$(PREFIX)/bin
	$(INSTALL) -m755 bfs $(DESTDIR)$(PREFIX)/bin/bfs
	$(MKDIR) $(DESTDIR)$(PREFIX)/share/man/man1
	$(INSTALL) -m644 bfs.1 $(DESTDIR)$(PREFIX)/share/man/man1/bfs.1

uninstall:
	$(RM) $(DESTDIR)$(PREFIX)/bin/bfs
	$(RM) $(DESTDIR)$(PREFIX)/share/man/man1/bfs.1

.PHONY: all release check clean install uninstall

-include $(wildcard *.d)
