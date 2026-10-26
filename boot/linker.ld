ENTRY(_start)

SECTIONS
{
    . = 0x40100000;
    __kernel_start = .;

    . = ALIGN(4096);
    __kernel_exec_start = .;

    .text :
    {
        KEEP(*(.text.boot))
        *(.text)
        KEEP(*(.text.user_entry))
    }

    . = ALIGN(2048);
    .vectors :
    {
        __vectors_start = .;
        KEEP(*(.text.vectors))
        __vectors_end = .;
    }

    . = ALIGN(4096);
    __kernel_exec_end = .;

    .rodata : ALIGN(4096)
    {
        *(.rodata)
        *(.rodata.*)
    }

    .data : ALIGN(4096)
    {
        *(.data)
        *(.data.*)
    }

    .bss (NOLOAD) : ALIGN(4096)
    {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(4096);
        __bss_end = .;
    }

    .stack (NOLOAD) : ALIGN(4096)
    {
        __stack_bottom = .;
        KEEP(*(.stack*))
        . = ALIGN(4096);
        __stack_top = .;
    }

    . = ALIGN(4096);
    __kernel_end = .;
    __heap_start = .;
}
