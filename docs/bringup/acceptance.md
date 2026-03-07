# Bring-up acceptance (living)

> **Status:** living document — update as hardware + firmware evolve.
>
> **Scope:** minimal “it builds + it talks” checklist for the **control@a4 + sense_a3** configuration.

## One-time setup

From the repo root:

```bash
west update
```

Notes:
- This repo is already a west workspace (no `west init` required).

## Canonical build command (known-good)

```bash
west build control -p auto -b control@a4 --shield sense_a3 -d control/build
```

## Flash

Typical flow (runner depends on your debug probe / setup):

```bash
west flash -d control/build
```

If flashing fails, capture:
- full console output from `west flash` (include runner name)
- probe type (e.g., J-Link) and target connection (USB/JTAG/SWD)

## USB CDC ACM shell

This project supports **Zephyr shell over USB CDC ACM**.

1. Connect the control board USB port to your host.
2. Flash firmware, then replug USB if your host does not enumerate immediately.
3. Open the virtual serial port:
   - Linux: `/dev/ttyACM*`
   - macOS: `/dev/cu.usbmodem*`
   - Windows: `COMx`

Example (Linux/macOS):

```bash
tio /dev/ttyACM0
# or
screen /dev/ttyACM0 115200
```

> Baud setting is ignored by USB CDC ACM, but many terminal apps require one.

You should see a prompt like (Zephyr default):

```text
uart:~$ 
```

Verify the shell is responsive:

```text
help
kernel version
kernel uptime
```

Troubleshooting notes:
- If no `ttyACM`/`usbmodem`/`COM` port appears, check cable quality and USB permissions/drivers.
- If the port appears but no prompt, press Enter a couple of times.

## Modem GPIO control (Iteration 1)

This firmware exposes **board-specific modem power/reset primitives** via shell commands.

Commands:

```text
modem status
modem power on
modem power off
modem power cycle
modem reset
```

Manual acceptance steps:

- [ ] Run `modem status` and confirm it prints the control lines and VGPIO-derived state:
  - `MODEM_3V8_EN`
  - `MODEM_PWR_ON_N`
  - `MODEM_RST_N`
  - `VGPIO_mV`
  - `MODEM_STATE`
- [ ] With the modem powered off, run `modem status` and confirm:
  - `MODEM_STATE=OFF`
  - `VGPIO_mV` is plausibly near 0 mV
- [ ] Run `modem power on` and confirm:
  - MODEM 3V8 rail is enabled (measure 3V8 on the modem rail if accessible)
  - modem begins booting (e.g., UART activity / status LED, depending on module)
- [ ] Once the modem is up, run `modem status` and confirm:
  - `MODEM_STATE=ON`
  - `VGPIO_mV` is plausibly within the expected VGPIO range (~1.7–1.9 V)
- [ ] Run `modem reset` and confirm the modem reboots
- [ ] Run `modem power off` and confirm:
  - the modem rail is disabled
  - a follow-up `modem status` reports `MODEM_STATE=OFF`
  - `VGPIO_mV` returns plausibly near 0 mV

> Timing notes: the PWR_ON_N pulse widths are currently conservative defaults. Confirm the module’s
> required timings (on/off pulse, rail settle time) and adjust if needed.

## Acceptance checklist

- [ ] `west update` completes cleanly
- [ ] Canonical build command completes without errors
- [ ] `control/build/zephyr/zephyr.elf` (and `.hex`/`.bin` as applicable) produced
- [ ] `west flash -d control/build` succeeds on target hardware
- [ ] USB CDC ACM port enumerates on host (`ttyACM` / `usbmodem` / `COM`)
- [ ] Shell prompt appears and is responsive (`help`, `kernel version`, `kernel uptime`)
