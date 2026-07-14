#ifndef KERNEL_POWER_H
#define KERNEL_POWER_H

/* Platform power controls used by the Nova control center and shell.
 * Both functions are noreturn on supported hardware/emulators. */
void power_reboot(void);
void power_shutdown(void);

#endif
