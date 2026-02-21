# AGENTS Guide

This file is for coding agents working in this repository.
Follow this guide first, then follow task-specific user instructions.

## Repository Snapshot

- Project type: Zephyr firmware workspace managed with `west`.
- Main application: `control/`.
- Out-of-tree board + shield definitions: `targets/boards/`.
- Platform-level DTS/Kconfig additions: `platform/`.
- Zephyr sources/modules are vendor-managed under `thirdparty/zephyrproject/`.
- Build system: CMake + Ninja through `west build`.
- Language mix currently present: C, CMake, Kconfig, DTS overlays/YAML.

## Environment and Setup

- Required tools: Python 3.10+, `west`, CMake, Ninja, `dtc`, Zephyr SDK.
- Recommended venv setup:
  - `python3 -m venv .venv`
  - `. .venv/bin/activate`
  - `pip install -U pip west pyelftools`
- Sync dependencies once per clone/update:
  - `west update`
- If SDK is not on a standard path, set:
  - `export ZEPHYR_TOOLCHAIN_VARIANT=zephyr`
  - `export ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk-0.17.x`

## Build Commands

- Canonical app build (board + revision + shield):
  - `west build control -p auto -b control@a4 --shield sense_a3 -d control/build`
- Build without auto-pristine reuse:
  - `west build control -b control@a4 --shield sense_a3 -d control/build`
- Force pristine rebuild when changing Kconfig/DTS/CMake wiring:
  - `west build -p always control -b control@a4 --shield sense_a3 -d control/build`
- Clean target in build dir:
  - `west build control -d control/build -t clean`
- Full reset of generated outputs:
  - `rm -rf control/build`

## Flash / Debug Commands

- Flash with board runner settings from `targets/boards/fph/control/board.cmake`:
  - `west flash -d control/build`
- Start debug session:
  - `west debug -d control/build`
- Confirm runner config used by current build:
  - `west build -d control/build -t runnertest`

## Lint / Static Checks

There is no standalone lint script checked in today.
Primary enforcement is compile-time warnings elevated to errors.

- Warnings are configured in `CMakeFiles/firmware-compile-options.cmake` and include:
  - `-Wall -Wextra -Wpedantic -Werror`
  - plus additional warning flags (`-Wshadow`, `-Wundef`, `-Wformat=2`, etc.)
- Practical lint command (fastest useful gate):
  - `west build control -b control@a4 --shield sense_a3 -d control/build`
- If touching board code, rebuild from pristine to catch hidden config issues:
  - `west build -p always control -b control@a4 --shield sense_a3 -d control/build`

## Test Commands

At time of writing, no committed test suites are present under this repository tree.
(`.gitignore` references `control/test-catch2/build/`, but matching sources are not checked in.)

Use these commands when tests are added:

- Run all Zephyr tests under a path:
  - `west twister -T <test-root> -p control@a4`
- Run a single test directory (most common "single test" command):
  - `west twister -T <path/to/one/test> -p control@a4`
- Run one named scenario only:
  - `west twister -T <test-root> -p control@a4 -s <suite_name.scenario_name>`
- Build-only validation for one test:
  - `west twister -T <path/to/one/test> -p control@a4 --build-only`

If a future Catch2 test tree appears, prefer its local README/build script for exact single-test syntax.

## File and Directory Conventions

- Application entrypoint and app C sources live in `control/src/`.
- The following software units live in `platform/src`:
  - Components shared between multiple applications.
  - Wrappers for third-party components.
  - Harware abstraction.
  - Zephyr abstraction.
- App config belongs in `control/prj.conf`.
- App-specific shield overlays belong in `control/boards/shields/<shield>/<shield>.overlay`.
- Custom board definitions live under `targets/boards/fph/control/`.
- Shared CMake helpers live in top-level `CMakeFiles/`.
- DTS bindings/extensions belong in `platform/dts/bindings/`.

## Code Style Rules (C / Headers)

- Follow existing local style in each touched file; keep diffs minimal and consistent.
- For new C files, prefer:
  - 2-space indentation, no tabs.
  - Opening brace on next line for functions and control blocks (Zephyr-like C style).
  - One statement per line.
  - Trailing newline at end of file.
- Include order:
  - Zephyr/system headers first (`<zephyr/...>` and vendor SDK headers).
  - Project/local headers after external headers.
  - Keep grouped and stable; avoid duplicate includes.
- Prefer `static` for file-local functions/objects.
- Use `const` aggressively for immutable data.
- Use fixed-width integer types where size matters.
- Avoid dynamic allocation unless required by subsystem design.
- Avoid introducing compiler-specific extensions unless already used in nearby code.

## Naming Conventions

- Macros/constants: `UPPER_SNAKE_CASE` (existing pattern in board code).
- Kconfig symbols: `CONFIG_*` names, uppercase with underscores.
- Local scoped C variables: `lowerCamelCase`.
- Module scoped C variables `mUpperCamelCase`.
- Private C functions `UpperCamelCase`.
- Public C functions (Prefix with 3-4 character module name abbreviation) `PRE_UpperCamelCase`.
- File names:
  - C/H files and overlays usually lowercase with underscores.
  - Board/shield naming follows Zephyr board qualifiers (`control@a4`, `sense_a3`).
- Logging modules use explicit module names (see `LOG_MODULE_REGISTER(...)`).

## Types and Interfaces

- Match Zephyr and NXP SDK types at API boundaries.
- Do not cast away `const` unless unavoidable and documented.
- Prefer explicit enum/struct types over magic integers.
- Keep public interfaces minimal; expose only what is needed.
- When adding config-dependent code, gate with the appropriate `CONFIG_*` macros.

## Error Handling and Robustness

- Check and propagate return values from Zephyr/NXP APIs.
- Fail fast for impossible states in startup/init paths.
- Use assertions for programmer errors, not runtime recoverable flow.
- In loops that wait on hardware state, ensure intent is clear and bounded where possible.
- Keep logging actionable: include subsystem/context in error logs.
- Avoid silent fallthrough in switch statements.

## CMake / Build Logic Guidelines

- Reuse existing helper CMake files instead of duplicating path logic.
- Keep board/shield assumptions explicit (this repo currently expects one shield in `CMakeFiles/shields.cmake`).
- Prefer target-scoped settings (`target_sources`, `target_compile_options`) over global mutation.
- Do not hardcode absolute developer-specific paths.

## Kconfig / Devicetree Guidelines

- Keep `prj.conf` grouped by subsystem with short rationale comments.
- For new symbols, follow Zephyr naming and dependency patterns.
- Put board-specific hardware details in board DTS/overlay, not generic app code.
- Keep bindings YAML strict and descriptive.

## What to Avoid

- Do not edit `thirdparty/` manually for normal feature work.
- Do not weaken warning levels or remove `-Werror` without explicit request.
- Do not reformat unrelated files in broad style-only sweeps.
- Do not introduce new tooling assumptions (formatters/linters) without adding docs.

## Agent Workflow Expectations

- Before editing, read nearby files and preserve established patterns.
- After edits, run at least one build command that exercises changed code.
- If no tests exist for your change, state that clearly in your handoff.
- Keep commits focused (build wiring vs feature logic vs config updates).

## Cursor / Copilot Rule Files

Checked locations:

- `.cursor/rules/`
- `.cursorrules`
- `.github/copilot-instructions.md`

No Cursor or Copilot instruction files were found in this repository at time of writing.
If these files are added later, update this document and treat those instructions as authoritative.
