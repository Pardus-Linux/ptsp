#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.59)
AC_INIT(ltspfs, 0.3, sbalneav@ltsp.org)
AM_INIT_AUTOMAKE(ltspfs, 0.3, sbalneav@ltsp.org)
AC_CONFIG_SRCDIR([common.c])

# Checks for programs.
AC_PROG_CC

# Checks for libraries.
AC_CHECK_LIB([fuse], [fuse_main])
AC_CHECK_LIB([pthread], [pthread_create])

# Checks for header files.
AC_HEADER_DIRENT
AC_HEADER_STDC
AC_CHECK_HEADERS([arpa/inet.h fcntl.h netdb.h netinet/in.h stdlib.h string.h sys/socket.h sys/statfs.h unistd.h fuse.h])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
