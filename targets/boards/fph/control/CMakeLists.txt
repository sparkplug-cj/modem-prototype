set(RT1050_BOARD_DIR "${CMAKE_CURRENT_LIST_DIR}/imx1050")

zephyr_library_sources_ifdef(CONFIG_EARLY_INIT ${RT1050_BOARD_DIR}/early_init.S)
zephyr_library_sources(${RT1050_BOARD_DIR}/soc.c)

if(CONFIG_NXP_IMXRT_BOOT_HEADER)
  zephyr_library()

  if(CONFIG_BOARD_REVISION_A4)
    set(FLASH_CONF IS25WX064_iMX1050_FlexSPI_NOR_config.c)
  endif()

  if(CONFIG_BOOT_FLEXSPI_NOR)
    # Include flash configuration block for RT1050 EVK from NXP's HAL.
    # This configuration block may need modification if another flash chip is
    # used on your custom board. See NXP AN12238 for more information.
    zephyr_compile_definitions(XIP_BOOT_HEADER_ENABLE=1)
    zephyr_compile_definitions(BOARD_FLASH_SIZE=CONFIG_FLASH_SIZE*1024)
    zephyr_library_sources(${RT1050_BOARD_DIR}/xip/${FLASH_CONF})
    zephyr_library_include_directories(${RT1050_BOARD_DIR}/xip)
  endif()

  if(CONFIG_DEVICE_CONFIGURATION_DATA)
    # DCD is excluded for this board
    # Include device configuration data block for RT1050 EVK from NXP's HAL.
    # This configuration block may need modification if another SDRAM chip
    # is used on your custom board.
    zephyr_compile_definitions(XIP_BOOT_HEADER_DCD_ENABLE=0)
    zephyr_library_sources(${RT1050_BOARD_DIR}/dcd.c)
  endif()
endif()

if(CONFIG_MCUX_GPT_TIMER)
  message(WARNING "You appear to be using the GPT hardware timer. "
    "This timer will enable lower power modes, but at the cost of reduced "
    "hardware timer resolution")
endif()
