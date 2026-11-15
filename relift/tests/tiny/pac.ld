ENTRY(_start)
SECTIONS {
  . = 0;
  .text : { *(.text.start) *(.text*) }
  . = 0x1000;
  .rodata : { *(.rodata*) }
  data_start = 0x2000;
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.data*) *(.bss*) }
}
