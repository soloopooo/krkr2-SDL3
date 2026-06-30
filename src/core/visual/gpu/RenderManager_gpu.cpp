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
	, m_texW(texW), m_texH(texH), m_format(fmt), m_opaque(opaque) {}

tTVPGPUTexture2D::~tTVPGPUTexture2D() {
	if (m_device && m_texture) {
		SDL_ReleaseGPUTexture(m_device, m_texture);
	}
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

	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_device);
	if (cmd) {
		SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(cmd);
		SDL_GPUTextureTransferInfo srcTI = { tb, 0 };
		SDL_GPUTextureRegion dstReg = {};
		dstReg.texture = m_texture;
		dstReg.w = (Uint32)w; dstReg.h = (Uint32)h; dstReg.d = 1;
		SDL_UploadToGPUTexture(cp, &srcTI, &dstReg, false);
		SDL_EndGPUCopyPass(cp);
		SDL_SubmitGPUCommandBuffer(cmd);
	}
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
	m_constColor[3] = (Value & 0xFF) / 255.0f;
	m_hasConstantColor = true;
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
const float TVPRenderManager_GPU::s_quadVerts[24] = {
	-1,-1, 0,1,   1,-1, 1,1,   -1,1, 0,0,
	-1,1,  0,0,   1,-1, 1,1,    1,1, 1,0,
};

TVPRenderManager_GPU::TVPRenderManager_GPU() {
	s_instance = this;
}

TVPRenderManager_GPU::~TVPRenderManager_GPU() {
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

	// Readback transfer buffers (double-buffered)
	SDL_GPUTransferBufferCreateInfo rci = {};
	rci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	rci.size = 1920 * 1080 * 4;
	for (int i = 0; i < READBACK_SLOTS; i++)
		m_rbSlot[i].tb = SDL_CreateGPUTransferBuffer(m_device, &rci);

	// Store swapchain format for display pipeline
	m_swapFormat = SDL_GetGPUSwapchainTextureFormat(m_device, m_window);

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
	if (m_quadVerts) SDL_ReleaseGPUBuffer(m_device, m_quadVerts);
	if (m_vs) SDL_ReleaseGPUShader(m_device, m_vs);
	if (m_fs) SDL_ReleaseGPUShader(m_device, m_fs);
	if (m_fs_gray) SDL_ReleaseGPUShader(m_device, m_fs_gray);
	if (m_fs_blur) SDL_ReleaseGPUShader(m_device, m_fs_blur);
	if (m_fs_adjustGamma) SDL_ReleaseGPUShader(m_device, m_fs_adjustGamma);
	if (m_fs_univTrans) SDL_ReleaseGPUShader(m_device, m_fs_univTrans);
	if (m_sampler) SDL_ReleaseGPUSampler(m_device, m_sampler);
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
		c.srcC = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		c.srcA = SDL_GPU_BLENDFACTOR_ONE;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
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
	// Framebuffer-fetch _d variants: fall back to their base blend mode
	else if (strstr(name, "_d")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		c.srcA = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE;
	}
	// Framebuffer-fetch _a variants: preserve destination alpha
	else if (strstr(name, "_a")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
		c.srcA = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE_MINUS_CONSTANT_COLOR;
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
		"Created GPU method: %s (blend=%d)", name, (int)cfg.enable);
	return m;
}

//------------------------------------------------------------------------------
// iTVPRenderManager — GetRenderMethod
//------------------------------------------------------------------------------
iTVPRenderMethod* TVPRenderManager_GPU::GetRenderMethod(const char *name,
	uint32_t *hint) {
	auto *m = _GetOrCreateMethod(name);
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
	if (!tex) return nullptr;

	auto *ret = new tTVPGPUTexture2D(m_device, tex, (int)w, (int)h,
		(int)w, (int)h, format, false);

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
	// End previous pass if target changed
	if (m_currentTarget != target) {
		if (m_currentPass) {
			SDL_EndGPURenderPass(m_currentPass);
			m_currentPass = nullptr;
		}
		m_currentTarget = target;
	}
	if (!target || !m_cmd) return;

	// Begin render pass on the GPU texture
	tTVPGPUTexture2D *gpuTex = dynamic_cast<tTVPGPUTexture2D*>(target);
	if (!gpuTex || !gpuTex->GetGPUTexture()) return;

	SDL_GPUColorTargetInfo tg = {};
	tg.texture = gpuTex->GetGPUTexture();
	tg.load_op = SDL_GPU_LOADOP_LOAD;
	tg.store_op = SDL_GPU_STOREOP_STORE;
	SDL_GPUColorTargetInfo targets[1] = { tg };
	m_currentPass = SDL_BeginGPURenderPass(m_cmd, targets, 1, NULL);
}

//------------------------------------------------------------------------------
// iTVPRenderManager — OperateRect
//------------------------------------------------------------------------------
void TVPRenderManager_GPU::OperateRect(iTVPRenderMethod* method,
	iTVPTexture2D *tar, iTVPTexture2D *reftar,
	const tTVPRect& rctar, const tRenderTexRectArray &textures) {
	if (!m_device || !method) return;

	tTVPGPURenderMethod *gpuMethod = dynamic_cast<tTVPGPURenderMethod*>(method);
	if (!gpuMethod || !gpuMethod->GetPipeline()) return;

	// Ensure render pass on target
	SetRenderTarget(tar);
	if (!m_currentPass) return;

	// Set viewport to destination rect
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

	// Push UBO data if dirty (AdjustGamma, UnivTrans)
	if (gpuMethod->m_uboDirty && gpuMethod->m_uboSize > 0 && m_cmd) {
		SDL_PushGPUFragmentUniformData(m_cmd, 0,
			gpuMethod->m_uboData, (Uint32)gpuMethod->m_uboSize);
		gpuMethod->m_uboDirty = false;
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

	// Draw
	SDL_DrawGPUPrimitives(m_currentPass, 6, 1, 0, 0);
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
	drawCount = 0; vmemsize = 0;
	return true;
}

//------------------------------------------------------------------------------
// Frame lifecycle — public
//------------------------------------------------------------------------------
void TVPRenderManager_GPU::BeginFrame() {
	_BeginFrame();
}

void TVPRenderManager_GPU::EndFrame() {
	if (!m_cmd) return;

	// ---- Step 1: Process previous readback (fence is done) ----
	int prevSlot = m_rbActive ^ 1;
	if (m_rbSlot[prevSlot].fence) {
		// Non-blocking check: skip if still pending
		SDL_GPUFence *fencePtr = m_rbSlot[prevSlot].fence;
		SDL_WaitForGPUFences(m_device, true, &fencePtr, 1);
	}
	if (m_rbSlot[prevSlot].fence) {
		// Readback still pending from last frame — skip processing
	} else if (m_rbSlot[prevSlot].tb && m_rbSlot[prevSlot].texW > 0) {
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
		tg.load_op = SDL_GPU_LOADOP_CLEAR;
		tg.store_op = SDL_GPU_STOREOP_STORE;
		tg.clear_color = (SDL_FColor){0.0f, 0.3f, 0.6f, 1.0f};
		SDL_GPURenderPass *rp = SDL_BeginGPURenderPass(m_cmd, &tg, 1, NULL);
		if (rp) {
			if (!m_quadPipeline) {
				m_quadPipeline = _CreateQuadPipeline(m_swapFormat,
					SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_ZERO,
					SDL_GPU_BLENDFACTOR_ONE, SDL_GPU_BLENDFACTOR_ZERO,
					SDL_GPU_BLENDOP_ADD, SDL_GPU_BLENDOP_ADD, false);
			}
			SDL_GPUViewport vp = {0,0,(float)m_swW,(float)m_swH,0,1};
			SDL_SetGPUViewport(rp, &vp);
			SDL_BindGPUGraphicsPipeline(rp, m_quadPipeline);
			SDL_GPUBufferBinding bb = { m_quadVerts, 0 };
			SDL_BindGPUVertexBuffers(rp, 0, &bb, 1);
			if (readbackTex) {
				tTVPGPUTexture2D *gpuTex = dynamic_cast<tTVPGPUTexture2D*>(readbackTex);
				if (gpuTex && gpuTex->GetGPUTexture() &&
					m_swapchainTex != gpuTex->GetGPUTexture()) {
					SDL_GPUTextureSamplerBinding ts = { gpuTex->GetGPUTexture(), m_sampler };
					SDL_BindGPUFragmentSamplers(rp, 0, &ts, 1);
				}
			}
			SDL_DrawGPUPrimitives(rp, 6, 1, 0, 0);
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
	m_currentTarget = nullptr;
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

void TVPRenderManager_GPU::ReadbackAndPresent(iTVPTexture2D *finalTex) {
	if (!m_device) return;
	if (finalTex) m_currentTarget = finalTex;
	BeginFrame();
	EndFrame();
}

//------------------------------------------------------------------------------
// Registration
//------------------------------------------------------------------------------
extern "C" void TVPRegisterGPURenderer() {
	static bool registered = false;
	if (!registered) {
		TVPRegisterRenderManager("gpu",
			[]() -> iTVPRenderManager* {
				return new TVPRenderManager_GPU;
			});
		registered = true;
	}
}
