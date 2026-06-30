# OGL Renderer — CPU Fallback 完整方案 (2024-06-30)

## 现状

| 项目 | 状态 |
|------|------|
| 默认渲染器 | `"opengl"` (`RenderManager.cpp:4402`) |
| bmCopy 快路径 | ✅ CopyRect → AssignTexture（零拷贝） |
| 非 Copy 合成 | ✅ CPU fallback（软件 DoRender + glTexSubImage2D 上传） |
| OGL 原生 FBO+shader 合成 | ❌ 设备 MIUI Android 12 GLES 3.x 上不输出像素 |
| 画面显示 | ✅ 位置/比例/颜色正确 |
| FPS（静态场景） | ~106 fps |
| FPS（大动画） | 1-2 fps（性能瓶颈在 CPU 回退） |
| FreeType 文字 | ❌ 显示为空（可能走 OperateTriangles 未拦截） |

## 关键文件

| 文件 | 修改 |
|------|------|
| `src/core/visual/RenderManager.cpp:4418` | `TVPGetSoftwareRenderManager` 内加 `Initialize()` 修复崩溃 |
| `src/core/visual/LayerBitmapIntf.cpp:1463-1487` | `Blt` CPU fallback（非软件渲染器时走软件 DoRender，PixelData 持续缓存） |
| `src/core/visual/ogl/RenderManager_ogl.cpp:3760-3761` | `OperateRect` 加 `glFlush`，移除 OPRECT 诊断 |
| `src/core/environ/sdl/WindowLayer_sdl.cpp:502-535` | `_useOGLTexture` 用 `GetInternalWidth/Height` 计算 UV scale；加 `_uploadCPU_OGLTexture` 统一上传 |

## 合成流程

1. `bmCopy + opa=255` → CopyRect 快路径 → `AssignTexture`（纹理指针交换，零拷贝）
2. 其他操作 → `iTVPBaseBitmap::Blt` 检测非软件渲染器 → 走 CPU fallback：
   - `GetScanLineForRead(0)` 确保 PixelData 存在（首次 glReadPixels）
   - `DoRender` 软件混合（修改 PixelData 原地）
   - 不调 `Update()` → PixelData 持续缓存，下一帧无需 glReadPixels
3. 每帧 `_useOGLTexture` → `_uploadCPU_OGLTexture` 用 `glTexSubImage2D` 上传到 GL 纹理（保留 PixelData）

## 待修复

- **性能瓶颈**：大动画时 1-2 fps。CPU fallback 每帧仍需逐行上传 1280x720 像素到 GL。优化方案：纹理分配时用 NPOT 消除 POT padding，减少上传量
- **FreeType 文字**：可能走 `OperateTriangles`（OGL 路径，未拦截）。需添加诊断确认路径，再决定加 CPU fallback 还是仅拦截
- **分离引擎 FPS 与 GL 渲染 FPS**：当前 `ENGINE: 106fps 0/107 draws` 不反映视觉帧率

## 已知 GL 合成不可用

OGL 原生 FBO+shader 合成（`OperateRect` 内 `glDrawArrays`）在此设备持续输出 0xFF000000。确认 FBO COMPLETE、shader linked=1、无 GL error。原因：设备特定 GLES 驱动 bug 或 shader `precision` 缺失。
