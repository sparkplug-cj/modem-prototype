
board_runner_args(jlink "--device=MIMXRT1052xxxxB"
                        "--iface=SWD"
                        "--speed=auto"
                        "--reset-after-load"
                        "--loader=BankAddr=0x60000000&Loader=FlexSpiOctal"
                        # Customisation needed to handle external flash chip reset.
                        "--tool-opt=-jlinkscriptfile ${WORKSPACE_ROOT_DIR}/.config/control.jlinkscript"
                        # From MCUXpresso debug configuration. 
                        "--gdb-server-opt=-endian little"
                        "--gdb-server-opt=-vd"
                        "--gdb-server-opt=-nohalt"
                        "--gdb-server-opt=-nosilent"
                        "--gdb-server-opt=-noir"
                 )

include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)
