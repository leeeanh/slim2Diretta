# Makefile Migration Design (CMake Replacement)

**Date:** 2026-03-07
**Project:** slim2diretta
**Status:** Approved

## Goal
Replace the CMake-based build with a top-level `Makefile` as the canonical and only build system, while preserving existing build behavior and optional codec handling.

## Architecture
- Replace `CMakeLists.txt` with one root `Makefile`.
- Preserve current compile/link behavior:
  - Same source set
  - Same architecture/variant selection model
  - Same SIMD tuning model
  - Same optional codec behavior
  - Same `NOLOG` compile definition behavior
- Use SDK path discovery behavior aligned with `DirettaRendererUPnP-X`:
  - Use `DIRETTA_SDK_PATH` if provided
  - Otherwise search, in order:
    1. `../DirettaHostSDK_148`
    2. `./DirettaHostSDK_148`
    3. `$(HOME)/DirettaHostSDK_148`
    4. `/opt/DirettaHostSDK_148`
    5. `../DirettaHostSDK_147`
    6. `./DirettaHostSDK_147`
    7. `$(HOME)/DirettaHostSDK_147`
    8. `/opt/DirettaHostSDK_147`
- Binary output path becomes `bin/slim2diretta`.

## Components and Targets
- Build directories:
  - `SRCDIR=src`
  - `OBJDIR=obj`
  - `BINDIR=bin`
- Primary targets:
  - `all` (default): build `bin/slim2diretta`
  - `clean`: remove `obj` and `bin`
  - `info`: print detected arch/variant/SDK/codec state
  - `install`: copy binary and systemd template/default files
  - `uninstall`: remove installed files
- Dependency generation via `-MMD -MP`.

## Codec Behavior
- FLAC: required; missing FLAC is a hard build error.
- MP3/Ogg/AAC: optional; if dependency is missing, codec is disabled and its source file is excluded.
- This matches requested behavior: codec support is disabled when library is missing.

## Error Handling
- Hard errors for:
  - Missing Diretta SDK
  - Missing required Diretta static library
  - Missing required FLAC dependency
- Optional codec misses are non-fatal and reported in `make info` summary.

## Docs and Script Migration
- Remove CMake usage from README build/install instructions.
- Update `install.sh` build steps from CMake to `make`.
- Remove `CMakeLists.txt`.

## Verification
- Run:
  - `make clean`
  - `make` (or `make -j$(nproc)`)
  - `make info`
- Confirm binary path: `bin/slim2diretta`.
