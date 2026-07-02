#pragma once
#include <SDL3/SDL_gpu.h>
#include <atomic>
#include <vector>
#include <map>
#include <unordered_map>
#include "RenderManager.h"

class tTJSNI_Window;

//------------------------------------------------------------------------------
// GPU display mode
//------------------------------------------------------------------------------
enum class DisplayMode {
	SOFTWARE,
	VULKAN
};
extern DisplayMode g_displayMode;
extern SDL_Window *g_window;

//------------------------------------------------------------------------------
// tTVPGPUTexture2D — wraps SDL_GPUTexture as iTVPTexture2D
//------------------------------------------------------------------------------
class tTVPGPUTexture2D : public iTVPTexture2D {
	SDL_GPUDevice *m_device;
	SDL_GPUTexture *m_texture;
	int m_texW, m_texH;
	int m_bmpW, m_bmpH;
	TVPTextureFormat::e m_format;
	bool m_opaque;
	// CPU-side pixel buffer for GetScanLineForRead/Write (software compat)
	std::vector<uint8_t> m_pixels;
	int m_width, m_height, m_pitch;
	bool m_needsInit = true; // clear on first render-pass use
public:
	tTVPGPUTexture2D(SDL_GPUDevice *dev, SDL_GPUTexture *tex,
		int texW, int texH, int w, int h,
		TVPTextureFormat::e fmt, bool opaque);
	~tTVPGPUTexture2D() override;

	SDL_GPUTexture *GetGPUTexture() const { return m_texture; }

	TVPTextureFormat::e GetFormat() const override { return m_format; }
	const void * GetScanLineForRead(tjs_uint l) override;
	void * GetScanLineForWrite(tjs_uint l) override;
	void Update(const void *pixel, TVPTextureFormat::e format, int pitch, const tTVPRect& rc) override;
	uint32_t GetPoint(int x, int y) override;
	void SetPoint(int x, int y, uint32_t clr) override;
	bool IsStatic() override { return false; }
	bool IsOpaque() override { return m_opaque; }
	cocos2d::Texture2D* GetAdapterTexture(cocos2d::Texture2D* origTex) override { return nullptr; }
	bool GetScale(float &x, float &y) override { x = 1.f; y = 1.f; return true; }
	bool NeedsInit() const { return m_needsInit; }
	void SetInitialized() { m_needsInit = false; }
};

//------------------------------------------------------------------------------
// tTVPGPURenderMethod — wraps SDL_GPUGraphicsPipeline with blend state
//------------------------------------------------------------------------------
class tTVPGPURenderMethod : public iTVPRenderMethod {
	SDL_GPUDevice *m_device;
	SDL_GPUGraphicsPipeline *m_pipeline;
	SDL_GPUShader *m_vertShader; // kept alive for reuse
	SDL_GPUShader *m_fragShader;
	float m_constColor[4];
	bool m_hasConstantColor;
	SDL_GPUBlendFactor m_srcColor, m_dstColor, m_srcAlpha, m_dstAlpha;
	SDL_GPUBlendOp m_colorOp, m_alphaOp;
	bool m_blendEnabled;
	int m_numTextures;

	// UBO data for shaders with uniforms
	uint8_t m_uboData[256];
	int m_uboSize = 0;
	bool m_uboDirty = false;

	friend class TVPRenderManager_GPU;
	float m_vague = 0;
	int m_uboID_Vague = -1;
	int m_uboID_Phase = -1;
	int m_opacity = 255;

public:
	tTVPGPURenderMethod(SDL_GPUDevice *dev,
		SDL_GPUGraphicsPipeline *pipe,
		SDL_GPUShader *vs, SDL_GPUShader *fs,
		const char *name,
		bool blend, int numTex,
		SDL_GPUBlendFactor srcC, SDL_GPUBlendFactor dstC,
		SDL_GPUBlendFactor srcA, SDL_GPUBlendFactor dstA,
		SDL_GPUBlendOp cOp = SDL_GPU_BLENDOP_ADD,
		SDL_GPUBlendOp aOp = SDL_GPU_BLENDOP_ADD);
	~tTVPGPURenderMethod() override;

	SDL_GPUGraphicsPipeline *GetPipeline() const { return m_pipeline; }
	int GetNumTextures() const { return m_numTextures; }
	float GetConstColor(int i) const { return m_constColor[i]; }
	bool HasConstantColor() const { return m_hasConstantColor; }

	void SetParameterFloat(int id, float Value) override;
	void SetParameterInt(int id, int Value) override;
	void SetParameterPtr(int id, const void *v) override;
	void SetParameterColor4B(int id, unsigned int clr) override;
	void SetParameterOpa(int id, int Value) override;
	iTVPRenderMethod* SetBlendFuncSeparate(int func,
		int srcRGB, int dstRGB, int srcAlpha, int dstAlpha) override;

	int EnumParameterID(const char *name) override;
};

//------------------------------------------------------------------------------
// TVPRenderManager_GPU — iTVPRenderManager using SDL_Gpu (Vulkan)
//------------------------------------------------------------------------------
class TVPRenderManager_GPU : public iTVPRenderManager {
	SDL_GPUDevice *m_device = nullptr;
	SDL_Window *m_window = nullptr;
	SDL_GPUSampler *m_sampler = nullptr;

	// Shared shaders (from SPIR-V)
	SDL_GPUShader *m_vs = nullptr; // textured quad vertex
	SDL_GPUShader *m_fs = nullptr; // textured quad fragment

	// Quad vertex buffer (fullscreen quad)
	SDL_GPUBuffer *m_quadVerts = nullptr;

	// Frame state
	SDL_GPUCommandBuffer *m_cmd = nullptr;
	SDL_GPUTexture *m_swapchainTex = nullptr;
	SDL_GPURenderPass *m_currentPass = nullptr;
	iTVPTexture2D *m_currentTarget = nullptr;
	iTVPTexture2D *m_displayTarget = nullptr; // DrawBuffer texture for present
	SDL_GPUTexture *m_fallbackTex = nullptr;  // CPU upload fallback texture
	int m_fallbackW = 0, m_fallbackH = 0;
	Uint32 m_swW = 0, m_swH = 0;

	// Readback state (double-buffered with fence)
	static const int READBACK_SLOTS = 2;
	struct ReadbackSlot {
		SDL_GPUTransferBuffer *tb = nullptr;
		SDL_GPUFence *fence = nullptr;
		int texW = 0, texH = 0;
	};
	ReadbackSlot m_rbSlot[READBACK_SLOTS];
	int m_rbActive = 0; // current slot being filled
	bool m_hasFrameResult = false;
	int m_frameW = 0, m_frameH = 0;
	std::vector<uint8_t> m_framePixels;

	// Pipeline cache
	SDL_GPUGraphicsPipeline *m_quadPipeline = nullptr;
	SDL_GPUGraphicsPipeline *m_presentPipeline = nullptr;
	std::unordered_map<uint32_t, tTVPGPURenderMethod*> m_methodCache;

	// Fullscreen vertex data (2 tris, 6 verts, pos + uv)
	static const float s_quadVerts[24];
	int m_drawCount = 0;
	bool m_frameFirstTarget = true;    // first SetRenderTarget this frame → CLEAR
	SDL_GPUTextureFormat m_swapFormat; // swapchain format
	SDL_GPUTextureFormat m_texFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
	SDL_GPUShader *m_fs_gray = nullptr; // grayscale frag shader
	SDL_GPUShader *m_fs_blur = nullptr; // box blur frag shader
	SDL_GPUShader *m_fs_adjustGamma = nullptr; // AdjustGamma frag shader (UBO)
	SDL_GPUShader *m_fs_univTrans = nullptr; // UnivTransBlend frag shader (3tex+UBO)
	SDL_GPUShader *m_fs_fill = nullptr;      // solid-color fill frag shader
	SDL_GPUShader *m_fs_present = nullptr;   // present (force alpha=1) frag shader
	SDL_GPUShader *m_fs_crossfade = nullptr; // crossfade (2-tex blend) frag shader

	SDL_GPUGraphicsPipeline* _CreateQuadPipeline(
		SDL_GPUTextureFormat format,
		SDL_GPUBlendFactor srcColor, SDL_GPUBlendFactor dstColor,
		SDL_GPUBlendFactor srcAlpha, SDL_GPUBlendFactor dstAlpha,
		SDL_GPUBlendOp colorOp, SDL_GPUBlendOp alphaOp,
		bool blendEnabled,
		SDL_GPUShader *fragShader = nullptr,
		int numSamplers = 1, int numUBO = 0);
	tTVPGPURenderMethod* _GetOrCreateMethod(const char *name);

	void _BeginFrame();
	void _EndFramePass();
	void _PresentToSwapchain();

	static TVPRenderManager_GPU *s_instance;
	static std::atomic<uint64_t> s_totalVMem;
	friend class tTVPGPUTexture2D;

public:
	TVPRenderManager_GPU();
	~TVPRenderManager_GPU() override;

	bool Init(SDL_Window *window);
	void Shutdown();
	static TVPRenderManager_GPU *Instance() { return s_instance; }
	static SDL_GPUCommandBuffer *CurrentCmd() {
		return s_instance ? s_instance->m_cmd : nullptr;
	}
	bool IsReady() const { return m_device != nullptr; }

	// iTVPRenderManager
	const char *GetName() override { return "gpu"; }
	bool IsSoftware() override { return false; }
	bool GetRenderStat(unsigned int &drawCount, uint64_t &vmemsize) override;

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

	void SetRenderTarget(iTVPTexture2D *target) override;
	void BeginStencil(iTVPTexture2D* reftex) override {}
	void EndStencil() override {}

	iTVPRenderMethod* GetRenderMethod(const char *name,
		uint32_t *hint = nullptr) override;

	// Frame lifecycle — called from TVPEngineTick
	void BeginFrame();
	void EndFrame();
	void SetFallbackDisplay(const void *pixels, int w, int h);
	// End current render pass (for texture updates between draws)
	void FlushPass();
	void ReadbackAndPresent(iTVPTexture2D *finalTex);
	const uint8_t* GetFramePixels(int &w, int &h) const {
		if (!m_hasFrameResult) return nullptr;
		w = m_frameW; h = m_frameH;
		return m_framePixels.data();
	}
	void ResetFrameResult() { m_hasFrameResult = false; }

	// Synchronous pixel readback for diagnostics (separate command buffer).
	// Reads pixel at (x,y) from tex. Returns 0 on failure.
	// Only use for debugging — SLOW (stalls GPU).
	uint32_t ReadbackPixel(SDL_GPUTexture *tex, int x, int y);
};

// Registration
extern "C" void TVPRegisterGPURenderer();
