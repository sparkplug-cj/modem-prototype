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

You should see a prompt like:

```text
usb:ctrl>
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

## Acceptance checklist

- [ ] `west update` completes cleanly
- [ ] Canonical build command completes without errors
- [ ] `control/build/zephyr/zephyr.elf` (and `.hex`/`.bin` as applicable) produced
- [ ] `west flash -d control/build` succeeds on target hardware
- [ ] USB CDC ACM port enumerates on host (`ttyACM` / `usbmodem` / `COM`)
- [ ] Shell prompt appears and is responsive (`help`, `kernel version`, `kernel uptime`)
