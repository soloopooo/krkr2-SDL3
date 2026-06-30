# Vulkan Renderer Backend

> **⚠️ SUPERSEDED — This raw-Vulkan plan was never implemented.**
> 
> Instead, the SDL_Gpu API (which wraps Vulkan/Metal/D3D12) was used as the GPU
> backend. See the actual implementation at:
> - `src/core/visual/gpu/RenderManager_gpu.cpp` (~1000 lines)
> - `src/core/visual/gpu/RenderManager_gpu.h`
> - `src/core/visual/gpu/shaders/` — SPIR-V shaders
>
> **Status:** 50+ blend modes implemented via `SDL_GPUGraphicsPipeline` cache.
> Pipeline count: ~40 SPIR-V variants (vs the 60+ GLES2 shader strings).
>
> For the SDL3 migration history, see [`SDL3_MIGRATION.md`](SDL3_MIGRATION.md).
>
> The original raw-Vulkan design below is kept for reference only.

## 1. Motivation

The engine currently has two `iTVPRenderManager` backends:

| Backend | File | Status |
|---------|------|--------|
| Software (CPU) | `visual/RenderManager.cpp` | Working — pure CPU pixel ops via `tvpgl.h` |
| OpenGL (GLES2) | `visual/ogl/RenderManager_ogl.cpp` | Broken in SDL2 port — ~4000 lines of GLES2 + cocos2d state cache + `#version 100` shaders |

The GLES2 path has chronic issues in the SDL2 port:
- GLES2 is deprecated on modern Android (HAL mapping to Vulkan is lossy)
- `#version 100` GLSL (`attribute`/`varying`/`gl_FragColor`/`texture2D`) is the lowest common denominator
- cocos2d GL state cache (`cocos2d::GL::bindTexture2DN`, `cocos2d::GL::useProgram`) adds indirection and breakage
- Framebuffer fetch extensions (`EXT/ARM/NV_shader_framebuffer_fetch`) are non-portable

Some visual novels use advanced compositing effects (blur, transition, blend modes, grayscale, gamma correction) that benefit from GPU acceleration. Vulkan gives us:
- **Deterministic driver behavior** — no hidden GL state machine
- **Explicit pipeline objects** — cacheable, no runtime shader compilation
- **Cross-platform** — Android (Vulkan 1.0+ mandatory on Android 10+), desktop (MoltenVK on macOS)
- **Clean SPIR-V shader compilation** — pre-compile offline, no GLSL compiler at runtime

## 2. Architecture

### Existing Interface (unchanged)

```
TJS script → Layer compositing → iTVPRenderManager → Vulkan backend → Screen
                                   ↑
                          (abstraction boundary — no change needed)
```

The Vulkan backend implements the same `iTVPRenderManager` / `iTVPTexture2D` / `iTVPRenderMethod` interfaces defined in `visual/RenderManager.h`. The engine core never knows which backend is active.

### New File Layout

```
src/core/visual/vk/                    # New directory, mirrors ogl/
├── RenderManager_vk.cpp               # Main backend: TVPRenderManager_Vulkan
├── RenderManager_vk.h                 # Class declaration
├── vk_common.h                        # Vulkan init, instance, device, queue
├── vk_pipeline.h/.cpp                 # Pipeline cache + shader module management
├── vk_texture.h/.cpp                  # iTVPTexture2D implementation (VkImage-backed)
├── vk_render_method.h/.cpp            # iTVPRenderMethod implementation (VkPipeline-backed)
├── vk_shaders/                        # Pre-compiled SPIR-V shaders
│   ├── copy.vert.spv
│   ├── copy.frag.spv
│   ├── alphablend.frag.spv
│   ├── alphatest.frag.spv
│   ├── addblend.frag.spv
│   ├── boxblur.frag.spv
│   ├── grayscale.frag.spv
│   ├── gamma.frag.spv
│   ├── perspective.vert.spv
│   └── ... (~60 fragment shaders)
└── CMakeLists.txt                     # Globs *.cpp, links vulkan
```

### Registration

```cpp
// RenderManager_vk.cpp — same pattern as GLES2
extern "C" void TVPRegisterVulkanRenderer() {
    TVPRegisterRenderManager("vulkan",
        []() -> iTVPRenderManager* { return new TVPRenderManager_Vulkan; });
}
```

### Pipeline vs. Shader Concept

**GLES2:** 60+ shader strings compiled at runtime → `glUseProgram`

**Vulkan:** Each "shader technique" is a `VkPipeline` object created at init time and cached. The key components:

```
Technique name (e.g. "AlphaBlend")
  → VkShaderModule (vert) + VkShaderModule (frag)
  → VkPipelineShaderStageCreateInfo x2
  → VkPipelineVertexInputStateCreateInfo (fixed: pos + texcoord per texture unit)
  → VkPipelineInputAssemblyStateCreateInfo (VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
  → VkPipelineRasterizationStateCreateInfo (no culling, dynamic viewport)
  → VkPipelineMultisampleStateCreateInfo (no MSAA)
  → VkPipelineDepthStencilStateCreateInfo (disabled)
  → VkPipelineColorBlendAttachmentState (per-technique blend function)
  → VkPipelineLayout (descriptor set layout: 1 combined sampler per texture)
  = VkPipeline (cached in unordered_map)
```

**Dynamic state:** Viewport, scissor, blend constants — set via `vkCmdSet*` at draw time, not baked into pipeline.

## 3. Pipeline Classification

The 60+ GLES2 techniques fall into categories. Each category maps to a small number of Vulkan pipelines.

### Category A: Copy / Fill (no blending, trivial)
| GLES2 Name | Vulkan Pipeline Variant |
|------------|------------------------|
| Copy | `copy` — `gl_FragColor = texture(tex0, uv)` |
| CopyOpaqueImage | `copy_opaque` — forced alpha=1 |
| CopyColor | `copy_color` — opaque copy, RGB only |
| CopyMask | `copy_mask` — alpha channel only |
| FillARGB | `fill` — `gl_FragColor = pushConstantColor` |
| FillColor | `fill_color` — opaque fill |
| FillMask | `fill_mask` — alpha = constant, color discarded |

**Vulkan count: 2 vertex × 4 fragment = 6 pipelines** (vertex varies: passthrough vs perspective)

### Category B: Alpha Blend (standard blend modes, ~15 variants)
| GLES2 Name | Blend Function Equivalent |
|------------|--------------------------|
| AlphaBlend | `VK_BLEND_FACTOR_SRC_ALPHA, ONE_MINUS_SRC_ALPHA` |
| AlphaBlend_a | Same, alpha rgb separate |
| AlphaBlend_color | Color-multiplied variant |
| AlphaTest | `VK_BLEND_FACTOR_ZERO, ONE` + discard |
| AddBlend | `VK_BLEND_FACTOR_ONE, ONE` |
| SubBlend | `VK_BLEND_FACTOR_ONE, ONE` + reverse subtract |
| MulBlend | `VK_BLEND_FACTOR_ZERO, SRC_COLOR` |
| ScreenBlend | `VK_BLEND_FACTOR_ONE, ONE_MINUS_SRC_COLOR` |
| ConstAlphaBlend | `VK_BLEND_FACTOR_CONSTANT_ALPHA, ...` |
| ConstColorAlphaBlend | `VK_BLEND_FACTOR_CONSTANT_COLOR, ...` |
| AdditiveAlphaBlend | `VK_BLEND_FACTOR_ONE, ONE_MINUS_SRC_ALPHA` |

**Blend pipeline variant logic:**
```
VkPipelineColorBlendAttachmentState {
    blendEnable = VK_TRUE;
    // map GL blend func → VkBlendFactor
    srcColorBlendFactor = mapGLToVk(gles2_src_rgb);
    dstColorBlendFactor = mapGLToVk(gles2_dst_rgb);
    colorBlendOp = mapGLToVk(gles2_func_rgb);  // VK_BLEND_OP_ADD / SUBTRACT / REVERSE_SUBTRACT
    srcAlphaBlendFactor = mapGLToVk(gles2_src_a);
    dstAlphaBlendFactor = mapGLToVk(gles2_dst_a);
    alphaBlendOp = mapGLToVk(gles2_func_a);
}
```

**Vulkan count: 2 vertex × 8 fragment = 16 pipelines**

### Category C: Framebuffer Fetch (currently dependent on EXT/ARM/NV extension)
These techniques read the current framebuffer pixel as input. In Vulkan this is **explicit** — use `VK_ATTACHMENT_LOAD_OP_LOAD` with subpass input attachments or a second descriptor for the output image.

| GLES2 Name | Technique |
|------------|-----------|
| AlphaBlend_d | Dest-blending alpha blend |
| ApplyColorMap_d | Dest-blending color map |
| DarkenBlend | `min(src, dst)` |
| LightenBlend | `max(src, dst)` |
| PsOverlayBlend | Photoshop overlay |
| PsHardLightBlend | Photoshop hard light |
| PsSoftLightBlend | Photoshop soft light |
| PsColorDodgeBlend | Photoshop color dodge |
| PsColorBurnBlend | Photoshop color burn |
| PsDiffBlend | Difference blend |
| PsExclusionBlend | Exclusion blend |
| DoGrayScale | Grayscale conversion |

**Vulkan approach:** No extension needed — render to an intermediate texture (or use subpass `VK_ATTACHMENT_LOAD_OP_LOAD` + render pass with input attachment descriptor). The fragment shader reads `tex1` (the existing dest content) as a sampled image.
- **Tradeoff:** Requires an extra descriptor binding (2 samplers per draw instead of 1). Pipeline layout stays the same; descriptor update varies.
- **Pipeline count: 1 - 2 variants** (the blend logic is in the shader, not the fixed-function blend state)

### Category D: Post-Processing (barrier-breaking, not framebuffer-fetch-dependent)
| GLES2 Name | Technique |
|------------|-----------|
| BoxBlur / BoxBlurAlpha | 3×3 box blur (9 texel fetches) |
| AdjustGamma / AdjustGamma_a | Gamma/brightness adjustment |
| DoGrayScale | Grayscale (also in C) |
| AdditiveAlphaToAlpha / AlphaToAdditiveAlpha | Format conversion |

**Pipeline count: 2 vertex × 4 fragment = 8 pipelines**

### Category E: Perspective / Triangle (3D transforms)
| GLES2 Name | Technique |
|------------|-----------|
| PerspectiveAlphaBlend_a | 3×3 matrix transform, then alpha blend |
| OperateTriangles | Arbitrary triangle mesh |

**Vertex shader variant:** Takes `uniform mat3 uMatrix` instead of identity passthrough.

### Summary

| Category | Frag Shaders | Vert Shaders | Pipeline Count |
|----------|-------------|-------------|----------------|
| A: Copy/Fill | 4 | 2 | 6 |
| B: Blend modes | 8 | 2 | 16 |
| C: Framebuffer fetch | 3 | 2 | 6 |
| D: Post-processing | 4 | 2 | 8 |
| E: Perspective | 2 | 2 | 4 |
| **Total** | **21** | **4** | **~40** |

Down from 60 shader strings compiled at runtime to **~40 pre-compiled SPIR-V pipelines**.

## 4. Vulkan Initialization

### Init Sequence (called from `TVPEngineTick` via `InitVK`)

```
1. Create VkInstance (VK_API_VERSION_1_0 min, no layers in release)
2. Pick physical device (first discrete GPU, fallback integrated)
3. Create VkDevice + queues (1 graphics queue, 1 optional compute)
4. Create VmaAllocator (VulkanMemoryAllocator for memory management)
5. Create descriptor pool (large enough for max texture count)
6. Create VkRenderPass (single subpass, VK_ATTACHMENT_LOAD_OP_CLEAR/LOAD)
7. Create VkCommandPool + per-frame command buffers (double-buffered)
8. Create VkDescriptorSetLayout (1 combined image sampler per tex unit)
9. Create VkPipelineLayout (shader stage flags: VS + FS)
10. Load all SPIR-V shaders → VkShaderModule
11. Create all VkPipeline objects (cached)
12. Create VkFence + VkSemaphore for sync
13. Create swapchain (if owning display) or accept external surface
```

### SDL2 Integration

SDL2.0.14 includes `SDL_vulkan.h` — we can:

```cpp
// In WindowLayer_sdl.cpp (or new WindowLayer_vk.cpp):
#include <SDL2/SDL_vulkan.h>

static VkInstance s_instance = VK_NULL_HANDLE;
static VkSurfaceKHR s_surface = VK_NULL_HANDLE;

static bool InitVulkan(SDL_Window *win) {
    unsigned int count;
    SDL_Vulkan_GetInstanceExtensions(win, &count, nullptr);
    // ... create VkInstance with extensions
    SDL_Vulkan_CreateSurface(win, s_instance, &s_surface);
    // ... proceed with device/renderpass/swapchain
}
```

**Display path:** A full-screen textured quad via Vulkan:

```
vkCmdBeginRenderPass(cmd, renderPass, framebuffer, ...)
vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, displayPipeline)
vkCmdBindVertexBuffers(cmd, 0, 1, &quadVbo, &offsets)
vkCmdBindDescriptorSets(cmd, ... &frameDescriptorSet)  // binds the engine output texture
vkCmdDraw(cmd, 6, 1, 0, 0)
vkCmdEndRenderPass(cmd)
vkQueueSubmit(queue, 1, &submitInfo, fence)
vkQueuePresentKHR(queue, &presentInfo)
```

### Software Renderer Fallback

When Vulkan init fails (old device, emulator), fall back to software renderer + CPU blit to SDL display. Selection in `TVPGetRenderManager()`:

```cpp
iTVPRenderManager* TVPGetRenderManager() {
    static iTVPRenderManager *_RenderManager = nullptr;
    if (!_RenderManager) {
        TVPRegisterVulkanRenderer();
        try {
            _RenderManager = TVPGetRenderManager(ttstr("vulkan"));
        } catch (...) {
            _RenderManager = TVPGetRenderManager(ttstr("software"));
        }
    }
    return _RenderManager;
}
```

## 5. Texture Management (iTVPTexture2D → VkImage)

```cpp
class tTVPVKTexture2D : public iTVPTexture2D {
    VkImage m_Image;
    VkDeviceMemory m_Memory;  // managed by VMA
    VkImageView m_ImageView;
    VkDescriptorSet m_DescriptorSet;  // bound to shared descriptor pool
    VkFormat m_Format;
    bool m_IsStatic;

    // CPU-side pixel data (for readback when software renderer needs it)
    std::vector<uint8_t> m_PixelData;
    bool m_Dirty;  // CPU→GPU sync flag

public:
    void Update(const void *pixel, TVPTextureFormat::e format,
                int pitch, const tTVPRect& rc) override;
    void* GetScanLineForWrite(tjs_uint l) override;
    // ...
};
```

**Key decisions:**
- Use `VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL` + `vkCmdCopyBufferToImage` for texture upload (staging buffer)
- After final upload, transition to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`
- For `GetScanLineForRead` (software path reads pixels back), map a staging buffer or keep CPU shadow copy
- For `GetGLTextureName()`, return `0` (not shared with GL)
- For `GetAdapterTexture()`, return `nullptr` (no cocos2d)

## 6. SPIR-V Shader Compilation

**No GLSL at runtime.** All shaders are pre-compiled to SPIR-V and stored as binary resources.

### Workflow
```
shaders/*.vert/.frag (GLSL) 
  → glslangValidator -V -o shaders/*.vert.spv shaders/*.vert
  → binary .spv files committed to repo
  → loaded at init via fread → VkShaderModuleCreateInfo
```

### Pipeline Cache
```cpp
class VKPipelineCache {
    std::unordered_map<uint64_t, VkPipeline> m_Cache;
    VkPipelineCache m_VkCache;  // Vulkan pipeline cache (disk serializable)
    
    VkPipeline GetOrCreate(PipelineKey key);  // hash of technique name + blend state
    void SaveToFile(const char *path);        // persist for fast warm-up
    void LoadFromFile(const char *path);
};
```

Key = hash of `(techniqueName, srcBlend, dstBlend, blendOp, srcAlphaBlend, dstAlphaBlend, alphaBlendOp, alphaToCoverage, texCount)`. Since only blend state and texture count vary (everything else is fixed), collisions should be minimal.

## 7. Render Loop (per-frame)

```
TVPEngineTick():
  . drain events, call Application->Run()
  . TVPDeliverWindowUpdateEvents()  // engine compositing via iTVPRenderManager
  . VK: acquire next swapchain image
  . VK: begin command buffer
  . VK: begin render pass on swapchain framebuffer
  . VK: get composited texture from DrawBuffer
  . VK: bind display pipeline
  . VK: draw fullscreen quad (aspect-ratio correct viewport via dynamic state)
  . VK: end render pass
  . VK: end command buffer
  . VK: queue submit + present
```

**Double buffering:**
```
frame 0: cmdBuf[0], semaphore[0], fence[0]
frame 1: cmdBuf[1], semaphore[1], fence[1]
```
Wait on fence for frame N before re-recording cmdBuf[N].

## 8. Implementation Phases

### Phase 1: Display layer only (Week 1-2)
- `vk_common.h`: Init instance, device, queue, VMA
- `WindowLayer_sdl.cpp`: Add `InitVK()` + fallback path — if Vulkan init succeeds, use Vulkan for the fullscreen quad instead of GLES2
- **No iTVPRenderManager changes yet** — software renderer still compositing
- **Checkpoint:** Vulkan fullscreen quad displaying software-rendered frames
- **GLES2 removed from:** `WindowLayer_sdl.cpp`

### Phase 2: Texture abstraction (Week 2-3)
- `vk_texture.h/.cpp`: `tTVPVKTexture2D` implementation
- Texture upload via staging buffer + `vkCmdCopyBufferToImage`
- `CreateTexture2D` overloads implemented
- **Checkpoint:** Engine creates textures via Vulkan

### Phase 3: Pipeline abstraction (Week 3-5)
- `vk_render_method.h/.cpp`: `tTVPVKRenderMethod` wrapping `VkPipeline`
- `vk_pipeline.h/.cpp`: Pipeline cache, shader module management
- Core techniques: Copy, AlphaBlend, FillARGB, AlphaTest, AddBlend
- `OperateRect` implementation (FBO render-to-texture)
- **Checkpoint:** Basic layer compositing via Vulkan works

### Phase 4: Full technique coverage (Week 5-7)
- Remaining blend modes (~20 more pipeline variants)
- Framebuffer fetch techniques (input attachment approach)
- Post-processing: BoxBlur, AdjustGamma, Grayscale
- Perspective/triangle variants
- **Checkpoint:** Feature parity with GLES2 backend

### Phase 5: Cleanup (Week 7-8)
- Delete `src/core/visual/ogl/RenderManager_ogl.cpp` from SDL build
- Remove cocos2d GL state cache dependency from core
- Remove `GLESv2`/`EGL` from `target_link_libraries`
- Pipeline cache serialization to disk
- **Checkpoint:** Zero GLES2 code in SDL variant, full Vulkan pipeline

## 9. Build System Changes

### `src/core/CMakeLists.txt`

```cmake
# New: Vulkan backend sources
file(GLOB KRKR2CORE_VULKAN
    ${KRKR2CORE_PATH}/visual/vk/*.cpp
)
list(APPEND KRKR2CORE_CODE ${KRKR2CORE_VULKAN})

# Remove GLES2 from SDL build
list(REMOVE_ITEM KRKR2CORE_CODE
    ${KRKR2CORE_PATH}/visual/ogl/RenderManager_ogl.cpp
    ${KRKR2CORE_PATH}/visual/ogl/RenderManager_ogl_test.hpp
)

# Link Vulkan instead of GLES2
target_link_libraries(${PROJECT_NAME} PUBLIC
    vulkan             # Android NDK provides libvulkan.so
    # REMOVE: GLESv2, EGL (SDL2 provides EGL if needed)
)
```

### SPIR-V Shader Build (optional CMake integration)
```cmake
# Custom command to compile GLSL→SPIR-V at build time (requires glslangValidator)
find_program(GLSLANG_VALIDATOR glslangValidator)
if(GLSLANG_VALIDATOR)
    foreach(SHADER_FILE ${VULKAN_SHADER_SOURCES})
        add_custom_command(
            OUTPUT ${SHADER_FILE}.spv
            COMMAND ${GLSLANG_VALIDATOR} -V -o ${SHADER_FILE}.spv ${SHADER_FILE}
            DEPENDS ${SHADER_FILE}
        )
    endforeach()
endif()
```

## 10. Vulkan Pipeline vs. GLES2 Register of Techniques

Since the OGL renderer's `TVPRegisterRenderMethod` compiles techniques from GLSL strings:

```cpp
// GLES2 path (current):
TVPRegisterRenderMethod("AlphaBlend", glsl_script, 2, flags);
// → at runtime: glCompileShader + glLinkProgram

// Vulkan path:
TVPRegisterRenderMethod("AlphaBlend", nullptr, 2, flags);
// → GetRenderMethod("AlphaBlend") returns pre-built pipeline from cache
// The "glsl_script" parameter is unused — pipeline key determines everything
```

The `iTVPRenderMethod::SetBlendFuncSeparate()` call still works — it updates the **pipeline key**; the next call to `OperateRect` triggers a pipeline lookup (or creation if the key hasn't been seen before):

```cpp
VkPipeline vkpipeline = m_PipelineCache->GetOrCreate({
    .techniqueName = method->GetName(),  // e.g. "AlphaBlend"
    .srcRGB = mapToVk(srcRGB),
    .dstRGB = mapToVk(dstRGB),
    .srcAlpha = mapToVk(srcAlpha),
    .dstAlpha = mapToVk(dstAlpha),
    .texCount = textureCount,
});
vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vkpipeline);
```

## 11. Device Compatibility

### Minimum requirements
- **Android:** Vulkan 1.0 (required on all Android 10+ devices; optional but near-universal on Android 7+)
- **Desktop:** Vulkan 1.0 via MoltenVK on macOS, native on Windows/Linux
- **Fallback:** Software renderer when Vulkan init fails

### Feature requirements (none exotic)
- `VK_KHR_swapchain` (display)
- `VK_KHR_get_physical_device_properties2` (Android)
- Optional: `VK_EXT_descriptor_indexing` (for >1024 textures)

## 12. State that Lives Per-RenderManager

| State | GLES2 | Vulkan |
|-------|-------|--------|
| Render target | `glBindFramebuffer` call | `vkCmdBeginRenderPass` with subpass/attachment |
| Blend func | `glBlendFuncSeparate` global state | Per-pipeline in `VkPipelineColorBlendAttachmentState` |
| Viewport | `glViewport` global state | `vkCmdSetViewport` dynamic state |
| Scissor | `glScissor` global state | `vkCmdSetScissor` dynamic state |
| Texture bindings | `glActiveTexture(GL_TEXTURE0+N) + glBindTexture` | Descriptor sets (update per-frame) |
| Shader program | `glUseProgram` | `vkCmdBindPipeline` |
| Clear color | `glClearColor` global state | `VkClearValue` in `VkRenderPassBeginInfo` |

## 13. Descriptor Management

**Layout:** One `VkDescriptorSetLayout` with N combined image samplers (N = max simultaneous textures). The engine typically uses 1-3 textures per `OperateRect` call.

```
layout(set = 0, binding = 0) uniform sampler2D tex0;
layout(set = 0, binding = 1) uniform sampler2D tex1;
layout(set = 0, binding = 2) uniform sampler2D tex2;
// up to N bindings (N=8 for perspective)
```

**Allocation:** One `VkDescriptorPool` with `VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER * N * FRAMES_IN_FLIGHT` pools. Descriptor sets updated per `OperateRect` call (write before submit).

For the simple display quad path, a single pre-baked descriptor set is reused every frame (binding = engine composited texture).

## 14. Timeline

| Phase | Deliverable | Effort |
|-------|-------------|--------|
| 1 | Vulkan display quad (replaces GLES2 in WindowLayer_sdl.cpp) | 1-2 weeks |
| 2 | Vulkan texture backend (CreateTexture2D, Update) | 1 week |
| 3 | Core pipelines (Copy, AlphaBlend, Fill) + OperateRect | 2 weeks |
| 4 | Full technique coverage (~40 pipelines) | 2-3 weeks |
| 5 | Cleanup: delete GLES2 code, pipeline cache serialization | 1 week |
| **Total** | | **~8 weeks** |

Phases 1-3 produce visible output on screen. Phase 1 alone eliminates the GLES2 dependency from the display path.
