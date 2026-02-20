/*
 * The Clear BSD License
 * Copyright 2017 NXP
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted (subject to the limitations in the disclaimer
 * below) provided that the following conditions are met:
 *
 * o Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * o Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * o Neither the name of the copyright holder nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE GRANTED BY
 * THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
 * CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT
 * NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* 
 * This is a Serial NOR Configuration Block definition for the ISSI
 * IS25WX064 XiP flash based on datasheet Rev. A1 (07 JUL 2023). 
 * 
 * Among other parameters it configures the system to operate with:
 * - Octal-SPI
 * - Double Data Rate (DDR)
 * - SCLK = 133MHz
 */

#include "imxrt1050_flexspi_nor_config.h"

/*******************************************************************************
 * Code
 ******************************************************************************/
#if defined(XIP_BOOT_HEADER_ENABLE) && (XIP_BOOT_HEADER_ENABLE == 1)
#if defined(__CC_ARM) || defined(__GNUC__)
__attribute__((section(".boot_hdr.conf")))
#elif defined(__ICCARM__)
#pragma location = ".boot_hdr.conf"
#endif

#define NUM_DUMMY_CYCLES 0x11 // Dummy cycles for DDR mode read commands. 12 cycles minimum for 133MHz.

const flexspi_nor_config_t ISSI_WX064_CONFIG = { 
    .memConfig =
        {
            .tag = FLEXSPI_CFG_BLK_TAG,
            .version = FLEXSPI_CFG_BLK_VERSION,
            .readSampleClkSrc = kFlexSPIReadSampleClk_ExternalInputFromDqsPad, 
            .csHoldTime = 3U,
            .csSetupTime = 3U,
            .columnAddressWidth = 0U,
            // Used to tell FlexSPI peripheral to transmit configuration commands to flash first before normal execution
            .deviceModeCfgEnable = 1,
            .waitTimeCfgCommands = 1,
            // Sequence for changing device mode. In this sequence we write to
            // the volatile configuration register.
            .deviceModeSeq =
                {.seqId = 6,
                 .seqNum = 1},
            .deviceModeArg = 0xE7, // Enable Octal DDR
            // Enable DDR mode, Safe configuration
            .controllerMiscOption =
                (1U << kFlexSpiMiscOffset_DdrModeEnable) |
                (1U << kFlexSpiMiscOffset_SafeConfigFreqEnable),
            .deviceType = kFlexSpiDeviceType_SerialNOR, // serial NOR
            .sflashPadType = kSerialFlash_8Pads,        // Use A_DATA[0:3] & B_DATA[0:3]
            .serialClkFreq = kFlexSpiSerialClk_133MHz,
            .lutCustomSeqEnable =
                0, // No Custom LUT sequence is required for this chip
            .sflashA1Size = 8U * 1024U * 1024U, // 8MB = 64Mbit
            .sflashA2Size = 0U,
            .sflashB1Size = 0U,
            .sflashB2Size = 0U,
            .dataValidTime = {[0] = 15}, // 1.5ns from DQS to data. Minimum of 1.3ns in datasheet.
            .busyOffset = 0,             // busy bit in bit 0
            .busyBitPolarity = 0,        // busy bit is 1 when device is busy
            // LUT table CMDs, pin modes and configurations are derived from Command Set Summary in Flash datasheet
            // and FlexSPI LUT opcode descriptions.
            .lookupTable =
                {
                    // Read Array
                    [4 * NOR_CMD_LUT_SEQ_IDX_READ + 0] = FLEXSPI_LUT_SEQ(
                        CMD_SDR, FLEXSPI_8PAD, 0xFD, RADDR_DDR, FLEXSPI_8PAD, 0x20),
                    [4 * NOR_CMD_LUT_SEQ_IDX_READ + 1] = FLEXSPI_LUT_SEQ(
                        DUMMY_DDR, FLEXSPI_8PAD, NUM_DUMMY_CYCLES, READ_DDR, FLEXSPI_8PAD, 0x04),
                    // Write Enable
                    [4 * NOR_CMD_LUT_SEQ_IDX_WRITEENABLE + 0] = FLEXSPI_LUT_SEQ(
                        CMD_SDR, FLEXSPI_1PAD, 0x06, STOP, FLEXSPI_1PAD, 0x0),
                    // Write Volatile Control Register to enable Octal DDR
                    [4 * 6 + 0] = FLEXSPI_LUT_SEQ(CMD_SDR, FLEXSPI_1PAD, 0x81,
                                                  CMD_SDR, FLEXSPI_1PAD, 0x0),
                    [4 * 6 + 1] = FLEXSPI_LUT_SEQ(CMD_SDR, FLEXSPI_1PAD, 0x00,
                                                  CMD_SDR, FLEXSPI_1PAD, 0x00),
                    [4 * 6 + 2] = FLEXSPI_LUT_SEQ(WRITE_SDR, FLEXSPI_1PAD, 0x01,
                                                  STOP, FLEXSPI_1PAD, 0),
                    // Read Status
                    [4 * NOR_CMD_LUT_SEQ_IDX_READSTATUS + 0] = FLEXSPI_LUT_SEQ(
                        CMD_SDR, FLEXSPI_1PAD, 0x05, READ_DDR, FLEXSPI_1PAD, 0x04),
                    // Read Status over XPI
                    [4 * NOR_CMD_LUT_SEQ_IDX_READSTATUS_XPI + 0] = FLEXSPI_LUT_SEQ(
                        CMD_SDR, FLEXSPI_8PAD, 0x05, DUMMY_DDR, FLEXSPI_8PAD, NUM_DUMMY_CYCLES),
                    [4 * NOR_CMD_LUT_SEQ_IDX_READSTATUS_XPI + 1] = FLEXSPI_LUT_SEQ(
                        READ_DDR, FLEXSPI_8PAD, 0x04, STOP, FLEXSPI_1PAD, 0x0),  
                    // * Write Enable over XPI
                    [4 * NOR_CMD_LUT_SEQ_IDX_WRITEENABLE_XPI + 0] = FLEXSPI_LUT_SEQ(
                        CMD_SDR, FLEXSPI_8PAD, 0x06, STOP, FLEXSPI_1PAD, 0x0),
                    // Erase Sector
                    [4 * NOR_CMD_LUT_SEQ_IDX_ERASESECTOR + 0] = FLEXSPI_LUT_SEQ(
                        CMD_SDR, FLEXSPI_8PAD, 0x21, RADDR_DDR, FLEXSPI_8PAD, 0x20),
                    // Erase Block
                    [4 * NOR_CMD_LUT_SEQ_IDX_ERASEBLOCK + 0] = FLEXSPI_LUT_SEQ(
                        CMD_SDR, FLEXSPI_8PAD, 0xDC, RADDR_DDR, FLEXSPI_8PAD, 0x20),
                    // Page Program
                    [4 * NOR_CMD_LUT_SEQ_IDX_PAGEPROGRAM + 0] = FLEXSPI_LUT_SEQ(
                        CMD_SDR, FLEXSPI_8PAD, 0x12, RADDR_DDR, FLEXSPI_8PAD, 0x20),
                    [4 * NOR_CMD_LUT_SEQ_IDX_PAGEPROGRAM + 1] = FLEXSPI_LUT_SEQ(
                        WRITE_DDR, FLEXSPI_8PAD, 0x04, STOP, FLEXSPI_1PAD, 0),
                    // Erase Chip
                    [4 * NOR_CMD_LUT_SEQ_IDX_CHIPERASE + 0] = FLEXSPI_LUT_SEQ(
                        CMD_SDR, FLEXSPI_8PAD, 0x60, STOP, FLEXSPI_1PAD, 0)
                },
        },
    .pageSize = 256U,
    .sectorSize = 4096U,      // 4K is actually the block size not a sector (has to
                              // match 'sector' erase size as Boot ROM FlexSPI LUT doesn't have a block erase)
    .ipcmdSerialClkFreq = 1   // 30 MHz. Max supported normal SPI frequency is 50MHz for this chip. Option 2 = 60MHz.
};
#endif                        /* XIP_BOOT_HEADER_ENABLE */
