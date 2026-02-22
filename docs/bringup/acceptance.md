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

## RTT shell (SEGGER)

This project supports **Zephyr shell over SEGGER RTT**.

One common way to attach is:

1. Start a J-Link session (device name varies by MCU; adjust as needed):

   ```bash
   JLinkExe -if SWD -speed 4000 -autoconnect 1
   ```

2. In the J-Link prompt, start RTT:

   ```text
   rtt start
   rtt terminal 0
   ```

3. You should see boot logs.

4. Open the shell channel (if you don’t see a shell prompt on terminal 0):

   ```text
   rtt terminal 1
   ```

You should see a prompt like:

```text
rtt:ctrl>
```

Verify the shell is responsive:

```text
help
kernel version
kernel uptime
```

Troubleshooting notes:
- If RTT can’t find the control block, ensure the firmware is running and built with RTT enabled.
- If you see logs but no prompt, press Enter a couple times and try `rtt terminal 1`.

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

- [ ] Run `modem status` and confirm it prints the three control lines:
  - `MODEM_3V8_EN`
  - `MODEM_PWR_ON_N`
  - `MODEM_nRST`
- [ ] Run `modem power on` and confirm:
  - MODEM 3V8 rail is enabled (measure 3V8 on the modem rail if accessible)
  - modem begins booting (e.g., UART activity / status LED, depending on module)
- [ ] Run `modem reset` and confirm the modem reboots
- [ ] Run `modem power off` and confirm the modem rail is disabled

> Timing notes: the PWR_ON_N pulse widths are currently conservative defaults. Confirm the module’s
> required timings (on/off pulse, rail settle time) and adjust if needed.

## Acceptance checklist

- [ ] `west update` completes cleanly
- [ ] Canonical build command completes without errors
- [ ] `control/build/zephyr/zephyr.elf` (and `.hex`/`.bin` as applicable) produced
- [ ] `west flash -d control/build` succeeds on target hardware
- [ ] RTT attaches and shows logs
- [ ] Shell prompt appears and is responsive (`help`, `kernel version`, `kernel uptime`)
