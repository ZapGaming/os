ENTRY(_start)

SECTIONS
{
    . = 1M;
    kernel_start = .;

    .multiboot ALIGN(8) : {
        *(.multiboot_header)
    }

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

    kernel_end = .;
}
