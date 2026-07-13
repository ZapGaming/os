//
// doomgeneric platform layer for ZapOS.
//
// The actual glue is tiny, by design -- doomgeneric exists precisely
// so a port only has to implement these six functions. Rendering goes
// through SYS_BLIT (kernel/syscall.c copies the buffer into a kernel-
// owned staging area and puts the GUI into fullscreen-takeover mode --
// see gui/compositor.c), input through SYS_POLL_KEY (fed by a raw
// scancode queue in drivers/keyboard.c), and timing through
// SYS_GET_TICKS/SYS_SLEEP (the kernel's own 100Hz PIT).

#include "doomgeneric.h"
#include "zapos_syscalls.h"

/* Doom's own key-event queue -- doomgeneric_Tick() drains DG_GetKey()
 * every tick, but I_StartTic() (i_video_zapos.c) already drains
 * SYS_POLL_KEY into real D_PostEvent() calls every tic, which is the
 * path the game logic actually uses. DG_GetKey() itself is really
 * only meaningful for ports with no other input path; wire it to the
 * same syscall so it's still correct if anything calls it directly. */

void DG_Init(void) {
    sys_write("doomgeneric_zapos: DG_Init\n");
}

void DG_DrawFrame(void) {
    sys_blit(DG_ScreenBuffer);
}

void DG_SleepMs(uint32_t ms) {
    sys_sleep(ms);
}

uint32_t DG_GetTicksMs(void) {
    return sys_get_ticks() * 10; /* kernel PIT runs at 100Hz */
}

int DG_GetKey(int *pressed, unsigned char *doomKey) {
    int packed = sys_poll_key();
    if (packed < 0) return 0;
    *pressed = (packed >> 8) & 1;
    *doomKey = (unsigned char)(packed & 0xFF);
    return 1;
}

void DG_SetWindowTitle(const char *title) {
    (void)title; /* no window chrome in fullscreen takeover mode */
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    static char *fake_argv[] = { "doom", 0 };
    doomgeneric_Create(1, fake_argv);
    for (;;) {
        doomgeneric_Tick();
    }
}

/* The ELF loader jumps straight to this entry point with no C runtime
 * set up beforehand (see userprogs/hello.c/evil.c for the same pattern)
 * -- there's no argc/argv to receive for real, so main() just gets
 * fake ones above. Falling off the end of main() can't actually happen
 * (the Tick() loop never returns), but sys_exit() here means it fails
 * safe instead of running off into whatever follows in memory. */
void _start(void) {
    main(0, 0);
    sys_exit();
}
