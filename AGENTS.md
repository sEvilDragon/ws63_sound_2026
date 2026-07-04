# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## Project Overview

This directory contains the application layer for a WS63 sound project. The root `CMakeLists.txt` builds a single `main_app` component and selects one endpoint implementation:

- `CONFIG_MAIN_RECEIVING_END` -> `receiving end`
- `CONFIG_MAIN_CONTROL_END` -> `control end`
- otherwise defaults to `sending end`

The component compiles C++ as C++17 and disables exceptions/unwind tables. Endpoint directories use the same build pattern: add include paths to `PUBLIC_HEADER`, add implementation files to `SOURCES`, and propagate both with `PARENT_SCOPE`.

## Build and Test Commands

Run SDK build commands from `receiving end/fbb_ws63/src`:

```sh
python3 build.py ws63-liteos-app
python3 build.py -c ws63-liteos-app
python3 build.py -c ws63-liteos-app menuconfig
```

Useful build variants from the SDK docs/build script:

```sh
python3 build.py -j1 ws63-liteos-app        # slower single-job build for clearer errors
python3 build.py -c ws63-liteos-app -dump   # print target build parameters
```

Run CI gate checks from `receiving end/fbb_ws63`:

```sh
python ci/test_ci_gate.py
python ci/ci_gate.py
```

The only existing scoped `AGENTS.md` is `receiving end/fbb_ws63/ci/AGENTS.md`; follow it for CI-gate-specific changes.

## Endpoint Architecture

### Root Selection

`CMakeLists.txt` at this directory is the integration point. It sets `COMPONENT_NAME` to `main_app`, selects the endpoint subdirectory, applies C++ flags, and calls `build_component()`.

### Control End

Entry point: `control end/main.cpp`

Startup order matters:

1. Initialize and open DMA once in `app_entry()`.
2. Load NV settings before `osal_kthread_lock()` because NV reads depend on the scheduler.
3. Create tasks while the kernel thread lock is held.

Current control-side tasks include:

- `spi_slave_task` in `control end/realize/spi_task/`
- `ui_task` in `control end/realize/ui_task/`
- `led_test_task`
- `sk9822_task` in `control end/realize/sk9822_task/`

Do not reinitialize DMA inside individual tasks. DMA init/open is centralized in `app_entry()` to avoid `hal_dma_v151_open()` resetting channel state while `spi_slave` and SK9822 are sharing DMA resources.

The TTP229 touch/UI design is documented in `control end/realize/ttp229/README.md`. The UI controls volume, brightness, bass, input mode, hotspot, and network state, then maps them to `spi_settings_t` from `control end/realize/spi_task/spi_settings.h`.

Control-side two-wire communication uses `dws_slave` (`control end/includes/dws_slave/`) through the `spi_slave_task` compatibility interface.

### Receiving End

Entry point: `receiving end/main.cpp`

Startup initializes DMA once, loads receive-side NV settings before thread locking, sets default SPI mode to SLE, and creates:

- `led_test_task`
- `spi_master_task`
- `audio_play_task`
- `wifi_task`

`spi_master_task` exchanges `spi_settings_t` data with the control side and also sends audio analyzer results. On first successful communication, it boot-syncs settings from the slave response; after that the master settings are authoritative.

### Sending End

Entry point: `sending end/main.cpp`

Startup creates:

- `audio_read_send_task`
- `led_test_task`

This endpoint is the default when neither receiving nor control endpoint config is selected.

## Module and CMake Conventions

When adding or moving endpoint code:

1. Add public include directories to the endpoint-level `PUBLIC_HEADER` list if headers must be visible across modules.
2. Add module directories with `add_subdirectory(...)` or `add_subdirectory_if_exist(...)`, matching nearby files.
3. In module `CMakeLists.txt` files, append implementation files to `SOURCES` and propagate with `PARENT_SCOPE`.
4. Keep headers and implementations in the existing split: reusable/peripheral wrappers under `includes/`, task-level orchestration under `realize/`.

Paths such as `control end`, `receiving end`, and `sending end` contain spaces; quote them in shell commands.

## Development Notes

- Preserve endpoint startup ordering, especially DMA initialization and NV-before-lock behavior.
- Avoid enabling C++ exceptions; the root component explicitly disables them.
- For TTP229/UI behavior changes, verify against `control end/realize/ttp229/README.md` and update that document if behavior changes.
- For SPI settings changes, keep both ends aligned on `spi_settings_t` field order, validation, and constants.
- For CI gate work, use `receiving end/fbb_ws63/ci/AGENTS.md` rather than duplicating its rules here.
