//
// ZapOS replacement for w_file.c/w_file_stdc.c.
//
// No filesystem access exists from this ELF program at all (no file-
// read syscall) -- instead, the shareware doom1.wad (the standard,
// freely-distributable one, md5 f0cefca49926d00903cf57551d901abe) is
// linked straight into this binary's own rodata via `ld -r -b binary`
// (see tools/make_disk_image.sh), and this file just treats that
// embedded blob as the one and only "file" W_OpenFile() can ever
// return -- the requested path is ignored entirely, since there's
// nothing else it could resolve to.

#include "w_file.h"
#include "doomtype.h"
#include <string.h>

extern unsigned char _binary_doom1_wad_start[];
extern unsigned char _binary_doom1_wad_end[];

static wad_file_class_t zapos_wad_file_class;
static wad_file_t zapos_wad_file;
static int opened_once = 0;

static size_t ZapOS_Read(wad_file_t *file, unsigned int offset, void *buffer, size_t buffer_len) {
    if (offset >= file->length) return 0;
    size_t avail = file->length - offset;
    size_t n = buffer_len < avail ? buffer_len : avail;
    memcpy(buffer, file->mapped + offset, n);
    return n;
}

static void ZapOS_CloseFile(wad_file_t *file) {
    (void)file;
}

static wad_file_t *ZapOS_OpenFile(char *path) {
    (void)path;
    if (opened_once) return NULL; /* only one IWAD is ever opened on this port */
    opened_once = 1;

    zapos_wad_file_class.OpenFile = ZapOS_OpenFile;
    zapos_wad_file_class.CloseFile = ZapOS_CloseFile;
    zapos_wad_file_class.Read = ZapOS_Read;

    zapos_wad_file.file_class = &zapos_wad_file_class;
    zapos_wad_file.mapped = _binary_doom1_wad_start;
    zapos_wad_file.length = (unsigned int)(_binary_doom1_wad_end - _binary_doom1_wad_start);
    return &zapos_wad_file;
}

wad_file_t *W_OpenFile(char *path) {
    return ZapOS_OpenFile(path);
}

void W_CloseFile(wad_file_t *wad) {
    wad->file_class->CloseFile(wad);
}

size_t W_Read(wad_file_t *wad, unsigned int offset, void *buffer, size_t buffer_len) {
    return wad->file_class->Read(wad, offset, buffer, buffer_len);
}
