# Bring-up acceptance (living)

> **Status:** living document — update as hardware + firmware evolve.
>
> **Scope:** minimal “it builds + it talks” checklist for the **control@a4 + sense_a3** configuration.

## Workflow conventions (repo hygiene)

- **One PR per functional increment.** Keep PRs small and reviewable.
- **Always reference the tracking issue in the PR description** (use GitHub keywords), e.g.:
  - `Fixes #9`
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

This project is expected to support a **shell over RTT** for bring-up. One common way to attach is:

1. Start a J-Link session (device name varies by MCU; adjust as needed):

   ```bash
   JLinkExe -if SWD -speed 4000 -autoconnect 1
   ```

2. In the J-Link prompt, start RTT:

   ```text
   rtt start
   rtt terminal 0
   ```

3. You should see boot logs and be able to type Zephyr shell commands.

Troubleshooting notes:
- If `rtt start` can’t find the control block, ensure the firmware is running and built with RTT console/log backend enabled.
- If multiple channels exist, try `rtt terminal 1`, etc.

## Acceptance checklist

- [ ] `west update` completes cleanly
- [ ] Canonical build command completes without errors
- [ ] `control/build/zephyr/zephyr.elf` (and `.hex`/`.bin` as applicable) produced
- [ ] `west flash -d control/build` succeeds on target hardware
- [ ] RTT attaches and shows logs
- [ ] Shell is responsive (run `help` and one simple command)
