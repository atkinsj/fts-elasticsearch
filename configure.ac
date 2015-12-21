#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.53])
AC_INIT([fts-elasticsearch], [0.1.0], [https://github.com/ascendantcom/fts-elasticsearch])
AM_INIT_AUTOMAKE([no-define foreign tar-ustar])
AC_OUTPUT(Makefile src/Makefile)
AC_CONFIG_SRCDIR([src])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_HEADERS([dummy-config.h])
AM_MAINTAINER_MODE

# Don't clobber Dovecot package versions
AC_DEFINE_UNQUOTED(FTS_ELASTICSEARCH_NAME, "$PACKAGE_NAME",
        [Define to the full name of fts-elasticsearch for Dovecot.])
AC_DEFINE_UNQUOTED(FTS_ELASTICSEARCH_VERSION, "$PACKAGE_VERSION",
        [Define to the version of fts-elasticsearch for Dovecot.])

# Couple with Dovecot
#

DC_DOVECOT
DC_DOVECOT_MODULEDIR
LIBDOVECOT_INCLUDE="$LIBDOVECOT_INCLUDE $LIBDOVECOT_STORAGE_INCLUDE"
CFLAGS="$DOVECOT_CFLAGS -I\$(top_srcdir)"
LIBS="$DOVECOT_LIBS"
AC_SUBST(LIBDOVECOT_INCLUDE)
# Allow shared objects
LT_INIT
AC_ENABLE_SHARED
AC_DISABLE_STATIC

# Checks for programs.
AC_PROG_CC

# Checks for libraries.
AC_SEARCH_LIBS([json_object_from_file], [json json-c], [], [ AC_MSG_ERROR([no json-c available]) ])
AC_SEARCH_LIBS([json_object_object_get_ex], [json json-c], [ AC_DEFINE([JSON_HAS_GET_EX], [1], [Define if we have json_object_object_get_ex])], [])

# Checks for header files.
AC_CHECK_HEADERS([inttypes.h stdlib.h syslog.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_SIZE_T
AC_TYPE_UINT32_T

# Checks for library functions.
AC_CHECK_FUNCS([memset])

AC_OUTPUT

echo
echo "Install prefix       : $prefix"
echo "Lib install prefix   : $dovecot_pkglibdir"
