# Kirikiroid2-Yuri roadmap (implementation notes)

## Phase 0 — Android beta stability

- Global preferences: `PreferenceConfig.h` persists on each change; `TVPWriteDataToFile` overwrites via `fopen(..., "wb")`.
- Title path list: `FileSelectorForm.cpp` keeps `ListItem.csb` cell wrapper for correct hit targets.
- In-game menu: nested menus use `eEnterAniOverFromRight`.
- Message box: `setSwallowTouches(true)` on dialog buttons.

## Phase 1 — Plugins

| Plugin | Status |
|--------|--------|
| scriptsEx | Ported (`scriptsEx.cpp`, `bitap_fuzzy.hpp`) |
| windowEx | Android stub (`windowEx_stub.cpp`); full port needs Win32 |
| layerExDraw / GdiPlus | Stub class + clear error on Android (`layerExDraw_stub.cpp`) |
| layerEx base | In-tree (`LayerExBase.cpp`, `layerExBase.hpp`) |

## Phase 2 — CX / XP3

- Pipeline: `xp3filter.cpp` + per-game `xp3filter.tjs` beside archives.
- Native `cxdec_decode` remains Win32-only reference; add decoders per title as needed.

## Phase 3 — Storage

- Scoped bypass: `MediaStoreHack.java`.
- SAF: `KR2Activity.java` picker (`triggerStorageAccessFramework` / `onActivityResult` requestCode 3) persists the document-tree URI in Android `SharedPreferences["URI"]` and now mirrors it into the engine via `nativeSetSafTreeUri` → `GlobalConfigManager` (`<Item key="saf_tree_uri" value="..."/>` in `GlobalPreference.xml`). Symmetric `nativeGetSafTreeUri` lets the engine read the URI back. In-engine preference item `preference_android_fetch_sdcard_permission` (`tTVPPreferenceInfoFetchSDCardPermission` in `PreferenceConfig.h`) re-triggers the SAF picker; locale strings exist in en/ja/zh_cn/zh_tw.

## Phase 4 — Config / CLI

- `GlobalPreference.xml` via `GlobalConfigManager`.
- Android `TVPCheckStartupArg` (`AndroidUtils_sdl.cpp`): runs the Breakpad dump check, then consumes launch args stashed by `nativeSetStartupArgs`. `KR2Activity.onCreate` reads intent extras (`startupPath` String → `.xp3`/bootable folder; `args` String[] of `-key=value`/`-flag`) and forwards them to native globals (`g_AndroidStartupPath` / `g_AndroidStartupArgs` in `krkr2_android_sdl.cpp`). `TVPCheckStartupArg` parses options into `TVPProgramArguments` via `TVPSetCommandLine` (exposed to TJS2 as `System.commandLineArgument`) and dispatches `startupFrom(path)` when the path is a bootable archive (`TVPCheckArchive == 1`) or directory containing `startup.tjs`; otherwise falls back to the file selector.

## Phase 5 — Desktop

- **Out of scope.** This fork is Android-only (arm64-v8a, SDK 22+). Root `CMakeLists.txt:18-23` stubs Windows/Linux with `not support yet`. No plans to build desktop targets.

## Phase 6 — SDL3 (replaced cocos2d-x — DONE)

- **Status: DONE.** cocos2d-x has been entirely removed. The engine now uses SDL3 as the sole backend.
- SDL3 provides: window creation, input (touch/key/IME), GPU API (Vulkan), audio, and Android activity bridge.
- No `Cocos2dxActivity`, no `GLSurfaceView`, no cocos2d GL state cache.
- GPU rendering: SDL_Gpu (Vulkan) with `TVPRenderManager_GPU` (~1050 lines, 50+ blend pipelines).
- Fallback display: SDL_Renderer (software path).
- Key files: `WindowLayer_sdl.cpp`, `krkr2_android_sdl.cpp`, `RenderManager_gpu.cpp`.
- See [`SDL3_MIGRATION.md`](SDL3_MIGRATION.md) for the full migration history.

## Phase 7 — Unit tests (proposed)

- **Goal:** Add a host-executable test suite (Google Test) targeting pure-logic engine components that don't require Android/cocos runtime.
- **Why:** Currently validation is manual only (build APK + run a game). Unit tests give regression coverage for refactors (notably Phase 6 SDL2 migration) and verify ported plugins against reference vectors.
- **Test framework:** Google Test (gtest) + CTest, built as a host target (not on-device). Host = the dev machine or CI runner, cross-compiling not needed for these since they're platform-independent logic.
- **Candidate components (high value, low coupling):**

  | Area | File(s) | Rationale |
  |------|---------|-----------|
  | TJS2 VM | `src/core/tjs2/*` (lexer, bytecode interp) | Core engine logic; well-isolated |
  | XP3 archive | `src/core/base/XP3Archive.cpp` | Parse + extract; verify against sample `.xp3` |
  | ZIP / 7z / TAR | `src/core/base/{ZIPArchive,7zArchive,TARArchive}.cpp` | Format correctness |
  | MD5 | `src/core/utils/md5` (used by `scriptsEx`) | Known-answer vectors (RFC 1321) |
  | Fuzzy match | `src/plugins/scriptsEx.cpp` + `bitap_fuzzy.hpp` | Pure algorithm, easy to vectorize |
  | Character set | `src/core/base/CharacterSet.cpp` | Encoding round-trips (SJIS/UTF-8/UTF-16) |
  | Streams | `src/core/base/{BinaryStream,TextStream,UtilStreams}.cpp` | Read/write invariants |
  | Config | `src/core/environ/ConfigManager/PreferenceConfig.h` | XML parse/serialize round-trip |

- **Layout:** `tests/` at repo root, mirroring `src/` structure; `tests/CMakeLists.txt` gated behind `option(KRKR2_BUILD_TESTS "Build unit tests" OFF)` so production builds are unaffected.
- **CI:** Add a `test` job to `.github/workflows/build_android.yml` (matrix on ubuntu-latest) running `cmake -DKRKR2_BUILD_TESTS=ON && ctest`. Tests link only the needed `.cpp` files (avoid pulling cocos/android), so they must be built with the test CMakeLists, not `add_subdirectory(src/core)`.

Build validation: GitHub Actions `.github/workflows/build_android.yml` on push of `v*` tags or **workflow_dispatch** (manual). No local compile required if CI deps tarballs are used (same as CI job).