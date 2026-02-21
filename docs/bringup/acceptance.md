# Bring-up acceptance (living)

> **Status:** living document — update as hardware + firmware evolve.
>
> **Scope:** minimal “it builds + it talks” checklist for the **control@a4 + sense_a3** configuration.

## Workflow conventions (repo hygiene)

- **One PR per functional increment.** Keep PRs small and reviewable.
- **Always reference the tracking issue in the PR description** (use GitHub keywords), e.g.:
  - `Fixes #3`
- **No self-approval.** The author (CJ) does not approve their own PRs; get another reviewer.
- Prefer **follow-up PRs** over stuffing unrelated changes into an existing PR.

## Environment

### Python / west

Use the shared Zephyr virtualenv:

```bash
. /home/node/.openclaw/workspace/tools/zephyr-venv/bin/activate
west --version
```

(If you’re using another venv, ensure it has at least `west` and `pyelftools` installed.)

### Zephyr SDK env vars

If your Zephyr SDK is not installed in a standard location, set:

```bash
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk-0.17.x
```

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

This project supports a **Zephyr shell over SEGGER RTT** (issue #3 baseline).

### Attach (J-Link RTT Viewer)

1. Flash and reset the target so the firmware is running.
2. Open **J-Link RTT Viewer**.
3. Connect to the target (device name varies by MCU; use the same device name as your flash/debug setup).
4. Open **Terminal 0** for logs.
5. Open **Terminal 1** for the shell.

You should see boot logs and a prompt like:

```text
rtt:ctrl> 
```

### Verify shell is responsive

In the RTT terminal, run:

```text
help
kernel version
kernel uptime
```

Expected:
- `help` prints the list of available commands
- `kernel version` prints the Zephyr version string
- `kernel uptime` returns a monotonically increasing uptime

Troubleshooting:
- If RTT attaches but you see logs and **no prompt**, press Enter a couple times.
- If RTT viewer can’t find the control block, ensure the firmware is running and built with RTT enabled.
- If you don’t see the prompt, confirm you’re on **Terminal 1** (this project places the shell on RTT buffer 1 to avoid conflicting with the log backend on buffer 0).

## Acceptance checklist

- [ ] `west update` completes cleanly
- [ ] Canonical build command completes without errors
- [ ] `control/build/zephyr/zephyr.elf` (and `.hex`/`.bin` as applicable) produced
- [ ] `west flash -d control/build` succeeds on target hardware
- [ ] RTT attaches and shows logs
- [ ] Shell prompt appears and is responsive (`help`, `kernel version`, `kernel uptime`)
