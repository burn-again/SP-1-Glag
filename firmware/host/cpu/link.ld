MEMORY { FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 1024K
         RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 256K }
_estack = ORIGIN(RAM) + LENGTH(RAM);
SECTIONS {
  .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) } > FLASH
  _sidata = LOADADDR(.data);
  .data : { _sdata = .; *(.data*) _edata = .; } > RAM AT > FLASH
  .bss (NOLOAD) : { _sbss = .; *(.bss*) *(COMMON) _ebss = .; } > RAM
}
