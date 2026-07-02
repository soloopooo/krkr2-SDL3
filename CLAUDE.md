# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Kirikiroid2-Yuri is a fork of Kirikiroid2 — a cross-platform port of the **Kirikiri2/KirikiriZ** Japanese visual-novel engine. The engine runs games written in the **TJS2** scripting language with the **KAG** ADV system. This fork targets newer Android devices (SDK 22+) and more file formats. Android (arm64-v8a) is the only platform currently buildable; Windows/Linux are stubbed out in CMake and not yet supported.

**Roadmap and implementation status:** see [`docs/ROADMAP.md`](docs/ROADMAP.md) (phases 0–6, what is done vs TODO). User-facing checklist remains in `readme.md`. Long-term goal: replace cocos2d-x rendering with SDL2 (phase 6 in `docs/ROADMAP.md`).

The original engine code is largely Win32-derived; much of `src/core` carries `win32/` subfolders that are selectively excluded per-platform in CMake.

## Build

Two-stage build: third-party dependencies are cross-compiled first, then the APK is assembled by Gradle (which drives the project's CMake via NDK externalNativeBuild).

```sh
# 1. Dependencies (from script/). Fetches sources and cross-compiles ~25 libs.
cd script
bash cross_androida64.sh                       # full: fetch + build ports, then build .so
SKIP_PORTS=yes bash cross_androida64.sh        # skip ports if already built
# Outputs into thirdparty/build/arch_androida64/ (lib + include)

# 2. APK
cd project/android
./gradlew assembleDebug                          # -> ../../../build_android/outputs/apk/debug/krkr2yuri_v<ver>.apk (configured by build.gradle)
./gradlew assembleRelease                        # requires signing env vars / sign.properties
```

Prerequisites: Android SDK with `ANDROID_HOME` set, **NDK 29.0.14206865**, and the host tools `wget 7z git make cmake` (plus python2 for cocos2d-x v3, msys2 on Windows for the ffmpeg port).

**Shortcut:** prebuilt dependency tarballs (`thirdparty_build.tar.gz`, `thirdparty_port.tar.gz`) are published under the repo's `deps` release and can be extracted into `thirdparty/build` and `thirdparty/port` to skip stage 1 entirely. The CI (`.github/workflows/build_android.yml`) does exactly this — it caches those tarballs and only runs `./gradlew assembleDebug`.

**CI triggers:** push of `v*` tags, or manual **workflow_dispatch** (`gh workflow run build_android.yml --ref <branch>`). Prefer GitHub Actions for builds when local compilation is not desired.

There is no test suite. Validation is manual: build the APK and run a game.

## Dependency layout (not in git)

These paths are gitignored and must exist before a build succeeds:

- `thirdparty/port/` — extracted third-party **source** (notably `thirdparty/port/cocos2d-x`, referenced throughout CMake as `COCOS2DX_PATH`).
- `thirdparty/build/arch_androida64/` — **compiled** static libs + headers (`PORTBUILD_PATH`). All audio/video/image/archive libs link from here.
- `assets/` — game engine assets, extracted from a prebuilt reference APK (e.g. `Kirikiroid2_yuri_1.3.9.apk`). Gradle pulls these from root `assets/` via `assets.srcDirs = ["../../../assets"]` (relative to `project/android/app/`).

## Architecture

The build produces one native shared library, `libkrkr2yuri.so`, composed of three CMake targets plus a JNI shim, loaded by a thin Java/Cocos Android app.

### CMake target graph

- **Root `CMakeLists.txt`** — builds the final `krkr2yuri` shared lib from `project/android/app/cpp/krkr2_android.cpp`. Links `krkr2core` and `krkr2plugin`. Note the `--whole-archive` wrapper around `cpp_android_spec` and the plugins: it forces JNI symbols and self-registering plugins to survive dead-code stripping — **do not remove it** or JNI entry points / plugins silently vanish.
- **`src/core/CMakeLists.txt` → `krkr2core`** (static) — the engine. Globs sources across `tjs2/ visual/ sound/ movie/ base/ environ/ msg/ utils/ extension/`. Uses an explicit `REMOVE_ITEM` blocklist to drop Win32-only files (GDI font rasterizer, SSE resamplers, JXR/BPG loaders, etc.). Appends `environ/android/`, `sound/ARM/`, `visual/ARM/` only when targeting Android. Links every third-party lib.
- **`src/plugins/CMakeLists.txt` → `krkr2plugin`** (static) — TJS2-callable native plugins, bound to the engine via `src/plugins/ncbind/`. All `src/plugins/*.cpp` are globbed in; new plugins only need a new `.cpp` with `NCB_MODULE_NAME` and ncbind registration.

**Plugins (recent):** `scriptsEx` ported (`scriptsEx.cpp`, `bitap_fuzzy.hpp`, MD5 via `src/core/utils/md5`). Android stubs: `windowEx_stub.cpp`, `layerExDraw_stub.cpp` (registers `GdiPlus` TJS class; real GDI+ only viable on future Win32 build). Existing: `xp3filter`, `layerExMovie`, `perspective`, etc. See plugin table in [`docs/ROADMAP.md`](docs/ROADMAP.md).

When adding/removing engine source files, remember the CMake globs require a re-CMake to pick them up, and check the `REMOVE_ITEM` blocklist if a file fails to compile on Android.

### Core engine (`src/core/`)

- `tjs2/` — the TJS2 language VM (lexer, bytecode interpreter, intrinsic objects). The scripting heart.
- `visual/` — rendering, layers, image codecs, real-time texture compression (`ARM/`, `gl/`, `ogl/`). Win32 rasterizer in `win32/` is excluded on Android.
- `sound/` — audio (ogg/opus/vorbis via OpenAL/oboe); `ARM/` holds NEON paths.
- `movie/` — video playback (ffmpeg-backed, originally from kodi).
- `base/` — file system, XP3 archive handling, storage abstraction.
- `msg/` — engine messages / localized strings.
- `environ/` — **platform integration layer**, the most fork-relevant area:
  - `environ/cocos2d/` — the cocos2d-x application: `AppDelegate`, `MainScene` (`TVPMainScene`), `CustomFileUtils`, `YUVSprite`. This is the bridge between the engine and the cocos rendering/event loop.
  - `environ/ui/` — all in-engine UI forms (file selector, preferences, in-game menu, message box) built as cocos `BaseForm` subclasses.
  - `environ/android/` — `AndroidUtils`, Android-specific storage/JNI helpers. `TVPCheckStartupArg` here consumes launch args stashed by the JNI `nativeSetStartupArgs` (forwarded from `KR2Activity.onCreate` intent extras) and dispatches `startupFrom` when the supplied path is a bootable archive/folder.
  - `environ/ConfigManager/` — `GlobalConfigManager` for persisted preferences (`GlobalPreference.xml` under `TVPGetInternalPreferencePath()` → `writablePath/.preference/`). Saves on preference UI exit and on each change via `PreferenceConfig.h` → `SaveToFile()`. Android writes through `TVPWriteDataToFile` in `environ/android/AndroidUtils.cpp` (`fopen` then JNI fallback). The SAF document-tree URI is mirrored here under key `saf_tree_uri` (written by `nativeSetSafTreeUri` from `KR2Activity.onActivityResult`).
  - `environ/ui/` beta touch fixes (see `docs/ROADMAP.md` phase 0): title path list cells in `FileSelectorForm.cpp`, `MessageBox.cpp` button swallow touches, `InGameMenuForm.cpp` nested menu push animation.

### Android app & JNI bridge (`project/android/`)

- Java side: `org.tvp.kirikiri2.KR2Activity` is the base activity (input, IME, message boxes, storage); `com.yuri.kirikiri2.MainActivity` extends it and is the launcher entry. `MediaStoreHack` implements the scoped-storage bypass.
- Native bridge: `project/android/app/cpp/krkr2_android.cpp` holds all `JNIEXPORT Java_org_tvp_kirikiri2_KR2Activity_*` functions. The pattern throughout is to marshal Java events (touch, key, IME, mouse, low-memory) and dispatch them onto the cocos thread via `Android_PushEvents(...)` or `performFunctionInCocosThread(...)` — **never call cocos/engine objects directly from a JNI thread**; always hop to the cocos thread. `cocos_android_app_init` constructs the `TVPAppDelegate`. Config/launch bridges are exceptions to varying degrees: `nativeGetSafTreeUri`/`nativeGetHideSystemButton` read-only from the JNI thread (plain data, no locking needed); `nativeSetStartupArgs` writes plain globals synchronously from `onCreate` (safe — runs before the cocos thread starts); `nativeSetSafTreeUri` writes to `GlobalConfigManager` (unlocked map) from `onActivityResult`, so it hops to the cocos thread via `Android_PushEvents` to avoid racing concurrent config access.
- Crash reporting via Google Breakpad is wired through `initDump` (minidump descriptor + exception handler), gated to skip during intentional shutdown (`TVPSystemUninitCalled`).
- `project/android/settings.gradle` includes two modules: `:cocos2dx` (sourced from `thirdparty/port/cocos2d-x/.../libcocos2dx`) and `:krkr2yuri` (`app/`). Build knobs live in `gradle.properties` (`PROP_APP_ABI`, `PROP_*_SDK_VERSION`, `PROP_BUILD_TYPE=cmake`).

### UI assets (`project/ui/`)

CocosStudio project (`.cocos-project.json`, `cocosstudio/`) holding image assets and the localization XMLs in `project/ui/Resources/res/locale/` (en/ja/zh_cn/zh_tw).

## Conventions

- Engine classes and globals use the upstream `TVP*` prefix (`TVPMainScene`, `TVPAppDelegate`, `TVPSystemUninitCalled`).
- The codebase mixes tabs and the original Win32 source style; match the style of the file you are editing rather than imposing a global standard.
- Platform-specific code is segregated into `win32/` / `android/` / `ARM/` subfolders and selected in CMake, not via `#ifdef` sprinkled everywhere — follow that pattern when adding platform code.

## Documentation

| Doc | Purpose |
|-----|---------|
| [`docs/ROADMAP.md`](docs/ROADMAP.md) | Phased roadmap, implementation notes, file pointers, CI |
| `readme.md` | Usage, build steps, compatibility table, issue list |
| `CLAUDE.md` | Agent-oriented architecture and conventions (this file) |

When extending the fork, update `docs/ROADMAP.md` for phase/status changes and keep `readme.md` roadmap checkboxes in sync where user-visible.

## Current session: SDL2-only port (branch `sdl2-port`)

**Goal:** Remove cocos2d-x entirely; replace with SDL2 + raw OpenGL ES for rendering.

### Build
```sh
# SDL2-only variant (no cocos2d-x linked):
cd project/android
ANDROID_HOME=$HOME/android-sdk ANDROID_NDK_HOME=$ANDROID_HOME/ndk/29.0.14206865 \
./gradlew assembleDebug
```
The `CMAKEFLAGS` property is NOT forwarded to cmake. To toggle variants, edit `build.gradle`'s cmake arguments directly (`-DKRKR2_USE_COCOS2D=OFF`).

### Key files
| File | Purpose |
|------|---------|
| `project/android/app/cpp/krkr2_android_sdl.cpp` | JNI bridge for SDL2 variant. Implements ALL `Java_org_cocos2dx_lib_*` and `Java_org_tvp_kirikiri2_*` methods directly (no cocos2d-x dependency). |
| `src/core/environ/android/AndroidUtils_sdl.cpp` | Platform utilities for SDL2 variant (TVPGetCurrentLanguage, TVPCheckStartupArg, JNI storage path queries, event queue). |
| `src/core/environ/android/JNIHelper_sdl.h` | Lightweight JNI helper (raw JNI, no cocos2d). |
| `src/core/environ/sdl/WindowLayer_sdl.h/.cpp` | SDL2 window layer (iWindowLayer impl), engine tick function, input forwarding helpers, and ALL stubs for excluded subsystems. |
| `src/core/environ/sdl/cocos2d.h` | Minimal stub replacing cocos2d-x's `cocos2d.h`. Provides enough types (`Node`, `Size`, `Texture2D`, `Director`, `FileUtils`) for core files to compile. |
| `src/core/environ/sdl/renderer/CCTexture2D.h` | Stub for `renderer/CCTexture2D.h` (includes cocos2d.h). |
| `src/core/environ/sdl/KRMovieStubs.cpp` | Stubs for KRMovie subsystem (all TVPMoviePlayer/VideoPresentLayer methods as no-ops). |

### Current state
- **Compiles and links** without cocos2d-x (0 compile errors, 0 link errors)
- APK installs but crashes with `UnsatisfiedLinkError` for `Cocos2dxHelper.nativeSetAudioDeviceInfo` — the JNI method is in `Cocos2dAndroidStubs.cpp` which is excluded from build. Need to move to `krkr2_android_sdl.cpp`.

### Excluded subsystems (stubs provided)
These files are removed from the SDL2 build via `KRKR2CORE_SDL_REMOVE` in `src/core/CMakeLists.txt`:
- `environ/android/AndroidUtils.cpp` — replaced by `AndroidUtils_sdl.cpp`
- `environ/win32/Platform.cpp` — replaced by Android impls
- `environ/linux/Platform.cpp` — not needed
- `visual/FontImpl.cpp` — font loading (TVPEnumFontsProc/TVPInitFontNames stubbed)
- `visual/LoadPVRv3.cpp` — PVR texture loading
- `visual/ogl/RenderManager_ogl.cpp` — OGL renderer (not used)
- `movie/ffmpeg/KRMovieLayer.cpp` — video presentation layer
- `movie/ffmpeg/krffmpeg.cpp` — ffmpeg glue
- `environ/sdl/Cocos2dAndroidStubs.cpp` — obsolete (stubs moved to WindowLayer_sdl.cpp)

### Current status
- `TVPInitializeStartupScript()` stubbed — engine starts, render loop runs at ~79fps.
- **Fixed:** `TVPTerminateOnNoWindowStartup`, font stubs, `System.inform` stub, double `eglSwapBuffers`.
- **Software renderer (GLES2) working**: Gradient + fullscreen textured quad.
- **Fixed: `TVPLoadPVRv3` crash** — stub returned `void` but declared returning `iTVPTexture2D*`. Wrong type caused garbage pointer → heap corruption → SIGTRAP at `~tTJSBinaryStream`. Fixed by returning `nullptr`.
- **Image loading works** — 400+ images load successfully through TVPLoadGraphic → CreateTexture2D → AssignTexture.
- **Render loop stable** at ~60fps, no crashes.

### Font system (SDL2 variant)

FontImpl.cpp is compiled in SDL2 variant (removed from SDL_REMOVE list in CMakeLists.txt). On startup, `TVPInitFontNames()` tries:

1. User-configured `default_font` path
2. App data path `/default.ttf`/`.ttc`/`.otf`/`.otc`
3. External storage `/default.ttf`
4. Internal storage `/default.ttf`
5. **Direct `stat()` + `fopen()` scan of known system font paths** (bypasses TVP storage system which doesn't handle absolute paths)
6. `fonts/` subdirectory scan in all storage paths

**System font paths tried** (`FontImpl.cpp` SDL section):
```cpp
"/system/fonts/MiSansLatinVF.ttf"       // Xiaomi Latin variable font
"/system/fonts/MiSansJapaneseVF.ttf"    // Xiaomi Japanese
"/system/fonts/MiSansTCVF.ttf"          // Xiaomi Traditional Chinese
"/system/fonts/DroidSans.ttf"           // Traditional Android font
"/system/fonts/DroidSans-Bold.ttf"      // Traditional Android bold
```

Fonts are registered with a `Getter` callback that opens via `fopen()` (bypasses the storage system's `TVPCreateBinaryStreamForRead`).

**Bundled font:** `assets/DroidSansFallback.ttf` (7.3MB) exists in the APK. On first launch, `KR2Activity.onCreate()` extracts it to `<internal_storage>/DroidSansFallback.ttf`. The SDL font scan (`FontImpl.cpp`) loads it via `stat()`+`fopen()`+`TVPInternalEnumFonts()` with a `Getter` callback (same bypass approach as system fonts).

**Future UI font picker:** When implementing native Android launcher, enumerate `/system/fonts/*.ttf` and let user choose.

### Known blocker: black screen (FIXED)
All images load correctly but the primary layer pixel buffer remains all zeros (0x00000000). The renderer reads blank pixels and displays black. Suspected causes:
1. Layer compositing (`InternalComplete2` → `Draw`) doesn't write to the primary layer's MainImage
2. Compositing output goes to a different buffer (draw device intermediate buffer)
3. `TVPDeliverWindowUpdateEvents()` called but doesn't help

### Key XP3 files (Hulotte engine)
| File | Content |
|------|---------|
| `startup.tjs` | `System.inform(...)` → `execStorage("Status.tjs")` → `execStorage("Initialize.tjs")` → `execStorage("begin.tjs")` |
| `system\Initialize.tjs` | Loads 30+ TJS modules, creates MainWindow, SceneManager, sounds, fonts |
| `system\begin.tjs` | `new SceneManager(win); changeScene(SCENE_LOGO);` — not blocking |
| `system\Status.tjs` | Constants, screen 1280×720 |
| `Window.tjs` | MainWindow class |

### Key XP3 files
| File | Content |
|------|---------|
| `startup.tjs` | `System.inform("免费资源")` → `execStorage("Status.tjs")` → `execStorage("Initialize.tjs")` → `execStorage("begin.tjs")` |
| `system\Initialize.tjs` | Loads 30+ TJS modules, creates scene manager, fonts, sounds, plugins |
| `system\begin.tjs` | `new SceneManager(win); changeScene(SCENE_LOGO);` — returns immediately |
| `system\Status.tjs` | Constants, screen 1280×720 |
| `Window.tjs` | MainWindow class |

### cocos2d UI forms (reference for rewrite)
All in `src/core/environ/ui/`, built on cocos2d-x (CSB + TableView + ListView). For SDL variant, replace with Android native UI.

| Form (File) | Purpose |
|-------------|---------|
| **MainFileSelectorForm** | Launcher: file browser + recent game history + side menu (prefs/help/exit) |
| **FileSelectorForm** | Generic file picker: dir listing, copy/cut/paste/delete/rename/unpack |
| **BaseForm** | Base: CSB loader, form stack (push/pop with animations), touch router |
| **MessageBox** | Modal message box (N buttons) + progress bar dialog |
| **PreferenceForm** | Settings framework (checkbox/select-list/slider/keymap) |
| **GlobalPreferenceForm** | Global settings (renderer, FPS, fonts, keymap) |
| **IndividualPreferenceForm** | Per-game settings |
| **GameMainMenu** | In-game draggable overlay toolbar |
| **InGameMenuForm** | TJS menu tree rendered as list |
| **ConsoleWindow** | Engine debug log window |
| **SeletListForm** | Select-list + key-pair input form |
| **SimpleMediaFilePlayer** | Video player overlay UI |
| **TipsHelpForm** | First-launch tips viewer |
| **DebugViewLayerForm** | Layer inspector debug overlay |
| **XP3RepackForm** | XP3 archive repacker |

### Future: Android native launcher UI
Replace cocos2d UI forms with Android native views (no GL text rendering needed).

**Architecture:**
```
LauncherActivity (native UI: file list, settings, game history)
  ↓ startActivity(MainActivity, extra=gamePath)
MainActivity (Cocos2dxActivity + GLSurfaceView → engine)
  ↓ game exits, finish()
LauncherActivity (back to launcher)
```

**Key benefits:**
- No font system dependency (Android handles text natively)
- SAF/MediaStore for file access
- Standard Android list/button/input widgets
- Launcher and engine are separate activities — clean lifecycle

**TODO when starting:**
1. Create `LauncherActivity.java` + layout XML (file list, settings button, game history)
2. Change `AndroidManifest.xml` to set `LauncherActivity` as main/launcher
3. Implement file scanning + display (scan Download/ for .xp3 or startup.tjs dirs)
4. On game select: `startActivity` with startupPath extra → `MainActivity`
5. Modify `TVPExitApplication` / `KR2Activity` to finish() instead of System.exit()

## Memory files

| File | Content |
|------|---------|
| `.opencode/memories/cursor-exit-multiprocess.md` | Virtual cursor fixes, exit flow, audio cleanup, multi-process FORTIFY fix |
| `.opencode/memories/native-launcher-complete.md` | Native Android LauncherActivity implementation |
| `.opencode/memories/audio-resampling-gpu-state.md` | SDL_AudioStream per-stream resampling, GPU renderer state |
| `.opencode/memories/project-architecture-overview.md` | Full architecture overview (rendering, audio, plugins, build) |
| `.opencode/memories/gpu-crossfade-shader-fix.md` | GPU crossfade shader: root cause and fix for 2-texture blend |
| `.opencode/memories/debug-xp3-tjs-workflow.md` | **Debugging workflow**: XP3 extraction to find TJS game scripts (critical for GPU mode debug)
