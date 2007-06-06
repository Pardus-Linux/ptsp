#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.59)
AC_INIT([ltspfsd], [0.2], [sbalneav@ltsp.org], [ltspfsd])
AM_INIT_AUTOMAKE([ltspfsd], [0.2], [sbalneav@ltsp.org], [ltspfsd])


# Checks for programs.
AC_PROG_CC

dnl add x11 to the env
AC_PATH_X
AC_PATH_XTRA

LIBS="$LIBS $X_LIBS"
LDFLAGS="$LDFLAGS $LIBS $X_PRE_LIBS"

# Checks for libraries.

AC_CHECK_LIB([X11], [XOpenDisplay])

# Checks for header files.
AC_HEADER_DIRENT
AC_HEADER_STDC
AC_HEADER_SYS_WAIT
AC_CHECK_HEADERS([arpa/inet.h fcntl.h netinet/in.h stdlib.h string.h sys/socket.h sys/statfs.h syslog.h unistd.h utime.h X11/Xlib.h X11/Xauth.h])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
