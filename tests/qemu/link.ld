/* SPDX-License-Identifier: Apache-2.0
 * Part of rewind -- deterministic record & replay for bare-metal firmware.
 *
 * Memory map of QEMU's mps2-an386 (Cortex-M4): code RAM at 0, SRAM at 0x20000000. */
ENTRY(reset_handler)
MEMORY {
  FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 1M
  RAM   (rwx) : ORIGIN = 0x20000000, LENGTH = 256K
}
SECTIONS {
  .text : { KEEP(*(.vectors)) *(.text*) *(.rodata*) } > FLASH
  .data : { _sdata = .; *(.data*) _edata = .; } > RAM AT > FLASH
  _sidata = LOADADDR(.data);
  .bss  : { _sbss = .; *(.bss*) *(COMMON) _ebss = .; } > RAM
  _estack = ORIGIN(RAM) + LENGTH(RAM);
}
