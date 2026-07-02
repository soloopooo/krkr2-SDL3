// Renderer selection — now controlled at RUNTIME via Intent/preference.
//
// The display backend is chosen by KR2Activity.onCreate() which reads the
// "renderer" key from GlobalPreference.xml (set by SettingsActivity) and
// passes it to the native side via nativeSetDisplayBackend().
//
// "vulkan" = Vulkan GPU display (SDL_ClaimWindowForGPUDevice, full GPU compositing)
// "sdl"    = SDL_Renderer display (software compositing)
//
// This compile-time flag is NO LONGER USED to control behavior. Both backends
// are always compiled in. The flag is kept for future use (e.g., build-only
// variants that exclude one backend to reduce .so size).
//
// The display backend cannot be switched at runtime without restarting the app,
// because the same window can only be claimed by one Vulkan backend at a time.
#define KRKR2_RENDERER_GPU
