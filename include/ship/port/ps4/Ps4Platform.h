#pragma once
#ifdef __PS4__

#include <stdint.h>

// PS4 (OpenOrbis) platform layer.
//
// Rendering goes through Piglet, Sony's OpenGL ES 2.0 / EGL implementation. The stock SDL2 has no
// PS4 backend, so SDL runs on its dummy video driver and is only used for timing, events and the
// game controller API; the DualShock 4 is exposed to it as an SDL virtual joystick.

namespace Ship {
namespace Ps4 {

constexpr int kDisplayWidth = 1920;
constexpr int kDisplayHeight = 1080;

// Loads the system modules + Piglet and creates the EGL window surface and GLES2 context.
// Returns false (after logging the reason) if anything fails.
// Loads the system modules the port calls into (user service, pad, audio...). Safe to call more
// than once; everything that touches one of them calls this first, since the order in which the
// engine sets up input, audio and the window differs between versions.
bool LoadSystemModules();
bool InitGraphics(int width, int height);
// Takes down the system's boot splash; call once the first frame is about to be drawn.
void HideSplashScreen();
void SwapBuffers();
void SetSwapInterval(int interval);
// True when buffer swaps wait for the vertical blank.
bool IsVsyncActive();
void ShutdownGraphics();
// Opaque non-null handle for code that wants "the GL context" (ImGui's SDL backend).
void* GetGlContext();

// Registers the DualShock 4 of the user that started the game as an SDL game controller.
// Has to be called after SDL_Init(SDL_INIT_GAMECONTROLLER).
void AttachPad();

// Writes one line with the state of the heap arena and of the system memory pools to the log.
void LogMemoryStats(const char* when);

// The user service is shared between pad and audio; safe to call more than once.
int32_t GetInitialUserId();

} // namespace Ps4
} // namespace Ship

#endif // __PS4__
