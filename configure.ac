#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([hevcasm], [0.0], [hevcasm@parabolaresearch.com])
AC_CONFIG_SRCDIR([src/bin/main.c])
AC_CONFIG_HEADERS([config.h])
AM_INIT_AUTOMAKE
AC_CONFIG_MACRO_DIR([m4])

# Remember externally set CFLAGS
EXTERNAL_CFLAGS="$CFLAGS"

# Checks for programs. These may set default variables, such as CFLAGS
AC_PROG_CPP
AC_PROG_CC_C99
AC_PROG_RANLIB
AX_PROG_YASM

# Reset the externally set CFLAGS after calling AC_PROG*
CFLAGS="$EXTERNAL_CFLAGS"

# Use the C language and compiler for the following checks
AC_LANG([C]) 

# Checks for libraries.

# Checks for header files.
AC_HEADER_STDC
AC_CHECK_HEADERS([stdio.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_SIZE_T

# Add debug support
AC_ARG_ENABLE(debug,
  AS_HELP_STRING(
    [--enable-debug],
    [enable debugging, default: no]),
    [case "${enableval}" in
      yes) debug=true ;;
      no)  debug=false ;;
      *)   AC_MSG_ERROR([bad value ${enableval} for --enable-debug]) ;;
    esac],
    [debug=false])
AM_CONDITIONAL(DEBUG, test x"$debug" = x"true")
AM_COND_IF(DEBUG,
    AC_DEFINE(DEBUG, 1, [Define to 0 if this is a release build]),
    AC_DEFINE(DEBUG, 0, [Define to 1 or higher if this is a debug build]))

# Checks for library functions.

AC_CONFIG_FILES([Makefile
                 src/lib/Makefile
                 src/lib/f265/Makefile
                 src/bin/Makefile])
AC_OUTPUT

