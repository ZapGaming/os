//
// ZapOS replacement for d_iwad.c.
//
// The original scans a list of real filesystem directories (env vars,
// argv[0]'s own directory, hardcoded Unix paths) looking for an IWAD
// file by name -- none of that applies here: there's exactly one IWAD,
// it's linked straight into this ELF binary's own rodata (see
// wad_data.c / w_file_zapos.c), and W_OpenFile() ignores whatever path
// string it's given anyway. So every one of these just needs to return
// something that satisfies its caller without doing real filesystem
// work. D_IdentifyVersion() (d_main.c) figures out gamemission/
// gamemode for real, by inspecting the WAD's actual lump names once
// it's open -- it doesn't trust D_FindIWAD's guess -- so getting that
// exactly right here doesn't matter.

#include "d_iwad.h"
#include "doomtype.h"

char *D_FindWADByName(char *filename) {
    (void)filename;
    return NULL; /* no extra/optional WADs on this port */
}

char *D_TryFindWADByName(char *filename) {
    return filename; /* passthrough -- W_OpenFile() ignores the string anyway */
}

char *D_FindIWAD(int mask, GameMission_t *mission) {
    (void)mask;
    *mission = doom;
    return "doom1.wad";
}

const iwad_t **D_FindAllIWADs(int mask) {
    (void)mask;
    return NULL;
}

char *D_SaveGameIWADName(GameMission_t gamemission) {
    (void)gamemission;
    return "doom";
}

char *D_SuggestIWADName(GameMission_t mission, GameMode_t mode) {
    (void)mission; (void)mode;
    return "doom1.wad";
}

char *D_SuggestGameName(GameMission_t mission, GameMode_t mode) {
    (void)mission; (void)mode;
    return "Doom";
}

void D_CheckCorrectIWAD(GameMission_t mission) {
    (void)mission;
}
