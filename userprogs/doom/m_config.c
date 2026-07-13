//
// ZapOS replacement for m_config.c.
//
// The original implements a generic name->pointer config-variable
// registry plus default.cfg load/save, including a DEFAULT_FLOAT
// variant (mouse acceleration, mostly) that does real float
// arithmetic in its generic get/set/print paths. This kernel is built
// -mgeneral-regs-only (no FPU/SSE state exists at all), so that code
// simply cannot exist in the compiled output, reachable at runtime or
// not -- and since there's no file I/O syscall for a config file to
// load from or save to anyway, the whole subsystem reduces to no-ops:
// every caller just registers/queries variables that are never
// persisted, which is fine, since nothing on this port depends on a
// config file actually working.

#include "m_config.h"
#include "doomtype.h"

char *configdir = "";

void M_LoadDefaults(void) { }
void M_SaveDefaults(void) { }
void M_SaveDefaultsAlternate(char *main, char *extra) { (void)main; (void)extra; }

void M_SetConfigDir(char *dir) { (void)dir; configdir = ""; }
void M_SetConfigFilenames(char *main_config, char *extra_config) {
    (void)main_config; (void)extra_config;
}

void M_BindVariable(char *name, void *variable) { (void)name; (void)variable; }

boolean M_SetVariable(char *name, char *value) { (void)name; (void)value; return false; }
int M_GetIntVariable(char *name) { (void)name; return 0; }
const char *M_GetStrVariable(char *name) { (void)name; return NULL; }

char *M_GetSaveGameDir(char *iwadname) {
    (void)iwadname;
    return ""; /* fopen() always fails on this port anyway -- see doomlibc.c */
}
