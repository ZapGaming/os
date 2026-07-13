#ifndef ZAPOS_SHIM_UNISTD_H
#define ZAPOS_SHIM_UNISTD_H

int isatty(int fd);
int access(const char *path, int mode);
char *getcwd(char *buf, unsigned long size);
int unlink(const char *path);

#define F_OK 0
#define R_OK 4
#define W_OK 2

#endif
