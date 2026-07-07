#pragma once
#include <atomic>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include "tjsCommHead.h"
#include "visual/ComplexRect.h"

struct SDL_GPUTexture;
struct SDL_GPUDevice;
struct SDL_GPUSampler;
struct SDL_GPUBuffer;
struct SDL_GPUCommandBuffer;
struct SDL_GPURenderPass;
struct SDL_GPUGraphicsPipeline;

class iTVPTexture2D;
class iTVPRenderMethod;
class tTVPGPUTexture2D;

// Per-draw-call info recorded during capture
struct TVPDrawCallInfo {
	std::string methodName;  // e.g. "AlphaBlend_d"
	tTVPRect rect;
	float opacity;           // 0..1
	float uvOffset[2], uvScale[2]; // tex0 UV
	float uvOffset1[2], uvScale1[2]; // tex1 UV (crossfade)
	bool hasColorUBO;
	float colorUBO[4];
	bool needsDestRead;

	struct Source {
		iTVPTexture2D* tex = nullptr;
	};
	std::vector<Source> sources;
	iTVPTexture2D* target = nullptr;
};

// Debug layer singleton: step/slowmo, draw-call recording/replay,
// texture capture, TCP command server, overlay rendering.
class TVPDebugLayer {
public:
	enum Mode { NORMAL, STEP, SLOWMO, DRAW_CALL_NAV };

	static TVPDebugLayer* Instance();
	void Init();
	void Shutdown();

	// --- Hooks called from TVPEngineTick ---
	void OnFrameBegin();
	void OnFrameEnd();
	// Returns adjusted targetUs (microseconds) for frame rate limiting
	int OnRateLimit(int baseTargetUs);

	// --- Draw call recording (called from OperateRect in RenderManager_GPU) ---
	void BeginRecording();
	void RecordDrawCall(const TVPDrawCallInfo& info);
	int GetDrawCallCount() const { return (int)m_calls.size(); }
	void EndRecording();
	bool IsRecording() const { return m_recording; }

	// --- Controls (from TCP / JNI / keyboard) ---
	void SetMode(Mode m);
	Mode GetMode() const { return m_mode; }
	void Step();    // release one frame in step mode
	void SetSlowMoRate(float rate);

	// --- Navigation (DRAW_CALL_NAV mode) ---
	int GetNavIndex() const { return m_navIdx; }
	int GetCallCount() const { return (int)m_calls.size(); }
	void NavNext();
	void NavPrev();
	void NavTo(int idx);
	const TVPDrawCallInfo* GetCurrentCall() const;

	// --- Replay ---
	// Replays draw calls 0..navIdx onto the debug render target.
	// Caller must ensure a valid GPU context (inside BeginFrame/EndFrame).
	void ReplayDrawCalls();
	SDL_GPUTexture* GetReplayTarget() const { return m_replayTarget; }
	int GetReplayW() const { return m_replayW; }
	int GetReplayH() const { return m_replayH; }

	// --- RenderDoc capture ---
	void TriggerRDOCapture();
	bool IsRDOCapturePending() const { return m_pendingRDocCapture; }

	// --- TCP command server ---
	void StartServer(int port);
	void StopServer();
	bool IsServerRunning() const { return m_srvRunning; }

	// --- Texture capture (save frame to disk) ---
	void CaptureFrame();
	void SetOutputDir(const std::string& dir);

	// --- Overlay rendering (called after game content on swapchain) ---
	void RenderOverlay(SDL_GPURenderPass* rp,
		SDL_GPUDevice* dev, SDL_GPUSampler* sampler,
		SDL_GPUBuffer* quadVerts,
		int swW, int swH);

private:
	TVPDebugLayer();
	~TVPDebugLayer();
	static TVPDebugLayer* s_instance;

	// Step mode
	std::mutex m_stepMtx;
	std::condition_variable m_stepCV;
	bool m_stepRequested = false;
	std::atomic<Mode> m_mode{NORMAL};

	// Slow-mo
	std::atomic<float> m_slowMoRate{1.0f};

	// Recording
	bool m_recording = false;
	std::vector<TVPDrawCallInfo> m_calls;

	// Navigation
	std::atomic<int> m_navIdx{-1};  // -1 = nav inactive

	// TCP server
	std::thread m_srvThread;
	std::atomic<bool> m_srvRunning{false};
	int m_srvPort = 9999;
	void ServerThreadFunc();

	// Capture
	std::string m_outDir;
	int m_seqNum = 0;

	// RenderDoc
	std::atomic<bool> m_pendingRDocCapture{false};

	// Replay
	SDL_GPUDevice* m_replayDev = nullptr;
	SDL_GPUTexture* m_replayTarget = nullptr;
	int m_replayW = 0, m_replayH = 0;
	SDL_GPUGraphicsPipeline* m_replayPipe = nullptr; // overlay text pipeline

	// Frame counter
	int m_frameCount = 0;
};

// Debug mode control (declared for JNI access)
extern bool g_debugAutoCapture;
