# GPU 渲染管线 Bug 诊断报告 & 修复计划

---

## 一、当前修复进度（已合入代码）

| Bug | 描述 | 状态 |
|-----|------|------|
| ① Gray 超读 | Gray 格式源数据按 4BPP 拷贝导致堆越界读 | ✅ **已修复** — `Update()` 按格式展开 Gray→RGBA |
| ② m_pixels 不同步 | GPU 纹理上传后 CPU 端 `m_pixels` 始终为零 | ✅ **已修复** — `Update()` 末尾同步写入 `m_pixels` |
| ④ 清除颜色不透明黑 | `SetRenderTarget` 第一帧清除为 `(0,0,0,1)` 而非 `(0,0,0,0)` | ✅ **已修复** — `clear_color` 改为 `(0,0,0,0)` |
| ⑥ CreateTexture2D 不复制 | `new(w,h,old)` 重载不复制内容 → 静默数据丢失 | ✅ **已修复** — GPU copy pass 复制旧纹理到新纹理 |
| ⑧ IsGPU() static cache | `IsGPU()` 函数内 `static bool` 首次调用后永久缓存 | ✅ **已修复** — 移除 `static`，每次重新求值 |
| ⑨ fastGPURoute static cache | `InternalBlendText` 的 `static bool fastGPURoute` 永久缓存 | 🔶 **部分修复** — `fastGPURoute` 已改为非 static，`GEMTHOD_OPA_CLR` macro 的 static 仍存在 |
| ⑤ ApplyColorMap 无自定义着色器 | `ApplyColorMap` 变体不使用自定义 shader，无颜色参数 | ✅ **已修复** — `apply_colormap.frag` + `apply_colormap_a.frag`，binding=1 UBO 传入 `text_color` |
| ⑦b quad.frag opacity 仅影响 alpha | 预乘混合（`ONE/ONE_MINUS_SRC_ALPHA`）时 RGB 未乘 opacity | 🔶 **部分修复** — `quad_pma.frag` 对 `AdditiveAlphaBlend_a` 正确，但对 `AlphaBlend_a` **错误**（见 Bug #3）：kirikiri 纹理是直通 alpha，`quad_pma` 做完整预乘导致 RGB 过度贡献 |
| ③ crossfade alpha=1.0 | `crossfade.frag` 强制 `FragColor.a = 1.0` | ✅ **已修复** — 已删除 `FragColor.a = 1.0`，alpha 自然混合 |
| — Photoshop 混合近似 | `PsOverlay/HardLight/SoftLight` 等用固定函数混合近似 | ✅ **PsOverlayBlend 已修复** — 2-pass dest-read 框架 + `ps_overlay.frag`；其他 PsBlend 用同样模式添加 |
| — 渲染 Pass 效率 | 每次 `OperateRect` 独立 BeginPass/EndPass，tile-based GPU 反复冲刷 | ✅ **已修复** — 同目标自动批处理，跳过 EndPass→BeginPass 循环；添加源=目标安全守卫 |
| — Dest-alpha (_d) 变体 | 无 opacity-on-opacity LUT，无 subpass input | ❌ **推迟** — 2-pass 框架已就绪，需逐个添加着色器 |
| — R8 灰度纹理 | Gray 纹理仍为 RGBA8（4字节/像素），未迁移至 R8_UNORM | ❌ **推迟** — SDL3 无 sampler swizzle，需 shader 侧 `sampledColor.rrrr` |
| — AssignTexture identity | GPU 路径跳过 `AssignTexture` 避免指针别名 → 额外 GPU copy | ❌ **未修复** |

---

## 二、架构概览：GPU 渲染器与软件渲染器的对比

### 2.1 渲染管线总览

```
LayerIntf.cpp  (GPU/软件 调度)
  │  IsGPU() → Draw_GPU / InternalComplete2_GPU
  │  Complete() → tCompleteDrawable_GPU (BltImage)
  │           vs tCompleteDrawable      (CopyRect)
  ▼
RenderManager_gpu.cpp  (1455 行)
  │  ~30 个 SDL_GPUGraphicsPipeline（每种混合方法一个）
  │  BlendState: 固定函数混合近似
  │  Shaders: 12 个 GLSL → SPIR-V 着色器
  ▼
SDL3 GPU API → Vulkan/Metal/D3D12 后端
```

### 2.2 软件渲染器 vs GPU 渲染器关键算法差异

#### 纹理 A8 (Gray) 格式

| 渲染器 | 格式 | 行为 |
|--------|------|------|
| 软件 | 8bpp Gray（1 字节/像素） | 按需通过 `GetPoint()`查表展开为颜色 |
| GPU（已修复） | RGBA8（4 字节/像素） | CPU 端逐像素展开：`(g, g, g, g)` 写入所有通道 |

**问题:** 当前的 `(g,g,g,g)` 展开意味着 `AlphaBlend(SRC_ALPHA/ONE_MINUS_SRC_ALPHA)` 时 `result = (g,g,g) * g/255 + dest * (1-g/255)`，与软件的 `result = dest + (g - dest) * g / 256` 略有差异（除数 255 vs 256），但在 8-bit 整数精度下误差 ≤1，肉眼不可分辨。

#### 混合函数覆盖面

| 软件 | GPU |
|------|-----|
| ~50 个 CPU 像素混合函数（含所有变体 `_d`, `_a`, `_o`, `_HDA`, `_do`, `_ao` 等） | ~15 个固定函数混合配置 + 6 个自定义着色器 |
| Photoshop 混合：每像素 LUT / 公式精确实现 | Photoshop 混合：固定函数近似（**视觉上错误**） |
| 4-lane 分离 Alpha 通道的精确逐像素数学运算 | 通过 `glBlendEquationSeparate`（RGB 与 Alpha 分离）的 Vulkan 等价近似 |
| `_d` 变体：opacity-on-opacity 查找表（64KB `TVPOpacityOnOpacityTable`） | `_d` 变体：标准 alpha 混合（**无 opacity-on-opacity**） |

#### 层合成

| 方面 | 软件路径 | GPU 路径 |
|------|---------|---------|
| `DrawCompleted`（缓存） | `Bitmap->CopyRect()`（原始 memcpy，无混合） | `BltImage()` → 基于着色器的正确 alpha 合成 |
| `DrawCompleted`（主层） | `DrawBuffer->Blt()` → CPU 像素循环 | `DrawBuffer->Blt()` → `OperateRect` → GPU draw call |
| 不透明度=255 时子层绘制到父层 | 有子层时总是使用临时缓冲区 | 有子层时总是使用临时缓冲区（**KrKr2-Next 的对立实现：当 opa=255 时直接绘制**） |

### 2.3 参考项目对比

| 方面 | krkrz（Win32） | KrKr2-Next（ANGLE） | Yuri OGL（cocos2d） | **Yuri GPU（SDL3）** |
|------|---------------|---------------------|---------------------|----------------------|
| 渲染后端 | GDI / CPU | GLES2 通过 ANGLE | GLES2 通过 cocos2d | Vulkan 通过 SDL3 |
| 着色器语言 | 无（仅 CPU） | GLSL（运行时编译） | GLSL（运行时编译） | GLSL → SPIR-V（预编译） |
| 混合着色器 | ~50 CPU 函数 | ~50 GLSL 程序 | ~50 GLSL 程序 | **6 SPIR-V + 固定函数** |
| ApplyColorMap | CPU lerp 带颜色 | GLSL 带颜色 Uniform | GLSL 带颜色 Uniform | **破损 — 无颜色 Uniform，文字将显示黑色** |
| Photoshop 混合 | 每像素 LUT | 完整 GLSL 着色器 | 完整 GLSL 着色器 | **PsOverlayBlend 已修复（2-pass dest-read），其他待添加** |
| Crossfade 变体 | 3 个变体（`_d`,`_a`,S） | 3 个 GLSL 变体 | 3 个 GLSL 变体 | **1 个着色器，alpha=1 bug** |
| 纹理压缩 | 无 | ETC2/PVRTC/ASTC | ETC2/PVRTC/ASTC | 无 |
| 超大纹理分片 | 无 | 有（`tTVPOGLTexture2D_split`） | 有（`tTVPOGLTexture2D_split`） | 无 |
| 渲染 Pass 批处理 | N/A | GL 批处理 | GL 批处理 | **可能每次操作一次 pass** |
| 目标 Alpha（`_d`） | opacity-on-opacity LUT | framebuffer_fetch / 2-pass | framebuffer_fetch / 2-pass | **无支持** |
| 子 pass 输入 | N/A | GL_EXT_shader_framebuffer_fetch | GL_EXT_shader_framebuffer_fetch | **SDL3 不支持** |

---

## 三、各 Bug 详细描述

### Bug ① — Gray 格式超读（内存安全）

**严重性：** CRITICAL ✅ **已修复**

**修复方案（已实施，`RenderManager_gpu.cpp:108-118`）：**
```cpp
// Update() 检测 format == Gray，CPU 端逐像素展开 Gray→RGBA
if (format == TVPTextureFormat::Gray) {
    for (int y = 0; y < h; y++) {
        const uint8_t *srcRow = (const uint8_t*)pixel + y * pitch;
        uint32_t *dstRow = (uint32_t*)map + y * w;
        for (int x = 0; x < w; x++) {
            uint8_t g = srcRow[x];
            dstRow[x] = 0xFF000000 | (g << 16) | (g << 8) | g;
        }
    }
}
```

**未来优化方向：** 将 Gray 纹理切换为 `R8_UNORM` 格式（单通道，1 字节/像素），节省 75% VRAM。此时需要一个带有 swizzle `R→RGBA(R,R,R,R)` 的专用采样器，以自动将单通道扩展为 RGBA。ApplyColorMap 着色器仍然读取 `.r` 通道作为掩码值——swizzle 使其在所有四个通道中都可用。

---

### Bug ② — CPU 端 m_pixels 永远不同步

**严重性：** LOW ✅ **已修复**

**修复方案（已实施，`RenderManager_gpu.cpp:133-146`）：**
`Update()` 末尾现在将展开后的 RGBA 数据（与上传到 GPU 的相同）回写到 `m_pixels`。这样 `GetScanLineForRead/GetPoint` 返回正确的值。

---

### Bug ③ — `crossfade.frag` 强制 alpha=1.0

**严重性：** MEDIUM ✅ **已修复**

**修复前（`crossfade.frag:17`）：**
```glsl
FragColor = mix(c1, c2, uP.opacity);
FragColor.a = 1.0;  // ← BUG: 强制不透明
```

**修复后：** 已删除 `FragColor.a = 1.0`，alpha 通道自然通过 `mix(c1.a, c2.a, opacity)` 混合。`_d` 和 `_a` 变体使用同一着色器（各自混合配置负责 alpha 行为）。

**注：** 两个纹理共用一个 UV 坐标（`vUV * uvScale + uvOffset`）。在正常使用场景中两个 ConstAlphaBlend_SD 的输入纹理尺寸相同，因此共用一个 UV 不会导致采样问题。

---

### Bug ④ — 清除颜色为不透明黑

**严重性：** MEDIUM ✅ **已修复**

**修复方案（已实施，`RenderManager_gpu.cpp:1033`）：**
`clear_color` 改为 `{0.0f, 0.0f, 0.0f, 0.0f}`（透明黑）。

---

### Bug ⑤ — ApplyColorMap 变体没有自定义着色器

**严重性：** HIGH ✅ **已修复**

**修复方案：**

1. 新增两个 GLSL 着色器：
   - `apply_colormap.frag` — 标准版：输出 `(text_color.rgb, mask * opacity)`，使用 `SRC_ALPHA/ONE_MINUS_SRC_ALPHA` 混合（RGB），`ZERO/ONE`（A 保持目标 alpha）
   - `apply_colormap_a.frag` — 预乘版：输出 `(text_color.rgb * mask * opacity, mask * opacity)`，使用 `ONE/ONE_MINUS_SRC_ALPHA` 混合

2. `text_color` 通过 binding=1 UBO 传入着色器（`ColorParams { vec4 text_color; }`）

3. 颜色流：TJS `drawText()` → `TVP_REVRGB(color)` → `SetParameterColor4B(-1, clr)` → `m_constColor[]` → OperateRect 推送到 binding=1 UBO

4. `_GetOrCreateMethod` 中 `strstr(name, "ApplyColorMap")` 选择对应自定义着色器

5. 混合配置：`ApplyColorMap_a` 使用 `ONE/ONE_MINUS_SRC_ALPHA`（预乘），其他变体使用 `SRC_ALPHA/ONE_MINUS_SRC_ALPHA, ZERO/ONE`

**文件变更：**
- `src/core/visual/gpu/shaders/apply_colormap.frag` (新增)
- `src/core/visual/gpu/shaders/apply_colormap_a.frag` (新增)
- `src/core/visual/gpu/shaders/shaders_inc.h` (重新生成)
- `src/core/visual/gpu/shaders/build_shaders.sh` (更新)
- `src/core/visual/gpu/RenderManager_gpu.h` (新增 `m_fs_applyColorMap`, `m_fs_applyColorMap_a`)
- `src/core/visual/gpu/RenderManager_gpu.cpp`: `Init()`, `Shutdown()`, `_getMethodBlend()`, `_GetOrCreateMethod()`, `OperateRect()` (binding=1 UBO 推送)

---

### Bug ⑥ — `CreateTexture2D(w, h, tex)` 不复制内容

**严重性：** CRITICAL ✅ **已修复**

**修复方案（已实施，`RenderManager_gpu.cpp:967-995`）：**
`CreateTexture2D(neww, newh, tex)` 现在是 GPU copy pass（`SDL_UploadToGPUTexture` 旧纹理 + 重新上传到新纹理），可复制内容。共享的 bitmap 在 `Independ()` 时不再丢失数据。

---

### Bug ⑦ — `AlphaBlend_a` 混合配置 + Quad Shader 不处理预乘 RGB×opacity

**严重性：** HIGH ✅ **已修复**

**Bug ⑦a：** ✅ **已修复** — 混合状态从 `CONSTANT_COLOR` 改为 `ONE/ONE_MINUS_SRC_ALPHA`（`RenderManager_gpu.cpp:622-628`）。

**Bug ⑦b：** ✅ **已修复** — 新增 `quad_pma.frag`（`FragColor.rgb *= uP.opacity; FragColor.a *= uP.opacity`），预乘混合专用。所有 `_a` 后缀方法（`AlphaBlend_a`, `AdditiveAlphaBlend_a`, `PerspectiveAlphaBlend_a` 等）在 `_GetOrCreateMethod` 中自动选择 `m_fs_pma`。

`quad.frag`（标准混合 `SRC_ALPHA/ONE_MINUS_SRC_ALPHA`）保持不变：仅对 alpha 应用 opacity 是正确的，因为混合单元使用 `SRC_ALPHA` 作为 RGB 因子，间接缩放源颜色贡献。

---

### Bug ⑧ — `IsGPU()` Static Cache

**严重性：** HIGH ✅ **已修复**

**修复方案（已实施，`LayerIntf.cpp:67-70`）：**
移除了 `static bool isGPU` 局部变量。函数现在每次调用都重新求值 `TVPIsSoftwareRenderManager()`。

---

### Bug ⑨ — `fastGPURoute` / `GEMTHOD_OPA_CLR` Static Cache

**严重性：** CRITICAL 🔶 **部分修复**

**Bug ⑨a：** ✅ 已修复 — `fastGPURoute` 改为非 static（`LayerBitmapImpl.cpp:860`）。

**Bug ⑨b：** ❌ 未修复 — `GEMTHOD_OPA_CLR` macro 仍然使用 `static` 指针：
```cpp
#define GEMTHOD_OPA_CLR(n) \
    static iTVPRenderMethod *_method = TVPGetRenderManager()->GetRenderMethod(#n); \
    static int _opa_id = _method->EnumParameterID("opacity"); \
    static int _clr_id = _method->EnumParameterID("color");
```
如果首次调用发生在 GPU 渲染器注册之前（在静态初始化期间），这些指针永远指向错误的渲染器。需要替换为动态查找（但可能会引入小的每帧开销）或配备渲染器切换回调。

---

## 四、综合修复计划

### P0 阶段 — ApplyColorMap 着色器 + R8 Gray 格式

**目标：** 使文字渲染正确工作（这是当前 GPU 渲染器最可见的缺陷）。

**改动文件：**

| 文件 | 变更 |
|------|------|
| `src/core/visual/gpu/RenderManager_gpu.h` | 添加 `text_color` uniform 跟踪，添加 `m_GraySampler` 成员，添加 `R8_UNORM` 常量 |
| `src/core/visual/gpu/RenderManager_gpu.cpp` | `CreateTexture2D` 约 20 行，`Update()` 约 15 行，`CreateSampler()` 约 5 行，新的 `ApplyColorMap` 着色器注册约 10 行 |
| `src/core/visual/gpu/shaders/apply_colormap.frag` | **新建** — GLSL 片段着色器 |
| `src/core/visual/gpu/shaders/shaders_inc.h` | **重新生成** — 嵌入新的 SPIR-V |
| `src/core/visual/gpu/shaders/build_shaders.sh` | 添加 `apply_colormap.frag` 的编译 |

#### R8 Gray 格式设计

**当前（Bug ① 修复后）：**
```cpp
// Update(): Gray → CPU 端逐像素展开 RGBA
pixel[0] = g; pixel[1] = g; pixel[2] = g; pixel[3] = g;  // 4 字节/像素
```

**新方案：**
1. `CreateTexture2D` 对 `Gray` 格式使用 `SDL_GPU_TEXTUREFORMAT_R8_UNORM`（1 字节/像素）
2. `Update()` 直接上传原始 1BPP 数据——无需 CPU 展开
3. `m_GraySampler`：带有 swizzle `{R,R,R,R}` 的独立采样器（将单通道映射到 RGBA）。在初始化时创建。
4. 所有现有着色器（quad.frag 等）保持不变——它们通过采样器读取，swizzle 透明地处理扩展。

**采样器 swizzle 语义（`(R,R,R,R)` 带 quad.frag）：**
- 纹理采样返回 `(g, g, g, g)` 其中 g∈[0,1]
- `FragColor` = `(g, g, g, g)`
- 混合 `SRC_ALPHA/ONE_MINUS_SRC_ALPHA`：`result = (g,g,g)*g + dst*(1-g)`（在 [0,1] 范围内）
- 8-bit 整数运算：`floor((g²·255 + dst·(255-g·255)) / 255)`
- 软件 AlphaBlend 对灰色：`floor((g² + dst·(256-g)) / 256)`
- 差异 ≤1（8-bit 除法器 255 vs 256）——肉眼不可分辨。

#### ApplyColorMap 着色器

```glsl
// apply_colormap.frag — 新建
#version 450
layout(location = 0) in vec2 fragTexCoord0;
layout(location = 0) out vec4 FragColor;

layout(set = 2, binding = 0) uniform sampler2D uTex;
layout(std140, set = 0, binding = 0) uniform Params {
    float opacity;
    float pad[3];
    vec4  text_color;   // (R, G, B, A)
    float gamma[4];
} uP;

void main() {
    float mask = texture(uTex, fragTexCoord0).r;  // R8 单通道 — swizzle 使其在所有四个通道中都可用
    FragColor = vec4(uP.text_color.rgb * mask, mask * uP.opacity);
}
```

采样器使用相同的 `(R,R,R,R)` swizzle — 所有 4 个通道给出相同的掩码值，`.r` 访问等价于任何通道。

**注册：**
```cpp
// ApplyColorMap: 标准 alpha 混合
RegisterMethod("ApplyColorMap", quad_vert, apply_colormap_frag,
    SDL_GPU_BLENDFACTOR_SRC_ALPHA, SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
    SDL_GPU_BLENDFACTOR_ZERO, SDL_GPU_BLENDFACTOR_ONE);

// ApplyColorMap_a: 预乘 alpha 混合
RegisterMethod("ApplyColorMap_a", quad_vert, apply_colormap_frag,
    SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
    SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA);

// ApplyColorMap_d: dest-alpha（留待 P3 — 需要 2-pass 读取目标）
```

#### `text_color` Uniform 传播

现有 UBO 结构体已经有 `float color[4]`（16 字节，偏移量 16）。当前代码在绘制调用之前只写入 `opacity`。需要追踪 `text_color` 从 TJS `Layer.drawText()` → `SetParameterColor4B` → `m_constColor` → UBO 偏移量 16 的传播路径。

---

### P1 阶段 — 着色器修复

#### P1a：拆分 quad.frag → quad.frag + quad_pma.frag

**当前 quad.frag：**
```glsl
FragColor = texture(uTex, uv);
FragColor.a *= uP.opacity;  // 仅调制 alpha
```

**新 quad.frag**（用于标准混合：`SRC_ALPHA/ONE_MINUS_SRC_ALPHA`）：
```glsl
FragColor = texture(uTex, uv);
FragColor.a *= uP.opacity;
// RGB 有意不乘 — 混合单元使用 SRC_ALPHA 因子
// 所以 FragColor.rgb * FragColor.a 已经按 alpha*opacity 调制
```
**不变。** 当前行为对于标准 alpha 混合是正确的。

**新 quad_pma.frag**（用于预乘混合：`ONE/ONE_MINUS_SRC_ALPHA`）：
```glsl
FragColor = texture(uTex, uv);
FragColor.rgb *= uP.opacity;  // 调制 RGB，因为混合不会！
FragColor.a *= uP.opacity;
```
为 `AlphaBlend_a`、`AdditiveAlphaBlend_a`、`ConstAlphaBlend_a` 等注册。

#### P1b：修复 crossfade.frag + 添加变体

**修复方案：**
- 移除 `FragColor.a = 1.0` — alpha 自然混合为 `mix(c1.a, c2.a, opacity)`
- 为每个纹理使用独立的 UV 坐标（`fragTexCoord0` 用于 tex0，`fragTexCoord1` 用于 tex1）
- 为 `ConstAlphaBlend_SD_d` 添加 `crossfade_d.frag`（需要 2-pass 读取目标 alpha — 留待 P3；近似使用相同的线性 lerp）
- 为 `ConstAlphaBlend_SD_a` 添加 `crossfade_a.frag`（预乘变体 — 带 `ONE/ONE_MINUS_SRC_ALPHA` 混合的 `mix(c1, c2, opacity)`）

#### P1c：Photoshop 混合着色器（8 个新着色器）

**问题：** 当前 Photoshop 混合近似使用固定函数混合状态，在视觉上是不正确的：

| 方法 | 当前 GPU（固定函数） | 软件（每像素公式） | 正确？ |
|------|----------------------|---------------------|--------|
| PsOverlayBlend | 屏幕混合近似 | `if(dark) mul else screen` 每通道 | **否** |
| PsHardLightBlend | 屏幕混合近似 | `if(dark) mul else screen`（与 overlay 相反） | **否** |
| PsSoftLightBlend | 屏幕混合近似 | 64KB LUT + 公式 | **否** |
| PsColorDodgeBlend | 与 MulBlend 相同（？） | 64KB LUT | **否** |
| PsColorBurnBlend | 与 MulBlend 相同（？） | 64KB LUT | **否** |
| PsLightenBlend | `MAX` blend op | `max(d, s)` 每通道 | ⚠️ 近似但不完全相同 |
| PsDarkenBlend | `MIN` blend op | `min(d, s)` 每通道 | ⚠️ 近似但不完全相同 |
| PsDiffBlend | `REVERSE_SUBTRACT` | `abs(d - s)` 每通道 | **否** |
| PsExclusionBlend | `REVERSE_SUBTRACT` | `d + s - 2*d*s/255` 每通道 | **否** |

**修复方案：** 为每个 Photoshop 混合创建一个专用的片段着色器。每个都需要一个 **2-pass** 方法：
1. `CopyPass(target → temp_dest_tex)` — 将目标复制为可读纹理
2. `BeginRenderPass(target, LOAD)` — 用 Photoshop 混合着色器重新渲染
3. 着色器读取 `uTex0`（源）和 `uTex1`（目标副本）并计算每像素 Photoshop 公式

每个着色器使用 `ONE/ZERO` 混合状态（着色器内部计算完整结果）。

**SDL3 子 pass 输入研究：** SDL 3.4.10 的 GPU API 在 `SDL_gpu.h` 中**没有子 pass 概念**。没有 `InputAttachment` 支持，也没有 `framebuffer_fetch` 等价物。对于所有需要目标读取的操作（`_d` 变体、Photoshop 混合），2-pass 方法是唯一的途径。

---

### P2 阶段 — 渲染 Pass 批处理架构

**问题：** 当前代码每次 `OperateRect` 调用执行 `BeginPass → DrawQuad → EndPass`。在基于图块的 GPU 上（所有 Android GPU：Mali、Adreno、PowerVR），每个 EndPass 都会将图块冲刷到主内存，下一个 BeginPass 再重新加载它们。每帧发生 20-50 次。

**设计目标：** 将对同一渲染目标的所有绘制调用批处理到一个渲染 pass 中。在自然的合成边界（`Draw_GPU` 结束，`CopyPass` 操作之前）刷新。

**数据结构：**

```cpp
struct BatchedOp {
    SDL_GPUGraphicsPipeline *pipeline;
    SDL_Rect viewport;
    float ubo_data[12];     // UBO 参数的副本
    SDL_GPUTexture *textures[3];
    SDL_GPUSampler *sampler;
    SDL_GPUBufferBinding vertex_binding;
};

class TVPRenderManager_GPU {
    std::map<SDL_GPUTexture*, std::vector<BatchedOp>> m_BatchedOps;
    bool m_Batching = false;
    
    void BeginBatch();
    void FlushBatch(SDL_GPUTexture *target = nullptr);
    // OperateRect: 当批处理时入队，否则立即执行
};
```

**LayerIntf.cpp 集成：**

```cpp
void tTJSNI_BaseLayer::Draw_GPU(...) {
    bool isRootBatch = !mgr->IsBatching();
    if (isRootBatch) mgr->BeginBatch();
    
    // ... 现有代码：CopySelfForRect，绘制子层，BltImage ...
    // 所有 OperateRect 调用入队
    
    if (isRootBatch) mgr->FlushBatch(target_texture);
}
```

**刷新逻辑（在 `RenderManager_gpu.cpp` 内）：**

```cpp
void TVPRenderManager_GPU::FlushBatch(SDL_GPUTexture *target) {
    for (auto& [render_target, ops] : m_BatchedOps) {
        if (target && render_target != target) continue;
        
        SDL_BeginGPURenderPass(cmd, &color_target, ...);
        SDL_GPUGraphicsPipeline *current_pipeline = nullptr;
        
        for (auto& op : ops) {
            if (op.pipeline != current_pipeline) {
                SDL_BindGPUGraphicsPipeline(cmd, op.pipeline);
                current_pipeline = op.pipeline;
            }
            SDL_SetGPUViewport(cmd, &op.viewport);
            SDL_SetGPUScissor(cmd, &op.viewport);
            // 推送 uniforms，绑定纹理，绘制
        }
        
        SDL_EndGPURenderPass(cmd);
    }
    m_BatchedOps.clear();
}
```

**刷新时机（必须刷新之前的时刻）：**
1. 在 `CopyPass` 操作之前（copy-to-texture 用于 Photoshop 混合 / dest-alpha）
2. 在将渲染目标作为采样器纹理读取之前
3. 在 `DrawCompleted` 将临时缓冲区合成到最终目标（不同的渲染目标）之前
4. 帧结束时

**状态跟踪优化：**
- 跟踪 `current_pipeline`（未变化时跳过 `BindGraphicsPipeline`）
- 将 UBO 数据与之前的值比较，仅在变化时使用 `PushGPUVertexUniformData`
- 每个 op 缓存 viewport+scissor

**风险：** 批处理改变了 pass 内的绘制顺序。如果一个 `OperateRect` 从当前渲染目标的子矩形读取（自读），批处理会给出陈旧的结果。这在当前合成流程中**不会发生**（所有 OperateRect 从不同的纹理读取，而不是目标 — `OperateRect` 内部的 `DuplicateIfSame` 检查已经通过先复制到临时纹理来处理自读情况，这会强制刷新）。

---

### P3 阶段 — 功能完善

#### P3a：Dest-Alpha（`_d`）变体（通过 2-Pass）

对于 `AlphaBlend_d`、`ApplyColorMap_d`、`ConstAlphaBlend_d`、`Crossfade_d`：

**2-pass 流程：**
1. `CopyPass: render_target → temp_dest_tex`（标记为 `SDL_GPU_TEXTUREUSAGE_SAMPLER`）
2. `BeginRenderPass(render_target, LOAD)`
3. 绑定读取 `uTex0`（源）+ `uTex1`（temp_dest_tex = 目标）的着色器
4. 着色器使用 UBO 中的打包 LUT 或计算出的公式计算 opacity-on-opacity：`effective_opa = lookup(tex0.a, tex1.a)`
5. 绘制四边形
6. `EndRenderPass`

LUT：`TVPOpacityOnOpacityTable[256*256]`（64KB）和 `TVPNegativeMulTable[256*256]`（64KB）可以打包到一个 UBO 或一个小的 1D 纹理中。

#### P3b：Gamma 着色器对齐

将 `gamma.frag` 替换为与软件 `TVPAdjustGamma` 匹配的每像素 `pow()`：

```glsl
// 匹配软件 TVPAdjustGamma 的每通道 gamma
float value = texture(uTex, uv).r;  // 每通道
float normalized = value;  // [0,1]
float adjusted = pow(normalized, 1.0 / uP.gamma_channel) * uP.amplitude + uP.floor;
```

或者将每个通道的 256 条目 LUT 预计算到 UBO 中（768 个浮点数），以精确匹配软件输出。

#### P3c：超大位图的拆分纹理

从 KrKr2-Next OGL 的 `tTVPOGLTexture2D_split` 移植：
- 当图像宽度 > GPU 最大纹理大小时，创建平铺的子纹理
- `GetScanLineForRead` 从正确的子纹理返回像素
- 按 (X, Y) 区域坐标键控的纹理缓存

#### P3d：`AssignTexture` 身份优化

不执行 `OperateRect` 加 Copy 着色器（避免不必要的 GPU blit），而是直接存储 `SDL_GPUTexture*` 引用：

```cpp
void AssignTexture(iTVPTexture2D *tex) {
    m_GPUTexture = static_cast<tTVPGPUTexture2D*>(tex)->GetSDLTexture();
    m_OwnsTexture = false;  // 析构时不释放
    // 标记为"共享" — 跳过 update，跳过 copy
}
```

需要向 `tTVPGPUTexture2D` 添加 `SDL_GPUTexture*` 访问器和共享所有权标志，以及对 `LayerBitmapIntf.cpp` 的 GPU `AssignTexture` 路径进行小幅重构（当前在 `LayerBitmapIntf.cpp:947` 跳过了 AssignTexture，回退到 `OperateRect`）。

---

## 五、实现顺序与依赖关系

```
P0（ApplyColorMap + R8）
 │
 ├── P1a（quad split）── 独立
 ├── P1b（crossfade 修复）── 独立
 ├── P1c（Photoshop 着色器）── 依赖 P2（需要 CopyPass 读取目标）
 │
 ├── P2（批处理架构）
 │    │
 │    └── P1c（Photoshop 着色器）── 在批处理内使用 2-pass
 │
 └── P3（完善）── 构建于 P2 + P1 之上
      ├── 3a（dest-alpha）── 依赖 P2（2-pass 基础设施）
      ├── 3b（gamma 对齐）── 独立
      ├── 3c（拆分纹理）── 独立
      └── 3d（AssignTexture 优化）── 依赖 P2（批处理）
```

**推荐执行顺序：**
1. **P0 优先**（ApplyColorMap + R8）—— 解除文字渲染阻塞，这是最可见的破损功能
2. **P1a + P1b 并行**（独立的着色器修复）
3. **P2**（批处理架构）—— 为高效的 Photoshop 混合奠定基础
4. **P1c**（Photoshop 着色器）—— 在 P2 稳定之后
5. **P3** — 任意顺序

---

## 六、调试建议

### RenderDoc 捕捉

项目中集成了 RenderDoc 应用内捕捉 API（`RenderManager_gpu.cpp:825-866`）。AndroidManifest 中的 RenderDoc Activity 入口已注释掉，但 `TriggerRenderDocCapture()` 可通过 JNI overlay 触发。

建议步骤：
1. 用 RenderDoc Android 层启动 APK
2. 触发帧捕捉
3. 检查每个 `OperateRect` 调用的输入/输出纹理
4. 特别关注 `AlphaBlend_a`、`ApplyColorMap_d`、`ConstAlphaBlend_SD_*` 的混合结果

### Log 分析

`OperateRect()` (line 1007-1029) 记录每帧前 30 次 + 所有 Alpha/ConstAlpha/Copy/Fill 操作。查看 logcat `##krkr` 标签的输出。

### 关键检查点

- `setParameterOpa` 是否被调用（`m_opacity` 默认 255）
- `SetParameterColor4B` 是否被调用（`m_hasConstantColor`）
- `SetRenderTarget` 的 `m_frameFirstTarget` 状态
- `Update()` 的 `format` 参数和实际源 BPP

---

## 七、深度代码审查：全部 27 项 Bug 清单

经对比软件渲染器（`tvpgl.cpp`）、参考 OGL 渲染器（KrKr2-Emu `RenderManager_ogl.cpp`，4950 行）和 SDL3 API 文档的逐行代码审查，发现以下全部问题。按严重程度分类。

### 7.1 🔴 严重（已确认视觉缺陷）

#### Bug #1 — 子矩形纹理上传偏移（"彩色马赛克块"根因）

**位置：** `RenderManager_gpu.cpp:157-161`

**问题：** `tTVPGPUTexture2D::Update()` 上传纹理子区域到 GPU 纹理的**错误位置**：

```cpp
// 第 157-161 行（有 Bug）：
SDL_GPUTextureTransferInfo srcTI = { tb, 0 };
SDL_GPUTextureRegion dstReg = {};
dstReg.texture = m_texture;
dstReg.w = (Uint32)w; dstReg.h = (Uint32)h; dstReg.d = 1;
// dstReg.x 和 dstReg.y 留在 0 — 未设置为 rc.left/rc.top！
SDL_UploadToGPUTexture(cp, &srcTI, &dstReg, false);
```

`SDL_GPUTextureRegion` 结构体（SDL3 文档确认）有 `x`/`y` 字段表示左上角偏移。当前代码**未设置** `dstReg.x/y`，始终上传到 (0,0)。

**调用路径确认：**
1. `tTVPBaseTexture::Update(pixel, pitch, x, y, w, h)`（`LayerBitmapIntf.cpp:4825-4827`）调用 `Update(pixel, RGBA, pitch, tTVPRect(x, y, x+w, y+h))` — 传递非零 x,y 偏移
2. `GetTextureForRender`（`win32/LayerBitmapImpl.cpp:1603-1614`，确认在 Android 编译）调用 `Independ()` 后返回 GPU 纹理
3. `tTVPGPUTexture2D::Update` — **Bug：dstReg.x/y 未设置为 rc.left/rc.top**

**对比 `ReadbackPixel`**（第 1621-1624 行）**正确设置** `srcReg.x = (Uint32)x; srcReg.y = (Uint32)y;` — 证明 Update 的遗漏是疏忽。

**CPU 镜像 `m_pixels` 正确更新**（第 134 行：`dstBase = m_pixels.data() + rc.top * m_pitch + rc.left * 4;`）— 所以 `GetScanLineForRead` 返回正确数据，但 GPU 纹理数据在错误位置。子矩形数据覆盖 (0,0)，目标位置保留陈旧数据 → 马赛克。

**`CreateTexture2D(pixel, ...)` 初始上传**（第 1042 行）用 `tTVPRect(0, 0, w, h)` — 正确。仅子矩形更新损坏。

**✅ 已修复：** 添加 `dstReg.x = (Uint32)rc.left; dstReg.y = (Uint32)rc.top;`

---

#### Bug #2 — `AlphaBlend_d` 着色器是死代码

**位置：** `RenderManager_gpu.cpp:464`（编译）、`865-930`（`_GetOrCreateMethod`）

**问题：** `m_fs_alphaBlendD` 在第 464 行编译，公式**正确**（`alpha_blend_d.frag`：`eff = Sa/(Sa+Da*(1-Sa))`），但 `_GetOrCreateMethod`（865-930 行）**没有映射** "AlphaBlend_d" 到该着色器。实际走固定功能 `SRC_ALPHA/ONE_MINUS_SRC_ALPHA`（第 831 行），完全不读取目标 alpha。`m_needsDestRead` 只对 PsOverlayBlend 设置（923 行）。

**影响：** `bmAlphaOnAlpha`（半透明图层叠加到已有半透明目标，最常见的 _d 场景）使用错误的混合比率。

**软件渲染器：** `TVPAlphaBlend_d_c` 用 `TVPOpacityOnOpacityTable[addr]` 查表（`Sa/(Da*(1-Sa)+Sa)`）。

**OGL 参考：** 着色器内 `s.a*=opacity; d.a=s.a+d.a-s.a*d.a; d.rgb=mix(d.rgb, s.rgb, s.a/(d.a+0.0001))` — 单 pass，通过 framebuffer_fetch 或 2-texture 读取目标。

**✅ 已修复：** 在 `_GetOrCreateMethod` 添加 AlphaBlend_d→m_fs_alphaBlendD 映射 + m_needsDestRead=true

---

#### Bug #3 — `AlphaBlend_a` RGB 过度贡献

**位置：** `RenderManager_gpu.cpp:903-906`（着色器选择）、`quad_pma.frag`

**问题：** Bug ⑦b 称 `quad_pma.frag` 用于 _a 变体"已修复"，但对 `AlphaBlend_a` 是**错误**的。`quad_pma.frag` 做完整预乘 `rgb *= opacity; a *= opacity`，而 kirikiri 纹理是**直通 alpha（straight alpha）**。

```
Yuri AlphaBlend_a (blend ONE/ONE_MINUS_SRC_ALPHA, quad_pma):
  RGB = src.rgb * opacity * 1 + dst * (1 - src.a * opacity)
      = src.rgb * opacity + dst * (1 - src.a * opacity)   ← 缺少 src.a 因子

OGL AlphaBlend_a (blend SRC_ALPHA/ONE_MINUS_SRC_ALPHA, shader s.a*=opacity):
  RGB = src.rgb * (src.a * opacity) + dst * (1 - src.a * opacity)  ← 正确
```

**影响：** `src.a=0.5, opacity=1.0` 时，Yuri 给出 `src.rgb * 1.0`（全 RGB），OGL 给出 `src.rgb * 0.5`（alpha 加权）。半透明精灵显得过亮/过不透明。

**根因：** `_a` 后缀意为"通过 blend 状态累积目标 alpha"，**不是**"预乘输入"。`AlphaBlend` 和 `AlphaBlend_a` 的 OGL 着色器源码**完全相同**（`s.a *= opacity`，RGB 不动），仅 blend 状态不同。

**✅ 已修复：** `AlphaBlend_a` 改用 `quad.frag`（仅 `a *= opacity`），blend 改为 `SRC_ALPHA/ONE_MINUS_SRC_ALPHA`（RGB），`ONE/ONE_MINUS_SRC_ALPHA`（A）。从 quad_pma catch-all 排除。

---

#### Bug #4 — `CONSTANT_COLOR` blend 无法缩放目标衰减因子

**位置：** `RenderManager_gpu.cpp:1280`（强制 opacity=1.0）、`_getMethodBlend` 各 CONSTANT_COLOR 方法

**问题：** `AdditiveAlphaBlend`、`AdditiveAlphaBlend_a`、`SubBlend` 等用 `CONSTANT_COLOR`（设为 opacity）作为 srcC。`OperateRect` 在 `useBlendConstants=true` 时强制 shader opacity=1.0（第 1280 行）。

```
AdditiveAlphaBlend (CONSTANT_COLOR/ONE):
  Yuri: RGB = src.rgb * opacity + dst * 1              ← dst 永不衰减
  OGL:  RGB = src.rgb * opacity + dst * (1 - src.a * opacity)  ← alpha 加权

AdditiveAlphaBlend_a (CONSTANT_COLOR/ONE_MINUS_SRC_ALPHA):
  Yuri: RGB = src.rgb * opacity + dst * (1 - src.a * 1)   ← dst 衰减因子缺 opacity
  OGL:  RGB = src.rgb * opacity + dst * (1 - src.a * opacity)
```

**影响：** `src.a=1, opacity=0.5` 时，Yuri 完全覆盖目标（`dst*0`），OGL 正确混合（`dst*0.5`）。

**根因：** `CONSTANT_COLOR` 只能缩放源，不能缩放 `ONE_MINUS_SRC_ALPHA` 中的 `src.a`。

**✅ 已修复：** AdditiveAlphaBlend/ScreenBlend 改为着色器内预乘（`quad_pma.frag`：`s *= opacity`），blend 改为 `ONE/ONE_MINUS_SRC_ALPHA`（或 `ONE/ONE_MINUS_SRC_COLOR` for Screen）。着色器内 `src.a` 已包含 opacity，`ONE_MINUS_SRC_ALPHA` 的 dst 因子自然正确。

---

#### Bug #5 — `univ_trans.frag` alpha 未交叉淡入淡出

**位置：** `univ_trans.frag:24`

**问题：** `s1.a *= opacity` — alpha 只乘图层 opacity，**不随规则 opa 淡入淡出**。只有 RGB 通过规则 opa 交叉淡入。

**影响：** `s1.alpha=0`、`s2.alpha=1`、`opa=0.5` 时，结果 alpha=0（应为≈0.5）。带 alpha 通道的图层做规则过渡时透明度跳变。

**✅ 已修复：** `s1.a = mix(s2.a, s1.a, opa) * opacity;`

---

#### Bug #6 — `ps_overlay.frag` 忽略源 alpha

**位置：** `ps_overlay.frag:21`

**问题：** `mix(dst.rgb, result, uP.opacity)` — 用 `opacity` 而非 `src.a * opacity`。

**影响：** `src.a < 1` 时，Yuri 应用完整 overlay 效果而非按比例减弱。半透明图层的 PsOverlay 效果过强。

**OGL 参考：** `mix(d.rgb, s.rgb, s.a * opacity)`

**注：** overlay 公式本身正确（第 18-20 行，已验证等价于 OGL 的 step 公式）。

**✅ 已修复：** `mix(dst.rgb, result, src.a * uP.opacity)`

---

#### Bug #7 — `CopyRect` AssignTexture 不一致

**位置：** `LayerBitmapIntf.cpp:799` vs `949`

**问题：** 两个 CopyRect 实现对 GPU 行为不同：
- `tTVPBaseBitmap::CopyRect`（:799）：**不检查 IsSoftware()** 就调 `AssignTexture(ref->GetTexture())` — GPU 会**别名纹理指针**（两个 bitmap 共享同一 GPU 纹理，写一个影响另一个）
- `iTVPBaseBitmap::CopyRect`（:949）：检查 `IsSoftware()` — 只对软件渲染 AssignTexture，GPU 走 OperateRect。正确。

**✅ 已修复：** 在 `tTVPBaseBitmap::CopyRect` 添加 `IsSoftware()` 检查。

---

#### Bug #8 — `m_pixels` 在 GPU 写入后过时

**位置：** `RenderManager_gpu.cpp` — `m_pixels` 仅在 `Update()` 更新

**问题：** `m_pixels`（CPU 镜像）只在 `Update()` 更新。纹理作为**渲染目标**被 GPU 写入（OperateRect）后，`m_pixels` 不更新。`GetScanLineForRead` 返回过时/初始数据。

**影响：** 任何通过 `GetScanLineForRead` 读取渲染目标像素的代码（截图、像素检测、CPU 侧后处理）得到错误数据。

**✅ 已修复：** 添加 `m_pixelsDirty` 标志，`SetRenderTarget` 时标记目标为脏，`GetScanLineForRead` 时触发同步 GPU 读回（RGBA→BGRA 交换）。

---

### 7.2 🟠 高（数据竞争 / 功能缺失）

#### Bug #9 — Fence 等待时序（纹理更新数据竞争）

**位置：** `RenderManager_gpu.cpp:1443-1449`（EndFrame）vs `Update()`（153-154）

**问题：** `EndFrame` 在 START 时等待**上一帧**的 fence，但当前帧的渲染（包括 `Update()` 调用）发生在 EndFrame **之前**（BeginFrame 和 EndFrame 之间）。

如果纹理 T 在帧 N-1 是渲染目标，帧 N 的 `Update(T)` 在 fence 等待之前调用 → GPU 可能仍在读 T → 数据竞争 → 潜在马赛克/损坏。

`Update()` 第 153-154 行：如果 `CurrentCmd()` 存在，上传附加到同一命令缓冲区（有序）。如果没有当前命令（帧外调用），获取**新**命令缓冲区并立即提交 — 与上一帧无序。

**✅ 已修复：** Fence 等待从 `EndFrame` 移到 `_BeginFrame` 开头，确保 GPU 空闲后再发当前帧命令。

---

#### Bug #10 — BGRA/RGBA 字节顺序问题

**位置：** `RenderManager_gpu.cpp:120-126`（Update memcpy）、`LayerIntf.h:435`（TVP_REVRGB）

**问题：** `TVP_REVRGB` 宏在 `Fill` 中使用 → bitmap 内存是 BGRA（Windows DIB 格式 `TVPRGBQUAD = {B,G,R,A}`）。`Update` 做原始 `memcpy`（120-126 行）将 BGRA 拷到 `R8G8B8A8_UNORM` 纹理 → GPU 读字节 0 为 R 但实际是 B → **R/B 通道互换**。

`m_swapFormat = SDL_GetGPUSwapchainTextureFormat()`（474 行）— 如果是 `R8G8B8A8`：颜色互换（可见 bug）；如果是 `B8G8R8A8`：抵消（不可见）。

24bpp 展开也复制 BGR→RGBA 位置 — 同样问题。

**✅ 已修复：** `Update()` 中 RGBA 格式上传时逐像素交换 R/B（BGRA→RGBA）。`m_pixels` 保持 BGRA（引擎期望格式）。24bpp 展开改为 BGR→BGRA（由 Update 统一交换）。`ReadbackToPixels()` 读回时 RGBA→BGRA 交换。

---

#### Bug #11 — `OperateTriangles` 和 `OperatePerspective` 是空桩

**位置：** `RenderManager_gpu.cpp:1357-1372`

**问题：** 两个函数体为空。`PerspectiveAlphaBlend_a` 方法已注册但 `OperatePerspective` 不执行。3D 透视变换图层（如 `PerspectiveAlphaBlend_a` 插件）、三角形网格渲染完全不工作。

---

#### Bug #12 — Photoshop 混合模式用错误公式（非近似，是完全错误的公式）

**位置：** `_getMethodBlend` 各 Ps 方法

| 方法 | Yuri（固定功能） | OGL（逐像素着色器） | 问题 |
|------|----------------|-------------------|------|
| PsHardLight | `ONE/ONE_MINUS_SRC_COLOR`（screen） | `step(0.5,s.rgb)` 条件 | 完全错误 |
| PsSoftLight | `ONE/ONE_MINUS_SRC_COLOR`（screen） | `pow(d.rgb,...)` | 完全错误 |
| PsDiff | `ONE/ONE, REVERSE_SUBTRACT` | `abs(s-d)` | REVERSE_SUBTRACT ≠ abs |
| PsExclusion | `ONE/ONE, REVERSE_SUBTRACT` | `d+(s-s*d*2)*s.a*opa` | 完全错误 |
| PsColorBurn | `ZERO/SRC_COLOR`（mul） | `1-min(1-d,s)/s` | 完全错误 |
| PsColorDodge | `ZERO/SRC_COLOR`（mul） | `d.rgb/max(1-s.rgb,d.rgb)` | 完全错误 |

**修复：** 实现为 dest-read 着色器（如 PsOverlay）。

---

#### Bug #13 — 所有 `_d` 变体缺乏 opacity-on-opacity

**位置：** `_getMethodBlend` 各 _d 方法

| 方法 | Yuri | OGL 参考 |
|--------|------|----------|
| `ApplyColorMap_d` | 固定功能 `SRC_ALPHA/ONE_MINUS_SRC_ALPHA` | dest-read 着色器 `d.a=s.r+d.a-s.r*d.a; mix(d.rgb,color.rgb,s.r/(d.a+ε))` |
| `ConstAlphaBlend_d` | 固定功能 | dest-read 着色器 |
| `ConstColorAlphaBlend_d` | 固定功能 | dest-read 着色器 |
| `AlphaBlend_d` | 固定功能（死着色器！） | dest-read 着色器 |

**影响：** 半透明背景上的文字/图层 alpha 混合错误。

---

#### Bug #14 — `CreateTexture2D(tTJSBinaryStream*)` 返回 nullptr

**位置：** `RenderManager_gpu.cpp:1080-1083`

**问题：** 流式纹理加载是桩。渐进式图像加载静默失败。

---

#### Bug #15 — `GetPoint`/`SetPoint` 是桩

**位置：** `RenderManager_gpu.cpp:169-170`

**问题：** `GetPoint` 返回 0，`SetPoint` 空操作。任何逐像素 CPU 访问返回黑色/无操作。

---

### 7.3 🟡 中（状态/同步问题）

#### Bug #16 — `SetBlendFuncSeparate` 是空操作

**位置：** `RenderManager_gpu.cpp:263-274`

**问题：** 存储 blend 因子到成员变量，但注释说"GPU 管线已烘焙 blend 状态 — 忽略动态更改"。管线创建时烘焙，之后调用 `SetBlendFuncSeparate` 对实际 GPU 管线无影响。

---

#### Bug #17 — 无绘制时呈现过时帧

**位置：** `RenderManager_gpu.cpp:1569-1589`（`_BeginFrame`）

**问题：** 注释："keep last frame's composited texture for idle frames"。如果一帧无绘制操作，swapchain 呈现上一帧内容。如果上一帧内容被释放，可能呈现垃圾。

---

#### Bug #18 — `m_frameFirstTarget` 重复设置

**位置：** `RenderManager_gpu.cpp:1579 和 1581`

**问题：** `m_frameFirstTarget = true` 连续设置两次（复制粘贴错误）。无功能影响。

**✅ 已修复：** Bug #9 修复时移除了重复行。

---

#### Bug #19 — 读回硬编码 1920×1080 限制

**位置：** `RenderManager_gpu.cpp:1489`

**问题：** `tw > 1920 || th > 1080` 时跳过读回。超过 1080p 的游戏（2K/4K 视觉小说）无读回。截图/像素检测失败。

---

#### Bug #20 — `g_childDrawToTemp` 全局标志线程安全

**位置：** `LayerIntf.cpp:6041/6086`

**问题：** `g_childDrawToTemp = true` 是全局标志。如果另一线程在子图层循环期间触发 Blt，会错误使用 `AlphaBlend_Copy`。

---

#### Bug #21 — `m_methodCache` 非线程安全

**位置：** `RenderManager_gpu.h` — `m_methodCache`

**问题：** 普通 `unordered_map`，无锁。多线程访问可能崩溃/数据竞争。

---

### 7.4 🟢 低（性能/代码质量）

#### Bug #22 — `Draw_GPU` 总用临时缓冲（即使 Opacity==255）

**位置：** `LayerIntf.cpp:6068-6104`

**问题：** 即使 opacity=255 也用临时缓冲。OGL 参考在 opa=255 时直接绘制。额外拷贝 + 额外 OperateRect。性能损失。

---

#### Bug #23 — `crossfade.frag` 共享一个 UV

**位置：** `crossfade.frag`

**问题：** 两个纹理用同一 `vUV`。OGL 用 `v_texCoord0`/`v_texCoord1`。如果 rect 不同则 tex1 用 tex0 的 UV 采样。通常 OK（同尺寸纹理）。

---

#### Bug #24 — 所有纹理 RGBA8 UNORM

**位置：** `CreateTexture2D`

**问题：** 无 Gray（8 位）、无 RGB（24 位）、无 R8、无纹理压缩。Gray 纹理在 `Update()` 展开为 RGBA（字体/规则掩码 4x 内存浪费）。

---

#### Bug #25 — `m_needsInit` 是死代码

**位置：** `RenderManager_gpu.h`

**问题：** 声明，默认 true，`SetInitialized()` 存在，但 `NeedsInit()` 从未被调用。

---

#### Bug #26 — UBO 命名不一致

**位置：** 各 shader 文件

**问题：** `quad.frag` 等用命名块 `} uP;`（`uP.uvScale`），`univ_trans.frag`/`adjust_gamma.frag` 用未命名块 `};`（全局 `uvScale`）。两者都合法，C++ 推送布局匹配，但不一致。

---

#### Bug #27 — `GetRenderStat` 每次调用重置 m_drawCount

**位置：** `RenderManager_gpu.cpp:1377-1381`

**问题：** draw count 只在一帧内有效，无法跨帧统计。

---

### 7.5 修复优先级表

| 优先级 | Bug # | 修复要点 | 状态 |
|--------|-------|---------|------|
| P0 | 1 | `Update()`: `dstReg.x=rc.left; dstReg.y=rc.top;` | ✅ 已修复 |
| P0 | 2 | `_GetOrCreateMethod`: AlphaBlend_d→m_fs_alphaBlendD + m_needsDestRead=true | ✅ 已修复 |
| P0 | 3 | AlphaBlend_a 用 `quad.frag`（非 quad_pma），blend 改 SRC_ALPHA/ONE_MINUS_SRC_ALPHA | ✅ 已修复 |
| P0 | 4 | CONSTANT_COLOR 方法 → 着色器预乘 + ONE/ONE_MINUS_SRC_ALPHA | ✅ 已修复 |
| P1 | 5 | `univ_trans.frag`: `s1.a = mix(s2.a, s1.a, opa) * opacity` | ✅ 已修复 |
| P1 | 6 | `ps_overlay.frag`: `mix(dst.rgb, result, src.a * uP.opacity)` | ✅ 已修复 |
| P1 | 7 | `tTVPBaseBitmap::CopyRect` 加 `IsSoftware()` 检查 | ✅ 已修复 |
| P1 | 8 | 渲染目标写入后标记 m_pixels 为脏 | ✅ 已修复 |
| P1 | 9 | Fence 等待移到 BeginFrame | ✅ 已修复 |
| P1 | 10 | `Update()` 中 BGRA→RGBA 交换 R/B | ✅ 已修复 |
| P2 | 11-15 | 实现 OperateTriangles/Perspective、Ps 着色器、_d 着色器、流式加载、GetPoint | ❌ 待实现 |
| P3 | 16-27 | 线程安全、格式优化、死代码清理（#18 已修复） | ❌ 待实现 |

### 7.6 参考实现对比要点

**软件渲染器**（`tvpgl.cpp`，工作正常，不应修改）：
- `TVPOpacityOnOpacityTable[256*256]`：opacity-on-opacity 查找表
- `TVPNegativeMulTable[256*256]`：新 alpha = `Sa + Da - Sa*Da`
- `TVPAlphaBlend_d_c`（第 560 行）：正确的 _d 混合
- `TVPAlphaBlend_c`（第 293 行）：标准 alpha 混合

**OGL 参考**（KrKr2-Emu `RenderManager_ogl.cpp`，4950 行）：
- `AlphaBlend_d`：`CompileAndRegRegularBlendMethod` → framebuffer_fetch 或 2-texture，着色器 `d.a=s.a+d.a-s.a*d.a; d.rgb=mix(d.rgb,s.rgb,s.a/(d.a+0.0001))`。**TVPOpacityOnOpacityTable 未使用** — 纯着色器数学
- `_a` 变体：**混合策略** — AlphaBlend/AlphaBlend_a 着色器**相同**（`s.a*=opacity`），仅 blend 状态不同；AdditiveAlphaBlend/_a 着色器**预乘**（`s*=opacity`）
- Ps 混合：全部逐像素着色器公式，无 LUT 纹理
- ApplyColorMap：`GL_LUMINANCE` 纹理返回 `vec4(r,r,r,1)`，`gl_FragColor = vec4(color.rgb, s.r*opacity)`
- 图层合成：Draw_GPU 在 opa=255 时**直接绘制**到目标（无临时缓冲）
- 无批处理：每个 OperateRect 一次 `glDrawArrays(6)`
