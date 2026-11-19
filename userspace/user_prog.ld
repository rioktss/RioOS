ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
    tls PT_TLS;
}

SECTIONS
{
    . = 0x100000000;

    .text : ALIGN(0x1000)
    {
        *(.text*)
    } :text

    . = ALIGN(0x1000);
    .rodata :
    {
        *(.rodata*)
    } :rodata

    . = ALIGN(0x1000);
    .tdata :
    {
        __tdata_start = .;
        *(.tdata .tdata.*)
        __tdata_end = .;
    } :data :tls

    .tbss :
    {
        *(.tbss .tbss.*)
        *(.tcommon)
    } :tls

    __tls_memsz = ABSOLUTE(ADDR(.tbss) + SIZEOF(.tbss) - ADDR(.tdata));
    __tls_align = ABSOLUTE(MAX(ALIGNOF(.tdata), ALIGNOF(.tbss)));

    .data :
    {
        *(.data*)
    } :data

    . = ALIGN(0x1000);
    .bss :
    {
        *(.bss*)
        *(COMMON)
    } :data
}
