#include "RenderManager_gpu.h"
#include <SDL3/SDL.h>
#include "tjsCommHead.h"
#include "visual/gpu/shaders/shaders_inc.h"
#include "DebugIntf.h"
#include "LayerBitmapIntf.h"
#include "GraphicsLoaderIntf.h"
#include "Application.h"
#include <android/log.h>
#include <cstring>
#include <new>

#define TAG "##gpu"

DisplayMode g_displayMode = DisplayMode::SOFTWARE;
SDL_Window *g_window = nullptr;
TVPRenderManager_GPU *TVPRenderManager_GPU::s_instance = nullptr;
std::atomic<uint64_t> TVPRenderManager_GPU::s_totalVMem;

// Debug capture mode (toggle from Java overlay)
static bool s_captureMode = false;
void SetCaptureMode(bool on) { s_captureMode = on; }
bool IsCaptureMode() { return s_captureMode; }

//==============================================================================
// Helpers
//==============================================================================
static SDL_GPUBlendFactor _mapBlendFactor(int glFactor) {
	switch (glFactor) {
	case 0: return SDL_GPU_BLENDFACTOR_ZERO;
	case 1: return SDL_GPU_BLENDFACTOR_ONE;
	case 0x0300: return SDL_GPU_BLENDFACTOR_SRC_COLOR;
	case 0x0301: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
	case 0x0302: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
	case 0x0303: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	case 0x0304: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
	case 0x0305: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
	case 0x0306: return SDL_GPU_BLENDFACTOR_DST_COLOR;
	case 0x0307: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
	case 0x8001: return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
	case 0x8002: return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
	case 0x8003: return SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;  // CONSTANT_ALPHA -> CONSTANT_COLOR
	case 0x8004: return SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
	case 0x8589: return SDL_GPU_BLENDFACTOR_SRC_COLOR;       // SRC1_COLOR -> SRC_COLOR
	case 0x858A: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
	case 0x858B: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;       // SRC1_ALPHA -> SRC_ALPHA
	case 0x858C: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	default: return SDL_GPU_BLENDFACTOR_ZERO;
	}
}

static SDL_GPUBlendOp _mapBlendOp(int glOp) {
	switch (glOp) {
	case 0x8006: return SDL_GPU_BLENDOP_ADD;
	case 0x800A: return SDL_GPU_BLENDOP_SUBTRACT;
	case 0x800B: return SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
	case 0x8007: return SDL_GPU_BLENDOP_MIN;
	case 0x8008: return SDL_GPU_BLENDOP_MAX;
	default: return SDL_GPU_BLENDOP_ADD;
	}
}

//==============================================================================
// tTVPGPUTexture2D
//==============================================================================
tTVPGPUTexture2D::tTVPGPUTexture2D(SDL_GPUDevice *dev, SDL_GPUTexture *tex,
	int texW, int texH, int w, int h,
	TVPTextureFormat::e fmt, bool opaque)
	: iTVPTexture2D(w, h), m_device(dev), m_texture(tex)
	, m_texW(texW), m_texH(texH), m_format(fmt), m_opaque(opaque) {
	m_width = w; m_height = h;
	m_pitch = w * 4;
	m_pixels.resize((size_t)(h * m_pitch));
}

tTVPGPUTexture2D::~tTVPGPUTexture2D() {
	if (m_device && m_texture) {
		TVPRenderManager_GPU::s_totalVMem -= (uint64_t)m_texW * m_texH * 4;
		SDL_ReleaseGPUTexture(m_device, m_texture);
	}
}

const void * tTVPGPUTexture2D::GetScanLineForRead(tjs_uint l) {
	if (l >= (tjs_uint)m_height) return nullptr;
	return m_pixels.data() + l * m_pitch;
}
void * tTVPGPUTexture2D::GetScanLineForWrite(tjs_uint l) {
	if (l >= (tjs_uint)m_height) return nullptr;
	return m_pixels.data() + l * m_pitch;
}

void tTVPGPUTexture2D::Update(const void *pixel, TVPTextureFormat::e format,
	int pitch, const tTVPRect& rc) {
	if (!m_device || !m_texture || !pixel) return;
	int w = rc.get_width(), h = rc.get_height();
	if (w <= 0 || h <= 0) { w = Width; h = Height; }

	SDL_GPUTransferBufferCreateInfo tci = {};
	tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	tci.size = (Uint32)(w * h * 4);
	SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(m_device, &tci);
	if (!tb) return;
	void *map = SDL_MapGPUTransferBuffer(m_device, tb, false);
	if (map) {
		if (pitch == w * 4) {
			memcpy(map, pixel, (size_t)(h * w * 4));
		} else {
			for (int y = 0; y < h; y++)
				memcpy((uint8_t*)map + y * w * 4,
					(const uint8_t*)pixel + y * pitch,
					(size_t)(w * 4));
		}
	}
	SDL_UnmapGPUTransferBuffer(m_device, tb);

	// End any active render pass first (Vulkan forbids copy inside render pass)
	auto *gpuMgr = TVPRenderManager_GPU::Instance();
	if (gpuMgr) gpuMgr->FlushPass();

	// Use current frame's command buffer if available (from TVPRenderManager_GPU)
	SDL_GPUCommandBuffer *cmd = TVPRenderManager_GPU::CurrentCmd();
	if (!cmd) cmd = SDL_AcquireGPUCommandBuffer(m_device);
	SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
	if (cp) {
		SDL_GPUTextureTransferInfo srcTI = { tb, 0 };
		SDL_GPUTextureRegion dstReg = {};
		dstReg.texture = m_texture;
		dstReg.w = (Uint32)w; dstReg.h = (Uint32)h; dstReg.d = 1;
		SDL_UploadToGPUTexture(cp, &srcTI, &dstReg, false);
		SDL_EndGPUCopyPass(cp);
	}
	if (!TVPRenderManager_GPU::CurrentCmd())
		SDL_SubmitGPUCommandBuffer(cmd);
	SDL_ReleaseGPUTransferBuffer(m_device, tb);
}

uint32_t tTVPGPUTexture2D::GetPoint(int x, int y) { return 0; }
void tTVPGPUTexture2D::SetPoint(int x, int y, uint32_t clr) {}

//==============================================================================
// tTVPGPURenderMethod
//==============================================================================
tTVPGPURenderMethod::tTVPGPURenderMethod(SDL_GPUDevice *dev,
	SDL_GPUGraphicsPipeline *pipe,
	SDL_GPUShader *vs, SDL_GPUShader *fs,
	const char *name, bool blend, int numTex,
	SDL_GPUBlendFactor srcC, SDL_GPUBlendFactor dstC,
	SDL_GPUBlendFactor srcA, SDL_GPUBlendFactor dstA,
	SDL_GPUBlendOp cOp, SDL_GPUBlendOp aOp)
	: m_device(dev), m_pipeline(pipe)
	, m_vertShader(vs), m_fragShader(fs)
	, m_blendEnabled(blend), m_numTextures(numTex)
	, m_srcColor(srcC), m_dstColor(dstC)
	, m_srcAlpha(srcA), m_dstAlpha(dstA)
	, m_colorOp(cOp), m_alphaOp(aOp)
	, m_hasConstantColor(false) {
	m_constColor[0] = m_constColor[1] = m_constColor[2] = m_constColor[3] = 1.0f;
	SetName(name);
}

tTVPGPURenderMethod::~tTVPGPURenderMethod() {
	if (m_device && m_pipeline)
		SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipeline);
}

void tTVPGPURenderMethod::SetParameterFloat(int id, float Value) {
	// Used for AdjustGamma, BoxBlur params
	if (id == 0) m_constColor[0] = Value;
	else if (id == 1) m_constColor[1] = Value;
}

void tTVPGPURenderMethod::SetParameterColor4B(int id, unsigned int clr) {
	m_constColor[0] = ((clr >> 16) & 0xFF) / 255.0f;
	m_constColor[1] = ((clr >> 8) & 0xFF) / 255.0f;
	m_constColor[2] = (clr & 0xFF) / 255.0f;
	m_constColor[3] = ((clr >> 24) & 0xFF) / 255.0f;
	m_hasConstantColor = true;
}

void tTVPGPURenderMethod::SetParameterOpa(int id, int Value) {
	m_opacity = Value & 0xFF;
}

int tTVPGPURenderMethod::EnumParameterID(const char *name) {
	if (!strcmp(name, "gammaAdjustData")) return 0x3a33aad6;
	if (!strcmp(name, "phase")) return 0x3a33aad7;
	if (!strcmp(name, "vague")) return 0x3a33aad8;
	return -1;
}

void tTVPGPURenderMethod::SetParameterInt(int id, int Value) {
	if (id == m_uboID_Vague) {
		m_vague = (float)(Value & 0xFF) / 255.0f;
		// Pack UnivTrans UBO: phase=(phase-m_vague)/255, vague=m_vague
		// phase is still pending — pack when phase arrives
	} else if (id == m_uboID_Phase) {
		float phaseNorm = ((float)(Value & 0xFF)) / 255.0f - m_vague;
		struct { float phase, vague, pad[2]; } ubo = { phaseNorm, m_vague, 0, 0 };
		memcpy(m_uboData, &ubo, sizeof(ubo));
		m_uboSize = sizeof(ubo);
		m_uboDirty = true;
	}
}

void tTVPGPURenderMethod::SetParameterPtr(int id, const void *v) {
	if (id == 0x3a33aad6 && v) {
		// AdjustGamma: tTVPGLGammaAdjustData (doubles) → 3 vec4 packed for std140
		const double *d = (const double*)v;
		double RGamma = d[0], GGamma = d[1], BGamma = d[2];
		double RCeil = d[3], GCeil = d[4], BCeil = d[5];
		double RFloor = d[6], GFloor = d[7], BFloor = d[8];
		float ubo[12];
		ubo[0] = 1.0f / (float)RGamma;
		ubo[1] = 1.0f / (float)GGamma;
		ubo[2] = 1.0f / (float)BGamma;
		ubo[3] = 0;
		ubo[4] = (float)RFloor / 255.0f;
		ubo[5] = (float)GFloor / 255.0f;
		ubo[6] = (float)BFloor / 255.0f;
		ubo[7] = 0;
		ubo[8] = ((float)RCeil - (float)RFloor) / 255.0f;
		ubo[9] = ((float)GCeil - (float)GFloor) / 255.0f;
		ubo[10] = ((float)BCeil - (float)BFloor) / 255.0f;
		ubo[11] = 0;
		memcpy(m_uboData, ubo, sizeof(ubo));
		m_uboSize = sizeof(ubo);
		m_uboDirty = true;
	}
}

iTVPRenderMethod* tTVPGPURenderMethod::SetBlendFuncSeparate(int func,
	int srcRGB, int dstRGB, int srcAlpha, int dstAlpha) {
	// GPU pipelines have baked blend state — ignore dynamic changes.
	// For OGL compat, we store the parameters but can't modify the pipeline.
	m_srcColor = _mapBlendFactor(srcRGB);
	m_dstColor = _mapBlendFactor(dstRGB);
	m_srcAlpha = _mapBlendFactor(srcAlpha);
	m_dstAlpha = _mapBlendFactor(dstAlpha);
	m_colorOp = _mapBlendOp(func);
	m_alphaOp = _mapBlendOp(func);
	return this;
}

//==============================================================================
// TVPRenderManager_GPU
//==============================================================================
// Vulkan: UV(0,0) = top-left, UV(1,1) = bottom-right.
// Engine scanline 0 = top → V=0 maps to screen top.
const float TVPRenderManager_GPU::s_quadVerts[24] = {
    -1,-1, 0,1,   1,-1, 1,1,   -1,1, 0,0,
    -1,1,  0,0,   1,-1, 1,1,    1,1, 1,0,
};

TVPRenderManager_GPU::TVPRenderManager_GPU() {
	s_instance = this;
}

TVPRenderManager_GPU::~TVPRenderManager_GPU() {
	if (s_instance == this) s_instance = nullptr;
	Shutdown();
}

bool TVPRenderManager_GPU::Init(SDL_Window *window) {
	if (m_device) return true;
	m_window = window;

	SDL_SetHint(SDL_HINT_RENDER_GPU_DEBUG, "1");
	m_device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, "vulkan");
	if (!m_device) {
		__android_log_print(ANDROID_LOG_ERROR, TAG,
			"SDL_CreateGPUDevice: %s", SDL_GetError());
		return false;
	}
	if (!SDL_ClaimWindowForGPUDevice(m_device, window)) {
		__android_log_print(ANDROID_LOG_ERROR, TAG,
			"ClaimWindow: %s", SDL_GetError());
		SDL_DestroyGPUDevice(m_device); m_device = nullptr;
		return false;
	}

	// Sampler
	SDL_GPUSamplerCreateInfo si = {};
	si.min_filter = SDL_GPU_FILTER_LINEAR;
	si.mag_filter = SDL_GPU_FILTER_LINEAR;
	si.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
	si.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	si.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
	m_sampler = SDL_CreateGPUSampler(m_device, &si);

	// Quad vertex buffer
	SDL_GPUBufferCreateInfo bi = {};
	bi.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
	bi.size = sizeof(s_quadVerts);
	m_quadVerts = SDL_CreateGPUBuffer(m_device, &bi);
	SDL_GPUTransferBufferCreateInfo tci = {};
	tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	tci.size = sizeof(s_quadVerts);
	SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(m_device, &tci);
	memcpy(SDL_MapGPUTransferBuffer(m_device, tb, false),
		s_quadVerts, sizeof(s_quadVerts));
	SDL_UnmapGPUTransferBuffer(m_device, tb);
	SDL_GPUCommandBuffer *uc = SDL_AcquireGPUCommandBuffer(m_device);
	SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(uc);
	SDL_GPUTransferBufferLocation srcTB = { tb, 0 };
	SDL_GPUBufferRegion dstBR = { m_quadVerts, 0, sizeof(s_quadVerts) };
	SDL_UploadToGPUBuffer(cp, &srcTB, &dstBR, false);
	SDL_EndGPUCopyPass(cp);
	SDL_SubmitGPUCommandBuffer(uc);
	SDL_ReleaseGPUTransferBuffer(m_device, tb);

	// Shared shaders
	SDL_GPUShaderCreateInfo sc = {};
	sc.format = SDL_GPU_SHADERFORMAT_SPIRV;
	sc.entrypoint = "main";
	sc.num_samplers = 0;
	sc.num_storage_textures = 0;
	sc.num_storage_buffers = 0;
	sc.num_uniform_buffers = 0;

	sc.stage = SDL_GPU_SHADERSTAGE_VERTEX;
	sc.code = (const Uint8*)quad_vertSpv;
	sc.code_size = quad_vertSpvSize;
	m_vs = SDL_CreateGPUShader(m_device, &sc);

	sc.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
	sc.code = (const Uint8*)quad_fragSpv;
	sc.code_size = quad_fragSpvSize;
	sc.num_samplers = 1;
	sc.num_uniform_buffers = 1;
	m_fs = SDL_CreateGPUShader(m_device, &sc);

	if (!m_vs || !m_fs) {
		__android_log_print(ANDROID_LOG_ERROR, TAG, "Shader creation failed");
		return false;
	}

	// Custom fragment shaders
	SDL_GPUShaderCreateInfo sc2 = {};
	sc2.format = SDL_GPU_SHADERFORMAT_SPIRV;
	sc2.entrypoint = "main";
	sc2.num_samplers = 1;
	sc2.num_storage_textures = 0;
	sc2.num_storage_buffers = 0;
	sc2.num_uniform_buffers = 0;
	sc2.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;

	sc2.code = (const Uint8*)gray_fragSpv;
	sc2.code_size = gray_fragSpvSize;
	m_fs_gray = SDL_CreateGPUShader(m_device, &sc2);

	sc2.code = (const Uint8*)blur_fragSpv;
	sc2.code_size = blur_fragSpvSize;
	m_fs_blur = SDL_CreateGPUShader(m_device, &sc2);

	// Custom shaders with UBO
	sc2.num_samplers = 1;
	sc2.num_uniform_buffers = 1;
	sc2.code = (const Uint8*)adjust_gamma_fragSpv;
	sc2.code_size = adjust_gamma_fragSpvSize;
	m_fs_adjustGamma = SDL_CreateGPUShader(m_device, &sc2);

	sc2.num_samplers = 3;
	sc2.code = (const Uint8*)univ_trans_fragSpv;
	sc2.code_size = univ_trans_fragSpvSize;
	m_fs_univTrans = SDL_CreateGPUShader(m_device, &sc2);

	// Fill shader (solid color, no texture, no UV)
	sc2.num_samplers = 0;
	sc2.code = (const Uint8*)fill_fragSpv;
	sc2.code_size = fill_fragSpvSize;
	m_fs_fill = SDL_CreateGPUShader(m_device, &sc2);

	// Present shader (forces alpha=1.0 for display)
	sc2.num_samplers = 1;
	sc2.num_uniform_buffers = 0;
	sc2.code = (const Uint8*)present_fragSpv;
	sc2.code_size = present_fragSpvSize;
	m_fs_present = SDL_CreateGPUShader(m_device, &sc2);

	// Crossfade shader (2-texture blend for transitions)
	sc2.num_samplers = 2;
	sc2.num_uniform_buffers = 1;
	sc2.code = (const Uint8*)crossfade_fragSpv;
	sc2.code_size = crossfade_fragSpvSize;
	m_fs_crossfade = SDL_CreateGPUShader(m_device, &sc2);

	// Readback transfer buffers (double-buffered)
	SDL_GPUTransferBufferCreateInfo rci = {};
	rci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	rci.size = 1920 * 1080 * 4;
	for (int i = 0; i < READBACK_SLOTS; i++)
		m_rbSlot[i].tb = SDL_CreateGPUTransferBuffer(m_device, &rci);

	// Store swapchain format for display pipeline
	m_swapFormat = SDL_GetGPUSwapchainTextureFormat(m_device, m_window);

	// Test texture creation with same usage as real textures
	SDL_GPUTextureCreateInfo testTi = {};
	testTi.type = SDL_GPU_TEXTURETYPE_2D;
	testTi.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	testTi.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
	testTi.width = 256; testTi.height = 256; testTi.layer_count_or_depth = 1;
	testTi.num_levels = 1; testTi.sample_count = SDL_GPU_SAMPLECOUNT_1;
	SDL_GPUTexture *testTex = SDL_CreateGPUTexture(m_device, &testTi);
	if (!testTex) {
		__android_log_print(ANDROID_LOG_ERROR, TAG,
			"GPU texture allocation test failed (256x256), GPU renderer unusable");
		return false;
	}
	SDL_ReleaseGPUTexture(m_device, testTex);

	__android_log_print(ANDROID_LOG_INFO, TAG, "GPU renderer initialized (swapfmt=0x%x)", (unsigned)m_swapFormat);
	return true;
}

void TVPRenderManager_GPU::Shutdown() {
	if (!m_device) return;
	SDL_WaitForGPUIdle(m_device);
	for (auto &kv : m_methodCache)
		delete kv.second;
	m_methodCache.clear();
	if (m_quadPipeline) SDL_ReleaseGPUGraphicsPipeline(m_device, m_quadPipeline);
	if (m_presentPipeline) SDL_ReleaseGPUGraphicsPipeline(m_device, m_presentPipeline);
	if (m_quadVerts) SDL_ReleaseGPUBuffer(m_device, m_quadVerts);
	if (m_vs) SDL_ReleaseGPUShader(m_device, m_vs);
	if (m_fs) SDL_ReleaseGPUShader(m_device, m_fs);
	if (m_fs_gray) SDL_ReleaseGPUShader(m_device, m_fs_gray);
	if (m_fs_blur) SDL_ReleaseGPUShader(m_device, m_fs_blur);
	if (m_fs_adjustGamma) SDL_ReleaseGPUShader(m_device, m_fs_adjustGamma);
	if (m_fs_univTrans) SDL_ReleaseGPUShader(m_device, m_fs_univTrans);
	if (m_fs_fill) SDL_ReleaseGPUShader(m_device, m_fs_fill);
	if (m_fs_present) SDL_ReleaseGPUShader(m_device, m_fs_present);
	if (m_fs_crossfade) SDL_ReleaseGPUShader(m_device, m_fs_crossfade);
	if (m_sampler) SDL_ReleaseGPUSampler(m_device, m_sampler);
	if (m_fallbackTex) SDL_ReleaseGPUTexture(m_device, m_fallbackTex);
	for (int i = 0; i < READBACK_SLOTS; i++) {
		if (m_rbSlot[i].fence) {
			SDL_GPUFence *fencePtr = m_rbSlot[i].fence;
			SDL_WaitForGPUFences(m_device, true, &fencePtr, 1);
			SDL_ReleaseGPUFence(m_device, m_rbSlot[i].fence);
		}
		if (m_rbSlot[i].tb) SDL_ReleaseGPUTransferBuffer(m_device, m_rbSlot[i].tb);
	}
	SDL_DestroyGPUDevice(m_device);
	m_device = nullptr; m_window = nullptr;
}

SDL_GPUGraphicsPipeline* TVPRenderManager_GPU::_CreateQuadPipeline(
	SDL_GPUTextureFormat format,
	SDL_GPUBlendFactor srcColor, SDL_GPUBlendFactor dstColor,
	SDL_GPUBlendFactor srcAlpha, SDL_GPUBlendFactor dstAlpha,
	SDL_GPUBlendOp colorOp, SDL_GPUBlendOp alphaOp,
	bool blendEnabled,
	SDL_GPUShader *fragShader,
	int numSamplers, int numUBO) {
	if (!fragShader) fragShader = m_fs;
	if (numSamplers <= 0) numSamplers = 1;
	SDL_GPUGraphicsPipelineCreateInfo pi = {};
	pi.vertex_shader = m_vs;
	pi.fragment_shader = fragShader;

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
	ct.format = format;
	ct.blend_state.enable_blend = blendEnabled;
	ct.blend_state.src_color_blendfactor = srcColor;
	ct.blend_state.dst_color_blendfactor = dstColor;
	ct.blend_state.color_blend_op = colorOp;
	ct.blend_state.src_alpha_blendfactor = srcAlpha;
	ct.blend_state.dst_alpha_blendfactor = dstAlpha;
	ct.blend_state.alpha_blend_op = alphaOp;
	ct.blend_state.enable_color_write_mask = false;

	pi.target_info.num_color_targets = 1;
	pi.target_info.color_target_descriptions = &ct;
	pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
	pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
	pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;

	return SDL_CreateGPUGraphicsPipeline(m_device, &pi);
}

//------------------------------------------------------------------------------
// Render method cache — map method names to blend configuration
//------------------------------------------------------------------------------
struct MethodBlendConfig {
	bool enable;
	SDL_GPUBlendFactor srcC, dstC, srcA, dstA;
	SDL_GPUBlendOp cOp, aOp;
};

static MethodBlendConfig _getMethodBlend(const char *name) {
	MethodBlendConfig c = {};
	c.enable = false;
	c.srcC = c.dstC = c.srcA = c.dstA = SDL_GPU_BLENDFACTOR_ONE;
	c.cOp = c.aOp = SDL_GPU_BLENDOP_ADD;

	if (!name) return c;

	// No-blend / copy methods
	if (!strcmp(name, "Copy") || !strcmp(name, "CopyColor")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ONE;
		c.dstC = SDL_GPU_BLENDFACTOR_ZERO;
		c.srcA = SDL_GPU_BLENDFACTOR_ONE;
		c.dstA = SDL_GPU_BLENDFACTOR_ZERO;
	}
	else if (!strcmp(name, "CopyMask")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE;
		c.srcA = SDL_GPU_BLENDFACTOR_ONE;
		c.dstA = SDL_GPU_BLENDFACTOR_ZERO;
	}
	else if (!strcmp(name, "CopyOpaqueImage")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ONE;
		c.dstC = SDL_GPU_BLENDFACTOR_ZERO;
		c.srcA = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE;
	}
	else if (!strcmp(name, "FillMask")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE;
		c.srcA = SDL_GPU_BLENDFACTOR_ONE;
		c.dstA = SDL_GPU_BLENDFACTOR_ZERO;
	}
	else if (!strcmp(name, "RemoveConstOpacity") || !strcmp(name, "RemoveOpacity")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE;
		c.srcA = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	}
	else if (!strcmp(name, "FillColor")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ONE;
		c.dstC = SDL_GPU_BLENDFACTOR_ZERO;
		c.srcA = SDL_GPU_BLENDFACTOR_ONE;
		c.dstA = SDL_GPU_BLENDFACTOR_ZERO;
	}
	else if (!strcmp(name, "AlphaBlend") || !strcmp(name, "AlphaBlend_color")
		|| !strcmp(name, "PsAlphaBlend") || !strcmp(name, "AddBlend_blend")
		|| !strcmp(name, "PerspectiveAlphaBlend_a")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		c.srcA = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE;
	}
	else if (!strcmp(name, "AlphaBlend_a") || !strcmp(name, "AlphaBlend_color_AlphaTest")
		|| !strcmp(name, "AlphaTest")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
		c.srcA = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
	}
	else if (!strcmp(name, "AdditiveAlphaBlend") || !strcmp(name, "AddBlend")
		|| !strcmp(name, "ScreenBlend") || !strcmp(name, "PsAddBlend")
		|| !strcmp(name, "PsScreenBlend")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ONE;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE;
		c.srcA = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE;
	}
	else if (!strcmp(name, "AdditiveAlphaBlend_a")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ONE;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		c.srcA = SDL_GPU_BLENDFACTOR_ONE;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	}
	else if (!strcmp(name, "SubBlend") || !strcmp(name, "PsSubBlend")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ONE;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE;
		c.cOp = SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
		c.aOp = SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
		c.srcA = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE;
	}
	else if (!strcmp(name, "MulBlend") || !strcmp(name, "MulBlend_HDA")
		|| !strcmp(name, "PsMulBlend")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstC = SDL_GPU_BLENDFACTOR_SRC_COLOR;
		c.srcA = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE;
	}
	else if (!strcmp(name, "ColorDodgeBlend") || !strcmp(name, "PsColorDodgeBlend")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstC = SDL_GPU_BLENDFACTOR_SRC_COLOR;
		c.srcA = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE;
	}
	else if (!strcmp(name, "FillARGB")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ONE;
		c.dstC = SDL_GPU_BLENDFACTOR_ZERO;
		c.srcA = SDL_GPU_BLENDFACTOR_ONE;
		c.dstA = SDL_GPU_BLENDFACTOR_ZERO;
	}
	else if (!strcmp(name, "AlphaBlend_SD")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
		c.srcA = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
	}
	else if (!strcmp(name, "AdditiveAlphaToAlpha")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ONE;
		c.dstC = SDL_GPU_BLENDFACTOR_ZERO;
		c.srcA = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstA = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
	}
	else if (!strcmp(name, "AlphaToAdditiveAlpha")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ONE;
		c.dstC = SDL_GPU_BLENDFACTOR_ZERO;
		c.srcA = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		c.dstA = SDL_GPU_BLENDFACTOR_ZERO;
	}
	else if (!strcmp(name, "DarkenBlend") || !strcmp(name, "PsDarkenBlend")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ONE;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE;
		c.cOp = SDL_GPU_BLENDOP_MIN;
		c.aOp = SDL_GPU_BLENDOP_MIN;
		c.srcA = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE;
	}
	else if (!strcmp(name, "LightenBlend") || !strcmp(name, "PsLightenBlend")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ONE;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE;
		c.cOp = SDL_GPU_BLENDOP_MAX;
		c.aOp = SDL_GPU_BLENDOP_MAX;
		c.srcA = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE;
	}
	// PsOverlayBlend, PsHardLightBlend, PsSoftLightBlend: use Screen blend approx
	else if (!strcmp(name, "PsOverlayBlend") || !strcmp(name, "PsHardLightBlend")
		|| !strcmp(name, "PsSoftLightBlend")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ONE;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
		c.srcA = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE;
	}
	// PsDiffBlend, PsExclusionBlend: use subtract approx
	else if (!strcmp(name, "PsDiffBlend") || !strcmp(name, "PsExclusionBlend")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ONE;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE;
		c.cOp = SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
		c.aOp = SDL_GPU_BLENDOP_REVERSE_SUBTRACT;
		c.srcA = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE;
	}
	// PsColorBurnBlend: use multiply approx
	else if (!strcmp(name, "PsColorBurnBlend")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstC = SDL_GPU_BLENDFACTOR_SRC_COLOR;
		c.srcA = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE;
	}
	// Constant-alpha variants (use CONSTANT_COLOR for both RGB and alpha)
	else if (strstr(name, "ConstAlphaBlend") || strstr(name, "ConstColorAlphaBlend")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
		c.srcA = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
	}
	// ApplyColorMap variants
	else if (strstr(name, "ApplyColorMap")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		c.srcA = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE;
	}
	// Custom-shader methods: use Copy (no blend) — shader handles compositing internally
	else if (strstr(name, "AdjustGamma") || strstr(name, "UnivTrans")) {
		// Copy mode
	}
	// _d variants: alpha blending with destination alpha preservation
	// (SRC_ALPHA/ONE_MINUS_SRC_ALPHA for both RGB and A)
	else if (strstr(name, "_d") && !strstr(name, "SD")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		c.srcA = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	}
	// _a variants: additive alpha blending (SRC_ALPHA/ONE)
	else if (strstr(name, "_a") && !strstr(name, "SD")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE;
		c.srcA = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE;
	}
	// Complex shader effects: DoGrayScale, BoxBlur, AdjustGamma, UnivTransBlend, PsOverlay, etc.
	// These use custom shaders with no blend (Copy-like)
	else {
		// Use Copy mode (no blending) — shader handles its own compositing
		c.enable = false;
	}
	return c;
}

tTVPGPURenderMethod* TVPRenderManager_GPU::_GetOrCreateMethod(const char *name) {
	// Hash-based cache key
	uint32_t h = 0;
	for (const char *p = name; *p; p++) h = h * 33 + (unsigned char)*p;
	auto it = m_methodCache.find(h);
	if (it != m_methodCache.end()) return it->second;

	MethodBlendConfig cfg = _getMethodBlend(name);
	SDL_GPUShader *customFS = nullptr;
	int customSamplers = 0, customUBO = 0;
	bool customShader = false;
	if (!strcmp(name, "DoGrayScale")) {
		customFS = m_fs_gray; customShader = true;
	} else if (!strcmp(name, "BoxBlur") || !strcmp(name, "BoxBlurAlpha")) {
		customFS = m_fs_blur; customShader = true;
	} else if (strstr(name, "AdjustGamma")) {
		customFS = m_fs_adjustGamma; customShader = true; customUBO = 1;
	} else if (strstr(name, "UnivTrans")) {
		customFS = m_fs_univTrans; customShader = true; customSamplers = 3; customUBO = 1;
	} else if (!strcmp(name, "FillARGB") || !strcmp(name, "FillColor") || !strcmp(name, "FillMask")) {
		customFS = m_fs_fill; customShader = true; customSamplers = 0;
	} else if (strstr(name, "ConstAlphaBlend_SD") || strstr(name, "ConstColorAlphaBlend_SD")) {
		// Two-source crossfade: shader samples tex0+tex1, Copy blend
		customFS = m_fs_crossfade; customShader = true; customSamplers = 2;
		cfg.enable = false;
	}
	SDL_GPUGraphicsPipeline *pipe = _CreateQuadPipeline(m_texFormat,
		cfg.srcC, cfg.dstC, cfg.srcA, cfg.dstA,
		cfg.cOp, cfg.aOp, cfg.enable,
		customFS, customSamplers, customUBO);
	if (!pipe) return nullptr;

	auto *m = new tTVPGPURenderMethod(m_device, pipe, m_vs, m_fs,
		name, cfg.enable, customSamplers > 0 ? customSamplers : 1,
		cfg.srcC, cfg.dstC, cfg.srcA, cfg.dstA,
		cfg.cOp, cfg.aOp);
	// Pre-assign UBO parameter IDs for UnivTrans
	if (strstr(name, "UnivTrans")) {
		m->m_uboID_Vague = m->EnumParameterID("vague");
		m->m_uboID_Phase = m->EnumParameterID("phase");
	}
	m_methodCache[h] = m;
	__android_log_print(ANDROID_LOG_INFO, TAG,
		"Created GPU method: %s (blend=%d sampler=%d)", name, (int)cfg.enable, customSamplers);
	return m;
}

//------------------------------------------------------------------------------
// iTVPRenderManager — GetRenderMethod
//------------------------------------------------------------------------------
iTVPRenderMethod* TVPRenderManager_GPU::GetRenderMethod(const char *name,
	uint32_t *hint) {
	auto *m = _GetOrCreateMethod(name);
	if (!m) {
		__android_log_print(ANDROID_LOG_WARN, TAG,
			"GetRenderMethod(%s) failed, falling back to Copy", name);
		m = _GetOrCreateMethod("Copy");
	}
	if (hint) *hint = 0;
	return m;
}

//------------------------------------------------------------------------------
// Texture creation
//------------------------------------------------------------------------------
iTVPTexture2D* TVPRenderManager_GPU::CreateTexture2D(const void *pixel,
	int pitch, unsigned int w, unsigned int h,
	TVPTextureFormat::e format, int flags) {
	if (!m_device || w == 0 || h == 0) return nullptr;

	// Clamp to sensible max size
	if (w > 4096) w = 4096;
	if (h > 4096) h = 4096;

	SDL_GPUTextureCreateInfo ti = {};
	ti.type = SDL_GPU_TEXTURETYPE_2D;
	ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	ti.usage = (SDL_GPU_TEXTUREUSAGE_SAMPLER |
		SDL_GPU_TEXTUREUSAGE_COLOR_TARGET);
	ti.width = w; ti.height = h; ti.layer_count_or_depth = 1;
	ti.num_levels = 1; ti.sample_count = SDL_GPU_SAMPLECOUNT_1;

	SDL_GPUTexture *tex = SDL_CreateGPUTexture(m_device, &ti);
	if (!tex) {
		__android_log_print(ANDROID_LOG_WARN, TAG,
			"SDL_CreateGPUTexture(%ux%u) failed, returning null", w, h);
		return nullptr;
	}

	// One-time clear: fill texture with transparent black on creation
	// so LOADOP_LOAD in SetRenderTarget preserves content across frames.
	if (!pixel) {
		SDL_GPUCommandBuffer *initCmd = SDL_AcquireGPUCommandBuffer(m_device);
		if (initCmd) {
			SDL_GPUColorTargetInfo initTi = {};
			initTi.texture = tex;
			initTi.load_op = SDL_GPU_LOADOP_CLEAR;
			initTi.store_op = SDL_GPU_STOREOP_STORE;
			initTi.clear_color = (SDL_FColor){0.0f, 0.0f, 0.0f, 0.0f};
			SDL_GPURenderPass *initRp = SDL_BeginGPURenderPass(initCmd, &initTi, 1, NULL);
			if (initRp) SDL_EndGPURenderPass(initRp);
			SDL_SubmitGPUCommandBuffer(initCmd);
		}
	}

	auto *ret = new tTVPGPUTexture2D(m_device, tex, (int)w, (int)h,
		(int)w, (int)h, format, false);
	s_totalVMem += (uint64_t)w * h * 4;

	if (pixel) {
		ret->Update(pixel, format, pitch,
			tTVPRect(0, 0, (tjs_int)w, (tjs_int)h));
	}
	return ret;
}

iTVPTexture2D* TVPRenderManager_GPU::CreateTexture2D(tTVPBitmap* bmp) {
	if (!bmp) return nullptr;
	int bpp = bmp->GetBPP();
	TVPTextureFormat::e fmt = (bpp == 8)
		? TVPTextureFormat::Gray : TVPTextureFormat::RGBA;
	iTVPTexture2D *tex = CreateTexture2D(bmp->GetScanLine(0),
		bmp->GetPitch(), bmp->GetWidth(), bmp->GetHeight(), fmt);
	return tex;
}

iTVPTexture2D* TVPRenderManager_GPU::CreateTexture2D(TJS::tTJSBinaryStream* s) {
	// FIXME: load from stream via image decoder
	return nullptr;
}

iTVPTexture2D* TVPRenderManager_GPU::CreateTexture2D(unsigned int neww,
	unsigned int newh, iTVPTexture2D* tex) {
	if (!tex) return nullptr;
	// Create new texture with desired size; copy content by re-uploading
	iTVPTexture2D *ret = CreateTexture2D(nullptr, 0, neww, newh,
		tex->GetFormat());
	return ret;
}

//------------------------------------------------------------------------------
// iTVPRenderManager — SetRenderTarget
//------------------------------------------------------------------------------
void TVPRenderManager_GPU::SetRenderTarget(iTVPTexture2D *target) {
	if (!m_cmd) return;

	// End previous pass unconditionally before switching
	if (m_currentPass) {
		SDL_EndGPURenderPass(m_currentPass);
		m_currentPass = nullptr;
	}
	m_currentTarget = target;
	if (target) {
		static int s_logTargets = 20;
		if (s_logTargets > 0) {
			s_logTargets--;
			tTVPGPUTexture2D *gpuTex = dynamic_cast<tTVPGPUTexture2D*>(target);
			__android_log_print(ANDROID_LOG_INFO, "##krkr", "SETRT: tar=%p isGPU=%d w=%d h=%d",
				target, !!gpuTex,
				target ? target->GetWidth() : 0,
				target ? target->GetHeight() : 0);
		}
	}

	if (!target) return;

	tTVPGPUTexture2D *gpuTex = dynamic_cast<tTVPGPUTexture2D*>(target);
	if (!gpuTex || !gpuTex->GetGPUTexture()) return;

	SDL_GPUColorTargetInfo tg = {};
	tg.texture = gpuTex->GetGPUTexture();
	tg.load_op = SDL_GPU_LOADOP_LOAD;
	tg.store_op = SDL_GPU_STOREOP_STORE;
	SDL_GPUColorTargetInfo targets[1] = { tg };
	m_currentPass = SDL_BeginGPURenderPass(m_cmd, targets, 1, NULL);
	if (!m_currentPass) {
		__android_log_print(ANDROID_LOG_ERROR, "##krkr", "[DIAG] SDL_BeginGPURenderPass FAILED — tar=%p", target);
	}
}

//------------------------------------------------------------------------------
// iTVPRenderManager — OperateRect
//------------------------------------------------------------------------------
void TVPRenderManager_GPU::OperateRect(iTVPRenderMethod* method,
	iTVPTexture2D *tar, iTVPTexture2D *reftar,
	const tTVPRect& rctar, const tRenderTexRectArray &textures) {
	if (!m_device || !method) return;

	tTVPGPURenderMethod *gpuMethod = dynamic_cast<tTVPGPURenderMethod*>(method);
	if (!gpuMethod || !gpuMethod->GetPipeline()) {
		__android_log_print(ANDROID_LOG_INFO, "##krkr", "[DIAG] OperateRect: skip — method=%p isGpu=%d pipe=%p", method, !!gpuMethod, gpuMethod ? gpuMethod->GetPipeline() : 0);
		return;
	}

	// Log first 120 frames; then only AlphaBlend/ConstAlpha/Copy
	const std::string &mname = gpuMethod->GetName();
	static int s_oprFrame = 0;
	s_oprFrame++;
	bool logOPR = s_oprFrame <= 30 || 
		(mname.find("Alpha") != std::string::npos) || 
		(mname.find("ConstAlpha") != std::string::npos) ||
		(mname.find("Copy") != std::string::npos) ||
		(mname.find("Fill") != std::string::npos);
	if (logOPR && tar)
	{
		int tw = tar->GetWidth(), th = tar->GetHeight();
		auto *gpuDst = dynamic_cast<tTVPGPUTexture2D*>(tar);
		bool dstIsGPU = (gpuDst && gpuDst->GetGPUTexture());
		void *srcTex0 = nullptr;
		tTVPGPUTexture2D *gpuSrc = nullptr;
		if (textures.size() > 0 && textures[0].first) {
			srcTex0 = textures[0].first;
			gpuSrc = dynamic_cast<tTVPGPUTexture2D*>(textures[0].first);
		}
		bool srcIsGPU = (gpuSrc && gpuSrc->GetGPUTexture());
		__android_log_print(ANDROID_LOG_INFO, "##krkr", "OPR[%d]: %s nTex=%d opa=%d tar=%p(%d,%d) dstGPU=%d src=%p srcGPU=%d",
			s_oprFrame, mname.c_str(), (int)textures.size(),
			gpuMethod->m_opacity, tar, tw, th, (int)dstIsGPU, srcTex0, (int)srcIsGPU);
	}



	// Ensure render pass on target
	SetRenderTarget(tar);
	if (!m_currentPass) {
		__android_log_print(ANDROID_LOG_INFO, "##krkr", "[DIAG] OperateRect: no render pass after SetRenderTarget — tar=%p", tar);
		return;
	}

	// Set viewport to destination rect
	{
		(void)0; // VIEWPORT log removed
	}
	SDL_GPUViewport vp = {
		(float)rctar.left, (float)rctar.top,
		(float)(rctar.get_width()), (float)(rctar.get_height()),
		0.0f, 1.0f
	};
	SDL_SetGPUViewport(m_currentPass, &vp);

	// Bind pipeline
	SDL_BindGPUGraphicsPipeline(m_currentPass, gpuMethod->GetPipeline());

	// Bind vertex buffer
	SDL_GPUBufferBinding bb = { m_quadVerts, 0 };
	SDL_BindGPUVertexBuffers(m_currentPass, 0, &bb, 1);

	// Build UBO data
	uint8_t uboPush[256] = {};

	// Determine if hardware blend constants should be set (CONSTANT_COLOR blend)
	bool useBlendConstants = gpuMethod->m_blendEnabled &&
		(gpuMethod->m_srcColor == SDL_GPU_BLENDFACTOR_CONSTANT_COLOR ||
		 gpuMethod->m_dstColor == SDL_GPU_BLENDFACTOR_CONSTANT_COLOR);

	// Fill methods: push fill color (no UV/opacity)
	if (textures.size() == 0 && gpuMethod->m_hasConstantColor) {
		float *col = (float*)uboPush;
		col[0] = gpuMethod->GetConstColor(0);
		col[1] = gpuMethod->GetConstColor(1);
		col[2] = gpuMethod->GetConstColor(2);
		col[3] = gpuMethod->GetConstColor(3);
		SDL_PushGPUFragmentUniformData(m_cmd, 0, uboPush, 16);
	} else {
	// Standard UBO: [uvOffset(8)][uvScale(8)][opacity(4)][pad(4)][methodData(0-240)]
	float *uvF = (float*)uboPush;
	uvF[0] = 0.0f; uvF[1] = 0.0f; uvF[2] = 1.0f; uvF[3] = 1.0f;
	// For CONSTANT_COLOR blend: shader outputs unmodified alpha (opacity=1.0),
	// and SDL_SetGPUBlendConstants handles the compositing.
	uvF[4] = useBlendConstants ? 1.0f : (float)gpuMethod->m_opacity / 255.0f;
	uvF[5] = 0.0f; // padding
	int uboSize = 24; // 6 floats = 24 bytes for UV + opacity
	// Adjust UV from first source texture's rect
	if (textures.size() > 0) {
		auto *srcTex = dynamic_cast<tTVPGPUTexture2D*>(textures[0].first);
		if (srcTex) {
			const tTVPRect &sr = textures[0].second;
			float tw = (float)srcTex->GetWidth();
			float th = (float)srcTex->GetHeight();
			if (tw > 0 && th > 0) {
				uvF[0] = (float)sr.left / tw;          // uvOffset.x
				uvF[1] = (float)sr.top / th;            // uvOffset.y
				uvF[2] = (float)sr.get_width() / tw;    // uvScale.x
				uvF[3] = (float)sr.get_height() / th;   // uvScale.y
				(void)0; // UV_CLIP log removed
	if (srcTex && srcTex->GetGPUTexture() == nullptr) {
		__android_log_print(ANDROID_LOG_WARN, "##krkr", "OPR_BUG: srcTex GPU texture is NULL! tex=%p w=%d h=%d", srcTex, srcTex->GetWidth(), srcTex->GetHeight());
	}
			}
		}
	}
	// Append method-specific UBO data (AdjustGamma, UnivTrans)
	if (gpuMethod->m_uboSize > 0) {
		memcpy(uboPush + 16, gpuMethod->m_uboData, gpuMethod->m_uboSize);
		uboSize += gpuMethod->m_uboSize;
		gpuMethod->m_uboDirty = false;
	}
	SDL_PushGPUFragmentUniformData(m_cmd, 0, uboPush, (Uint32)uboSize);
	}

	// Bind source textures (up to 8)
	SDL_GPUTextureSamplerBinding tsBindings[8] = {};
	int nBind = (int)textures.size();
	if (nBind > 8) nBind = 8;
	for (int i = 0; i < nBind; i++) {
		auto *srcTex = dynamic_cast<tTVPGPUTexture2D*>(textures[i].first);
		if (srcTex && srcTex->GetGPUTexture()) {
			tsBindings[i].texture = srcTex->GetGPUTexture();
			tsBindings[i].sampler = m_sampler;
		}
	}
	if (nBind > 0)
		SDL_BindGPUFragmentSamplers(m_currentPass, 0, tsBindings, nBind);

	// Set blend constants for CONSTANT_COLOR blend modes
	if (useBlendConstants) {
		float a = (float)gpuMethod->m_opacity / 255.0f;
		SDL_FColor bc = { a, a, a, a };
		SDL_SetGPUBlendConstants(m_currentPass, bc);
	}

	// Draw
	SDL_DrawGPUPrimitives(m_currentPass, 6, 1, 0, 0);
	m_drawCount++;

	// Debug capture: dump ALL source textures (static content) + target.
	// Source textures are fully loaded before rendering; target shows
	// accumulated state from previous operations (current draw not visible
	// yet due to GPU pipeline buffering).
	if (s_captureMode) {
		// Dump source textures
		for (size_t si = 0; si < textures.size(); si++) {
			auto *srcTex = dynamic_cast<tTVPGPUTexture2D*>(textures[si].first);
			if (srcTex && srcTex->GetGPUTexture() && srcTex->GetWidth() > 0 && srcTex->GetHeight() <= 4096) {
				char lbl[64];
				snprintf(lbl, sizeof(lbl), "src%d_%s_%p", (int)si,
					gpuMethod->GetName().c_str(), (void*)textures[si].first);
				DumpTextureToFile(lbl, srcTex->GetGPUTexture(),
					srcTex->GetWidth(), srcTex->GetHeight());
			}
		}
		// Dump target (accumulated state from previous operations this frame)
		if (tar) {
			auto *capTex = dynamic_cast<tTVPGPUTexture2D*>(tar);
			if (capTex && capTex->GetGPUTexture() && capTex->GetWidth() <= 4096) {
				char lbl[64];
				snprintf(lbl, sizeof(lbl), "dst_%s_%p",
					gpuMethod->GetName().c_str(), (void*)tar);
				DumpTextureToFile(lbl, capTex->GetGPUTexture(),
					capTex->GetWidth(), capTex->GetHeight());
			}
		}
	}
}

//------------------------------------------------------------------------------
// iTVPRenderManager — OperateTriangles (stub)
//------------------------------------------------------------------------------
void TVPRenderManager_GPU::OperateTriangles(iTVPRenderMethod* method,
	int nTriangles, iTVPTexture2D *target, iTVPTexture2D *reftar,
	const tTVPRect& rcclip, const tTVPPointD* pttar,
	const tRenderTexQuadArray &textures) {
	// FIXME: proper triangle rendering
}

//------------------------------------------------------------------------------
// iTVPRenderManager — OperatePerspective (stub)
//------------------------------------------------------------------------------
void TVPRenderManager_GPU::OperatePerspective(iTVPRenderMethod* method,
	int nQuads, iTVPTexture2D *target, iTVPTexture2D *reftar,
	const tTVPRect& rcclip, const tTVPPointD* pttar,
	const tRenderTexQuadArray &textures) {
	// FIXME: proper perspective rendering
}

//------------------------------------------------------------------------------
// iTVPRenderManager — GetRenderStat
//------------------------------------------------------------------------------
bool TVPRenderManager_GPU::GetRenderStat(unsigned int &drawCount,
	uint64_t &vmemsize) {
	drawCount = m_drawCount; m_drawCount = 0; vmemsize = s_totalVMem.load();
	return true;
}

//------------------------------------------------------------------------------
// Frame lifecycle — public
//------------------------------------------------------------------------------
void TVPRenderManager_GPU::FlushPass() {
	_EndFramePass();
}

void TVPRenderManager_GPU::BeginFrame() {
	_BeginFrame();
}

void TVPRenderManager_GPU::EndFrame() {
	if (!m_cmd) return;

	// ---- Step 1: Process previous readback (fence is done) ----
	int prevSlot = m_rbActive ^ 1;
	bool rbReady = false;
	if (m_rbSlot[prevSlot].fence) {
		// Wait for previous frame's readback to complete
		SDL_GPUFence *fencePtr = m_rbSlot[prevSlot].fence;
		SDL_WaitForGPUFences(m_device, true, &fencePtr, 1);
		// SDL_WaitForGPUFences does NOT destroy or null the pointer,
		// so we release it here and mark readback as ready.
		SDL_ReleaseGPUFence(m_device, m_rbSlot[prevSlot].fence);
		m_rbSlot[prevSlot].fence = nullptr;
		rbReady = true;
	}
	if (rbReady && m_rbSlot[prevSlot].tb && m_rbSlot[prevSlot].texW > 0) {
		// Fence is signaled — map and copy
		void *map = SDL_MapGPUTransferBuffer(m_device,
			m_rbSlot[prevSlot].tb, true);
		if (map) {
			int tw = m_rbSlot[prevSlot].texW;
			int th = m_rbSlot[prevSlot].texH;
			m_frameW = tw; m_frameH = th;
			m_framePixels.resize(tw * th * 4);
			memcpy(m_framePixels.data(), map, (size_t)(tw * th * 4));
			m_hasFrameResult = true;
			SDL_UnmapGPUTransferBuffer(m_device, m_rbSlot[prevSlot].tb);
		}
		m_rbSlot[prevSlot].texW = 0;
	}
	// Release old fence (was waited or timed out)
	if (m_rbSlot[prevSlot].fence) {
		SDL_ReleaseGPUFence(m_device, m_rbSlot[prevSlot].fence);
		m_rbSlot[prevSlot].fence = nullptr;
	}

	// ---- Step 2: End compositing render pass ----
	_EndFramePass();

	// ---- Step 3: Issue readback for current frame ----
	int slot = m_rbActive;
	m_rbSlot[slot].texW = 0; m_rbSlot[slot].texH = 0;
	iTVPTexture2D *readbackTex = m_currentTarget;
	if (readbackTex) {
		tTVPGPUTexture2D *gpuTex = dynamic_cast<tTVPGPUTexture2D*>(readbackTex);
		if (gpuTex && gpuTex->GetGPUTexture()) {
			Uint32 tw = (Uint32)gpuTex->GetWidth();
			Uint32 th = (Uint32)gpuTex->GetHeight();
			if (tw > 0 && th > 0 && tw <= 1920 && th <= 1080) {
				m_rbSlot[slot].texW = (int)tw;
				m_rbSlot[slot].texH = (int)th;
				SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(m_cmd);
				if (cp) {
					SDL_GPUTextureRegion srcReg = {};
					srcReg.texture = gpuTex->GetGPUTexture();
					srcReg.w = tw; srcReg.h = th; srcReg.d = 1;
					SDL_GPUTextureTransferInfo dstTI = { m_rbSlot[slot].tb, 0 };
					SDL_DownloadFromGPUTexture(cp, &srcReg, &dstTI);
					SDL_EndGPUCopyPass(cp);
				}
			}
		}
	}

	// ---- Step 4: Present to swapchain ----
	if (m_swapchainTex) {
		SDL_GPUColorTargetInfo tg = {};
		tg.texture = m_swapchainTex;
		tg.load_op = SDL_GPU_LOADOP_CLEAR;  // clear each frame to prevent swapchain flicker
		tg.store_op = SDL_GPU_STOREOP_STORE;
		tg.clear_color = (SDL_FColor){0.0f, 0.0f, 0.0f, 1.0f};
		SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(m_cmd, &tg, 1, NULL);
		if (rp) {
			if (!m_presentPipeline) {
				m_presentPipeline = _CreateQuadPipeline(m_swapFormat,
					SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_ZERO,
					SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_ZERO,
					SDL_GPU_BLENDOP_ADD, SDL_GPU_BLENDOP_ADD, false,
					m_fs_present, 1, 0);
				if (!m_presentPipeline)
					__android_log_print(ANDROID_LOG_ERROR, "##krkr", "FAILED to create swapchain present pipeline");
				else
					__android_log_print(ANDROID_LOG_INFO, "##krkr", "Present pipeline created");
			}
			// Determine which texture to present: GPU render target or fallback from CPU
			SDL_GPUTexture *presentTex = nullptr;
			float gameW = 0, gameH = 0;
			if (readbackTex) {
				tTVPGPUTexture2D *gpuTex = dynamic_cast<tTVPGPUTexture2D*>(readbackTex);
				if (gpuTex && gpuTex->GetGPUTexture() &&
					m_swapchainTex != gpuTex->GetGPUTexture()) {
					presentTex = gpuTex->GetGPUTexture();
					gameW = (float)gpuTex->GetWidth();
					gameH = (float)gpuTex->GetHeight();
				}
			}
			if (!presentTex && m_fallbackTex && m_fallbackW > 0) {
				presentTex = m_fallbackTex;
				gameW = (float)m_fallbackW;
				gameH = (float)m_fallbackH;
			}
			if (m_presentPipeline && presentTex) {
				float vpX = 0, vpY = 0, vpW = (float)m_swW, vpH = (float)m_swH;
				extern bool g_fullscreenStretch;
				if (!g_fullscreenStretch && gameW > 0 && gameH > 0) {
					float scale = fminf(vpW / gameW, vpH / gameH);
					vpW = gameW * scale; vpH = gameH * scale;
					vpX = (m_swW - vpW) * 0.5f; vpY = (m_swH - vpH) * 0.5f;
				}
				SDL_GPUViewport vp = {vpX, vpY, vpW, vpH, 0, 1};
				SDL_SetGPUViewport(rp, &vp);
				SDL_BindGPUGraphicsPipeline(rp, m_presentPipeline);
				SDL_GPUBufferBinding bb = { m_quadVerts, 0 };
				SDL_BindGPUVertexBuffers(rp, 0, &bb, 1);
				SDL_GPUTextureSamplerBinding ts = { presentTex, m_sampler };
				SDL_BindGPUFragmentSamplers(rp, 0, &ts, 1);
				SDL_DrawGPUPrimitives(rp, 6, 1, 0, 0);
			}
			SDL_EndGPURenderPass(rp);
		}
	}

	// ---- Step 5: Submit with fence (no blocking) ----
	m_rbSlot[slot].fence = SDL_SubmitGPUCommandBufferAndAcquireFence(m_cmd);
	m_cmd = nullptr;
	m_currentPass = nullptr;
	m_swapchainTex = nullptr;
	m_rbActive ^= 1;
}

//------------------------------------------------------------------------------
// Frame lifecycle — internal
//------------------------------------------------------------------------------
void TVPRenderManager_GPU::_BeginFrame() {
	if (m_cmd) return;
	m_cmd = SDL_AcquireGPUCommandBuffer(m_device);
	if (!m_cmd) return;

	// Acquire swapchain texture
	SDL_WaitAndAcquireGPUSwapchainTexture(m_cmd, m_window,
		&m_swapchainTex, &m_swW, &m_swH);
	// swapchainTex may be null if window minimized
	m_currentPass = nullptr;
	// Don't clear m_currentTarget — keep last frame's composited texture for idle frames
	m_frameFirstTarget = true;
}

void TVPRenderManager_GPU::_EndFramePass() {
	if (m_currentPass) {
		SDL_EndGPURenderPass(m_currentPass);
		m_currentPass = nullptr;
	}
}

void TVPRenderManager_GPU::_PresentToSwapchain() {
	if (!m_cmd || !m_swapchainTex) return;
	_EndFramePass();

	// Start render pass on swapchain
	SDL_GPUColorTargetInfo tg = {};
	tg.texture = m_swapchainTex;
	tg.load_op = SDL_GPU_LOADOP_CLEAR;
	tg.store_op = SDL_GPU_STOREOP_STORE;
	tg.clear_color = (SDL_FColor){0.0f, 0.3f, 0.6f, 1.0f};
	SDL_GPUColorTargetInfo targets[1] = { tg };
	SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(m_cmd, targets, 1, NULL);
	if (!rp) return;

	// Create/noop pipeline if needed
	if (!m_quadPipeline) {
		m_quadPipeline = _CreateQuadPipeline(m_swapFormat,
			SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_ZERO,
			SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_ZERO,
			SDL_GPU_BLENDOP_ADD, SDL_GPU_BLENDOP_ADD, false);
	}
	SDL_EndGPURenderPass(rp);
}

void TVPRenderManager_GPU::SetFallbackDisplay(const void *pixels, int w, int h) {
	if (!m_device || !m_cmd || !pixels || w <= 0 || h <= 0) return;
	// Always create new texture (avoids layout transition issues across frames)
	if (m_fallbackTex) {
		SDL_ReleaseGPUTexture(m_device, m_fallbackTex);
		m_fallbackTex = nullptr;
	}
	SDL_GPUTextureCreateInfo ti = {};
	ti.type = SDL_GPU_TEXTURETYPE_2D;
	ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
	ti.width = (Uint32)w; ti.height = (Uint32)h;
	ti.layer_count_or_depth = 1; ti.num_levels = 1;
	ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
	m_fallbackTex = SDL_CreateGPUTexture(m_device, &ti);
	if (!m_fallbackTex) return;
	m_fallbackW = w; m_fallbackH = h;
	SDL_GPUTransferBufferCreateInfo tci = {};
	tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
	tci.size = (Uint32)(w * h * 4);
	SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(m_device, &tci);
	if (!tb) return;
	void *map = SDL_MapGPUTransferBuffer(m_device, tb, false);
	if (map) memcpy(map, pixels, (size_t)(w * h * 4));
	SDL_UnmapGPUTransferBuffer(m_device, tb);
	_EndFramePass();
	SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(m_cmd);
	if (cp) {
		SDL_GPUTextureTransferInfo srcTI = { tb, 0 };
		SDL_GPUTextureRegion dstReg = {};
		dstReg.texture = m_fallbackTex;
		dstReg.w = (Uint32)w; dstReg.h = (Uint32)h; dstReg.d = 1;
		SDL_UploadToGPUTexture(cp, &srcTI, &dstReg, false);
		SDL_EndGPUCopyPass(cp);
	}
	SDL_ReleaseGPUTransferBuffer(m_device, tb);
}

void TVPRenderManager_GPU::ReadbackAndPresent(iTVPTexture2D *finalTex) {
	if (!m_device) return;
	if (finalTex) m_currentTarget = finalTex;
	BeginFrame();
	EndFrame();
}

//------------------------------------------------------------------------------
// ReadbackPixel — synchronous GPU pixel readback for diagnostics
// Uses a separate command buffer to avoid interfering with main rendering.
//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
// Debug capture — dump full GPU texture to BMP file
//------------------------------------------------------------------------------

static int s_captureSeq = 0;
void TVPRenderManager_GPU::DumpTextureToFile(const char *label, SDL_GPUTexture *tex, int w, int h) {
	if (!m_device || !tex || w <= 0 || h <= 0) return;
	if (w > 4096 || h > 4096) return; // sanity check

	// Flush any pending render pass so the texture content is finalized
	_EndFramePass();

	SDL_GPUTransferBufferCreateInfo tci = {};
	tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	tci.size = (Uint32)(w * h * 4);
	SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(m_device, &tci);
	if (!tb) return;

	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_device);
	if (!cmd) { SDL_ReleaseGPUTransferBuffer(m_device, tb); return; }

	SDL_GPUTextureRegion srcReg = {};
	srcReg.texture = tex;
	srcReg.w = (Uint32)w; srcReg.h = (Uint32)h; srcReg.d = 1;

	SDL_GPUTextureTransferInfo dstTI = { tb, 0 };
	SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
	if (cp) {
		SDL_DownloadFromGPUTexture(cp, &srcReg, &dstTI);
		SDL_EndGPUCopyPass(cp);
	}
	SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
	if (!fence) { SDL_ReleaseGPUTransferBuffer(m_device, tb); return; }
	SDL_WaitForGPUFences(m_device, true, &fence, 1);
	SDL_ReleaseGPUFence(m_device, fence);

	void *map = SDL_MapGPUTransferBuffer(m_device, tb, true);
	if (map) {
		// BMP header
		int pitch = w * 4;
		int dataSize = pitch * h;
		int bmpSize = 14 + 40 + dataSize;
		std::vector<uint8_t> bmp(bmpSize);
		bmp[0] = 'B'; bmp[1] = 'M';
		*(uint32_t*)&bmp[2] = bmpSize;
		*(uint32_t*)&bmp[10] = 14 + 40;
		*(uint32_t*)&bmp[14] = 40;
		*(int32_t*) &bmp[18] = w;
		*(int32_t*) &bmp[22] = -h; // top-down
		*(uint16_t*)&bmp[26] = 1;
		*(uint16_t*)&bmp[28] = 32;
		*(uint32_t*)&bmp[30] = 0;
		*(uint32_t*)&bmp[34] = dataSize;

		auto *src = (const uint8_t*)map;
		auto *dst = &bmp[54];
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				uint32_t px = *(const uint32_t*)(src + y * pitch + x * 4);
				dst[0] = (uint8_t)(px >> 16); // B
				dst[1] = (uint8_t)(px >> 8);  // G
				dst[2] = (uint8_t)(px);       // R
				dst[3] = (uint8_t)(px >> 24); // A (preserve actual alpha)
				dst += 4;
			}
		}
		SDL_UnmapGPUTransferBuffer(m_device, tb);

		s_captureSeq++;
		char path[256];
		snprintf(path, sizeof(path), "/sdcard/Download/cap_%s_%03d_%dx%d.bmp",
			label, s_captureSeq, w, h);
		FILE *f = fopen(path, "wb");
		if (f) { fwrite(bmp.data(), 1, bmpSize, f); fclose(f); }
		// Log corner + center pixel RGBA values for quick reference
		auto samplePx = [&](int sx, int sy) -> uint32_t {
			if (sx < 0) sx = 0; if (sx >= w) sx = w-1;
			if (sy < 0) sy = 0; if (sy >= h) sy = h-1;
			return *(const uint32_t*)(src + sy * pitch + sx * 4);
		};
		uint32_t tl = samplePx(0,0), tr = samplePx(w-1,0);
		uint32_t bl = samplePx(0,h-1), br = samplePx(w-1,h-1);
		uint32_t ct = samplePx(w/2,h/2);
		__android_log_print(ANDROID_LOG_INFO, "##krkr",
			"CAPTURE: %s (RGBA) TL=0x%08X TR=0x%08X BL=0x%08X BR=0x%08X CT=0x%08X",
			path, tl, tr, bl, br, ct);
	}
	SDL_ReleaseGPUTransferBuffer(m_device, tb);
}

uint32_t TVPRenderManager_GPU::ReadbackPixel(SDL_GPUTexture *tex, int x, int y) {
	if (!m_device || !tex) return 0;

	// Use a separate command buffer to not disturb m_cmd
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_device);
	if (!cmd) return 0;

	SDL_GPUTransferBufferCreateInfo tci = {};
	tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	tci.size = 4;
	SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(m_device, &tci);
	if (!tb) {
		SDL_CancelGPUCommandBuffer(cmd);
		return 0;
	}

	uint32_t pixel = 0;
	SDL_GPUTextureRegion srcReg = {};
	srcReg.texture = tex;
	srcReg.x = (Uint32)x; srcReg.y = (Uint32)y;
	srcReg.w = 1; srcReg.h = 1; srcReg.d = 1;
	SDL_GPUTextureTransferInfo dstTI = { tb, 0 };

	SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
	if (cp) {
		SDL_DownloadFromGPUTexture(cp, &srcReg, &dstTI);
		SDL_EndGPUCopyPass(cp);
		SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
		if (fence) {
			SDL_WaitForGPUFences(m_device, true, &fence, 1);
			SDL_ReleaseGPUFence(m_device, fence);
			void *map = SDL_MapGPUTransferBuffer(m_device, tb, true);
			if (map) {
				memcpy(&pixel, map, 4);
				SDL_UnmapGPUTransferBuffer(m_device, tb);
			}
		}
	} else {
		// Can't begin copy pass — just discard the cmd buffer
		SDL_SubmitGPUCommandBuffer(cmd);
	}
	SDL_ReleaseGPUTransferBuffer(m_device, tb);
	return pixel;
}

//------------------------------------------------------------------------------
// Registration
//------------------------------------------------------------------------------
extern "C" void TVPRegisterGPURenderer() {
	static bool registered = false;
	if (!registered) {
		TVPRegisterRenderManager("gpu",
			[]() -> iTVPRenderManager* {
				auto *inst = TVPRenderManager_GPU::Instance();
				if (inst && inst->IsReady()) {
					return inst;
				}
				// No initialized GPU renderer available — return nullptr so
				// TVPGetRenderManager() falls back to "software".
				return nullptr;
			});
		registered = true;
	}
}
