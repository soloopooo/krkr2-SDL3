# SDL3 Migration & Android JNI Unification

## 1. Why SDL3, Why Now

### SDL2 problems

| Issue | Detail |
|-------|--------|
| `SDL_AndroidSetJavaVM` | SDL2-only API removed in SDL3. Forces us into a custom JNI bridge pattern |
| Dual event path | Android touch/key → JNI → `Android_PushEvents` queue **and** SDL `SDL_PollEvent` for window events. Two competing input pathways |
| GLES2 burden | EGL context created by Java `Cocos2dxGLSurfaceView`, not managed by SDL. SDL doesn't know about our GL context lifecycle |
| No GPU API | SDL2 lacks `SDL_Gpu*`. To get Vulkan we'd write raw Vulkan from scratch anyway |

### SDL3 solves

| Feature | What it gives us |
|---------|-----------------|
| **Single event path** | Android touch/key → SDL3 internal JNI → `SDL_PollEvent` → our `TVPProcessSDLEvents` |
| **GL context owned by SDL** | `SDL_GL_CreateContext`, `SDL_GL_MakeCurrent`, `SDL_GL_SetSwapInterval` |
| **SDL_Gpu API** | `SDL_GpuDevice`, `SDL_GpuTexture`, `SDL_GpuGraphicsPipeline` — cross-platform GPU (Vulkan/Metal/D3D12) |
| **`SDL_CreateWindow` on Android** | SDL3 can create a native window + surface without cocos2d |
| **No more Cocos2dxActivity** | `KR2Activity` extends `SDLActivity` directly, not `Cocos2dxActivity` |

## 2. Architecture: Before vs After

### Current (SDL2 + custom JNI)

```
Java: KR2Activity extends Cocos2dxActivity
  ├── GLSurfaceView (Cocos2dxGLSurfaceView)
  ├── onTouchEvent → JNI nativeTouchesBegin
  ├── onKeyDown → JNI nativeKeyAction
  ├── TextInput → IME bridge → JNI
  └── onCreate/setup → JNI nativeSetStartupArgs

JNI bridge (krkr2_android_sdl.cpp, 616 lines):
  ├── JNI_OnLoad (own) — sets g_JVM + calls SDL_AndroidSetJavaVM
  ├── Cocos2dxRenderer JNI — render loop, touch, key, text
  ├── Cocos2dxActivity JNI — GL context attrs
  └── KR2Activity JNI — SAF, menu, message box, dump, storage

Event flow:
  Touch → JNI nativeTouchesBegin → Android_PushEvents → TVPForwardTouchBegin
  Window focus → SDL video driver → SDL_PollEvent → TVPProcessSDLEvents

Engine C++:
  ├── tvpsdl.cpp (SDL_Init, SDL_PollEvent for window events only)
  ├── WindowLayer_sdl.cpp (GLES2 init + fullscreen quad + input forwarding)
  └── AndroidUtils_sdl.cpp (JNI utilities, game picker, storage paths)

Rendering:
  EGL (from Java GLSurfaceView) → GLES2 → fullscreen quad
```

### Current (Phase 4c — SDL_Gpu display + GPU compositing pipelines)

```
Java: KR2Activity extends SDLActivity
  ├── SDLSurface.java handles touch/key/IME → SDL event queue
  ├── SDL_CreateWindow (plain, no OPENGL flag)
  └── KR2Activity extras: SAF, menu, message box, dump, storage, GameMenuOverlay

JNI bridge (krkr2_android_sdl.cpp):
  ├── No JNI_OnLoad (SDL3's SDL_android.c handles it)
  ├── No Cocos2dx JNI
  └── KR2Activity extras only

Event flow:
  ALL input → SDLSurface.java → SDL_android.c → SDL_PollEvent → TVPProcessSDLEvents
  SAF/menu → Android_PushEvents → DrainAndroidEventQueue (for thread hopping)

Engine C++:
  ├── tvpsdl.cpp — central event router (touch, mouse, key, text, window events)
  ├── WindowLayer_sdl.cpp — SDL_Gpu display pipeline, no GLES2
  └── AndroidUtils_sdl.cpp — JNI utilities, game picker, storage

Display:
  SDL_main → SDL_CreateGPUDevice(Vulkan) → SPIR-V shader pipeline
  → TVPEngineTick reads composited pixels → SDL_UpdateGPUTexture → fullscreen quad
  → SDL_SubmitGPUCommandBuffer

Compositing: Software renderer (CPU, all 60+ blend ops)
```

## 3. What Changes

### 3a. Java: `KR2Activity` extends `SDLActivity` instead of `Cocos2dxActivity`

```java
// Before
import org.cocos2dx.lib.Cocos2dxActivity;
public class KR2Activity extends Cocos2dxActivity { ... }

// After  
import org.libsdl.app.SDLActivity;
public class KR2Activity extends SDLActivity { ... }
```

SDL3's `SDLActivity` provides:
- `onCreate` → native JNI init
- `onTouchEvent` → feeds SDL event queue
- `onKeyDown`/`onKeyUp` → feeds SDL event queue
- `onPause`/`onResume` → GL context lifecycle
- `GLSurfaceView` equivalent (built into SDL3 Java layer)
- IME/text input handling

**Custom JNI methods to keep** (add as extension methods):

| Method | Purpose | Stays in |
|--------|---------|----------|
| `nativeSetStartupArgs` | Launch path from intent | `KR2Activity.java` |
| `nativeSetSafTreeUri` | SAF persistent URI | `KR2Activity.java` |
| `nativeGetSafTreeUri` | Read back SAF URI | `KR2Activity.java` |
| `initDump` | Breakpad crash dump | `krkr2_android_sdl.cpp` |
| `ShowMessageBox` | Modal dialog | `KR2Activity.java` |
| `ShowInputBox` | Modal text input | `KR2Activity.java` |
| `ShowMenuDialog` | Popup menu list | `KR2Activity.java` |
| `getExternalStoragePath` | Storage query | `KR2Activity.java` |
| `getInternalStoragePath` | Storage query | `KR2Activity.java` |
| `getDriverPath` | Storage query | `KR2Activity.java` |
| `exit` | Quit app | `KR2Activity.java` |
| `showTextInput`/`hideTextInput` | IME control | `KR2Activity.java` |
| `getApkStoragePath` | APK path for assets | `KR2Activity.java` |

**Methods to DELETE** (now handled by SDL3):

| Method | SDL3 Replacement |
|--------|-----------------|
| `nativeTouchesBegin`/`End`/`Move`/`Cancel` | SDL3 Java layer feeds SDL events |
| `nativeKeyAction` | SDL3 Java layer feeds SDL events |
| `nativeCharInput`/`nativeCommitText`/`nativeInsertText`/`nativeDeleteBackward` | SDL3 IME bridge |
| `nativeHoverMoved`/`nativeMouseScrolled` | SDL3 mouse bridge |
| `nativeGetContentText` | SDL3 IME |
| `nativeOnLowMemory` | SDL3 lifecycle |
| `nativeInit` / `nativeRender` | SDL3 render loop |
| `nativeOnPause` / `nativeOnResume` | SDL3 lifecycle |
| `getGLContextAttrs` | SDL3 has its own attrs |
| `Cocos2dxHelper_native*` stubs | Gone — no cocos2d |

### 3b. Native: `krkr2_android_sdl.cpp` shrinks from 616 → ~150 lines

**DELETE** (lines 44-233): JNI_OnLoad + all Cocos2dxRenderer + Cocos2dxActivity JNI
**KEEP** (lines 250-616): KR2Activity-specific JNI (SAF, menu, message box, dump, storage, startup args)

New JNI_OnLoad not needed — SDL3's own `SDL_android.c` provides it.

### 3c. Native: `tvpsdl.cpp` becomes the central event router

```cpp
void TVPProcessSDLEvents() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT: ...
        case SDL_EVENT_WINDOW_FOCUS_GAINED: ...
        case SDL_EVENT_FINGER_DOWN:
            TVPForwardTouchBegin(0, e.tfinger.x * winW, e.tfinger.y * winH);
            break;
        case SDL_EVENT_FINGER_UP: ...
        case SDL_EVENT_FINGER_MOTION: ...
        case SDL_EVENT_KEY_DOWN:
            TVPForwardKeyEvent(TVPTranslateSDLKey(e.key), true);
            break;
        case SDL_EVENT_KEY_UP: ...
        case SDL_EVENT_TEXT_INPUT:
            TVPForwardTextInput(e.text.text);
            break;
        case SDL_EVENT_WINDOW_RESIZED:
            SDL_GL_GetDrawableSize(window, &w, &h);
            TVPSetScreenSizeFromSDL(w, h);
            break;
        case SDL_EVENT_WINDOW_ICCPROF_CHANGED: ...
        }
    }
}
```

### 3d. Native: `WindowLayer_sdl.cpp` uses SDL_GL context

```cpp
// Before: EGL from Java surface, GLES2 raw
#include <EGL/egl.h>
#include <GLES2/gl2.h>
EGLDisplay dpy = eglGetCurrentDisplay();
EGLSurface surf = eglGetCurrentSurface(EGL_DRAW);

// After: SDL_GL context
#include <SDL3/SDL.h>
SDL_Window *window = SDL_CreateWindow("krkr", w, h, SDL_WINDOW_OPENGL);
SDL_GLContext ctx = SDL_GL_CreateContext(window);
SDL_GL_MakeCurrent(window, ctx);
SDL_GL_SetSwapInterval(1);  // vsync
SDL_GL_SwapWindow(window);  // swap buffers (replaces eglSwapBuffers)
```

### 3e. GL context lifecycle (no more Java GLSurfaceView)

SDL3 manages GL context on Android internally. `RECT`:
1. `SDL_CreateWindow("krkr", 0, 0, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE)`
2. `SDL_GL_CreateContext(window)`
3. `SDL_GL_MakeCurrent(window, ctx)` 
4. Render loop per frame
5. `SDL_GL_SwapWindow(window)` at end of frame
6. On surface destroy: SDL3 sends `SDL_EVENT_WINDOW_ICCPROF_CHANGED` or resets context internally

No more `ResetGLState()` / `TVPReinitOGL()` — SDL3 handles EGL context recreation.

### 3f. Gradle / AndroidManifest changes

**Manifest:** Launcher activity changes from `Cocos2dxActivity` to `SDLActivity`:

```xml
<!-- Before: -->
<activity android:name="org.cocos2dx.lib.Cocos2dxActivity" ... />
<!-- Switch to SDLActivity base: -->
```

But we use `KR2Activity` as the actual class. SDL3 provides `org.libsdl.app.SDLActivity`. Our `KR2Activity` extends `SDLActivity` instead of `Cocos2dxActivity`.

**build.gradle:** Remove all cocos2d-x dependencies from Java side:
- Remove `:cocos2dx` module dependency
- Add SDL3 JAR or source (`SDL3-3.4.10/android-project/app/src/java/org/libsdl/app/`)
- Keep NDK + CMake for native build

## 4. Migration Phases

Each phase ends with a working build that can be tested.

### Phase 0: Build SDL3 (DONE)
- [x] Source downloaded to `thirdparty/port/SDL3-3.4.10/`
- [x] Fetch function in `_fetch.sh`
- [x] Build function in `_androida64.sh` (static lib, Vulkan ON, GLES OFF)
- [x] CMake link from `sdl2_static` → `sdl3_static`
- [x] Build SDL3 and link successfully

### Phase 1: SDL3 event path + input routing (DONE)
- [x] `SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)` — video for window + GL
- [x] `TVPProcessSDLEvents` handles ALL events (window, finger, mouse, key, text)
- [x] `SDL_main` entry point with SDL_GL context (replaces Java EGL)
- [x] All input: SDL3 Java layer → C → SDL_PollEvent → TVPProcessSDLEvents → engine
- [x] **Test:** APK builds successfully (25MB debug APK)

### Phase 2: SDL3 GL context + Java SDLActivity migration (DONE)
- [x] `SDL_Init(SDL_INIT_VIDEO)` + `SDL_CreateWindow` + `SDL_GL_CreateContext`
- [x] Remove raw EGL from `WindowLayer_sdl.cpp` (no more `<EGL/egl.h>`)
- [x] `InitGL` uses `SDL_GL_MakeCurrent` instead of EGL queries
- [x] `TVPForceSwapBuffer` uses `SDL_GL_SwapWindow(g_sdlWindow)`
- [x] Java `Cocos2dxGLSurfaceView` removed — `KR2Activity` extends `SDLActivity`
- [x] `SDL3 Java source files (`SDLActivity.java`, `SDLSurface.java`, etc.) added to project
- [x] `getLibraries()` returns `{"krkr2yuri"}` (single .so, SDL3 statically linked)
- [x] All `Cocos2dxRenderer`/`Cocos2dxActivity` JNI methods deleted from C++
- [x] All touch/key/IME JNI methods deleted from Java (`KR2GLSurfaceView`, `DummyEdit`, `SDLInputConnection`)
- [x] SDL3 event handling for finger down/move/up, mouse motion/button, keyboard, text input
- [x] Removed `Cocos2dAndroidStubs.cpp` and `--allow-multiple-definition`
- [x] **Test:** APK builds (25MB), 0 C++ errors, 0 Java errors

### Phase 3: SDL3 unified input (DONE)
- [x] `KR2Activity` extends `SDLActivity`
- [x] Delete `Cocos2dxRenderer`/`Cocos2dxActivity`/`Cocos2dxHelper` JNI methods
- [x] Delete touch/key/ime JNI from Java (`KR2GLSurfaceView`, `DummyEdit`, `SDLInputConnection`)
- [x] Add SDL3's `SDLActivity.java` + 11 support Java files
- [x] `TVPProcessSDLEvents` handles finger/key/mouse/text events
- [x] Removed `TVPForwardCharInput` (dead code from old IME bridge)
- [x] SDL3 build flags: `SDL_INIT_VIDEO | SDL_INIT_EVENTS`
- [x] `Android_PushEvents`/`DrainAndroidEventQueue` retained for SAF/menu callbacks (still needed)
- [x] **Test:** APK builds clean, 0 errors

**Phase 3 design notes:**
- Input events flow: SDLSurface.java → `SDL_android.c` → `SDL_PollEvent` → `tvpsdl.cpp:TVPProcessSDLEvents` → `TVPForward*` → engine
- SAF/menu async callbacks still use `Android_PushEvents` to hop from UI thread to engine thread
- `TVPForward*` functions kept as input coordinate conversion layer

### Phase 4: SDL3 GPU API (DONE: 4a+4b+4c display + pipelines, remaining: cleanup)
**Phase 4a: Skeleton (DONE)**
- [x] `SDL_GpuDevice` creation
- [x] `SDL_ClaimWindowForGPUDevice`
- [x] `TVPRenderManager_GPU` skeleton class in `src/core/visual/gpu/`
- [x] `TVPRegisterGPURenderer()` registered

**Phase 4b: Display layer (DONE)**
- [x] Created SPIR-V shaders (quad.vert → quad_vert.spv, quad.frag → quad_frag.spv)
- [x] Fragment shader fix: `layout(set=2, binding=0)` for SDL3's descriptor set layout
- [x] `_initGpuDisplay`: device → sampler → vertex buffer → shaders → pipeline
- [x] `_gpuPresent`: transfer buffer upload → swapchain acquire → viewport → draw → submit
- [x] Aspect ratio correction via `SDL_SetGPUViewport`
- [x] Removed ALL GLES2 code: `gl2.h`, shader compile, FBO, glViewport, glClear, etc.
- [x] Removed `SDL_GL_CreateContext`/`SDL_GL_MakeCurrent`/`SDL_GL_SwapWindow`
- [x] `TVPForceSwapBuffer` a no-op (SDL_Gpu handles per-frame present)
- [x] `RESOLVED: vkCreateGraphicsPipelines` was failing due to `sampler2D` at `DescriptorSet 0` instead of `DescriptorSet 2` (SDL3 puts frag samplers at set 2)
- [x] Window 2536x1200, SDL_Gpu display @ ~120fps, ~8ms/frame (software compositing)

**Phase 4c: SDL_Gpu compositing pipeline (PARTIALLY DONE)**
- [x] Replace 60+ blend modes with `SDL_GPUGraphicsPipeline` variants (50+ modes mapped)
- [x] Create `TVPRenderManager_GPU` with pipeline cache (`_GetOrCreateMethod`)
- [x] SPIR-V custom shaders: DoGrayScale, BoxBlur, AdjustGamma (UBO), UnivTransBlend (3 tex + UBO)
- [x] Fence-based async readback (no `SDL_WaitForGPUIdle` per-frame)
- [x] Dual display mode: Software (`SDL_Renderer`) or Vulkan (`SDL_Gpu`) — toggled by `KRKR2_USE_VULKAN`
- [x] Android native debug overlay (TextView via JNI, works with both modes)
- [ ] Remove `RenderManager_ogl.cpp` (dead code — no GL context)
- [ ] Remove software compositing fallback (`RenderManager.cpp` CPU path not needed when GPU mode active)
- [ ] Consider compute shader for TLG decode / font SDF

See also `GPU_COMPUTE_PLAN.md` for future compute-shader acceleration ideas.

**Key files implemented:**
```
src/core/visual/gpu/RenderManager_gpu.cpp  — ~1050 lines, full iTVPRenderManager
src/core/visual/gpu/RenderManager_gpu.h     — class declarations
src/core/visual/gpu/shaders/                — SPIR-V: quad, gray, blur, gamma, adjust_gamma, univ_trans
```

## 5. Test Plan

### Per-phase testing

```
Phase 0: cmake build succeeds, libkrkr2yuri.so links without SDL2 symbols
Phase 1: APK installs, log shows "SDL3 initialized", game plays (same as before)
Phase 2: APK installs, GL rendering works (check fps log), no EGL calls from our code
Phase 3: APK installs, touch taps respond, keyboard input works
Phase 4: APK installs, log shows "SDL3 GPU device created", VN effects render correctly
```

### Test device matrix

| Device | Android | Vulkan | Notes |
|--------|---------|--------|-------|
| Pixel 7+ | 14+ | 1.3 | Primary target |
| Samsung S22+ | 12+ | 1.1 | Wide Vulkan support |
| Emulator (API 33+) | 13+ | 1.0 (swiftshader) | Vulkan via software |
| Old device (API 24) | 7+ | 1.0+ | Fallback to software renderer |

### Known regression risks

| Risk | Mitigation |
|------|-----------|
| Touch coordinates wrong | Use `SDL_GetWindowSize` to scale SDL touch coords → engine coords |
| IME text input broken | Test CJK text input on Phase 3 |
| GL context loss during game | SDL3 should emit events; handle `SDL_EVENT_WINDOW_ICCPROF_CHANGED` |
| Performance regression | Compare fps log from Phase 1 vs Phase 0 |
| Multi-window / multi-display | Not supported (single window) |

## 6. File Change Summary

### Files to create
```
project/android/app/java/org/libsdl/app/SDLActivity.java  (from SDL3 source)
```

### Files to modify
```
script/_fetch.sh                     — fetch_sdl3 added
script/_androida64.sh                — build_sdl3 added
script/cross_androida64.sh           — sdl2→sdl3 in fetch/build list
src/core/CMakeLists.txt              — sdl2→sdl3 everywhere
CMakeLists.txt                       — sdl2→sdl3
src/core/environ/sdl/tvpsdl.cpp      — SDL3 init + event mapping
src/core/environ/sdl/WindowLayer_sdl.cpp  — SDL3 GL context
src/core/environ/sdl/WindowLayer_sdl.h    — SDL3 include
project/android/app/java/org/tvp/kirikiri2/KR2Activity.java  — extends SDLActivity
project/android/app/build.gradle     — remove cocos2d, add SDL
project/android/AndroidManifest.xml  — activity class path
```

### Files to delete (Phase 3+)
```
project/android/app/cpp/Cocos2dAndroidStubs.cpp      — no more cocos2d refs
project/android/app/java/org/cocos2dx/lib/*           — entire cocos2dx java package
src/core/environ/sdl/Cocos2dAndroidStubs.cpp          — no longer needed
src/core/environ/sdl/renderer/ccGLStateCache.h        — no cocos2d GL
src/core/environ/sdl/renderer/CCGLProgramCache.h      — no cocos2d GL
src/core/environ/sdl/renderer/CCGLProgram.h            — no cocos2d GL
```

### Files to shrink (Phase 3)
```
project/android/app/cpp/krkr2_android_sdl.cpp   — 616→~150 lines (remove Cocos2dxRenderer JNI)
src/core/environ/android/AndroidUtils_sdl.cpp    — remove DrainAndroidEventQueue, Android_PushEvents
src/core/environ/sdl/WindowLayer_sdl.cpp         — remove GLES2 shader setup, keep SDL3 GL context
```

## 7. Vulkan Path

After SDL3 migration, two rendering paths:

### Path A: SDL3 GPU API (recommended if you want B)
```
SDL_GpuDevice (Vulkan backend)
  ├── SDL_CreateGpuGraphicsPipeline → pre-built pipelines (40 variants)
  ├── SDL_CreateGpuTexture → texture management
  ├── SDL_BindGpuGraphicsPipeline
  └── SDL_DrawGpuPrimitives
```
- No GL shaders at all
- No raw Vulkan boilerplate (SDL3 wraps it)
- Cross-platform (Metal on iOS/Mac, D3D12 on Windows)
- ~4 weeks to implement

### Path B: Raw Vulkan (recommended if you want A)
```
VkDevice
  ├── VkPipeline cache → 40 variants
  ├── VkImage → texture
  └── vkCmdDraw
```
- Maximum control
- Full framebuffer fetch via subpass input attachments
- ~8 weeks to implement (documented in VULKAN_RENDERER.md)

**Decision criteria:**
- If you want to target Windows/desktop too: **Path A (SDL_Gpu)** — cross-platform for free
- If Android-only and want full Vulkan control: **Path B** — or even easier: skip SDL3 GPU, use raw Vulkan directly via `vkCreateInstance` + `VK_KHR_android_surface`
- If you want something working fast: **Path A**
