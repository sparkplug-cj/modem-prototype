/*
 * This file redefines weakly defined routines in Zephyr's soc code for
 * the iMX 1050 where necessary (i.e. order of execution must be kept
 * but certain parts of it need to be modified)
 */

/*
  * CHANGELOG
  * Maintain a list of changes made to the original Zephyr code here so
  * that maintaining parity with the original Zephyr SoC code is easy

  * 17 MAR 2025
  * - Hardcode clock_init rather than relying on nxp-zephyr clock_init function
  *   Rationale: NXP-Zephyr clock_init function used a mix of device-tree and hard-coded values.
  *              Tailoring this to our MIMXRT1052CVL5B is required to ensure it is driven to the specification
  */

// Below code must be kept in sync with Zephyr path:
// soc/nxp/imxrt/imxrt10xx/soc.c
// Some constants are global, and hence not redefined here.
#include <fsl_clock.h>
#include <zephyr/cache.h>
#include <zephyr/linker/linker-defs.h>
#ifdef CONFIG_NXP_IMXRT_BOOT_HEADER
#include <fsl_flexspi_nor_boot.h>
#endif
#if CONFIG_USB_DC_NXP_EHCI
#include "usb.h"
#include "usb_phy.h"
#endif

#if CONFIG_USB_DC_NXP_EHCI
/* USB PHY configuration */
#define BOARD_USB_PHY_D_CAL     (0x0CU)
#define BOARD_USB_PHY_TXCAL45DP (0x06U)
#define BOARD_USB_PHY_TXCAL45DM (0x06U)
#endif

#define EXT_OSC_FREQ_HZ 24000000U
#define EXT_RTC_FREQ_HZ 32768U

#define PLL_ARM_DIVIDER        (88U) ///< ARM PLL:                     1056MHz                 Ref Man. 14.6.1.3.1
#define ARM_PODF               (1U)  ///< Divider of 2                                         Ref Man. 14.7.4
#define SYS_PLL_BYPASS_CLK_SEL (0U)  ///< SYS_PLL_BYPASS_CLK_ROOT_SRC: OSC_CLK                 Ref Man. 14.8.4
#define PRE_PERIPH_CLK_SEL     (3U)  ///< PRE_PERIPH_CLK_ROOT_SRC:     divided PLL1            Ref Man. 14.7.6
#define PERIPH_CLK2_SEL        (1U)  ///< PERIPH_CLK2_ROOT_SRC:        osc_clk(pll1_ref_clk)   Ref Man. 14.7.6
#define PERIPH_CLK2_PODF       (0U)  ///< Divider of 1                                         Ref Man. 14.7.5
#define PERIPH_CLK_SEL         (0U)  ///< PERIP_CLK_ROOT_SRC:          pre_periph_clk_sel      Ref Man. 14.7.5
#define AHB_PODF               (0U)  ///< Divider of 1                                         Ref Man. 14.7.5
#define IPG_PODF               (3U)  ///< Divider of 4                                         Ref Man. 14.7.5
#define PERCLK_CLK_SEL         (0U)  ///< PERCLK_CLK_ROOT_SRC:         ipg clk root            Ref Man. 14.7.7
#define PERCLK_PODF            (1U)  ///< Divider of 2                                         Ref Man. 14.7.7
#define FLEXSPI_CLK_SEL        (3U)  ///< FLEXSPI_CLK_ROOT_SRC:        PLL3 PFD0               Ref Man. 14.7.7
#define LPSPI_CLK_SEL          (2U)  ///< LPSPI_CLK_ROOT_SRC:          PLL2 main               Ref Man. 14.7.6
#define LPSPI_CLK_PODF         (7U)  ///< Divider of 8                                         Ref Man. 14.7.6
#define LPI2C_CLK_SEL          (0U)  ///< LPI2C_CLK_ROOT_SRC:          PLL3 60MHz              Ref Man. 14.7.13
#define LPI2C_CLK_PODF         (5U)  ///< Divider of 6                                         Ref Man. 14.7.13
#define UART_CLK_SEL           (0U)  ///< UART_CLK_ROOT_SRC:           PLL3 80MHz              Ref Man. 14.7.9
#define UART_CLK_PODF          (0U)  ///< Divider of 1                                         Ref Man. 14.7.9

#ifdef CONFIG_INIT_ARM_PLL
/* ARM PLL configuration for RUN mode
 * Caution: IMXRT1052XXXXB max CPU speed at Full Speed RUN mode is 528MHz
 * Adjust PLl1 (ARM PLL) loop divider accordingly.
 * See Table 13-1 in Ref Man. 13.4.1.7
 */
static const clock_arm_pll_config_t ARM_PLL_CONFIG = { .loopDivider = PLL_ARM_DIVIDER };
#endif

#ifdef CONFIG_INIT_SYS_PLL
/* Configure System PLL */
// The parameters are hardcoded as Ref Man. 14.6.1.3.2 dictates that the SYS PLL is intended to be run at 528 MHz
static const clock_sys_pll_config_t sysPllConfig = {
  .loopDivider = 1,
  .numerator   = 0,
  .denominator = 1,
  .src         = SYS_PLL_BYPASS_CLK_SEL,
};
#endif


#if CONFIG_USB_DC_NXP_EHCI
extern usb_phy_config_struct_t usbPhyConfig;
#endif

/**
 * @brief Initialize the system clock
 * The order of these clock root checks are based on the layout in MCUXpresso's GUI
 * which provides easier visualisation of how the clocks are generated and what their
 * speeds are
 */
void clock_init(void)
{
  if (((CCM_ANALOG->PLL_ARM & CCM_ANALOG_PLL_ARM_CLR_DIV_SELECT_MASK) == CCM_ANALOG_PLL_ARM_DIV_SELECT(PLL_ARM_DIVIDER))
      && ((CCM->CBCMR & CCM_CBCMR_PRE_PERIPH_CLK_SEL_MASK) == CCM_CBCMR_PRE_PERIPH_CLK_SEL(PRE_PERIPH_CLK_SEL))
      && ((CCM->CBCMR & CCM_CBCMR_PERIPH_CLK2_SEL_MASK) == CCM_CBCMR_PERIPH_CLK2_SEL(PERIPH_CLK2_SEL))
      && ((CCM->CBCDR & CCM_CBCDR_PERIPH_CLK2_PODF_MASK) == CCM_CBCDR_PERIPH_CLK2_PODF(PERIPH_CLK2_PODF))
      && ((CCM->CBCDR & CCM_CBCDR_PERIPH_CLK_SEL_MASK) == CCM_CBCDR_PERIPH_CLK_SEL(PERIPH_CLK_SEL))
      && ((CCM->CBCDR & CCM_CBCDR_AHB_PODF_MASK) == CCM_CBCDR_AHB_PODF(AHB_PODF))
      && ((CCM->CBCDR & CCM_CBCDR_IPG_PODF_MASK) == CCM_CBCDR_IPG_PODF(IPG_PODF))
      && ((CCM->CSCMR1 & CCM_CSCMR1_PERCLK_CLK_SEL_MASK) == CCM_CSCMR1_PERCLK_CLK_SEL(PERCLK_CLK_SEL))
      && ((CCM->CSCMR1 & CCM_CSCMR1_PERCLK_PODF_MASK) == CCM_CSCMR1_PERCLK_PODF(PERCLK_PODF))
      && ((CCM->CSCMR1 & CCM_CSCMR1_FLEXSPI_CLK_SEL_MASK) == CCM_CSCMR1_FLEXSPI_CLK_SEL(FLEXSPI_CLK_SEL))
      && ((CCM->CBCMR & CCM_CBCMR_LPSPI_CLK_SEL_MASK) == CCM_CBCMR_LPSPI_CLK_SEL(LPSPI_CLK_SEL))
      && ((CCM->CBCMR & CCM_CBCMR_LPSPI_PODF_MASK) == CCM_CBCMR_LPSPI_PODF(LPSPI_CLK_PODF))
      && ((CCM->CSCDR2 & CCM_CSCDR2_LPI2C_CLK_SEL_MASK) == CCM_CSCDR2_LPI2C_CLK_SEL(LPI2C_CLK_SEL))
      && ((CCM->CSCDR2 & CCM_CSCDR2_LPI2C_CLK_PODF_MASK) == CCM_CSCDR2_LPI2C_CLK_PODF(LPI2C_CLK_PODF))
      && ((CCM->CSCDR1 & CCM_CSCDR1_UART_CLK_SEL_MASK) == CCM_CSCDR1_UART_CLK_SEL(UART_CLK_SEL))
      && ((CCM->CSCDR1 & CCM_CSCDR1_UART_CLK_PODF_MASK) == CCM_CSCDR1_UART_CLK_PODF(UART_CLK_PODF)))
  {
    /* Boot ROM did initialize the XTAL, here we only sets external XTAL
	 * OSC freq. 
	 * Note: Other fsl_clock.h functions rely on the values of these variables
	 */
    CLOCK_SetXtalFreq(EXT_OSC_FREQ_HZ);
    CLOCK_SetRtcXtalFreq(EXT_RTC_FREQ_HZ);

    // Repeated here for app. as bootloader may not have Zephyr's USB driver enabled
#if CONFIG_USB_DC_NXP_EHCI
    CLOCK_EnableUsbhs0PhyPllClock(0, 0);
    CLOCK_EnableUsbhs0Clock(0, 0);
    USB_EhciPhyInit(kUSB_ControllerEhci0, 0, &usbPhyConfig);
#endif
  }
  // If any of the clocks are not configured, then explicitly configure clocks in the correct order
  else
  {
    /* Boot ROM did initialize the XTAL, here we only sets external XTAL
	 * OSC freq
	 */
    CLOCK_SetXtalFreq(EXT_OSC_FREQ_HZ);
    CLOCK_SetRtcXtalFreq(EXT_RTC_FREQ_HZ);

    /* Set PERIPH_CLK2 MUX to OSC */
    CLOCK_SetMux(kCLOCK_PeriphClk2Mux, 0x1);

    /* Set PERIPH_CLK MUX to PERIPH_CLK2 */
    CLOCK_SetMux(kCLOCK_PeriphMux, 0x1);

    /* Setting the VDD_SOC value.
	 */
    DCDC->REG3 = (DCDC->REG3 & (~DCDC_REG3_TRG_MASK)) | DCDC_REG3_TRG(CONFIG_DCDC_VALUE);
    /* Waiting for DCDC_STS_DC_OK bit is asserted */
    while (DCDC_REG0_STS_DC_OK_MASK != (DCDC_REG0_STS_DC_OK_MASK & DCDC->REG0))
    {
      ;
    }

#ifdef CONFIG_INIT_ARM_PLL
    CLOCK_InitArmPll(&ARM_PLL_CONFIG); /* Configure ARM PLL */
#endif
#ifdef CONFIG_INIT_SYS_PLL
    CLOCK_InitSysPll(&sysPllConfig);
#endif

    /* Set ARM PODF */
    CLOCK_SetDiv(kCLOCK_ArmDiv, ARM_PODF);
    /* Set AHB PODF */
    CLOCK_SetDiv(kCLOCK_AhbDiv, AHB_PODF);
    /* Set IPG PODF */
    CLOCK_SetDiv(kCLOCK_IpgDiv, IPG_PODF);

    /* Set PRE_PERIPH_CLK to divided PLL1*/
    CLOCK_SetMux(kCLOCK_PrePeriphMux, PRE_PERIPH_CLK_SEL);

    /* Set PERIPH_CLK MUX to PRE_PERIPH_CLK */
    CLOCK_SetMux(kCLOCK_PeriphMux, PERIPH_CLK_SEL);

    // PLLs and PFDs are now stabilised and configured

    CLOCK_SetMux(kCLOCK_UartMux, UART_CLK_SEL);
    CLOCK_SetDiv(kCLOCK_UartDiv, UART_CLK_PODF);

    CLOCK_SetMux(kCLOCK_Lpi2cMux, LPI2C_CLK_SEL);
    CLOCK_SetDiv(kCLOCK_Lpi2cDiv, LPI2C_CLK_PODF);

    CLOCK_SetMux(kCLOCK_LpspiMux, LPSPI_CLK_SEL);
    CLOCK_SetDiv(kCLOCK_LpspiDiv, LPSPI_CLK_PODF);
#if CONFIG_USB_DC_NXP_EHCI
    CLOCK_EnableUsbhs0PhyPllClock(0, 0);
    CLOCK_EnableUsbhs0Clock(0, 0);
    USB_EhciPhyInit(kUSB_ControllerEhci0, 0, &usbPhyConfig);
#endif
    /* Keep the system clock running so SYSTICK can wake up the system from
	 * wfi.
	 */
    CLOCK_SetMode(kCLOCK_ModeRun);
  }
}
