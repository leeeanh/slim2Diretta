# Makefile Migration Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fully replace CMake with a top-level Makefile workflow while preserving existing build behavior and optional codec fallback behavior.

**Architecture:** Introduce a root `Makefile` that handles architecture and SDK variant resolution, dependency probing, optional codec toggles, and final linking to `bin/slim2diretta`. Then migrate scripts/docs to call `make` directly and remove CMake entrypoints.

**Tech Stack:** GNU Make, g++, pkg-config, Linux system tooling, shell script updates.

---

### Task 1: Add new root Makefile with feature-parity behavior

**Files:**
- Create: `Makefile`
- Reference: `CMakeLists.txt`
- Reference: `../DirettaRendererUPnP-X/Makefile`

**Step 1: Define failing verification command**

Run: `test -f Makefile && rg -n "^all:|^info:|^clean:" Makefile`
Expected: FAIL before file creation because `Makefile` is missing.

**Step 2: Create minimal Makefile skeleton**
- Add compiler vars, directories, base targets (`all`, `clean`, `info`).
- Set target output to `bin/slim2diretta`.

**Step 3: Add architecture + variant detection**
- Implement base architecture detection from `uname -m`.
- Auto-select default variant; respect `ARCH_NAME` override.
- Keep SIMD selection and `TARGET_MARCH` compatibility.

**Step 4: Add SDK detection and validation**
- Implement `DIRETTA_SDK_PATH` override.
- Implement fallback search order matching `DirettaRendererUPnP-X`.
- Validate required Diretta library exists.
- Add optional ACQUA linking if present.

**Step 5: Add codec probing and conditional source inclusion**
- FLAC required: error if unavailable.
- MP3/Ogg/AAC optional: include source/defines/libs only when detected.
- Add compile-time defines (`ENABLE_MP3`, `ENABLE_OGG`, `ENABLE_AAC`) conditionally.

**Step 6: Add object/dependency/link rules**
- Add `-MMD -MP` dep generation.
- Ensure source list matches current CMake behavior.
- Add `install` and `uninstall` targets.

**Step 7: Verify Makefile behavior**
Run:
- `make -n info`
- `make -n`
Expected: command generation succeeds without syntax errors.

### Task 2: Migrate installer script to Makefile workflow

**Files:**
- Modify: `install.sh`

**Step 1: Write failing check**
Run: `rg -n "cmake|build/slim2diretta" install.sh`
Expected: FAIL desired state because legacy CMake references still exist.

**Step 2: Replace dependency messaging**
- Remove CMake from dependency lists and manual guidance where it is build-critical.

**Step 3: Replace build function implementation**
- Update build flow to run `make clean` then `make -j$(nproc)` at repo root.
- Update binary path checks to `bin/slim2diretta`.

**Step 4: Replace all binary path references**
- Update `setup_systemd_service`, `update_binary`, `test_installation` to use `bin/slim2diretta` fallback path.

**Step 5: Verify script migration**
Run: `rg -n "cmake|build/slim2diretta" install.sh`
Expected: no build-system references remain.

### Task 3: Migrate README to Makefile-first documentation

**Files:**
- Modify: `README.md`

**Step 1: Write failing check**
Run: `rg -n "cmake|CMake|build/slim2diretta|mkdir build" README.md`
Expected: FAIL desired state because legacy CMake docs still exist.

**Step 2: Update requirements and install commands**
- Remove CMake from required build tools/dependency snippets.
- Update manual build section to `make -j$(nproc)`.

**Step 3: Update architecture override docs**
- Replace `cmake -DARCH_NAME=...` examples with `make ARCH_NAME=...`.
- Document codec disable knobs with Make vars (`ENABLE_MP3=0`, etc.).

**Step 4: Update runtime command paths**
- Replace `./build/slim2diretta` and `cp build/slim2diretta` with `./bin/slim2diretta` and `cp bin/slim2diretta`.

**Step 5: Verify README migration**
Run: `rg -n "cmake|CMake|build/slim2diretta|mkdir build" README.md`
Expected: no legacy build references remain.

### Task 4: Remove CMake entrypoint and validate migration

**Files:**
- Delete: `CMakeLists.txt`

**Step 1: Remove legacy file**
- Delete `CMakeLists.txt` from repo root.

**Step 2: Verify expected build files**
Run: `ls -la | rg -n "Makefile|CMakeLists.txt"`
Expected: `Makefile` present, `CMakeLists.txt` absent.

**Step 3: Run final validation commands**
Run:
- `make clean`
- `make info`
- `make -j$(nproc)`
Expected: Makefile executes; if SDK/deps are absent in environment, failure should be explicit and actionable.

**Step 4: Commit**
```bash
git add Makefile install.sh README.md docs/plans/2026-03-07-makefile-migration-design.md docs/plans/2026-03-07-makefile-migration.md
# CMakeLists.txt deletion should be staged too
git commit -m "build: replace CMake with Makefile workflow"
```
