#ifdef __PS4__

#include "ship/port/ps4/Ps4Platform.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <atomic>
#include <string>
#include <thread>

#include <SDL2/SDL.h>
#include <spdlog/spdlog.h>

#include <orbis/libkernel.h>
#include <orbis/Pad.h>
#include <orbis/Pigletv2VSH.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>

namespace Ship {
namespace Ps4 {

// ---------------------------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------------------------

// Retail firmware ships Piglet without its runtime shader compiler. Like other GLES homebrew
// (e.g. the Super Mario 64 port) we load a Piglet + shader compiler pair from this directory.
static constexpr const char* kUserModuleDir = "/data/self/system/common/lib";
// A package may also carry the pair itself, which makes it self-contained.
static constexpr const char* kBundledModuleDir = "/app0/assets/misc";

static int32_t sPigletModule = -1;
static int32_t sShaderCompilerModule = -1;

static int32_t LoadModule(const std::string& path) {
    int32_t startResult = 0;
    SPDLOG_INFO("[PS4] loading module \"{}\"", path);
    int32_t handle = (int32_t)sceKernelLoadStartModule(path.c_str(), 0, nullptr, 0, nullptr, &startResult);
    if (handle < 0) {
        SPDLOG_WARN("[PS4] sceKernelLoadStartModule(\"{}\") failed: 0x{:08X}", path, (uint32_t)handle);
    } else {
        SPDLOG_INFO("[PS4] loaded module \"{}\" (handle {}, module_start {})", path, handle, startResult);
    }
    return handle;
}

static std::string SystemModuleDir() {
    const char* sandboxWord = sceKernelGetFsSandboxRandomWord();
    return std::string("/") + (sandboxWord != nullptr ? sandboxWord : "system") + "/common/lib";
}

static bool LoadGraphicsModules() {
    if (sPigletModule >= 0) {
        return true;
    }

    const std::string systemDir = SystemModuleDir();
    static const char* const kSystemModules[] = {
        "libSceSysCore", "libSceMbus", "libSceIpmi", "libSceSystemService",
        "libSceUserService", "libSceAudioOut", "libScePad",
    };
    for (const char* name : kSystemModules) {
        LoadModule(systemDir + "/" + name + ".sprx");
    }

    // Both modules have to come from the same place, they are a matched pair.
    const char* moduleDir = kUserModuleDir;
    if (access((std::string(kBundledModuleDir) + "/libScePigletv2VSH.sprx").c_str(), F_OK) == 0) {
        sPigletModule = LoadModule(std::string(kBundledModuleDir) + "/libScePigletv2VSH.sprx");
        if (sPigletModule >= 0) {
            moduleDir = kBundledModuleDir;
        }
    }
    if (sPigletModule < 0) {
        sPigletModule = LoadModule(std::string(kUserModuleDir) + "/libScePigletv2VSH.sprx");
    }
    if (sPigletModule < 0) {
        SPDLOG_WARN("[PS4] no Piglet in {}, falling back to the system one (shader compilation will most likely "
                    "not work)",
                    kUserModuleDir);
        sPigletModule = LoadModule(systemDir + "/libScePigletv2VSH.sprx");
    }
    if (sPigletModule < 0) {
        SPDLOG_CRITICAL("[PS4] could not load libScePigletv2VSH.sprx");
        return false;
    }

    sShaderCompilerModule = LoadModule(std::string(moduleDir) + "/libSceShaccVSH.sprx");
    if (sShaderCompilerModule < 0) {
        sShaderCompilerModule = LoadModule(systemDir + "/libSceShaccVSH.sprx");
    }
    if (sShaderCompilerModule < 0) {
        SPDLOG_ERROR("[PS4] could not load libSceShaccVSH.sprx (the GLSL compiler). Copy libScePigletv2VSH.sprx and "
                     "libSceShaccVSH.sprx to {}/",
                     kUserModuleDir);
    }

    return true;
}

// ---------------------------------------------------------------------------------------------
// EGL / Piglet
// ---------------------------------------------------------------------------------------------

static constexpr const char* kVsyncMarker = "/data/soh/ps4_vsync";
static constexpr const char* kNoVsyncMarker = "/data/soh/ps4_novsync";
static bool sVsync = false;
// SDL_GetTicks64() + 1 while eglSwapBuffers() is running, 0 otherwise.
static std::atomic<uint64_t> sSwapStartedAt{ 0 };
static std::atomic<uint64_t> sSwapsDone{ 0 };

static void SwapWatchdog() {
    for (;;) {
        SDL_Delay(250);
        const uint64_t startedAt = sSwapStartedAt.load();
        if (startedAt != 0 && SDL_GetTicks64() + 1 - startedAt > 4000) {
            FILE* marker = fopen(kNoVsyncMarker, "w");
            if (marker != nullptr) {
                fputs("eglSwapBuffers blocked with swap interval 1; delete this file to try vsync again\n", marker);
                fclose(marker);
            }
            SPDLOG_ERROR("[PS4] eglSwapBuffers has been blocked for 4 s after {} swaps: vsync disabled for the next "
                         "start, quitting",
                         sSwapsDone.load());
            spdlog::default_logger()->flush();
            sceSystemServiceLoadExec("exit", nullptr);
            _exit(0);
        }
    }
}

static EGLDisplay sDisplay = EGL_NO_DISPLAY;
static EGLSurface sSurface = EGL_NO_SURFACE;
static EGLContext sContext = EGL_NO_CONTEXT;

extern "C" int Ps4Heap_GetKind(void) __attribute__((weak));
extern "C" void Ps4Heap_GetStats(size_t* arenaSize, size_t* inUse, size_t* peak) __attribute__((weak));

static size_t LargestFreeDirectBlock() {
    typedef int32_t (*AvailableDirectFn)(off_t, off_t, size_t, off_t*, size_t*);
    off_t physicalAddress = 0;
    size_t directSize = 0;
    ((AvailableDirectFn)(void*)sceKernelAvailableDirectMemorySize)(0, (off_t)sceKernelGetDirectMemorySize(), 0,
                                                                   &physicalAddress, &directSize);
    return directSize;
}

void LogMemoryStats(const char* when) {
    typedef int32_t (*AvailableFlexibleFn)(size_t*);
    size_t flexibleSize = 0;
    ((AvailableFlexibleFn)(void*)sceKernelAvailableFlexibleMemorySize)(&flexibleSize);

    size_t arena = 0;
    size_t inUse = 0;
    size_t peak = 0;
    int kind = 0;
    if (Ps4Heap_GetStats != nullptr && Ps4Heap_GetKind != nullptr) {
        Ps4Heap_GetStats(&arena, &inUse, &peak);
        kind = Ps4Heap_GetKind();
    }
    SPDLOG_INFO("[PS4] memory {}: heap {} MiB used / {} MiB peak of {} MiB ({}), free direct block {} MiB, free "
                "flexible {} MiB",
                when, inUse >> 20, peak >> 20, arena >> 20,
                kind == 1 ? "system flexible" : kind == 2 ? "direct" : "no arena", LargestFreeDirectBlock() >> 20,
                flexibleSize >> 20);
}

static void LogMemory(const char* when) {
    // The prototypes in the OpenOrbis header don't match the real functions (out parameters are
    // declared by value), hence the casts.
    typedef int32_t (*AvailableDirectFn)(off_t, off_t, size_t, off_t*, size_t*);
    typedef int32_t (*AvailableFlexibleFn)(size_t*);

    off_t physicalAddress = 0;
    size_t directSize = 0;
    size_t flexibleSize = 0;
    const size_t totalDirect = sceKernelGetDirectMemorySize();
    ((AvailableDirectFn)(void*)sceKernelAvailableDirectMemorySize)(0, (off_t)totalDirect, 0, &physicalAddress,
                                                                   &directSize);
    ((AvailableFlexibleFn)(void*)sceKernelAvailableFlexibleMemorySize)(&flexibleSize);
    SPDLOG_INFO("[PS4] memory {}: direct total {} MiB, largest free direct block {} MiB, free flexible {} MiB", when,
                totalDirect >> 20, directSize >> 20, flexibleSize >> 20);
}

struct PigletSizes {
    uint64_t systemShared;
    uint64_t videoShared;
    uint64_t flexible; // 0: don't let Piglet use flexible memory at all
    uint32_t drawCommandBuffer;
    uint32_t lcueResourceBuffer;
    uint64_t neededDirect; // only tried when this much direct memory is free in one block
};

// Tried in this order until eglGetDisplay() succeeds.
//
// The application has 768 MiB of direct memory and ~255 MiB of flexible memory. Piglet takes its
// "shared" memory from the former and, when allowed to, textures/render targets from the latter.
// The first entries need the game heap to live outside of direct memory (see Ps4Heap.c); the
// last ones are the split that was verified on a console with the heap inside of it.
static const PigletSizes kPigletCandidates[] = {
    { 128ull << 20, 512ull << 20, 236ull << 20, 4u << 20, 4u << 20, 700ull << 20 },
    { 128ull << 20, 512ull << 20, 208ull << 20, 4u << 20, 4u << 20, 700ull << 20 },
    { 128ull << 20, 512ull << 20, 208ull << 20, 1u << 20, 1u << 20, 700ull << 20 },
    { 64ull << 20, 128ull << 20, 208ull << 20, 1u << 20, 1u << 20, 0 },
    { 64ull << 20, 128ull << 20, 176ull << 20, 1u << 20, 1u << 20, 0 },
    { 64ull << 20, 128ull << 20, 64ull << 20, 1u << 20, 1u << 20, 0 },
};

static bool ConfigurePiglet(int width, int height, const PigletSizes& sizes) {
    OrbisPglConfig config;
    memset(&config, 0, sizeof(config));
    config.size = sizeof(config);
    config.flags = ORBIS_PGL_FLAGS_USE_COMPOSITE_EXT | 0x60;
    if (sizes.flexible != 0) {
        config.flags |= ORBIS_PGL_FLAGS_USE_FLEXIBLE_MEMORY;
    }
    config.processOrder = 1;
    config.systemSharedMemorySize = sizes.systemShared;
    config.videoSharedMemorySize = sizes.videoShared;
    config.maxMappedFlexibleMemory = sizes.flexible;
    config.drawCommandBufferSize = sizes.drawCommandBuffer;
    config.lcueResourceBufferSize = sizes.lcueResourceBuffer;
    config.dbgPosCmd_0x40 = width;
    config.dbgPosCmd_0x44 = height;
    config.dbgPosCmd_0x48 = 0;
    config.dbgPosCmd_0x4C = 0;
    config.unk_0x5C = 2;

    SPDLOG_INFO("[PS4] scePigletSetConfigurationVSH: video {} MiB, system {} MiB, flexible {} MiB, command buffer {} "
                "MiB",
                sizes.videoShared >> 20, sizes.systemShared >> 20, sizes.flexible >> 20,
                sizes.drawCommandBuffer >> 20);
    if (!scePigletSetConfigurationVSH(&config)) {
        SPDLOG_WARN("[PS4] scePigletSetConfigurationVSH rejected that configuration");
        return false;
    }
    return true;
}

static bool ChooseConfig(EGLConfig* config) {
    // Preferred: 24 bit depth + stencil. The last entry is what the known-good homebrew uses.
    static const EGLint kDepthStencil[][2] = { { 24, 8 }, { 24, 0 }, { 16, 0 }, { 0, 0 } };

    for (const auto& ds : kDepthStencil) {
        const EGLint attribs[] = {
            EGL_RED_SIZE,        8,
            EGL_GREEN_SIZE,      8,
            EGL_BLUE_SIZE,       8,
            EGL_ALPHA_SIZE,      8,
            EGL_DEPTH_SIZE,      ds[0],
            EGL_STENCIL_SIZE,    ds[1],
            EGL_SAMPLE_BUFFERS,  0,
            EGL_SAMPLES,         0,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
            EGL_NONE,
        };
        EGLint numConfigs = 0;
        if (eglChooseConfig(sDisplay, attribs, config, 1, &numConfigs) && numConfigs >= 1) {
            EGLint depth = 0;
            EGLint stencil = 0;
            eglGetConfigAttrib(sDisplay, *config, EGL_DEPTH_SIZE, &depth);
            eglGetConfigAttrib(sDisplay, *config, EGL_STENCIL_SIZE, &stencil);
            SPDLOG_INFO("[PS4] EGL config: requested depth {} / stencil {}, got depth {} / stencil {}", ds[0], ds[1],
                        depth, stencil);
            return true;
        }
        SPDLOG_WARN("[PS4] eglChooseConfig(depth {}, stencil {}) failed: 0x{:04X}", ds[0], ds[1],
                    (unsigned int)eglGetError());
    }
    return false;
}

bool InitGraphics(int width, int height) {
    if (sContext != EGL_NO_CONTEXT) {
        return true;
    }

    SPDLOG_INFO("[PS4] InitGraphics: loading modules");
    if (!LoadGraphicsModules()) {
        return false;
    }

    LogMemory("before Piglet");
    LogMemoryStats("before Piglet");
    for (const PigletSizes& sizes : kPigletCandidates) {
        if (LargestFreeDirectBlock() < sizes.neededDirect) {
            continue;
        }
        if (!ConfigurePiglet(width, height, sizes)) {
            continue;
        }
        SPDLOG_INFO("[PS4] eglGetDisplay");
        sDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (sDisplay != EGL_NO_DISPLAY) {
            break;
        }
        SPDLOG_WARN("[PS4] eglGetDisplay failed with that configuration");
        LogMemory("after the failed attempt");
    }
    if (sDisplay == EGL_NO_DISPLAY) {
        SPDLOG_CRITICAL("[PS4] eglGetDisplay failed with every Piglet configuration");
        return false;
    }
    LogMemory("after eglGetDisplay");

    EGLint major = 0;
    EGLint minor = 0;
    SPDLOG_INFO("[PS4] eglInitialize");
    if (!eglInitialize(sDisplay, &major, &minor)) {
        SPDLOG_CRITICAL("[PS4] eglInitialize failed: 0x{:04X}", (unsigned int)eglGetError());
        return false;
    }
    SPDLOG_INFO("[PS4] EGL {}.{}", major, minor);

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        SPDLOG_CRITICAL("[PS4] eglBindAPI failed: 0x{:04X}", (unsigned int)eglGetError());
        return false;
    }

    // Vertical sync is opt-in (create an empty /data/soh/ps4_vsync file): on a real console the
    // game felt clearly worse with a swap interval of 1 than paced by its own timer, any frame
    // that is slightly late waits for a whole extra refresh. The watchdog stays as a safety net,
    // eglSwapBuffers() is known to block forever with some ways of enabling it.
    sVsync = access(kVsyncMarker, F_OK) == 0 && access(kNoVsyncMarker, F_OK) != 0;
    SPDLOG_INFO("[PS4] vsync: {}", sVsync ? "swap interval 1 (ps4_vsync marker found)" : "off, swap interval 0");
    if (!eglSwapInterval(sDisplay, sVsync ? 1 : 0)) {
        SPDLOG_WARN("[PS4] eglSwapInterval failed: 0x{:04X}", (unsigned int)eglGetError());
    }
    if (sVsync) {
        std::thread(SwapWatchdog).detach();
    }

    EGLConfig config = nullptr;
    if (!ChooseConfig(&config)) {
        SPDLOG_CRITICAL("[PS4] no usable EGL configuration");
        return false;
    }

    OrbisPglWindow window;
    memset(&window, 0, sizeof(window));
    window.uID = 0;
    window.uWidth = (uint32_t)width;
    window.uHeight = (uint32_t)height;

    const EGLint windowAttribs[] = { EGL_RENDER_BUFFER, EGL_BACK_BUFFER, EGL_NONE };
    SPDLOG_INFO("[PS4] eglCreateWindowSurface");
    sSurface = eglCreateWindowSurface(sDisplay, config, &window, windowAttribs);
    if (sSurface == EGL_NO_SURFACE) {
        SPDLOG_CRITICAL("[PS4] eglCreateWindowSurface failed: 0x{:04X}", (unsigned int)eglGetError());
        return false;
    }

    const EGLint contextAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    SPDLOG_INFO("[PS4] eglCreateContext");
    sContext = eglCreateContext(sDisplay, config, EGL_NO_CONTEXT, contextAttribs);
    if (sContext == EGL_NO_CONTEXT) {
        SPDLOG_CRITICAL("[PS4] eglCreateContext failed: 0x{:04X}", (unsigned int)eglGetError());
        return false;
    }

    SPDLOG_INFO("[PS4] eglMakeCurrent");
    if (!eglMakeCurrent(sDisplay, sSurface, sSurface, sContext)) {
        SPDLOG_CRITICAL("[PS4] eglMakeCurrent failed: 0x{:04X}", (unsigned int)eglGetError());
        return false;
    }

    SPDLOG_INFO("[PS4] GLES context ready ({}x{})", width, height);

    return true;
}

void SetSwapInterval(int interval) {
    // Deliberately ignored. With a swap interval of 1 eglSwapBuffers() never returned on a real
    // console; Piglet is left at interval 0 (set once in InitGraphics, like every other Piglet
    // homebrew does) and the frame pacing is done by the window backend's own timer.
    (void)interval;
}

void SwapBuffers() {
    if (sDisplay != EGL_NO_DISPLAY && sSurface != EGL_NO_SURFACE) {
        sSwapStartedAt.store(SDL_GetTicks64() + 1);
        eglSwapBuffers(sDisplay, sSurface);
        sSwapStartedAt.store(0);
        sSwapsDone.fetch_add(1);
    }
}

bool IsVsyncActive() {
    return sVsync;
}

void ShutdownGraphics() {
    if (sDisplay == EGL_NO_DISPLAY) {
        return;
    }
    eglMakeCurrent(sDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (sSurface != EGL_NO_SURFACE) {
        eglDestroySurface(sDisplay, sSurface);
        sSurface = EGL_NO_SURFACE;
    }
    if (sContext != EGL_NO_CONTEXT) {
        eglDestroyContext(sDisplay, sContext);
        sContext = EGL_NO_CONTEXT;
    }
    eglTerminate(sDisplay);
    sDisplay = EGL_NO_DISPLAY;
}

void HideSplashScreen() {
    // The system keeps showing the boot splash until the application says it's alive.
    sceSystemServiceHideSplashScreen();
}

void* GetGlContext() {
    return (void*)sContext;
}

// ---------------------------------------------------------------------------------------------
// User / pad
// ---------------------------------------------------------------------------------------------

static bool sUserServiceReady = false;
static int32_t sUserId = -1;

int32_t GetInitialUserId() {
    if (!sUserServiceReady) {
        // 700 == SCE_KERNEL_PRIO_FIFO_DEFAULT. Fails harmlessly if the service is already up.
        struct {
            int32_t priority;
        } params = { 700 };
        sceUserServiceInitialize(&params);
        if (sceUserServiceGetInitialUser(&sUserId) < 0) {
            SPDLOG_ERROR("[PS4] sceUserServiceGetInitialUser failed");
            sUserId = -1;
        }
        sUserServiceReady = true;
    }
    return sUserId;
}

static int32_t sPadHandle = -1;
static SDL_JoystickID sPadInstanceId = -1;

static Sint16 StickToAxis(uint8_t value) {
    // 0..255 with 128 as rest position -> -32768..32767
    int scaled = ((int)value - 128) * 258;
    if (scaled > 32767) {
        scaled = 32767;
    }
    if (scaled < -32768) {
        scaled = -32768;
    }
    return (Sint16)scaled;
}

static Sint16 TriggerToAxis(uint8_t value) {
    // 0..255 -> -32768..32767 (SDL treats virtual trigger axes as full range)
    return (Sint16)((int)value * 257 - 32768);
}

static void SDLCALL PadUpdate(void* userdata) {
    if (sPadHandle < 0) {
        return;
    }

    SDL_Joystick* joystick = SDL_JoystickFromInstanceID(sPadInstanceId);
    if (joystick == nullptr) {
        return;
    }

    OrbisPadData data;
    memset(&data, 0, sizeof(data));
    if (scePadReadState(sPadHandle, &data) < 0 || !data.connected) {
        return;
    }

    const uint32_t buttons = data.buttons;
    const struct {
        SDL_GameControllerButton sdl;
        uint32_t orbis;
    } kButtons[] = {
        { SDL_CONTROLLER_BUTTON_A, ORBIS_PAD_BUTTON_CROSS },
        { SDL_CONTROLLER_BUTTON_B, ORBIS_PAD_BUTTON_CIRCLE },
        { SDL_CONTROLLER_BUTTON_X, ORBIS_PAD_BUTTON_SQUARE },
        { SDL_CONTROLLER_BUTTON_Y, ORBIS_PAD_BUTTON_TRIANGLE },
        // The SHARE button belongs to the system; the touch pad click stands in for "select".
        { SDL_CONTROLLER_BUTTON_BACK, ORBIS_PAD_BUTTON_TOUCH_PAD },
        { SDL_CONTROLLER_BUTTON_START, ORBIS_PAD_BUTTON_OPTIONS },
        { SDL_CONTROLLER_BUTTON_LEFTSTICK, ORBIS_PAD_BUTTON_L3 },
        { SDL_CONTROLLER_BUTTON_RIGHTSTICK, ORBIS_PAD_BUTTON_R3 },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, ORBIS_PAD_BUTTON_L1 },
        { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, ORBIS_PAD_BUTTON_R1 },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, ORBIS_PAD_BUTTON_UP },
        { SDL_CONTROLLER_BUTTON_DPAD_DOWN, ORBIS_PAD_BUTTON_DOWN },
        { SDL_CONTROLLER_BUTTON_DPAD_LEFT, ORBIS_PAD_BUTTON_LEFT },
        { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, ORBIS_PAD_BUTTON_RIGHT },
    };
    for (const auto& button : kButtons) {
        SDL_JoystickSetVirtualButton(joystick, button.sdl, (buttons & button.orbis) ? SDL_PRESSED : SDL_RELEASED);
    }

    SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, StickToAxis(data.leftStick.x));
    SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTY, StickToAxis(data.leftStick.y));
    SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTX, StickToAxis(data.rightStick.x));
    SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTY, StickToAxis(data.rightStick.y));
    SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, TriggerToAxis(data.analogButtons.l2));
    SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, TriggerToAxis(data.analogButtons.r2));
}

static int SDLCALL PadRumble(void* userdata, Uint16 lowFrequencyRumble, Uint16 highFrequencyRumble) {
    if (sPadHandle < 0) {
        return -1;
    }
    OrbisPadVibeParam vibration;
    vibration.lgMotor = (uint8_t)(lowFrequencyRumble >> 8);
    vibration.smMotor = (uint8_t)(highFrequencyRumble >> 8);
    return scePadSetVibration(sPadHandle, &vibration) < 0 ? -1 : 0;
}

static int SDLCALL PadSetLED(void* userdata, Uint8 red, Uint8 green, Uint8 blue) {
    if (sPadHandle < 0) {
        return -1;
    }
    OrbisPadColor color;
    color.r = red;
    color.g = green;
    color.b = blue;
    color.a = 255;
    return scePadSetLightBar(sPadHandle, &color) < 0 ? -1 : 0;
}

void AttachPad() {
    if (sPadInstanceId >= 0) {
        return;
    }

    // The dummy video driver never gives the window input focus, and SDL drops joystick input of
    // unfocused applications unless told otherwise. Override so the config can't turn this off.
    SDL_SetHintWithPriority(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1", SDL_HINT_OVERRIDE);

    const int32_t userId = GetInitialUserId();

    int32_t result = scePadInit();
    if (result < 0) {
        SPDLOG_WARN("[PS4] scePadInit returned 0x{:08X}", (uint32_t)result);
    }

    sPadHandle = scePadOpen(userId, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
    if (sPadHandle < 0) {
        // Already opened by somebody else in this process.
        sPadHandle = scePadGetHandle(userId, ORBIS_PAD_PORT_TYPE_STANDARD, 0);
    }
    if (sPadHandle < 0) {
        SPDLOG_ERROR("[PS4] scePadOpen failed: 0x{:08X}", (uint32_t)sPadHandle);
        return;
    }

    SDL_VirtualJoystickDesc desc;
    SDL_zero(desc);
    desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes = SDL_CONTROLLER_AXIS_MAX;
    desc.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    desc.nhats = 0;
    // Sony DualShock 4: lets SDL (and the UI built on it) pick PlayStation button names.
    desc.vendor_id = 0x054C;
    desc.product_id = 0x09CC;
    desc.name = "PS4 Controller";
    desc.Update = PadUpdate;
    desc.Rumble = PadRumble;
    desc.SetLED = PadSetLED;

    const int deviceIndex = SDL_JoystickAttachVirtualEx(&desc);
    if (deviceIndex < 0) {
        SPDLOG_ERROR("[PS4] SDL_JoystickAttachVirtualEx failed: {}", SDL_GetError());
        return;
    }

    sPadInstanceId = SDL_JoystickGetDeviceInstanceID(deviceIndex);
    SPDLOG_INFO("[PS4] DualShock 4 attached as SDL joystick {} (instance {}, pad handle {})", deviceIndex,
                (int)sPadInstanceId, sPadHandle);
}

} // namespace Ps4
} // namespace Ship

// imgui_impl_opengl3.cpp is compiled with glShaderSource redirected here: it always prepends a
// "#version" line to its shaders, while the shaders known to compile on Piglet carry none.
extern "C" void Ps4GlShaderSourceNoVersion(GLuint shader, GLsizei count, const GLchar* const* strings,
                                           const GLint* lengths) {
    const GLchar* filtered[8];
    GLint filteredLengths[8];
    GLsizei filteredCount = 0;

    for (GLsizei i = 0; i < count && filteredCount < 8; i++) {
        const GLchar* str = strings[i];
        GLint length = (lengths != nullptr && lengths[i] >= 0) ? lengths[i] : (GLint)strlen(str);
        if (length >= 8 && strncmp(str, "#version", 8) == 0) {
            // Drop the directive line, keep whatever follows it in the same string.
            const GLchar* lineEnd = (const GLchar*)memchr(str, '\n', (size_t)length);
            if (lineEnd == nullptr) {
                continue;
            }
            length -= (GLint)(lineEnd + 1 - str);
            str = lineEnd + 1;
        }
        if (length <= 0) {
            continue;
        }
        filtered[filteredCount] = str;
        filteredLengths[filteredCount] = length;
        filteredCount++;
    }

#undef glShaderSource
    glShaderSource(shader, filteredCount, filtered, filteredLengths);
}

#endif // __PS4__
