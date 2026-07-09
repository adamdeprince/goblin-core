#ifndef _JIMAUTOCONF_H
#define _JIMAUTOCONF_H
/* Portable, hand-written minimal config for the embedded Jim Tcl subset used by
 * Goblin Core's TCL.* commands. Written by hand (not autosetup-generated) so the
 * vendored copy builds identically on Linux and macOS with no configure step.
 * Only features the vendored core references are enabled; platform-divergent
 * probes are deliberately left undefined so Jim takes its portable fallback. */
#define HAVE_UNISTD_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_LONG_LONG 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_ISASCII 1
#define HAVE_UMASK 1
#define HAVE_MKSTEMP 1
#define HAVE_DECL_ISINF 1
#define HAVE_DECL_ISNAN 1
#define SIZEOF_INT 4
#define JIM_REGEXP 1
#define JIM_REFERENCES 1
#define JIM_VERSION 84
#if defined(__linux__)
#define TCL_PLATFORM_OS "linux"
#elif defined(__APPLE__)
#define TCL_PLATFORM_OS "darwin"
#else
#define TCL_PLATFORM_OS "unknown"
#endif
#define TCL_PLATFORM_PLATFORM "unix"
#define TCL_PLATFORM_PATH_SEPARATOR ":"
#define jim_ext_package 1
#define jim_ext_stdlib 1
#define jim_ext_tclcompat 1
#endif
