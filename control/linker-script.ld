#include <zephyr/arch/arm/cortex_m/scripts/linker.ld>

/** Bootloader expects even sized images. Padding it as below ensures we  always create even binary file size*/
SECTIONS
{
  
  .ocram_nocache (NOLOAD) : ALIGN(4)
  {
      KEEP(*(.ocram_nocache))
  } > OCRAM_NOCACHE

  .ocram_cached (NOLOAD) : ALIGN(4)
  {
      KEEP(*(.ocram_cached))
  } > OCRAM_CACHED

  .padding : ALIGN(8)
  {   
      FILL(0xff)
      LONG(0xFFFFFFFF) ;  
  } > FLASH
}
