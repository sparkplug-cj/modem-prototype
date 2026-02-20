#include <zephyr/toolchain.h>

_ASM_FILE_PROLOGUE

GTEXT(__earlyInit)

SECTION_SUBSEC_FUNC(TEXT,_reset_section,__earlyInit)

/* 
 * This subroutine reconfigures the IOMUXC->GPR17 register to modify the FlexRAM regions,
 * and then changes IOMUXC->GPR16 register to use the GPR17 configuration rather than the eFuse configuration
 * See NXP AN 12077. Section 3.1.3 explains why this reconfiguration is done as soon as possible
 * 
 * This subroutine should be called with a bl (branch and link) instruction
 * so that the `bx lr` instruction will result in the resumption of initialisation
 */

ldr r0, =0x400AC000 // Base Address of IOMUXC-Control GPR registers

/* As detailed in Section 3.1.3.1, it is recommended that GPR17 is configured before GPR16. */

ldr r1, =0x5affaaaa // 64K OCRAM, 128K ITCM, 320K DTCM
str r1, [r0, #0x44] // Load r1 value into 0x400AC044 (IOMUXC->GPR17)

ldr r1, [r0, #0x40] // Load value of IOMUXC->GPR16 into r1 
orr r1, r1,#0x04    // Set bit 2 of r1
str r1, [r0, #0x40] // Load value of r1 into IOMUXC->GPR16 

bx lr               // Jump back to callee of __earlyInit
