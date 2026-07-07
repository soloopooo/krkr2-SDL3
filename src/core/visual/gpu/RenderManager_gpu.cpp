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
#include <dlfcn.h>

#define TAG "##gpu"

DisplayMode g_displayMode = DisplayMode::SOFTWARE;
SDL_Window *g_window = nullptr;
TVPRenderManager_GPU *TVPRenderManager_GPU::s_instance = nullptr;
std::atomic<uint64_t> TVPRenderManager_GPU::s_totalVMem;

// RenderDoc capture trigger (called from Java overlay / JNI)
static TVPRenderManager_GPU *s_rdocTriggerTarget = nullptr;
void TriggerRenderDocCapture() {
	if (s_rdocTriggerTarget)
		s_rdocTriggerTarget->TriggerRenderDocCapture();
}

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
	// Bug #8: if GPU wrote to this texture (as render target), m_pixels is stale.
	// Trigger synchronous readback to refresh m_pixels before returning.
	if (m_pixelsDirty) {
		ReadbackToPixels();
		m_pixelsDirty = false;
	}
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
		if (format == TVPTextureFormat::Gray) {
			// Expand 8-bit Gray → RGBA (R=G=B=gray, A=255)
			// Avoids heap overread from treating Gray pitch as w*4.
			for (int y = 0; y < h; y++) {
				const uint8_t *srcRow = (const uint8_t*)pixel + y * pitch;
				uint32_t *dstRow = (uint32_t*)map + y * w;
				for (int x = 0; x < w; x++) {
					uint8_t g = srcRow[x];
					dstRow[x] = 0xFF000000 | (g << 16) | (g << 8) | g;
				}
			}
		} else {
			// Bitmap memory is RGBA (byte0=R, byte1=G, byte2=B, byte3=A).
			// TVPRGBQUAD struct names are misleading — Fill() uses TVP_REVRGB to
			// convert 0xAARRGGBB → 0xAABBGGRR, so in little-endian memory byte0=R.
			// GPU texture R8G8B8A8_UNORM expects byte0=R, so direct memcpy is correct.
			// (Bug #10: previously did R/B swap here — was wrong, caused blue-tinted skin)
			if (pitch == w * 4) {
				memcpy(map, pixel, (size_t)(h * w * 4));
			} else {
				for (int y = 0; y < h; y++)
					memcpy((uint8_t*)map + y * w * 4,
						(const uint8_t*)pixel + y * pitch,
						(size_t)(w * 4));
			}
		}
	}
	SDL_UnmapGPUTransferBuffer(m_device, tb);

	// Sync CPU-side m_pixels so GetScanLineForRead/GetPoint return valid data
	{
		uint8_t *dstBase = m_pixels.data() + rc.top * m_pitch + rc.left * 4;
		if (format == TVPTextureFormat::Gray) {
			for (int y = 0; y < h; y++) {
				const uint8_t *srcRow = (const uint8_t*)pixel + y * pitch;
				uint32_t *dstRow = (uint32_t*)(dstBase + y * m_pitch);
				for (int x = 0; x < w; x++)
					dstRow[x] = 0xFF000000 | (srcRow[x] << 16) | (srcRow[x] << 8) | srcRow[x];
			}
		} else {
			for (int y = 0; y < h; y++)
				memcpy(dstBase + y * m_pitch, (const uint8_t*)pixel + y * pitch, (size_t)(w * 4));
		}
	}

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
		dstReg.x = (Uint32)rc.left; dstReg.y = (Uint32)rc.top;
		dstReg.w = (Uint32)w; dstReg.h = (Uint32)h; dstReg.d = 1;
		SDL_UploadToGPUTexture(cp, &srcTI, &dstReg, false);
		SDL_EndGPUCopyPass(cp);
	}
	if (!TVPRenderManager_GPU::CurrentCmd()) {
		// Out-of-frame update: submit immediately and wait with fence.
		// This ensures the upload completes (and transfer buffer is safe to release)
		// before any subsequent frame tries to read this texture.
		SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
		if (fence) {
			SDL_WaitForGPUFences(m_device, true, &fence, 1);
			SDL_ReleaseGPUFence(m_device, fence);
		}
	}
	SDL_ReleaseGPUTransferBuffer(m_device, tb);
}

// Bug #15 fix: implement GetPoint/SetPoint using m_pixels (RGBA layout).
// GetScanLineForRead handles dirty readback; we delegate to it.
uint32_t tTVPGPUTexture2D::GetPoint(int x, int y) {
	if (x < 0 || y < 0 || x >= m_width || y >= m_height) return 0;
	const void *sl = GetScanLineForRead((tjs_uint)y);
	if (!sl) return 0;
	const uint8_t *p = (const uint8_t*)sl + x * 4;
	// m_pixels is RGBA (byte0=R) → return as 0xAARRGGBB
	return ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) |
	       ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}
void tTVPGPUTexture2D::SetPoint(int x, int y, uint32_t clr) {
	if (x < 0 || y < 0 || x >= m_width || y >= m_height) return;
	void *sl = GetScanLineForWrite((tjs_uint)y);
	if (!sl) return;
	uint8_t *p = (uint8_t*)sl + x * 4;
	// 0xAARRGGBB → RGBA (byte0=R)
	p[0] = (uint8_t)((clr >> 16) & 0xFF); // R
	p[1] = (uint8_t)((clr >> 8) & 0xFF);  // G
	p[2] = (uint8_t)(clr & 0xFF);         // B
	p[3] = (uint8_t)((clr >> 24) & 0xFF); // A
	// Note: this only updates CPU-side m_pixels, not the GPU texture.
	// Caller must call Update() to push changes to GPU if needed.
}

// Bug #8: Synchronous GPU→CPU readback to refresh m_pixels after render target writes.
// GPU texture and m_pixels are both RGBA (byte0=R), so direct copy — no R/B swap needed.
void tTVPGPUTexture2D::ReadbackToPixels() {
	if (!m_device || !m_texture) return;
	SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_device);
	if (!cmd) return;

	SDL_GPUTransferBufferCreateInfo tci = {};
	tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	tci.size = (Uint32)(m_width * m_height * 4);
	SDL_GPUTransferBuffer *tb = SDL_CreateGPUTransferBuffer(m_device, &tci);
	if (!tb) { SDL_CancelGPUCommandBuffer(cmd); return; }

	SDL_GPUTextureRegion srcReg = {};
	srcReg.texture = m_texture;
	srcReg.w = (Uint32)m_width; srcReg.h = (Uint32)m_height; srcReg.d = 1;
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
				// GPU texture is R8G8B8A8 (byte0=R), m_pixels is RGBA (byte0=R) — direct copy
				memcpy(m_pixels.data(), map, (size_t)(m_width * m_height * 4));
				SDL_UnmapGPUTransferBuffer(m_device, tb);
			}
		}
	} else {
		SDL_SubmitGPUCommandBuffer(cmd);
	}
	SDL_ReleaseGPUTransferBuffer(m_device, tb);
}

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

	// Enable Vulkan validation layer (libVkLayer_khronos_validation.so pushed to /data/local/tmp/)
	setenv("VK_LAYER_PATH", "/data/local/tmp", 1);
	setenv("VK_INSTANCE_LAYERS", "VK_LAYER_KHRONOS_validation", 1);
	__android_log_print(ANDROID_LOG_INFO, TAG,
		"Vulkan validation layer enabled via VK_LAYER_PATH=/data/local/tmp");

	// Use properties to request Vulkan 1.3 (required by RenderDoc layer).
	SDL_PropertiesID props = SDL_CreateProperties();
	SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
	SDL_SetBooleanProperty(props, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, true);
	SDL_SetStringProperty(props, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "vulkan");

	SDL_GPUVulkanOptions vulkanOpts = {};
	vulkanOpts.vulkan_api_version = 0x00403000; // VK_API_VERSION_1_3
	SDL_SetPointerProperty(props, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_OPTIONS_POINTER, &vulkanOpts);

	m_device = SDL_CreateGPUDeviceWithProperties(props);
	SDL_DestroyProperties(props);

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

	sc.code = (const Uint8*)quad_pma_fragSpv;
	sc.code_size = quad_pma_fragSpvSize;
	m_fs_pma = SDL_CreateGPUShader(m_device, &sc);

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

	// ApplyColorMap shader (reads glyph mask, outputs colored text)
	// Uses binding=0 (standard FragParams) + binding=1 (ColorParams: text_color)
	sc2.num_samplers = 1;
	sc2.num_uniform_buffers = 2;
	sc2.code = (const Uint8*)apply_colormap_fragSpv;
	sc2.code_size = apply_colormap_fragSpvSize;
	m_fs_applyColorMap = SDL_CreateGPUShader(m_device, &sc2);

	sc2.code = (const Uint8*)apply_colormap_a_fragSpv;
	sc2.code_size = apply_colormap_a_fragSpvSize;
	m_fs_applyColorMap_a = SDL_CreateGPUShader(m_device, &sc2);

	// PsOverlayBlend shader (2-tex: source + dest copy)
	sc2.num_samplers = 2;
	sc2.num_uniform_buffers = 1;
	sc2.code = (const Uint8*)ps_overlay_fragSpv;
	sc2.code_size = ps_overlay_fragSpvSize;
	m_fs_psOverlay = SDL_CreateGPUShader(m_device, &sc2);

	// AlphaBlend_d shader (2-tex: source + dest copy, opacity-on-opacity LUT)
	sc2.code = (const Uint8*)alpha_blend_d_fragSpv;
	sc2.code_size = alpha_blend_d_fragSpvSize;
	m_fs_alphaBlendD = SDL_CreateGPUShader(m_device, &sc2);

	// Photoshop blend shaders (all 2-tex dest-read, 1 UBO for FragParams)
	sc2.num_samplers = 2;
	sc2.num_uniform_buffers = 1;
	sc2.code = (const Uint8*)ps_hardlight_fragSpv;
	sc2.code_size = ps_hardlight_fragSpvSize;
	m_fs_psHardLight = SDL_CreateGPUShader(m_device, &sc2);

	sc2.code = (const Uint8*)ps_softlight_fragSpv;
	sc2.code_size = ps_softlight_fragSpvSize;
	m_fs_psSoftLight = SDL_CreateGPUShader(m_device, &sc2);

	sc2.code = (const Uint8*)ps_colordodge_fragSpv;
	sc2.code_size = ps_colordodge_fragSpvSize;
	m_fs_psColorDodge = SDL_CreateGPUShader(m_device, &sc2);

	sc2.code = (const Uint8*)ps_colorburn_fragSpv;
	sc2.code_size = ps_colorburn_fragSpvSize;
	m_fs_psColorBurn = SDL_CreateGPUShader(m_device, &sc2);

	sc2.code = (const Uint8*)ps_diff_fragSpv;
	sc2.code_size = ps_diff_fragSpvSize;
	m_fs_psDiff = SDL_CreateGPUShader(m_device, &sc2);

	sc2.code = (const Uint8*)ps_exclusion_fragSpv;
	sc2.code_size = ps_exclusion_fragSpvSize;
	m_fs_psExclusion = SDL_CreateGPUShader(m_device, &sc2);

	sc2.code = (const Uint8*)ps_lighten_fragSpv;
	sc2.code_size = ps_lighten_fragSpvSize;
	m_fs_psLighten = SDL_CreateGPUShader(m_device, &sc2);

	sc2.code = (const Uint8*)ps_darken_fragSpv;
	sc2.code_size = ps_darken_fragSpvSize;
	m_fs_psDarken = SDL_CreateGPUShader(m_device, &sc2);

	// _d variant shaders (2-tex dest-read, opacity-on-opacity)
	// ApplyColorMap_d uses 2 UBOs (FragParams + ColorParams like ApplyColorMap)
	sc2.num_samplers = 2;
	sc2.num_uniform_buffers = 2;
	sc2.code = (const Uint8*)apply_colormap_d_fragSpv;
	sc2.code_size = apply_colormap_d_fragSpvSize;
	m_fs_applyColorMap_d = SDL_CreateGPUShader(m_device, &sc2);

	// ConstAlphaBlend_d: 1 UBO (FragParams only)
	sc2.num_uniform_buffers = 1;
	sc2.code = (const Uint8*)const_alpha_blend_d_fragSpv;
	sc2.code_size = const_alpha_blend_d_fragSpvSize;
	m_fs_constAlphaBlend_d = SDL_CreateGPUShader(m_device, &sc2);

	// ConstColorAlphaBlend_d: 2 UBOs (FragParams + ColorParams)
	sc2.num_uniform_buffers = 2;
	sc2.code = (const Uint8*)const_color_alpha_blend_d_fragSpv;
	sc2.code_size = const_color_alpha_blend_d_fragSpvSize;
	m_fs_constColorAlphaBlend_d = SDL_CreateGPUShader(m_device, &sc2);

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
	InitRenderDoc();
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
	if (m_fs_pma) SDL_ReleaseGPUShader(m_device, m_fs_pma);
	if (m_fs_gray) SDL_ReleaseGPUShader(m_device, m_fs_gray);
	if (m_fs_blur) SDL_ReleaseGPUShader(m_device, m_fs_blur);
	if (m_fs_adjustGamma) SDL_ReleaseGPUShader(m_device, m_fs_adjustGamma);
	if (m_fs_univTrans) SDL_ReleaseGPUShader(m_device, m_fs_univTrans);
	if (m_fs_fill) SDL_ReleaseGPUShader(m_device, m_fs_fill);
	if (m_fs_present) SDL_ReleaseGPUShader(m_device, m_fs_present);
	if (m_fs_crossfade) SDL_ReleaseGPUShader(m_device, m_fs_crossfade);
	if (m_fs_applyColorMap) SDL_ReleaseGPUShader(m_device, m_fs_applyColorMap);
	if (m_fs_applyColorMap_a) SDL_ReleaseGPUShader(m_device, m_fs_applyColorMap_a);
	if (m_fs_psOverlay) SDL_ReleaseGPUShader(m_device, m_fs_psOverlay);
	if (m_fs_alphaBlendD) SDL_ReleaseGPUShader(m_device, m_fs_alphaBlendD);
	if (m_fs_psHardLight) SDL_ReleaseGPUShader(m_device, m_fs_psHardLight);
	if (m_fs_psSoftLight) SDL_ReleaseGPUShader(m_device, m_fs_psSoftLight);
	if (m_fs_psColorDodge) SDL_ReleaseGPUShader(m_device, m_fs_psColorDodge);
	if (m_fs_psColorBurn) SDL_ReleaseGPUShader(m_device, m_fs_psColorBurn);
	if (m_fs_psDiff) SDL_ReleaseGPUShader(m_device, m_fs_psDiff);
	if (m_fs_psExclusion) SDL_ReleaseGPUShader(m_device, m_fs_psExclusion);
	if (m_fs_psLighten) SDL_ReleaseGPUShader(m_device, m_fs_psLighten);
	if (m_fs_psDarken) SDL_ReleaseGPUShader(m_device, m_fs_psDarken);
	if (m_fs_applyColorMap_d) SDL_ReleaseGPUShader(m_device, m_fs_applyColorMap_d);
	if (m_fs_constAlphaBlend_d) SDL_ReleaseGPUShader(m_device, m_fs_constAlphaBlend_d);
	if (m_fs_constColorAlphaBlend_d) SDL_ReleaseGPUShader(m_device, m_fs_constColorAlphaBlend_d);
	if (m_tempDestCopy) { SDL_ReleaseGPUTexture(m_device, m_tempDestCopy); m_tempDestCopy = nullptr; }
	if (m_sampler) SDL_ReleaseGPUSampler(m_device, m_sampler);
	for (int i = 0; i < READBACK_SLOTS; i++) {
		if (m_rbSlot[i].fence) {
			SDL_GPUFence *fencePtr = m_rbSlot[i].fence;
			SDL_WaitForGPUFences(m_device, true, &fencePtr, 1);
			SDL_ReleaseGPUFence(m_device, m_rbSlot[i].fence);
		}
		if (m_rbSlot[i].tb) SDL_ReleaseGPUTransferBuffer(m_device, m_rbSlot[i].tb);
	}
	if (m_rdoc_lib) { dlclose(m_rdoc_lib); m_rdoc_lib = nullptr; m_rdoc = nullptr; }
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
	// AlphaBlend_Copy: Copy blend (ONE/ZERO) with shader opacity for alpha modulation.
	// Used when children draw to temp buffer: preserves correct per-pixel alpha 
	// without squaring (unlike SRC_ALPHA) and without leaking RGB from alpha=0
	// pixels (unlike CONSTANT_COLOR).
	else if (strstr(name, "AlphaBlend_Copy")) {
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
		c.srcA = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	}
	else if (!strcmp(name, "AlphaBlend_a") || !strcmp(name, "AlphaBlend_color_AlphaTest")
		|| !strcmp(name, "AlphaTest")) {
		// AlphaBlend_a: same shader as AlphaBlend (s.a *= opacity), but accumulates dest alpha.
		// OGL ref: blend SRC_ALPHA/ONE_MINUS_SRC_ALPHA (RGB), ONE/ONE_MINUS_SRC_ALPHA (A)
		// shader: s.a *= opacity (RGB untouched) — uses quad.frag, NOT quad_pma
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		c.srcA = SDL_GPU_BLENDFACTOR_ONE;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	}
	else if (!strcmp(name, "AdditiveAlphaBlend") || !strcmp(name, "AddBlend")) {
		// OGL ref: shader s *= opacity (premultiplied), blend ONE/ONE_MINUS_SRC_ALPHA (RGB), ZERO/ONE (A)
		// Result: RGB = src.rgb*opacity + dst*(1-src.a*opacity), A = dst (preserved)
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ONE;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		c.srcA = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE;
	}
	else if (!strcmp(name, "AdditiveAlphaBlend_a")) {
		// OGL ref: shader s *= opacity (premultiplied), blend ONE/ONE_MINUS_SRC_ALPHA (both RGB+A)
		// Result: RGB = src.rgb*opacity + dst*(1-src.a*opacity), A = src.a*opacity + dst*(1-src.a*opacity)
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ONE;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		c.srcA = SDL_GPU_BLENDFACTOR_ONE;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	}
	else if (!strcmp(name, "ScreenBlend")) {
		// OGL ref: shader s.rgb *= opacity, blend ONE/ONE_MINUS_SRC_COLOR (RGB), ZERO/ONE (A)
		// Result: RGB = src.rgb*opacity + dst*(1-src.rgb*opacity) — screen blend
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ONE;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
		c.srcA = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE;
	}
	else if (!strcmp(name, "PsAddBlend") || !strcmp(name, "PsScreenBlend")) {
		// PsAddBlend/PsScreenBlend: kept as CONSTANT_COLOR for now (need dest-read shader — Bug #12)
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE;
		c.srcA = SDL_GPU_BLENDFACTOR_ZERO;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE;
	}
	else if (!strcmp(name, "SubBlend") || !strcmp(name, "PsSubBlend")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_CONSTANT_COLOR;
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
	// PsOverlayBlend: uses custom 2-pass dest-read shader (Copy blend)
	else if (!strcmp(name, "PsOverlayBlend")) {
		// Copy mode — shader reads both src and dest copy, handles everything
		c.enable = false;
	}
	// PsHardLightBlend, PsSoftLightBlend: use Screen blend approx (temporary,
	// will be replaced with dest-read shaders like PsOverlayBlend)
	else if (!strcmp(name, "PsHardLightBlend") || !strcmp(name, "PsSoftLightBlend")) {
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
	else if (!strcmp(name, "ApplyColorMap_d")) {
		// Dest-alpha: SRC_ALPHA for RGB, Porter-Duff ONE for A
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		c.srcA = SDL_GPU_BLENDFACTOR_ONE;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	}
	else if (!strcmp(name, "ApplyColorMap_a")) {
		// Premultiplied: ONE/ONE_MINUS_SRC_ALPHA for both RGB and A
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ONE;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		c.srcA = SDL_GPU_BLENDFACTOR_ONE;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	}
	else if (strstr(name, "ApplyColorMap")) {
		// Standard: SRC_ALPHA/ONE_MINUS_SRC_ALPHA for RGB, ZERO/ONE for A (preserve dest alpha)
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
	// AlphaBlend_d: use fixed-function blend with Porter-Duff alpha
	// (ONE/ONE_MINUS_SRC_ALPHA for A, SRC_ALPHA/ONE_MINUS_SRC_ALPHA for RGB)
	// The full opacity-on-opacity LUT requires a dest-read shader which
	// introduces picture-in-picture artifacts on some compositing paths.
	// Fixed-function is correct for Da≈255 (opaque backgrounds) and is
	// a close approximation for semi-transparent compositing.
	else if (!strcmp(name, "AlphaBlend_d")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		c.srcA = SDL_GPU_BLENDFACTOR_ONE;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	}
	// _d variants: alpha blending with Porter-Duff alpha accumulation
	// RGB: SRC_ALPHA/ONE_MINUS_SRC_ALPHA (standard blend)
	// A:   ONE/ONE_MINUS_SRC_ALPHA     (Porter-Duff: new_a = Sa + Da*(1-Sa))
	else if (strstr(name, "_d") && !strstr(name, "SD") && !strstr(name, "ConstAlphaBlend") && !strstr(name, "ConstColorAlphaBlend")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		c.srcA = SDL_GPU_BLENDFACTOR_ONE;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	}
	// _a variants: additive alpha blending (ONE/ONE_MINUS_SRC_ALPHA)
	else if (strstr(name, "_a") && !strstr(name, "SD")) {
		c.enable = true;
		c.srcC = SDL_GPU_BLENDFACTOR_ONE;
		c.dstC = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		c.srcA = SDL_GPU_BLENDFACTOR_ONE;
		c.dstA = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
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
	} else if (!strcmp(name, "PsOverlayBlend")) {
		customFS = m_fs_psOverlay; customShader = true; customSamplers = 2;
		cfg.enable = false;
	} else if (!strcmp(name, "PsHardLightBlend")) {
		customFS = m_fs_psHardLight; customShader = true; customSamplers = 2;
		cfg.enable = false;
	} else if (!strcmp(name, "PsSoftLightBlend")) {
		customFS = m_fs_psSoftLight; customShader = true; customSamplers = 2;
		cfg.enable = false;
	} else if (!strcmp(name, "PsColorDodgeBlend")) {
		customFS = m_fs_psColorDodge; customShader = true; customSamplers = 2;
		cfg.enable = false;
	} else if (!strcmp(name, "PsColorBurnBlend")) {
		customFS = m_fs_psColorBurn; customShader = true; customSamplers = 2;
		cfg.enable = false;
	} else if (!strcmp(name, "PsDiffBlend")) {
		customFS = m_fs_psDiff; customShader = true; customSamplers = 2;
		cfg.enable = false;
	} else if (!strcmp(name, "PsExclusionBlend")) {
		customFS = m_fs_psExclusion; customShader = true; customSamplers = 2;
		cfg.enable = false;
	} else if (!strcmp(name, "PsLightenBlend")) {
		customFS = m_fs_psLighten; customShader = true; customSamplers = 2;
		cfg.enable = false;
	} else if (!strcmp(name, "PsDarkenBlend")) {
		customFS = m_fs_psDarken; customShader = true; customSamplers = 2;
		cfg.enable = false;
	} else if (!strcmp(name, "AlphaBlend_d")) {
		// Dest-alpha opacity-on-opacity: 2-texture shader reads src+dst
		// OGL ref: d.a = s.a + d.a - s.a*d.a; d.rgb = mix(d.rgb, s.rgb, s.a/(d.a+ε))
		customFS = m_fs_alphaBlendD; customShader = true; customSamplers = 2;
		cfg.enable = false;  // shader computes full result
	} else if (!strcmp(name, "AdditiveAlphaBlend") || !strcmp(name, "AddBlend")
	           || !strcmp(name, "ScreenBlend")) {
		// Premultiplied additive/screen: shader does s *= opacity (quad_pma)
		// Blend state (ONE/ONE_MINUS_SRC_ALPHA or ONE/ONE_MINUS_SRC_COLOR) handles the rest
		customFS = m_fs_pma; customShader = true;
	} else if (strstr(name, "ApplyColorMap")) {
		// ApplyColorMap: reads glyph mask (.r), applies text_color from binding=1 UBO
		if (strstr(name, "ApplyColorMap_d")) {
			// _d variant: dest-read opacity-on-opacity compositing
			customFS = m_fs_applyColorMap_d; customShader = true; customSamplers = 2; customUBO = 1;
			cfg.enable = false;
		} else if (strstr(name, "ApplyColorMap_a")) {
			customFS = m_fs_applyColorMap_a; customShader = true; customSamplers = 1;
		} else {
			customFS = m_fs_applyColorMap; customShader = true; customSamplers = 1;
		}
	} else if (!strcmp(name, "ConstAlphaBlend_d")) {
		// Dest-alpha opacity-on-opacity: 2-texture, 1 UBO
		customFS = m_fs_constAlphaBlend_d; customShader = true; customSamplers = 2;
		cfg.enable = false;
	} else if (!strcmp(name, "ConstColorAlphaBlend_d")) {
		// Dest-alpha opacity-on-opacity with color: 2-texture, 2 UBO
		customFS = m_fs_constColorAlphaBlend_d; customShader = true; customSamplers = 2; customUBO = 1;
		cfg.enable = false;
	} else if (strstr(name, "_a") && !strstr(name, "ApplyColorMap") && !strstr(name, "ConstAlphaBlend")
	           && strcmp(name, "AlphaBlend_a")) {
		// Premultiplied alpha blend: use quad_pma shader (rgb *= opacity)
		// AlphaBlend_a excluded — it uses quad.frag (only a *= opacity, like AlphaBlend)
		customFS = m_fs_pma; customShader = true;
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
	// Mark dest-read methods (2-pass: copy target → temp before rendering)
	static const char *const destReadMethods[] = {
		"PsOverlayBlend", "AlphaBlend_d",
		"PsHardLightBlend", "PsSoftLightBlend", "PsColorDodgeBlend",
		"PsColorBurnBlend", "PsDiffBlend", "PsExclusionBlend",
		"PsLightenBlend", "PsDarkenBlend",
		"ApplyColorMap_d", "ConstAlphaBlend_d", "ConstColorAlphaBlend_d"
	};
	bool isDestRead = false;
	for (auto drn : destReadMethods) {
		if (!strcmp(name, drn)) { isDestRead = true; break; }
	}
	if (isDestRead) m->m_needsDestRead = true;
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
// RenderDoc in-app capture API
//------------------------------------------------------------------------------
void TVPRenderManager_GPU::InitRenderDoc() {
	if (m_rdoc) return;
	m_rdoc_lib = dlopen("libVkLayer_GLES_RenderDoc.so", RTLD_NOW);
	if (!m_rdoc_lib) {
		__android_log_print(ANDROID_LOG_INFO, TAG,
			"RenderDoc not available (libVkLayer_GLES_RenderDoc.so not loaded)");
		return;
	}
	pRENDERDOC_GetAPI getApi = (pRENDERDOC_GetAPI)dlsym(m_rdoc_lib, "RENDERDOC_GetAPI");
	if (!getApi) {
		__android_log_print(ANDROID_LOG_WARN, TAG,
			"RenderDoc: RENDERDOC_GetAPI not found");
		dlclose(m_rdoc_lib); m_rdoc_lib = nullptr;
		return;
	}
	int ret = getApi(eRENDERDOC_API_Version_1_6_0, (void**)&m_rdoc);
	if (ret != 1 || !m_rdoc) {
		__android_log_print(ANDROID_LOG_WARN, TAG,
			"RenderDoc: GetAPI failed (ret=%d)", ret);
		m_rdoc = nullptr;
		dlclose(m_rdoc_lib); m_rdoc_lib = nullptr;
		return;
	}
	// Set capture path to /sdcard/Download/krkr2yuri_captures/ so we can easily pull files
	m_rdoc->SetCaptureFilePathTemplate("/sdcard/Download/krkr2yuri_captures/krkr2yuri");
	m_rdoc->SetCaptureOptionU32(eRENDERDOC_Option_DelayForDebugger, 0);
	int maj = 0, min = 0, pat = 0;
	m_rdoc->GetAPIVersion(&maj, &min, &pat);
	__android_log_print(ANDROID_LOG_INFO, TAG,
		"RenderDoc in-app API initialized v%d.%d.%d", maj, min, pat);
	s_rdocTriggerTarget = this;
}

void TVPRenderManager_GPU::TriggerRenderDocCapture() {
	if (!m_rdoc) {
		__android_log_print(ANDROID_LOG_WARN, TAG,
			"RenderDoc capture requested but RenderDoc not available");
		return;
	}
	m_captureThisFrame = true;
	__android_log_print(ANDROID_LOG_INFO, TAG,
		"RenderDoc capture queued for next frame");
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
			"SDL_CreateGPUTexture(%ux%u fmt=RGBA8) FAILED, returning null", w, h);
		return nullptr;
	}

	// One-time clear: fill texture with transparent black on creation
	// so LOADOP_LOAD in SetRenderTarget preserves content across frames.
	// Use fence to ensure clear completes before texture is used (avoids
	// race between initCmd and main compositing on m_cmd).
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
			SDL_GPUFence *initFence = SDL_SubmitGPUCommandBufferAndAcquireFence(initCmd);
			if (initFence) {
				SDL_WaitForGPUFences(m_device, true, &initFence, 1);
				SDL_ReleaseGPUFence(m_device, initFence);
			}
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
	int w = bmp->GetWidth(), h = bmp->GetHeight();
	int bpp = bmp->GetBPP();

	if (bpp == 24) {
		// Expand 24bpp RGB → 32bpp RGBA (bitmap memory is RGBA layout)
		__android_log_print(ANDROID_LOG_INFO, TAG,
			"CreateTexture2D(bmp): 24bpp RGB → RGBA expansion %dx%d", w, h);
		std::vector<uint8_t> rgba((size_t)(w * h * 4));
		for (int y = 0; y < h; y++) {
			const uint8_t *srcRow = (const uint8_t*)bmp->GetScanLine(y);
			uint8_t *dstRow = rgba.data() + y * w * 4;
			for (int x = 0; x < w; x++) {
				dstRow[x * 4 + 0] = srcRow[x * 3 + 0];  // R
				dstRow[x * 4 + 1] = srcRow[x * 3 + 1];  // G
				dstRow[x * 4 + 2] = srcRow[x * 3 + 2];  // B
				dstRow[x * 4 + 3] = 255;                 // A
			}
		}
		TVPTextureFormat::e fmt = TVPTextureFormat::RGBA;
		return CreateTexture2D(rgba.data(), w * 4, (unsigned int)w, (unsigned int)h, fmt);
	}

	TVPTextureFormat::e fmt = (bpp == 8)
		? TVPTextureFormat::Gray : TVPTextureFormat::RGBA;
	iTVPTexture2D *tex = CreateTexture2D(bmp->GetScanLine(0),
		bmp->GetPitch(), (unsigned int)w, (unsigned int)h, fmt);
	return tex;
}

iTVPTexture2D* TVPRenderManager_GPU::CreateTexture2D(TJS::tTJSBinaryStream* s) {
	if (!s) return nullptr;
	// Bug #14 fix: load uncompressed PVRv3 textures (BGRA/RGBA/Gray) from stream.
	// Compressed PVR formats (PVRTC/ETC) are not supported on Vulkan without
	// format-specific decompression — skip those for now.
	#pragma pack(push, 1)
	struct PVRv3Header {
		uint32_t signature;      // 'PVR\3'
		uint32_t textureHeight;
		uint32_t textureWidth;
		uint32_t pixelFormat;    // PVR3TexturePixelFormat enum
		uint32_t colorSpace;     // 0 = linear, 1 = sRGB
		uint32_t channelType;    // 0 = unsigned byte
		uint32_t metadataLength;
	};
	#pragma pack(pop)
	PVRv3Header hdr;
	if (s->Read(&hdr, sizeof(hdr)) != sizeof(hdr)) return nullptr;
	if (memcmp(&hdr.signature, "PVR\3", 4) != 0) return nullptr;
	if (hdr.textureWidth > 4096 || hdr.textureHeight > 4096) return nullptr;
	s->SetPosition(s->GetPosition() + hdr.metadataLength);

	tjs_uint pixsize = 0;
	TVPTextureFormat::e texfmt = TVPTextureFormat::RGBA;
	// PVR3TexturePixelFormat enum values
	enum PVR3Fmt : uint32_t {
		PVR_RGBA8888 = 0, PVR_BGRA8888 = 1, PVR_RGB565 = 2, PVR_RGBA5551 = 3,
		PVR_RGBA4444 = 4, PVR_RGB888 = 5, PVR_BGR888 = 6, PVR_A8 = 11, PVR_L8 = 12
	};
	switch ((PVR3Fmt)hdr.pixelFormat) {
		case PVR_RGBA8888: case PVR_BGRA8888: pixsize = 4; texfmt = TVPTextureFormat::RGBA; break;
		case PVR_RGB888: case PVR_BGR888: pixsize = 3; texfmt = TVPTextureFormat::RGBA; break;  // expand to RGBA
		case PVR_A8: case PVR_L8: pixsize = 1; texfmt = TVPTextureFormat::Gray; break;
		default: return nullptr;  // compressed or unsupported
	}
	tjs_uint pitch = hdr.textureWidth * pixsize;
	tjs_uint dataSize = pitch * hdr.textureHeight;
	std::vector<uint8_t> buf(dataSize);
	if (s->Read(buf.data(), dataSize) != dataSize) return nullptr;

	// For 3bpp RGB, expand to 4bpp RGBA (BGRA layout for bitmap compat)
	if (pixsize == 3) {
		std::vector<uint8_t> rgba((size_t)hdr.textureWidth * hdr.textureHeight * 4);
		for (uint32_t y = 0; y < hdr.textureHeight; y++) {
			for (uint32_t x = 0; x < hdr.textureWidth; x++) {
				rgba[(y * hdr.textureWidth + x) * 4 + 0] = buf[(y * hdr.textureWidth + x) * 3 + 0];
				rgba[(y * hdr.textureWidth + x) * 4 + 1] = buf[(y * hdr.textureWidth + x) * 3 + 1];
				rgba[(y * hdr.textureWidth + x) * 4 + 2] = buf[(y * hdr.textureWidth + x) * 3 + 2];
				rgba[(y * hdr.textureWidth + x) * 4 + 3] = 255;
			}
		}
		return CreateTexture2D(rgba.data(), hdr.textureWidth * 4,
			hdr.textureWidth, hdr.textureHeight, TVPTextureFormat::RGBA);
	}
	return CreateTexture2D(buf.data(), pitch, hdr.textureWidth, hdr.textureHeight, texfmt);
}

iTVPTexture2D* TVPRenderManager_GPU::CreateTexture2D(unsigned int neww,
	unsigned int newh, iTVPTexture2D* tex) {
	if (!tex) return nullptr;
	iTVPTexture2D *ret = CreateTexture2D(nullptr, 0, neww, newh,
		tex->GetFormat());
	if (!ret) return nullptr;

	// Copy old texture content to new texture using a GPU copy pass.
	// Uses a separate command buffer to avoid interfering with any
	// active render pass on the frame's command buffer.
	tTVPGPUTexture2D *oldGpu = dynamic_cast<tTVPGPUTexture2D*>(tex);
	tTVPGPUTexture2D *newGpu = dynamic_cast<tTVPGPUTexture2D*>(ret);
	if (oldGpu && newGpu && oldGpu->GetGPUTexture() && newGpu->GetGPUTexture()) {
		SDL_GPUCommandBuffer *copyCmd = SDL_AcquireGPUCommandBuffer(m_device);
		if (copyCmd) {
			SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(copyCmd);
			if (cp) {
				int copyW = std::min((int)neww, (int)oldGpu->GetWidth());
				int copyH = std::min((int)newh, (int)oldGpu->GetHeight());
				SDL_GPUTextureLocation srcLoc = { oldGpu->GetGPUTexture(), 0, 0, 0, 0, 0 };
				SDL_GPUTextureLocation dstLoc = { newGpu->GetGPUTexture(), 0, 0, 0, 0, 0 };
				SDL_CopyGPUTextureToTexture(cp, &srcLoc, &dstLoc, copyW, copyH, 1, false);
				SDL_EndGPUCopyPass(cp);
			}
			// Fence to ensure copy completes before texture is used in compositing
			SDL_GPUFence *copyFence = SDL_SubmitGPUCommandBufferAndAcquireFence(copyCmd);
			if (copyFence) {
				SDL_WaitForGPUFences(m_device, true, &copyFence, 1);
				SDL_ReleaseGPUFence(m_device, copyFence);
			}
		}
	}
	return ret;
}

//------------------------------------------------------------------------------
// iTVPRenderManager — SetRenderTarget
//------------------------------------------------------------------------------
void TVPRenderManager_GPU::SetRenderTarget(iTVPTexture2D *target) {
	if (!m_cmd) return;

	// Batching: if targeting the same texture, keep the pass open to
	// avoid per-draw-call BeginPass/EndPass tile flushes.
	if (target && target == m_currentTarget && m_currentPass) {
		return;
	}

	// End previous pass
	if (m_currentPass) {
		SDL_EndGPURenderPass(m_currentPass);
		m_currentPass = nullptr;
	}
	m_currentTarget = target;
	if (target) {
		// Bug #8: mark target as dirty — GPU will write to it, m_pixels becomes stale
		tTVPGPUTexture2D *gpuTexDirty = dynamic_cast<tTVPGPUTexture2D*>(target);
		if (gpuTexDirty) gpuTexDirty->SetPixelsDirty();
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
	// First render target each frame: clear to opaque black to prevent
	// frame-to-frame accumulation. Subsequent targets within the same
	// frame use LOAD to preserve content from previous passes.
	tg.load_op = m_frameFirstTarget ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
	tg.store_op = SDL_GPU_STOREOP_STORE;
	tg.clear_color = (SDL_FColor){0.0f, 0.0f, 0.0f, 0.0f};
	m_frameFirstTarget = false;
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
		__android_log_print(ANDROID_LOG_INFO, "##krkr", "OPR[%d]: %s nTex=%d opa=%d tar=%p(%d,%d) dstGPU=%d src=%p srcGPU=%d blend:sc=%d dc=%d sa=%d da=%d",
			s_oprFrame, mname.c_str(), (int)textures.size(),
			gpuMethod->m_opacity, tar, tw, th, (int)dstIsGPU, srcTex0, (int)srcIsGPU,
			(int)gpuMethod->m_srcColor, (int)gpuMethod->m_dstColor,
			(int)gpuMethod->m_srcAlpha, (int)gpuMethod->m_dstAlpha);
	}



	// Safety guard: if any source texture is the current render target,
	// flush the pass first (Vulkan forbids sampling a render target in the
	// same pass). SetRenderTarget will then start a fresh pass.
	{
		bool srcIsCurrentTarget = false;
		for (size_t i = 0; i < textures.size(); i++) {
			if (textures[i].first == m_currentTarget) {
				srcIsCurrentTarget = true;
				break;
			}
		}
		if (srcIsCurrentTarget && m_currentPass) {
			FlushPass();
		}
	}

	// Ensure render pass on target FIRST so m_currentTarget is valid
	// before PrepareDestReadCopy (which needs it to copy the correct dest).
	SetRenderTarget(tar);
	if (!m_currentPass) {
		__android_log_print(ANDROID_LOG_INFO, "##krkr", "[DIAG] OperateRect: no render pass after SetRenderTarget — tar=%p", tar);
		return;
	}

	// Dest-read methods: copy current target to temp texture before rendering.
	SDL_GPUTexture *tempDestTex = nullptr;
	if (gpuMethod->m_needsDestRead) {
		tempDestTex = PrepareDestReadCopy();
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
	// Bug #23 fix: for 2-texture crossfade methods, push second texture's UV
	// UBO layout: [uvOffset0(8)][uvScale0(8)][opacity(4)][pad(4)][uvOffset1(8)][uvScale1(8)]
	bool isCrossfade = (mname.find("ConstAlphaBlend_SD") != std::string::npos ||
	                    mname.find("ConstColorAlphaBlend_SD") != std::string::npos);
	if (isCrossfade && textures.size() > 1) {
		auto *srcTex2 = dynamic_cast<tTVPGPUTexture2D*>(textures[1].first);
		if (srcTex2) {
			const tTVPRect &sr2 = textures[1].second;
			float tw2 = (float)srcTex2->GetWidth();
			float th2 = (float)srcTex2->GetHeight();
			if (tw2 > 0 && th2 > 0) {
				uvF[6] = (float)sr2.left / tw2;       // uvOffset1.x
				uvF[7] = (float)sr2.top / th2;         // uvOffset1.y
				uvF[8] = (float)sr2.get_width() / tw2; // uvScale1.x
				uvF[9] = (float)sr2.get_height() / th2;// uvScale1.y
				uboSize = 40; // 10 floats = 40 bytes
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

	// Push binding=1 UBO for methods with ColorParams (text_color)
	// ApplyColorMap* and ConstColorAlphaBlend_d use binding=1 for color uniform
	if (mname.find("ApplyColorMap") != std::string::npos ||
	    mname.find("ConstColorAlphaBlend_d") != std::string::npos) {
		float colData[4] = {
			gpuMethod->GetConstColor(0),
			gpuMethod->GetConstColor(1),
			gpuMethod->GetConstColor(2),
			gpuMethod->GetConstColor(3)
		};
		SDL_PushGPUFragmentUniformData(m_cmd, 1, colData, 16);
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

	// Dest-read: bind temp dest copy as extra sampler (slot = nBind)
	if (tempDestTex) {
		SDL_GPUTextureSamplerBinding ts = { tempDestTex, m_sampler };
		SDL_BindGPUFragmentSamplers(m_currentPass, nBind, &ts, 1);
	}

	// Set blend constants for CONSTANT_COLOR blend modes
	if (useBlendConstants) {
		float a = (float)gpuMethod->m_opacity / 255.0f;
		SDL_FColor bc = { a, a, a, a };
		SDL_SetGPUBlendConstants(m_currentPass, bc);
	}

	// Draw
	SDL_DrawGPUPrimitives(m_currentPass, 6, 1, 0, 0);
	m_drawCount++;
}

//------------------------------------------------------------------------------
// iTVPRenderManager — OperateTriangles
//------------------------------------------------------------------------------
void TVPRenderManager_GPU::OperateTriangles(iTVPRenderMethod* method,
	int nTriangles, iTVPTexture2D *target, iTVPTexture2D *reftar,
	const tTVPRect& rcclip, const tTVPPointD* pttar,
	const tRenderTexQuadArray &textures) {
	// Bug #11 fix: basic triangle rendering via dynamic vertex buffer.
	// Uses the method's pipeline with triangle list topology.
	// Each vertex: pos2D (float2) + uv2D (float2) = 16 bytes, matching quad format.
	if (!method || !target || nTriangles <= 0) return;
	tTVPGPURenderMethod *gpuMethod = dynamic_cast<tTVPGPURenderMethod*>(method);
	if (!gpuMethod) return;

	// For now, handle nTriangles==2 as a quad (most common case from AffineBlt)
	// by delegating to OperateRect with the clip rect as destination.
	if (nTriangles == 2 && textures.size() > 0) {
		// Build a tRenderTexRectArray from the quad textures.
		// Use the source texture's full rect as the texture coords.
		std::vector<std::pair<iTVPTexture2D*, tTVPRect>> rectVec;
		for (size_t i = 0; i < textures.size(); i++) {
			iTVPTexture2D *tex = textures[i].first;
			if (tex) {
				tTVPRect fullRect(0, 0, (tjs_int)tex->GetWidth(), (tjs_int)tex->GetHeight());
				rectVec.push_back(std::make_pair(tex, fullRect));
			}
		}
		tRenderTexRectArray rectTextures(rectVec.data(), rectVec.size());
		OperateRect(method, target, reftar, rcclip, rectTextures);
		return;
	}
	// For arbitrary triangle counts, log a warning (full implementation needs
	// a dynamic vertex buffer + triangle-list pipeline — TODO).
	static int s_warnCount = 10;
	if (s_warnCount > 0) {
		s_warnCount--;
		__android_log_print(ANDROID_LOG_WARN, TAG,
			"OperateTriangles: nTriangles=%d not yet supported (only 2=quad)", nTriangles);
	}
}

//------------------------------------------------------------------------------
// iTVPRenderManager — OperatePerspective (stub)
//------------------------------------------------------------------------------
void TVPRenderManager_GPU::OperatePerspective(iTVPRenderMethod* method,
	int nQuads, iTVPTexture2D *target, iTVPTexture2D *reftar,
	const tTVPRect& rcclip, const tTVPPointD* pttar,
	const tRenderTexQuadArray &textures) {
	// Bug #11: PerspectiveAlphaBlend_a registered but needs perspective matrix
	// in vertex shader. Full implementation TODO — needs custom vertex shader
	// with mat4 uniform for perspective projection.
	static int s_warnCount = 10;
	if (s_warnCount > 0) {
		s_warnCount--;
		__android_log_print(ANDROID_LOG_WARN, TAG,
			"OperatePerspective: not yet implemented (nQuads=%d)", nQuads);
	}
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

SDL_GPUTexture* TVPRenderManager_GPU::PrepareDestReadCopy() {
	if (!m_cmd || !m_currentTarget) return nullptr;
	auto *gpuTar = dynamic_cast<tTVPGPUTexture2D*>(m_currentTarget);
	if (!gpuTar || !gpuTar->GetGPUTexture()) return nullptr;

	int w = gpuTar->GetWidth(), h = gpuTar->GetHeight();
	if (w <= 0 || h <= 0) return nullptr;

	// End current render pass so we can copy the target
	_EndFramePass();

	// Resize temp texture if needed
	if (!m_tempDestCopy || m_tempDestCopyW != w || m_tempDestCopyH != h) {
		if (m_tempDestCopy) SDL_ReleaseGPUTexture(m_device, m_tempDestCopy);
		SDL_GPUTextureCreateInfo ti = {};
		ti.type = SDL_GPU_TEXTURETYPE_2D;
		ti.format = m_texFormat;
		ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		ti.width = (Uint32)w; ti.height = (Uint32)h; ti.layer_count_or_depth = 1;
		ti.num_levels = 1; ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
		m_tempDestCopy = SDL_CreateGPUTexture(m_device, &ti);
		m_tempDestCopyW = w; m_tempDestCopyH = h;
	}
	if (!m_tempDestCopy) return nullptr;

	// Copy current target to temp (GPU-side copy, no CPU round-trip)
	SDL_GPUCopyPass *cp = SDL_BeginGPUCopyPass(m_cmd);
	if (cp) {
		SDL_GPUTextureLocation srcLoc = { gpuTar->GetGPUTexture(), 0, 0, 0, 0, 0 };
		SDL_GPUTextureLocation dstLoc = { m_tempDestCopy, 0, 0, 0, 0, 0 };
		SDL_CopyGPUTextureToTexture(cp, &srcLoc, &dstLoc, (Uint32)w, (Uint32)h, 1, false);
		SDL_EndGPUCopyPass(cp);
	}

	// Begin a new render pass on the same target (LOAD to preserve previous pixel state)
	SDL_GPUColorTargetInfo tg = {};
	tg.texture = gpuTar->GetGPUTexture();
	tg.load_op = SDL_GPU_LOADOP_LOAD;
	tg.store_op = SDL_GPU_STOREOP_STORE;
	tg.clear_color = (SDL_FColor){0.0f, 0.0f, 0.0f, 0.0f};
	SDL_GPUColorTargetInfo targets[1] = { tg };
	m_currentPass = SDL_BeginGPURenderPass(m_cmd, targets, 1, NULL);

	return m_tempDestCopy;
}

void TVPRenderManager_GPU::BeginFrame() {
	_BeginFrame();
}

void TVPRenderManager_GPU::EndFrame() {
	if (!m_cmd) return;

	// (Bug #9 fix: previous frame's fence wait moved to _BeginFrame so it happens
	//  before any Update() calls in this frame, preventing texture data races.)

	// ---- Step 1: End compositing render pass ----
	_EndFramePass();

	// ---- Step 2: Issue readback for current frame ----
	int slot = m_rbActive;
	m_rbSlot[slot].texW = 0; m_rbSlot[slot].texH = 0;
	iTVPTexture2D *readbackTex = m_currentTarget;
	if (readbackTex) {
		tTVPGPUTexture2D *gpuTex = dynamic_cast<tTVPGPUTexture2D*>(readbackTex);
		if (gpuTex && gpuTex->GetGPUTexture()) {
			Uint32 tw = (Uint32)gpuTex->GetWidth();
			Uint32 th = (Uint32)gpuTex->GetHeight();
			// Bug #19 fix: removed hard-coded 1920x1080 limit — allocate transfer buffer dynamically
			if (tw > 0 && th > 0) {
				// Ensure transfer buffer is large enough for this frame's readback
				Uint32 needed = tw * th * 4;
				if (needed > m_rbSlot[slot].tbSize) {
					// Reallocate larger transfer buffer
					if (m_rbSlot[slot].tb) SDL_ReleaseGPUTransferBuffer(m_device, m_rbSlot[slot].tb);
					SDL_GPUTransferBufferCreateInfo rci = {};
					rci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
					rci.size = needed;
					m_rbSlot[slot].tb = SDL_CreateGPUTransferBuffer(m_device, &rci);
					m_rbSlot[slot].tbSize = needed;
				}
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
			// Determine which texture to present
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

	// ---- Step 6: Submit with fence (no blocking) ----
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

	// ---- Step 0: Wait for previous frame's fence BEFORE issuing any new commands ----
	// This ensures the GPU has finished using any textures that were render targets
	// in the previous frame, so Update() calls in this frame don't race with GPU reads.
	// (Bug #9 fix: was previously in EndFrame, too late to prevent data races.)
	int prevSlot = m_rbActive ^ 1;
	bool rbReady = false;
	if (m_rbSlot[prevSlot].fence) {
		SDL_GPUFence *fencePtr = m_rbSlot[prevSlot].fence;
		SDL_WaitForGPUFences(m_device, true, &fencePtr, 1);
		SDL_ReleaseGPUFence(m_device, m_rbSlot[prevSlot].fence);
		m_rbSlot[prevSlot].fence = nullptr;
		rbReady = true;
	}
	if (rbReady && m_rbSlot[prevSlot].tb && m_rbSlot[prevSlot].texW > 0) {
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
	if (m_rbSlot[prevSlot].fence) {
		SDL_ReleaseGPUFence(m_device, m_rbSlot[prevSlot].fence);
		m_rbSlot[prevSlot].fence = nullptr;
	}

	m_cmd = SDL_AcquireGPUCommandBuffer(m_device);
	if (!m_cmd) return;

	// Acquire swapchain texture
	SDL_WaitAndAcquireGPUSwapchainTexture(m_cmd, m_window,
		&m_swapchainTex, &m_swW, &m_swH);
	// swapchainTex may be null if window minimized
	m_currentPass = nullptr;
	m_frameFirstTarget = true;  // next SetRenderTarget will clear

	// RenderDoc capture: captures the next present atomically
	if (m_captureThisFrame && m_rdoc) {
		m_rdoc->TriggerCapture();
		m_captureThisFrame = false;
		__android_log_print(ANDROID_LOG_INFO, TAG, "RenderDoc: capture triggered");
	}
}

//------------------------------------------------------------------------------
//------------------------------------------------------------------------------
void TVPRenderManager_GPU::_EndFramePass() {
	if (m_currentPass) {
		SDL_EndGPURenderPass(m_currentPass);
		m_currentPass = nullptr;
	}
}

//------------------------------------------------------------------------------
// ReadbackPixel — synchronous GPU pixel readback for diagnostics
//------------------------------------------------------------------------------

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
