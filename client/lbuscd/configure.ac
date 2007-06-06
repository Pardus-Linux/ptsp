AC_PREREQ(2.59)
AC_INIT("lbuscd", "0.1", "jam@mcquil.com")
AM_INIT_AUTOMAKE("lbuscd", "0.1", "jam@mcquil.com")
AC_CONFIG_SRCDIR([lbuscd.c])

# Checks for programs.
AC_PROG_CC

# Checks for libraries.
# FIXME: Replace `main' with a function in `-lpopt':
AC_CHECK_LIB([popt], [poptGetContext])

# Checks for header files.
AC_HEADER_STDC
AC_HEADER_SYS_WAIT
AC_CHECK_HEADERS([arpa/inet.h fcntl.h netdb.h netinet/in.h stdlib.h string.h sys/ioctl.h sys/socket.h syslog.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_TYPE_OFF_T
AC_HEADER_TIME

# Checks for library functions.
AC_FUNC_FORK
AC_PROG_GCC_TRADITIONAL
AC_FUNC_MALLOC
AC_FUNC_REALLOC
AC_FUNC_SELECT_ARGTYPES
AC_CHECK_FUNCS([inet_ntoa memset mkfifo select socket strcasecmp strerror])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
