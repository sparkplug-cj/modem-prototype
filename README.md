# Zephyr app scaffold (west workspace)

This repo is a self-contained **Zephyr west workspace**. Zephyr and modules live under `thirdparty/zephyrproject/`.

## Prerequisites

- Python 3.10+
- CMake + Ninja + dtc
- Zephyr SDK (recommended: 0.17.x)

### Python env (recommended)

```bash
python3 -m venv .venv
. .venv/bin/activate
pip install -U pip west pyelftools
```

Notes:
- `pyelftools` is required for Zephyr’s `gen_kobject_list.py`.

## One-time setup

```bash
# From repo root
west update
```

No `west init` is needed (the repo already contains a local manifest under `.west/`).

## Build (control@a4 + sense_a3 shield)

If your Zephyr SDK is not installed in a standard location, set these before building:

```bash
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk-0.17.x
```

Then build:

```bash
west build control -p auto -b control@a4 --shield sense_a3 -d control/build
```

Notes:
- You do **not** need to export `BOARD_ROOT`/`DTS_ROOT` env vars; the repo CMake wiring sets the required roots.

## Cleaning

To clean the existing build directory:

```bash
west build control -d control/build -t clean
```

For a completely fresh build (recommended when changing DTS/Kconfig wiring):

```bash
rm -rf control/build
```

## Repo layout

- `control/` — application code (`prj.conf`, `src/`, app overlays under `control/boards/`)
- `targets/boards/` — out-of-tree boards + shields
  - `targets/boards/fph/control/` — board definition + revision overlays
  - `targets/boards/shields/` — shield definitions/overlays
- `platform/` — platform-specific additions (e.g. DTS bindings under `platform/src/board/dts/bindings/`)
- `thirdparty/zephyrproject/` — Zephyr + west modules (managed by `west update`)
