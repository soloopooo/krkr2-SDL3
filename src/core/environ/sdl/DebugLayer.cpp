#include "DebugLayer.h"
#include "font_8x8.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#ifdef __ANDROID__
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <android/log.h>
#endif
#include <cstdio>
#include <cstring>
#include <algorithm>
// iTVPTexture2D forward-declared in DebugLayer.h — full definition via RenderManager_gpu.h
#include "WindowLayer_sdl.h"
#include "DebugIntf.h"
#include "ConfigManager/GlobalConfigManager.h"
#include "../gpu/RenderManager_gpu.h"
#include "TVPSDL.h"

bool g_debugAutoCapture = false;

// Forward decl for utility function
static SDL_GPUTexture* PrepareDestReadTarget(SDL_GPUDevice* dev,
	SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* src, int w, int h);

TVPDebugLayer* TVPDebugLayer::s_instance = nullptr;

TVPDebugLayer::TVPDebugLayer() {
	s_instance = this;
}

TVPDebugLayer::~TVPDebugLayer() {
	Shutdown();
	s_instance = nullptr;
}

TVPDebugLayer* TVPDebugLayer::Instance() {
	if (!s_instance) {
		auto* layer = new TVPDebugLayer();
		layer->Init();
	}
	return s_instance;
}

void TVPDebugLayer::Init() {
	m_outDir = "/sdcard/Download/krkr2_debug";
	m_seqNum = 0;
	m_replayW = 1280;
	m_replayH = 720;
	// Read config
	auto* cfg = GlobalConfigManager::GetInstance();
	int enabled = cfg->GetValue<int>("debug_layer", 0);
	if (!enabled) return; // disabled by default
	int port = cfg->GetValue<int>("debug_tcp_port", 9999);
	StartServer(port);
	float rate = cfg->GetValue<float>("slow_mo_rate", 1.0f);
	if (rate < 1.0f) { m_mode = SLOWMO; m_slowMoRate = rate; }
}

void TVPDebugLayer::Shutdown() {
	StopServer();
	if (m_replayTarget) {
		// Note: can't release here without device ref; Release called from FrameEnd
	}
}

// ==========================
// hooks
// ==========================

void TVPDebugLayer::OnFrameBegin() {
	if (m_mode == STEP) {
		if (m_recording) {
			// This is the step frame — block until step is requested
			// but only if recording hasn't started yet (first step after entering step mode)
			// Actually, recording is set by Step(), so we don't block here.
			// The recording flag is set before OnFrameBegin is called.
		}
	} else if (m_mode == DRAW_CALL_NAV) {
		m_recording = false;
	}
}

void TVPDebugLayer::OnFrameEnd() {
	m_frameCount++;

	if (m_recording) {
		EndRecording();
		m_recording = false;
		m_mode = DRAW_CALL_NAV;
		m_navIdx = 0;
		// Auto-capture
		if (g_debugAutoCapture) CaptureFrame();
	}

	// Replay for nav mode
	if (m_mode == DRAW_CALL_NAV && m_navIdx >= 0 && !m_calls.empty()) {
		ReplayDrawCalls();
	}
}

int TVPDebugLayer::OnRateLimit(int baseTargetUs) {
	if (m_mode == SLOWMO) {
		float rate = m_slowMoRate;
		if (rate < 0.01f) rate = 0.01f;
		return (int)(baseTargetUs / rate);
	}
	if (m_mode == STEP || m_mode == DRAW_CALL_NAV) {
		return 100000; // 100ms pause between frames in debug modes
	}
	return baseTargetUs;
}

// ==========================
// Recording
// ==========================

void TVPDebugLayer::BeginRecording() {
	m_calls.clear();
	m_recording = true;
	m_navIdx = -1;
}

void TVPDebugLayer::RecordDrawCall(const TVPDrawCallInfo& info) {
	if (!m_recording) return;
	m_calls.push_back(info);
}

void TVPDebugLayer::EndRecording() {
	m_recording = false;
}

// ==========================
// Controls
// ==========================

void TVPDebugLayer::SetMode(Mode m) {
	m_mode = m;
	if (m != STEP) {
		m_recording = false;
	}
	if (m != DRAW_CALL_NAV) {
		m_navIdx = -1;
	}
}

void TVPDebugLayer::Step() {
	if (m_mode == STEP) {
		m_recording = true;
		// No blocking needed — OnFrameBegin checks recording flag
	}
}

void TVPDebugLayer::SetSlowMoRate(float rate) {
	m_slowMoRate = std::max(0.01f, std::min(1.0f, rate));
	m_mode = SLOWMO;
}

void TVPDebugLayer::NavNext() {
	if (m_calls.empty()) return;
	int idx = m_navIdx.load();
	if (idx < (int)m_calls.size() - 1) {
		m_navIdx = idx + 1;
	} else {
		// Wrap to final (show all)
		m_navIdx = -1;
	}
}

void TVPDebugLayer::NavPrev() {
	if (m_calls.empty()) return;
	int idx = m_navIdx.load();
	if (idx < 0) {
		// from final go to last call
		m_navIdx = (int)m_calls.size() - 1;
	} else if (idx > 0) {
		m_navIdx = idx - 1;
	}
}

void TVPDebugLayer::NavTo(int idx) {
	if (idx < 0) { m_navIdx = -1; return; }
	if (idx >= (int)m_calls.size()) { m_navIdx = (int)m_calls.size() - 1; return; }
	m_navIdx = idx;
}

const TVPDrawCallInfo* TVPDebugLayer::GetCurrentCall() const {
	int idx = m_navIdx;
	if (idx < 0 || idx >= (int)m_calls.size()) return nullptr;
	return &m_calls[idx];
}

// ==========================
// Replay
// ==========================

void TVPDebugLayer::ReplayDrawCalls() {
	if (m_calls.empty()) return;
	auto* gpu = TVPRenderManager_GPU::Instance();
	if (!gpu) return;
	SDL_GPUDevice* dev = gpu->GetDevice();
	if (!dev) return;
	SDL_GPUCommandBuffer* cmd = TVPRenderManager_GPU::CurrentCmd();
	if (!cmd) return;

	m_replayDev = dev;

	// Create replay target if needed
	int rw = m_replayW, rh = m_replayH;
	if (!m_replayTarget) {
		SDL_GPUTextureCreateInfo ti = {};
		ti.type = SDL_GPU_TEXTURETYPE_2D;
		ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
		ti.width = (Uint32)rw; ti.height = (Uint32)rh;
		ti.layer_count_or_depth = 1;
		ti.num_levels = 1;
		ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
		m_replayTarget = SDL_CreateGPUTexture(dev, &ti);
		if (!m_replayTarget) {
			return;
		}
	}

	// Clear the replay target and sequentially render each draw call
	int upTo = m_navIdx; // -1 = all
	if (upTo < 0 || upTo >= (int)m_calls.size()) upTo = (int)m_calls.size() - 1;

	SDL_GPUColorTargetInfo tg = {};
	tg.texture = m_replayTarget;
	tg.load_op = SDL_GPU_LOADOP_CLEAR;
	tg.store_op = SDL_GPU_STOREOP_STORE;
	tg.clear_color = {0, 0, 0, 0};
	SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &tg, 1, NULL);
	if (!rp) return;

	for (int i = 0; i <= upTo; i++) {
		auto& dc = m_calls[i];
		auto* method = (iTVPRenderMethod*)gpu->GetRenderMethod(dc.methodName.c_str());
		if (!method) continue;
		auto* gpuMethod = dynamic_cast<tTVPGPURenderMethod*>(method);
		if (!gpuMethod || !gpuMethod->GetPipeline()) continue;

		// Must flush pass if we need dest-read copy
		if (dc.needsDestRead) {
			// End this pass, copy target → temp, begin new pass with LOAD
			SDL_EndGPURenderPass(rp);
			SDL_GPUTexture* tempTex = PrepareDestReadTarget(dev, cmd, m_replayTarget, rw, rh);
			tg.load_op = SDL_GPU_LOADOP_LOAD;
			rp = SDL_BeginGPURenderPass(cmd, &tg, 1, NULL);
			if (!rp) return;
		}

		SDL_GPUViewport vp = {
			(float)dc.rect.left, (float)dc.rect.top,
			(float)dc.rect.get_width(), (float)dc.rect.get_height(),
			0, 1
		};
		SDL_SetGPUViewport(rp, &vp);
		SDL_BindGPUGraphicsPipeline(rp, gpuMethod->GetPipeline());

		// Use shared quad vertex buffer
		auto* verts = gpu->GetQuadVerts();
		if (verts) {
			SDL_GPUBufferBinding bb = { verts, 0 };
			SDL_BindGPUVertexBuffers(rp, 0, &bb, 1);
		}

		// Push UBO
		uint8_t uboPush[64] = {};
		float* uvF = (float*)uboPush;
		uvF[0] = dc.uvOffset[0];
		uvF[1] = dc.uvOffset[1];
		uvF[2] = dc.uvScale[0];
		uvF[3] = dc.uvScale[1];
		uvF[4] = dc.opacity;
		uvF[5] = 0; // pad
		int uboSize = 24;
		// crossfade: 2nd UV
		if (dc.methodName.find("ConstAlphaBlend_SD") != std::string::npos ||
		    dc.methodName.find("ConstColorAlphaBlend_SD") != std::string::npos) {
			uvF[6] = dc.uvOffset1[0];
			uvF[7] = dc.uvOffset1[1];
			uvF[8] = dc.uvScale1[0];
			uvF[9] = dc.uvScale1[1];
			uboSize = 40;
		}
		SDL_PushGPUFragmentUniformData(cmd, 0, uboPush, (Uint32)uboSize);

		// binding=1 UBO (text_color)
		if (dc.hasColorUBO) {
			SDL_PushGPUFragmentUniformData(cmd, 1, dc.colorUBO, 16);
		}

		// Bind source textures — skip call if any required texture is unavailable
		SDL_GPUTextureSamplerBinding tsBind[8] = {};
		int nValid = 0;
		int nBind = (int)dc.sources.size();
		if (nBind > 8) nBind = 8;
		auto* sampler = gpu->GetSampler();
		for (int si = 0; si < nBind; si++) {
			auto* src = dynamic_cast<tTVPGPUTexture2D*>(dc.sources[si].tex);
			if (src && src->GetGPUTexture()) {
				tsBind[si].texture = src->GetGPUTexture();
				tsBind[si].sampler = sampler;
				nValid++;
			}
		}
		// If method expects textures but none are valid, skip this draw call
		if (nBind > 0 && nValid != nBind) {
			// Missing textures — skip (textures may have been released since recording)
			continue;
		}
		if (nBind > 0)
			SDL_BindGPUFragmentSamplers(rp, 0, tsBind, nBind);

		SDL_DrawGPUPrimitives(rp, 6, 1, 0, 0);
	}
	SDL_EndGPURenderPass(rp);
}

// ==========================
// TCP Command Server
// ==========================

void TVPDebugLayer::ServerThreadFunc() {
#ifdef __ANDROID__
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return;
	int opt = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	struct sockaddr_in addr = {};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons(m_srvPort);
	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return; }
	if (listen(fd, 1) < 0) { close(fd); return; }
	// Non-blocking accept for clean shutdown
	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	while (m_srvRunning) {
		struct sockaddr_in client;
		socklen_t clen = sizeof(client);
		int cfd = accept(fd, (struct sockaddr*)&client, &clen);
		if (cfd < 0) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}
		// Read command
		uint8_t cmd;
		if (read(cfd, &cmd, 1) != 1) { close(cfd); continue; }
		switch (cmd) {
		case 0x01: // STEP
			SetMode(STEP);
			Step();
			write(cfd, &cmd, 1);
			break;
		case 0x02: { // SET_MODE
			uint8_t m;
			if (read(cfd, &m, 1) == 1) SetMode((Mode)m);
			write(cfd, &cmd, 1);
			break;
		}
		case 0x03: { // SET_RATE
			float rate;
			if (read(cfd, &rate, 4) == 4) SetSlowMoRate(rate);
			write(cfd, &cmd, 1);
			break;
		}
		case 0x04: // CAPTURE
			CaptureFrame();
			write(cfd, &cmd, 1);
			break;
		case 0x05: // NAV_NEXT
			NavNext();
			write(cfd, &cmd, 1);
			break;
		case 0x06: // NAV_PREV
			NavPrev();
			write(cfd, &cmd, 1);
			break;
		case 0x07: { // NAV_SET
			int idx;
			if (read(cfd, &idx, 4) == 4) NavTo(idx);
			write(cfd, &cmd, 1);
			break;
		}
		case 0x09: // GET_STATUS
		{
			uint8_t status[32] = {};
			*(int*)(status+0) = m_frameCount;
			int cc = (int)m_calls.size();
			*(int*)(status+4) = cc;
			*(int*)(status+8) = m_navIdx;
			status[12] = (uint8_t)m_mode;
			write(cfd, status, 32);
			break;
		}
		case 0x0A: // GET_DRAW_CALL_INFO
		{
			int idx;
			if (read(cfd, &idx, 4) == 4 && idx >= 0 && idx < (int)m_calls.size()) {
				auto& dc = m_calls[idx];
				int len = (int)dc.methodName.size();
				uint8_t buf[1024];
				*(int*)buf = len;
				memcpy(buf+4, dc.methodName.data(), len);
				*(int*)(buf+4+len) = dc.rect.left;
				*(int*)(buf+8+len) = dc.rect.top;
				*(int*)(buf+12+len) = dc.rect.get_width();
				*(int*)(buf+16+len) = dc.rect.get_height();
				write(cfd, buf, 20+len);
			}
			break;
		}
		default:
			break;
		}
		close(cfd);
	}
	close(fd);
#endif
}

void TVPDebugLayer::StartServer(int port) {
	if (m_srvRunning) return;
	m_srvPort = port;
	m_srvRunning = true;
	m_srvThread = std::thread(&TVPDebugLayer::ServerThreadFunc, this);
	m_srvThread.detach();
}

void TVPDebugLayer::StopServer() {
	m_srvRunning = false;
	if (m_srvThread.joinable())
		m_srvThread.join();
}

// ==========================
// Texture capture
// ==========================

static void WriteBMP(const char* path, const uint32_t* pixels, int w, int h) {
	if (!pixels || w <= 0 || h <= 0) return;
	int pitch = w * 4;
	int dataSize = pitch * h;
	int bmpSize = 14 + 40 + dataSize;
	auto* bmp = new uint8_t[bmpSize];
	bmp[0] = 'B'; bmp[1] = 'M';
	*(uint32_t*)(bmp+2) = bmpSize;
	*(uint32_t*)(bmp+10) = 14 + 40;
	*(uint32_t*)(bmp+14) = 40;
	*(int32_t*)(bmp+18) = w;
	*(int32_t*)(bmp+22) = -h; // top-down
	*(uint16_t*)(bmp+26) = 1;
	*(uint16_t*)(bmp+28) = 32;
	*(uint32_t*)(bmp+30) = 0;
	*(uint32_t*)(bmp+34) = dataSize;
	auto* dst = bmp + 54;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			uint32_t px = pixels[y * w + x];
			dst[0] = (uint8_t)(px >> 16);
			dst[1] = (uint8_t)(px >> 8);
			dst[2] = (uint8_t)(px);
			dst[3] = (uint8_t)(px >> 24);
			dst += 4;
		}
	}
	FILE* f = fopen(path, "wb");
	if (f) { fwrite(bmp, 1, bmpSize, f); fclose(f); }
	delete[] bmp;
}

void TVPDebugLayer::CaptureFrame() {
	char dir[256];
	snprintf(dir, sizeof(dir), "%s/frame_%04d", m_outDir.c_str(), m_seqNum++);
	// Platform support for dir creation
#ifdef __ANDROID__
	mkdir(dir, 0755);
	__android_log_print(ANDROID_LOG_INFO, "DebugLayer", "CAPTURE: %s", dir);
#endif
	// Try reading from renderer framebuffer result
	int w = 0, h = 0;
	auto* gpu = TVPRenderManager_GPU::Instance();
	const uint8_t* pixels = gpu ? gpu->GetFramePixels(w, h) : nullptr;
	if (pixels && w > 0 && h > 0) {
		char path[256];
		snprintf(path, sizeof(path), "%s/final.bmp", dir);
		WriteBMP(path, (const uint32_t*)pixels, w, h);
	} else if (g_gameW > 0 && g_gameH > 0) {
		// Software path: read from draw buffers (not implemented in initial version)
		char path[256];
		snprintf(path, sizeof(path), "%s/final.bmp", dir);
		// Write a 0-size placeholder
		FILE* f = fopen(path, "wb");
		if (f) fclose(f);
	}
	// Write metadata
	char metaPath[256];
	snprintf(metaPath, sizeof(metaPath), "%s/metadata.txt", dir);
	FILE* f = fopen(metaPath, "w");
	if (f) {
		fprintf(f, "frame=%d\ncalls=%zu\n", m_frameCount, m_calls.size());
		for (size_t i = 0; i < m_calls.size(); i++) {
			auto& dc = m_calls[i];
			fprintf(f, "call[%zu]: %s rect=(%d,%d,%d,%d) opa=%.2f src=%zu tex\n",
				i, dc.methodName.c_str(),
				dc.rect.left, dc.rect.top,
				dc.rect.get_width(), dc.rect.get_height(),
				(double)dc.opacity, dc.sources.size());
		}
		fclose(f);
	}
}

void TVPDebugLayer::SetOutputDir(const std::string& dir) {
	m_outDir = dir;
}

// ==========================
// Utility: dest-read temp texture for replay
// ==========================

static SDL_GPUTexture* PrepareDestReadTarget(SDL_GPUDevice* dev,
	SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* src, int w, int h)
{
	static SDL_GPUTexture* s_temp = nullptr;
	static int s_tempW = 0, s_tempH = 0;
	if (!s_temp || s_tempW != w || s_tempH != h) {
		if (s_temp) SDL_ReleaseGPUTexture(dev, s_temp);
		SDL_GPUTextureCreateInfo ti = {};
		ti.type = SDL_GPU_TEXTURETYPE_2D;
		ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		ti.width = (Uint32)w; ti.height = (Uint32)h;
		ti.layer_count_or_depth = 1; ti.num_levels = 1;
		ti.sample_count = SDL_GPU_SAMPLECOUNT_1;
		s_temp = SDL_CreateGPUTexture(dev, &ti);
		s_tempW = w; s_tempH = h;
	}
	if (s_temp && src) {
		SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
		if (cp) {
			SDL_GPUTextureLocation srcLoc = { src, 0, 0, 0, 0, 0 };
			SDL_GPUTextureLocation dstLoc = { s_temp, 0, 0, 0, 0, 0 };
			SDL_CopyGPUTextureToTexture(cp, &srcLoc, &dstLoc, (Uint32)w, (Uint32)h, 1, false);
			SDL_EndGPUCopyPass(cp);
		}
	}
	return s_temp;
}
