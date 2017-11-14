bin_PROGRAMS = vnc_server
vnc_server_SOURCES = src/framebuffer-vnc-server.c

GIT_COMMIT = $(shell git log -1 --format=%h)
GIT_BRANCH = $(shell git rev-parse --abbrev-ref HEAD)

AM_CFLAGS = -DGIT_BRANCH=\"$(GIT_BRANCH)\" -DGIT_COMMIT_HASH=\"$(GIT_COMMIT)\"
