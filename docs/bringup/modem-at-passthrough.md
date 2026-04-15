# Modem AT Passthrough (IRQ UART)

This guide describes how to use the modem AT shell flow implemented in this repository.

The implementation uses Zephyr features directly:
- Shell command interface
- Interrupt-driven UART RX/TX for modem traffic
- DeviceTree for UART and USB CDC ACM routing
- Kconfig for shell/USB/UART enablement

## Prerequisites

- Board build target: `control@a4`
- Shield: `sense_a3`
- USB shell enabled (already configured in `control/prj.conf`)

## Build

From repository root:

```bash
west build control -p auto -b control@a4 --shield sense_a3 -d control/build
```

If Kconfig or DTS changed, do a pristine rebuild:

```bash
west build -p always control -b control@a4 --shield sense_a3 -d control/build
```

## Flash

```bash
west flash -d control/build
```

## Open the Zephyr shell

The shell is exposed over USB CDC ACM.

- Windows: connect to the board COM port with a serial terminal.
- Linux: use `/dev/ttyACM*`.

Example:

```bash
screen /dev/ttyACM0 115200
```

## Power on modem and send AT

1. Check status:

```text
modem status
```

2. Power on modem:

```text
modem power on
```

3. Send AT commands:

```text
modem at AT
modem at ATI
modem at AT+CSQ
```

4. Optional debug mode (prints raw response and transport diagnostics):

```text
modem at --debug AT+CSQ
```

## Raw passthrough mode

Use passthrough when you want interactive byte stream access to the modem UART.

```text
modem passthrough
```

Debug trace mode (hex/text RX trace):

```text
modem passthrough --debug
```

Exit passthrough by pressing:
- `Ctrl-X` then `Ctrl-Q`

## Notes on UART mode

Modem command path is interrupt-driven for UART RX and TX.
- TX uses UART IRQ FIFO writes (no poll-out path for AT command transport).
- RX uses UART IRQ FIFO reads into a ring buffer consumed by shell/AT flow.

If you see `modem UART RX is busy`, another modem UART session is active (AT command or passthrough).
