#pragma once
#include <SDL3/SDL_gpu.h>
#include <vector>
#include <unordered_map>
#include <string>
#include <cstdint>
#include "RenderManager.h"

class tTVPGPUTexture2D : public iTVPTexture2D {
	friend class TVPRenderManager_GPU;
	std::vector<uint8_t> m_pixels;
	SDL_GPUTexture *m_gpuTex = nullptr;
	int m_pitch = 0;
	bool m_cpuDirty = false;
	bool m_gpuDirty = false;
	TVPTextureFormat::e m_format = TVPTextureFormat::None;
	bool m_opaque = false;
	static std::atomic<uint64_t> s_gpuTotalVMem;
public:
	tTVPGPUTexture2D(int w, int h, TVPTextureFormat::e fmt, bool opaque, const void *pixels = nullptr, int pitch = 0);
	~tTVPGPUTexture2D() override;

	TVPTextureFormat::e GetFormat() const override { return m_format; }
	const void * GetScanLineForRead(tjs_uint l) override;
	void * GetScanLineForWrite(tjs_uint l) override;
	tjs_int GetPitch() const override { return m_pitch; }

	void Update(const void *pixel, TVPTextureFormat::e format, int pitch, const tTVPRect& rc) override;
	uint32_t GetPoint(int x, int y) override;
	void SetPoint(int x, int y, uint32_t clr) override;
	bool IsStatic() override { return false; }
	bool IsOpaque() override { return m_opaque; }
	void SetOpaque(bool v) { m_opaque = v; }
	cocos2d::Texture2D* GetAdapterTexture(cocos2d::Texture2D* origTex) override { return nullptr; }

	void UploadToGPU(SDL_GPUDevice *dev);
	void DownloadFromGPU(SDL_GPUDevice *dev);
	SDL_GPUTexture* GetGPUTexture() { return m_gpuTex; }
};

struct BlendConfig {
	bool enable = true;
	SDL_GPUBlendFactor srcColor = SDL_GPU_BLENDFACTOR_ONE;
	SDL_GPUBlendFactor dstColor = SDL_GPU_BLENDFACTOR_ZERO;
	SDL_GPUBlendOp colorOp = SDL_GPU_BLENDOP_ADD;
	SDL_GPUBlendFactor srcAlpha = SDL_GPU_BLENDFACTOR_ONE;
	SDL_GPUBlendFactor dstAlpha = SDL_GPU_BLENDFACTOR_ZERO;
	SDL_GPUBlendOp alphaOp = SDL_GPU_BLENDOP_ADD;
};

class tTVPGPURenderMethod : public iTVPRenderMethod {
public:
	SDL_GPUShader *m_fs = nullptr;
	int m_numTextures = 1;
	int m_numUBOs = 1;
	BlendConfig m_blend;
	mutable SDL_GPUGraphicsPipeline *m_pipeline = nullptr;

	tTVPGPURenderMethod(SDL_GPUShader *fs, int tex, int ubos, const BlendConfig &b)
		: m_fs(fs), m_numTextures(tex), m_numUBOs(ubos), m_blend(b) {}
	SDL_GPUGraphicsPipeline* GetPipeline() { return m_pipeline; }
};

class TVPRenderManager_GPU : public iTVPRenderManager {
public:
	// Static resources
	static SDL_GPUDevice *s_device;
	static SDL_Window *s_window;
	static SDL_GPUShader *s_vs;
	static SDL_GPUBuffer *s_quadVerts;
	static SDL_GPUSampler *s_sampler;
	static SDL_GPUGraphicsPipeline *s_presentPipe;
	static SDL_GPUGraphicsPipeline *s_displayPipe;

	// Fragment shaders (created from display_spv.h)
	static SDL_GPUShader *s_quad_fs;
	static SDL_GPUShader *s_quad_pma_fs;
	static SDL_GPUShader *s_present_fs;
	static SDL_GPUShader *s_fill_fs;
	static SDL_GPUShader *s_fill_mask_fs;
	static SDL_GPUShader *s_copy_color_fs;
	static SDL_GPUShader *s_copy_mask_fs;
	static SDL_GPUShader *s_copy_opaque_fs;
	static SDL_GPUShader *s_remove_opacity_fs;
	static SDL_GPUShader *s_gray_fs;
	static SDL_GPUShader *s_blur_fs;
	static SDL_GPUShader *s_gamma_fs;
	static SDL_GPUShader *s_adjust_gamma_fs;
	static SDL_GPUShader *s_crossfade_fs;
	static SDL_GPUShader *s_univ_trans_fs;
	static SDL_GPUShader *s_alpha_blend_d_fs;
	static SDL_GPUShader *s_const_alpha_blend_d_fs;
	static SDL_GPUShader *s_const_color_alpha_blend_d_fs;
	static SDL_GPUShader *s_apply_colormap_fs;
	static SDL_GPUShader *s_apply_colormap_a_fs;
	static SDL_GPUShader *s_apply_colormap_d_fs;
	static SDL_GPUShader *s_ps_overlay_fs;
	static SDL_GPUShader *s_ps_hardlight_fs;
	static SDL_GPUShader *s_ps_softlight_fs;
	static SDL_GPUShader *s_ps_colordodge_fs;
	static SDL_GPUShader *s_ps_colorburn_fs;
	static SDL_GPUShader *s_ps_diff_fs;
	static SDL_GPUShader *s_ps_exclusion_fs;
	static SDL_GPUShader *s_ps_lighten_fs;
	static SDL_GPUShader *s_ps_darken_fs;

	// Temp texture for dest-read
	static SDL_GPUTexture *s_tempDest;
	static int s_tempW, s_tempH;

	// Primary render target (intermediate for layer compositing)
	static SDL_GPUTexture *s_layerTex;
	static int s_layerW, s_layerH;

	// Command buffer currently accumulating
	static SDL_GPUCommandBuffer *s_cmd;

	static std::unordered_map<uint64_t, SDL_GPUGraphicsPipeline*> s_pipeCache;

	static bool s_initialized;
	static TVPRenderManager_GPU *s_instance;

	TVPRenderManager_GPU();
	~TVPRenderManager_GPU() override;

	static bool InitDevice(SDL_Window *window);
	static void Shutdown();
	static bool IsDeviceReady() { return s_device != nullptr; }
	static TVPRenderManager_GPU *Instance() { return s_instance; }

	// iTVPRenderManager
	const char *GetName() override { return "gpu"; }
	bool IsSoftware() override { return false; }
	bool GetRenderStat(unsigned int &drawCount, uint64_t &vmemsize) override;
	void Initialize();

	iTVPTexture2D* CreateTexture2D(const void *pixel, int pitch,
		unsigned int w, unsigned int h, TVPTextureFormat::e format,
		int flags = RENDER_CREATE_TEXTURE_FLAG_ANY) override;
	iTVPTexture2D* CreateTexture2D(tTVPBitmap* bmp) override;
	iTVPTexture2D* CreateTexture2D(TJS::tTJSBinaryStream* s) override;
	iTVPTexture2D* CreateTexture2D(unsigned int neww, unsigned int newh,
		iTVPTexture2D* tex) override;

	void OperateRect(iTVPRenderMethod* method, iTVPTexture2D *tar,
		iTVPTexture2D *reftar, const tTVPRect& rctar,
		const tRenderTexRectArray &textures) override;
	void OperateTriangles(iTVPRenderMethod* method, int nTriangles,
		iTVPTexture2D *target, iTVPTexture2D *reftar,
		const tTVPRect& rcclip, const tTVPPointD* pttar,
		const tRenderTexQuadArray &textures) override;
	void OperatePerspective(iTVPRenderMethod* method, int nQuads,
		iTVPTexture2D *target, iTVPTexture2D *reftar,
		const tTVPRect& rcclip, const tTVPPointD* pttar,
		const tRenderTexQuadArray &textures) override;

	// Frame lifecycle
	void BeginFrame();
	void EndFrame();
	void PresentFrame(const uint8_t *pixels, int w, int h);
	const uint8_t* GetFramePixels(int &w, int &h) { w = 0; h = 0; return nullptr; }
	void ResetFrameResult() {}

	// DebugLayer accessors
	SDL_GPUDevice* GetDevice() { return s_device; }
	static SDL_GPUCommandBuffer* CurrentCmd() { return s_cmd; }
	SDL_GPUBuffer* GetQuadVerts() { return s_quadVerts; }
	SDL_GPUSampler* GetSampler() { return s_sampler; }

	// Helpers
	static SDL_GPUGraphicsPipeline* _GetOrCreatePipeline(
		SDL_GPUShader *fs, const BlendConfig &blend,
		SDL_GPUTextureFormat rtFmt, int numTextures, int numUBOs);
	static void _DrawQuad(tTVPGPURenderMethod *method,
		iTVPTexture2D *tar, const tTVPRect &rctar,
		const tRenderTexRectArray &textures,
		float fillColor[4] = nullptr,
		float opacity = 1.0f,
		const float *uv = nullptr);
	static void _DrawQuadRaw(SDL_GPUGraphicsPipeline *pipe,
		SDL_GPUTexture *tex0, SDL_GPUTexture *tex1, SDL_GPUTexture *tex2,
		int w, int h, int numTextures, int numUBOs,
		const float *uboData, int uboSize,
		const float *uboData2, int uboSize2);
	static void _EnsureDestRead(iTVPTexture2D *tar, const tTVPRect &rctar);
	void RegisterMethods();
};

extern "C" void TVPRegisterGPURenderer();
