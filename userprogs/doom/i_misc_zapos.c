/* Stubs for the timer/joystick/statdump/endoom interfaces whose real
 * implementations (i_timer.c, i_joystick.c, statdump.c, i_endoom.c)
 * were excluded from this port -- ZapOS has no joystick, no ENDOOM
 * text mode, and no -statdump output file to write. The timer
 * functions are the only ones with real behavior, backed by the
 * kernel's SYS_GET_TICKS/SYS_SLEEP syscalls (100Hz PIT ticks). */

#include "doomtype.h"
#include "d_player.h"
#include "i_timer.h"
#include "i_joystick.h"
#include "statdump.h"
#include "i_endoom.h"
#include "zapos_syscalls.h"

int I_GetTimeMS(void) {
    return (int)(sys_get_ticks() * 10u);
}

int I_GetTime(void) {
    return (int)((sys_get_ticks() * 35u) / 100u);
}

void I_Sleep(int ms) {
    if (ms > 0) sys_sleep((unsigned int)ms);
}

void I_InitTimer(void) { }

void I_WaitVBL(int count) {
    sys_sleep((unsigned int)(count * 1000 / 70));
}

void I_InitJoystick(void) { }
void I_ShutdownJoystick(void) { }
void I_UpdateJoystick(void) { }
void I_BindJoystickVariables(void) { }

void I_Endoom(byte *data) { (void)data; }

void StatCopy(wbstartstruct_t *stats) { (void)stats; }
void StatDump(void) { }
