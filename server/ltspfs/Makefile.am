## Process this file with automake to produce Makefile.in

bin_PROGRAMS = ltspfs
ltspfs_SOURCES = ltspfs.c common.c common.h ltspfs.h
ltspfs_CFLAGS = -DFUSE_USE_VERSION=22 -D_REENTRANT -D_FILE_OFFSET_BITS=64
AM_CFLAGS = -Wall -W ${ltspfs_CFLAGS}
