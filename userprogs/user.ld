/* Links a user program to load at the base of the fixed window
 * kernel/elf.c reserves for user programs (0x04000000-0x0F000000 --
 * see USER_LOAD_MIN/MAX there). Produces a static ET_EXEC, which is
 * all the loader accepts (no PIE/shared/relocatable support). */
ENTRY(_start)

SECTIONS
{
    . = 0x04000000;

    .text ALIGN(4K) : {
        *(.text)
    }

    .rodata ALIGN(4K) : {
        *(.rodata*)
    }

    .data ALIGN(4K) : {
        *(.data)
    }

    .bss ALIGN(4K) : {
        *(COMMON)
        *(.bss)
    }
}
