#ifndef ZAPOS_SHIM_SYS_STAT_H
#define ZAPOS_SHIM_SYS_STAT_H

/* No real filesystem access from this ELF program at all -- mkdir()
 * is a no-op (see doomlibc.c); nothing on this port needs a directory
 * to actually exist, since every "file" operation (config, saves,
 * screenshots) already fails or discards regardless. */
int mkdir(const char *path, int mode);

#endif
