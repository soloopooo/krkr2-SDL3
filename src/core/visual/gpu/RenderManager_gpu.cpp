#include "RenderManager_gpu.h"
#include "display_spv.h"
#include <SDL3/SDL.h>
#include <android/log.h>
#include <cstring>
#include <algorithm>
#include "tjsCommHead.h"
#include "DebugIntf.h"
#include "LayerBitmapIntf.h"

#define TAG "gpu"

//==============================================================================
// Statics
//==============================================================================
std::atomic<uint64_t> tTVPGPUTexture2D::s_gpuTotalVMem{0};

SDL_GPUDevice *TVPRenderManager_GPU::s_device = nullptr;
SDL_Window *TVPRenderManager_GPU::s_window = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_vs = nullptr;
SDL_GPUBuffer *TVPRenderManager_GPU::s_quadVerts = nullptr;
SDL_GPUSampler *TVPRenderManager_GPU::s_sampler = nullptr;
SDL_GPUGraphicsPipeline *TVPRenderManager_GPU::s_presentPipe = nullptr;
SDL_GPUGraphicsPipeline *TVPRenderManager_GPU::s_displayPipe = nullptr;

SDL_GPUShader *TVPRenderManager_GPU::s_quad_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_quad_pma_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_present_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_fill_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_fill_mask_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_copy_color_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_copy_mask_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_copy_opaque_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_remove_opacity_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_gray_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_blur_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_gamma_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_adjust_gamma_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_crossfade_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_univ_trans_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_alpha_blend_d_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_const_alpha_blend_d_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_const_color_alpha_blend_d_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_apply_colormap_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_apply_colormap_a_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_apply_colormap_d_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_ps_overlay_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_ps_hardlight_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_ps_softlight_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_ps_colordodge_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_ps_colorburn_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_ps_diff_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_ps_exclusion_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_ps_lighten_fs = nullptr;
SDL_GPUShader *TVPRenderManager_GPU::s_ps_darken_fs = nullptr;

SDL_GPUTexture *TVPRenderManager_GPU::s_tempDest = nullptr;
int TVPRenderManager_GPU::s_tempW = 0;
int TVPRenderManager_GPU::s_tempH = 0;

SDL_GPUTexture *TVPRenderManager_GPU::s_layerTex = nullptr;
int TVPRenderManager_GPU::s_layerW = 0;
int TVPRenderManager_GPU::s_layerH = 0;

SDL_GPUCommandBuffer *TVPRenderManager_GPU::s_cmd = nullptr;
std::unordered_map<uint64_t, SDL_GPUGraphicsPipeline*> TVPRenderManager_GPU::s_pipeCache;
bool TVPRenderManager_GPU::s_initialized = false;
TVPRenderManager_GPU *TVPRenderManager_GPU::s_instance = nullptr;

//==============================================================================
// tTVPGPUTexture2D — GPU texture wrapping a CPU pixel buffer
//==============================================================================
static uint32_t ABGR2RGBA(uint32_t abgr) {
	return ((abgr & 0xFF) << 24) | ((abgr & 0xFF00) << 8) |
	       ((abgr & 0xFF0000) >> 8) | ((abgr & 0xFF000000) >> 24);
}

tTVPGPUTexture2D::tTVPGPUTexture2D(int w, int h, TVPTextureFormat::e fmt,
	bool opaque, const void *pixels, int pitch)
	: iTVPTexture2D(w, h), m_format(fmt), m_opaque(opaque)
{
	int bpp = (fmt == TVPTextureFormat::Gray) ? 1 : 4;
	m_pitch = pitch ? pitch : w * bpp;
	m_pixels.resize((size_t)h * m_pitch);
	if (pixels) {
		for (int y = 0; y < h; y++)
			memcpy(m_pixels.data() + y * m_pitch,
				(const uint8_t*)pixels + y * pitch, (size_t)(w * bpp));
	}
	s_gpuTotalVMem += (uint64_t)h * m_pitch;
}

tTVPGPUTexture2D::~tTVPGPUTexture2D() {
	s_gpuTotalVMem -= (uint64_t)Height * m_pitch;
	if (m_gpuTex && TVPRenderManager_GPU::s_device)
		SDL_ReleaseGPUTexture(TVPRenderManager_GPU::s_device, m_gpuTex);
}

const void * tTVPGPUTexture2D::GetScanLineForRead(tjs_uint l) {
	if (l >= (tjs_uint)Height) return nullptr;
	return m_pixels.data() + l * m_pitch;
}

void * tTVPGPUTexture2D::GetScanLineForWrite(tjs_uint l) {
	if (l >= (tjs_uint)Height) return nullptr;
	m_opaque = false;
	m_cpuDirty = true;
	return m_pixels.data() + l * m_pitch;
}

void tTVPGPUTexture2D::Update(const void *pixel, TVPTextureFormat::e format,
	int pitch, const tTVPRect& rc)
{
	int w = rc.get_width(), h = rc.get_height();
	if (w <= 0 || h <= 0) return;
	uint8_t *dst = m_pixels.data() + rc.top * m_pitch + rc.left * 4;
	if (format == TVPTextureFormat::Gray) {
		for (int y = 0; y < h; y++) {
			const uint8_t *src = (const uint8_t*)pixel + y * pitch;
			uint32_t *d = (uint32_t*)(dst + y * m_pitch);
			for (int x = 0; x < w; x++)
				d[x] = 0xFF000000 | (src[x] << 16) | (src[x] << 8) | src[x];
		}
	} else {
		int lineBytes = w * 4;
		for (int y = 0; y < h; y++)
			memcpy(dst + y * m_pitch, (const uint8_t*)pixel + y * pitch, (size_t)lineBytes);
	}
	m_cpuDirty = true;
}

uint32_t tTVPGPUTexture2D::GetPoint(int x, int y) {
	if (x < 0 || y < 0 || x >= Width || y >= Height) return 0;
	if (m_format == TVPTextureFormat::Gray)
		return m_pixels[y * m_pitch + x];
	const uint8_t *p = m_pixels.data() + y * m_pitch + x * 4;
	return ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) |
	       ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

void tTVPGPUTexture2D::SetPoint(int x, int y, uint32_t clr) {
	if (x < 0 || y < 0 || x >= Width || y >= Height) return;
	if (m_format == TVPTextureFormat::Gray) {
		m_pixels[y * m_pitch + x] = (uint8_t)clr;
	} else {
		uint8_t *p = m_pixels.data() + y * m_pitch + x * 4;
		p[0] = (clr >> 16) & 0xFF;
		p[1] = (clr >> 8) & 0xFF;
		p[2] = clr & 0xFF;
		p[3] = (clr >> 24) & 0xFF;
	}
	m_cpuDirty = true;
}

void tTVPGPUTexture2D::UploadToGPU(SDL_GPUDevice *dev) {
	if (!m_cpuDirty && m_gpuTex) return;
	if (!m_gpuTex) {
		SDL_GPUTextureCreateInfo ti = {};
		ti.type = SDL_GPU_TEXTURETYPE_2D;
		ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		ti.width = (Uint32)Width; ti.height = (Uint32)Height;
		ti.layer_count_or_depth = 1; ti.num_levels = 1;
		ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
		m_gpuTex = SDL_CreateGPUTexture(dev, &ti);
		if (!m_gpuTex) return;
	}
	SDL_GPUTransferBufferCreateInfo tci = {};
	tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	tci.size = (Uint32)(Width * Height * 4);
	SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tci);
	if (!tb) return;
	uint8_t *map = (uint8_t*)SDL_MapGPUTransferBuffer(dev, tb, false);
	if (map) {
		for (int y = 0; y < Height; y++) {
			const uint8_t *src = m_pixels.data() + y * m_pitch;
			uint32_t *dst = (uint32_t*)(map + y * Width * 4);
			for (int x = 0; x < Width; x++) {
				uint32_t abgr = *(const uint32_t*)(src + x * 4);
				dst[x] = ABGR2RGBA(abgr);
			}
		}
	}
	SDL_UnmapGPUTransferBuffer(dev, tb);
	SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(dev);
	if (cb) {
		SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
		if (cp) {
			SDL_GPUTextureTransferInfo s = { tb, 0 };
			SDL_GPUTextureRegion d = {};
			d.texture = m_gpuTex; d.w = (Uint32)Width; d.h = (Uint32)Height; d.d = 1;
			SDL_UploadToGPUTexture(cp, &s, &d, false);
			SDL_EndGPUCopyPass(cp);
		}
		SDL_SubmitGPUCommandBuffer(cb);
	}
	SDL_ReleaseGPUTransferBuffer(dev, tb);
	m_cpuDirty = false;
	m_gpuDirty = false;
}

void tTVPGPUTexture2D::DownloadFromGPU(SDL_GPUDevice *dev) {
	if (!m_gpuDirty || !m_gpuTex) return;
	if (!dev) return;
	SDL_GPUTransferBufferCreateInfo tci = {};
	tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	tci.size = (Uint32)(Width * Height * 4);
	SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tci);
	if (!tb) return;
	SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(dev);
	if (cb) {
		SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
		if (cp) {
			SDL_GPUTextureRegion s = {};
			s.texture = m_gpuTex; s.w = (Uint32)Width; s.h = (Uint32)Height; s.d = 1;
			SDL_GPUTextureTransferInfo d = { tb, 0 };
			SDL_DownloadFromGPUTexture(cp, &s, &d);
			SDL_EndGPUCopyPass(cp);
		}
		SDL_SubmitGPUCommandBuffer(cb);
		SDL_WaitForGPUIdle(dev);
	}
	const uint8_t *map = (const uint8_t*)SDL_MapGPUTransferBuffer(dev, tb, true);
	if (map) {
		for (int y = 0; y < Height; y++) {
			uint8_t *dst = m_pixels.data() + y * m_pitch;
			const uint32_t *src = (const uint32_t*)(map + y * Width * 4);
			for (int x = 0; x < Width; x++)
				*(uint32_t*)(dst + x * 4) = ABGR2RGBA(src[x]);
		}
	}
	SDL_UnmapGPUTransferBuffer(dev, tb);
	SDL_ReleaseGPUTransferBuffer(dev, tb);
	m_gpuDirty = false;
}

//==============================================================================
// Helper: create SPIR-V shader
//==============================================================================
static SDL_GPUShader* _CreateShader(SDL_GPUDevice *dev,
	const unsigned char *code, unsigned int codeSize,
	SDL_GPUShaderStage stage, int numSamplers, int numUBOs)
{
	SDL_GPUShaderCreateInfo ci = {};
	ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
	ci.entrypoint = "main";
	ci.stage = stage;
	ci.code = code;
	ci.code_size = codeSize;
	ci.num_samplers = numSamplers;
	ci.num_storage_textures = 0;
	ci.num_storage_buffers = 0;
	ci.num_uniform_buffers = numUBOs;
	return SDL_CreateGPUShader(dev, &ci);
}

//==============================================================================
// Quad vertex data (position2 + uv2 = 4 floats per vertex, 6 vertices)
//==============================================================================
static const float s_quadData[24] = {
	-1,-1, 0,1,   1,-1, 1,1,   -1,1, 0,0,
	-1,1,  0,0,   1,-1, 1,1,    1,1, 1,0,
};

static void _UploadQuadVerts(SDL_GPUDevice *dev, SDL_GPUBuffer *buf) {
	SDL_GPUTransferBufferCreateInfo tci = {};
	tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	tci.size = sizeof(s_quadData);
	SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(dev, &tci);
	memcpy(SDL_MapGPUTransferBuffer(dev, tb, false), s_quadData, sizeof(s_quadData));
	SDL_UnmapGPUTransferBuffer(dev, tb);
	SDL_GPUCommandBuffer *cb = SDL_AcquireGPUCommandBuffer(dev);
	SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cb);
	SDL_GPUTransferBufferLocation s = { tb, 0 };
	SDL_GPUBufferRegion d = { buf, 0, sizeof(s_quadData) };
	SDL_UploadToGPUBuffer(cp, &s, &d, false);
	SDL_EndGPUCopyPass(cp);
	SDL_SubmitGPUCommandBuffer(cb);
	SDL_ReleaseGPUTransferBuffer(dev, tb);
}

//==============================================================================
// Pipeline cache key
//==============================================================================
static uint64_t _PipeKey(SDL_GPUShader *fs, const BlendConfig &b,
	SDL_GPUTextureFormat fmt, int tex, int ubos)
{
	uint64_t k = (uint64_t)(uintptr_t)fs;
	k ^= ((uint64_t)b.srcColor << 8) ^ ((uint64_t)b.dstColor << 16);
	k ^= ((uint64_t)b.srcAlpha << 24) ^ ((uint64_t)b.dstAlpha << 32);
	k ^= ((uint64_t)b.enable << 40) ^ ((uint64_t)b.colorOp << 42) ^ ((uint64_t)b.alphaOp << 45);
	k ^= ((uint64_t)fmt << 48) ^ ((uint64_t)tex << 56) ^ ((uint64_t)ubos << 60);
	return k;
}

SDL_GPUGraphicsPipeline* TVPRenderManager_GPU::_GetOrCreatePipeline(
	SDL_GPUShader *fs, const BlendConfig &blend,
	SDL_GPUTextureFormat rtFmt, int numTextures, int numUBOs)
{
	uint64_t key = _PipeKey(fs, blend, rtFmt, numTextures, numUBOs);
	auto it = s_pipeCache.find(key);
	if (it != s_pipeCache.end()) return it->second;

	SDL_GPUGraphicsPipelineCreateInfo pi = {};
	pi.vertex_shader = s_vs;
	pi.fragment_shader = fs;

	SDL_GPUVertexAttribute va[2] = {};
	va[0].location = 0; va[0].buffer_slot = 0;
	va[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; va[0].offset = 0;
	va[1].location = 1; va[1].buffer_slot = 0;
	va[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2; va[1].offset = 8;
	SDL_GPUVertexBufferDescription vd = {};
	vd.slot = 0; vd.pitch = 16; vd.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
	pi.vertex_input_state.vertex_buffer_descriptions = &vd;
	pi.vertex_input_state.num_vertex_buffers = 1;
	pi.vertex_input_state.vertex_attributes = va;
	pi.vertex_input_state.num_vertex_attributes = 2;

	SDL_GPUColorTargetDescription ct = {};
	ct.format = rtFmt;
	ct.blend_state.enable_blend = blend.enable;
	ct.blend_state.alpha_blend_op = blend.alphaOp;
	ct.blend_state.color_blend_op = blend.colorOp;
	ct.blend_state.src_color_blendfactor = blend.srcColor;
	ct.blend_state.dst_color_blendfactor = blend.dstColor;
	ct.blend_state.src_alpha_blendfactor = blend.srcAlpha;
	ct.blend_state.dst_alpha_blendfactor = blend.dstAlpha;
	ct.blend_state.enable_color_write_mask = false;
	pi.target_info.num_color_targets = 1;
	pi.target_info.color_target_descriptions = &ct;

	pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
	pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;

	SDL_GPUGraphicsPipeline *p = SDL_CreateGPUGraphicsPipeline(s_device, &pi);
	if (!p)
		__android_log_print(ANDROID_LOG_ERROR, TAG, "CreatePipeline failed (fs=%p, blend=%d)", (void*)fs, blend.enable);
	s_pipeCache[key] = p;
	return p;
}

//==============================================================================
// _DrawQuadRaw — low-level quad draw with explicit params
//==============================================================================
void TVPRenderManager_GPU::_DrawQuadRaw(SDL_GPUGraphicsPipeline *pipe,
	SDL_GPUTexture *tex0, SDL_GPUTexture *tex1, SDL_GPUTexture *tex2,
	int w, int h, int numTextures, int numUBOs,
	const float *uboData, int uboSize,
	const float *uboData2, int uboSize2)
{
	if (!s_cmd || !pipe) return;
	if (!s_layerTex) return;

	SDL_GPUColorTargetInfo ct = {};
	ct.texture = s_layerTex;
	ct.load_op = SDL_GPU_LOADOP_LOAD;
	ct.store_op = SDL_GPU_STOREOP_STORE;

	SDL_Rect viewport = {0, 0, s_layerW, s_layerH};

	SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(s_cmd, &ct, 1, NULL);
	if (!rp) return;

	SDL_BindGPUGraphicsPipeline(rp, pipe);
	SDL_GPUBufferBinding bb = { s_quadVerts, 0 };
	SDL_BindGPUVertexBuffers(rp, 0, &bb, 1);

	if (numTextures >= 1 && tex0) {
		SDL_GPUTextureSamplerBinding ts = { tex0, s_sampler };
		SDL_BindGPUFragmentSamplers(rp, 0, &ts, 1);
	}
	if (numTextures >= 2 && tex1) {
		SDL_GPUTextureSamplerBinding ts = { tex1, s_sampler };
		SDL_BindGPUFragmentSamplers(rp, 1, &ts, 1);
	}
	if (numTextures >= 3 && tex2) {
		SDL_GPUTextureSamplerBinding ts = { tex2, s_sampler };
		SDL_BindGPUFragmentSamplers(rp, 2, &ts, 1);
	}
	if (numUBOs >= 1 && uboData && uboSize > 0)
		SDL_PushGPUFragmentUniformData(s_cmd, 0, uboData, (Uint32)uboSize);
	if (numUBOs >= 2 && uboData2 && uboSize2 > 0)
		SDL_PushGPUFragmentUniformData(s_cmd, 1, uboData2, (Uint32)uboSize2);

	SDL_DrawGPUPrimitives(rp, 6, 1, 0, 0);
	SDL_EndGPURenderPass(rp);
}

//==============================================================================
// _DrawQuad — high-level draw with method info + texture array
//==============================================================================
void TVPRenderManager_GPU::_DrawQuad(tTVPGPURenderMethod *method,
	iTVPTexture2D *tar, const tTVPRect &rctar,
	const tRenderTexRectArray &textures,
	float fillColor[4], float opacity, const float *uv)
{
	if (!s_device || !s_cmd) return;

	// Get GPU textures from passed iTVPTexture2D objects
	auto getGPUTex = [](iTVPTexture2D *tex) -> SDL_GPUTexture* {
		if (!tex) return nullptr;
		tTVPGPUTexture2D *gpuTex = static_cast<tTVPGPUTexture2D*>(tex);
		gpuTex->UploadToGPU(s_device);
		return gpuTex->GetGPUTexture();
	};

	SDL_GPUTexture *tex0 = nullptr, *tex1 = nullptr, *tex2 = nullptr;
	if (textures.size() >= 1)
		tex0 = getGPUTex(textures[0].first);
	// For dest-read methods (numTextures >= 2), textures[1] is the dest copy
	if (method->m_numTextures >= 2 && textures.size() >= 2) {
		tex1 = getGPUTex(textures[1].first);
	} else if (method->m_numTextures >= 2 && textures.size() == 1) {
		// Only one texture provided — use dest-read temp texture as tex1
		tex1 = s_tempDest;
	}
	if (textures.size() >= 3)
		tex2 = getGPUTex(textures[2].first);

	// Get pipeline from cache
	SDL_GPUTextureFormat rtFmt = SDL_GetGPUSwapchainTextureFormat(s_device, s_window);
	SDL_GPUGraphicsPipeline *pipe = _GetOrCreatePipeline(
		method->m_fs, method->m_blend, rtFmt,
		method->m_numTextures, method->m_numUBOs);
	if (!pipe) return;

	// Build UBO data
	float uboBuf[24] = {}; // 24 floats = 96 bytes max
	int uboSize = 0;
	float uboBuf2[4] = {};
	int uboSize2 = 0;

	SDL_GPUShader *fs = method->m_fs;

	if (fs == s_fill_fs || fs == s_fill_mask_fs) {
		// FillColor { vec4 color }
		float c[4] = { fillColor ? fillColor[0] : 1,
			fillColor ? fillColor[1] : 1,
			fillColor ? fillColor[2] : 1,
			fillColor ? fillColor[3] : 1 };
		memcpy(uboBuf, c, 16);
		uboSize = 16;
	} else if (fs == s_crossfade_fs) {
		// FragParams { uvOffset, uvScale, opacity, pad, uvOffset1, uvScale1, pad2 }
		float ou = uv ? uv[0] : 0, ov = uv ? uv[1] : 0;
		float su = uv ? uv[2] : 1, sv = uv ? uv[3] : 1;
		uboBuf[0] = ou; uboBuf[1] = ov; uboBuf[2] = su; uboBuf[3] = sv;
		uboBuf[4] = opacity; uboBuf[5] = 0;
		// Second UV set = same (for now)
		uboBuf[6] = ou; uboBuf[7] = ov; uboBuf[8] = su; uboBuf[9] = sv;
		uboSize = 40;
	} else if (fs == s_univ_trans_fs) {
		// FragParams { uvOffset, uvScale, opacity, pad, phase, vague, pad1, pad2 }
		float ou = uv ? uv[0] : 0, ov = uv ? uv[1] : 0;
		float su = uv ? uv[2] : 1, sv = uv ? uv[3] : 1;
		uboBuf[0] = ou; uboBuf[1] = ov; uboBuf[2] = su; uboBuf[3] = sv;
		uboBuf[4] = opacity; uboBuf[5] = 0;
		uboBuf[6] = 0.5f; uboBuf[7] = 0.1f; // phase, vague defaults
		uboSize = 32;
	} else if (fs == s_adjust_gamma_fs) {
		float ou = uv ? uv[0] : 0, ov = uv ? uv[1] : 0;
		float su = uv ? uv[2] : 1, sv = uv ? uv[3] : 1;
		uboBuf[0] = ou; uboBuf[1] = ov; uboBuf[2] = su; uboBuf[3] = sv;
		uboBuf[4] = opacity; uboBuf[5] = 0;
		uboBuf[6] = 1; uboBuf[7] = 1; uboBuf[8] = 1; uboBuf[9] = 1; // u_gamma
		uboBuf[10] = 0; uboBuf[11] = 0; uboBuf[12] = 0; uboBuf[13] = 0; // u_floor
		uboBuf[14] = 1; uboBuf[15] = 1; uboBuf[16] = 1; uboBuf[17] = 0; // u_amp
		uboSize = 72;
	} else if (fs == s_gamma_fs) {
		float ou = uv ? uv[0] : 0, ov = uv ? uv[1] : 0;
		float su = uv ? uv[2] : 1, sv = uv ? uv[3] : 1;
		uboBuf[0] = ou; uboBuf[1] = ov; uboBuf[2] = su; uboBuf[3] = sv;
		uboBuf[4] = opacity; uboBuf[5] = 0;
		uboBuf[6] = 1.0f; uboBuf[7] = 0; uboBuf[8] = 1.0f; // gamma, brightness, contrast
		uboSize = 36;
	} else if (fs == s_apply_colormap_fs || fs == s_apply_colormap_a_fs) {
		float ou = uv ? uv[0] : 0, ov = uv ? uv[1] : 0;
		float su = uv ? uv[2] : 1, sv = uv ? uv[3] : 1;
		uboBuf[0] = ou; uboBuf[1] = ov; uboBuf[2] = su; uboBuf[3] = sv;
		uboBuf[4] = opacity; uboBuf[5] = 0;
		uboSize = 24;
		// Second UBO: ColorParams { vec4 textColor }
		uboBuf2[0] = fillColor ? fillColor[0] : 1;
		uboBuf2[1] = fillColor ? fillColor[1] : 1;
		uboBuf2[2] = fillColor ? fillColor[2] : 1;
		uboBuf2[3] = fillColor ? fillColor[3] : 1;
		uboSize2 = 16;
	} else if (fs == s_apply_colormap_d_fs || fs == s_const_color_alpha_blend_d_fs) {
		float ou = uv ? uv[0] : 0, ov = uv ? uv[1] : 0;
		float su = uv ? uv[2] : 1, sv = uv ? uv[3] : 1;
		uboBuf[0] = ou; uboBuf[1] = ov; uboBuf[2] = su; uboBuf[3] = sv;
		uboBuf[4] = opacity; uboBuf[5] = 0;
		uboSize = 24;
		uboBuf2[0] = fillColor ? fillColor[0] : 1;
		uboBuf2[1] = fillColor ? fillColor[1] : 1;
		uboBuf2[2] = fillColor ? fillColor[2] : 1;
		uboBuf2[3] = fillColor ? fillColor[3] : 1;
		uboSize2 = 16;
	} else {
		// Default FragParams { uvOffset, uvScale, opacity, pad }
		float ou = uv ? uv[0] : 0, ov = uv ? uv[1] : 0;
		float su = uv ? uv[2] : 1, sv = uv ? uv[3] : 1;
		uboBuf[0] = ou; uboBuf[1] = ov; uboBuf[2] = su; uboBuf[3] = sv;
		uboBuf[4] = opacity; uboBuf[5] = 0;
		uboSize = 24;
	}

	_DrawQuadRaw(pipe, tex0, tex1, tex2,
		rctar.get_width(), rctar.get_height(),
		method->m_numTextures, method->m_numUBOs,
		uboBuf, uboSize, uboBuf2, uboSize2);
}

//==============================================================================
// Dest-read support: copy current target pixels to temp texture
//==============================================================================
void TVPRenderManager_GPU::_EnsureDestRead(iTVPTexture2D *tar, const tTVPRect &rctar) {
	if (!s_device || !s_cmd) return;
	tTVPGPUTexture2D *gpuTar = static_cast<tTVPGPUTexture2D*>(tar);
	gpuTar->UploadToGPU(s_device);
	SDL_GPUTexture *srcTex = gpuTar->GetGPUTexture();
	if (!srcTex) return;
	int w = rctar.get_width(), h = rctar.get_height();
	if (w <= 0 || h <= 0) { w = tar->GetWidth(); h = tar->GetHeight(); }
	if (!s_tempDest || s_tempW != w || s_tempH != h) {
		if (s_tempDest) SDL_ReleaseGPUTexture(s_device, s_tempDest);
		SDL_GPUTextureCreateInfo ti = {};
		ti.type = SDL_GPU_TEXTURETYPE_2D;
		ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		ti.width = (Uint32)w; ti.height = (Uint32)h;
		ti.layer_count_or_depth = 1; ti.num_levels = 1;
		ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
		s_tempDest = SDL_CreateGPUTexture(s_device, &ti);
		s_tempW = w; s_tempH = h;
	}
	if (!s_tempDest) return;
	// Copy srcTex → s_tempDest
	SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(s_cmd);
	if (cp) {
		SDL_GPUTextureLocation s = {}, d = {};
		s.texture = srcTex;
		d.texture = s_tempDest;
		SDL_CopyGPUTextureToTexture(cp, &s, &d, (Uint32)w, (Uint32)h, 1, false);
		SDL_EndGPUCopyPass(cp);
	}
}

//==============================================================================
// TVPRenderManager_GPU
//==============================================================================
TVPRenderManager_GPU::TVPRenderManager_GPU() { s_instance = this; }

TVPRenderManager_GPU::~TVPRenderManager_GPU() {
	if (s_instance == this) s_instance = nullptr;
}

bool TVPRenderManager_GPU::InitDevice(SDL_Window *window) {
	if (s_initialized) return true;
	s_window = window;

	// Create GPU device (Vulkan via SDL3 GPU API)
	SDL_PropertiesID props = SDL_CreateProperties();
	SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
	SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "vulkan");
	s_device = SDL_CreateGPUDeviceWithProperties(props);
	SDL_DestroyProperties(props);
	if (!s_device) {
		__android_log_print(ANDROID_LOG_ERROR, TAG, "SDL_CreateGPUDevice: %s", SDL_GetError());
		return false;
	}
	if (!SDL_ClaimWindowForGPUDevice(s_device, window)) {
		__android_log_print(ANDROID_LOG_ERROR, TAG, "ClaimWindow: %s", SDL_GetError());
		SDL_DestroyGPUDevice(s_device); s_device = nullptr;
		return false;
	}

	// Sampler
	SDL_GPUSamplerCreateInfo si = {};
	si.min_filter = SDL_GPU_FILTER_LINEAR;
	si.mag_filter = SDL_GPU_FILTER_LINEAR;
	si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
	si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	s_sampler = SDL_CreateGPUSampler(s_device, &si);

	// Vertex shader
	s_vs = _CreateShader(s_device, g_quad_vertSpv, g_quad_vertSpvSize,
		SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
	if (!s_vs) { __android_log_print(ANDROID_LOG_ERROR, TAG, "VS failed"); Shutdown(); return false; }

	// Create all fragment shaders
	auto createFS = [&](const unsigned char *code, unsigned int size,
		const char *name, int samplers, int ubos) -> SDL_GPUShader* {
		SDL_GPUShader *s = _CreateShader(s_device, code, size,
			SDL_GPU_SHADERSTAGE_FRAGMENT, samplers, ubos);
		if (!s) __android_log_print(ANDROID_LOG_ERROR, TAG, "FS '%s' failed", name);
		return s;
	};

	s_quad_fs = createFS(g_quad_fragSpv, g_quad_fragSpvSize, "quad_frag", 1, 1);
	s_quad_pma_fs = createFS(g_quad_pma_fragSpv, g_quad_pma_fragSpvSize, "quad_pma_frag", 1, 1);
	s_present_fs = createFS(g_present_fragSpv, g_present_fragSpvSize, "present_frag", 1, 0);
	s_fill_fs = createFS(g_fill_fragSpv, g_fill_fragSpvSize, "fill_frag", 0, 1);
	s_fill_mask_fs = createFS(g_fill_mask_fragSpv, g_fill_mask_fragSpvSize, "fill_mask_frag", 0, 1);
	s_copy_color_fs = createFS(g_copy_color_fragSpv, g_copy_color_fragSpvSize, "copy_color_frag", 1, 1);
	s_copy_mask_fs = createFS(g_copy_mask_fragSpv, g_copy_mask_fragSpvSize, "copy_mask_frag", 1, 1);
	s_copy_opaque_fs = createFS(g_copy_opaque_fragSpv, g_copy_opaque_fragSpvSize, "copy_opaque_frag", 2, 1);
	s_remove_opacity_fs = createFS(g_remove_opacity_fragSpv, g_remove_opacity_fragSpvSize, "remove_opacity_frag", 1, 1);
	s_gray_fs = createFS(g_gray_fragSpv, g_gray_fragSpvSize, "gray_frag", 1, 1);
	s_blur_fs = createFS(g_blur_fragSpv, g_blur_fragSpvSize, "blur_frag", 1, 1);
	s_gamma_fs = createFS(g_gamma_fragSpv, g_gamma_fragSpvSize, "gamma_frag", 1, 1);
	s_adjust_gamma_fs = createFS(g_adjust_gamma_fragSpv, g_adjust_gamma_fragSpvSize, "adjust_gamma_frag", 1, 1);
	s_crossfade_fs = createFS(g_crossfade_fragSpv, g_crossfade_fragSpvSize, "crossfade_frag", 2, 1);
	s_univ_trans_fs = createFS(g_univ_trans_fragSpv, g_univ_trans_fragSpvSize, "univ_trans_frag", 3, 1);
	s_alpha_blend_d_fs = createFS(g_alpha_blend_d_fragSpv, g_alpha_blend_d_fragSpvSize, "alpha_blend_d_frag", 2, 1);
	s_const_alpha_blend_d_fs = createFS(g_const_alpha_blend_d_fragSpv, g_const_alpha_blend_d_fragSpvSize, "const_alpha_blend_d_frag", 2, 1);
	s_const_color_alpha_blend_d_fs = createFS(g_const_color_alpha_blend_d_fragSpv, g_const_color_alpha_blend_d_fragSpvSize, "const_color_alpha_blend_d_frag", 2, 2);
	s_apply_colormap_fs = createFS(g_apply_colormap_fragSpv, g_apply_colormap_fragSpvSize, "apply_colormap_frag", 1, 2);
	s_apply_colormap_a_fs = createFS(g_apply_colormap_a_fragSpv, g_apply_colormap_a_fragSpvSize, "apply_colormap_a_frag", 1, 2);
	s_apply_colormap_d_fs = createFS(g_apply_colormap_d_fragSpv, g_apply_colormap_d_fragSpvSize, "apply_colormap_d_frag", 2, 2);
	s_ps_overlay_fs = createFS(g_ps_overlay_fragSpv, g_ps_overlay_fragSpvSize, "ps_overlay_frag", 2, 1);
	s_ps_hardlight_fs = createFS(g_ps_hardlight_fragSpv, g_ps_hardlight_fragSpvSize, "ps_hardlight_frag", 2, 1);
	s_ps_softlight_fs = createFS(g_ps_softlight_fragSpv, g_ps_softlight_fragSpvSize, "ps_softlight_frag", 2, 1);
	s_ps_colordodge_fs = createFS(g_ps_colordodge_fragSpv, g_ps_colordodge_fragSpvSize, "ps_colordodge_frag", 2, 1);
	s_ps_colorburn_fs = createFS(g_ps_colorburn_fragSpv, g_ps_colorburn_fragSpvSize, "ps_colorburn_frag", 2, 1);
	s_ps_diff_fs = createFS(g_ps_diff_fragSpv, g_ps_diff_fragSpvSize, "ps_diff_frag", 2, 1);
	s_ps_exclusion_fs = createFS(g_ps_exclusion_fragSpv, g_ps_exclusion_fragSpvSize, "ps_exclusion_frag", 2, 1);
	s_ps_lighten_fs = createFS(g_ps_lighten_fragSpv, g_ps_lighten_fragSpvSize, "ps_lighten_frag", 2, 1);
	s_ps_darken_fs = createFS(g_ps_darken_fragSpv, g_ps_darken_fragSpvSize, "ps_darken_frag", 2, 1);

	// Vertex buffer
	SDL_GPUBufferCreateInfo bi = {};
	bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
	bi.size = sizeof(s_quadData);
	s_quadVerts = SDL_CreateGPUBuffer(s_device, &bi);
	if (!s_quadVerts) { __android_log_print(ANDROID_LOG_ERROR, TAG, "Vert buf failed"); Shutdown(); return false; }
	_UploadQuadVerts(s_device, s_quadVerts);

	// Present pipeline (no blend, direct copy with alpha=1)
	{
		SDL_GPUTextureFormat fmt = SDL_GetGPUSwapchainTextureFormat(s_device, s_window);
		BlendConfig b; b.enable = false;
		s_presentPipe = _GetOrCreatePipeline(s_present_fs, b, fmt, 1, 0);
	}

	s_initialized = true;
	// Create the singleton instance
	if (!s_instance) new TVPRenderManager_GPU();
	__android_log_print(ANDROID_LOG_INFO, TAG, "GPU device initialized (%d shaders, SPIR-V)", 31);
	return true;
}

void TVPRenderManager_GPU::Shutdown() {
	if (s_device) SDL_WaitForGPUIdle(s_device);
	auto releasePipe = [&](SDL_GPUGraphicsPipeline *&p) {
		if (p && s_device) { SDL_ReleaseGPUGraphicsPipeline(s_device, p); p = nullptr; }
	};
	auto releaseShader = [&](SDL_GPUShader *&s) {
		if (s && s_device) { SDL_ReleaseGPUShader(s_device, s); s = nullptr; }
	};
	releasePipe(s_presentPipe);
	if (s_quadVerts && s_device) { SDL_ReleaseGPUBuffer(s_device, s_quadVerts); s_quadVerts = nullptr; }
	if (s_sampler && s_device) { SDL_ReleaseGPUSampler(s_device, s_sampler); s_sampler = nullptr; }
	releaseShader(s_vs);
	releaseShader(s_quad_fs); releaseShader(s_quad_pma_fs); releaseShader(s_present_fs);
	releaseShader(s_fill_fs); releaseShader(s_fill_mask_fs);
	releaseShader(s_copy_color_fs); releaseShader(s_copy_mask_fs); releaseShader(s_copy_opaque_fs);
	releaseShader(s_remove_opacity_fs); releaseShader(s_gray_fs); releaseShader(s_blur_fs);
	releaseShader(s_gamma_fs); releaseShader(s_adjust_gamma_fs);
	releaseShader(s_crossfade_fs); releaseShader(s_univ_trans_fs);
	releaseShader(s_alpha_blend_d_fs); releaseShader(s_const_alpha_blend_d_fs);
	releaseShader(s_const_color_alpha_blend_d_fs);
	releaseShader(s_apply_colormap_fs); releaseShader(s_apply_colormap_a_fs); releaseShader(s_apply_colormap_d_fs);
	releaseShader(s_ps_overlay_fs); releaseShader(s_ps_hardlight_fs); releaseShader(s_ps_softlight_fs);
	releaseShader(s_ps_colordodge_fs); releaseShader(s_ps_colorburn_fs); releaseShader(s_ps_diff_fs);
	releaseShader(s_ps_exclusion_fs); releaseShader(s_ps_lighten_fs); releaseShader(s_ps_darken_fs);

	if (s_tempDest && s_device) { SDL_ReleaseGPUTexture(s_device, s_tempDest); s_tempDest = nullptr; }
	for (auto &kv : s_pipeCache)
		if (kv.second && s_device) SDL_ReleaseGPUGraphicsPipeline(s_device, kv.second);
	s_pipeCache.clear();
	if (s_device && s_window) { SDL_ReleaseWindowFromGPUDevice(s_device, s_window); }
	if (s_device) { SDL_DestroyGPUDevice(s_device); s_device = nullptr; }
	s_window = nullptr; s_initialized = false;
}

bool TVPRenderManager_GPU::GetRenderStat(unsigned int &drawCount, uint64_t &vmemsize) {
	drawCount = 0;
	vmemsize = tTVPGPUTexture2D::s_gpuTotalVMem.load();
	return true;
}

void TVPRenderManager_GPU::Initialize() {
	RegisterMethods();
}

//==============================================================================
// Texture creation
//==============================================================================
iTVPTexture2D* TVPRenderManager_GPU::CreateTexture2D(const void *pixel, int pitch,
	unsigned int w, unsigned int h, TVPTextureFormat::e format, int flags)
{
	return new tTVPGPUTexture2D((int)w, (int)h, format, false, pixel, pitch);
}

iTVPTexture2D* TVPRenderManager_GPU::CreateTexture2D(tTVPBitmap* bmp) {
	if (!bmp) return nullptr;
	TVPTextureFormat::e fmt = (bmp->GetBPP() == 8) ? TVPTextureFormat::Gray : TVPTextureFormat::RGBA;
	return new tTVPGPUTexture2D((int)bmp->GetWidth(), (int)bmp->GetHeight(),
		fmt, bmp->IsOpaque, bmp->GetBits(), bmp->GetPitch());
}

iTVPTexture2D* TVPRenderManager_GPU::CreateTexture2D(TJS::tTJSBinaryStream* s) { return nullptr; }

iTVPTexture2D* TVPRenderManager_GPU::CreateTexture2D(unsigned int neww,
	unsigned int newh, iTVPTexture2D* tex)
{
	auto *src = static_cast<tTVPGPUTexture2D*>(tex);
	TVPTextureFormat::e fmt = src ? src->GetFormat() : TVPTextureFormat::RGBA;
	auto *dst = new tTVPGPUTexture2D((int)neww, (int)newh, fmt, false);
	if (src) {
		int cw = ((int)neww < (int)src->GetWidth()) ? (int)neww : (int)src->GetWidth();
		int ch = ((int)newh < (int)src->GetHeight()) ? (int)newh : (int)src->GetHeight();
		for (int y = 0; y < ch; y++)
			memcpy(dst->GetScanLineForWrite(y), src->GetScanLineForRead(y), (size_t)(cw * 4));
	}
	return dst;
}

//==============================================================================
// RegisterMethods — register all blend methods
//==============================================================================
void TVPRenderManager_GPU::RegisterMethods() {
	if (!s_device) return; // device not ready, will retry after InitDevice
	static bool registered = false;
	if (registered) return;

	auto reg = [&](const char *name, SDL_GPUShader *fs,
		int tex, int ubos, const BlendConfig &b) -> tTVPGPURenderMethod*
	{
		auto *m = new tTVPGPURenderMethod(fs, tex, ubos, b);
		RegisterRenderMethod(name, m);
		return m;
	};

	BlendConfig noBlend = {}; noBlend.enable = false;
	BlendConfig overwrite = {}; overwrite.enable = false; // ONE/ZERO = overwrite
	BlendConfig copyColor = {}; // ONE/ZERO RGB, ZERO/ONE A
	copyColor.srcAlpha = SDL_GPU_BLENDFACTOR_ZERO;
	copyColor.dstAlpha = SDL_GPU_BLENDFACTOR_ONE;
	BlendConfig copyMask = {}; // ZERO/ONE RGB, ONE/ZERO A
	copyMask.srcColor = SDL_GPU_BLENDFACTOR_ZERO;
	copyMask.dstColor = SDL_GPU_BLENDFACTOR_ONE;

	BlendConfig alphaBlend = {}; // SRC_A/OMSA both
	alphaBlend.srcColor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
	alphaBlend.dstColor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	alphaBlend.srcAlpha = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
	alphaBlend.dstAlpha = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

	BlendConfig alphaBlend_a = {}; // SRC_A/OMSA RGB, ONE/OMSA A
	alphaBlend_a.srcColor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
	alphaBlend_a.dstColor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	alphaBlend_a.srcAlpha = SDL_GPU_BLENDFACTOR_ONE;
	alphaBlend_a.dstAlpha = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

	BlendConfig additive = {}; // ONE/OMSA RGB, ZERO/ONE A
	additive.srcColor = SDL_GPU_BLENDFACTOR_ONE;
	additive.dstColor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	additive.srcAlpha = SDL_GPU_BLENDFACTOR_ZERO;
	additive.dstAlpha = SDL_GPU_BLENDFACTOR_ONE;

	BlendConfig additive_a = {}; // ONE/OMSA both
	additive_a.srcColor = SDL_GPU_BLENDFACTOR_ONE;
	additive_a.dstColor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	additive_a.srcAlpha = SDL_GPU_BLENDFACTOR_ONE;
	additive_a.dstAlpha = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

	BlendConfig multiply = {}; // ZERO/SRC_COLOR RGB, ZERO/ONE A
	multiply.srcColor = SDL_GPU_BLENDFACTOR_ZERO;
	multiply.dstColor = SDL_GPU_BLENDFACTOR_SRC_COLOR;
	multiply.srcAlpha = SDL_GPU_BLENDFACTOR_ZERO;
	multiply.dstAlpha = SDL_GPU_BLENDFACTOR_ONE;

	BlendConfig screen = {}; // ONE/OMSRC_COLOR RGB, ZERO/ONE A
	screen.srcColor = SDL_GPU_BLENDFACTOR_ONE;
	screen.dstColor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
	screen.srcAlpha = SDL_GPU_BLENDFACTOR_ZERO;
	screen.dstAlpha = SDL_GPU_BLENDFACTOR_ONE;

	BlendConfig minBlend = {}; // ONE/ONE, MIN
	minBlend.colorOp = SDL_GPU_BLENDOP_MIN;
	minBlend.alphaOp = SDL_GPU_BLENDOP_MIN;
	minBlend.srcColor = SDL_GPU_BLENDFACTOR_ONE;
	minBlend.dstColor = SDL_GPU_BLENDFACTOR_ONE;
	minBlend.srcAlpha = SDL_GPU_BLENDFACTOR_ONE;
	minBlend.dstAlpha = SDL_GPU_BLENDFACTOR_ONE;

	BlendConfig maxBlend = {}; // ONE/ONE, MAX
	maxBlend.colorOp = SDL_GPU_BLENDOP_MAX;
	maxBlend.alphaOp = SDL_GPU_BLENDOP_MAX;
	maxBlend.srcColor = SDL_GPU_BLENDFACTOR_ONE;
	maxBlend.dstColor = SDL_GPU_BLENDFACTOR_ONE;
	maxBlend.srcAlpha = SDL_GPU_BLENDFACTOR_ONE;
	maxBlend.dstAlpha = SDL_GPU_BLENDFACTOR_ONE;

	BlendConfig subBlend = {}; // CONST_COLOR/ONE, REV_SUB
	subBlend.colorOp = SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
	subBlend.alphaOp = SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
	subBlend.srcColor = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
	subBlend.dstColor = SDL_GPU_BLENDFACTOR_ONE;
	subBlend.srcAlpha = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
	subBlend.dstAlpha = SDL_GPU_BLENDFACTOR_ONE;

	BlendConfig constAlpha = {}; // CONST_COLOR/OMCONST_COLOR both (constant alpha set via blend factor)
	constAlpha.srcColor = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
	constAlpha.dstColor = SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
	constAlpha.srcAlpha = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
	constAlpha.dstAlpha = SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;

	// RemoveConstOpacity: ZERO/ONE RGB, ZERO/OMSA A
	BlendConfig removeConstOp = {};
	removeConstOp.srcColor = SDL_GPU_BLENDFACTOR_ZERO;
	removeConstOp.dstColor = SDL_GPU_BLENDFACTOR_ONE;
	removeConstOp.srcAlpha = SDL_GPU_BLENDFACTOR_ZERO;
	removeConstOp.dstAlpha = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

	// AlphaToAdditiveAlpha: ONE/ZERO RGB, SRC_A/ZERO A
	BlendConfig alphaToAdd = {};
	alphaToAdd.srcColor = SDL_GPU_BLENDFACTOR_ONE;
	alphaToAdd.dstColor = SDL_GPU_BLENDFACTOR_ZERO;
	alphaToAdd.srcAlpha = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
	alphaToAdd.dstAlpha = SDL_GPU_BLENDFACTOR_ZERO;

	// AdditiveAlphaToAlpha: ONE/ZERO RGB, ZERO/SRC_A A
	BlendConfig addToAlpha = {};
	addToAlpha.srcColor = SDL_GPU_BLENDFACTOR_ONE;
	addToAlpha.dstColor = SDL_GPU_BLENDFACTOR_ZERO;
	addToAlpha.srcAlpha = SDL_GPU_BLENDFACTOR_ZERO;
	addToAlpha.dstAlpha = SDL_GPU_BLENDFACTOR_SRC_ALPHA;

	// Group A: Copy/Fill (already implemented)
	reg("Copy", s_quad_fs, 1, 1, noBlend);
	reg("CopyColor", s_copy_color_fs, 1, 1, copyColor);
	reg("CopyMask", s_copy_mask_fs, 1, 1, copyMask);
	reg("FillARGB", s_fill_fs, 0, 1, noBlend);
	reg("FillColor", s_fill_fs, 0, 1, noBlend);
	reg("FillMask", s_fill_mask_fs, 0, 1, copyMask);
	reg("RemoveOpacity", s_remove_opacity_fs, 1, 1, noBlend);
	reg("RemoveConstOpacity", s_remove_opacity_fs, 1, 1, removeConstOp);

	// Group B: Standard alpha blends (quad.frag)
	reg("AlphaBlend", s_quad_fs, 1, 1, alphaBlend);
	reg("AlphaBlend_color", s_quad_fs, 1, 1, alphaBlend);
	reg("PsAlphaBlend", s_quad_fs, 1, 1, alphaBlend);
	reg("AlphaBlend_a", s_quad_fs, 1, 1, alphaBlend_a);
	reg("AlphaTest", s_quad_fs, 1, 1, alphaBlend_a);
	reg("DarkenBlend", s_quad_fs, 1, 1, minBlend);
	reg("LightenBlend", s_quad_fs, 1, 1, maxBlend);
	reg("AlphaToAdditiveAlpha", s_quad_fs, 1, 1, alphaToAdd);
	reg("AdditiveAlphaToAlpha", s_quad_fs, 1, 1, addToAlpha);

	// Group C: Premultiplied alpha blends (quad_pma.frag)
	reg("AddBlend", s_quad_pma_fs, 1, 1, additive);
	reg("AdditiveAlphaBlend", s_quad_pma_fs, 1, 1, additive);
	reg("AdditiveAlphaBlend_a", s_quad_pma_fs, 1, 1, additive_a);
	reg("MulBlend", s_quad_pma_fs, 1, 1, multiply);
	reg("MulBlend_HDA", s_quad_pma_fs, 1, 1, multiply);
	reg("ScreenBlend", s_quad_pma_fs, 1, 1, screen);
	reg("SubBlend", s_quad_pma_fs, 1, 1, subBlend);
	reg("PsSubBlend", s_quad_pma_fs, 1, 1, subBlend);
	reg("ConstAlphaBlend", s_quad_pma_fs, 1, 1, constAlpha);
	reg("ConstAlphaBlend_a", s_quad_pma_fs, 1, 1, constAlpha);
	reg("AlphaBlend_SD", s_quad_pma_fs, 1, 1, constAlpha);
	reg("ConstColorAlphaBlend", s_fill_fs, 0, 1, constAlpha); // fill color from UBO

	// Group D: Special effects
	reg("DoGrayScale", s_gray_fs, 1, 1, noBlend);
	reg("BoxBlur", s_blur_fs, 1, 1, noBlend);
	reg("BoxBlurAlpha", s_blur_fs, 1, 1, noBlend);
	reg("AdjustGamma", s_adjust_gamma_fs, 1, 1, noBlend);
	reg("AdjustGamma_a", s_adjust_gamma_fs, 1, 1, noBlend);

	// Group E: Dest-read methods (no blend, use temp dest texture)
	reg("CopyOpaqueImage", s_copy_opaque_fs, 2, 1, noBlend);
	reg("AlphaBlend_d", s_alpha_blend_d_fs, 2, 1, noBlend);
	reg("AlphaBlend_d_a", s_alpha_blend_d_fs, 2, 1, noBlend);
	reg("PsOverlayBlend", s_ps_overlay_fs, 2, 1, noBlend);
	reg("PsHardLightBlend", s_ps_hardlight_fs, 2, 1, noBlend);
	reg("PsSoftLightBlend", s_ps_softlight_fs, 2, 1, noBlend);
	reg("PsColorDodgeBlend", s_ps_colordodge_fs, 2, 1, noBlend);
	reg("PsColorBurnBlend", s_ps_colorburn_fs, 2, 1, noBlend);
	reg("PsDiffBlend", s_ps_diff_fs, 2, 1, noBlend);
	reg("PsExclusionBlend", s_ps_exclusion_fs, 2, 1, noBlend);
	reg("PsLightenBlend", s_ps_lighten_fs, 2, 1, noBlend);
	reg("PsDarkenBlend", s_ps_darken_fs, 2, 1, noBlend);

	// Group F: Crossfade (source-dest constant alpha)
	reg("ConstAlphaBlend_SD", s_crossfade_fs, 2, 1, noBlend);
	reg("ConstAlphaBlend_SD_a", s_crossfade_fs, 2, 1, noBlend);
	reg("ConstAlphaBlend_SD_d", s_crossfade_fs, 2, 1, noBlend);

	// Group G: Const alpha dest-read
	reg("ConstAlphaBlend_d", s_const_alpha_blend_d_fs, 2, 1, noBlend);
	reg("ConstColorAlphaBlend_d", s_const_color_alpha_blend_d_fs, 2, 2, noBlend);

	// Group H: ApplyColorMap (text rendering)
	reg("ApplyColorMap", s_apply_colormap_fs, 1, 2, alphaBlend);
	reg("ApplyColorMap_a", s_apply_colormap_a_fs, 1, 2, additive_a);
	reg("ApplyColorMap_d", s_apply_colormap_d_fs, 2, 2, noBlend);

	// Group I: UnivTrans (rule-driven transition)
	reg("UnivTransBlend", s_univ_trans_fs, 3, 1, noBlend);
	reg("UnivTransBlend_d", s_univ_trans_fs, 3, 1, noBlend);
	reg("UnivTransBlend_a", s_univ_trans_fs, 3, 1, noBlend);

	registered = true;
	__android_log_print(ANDROID_LOG_INFO, TAG, "Registered GPU render methods");
}

//==============================================================================
// OperateRect — dispatch compositing to GPU
//==============================================================================
void TVPRenderManager_GPU::OperateRect(iTVPRenderMethod* method,
	iTVPTexture2D *tar, iTVPTexture2D *reftar, const tTVPRect& rctar,
	const tRenderTexRectArray &textures)
{
	tTVPGPURenderMethod *m = static_cast<tTVPGPURenderMethod*>(method);

	// For dest-read methods, copy target to temp texture first
	bool destRead = (m->m_numTextures >= 2);
	if (destRead && tar) {
		_EnsureDestRead(tar, rctar);
		// Append temp texture as additional texture source
		tRenderTexRectArray texArray = textures;
		// texArray[0] is the source, texArray[1] should be the dest copy
		// We create a fake iTVPTexture2D that wraps the temp GPU texture
		// Actually, we need to pass s_tempDest as tex1 to _DrawQuadRaw
		// This is handled via the method's numTextures count
		// _DrawQuad checks textures.size() and if < numTextures, fills the rest
		// with the dest-read temp texture
		_DrawQuad(m, tar, rctar, texArray);
	} else {
		_DrawQuad(m, tar, rctar, textures);
	}
}

void TVPRenderManager_GPU::OperateTriangles(iTVPRenderMethod* method, int nTriangles,
	iTVPTexture2D *target, iTVPTexture2D *reftar,
	const tTVPRect& rcclip, const tTVPPointD* pttar,
	const tRenderTexQuadArray &textures)
{
	(void)method; (void)nTriangles; (void)target; (void)reftar;
	(void)rcclip; (void)pttar; (void)textures;
}

void TVPRenderManager_GPU::OperatePerspective(iTVPRenderMethod* method, int nQuads,
	iTVPTexture2D *target, iTVPTexture2D *reftar,
	const tTVPRect& rcclip, const tTVPPointD* pttar,
	const tRenderTexQuadArray &textures)
{
	(void)method; (void)nQuads; (void)target; (void)reftar;
	(void)rcclip; (void)pttar; (void)textures;
}

//==============================================================================
// Frame lifecycle
//==============================================================================
void TVPRenderManager_GPU::BeginFrame() {
	if (!s_device) return;
	s_cmd = SDL_AcquireGPUCommandBuffer(s_device);
	if (!s_cmd)
		__android_log_print(ANDROID_LOG_WARN, TAG, "BeginFrame: acquire cmd buf failed");
	// Ensure layer texture exists (match window size if possible)
	int sw = 1280, sh = 720;
	if (s_window) {
		SDL_GetWindowSize(s_window, &sw, &sh);
	}
	if (!s_layerTex || s_layerW != sw || s_layerH != sh) {
		if (s_layerTex) SDL_ReleaseGPUTexture(s_device, s_layerTex);
		SDL_GPUTextureCreateInfo ti = {};
		ti.type = SDL_GPU_TEXTURETYPE_2D;
		ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		ti.width = (Uint32)sw; ti.height = (Uint32)sh;
		ti.layer_count_or_depth = 1; ti.num_levels = 1;
		ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
		s_layerTex = SDL_CreateGPUTexture(s_device, &ti);
		s_layerW = sw; s_layerH = sh;
	}
}

void TVPRenderManager_GPU::EndFrame() {
	if (!s_device || !s_cmd) return;

	// Present the swapchain — blit s_layerTex to swapchain
	SDL_GPUTexture *swapchainTex = nullptr;
	Uint32 swW = 0, swH = 0;
	if (!SDL_WaitAndAcquireGPUSwapchainTexture(s_cmd, s_window, &swapchainTex, &swW, &swH)) {
		SDL_SubmitGPUCommandBuffer(s_cmd);
		s_cmd = nullptr;
		return;
	}

	if (swapchainTex) {
		SDL_GPUColorTargetInfo ct = {};
		ct.texture = swapchainTex;
		ct.load_op = SDL_GPU_LOADOP_CLEAR;
		ct.store_op = SDL_GPU_STOREOP_STORE;
		ct.clear_color = {0, 0, 0, 1};

		SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(s_cmd, &ct, 1, NULL);
		if (rp) {
			SDL_BindGPUGraphicsPipeline(rp, s_presentPipe);
			SDL_GPUBufferBinding bb = { s_quadVerts, 0 };
			SDL_BindGPUVertexBuffers(rp, 0, &bb, 1);
			if (s_layerTex) {
				SDL_GPUTextureSamplerBinding ts = { s_layerTex, s_sampler };
				SDL_BindGPUFragmentSamplers(rp, 0, &ts, 1);
			}
			SDL_DrawGPUPrimitives(rp, 6, 1, 0, 0);
			SDL_EndGPURenderPass(rp);
		}
	}

	SDL_SubmitGPUCommandBuffer(s_cmd);
	s_cmd = nullptr;
}

void TVPRenderManager_GPU::PresentFrame(const uint8_t *pixels, int w, int h) {
	if (!s_device || !s_window || !pixels || w <= 0 || h <= 0) return;

	static SDL_GPUTexture *s_displayTex = nullptr;
	static int s_dispW = 0, s_dispH = 0;

	if (!s_displayTex || s_dispW != w || s_dispH != h) {
		if (s_displayTex) SDL_ReleaseGPUTexture(s_device, s_displayTex);
		SDL_GPUTextureCreateInfo ti = {};
		ti.type = SDL_GPU_TEXTURETYPE_2D;
		ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		ti.width = (Uint32)w; ti.height = (Uint32)h;
		ti.layer_count_or_depth = 1; ti.num_levels = 1;
		ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
		s_displayTex = SDL_CreateGPUTexture(s_device, &ti);
		s_dispW = w; s_dispH = h;
		if (!s_displayTex) return;
	}

	// Upload pixels with ABGR→RGBA conversion
	SDL_GPUTransferBufferCreateInfo tci = {};
	tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	tci.size = (Uint32)(w * h * 4);
	SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(s_device, &tci);
	if (!tb) return;
	uint8_t *map = (uint8_t*)SDL_MapGPUTransferBuffer(s_device, tb, false);
	if (map) {
		for (int y = 0; y < h; y++) {
			const uint8_t *src = pixels + y * w * 4;
			uint32_t *dst = (uint32_t*)(map + y * w * 4);
			for (int x = 0; x < w; x++) {
				uint32_t abgr = *(const uint32_t*)(src + x * 4);
				dst[x] = ((abgr & 0xFF) << 24) | ((abgr & 0xFF00) << 8) |
				         ((abgr & 0xFF0000) >> 8) | ((abgr & 0xFF000000) >> 24);
			}
		}
	}
	SDL_UnmapGPUTransferBuffer(s_device, tb);

	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(s_device);
	if (!cmd) { SDL_ReleaseGPUTransferBuffer(s_device, tb); return; }

	SDL_GPUTexture *swapchainTex = nullptr;
	Uint32 swW = 0, swH = 0;
	if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, s_window, &swapchainTex, &swW, &swH)) {
		SDL_SubmitGPUCommandBuffer(cmd);
		SDL_ReleaseGPUTransferBuffer(s_device, tb);
		return;
	}

	// Upload to display texture
	{
		SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
		if (cp) {
			SDL_GPUTextureTransferInfo srcTI = { tb, 0 };
			SDL_GPUTextureRegion dstReg = {};
			dstReg.texture = s_displayTex;
			dstReg.w = (Uint32)w; dstReg.h = (Uint32)h; dstReg.d = 1;
			SDL_UploadToGPUTexture(cp, &srcTI, &dstReg, false);
			SDL_EndGPUCopyPass(cp);
		}
	}

	// Draw fullscreen quad
	SDL_GPUColorTargetInfo ct = {};
	ct.texture = swapchainTex;
	ct.load_op = SDL_GPU_LOADOP_CLEAR;
	ct.store_op = SDL_GPU_STOREOP_STORE;
	ct.clear_color = {0, 0, 0, 1};

	SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
	if (rp) {
		SDL_BindGPUGraphicsPipeline(rp, s_presentPipe);
		SDL_GPUBufferBinding bb = { s_quadVerts, 0 };
		SDL_BindGPUVertexBuffers(rp, 0, &bb, 1);
		SDL_GPUTextureSamplerBinding ts = { s_displayTex, s_sampler };
		SDL_BindGPUFragmentSamplers(rp, 0, &ts, 1);
		SDL_DrawGPUPrimitives(rp, 6, 1, 0, 0);
		SDL_EndGPURenderPass(rp);
	}

	SDL_SubmitGPUCommandBuffer(cmd);
	SDL_ReleaseGPUTransferBuffer(s_device, tb);
}

//==============================================================================
// Registration
//==============================================================================
static iTVPRenderManager* _GPUMgrFactory() {
	auto *inst = TVPRenderManager_GPU::Instance();
	if (inst) return inst;
	return new TVPRenderManager_GPU();
}

extern "C" void TVPRegisterGPURenderer() {
	static bool s_registered = false;
	if (s_registered) return;
	s_registered = true;
	TVPRegisterRenderManager("gpu", _GPUMgrFactory);
}
