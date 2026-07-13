#ifndef GUI_SHELL_H
#define GUI_SHELL_H

#include <stdint.h>

/* Executes one shell command line against `*cwd` (a FAT32 directory
 * cluster -- cd/mkdir-into navigation update it in place), writing all
 * output through `out` (called with arbitrary chunks of text, not
 * necessarily line-terminated -- callers just append verbatim to their
 * own scrollback buffer). Returns the pid of a program a "run"/bare-
 * ".ELF" command just launched, so the caller can route that task's own
 * SYS_WRITE output back through the same `out` sink (see
 * gui/compositor.c's terminal_route_output()); -1 if this command
 * didn't launch one. */
int shell_execute(const char *cmdline, uint32_t *cwd, void (*out)(const char *));

#endif
