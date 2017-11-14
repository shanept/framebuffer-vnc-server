AC_INIT([framebuffer-vnc-server], [0])
AC_PREREQ([2.68])
AC_CONFIG_SRCDIR([src/])
AC_CONFIG_AUX_DIR([build-aux])

AC_SEARCH_LIBS([rfbGetScreen], [libvncserver vncserver], [], [
  AC_MSG_ERROR([LibVNC dependency missing!])
])

AM_INIT_AUTOMAKE([1.11 -Wall -Wno-portability subdir-objects foreign])
AC_CONFIG_FILES([Makefile])
AC_PROG_CC
AC_OUTPUT
