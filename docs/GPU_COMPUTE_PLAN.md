# GPU Compute Shader Plan

## Motivation
Currently the Kirikiri engine does many operations on CPU that could be GPU-accelerated:

| Operation | Current | GPU Future |
|-----------|---------|------------|
| FreeType font rasterization | CPU, FT_Load_Glyph + blit | Glyph cache texture, SDF rendering via compute |
| TLG5/TLG6 image decompression | CPU, LZ4 + color plane math | Compute shader per plane |
| Image scaling/transforms | CPU, tvpgl.h functions | Sampler + compute |
| Layer compositing | CPU or GLES2 FBO blend | SDL_Gpu render pass + compute |
| Pixel format conversion | CPU, per-pixel loops | Compute shader |
| Blend chain (60+ modes) | GLES2 shaders (frag) | SDL_Gpu graphics pipeline (already GPU) |

## Phase 0: Audit CPU hotspots
Profile and identify which CPU ops consume >5% frame time.

## Phase 1: TLG decode on GPU
TLG5/TLG6 are the most common compressed image formats in Kirikiri games.
Current: CPU LZ4 decompress + TLG5 color plane separation → pixel buffer → glTexImage2D.
Target: Upload compressed planes as textures, compute shader reconstructs RGBA, copy to output texture.

Candidates for first compute shader:
- Color plane reconstruction (TLG5: YCoCg → RGBA)
- Alpha recovery (TLG6: lossy Y + lossless A)
- LZ4 decompression on GPU (requires GPU-side LZ4, non-trivial)

## Phase 2: Font caching on GPU
- Generate glyph atlasses once on CPU (or SDF via compute)
- Cache as SDL_GpuTexture
- Text rendering = textured quads

## Phase 3: Layer compositing via compute
Replace the full compositing chain (60 blend modes) with a single compute dispatch
that iterates layer rectangles. Reduces CPU-GPU sync.

## Implementation strategy
- All new GPU code lives in `src/core/visual/gpu/`
- SDL_Gpu backend only (no raw Vulkan)
- Fall back to CPU if GPU feature unavailable
- Initial target: TLG decode (immediate gains for image-heavy VNs)
