#include <SDL2/SDL.h>
#include <SDL2/SDL_opengles2.h>

#include <cstdio>
extern "C" void __gcov_dump(void) __attribute__((weak));  // libgcov, -fprofile-generate builds only
extern "C" int __llvm_profile_write_file(void) __attribute__((weak));  // clang profile runtime (pgo_clang_vm.sh gen)
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <filesystem>
#include <algorithm>
#include <atomic>

#include "guest_runtime.h"
#include "guest_memory.h"
#include "d3d9_bridge.h"
#include "d3d9_gles.h"
#include "guest_boot_worker.h"
#include "nfs_widescreen.h"
#include "runtime_log.h"
#include "vkbd.h"
#include "sdl_gamepad.h"

#if defined(__linux__)
#include <sched.h>
#endif

#include <csignal>
#include <unistd.h>

extern uint32_t g_nfsScreenRotation;  // d3d9_gles.cpp
bool toggleD3D9FpsCounter();           // d3d9_gles.cpp

namespace {

constexpr uint32_t R36S_SCREEN_WIDTH = 640;   // fallback when the display mode can't be read
constexpr uint32_t R36S_SCREEN_HEIGHT = 480;

struct R36SHostConfig {
    std::string gameDir;
    std::string language = "Spanish";
    uint32_t targetWidth = R36S_SCREEN_WIDTH;
    uint32_t targetHeight = R36S_SCREEN_HEIGHT;
    uint32_t frameLimit = 0;  // 0 = uncapped (no frame-pacing sleep); --cap30 / --cap60 still limit the frame rate
    uint32_t screenWidth = R36S_SCREEN_WIDTH;   // the panel (window) size
    uint32_t screenHeight = R36S_SCREEN_HEIGHT;
    uint32_t rotation = 0;                       // 0, 90, 270 (NFSU2_ROTATE)
    bool fullscreen = true;
    bool widescreen = false;
    bool autoAdvance = false;
};

// ---------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// Gamepad (see sdl_gamepad.h; same model as the Android port)
// -----------------------------------------------------------------------------
static SdlGamepad g_gamepad;
static void padSetKey(unsigned scan, bool down) { setGuestKey(scan, down); }
static void publishGamepad() {
    float axes[6];
    uint32_t buttons;
    int hatX, hatY;
    g_gamepad.padState(axes, buttons, hatX, hatY);
    setGuestPad(axes, buttons, hatX, hatY);
}

// The R36S built-in pad ("GO-Super Gamepad", odroidgo3-joypad driver; buttons from the device tree
// BOOT/rk3326-r36s-linux.dtb). SDL numbers evdev buttons by code: b0 BTN_SOUTH (physical B), b1 BTN_EAST
// (A), b2 BTN_NORTH (X), b3 BTN_WEST (Y), b4/b5 L1/R1, b6/b7 L2/R2, b8-b11 d-pad up/down/left/right,
// b12-b16 TRIGGER_HAPPY1-5 = SELECT, START, L3, R3, FN (ArkOS/dArkOS es_input ids 12-16); sticks a0-a3.
static const char* kR36SPadMapping =
    "a:b0,b:b1,y:b2,x:b3,leftshoulder:b4,rightshoulder:b5,lefttrigger:b6,righttrigger:b7,"
    "dpup:b8,dpdown:b9,dpleft:b10,dpright:b11,back:b12,start:b13,leftstick:b14,rightstick:b15,guide:b16,"
    "leftx:a0,lefty:a1,rightx:a2,righty:a3,platform:Linux,";

// Log the mapping SDL will use and, for the R36S pad, add the full mapping when the one supplied by the
// system/PortMaster lacks the stick buttons (then R3 never reaches the game as RIGHTSTICK).
static void preparePadMapping(int index) {
    const char* name = SDL_JoystickNameForIndex(index);
    char guid[64] = {};
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof(guid));
    char* mapping = SDL_GameControllerMappingForDeviceIndex(index);
    std::printf("[NFSU2 PAD] device %d '%s' guid=%s mapping=%s\n", index, name ? name : "?", guid,
                mapping ? mapping : "(none)");
    const bool r36s = name && std::strstr(name, "GO-Super Gamepad");
    const bool hasSticks = mapping && std::strstr(mapping, "rightstick:") && std::strstr(mapping, "leftstick:");
    if (r36s && (!hasSticks || std::getenv("NFSU2_FORCE_PAD_MAPPING"))) {
        std::string m = std::string(guid) + "," + name + "," + kR36SPadMapping;
        const int r = SDL_GameControllerAddMapping(m.c_str());
        std::printf("[NFSU2 PAD] installed R36S mapping (%d): %s\n", r, m.c_str());
    }
    if (mapping) SDL_free(mapping);
}

static SDL_GameController* openPad(int index) {
    preparePadMapping(index);
    if (!SDL_IsGameController(index)) return nullptr;
    SDL_GameController* c = SDL_GameControllerOpen(index);
    if (c) {
        std::printf("Connected GameController: %s\n", SDL_GameControllerName(c));
        if (char* m = SDL_GameControllerMapping(c)) { std::printf("[NFSU2 PAD] active mapping: %s\n", m); SDL_free(m); }
        setGuestPadConnected(true);
    }
    return c;
}

// Virtual keyboard input (overlay drawn by the GLES layer, see d3d9_gles/vkbd.h)
// ---------------------------------------------------------------------------
struct PendingKey { std::chrono::steady_clock::time_point due; unsigned scan; bool down; };
static std::vector<PendingKey> g_vkbdPending;

static void vkbdTap(unsigned scan, bool withShift) {
    const auto now = std::chrono::steady_clock::now();
    // Hold long enough for the game's per-frame keyboard polling to see the press.
    if (withShift) setGuestKey(0x2a, true);
    setGuestKey(scan, true);
    g_vkbdPending.push_back({now + std::chrono::milliseconds(80), scan, false});
    if (withShift) g_vkbdPending.push_back({now + std::chrono::milliseconds(100), 0x2a, false});
}

static void vkbdFlushPending(bool all) {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = g_vkbdPending.begin(); it != g_vkbdPending.end();) {
        if (all || now >= it->due) { setGuestKey(it->scan, it->down); it = g_vkbdPending.erase(it); }
        else ++it;
    }
}

static void vkbdSetVisible(bool visible) {
    vkbd::state().visible = visible;
    std::printf("[NFSU2] Virtual keyboard %s\n", visible ? "opened" : "closed");
}

// Select is also a combo modifier: Select+Start quits, Select+R3 toggles the frame counter. Its own action
// (reset car while driving) reaches the game only once no combo can follow: after it has been held for 200 ms,
// or as a short tap when it is released sooner. A combo cancels it, so the car is not reset.
struct SelectGate {
    bool held = false, sent = false, combo = false;
    std::chrono::steady_clock::time_point since{}, releaseAt{};
    bool releasePending = false;
};
static SelectGate g_select;
static bool g_quitByCombo = false;   // read by the main loop (g_running is defined further down)
static void selectSend(bool down) {
    std::printf("[NFSU2 PAD] select -> game %s\n", down ? "down" : "up");
    g_gamepad.button(SDL_CONTROLLER_BUTTON_BACK, down, guestDrivingControls(), padSetKey);
    publishGamepad();
}
static void selectTick() {
    const auto now = std::chrono::steady_clock::now();
    if (g_select.held && !g_select.sent && !g_select.combo && now - g_select.since >= std::chrono::milliseconds(200)) {
        selectSend(true);
        g_select.sent = true;
    }
    if (g_select.releasePending && now >= g_select.releaseAt) { selectSend(false); g_select.releasePending = false; }
}
// Returns true when the button belongs to the Select gate or a Select combo.
static bool selectHandleButton(uint8_t button, bool down) {
    const auto now = std::chrono::steady_clock::now();
    if (button == SDL_CONTROLLER_BUTTON_BACK) {
        if (down) { g_select.held = true; g_select.sent = false; g_select.combo = false; g_select.since = now; return true; }
        g_select.held = false;
        if (g_select.sent) selectSend(false);
        else if (!g_select.combo) {                    // a short tap: press now, release after 80 ms
            selectSend(true);
            g_select.releasePending = true;
            g_select.releaseAt = now + std::chrono::milliseconds(80);
        }
        g_select.sent = false;
        return true;
    }
    if (!g_select.held || !down) return false;
    if (button == SDL_CONTROLLER_BUTTON_RIGHTSTICK) {  // Select + R3: frame counter (not the keyboard)
        g_select.combo = true;
        if (g_select.sent) { selectSend(false); g_select.sent = false; }
        toggleD3D9FpsCounter();
        return true;
    }
    if (button == SDL_CONTROLLER_BUTTON_START) {       // Select + Start: quit
        g_select.combo = true;
        std::printf("[NFSU2] Hotkey Select+Start triggered. Quitting.\n");
        g_quitByCombo = true;
        return true;
    }
    return false;
}

// Returns true when the button was consumed by the virtual keyboard.
static bool vkbdHandleButton(uint8_t button, bool down) {
    vkbd::State& st = vkbd::state();
    // Only R3 toggles the keyboard (L3 is easy to press by accident while steering with the left stick).
    if (button == SDL_CONTROLLER_BUTTON_RIGHTSTICK) {
        if (down) vkbdSetVisible(!st.visible.load());
        return true;
    }
    if (!st.visible.load()) return false;
    if (!down) return true;
    switch (button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: vkbd::move(0, -1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: vkbd::move(0, 1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: vkbd::move(-1, 0); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: vkbd::move(1, 0); break;
        case SDL_CONTROLLER_BUTTON_A: {  // South (physical B on R36S): type the selected key
            const vkbd::Key& k = vkbd::current();
            switch (k.special) {
                case vkbd::kShift: st.shift = !st.shift.load(); break;
                case vkbd::kSpace: vkbdTap(0x39, false); break;
                case vkbd::kDelete: vkbdTap(0x0E, false); break;
                case vkbd::kEnter: vkbdTap(0x1C, false); vkbdSetVisible(false); break;
                default: {
                    const bool letter = k.lower >= 'a' && k.lower <= 'z';
                    vkbdTap(k.scan, k.needsShift || (letter && st.shift.load()));
                    break;
                }
            }
            break;
        }
        case SDL_CONTROLLER_BUTTON_X: vkbdTap(0x0E, false); break;            // West: backspace
        case SDL_CONTROLLER_BUTTON_Y: st.shift = !st.shift.load(); break;      // North: shift
        case SDL_CONTROLLER_BUTTON_START: vkbdTap(0x1C, false); vkbdSetVisible(false); break;
        case SDL_CONTROLLER_BUTTON_B: vkbdSetVisible(false); break;            // East: close
        default: break;
    }
    return true;
}

// Left stick drives the keyboard cursor while it is open (edge-triggered).
static bool vkbdHandleAxis(uint8_t axis, float value) {
    if (!vkbd::state().visible.load()) return false;
    static int lastX = 0, lastY = 0;
    if (axis == SDL_CONTROLLER_AXIS_LEFTX) {
        const int d = value < -0.5f ? -1 : (value > 0.5f ? 1 : 0);
        if (d && d != lastX) vkbd::move(d, 0);
        lastX = d;
    } else if (axis == SDL_CONTROLLER_AXIS_LEFTY) {
        const int d = value < -0.5f ? -1 : (value > 0.5f ? 1 : 0);
        if (d && d != lastY) vkbd::move(0, d);
        lastY = d;
    }
    return true;
}

static void signalHandler(int sig) {
    std::printf("[NFSU2] Caught signal %d, exiting immediately.\n", sig);
    ::sync();   // killed from the system/PortMaster: get pending writes (saves, logs) onto the card first
    std::_Exit(0);
}

SDL_Window* g_window = nullptr;
std::atomic<bool> g_running{true};
GuestBootWorker g_guestWorker;

// DirectInput 8 gamepad mapping state
struct GamepadState {
    float axes[6]{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    uint32_t buttons = 0;
    int hatX = 0;
    int hatY = 0;
    bool startHeld = false;
    bool selectHeld = false;
} g_pad;

float normalizeAxis(Sint16 value) {
    if (std::abs(value) < 3200) return 0.0f; // Deadzone
    return value < 0 ? static_cast<float>(value) / 32768.0f : static_cast<float>(value) / 32767.0f;
}

void syncPadToGuest() {
    setGuestPad(g_pad.axes, g_pad.buttons, g_pad.hatX, g_pad.hatY);
}

// Convert common keys to PC DirectInput scan codes for keyboard fallback
unsigned scanFromKeycode(SDL_Keycode key) {
    switch (key) {
        case SDLK_UP: return 0xc8;
        case SDLK_DOWN: return 0xd0;
        case SDLK_LEFT: return 0xcb;
        case SDLK_RIGHT: return 0xcd;
        case SDLK_RETURN: case SDLK_KP_ENTER: return 0x1c;
        case SDLK_ESCAPE: return 0x01;
        case SDLK_SPACE: return 0x39;
        case SDLK_BACKSPACE: return 0x0e;
        case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x2a;
        case SDLK_LCTRL: case SDLK_RCTRL: return 0x1d;
        case SDLK_TAB: return 0x0f;
        default: {
            if (key >= SDLK_a && key <= SDLK_z) {
                static const unsigned letters[] = {
                    0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,
                    0x32,0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c
                };
                return letters[key - SDLK_a];
            }
            if (key >= SDLK_0 && key <= SDLK_9) {
                return (key == SDLK_0) ? 0x0b : unsigned(key - SDLK_0) + 1;
            }
            return 0;
        }
    }
}

std::string findGamePath(int argc, char** argv) {
    // 1. Explicit CLI argument
    if (argc > 1 && argv[1] && argv[1][0] != '-') {
        std::filesystem::path p(argv[1]);
        if (std::filesystem::exists(p / "SPEED2.EXE") || std::filesystem::exists(p / "speed2.exe")) {
            return p.string();
        }
        if (std::filesystem::exists(p) && !std::filesystem::is_directory(p)) {
            return p.parent_path().string();
        }
    }

    // 2. Local gamedata folder (PortMaster standard)
    if (std::filesystem::exists("gamedata/SPEED2.EXE") || std::filesystem::exists("gamedata/speed2.exe")) {
        return "gamedata";
    }

    // 3. Current working directory
    if (std::filesystem::exists("SPEED2.EXE") || std::filesystem::exists("speed2.exe")) {
        return ".";
    }

    // 4. Default PortMaster mount point on ArkOS / R36S SD cards
    const char* defaultPaths[] = {
        "/roms/ports/nfsu2/gamedata",
        "/roms2/ports/nfsu2/gamedata",
        "/storage/roms/ports/nfsu2/gamedata",
        "/roms/ports/nfsu2",
        "/roms2/ports/nfsu2"
    };
    for (const char* path : defaultPaths) {
        if (std::filesystem::exists(std::string(path) + "/SPEED2.EXE") ||
            std::filesystem::exists(std::string(path) + "/speed2.exe")) {
            return path;
        }
    }

    return "gamedata";
}

} // namespace

static bool parseSize(const char* text, uint32_t& w, uint32_t& h) {
    unsigned a = 0, b = 0;
    if (!text || std::sscanf(text, "%ux%u", &a, &b) != 2 || a < 320 || b < 240 || a > 8192 || b > 8192) return false;
    w = a; h = b;
    return true;
}

// Screen and render size for whatever handheld this runs on (call after SDL_Init(VIDEO)):
//   screen  = the panel's current mode (KMSDRM: the native mode), or NFSU2_SCREEN=WxH.
//   NFSU2_ROTATE=90|270: the panel is mounted rotated; the image is turned to match (auto: 90 for a
//   portrait panel).
//   render  = what the game draws at. NFSU2_ASPECT=auto (default): the screen's own aspect when it is
//   4:3 or wider (widescreen FOV and HUD), 4:3 with bars on narrower screens (e.g. 1:1). NFSU2_ASPECT=4:3:
//   always 4:3 (pillarbox on wide screens). NFSU2_RENDER_SCALE=0.5..1 renders below the panel resolution
//   (scaled up; saves GPU time on big panels). NFSU2_RENDER=WxH sets the render size directly.
static void chooseResolution(R36SHostConfig& config) {
    uint32_t sw = R36S_SCREEN_WIDTH, sh = R36S_SCREEN_HEIGHT;
    SDL_DisplayMode mode{};
    if (parseSize(std::getenv("NFSU2_SCREEN"), sw, sh)) {
    } else if (SDL_GetCurrentDisplayMode(0, &mode) == 0 && mode.w >= 320 && mode.h >= 240) {
        sw = static_cast<uint32_t>(mode.w); sh = static_cast<uint32_t>(mode.h);
    } else if (SDL_GetDesktopDisplayMode(0, &mode) == 0 && mode.w >= 320 && mode.h >= 240) {
        sw = static_cast<uint32_t>(mode.w); sh = static_cast<uint32_t>(mode.h);
    }
    config.screenWidth = sw; config.screenHeight = sh;
    const char* rotEnv = std::getenv("NFSU2_ROTATE");
    config.rotation = rotEnv ? static_cast<uint32_t>(std::atoi(rotEnv)) : (sh > sw ? 90u : 0u);
    if (config.rotation != 90 && config.rotation != 270) config.rotation = 0;
    // The screen as the player holds it.
    const uint32_t lw = config.rotation ? sh : sw, lh = config.rotation ? sw : sh;
    uint32_t rw = lw, rh = lh;
    const char* aspect = std::getenv("NFSU2_ASPECT");
    const bool force43 = aspect && (!std::strcmp(aspect, "4:3") || !std::strcmp(aspect, "43"));
    if (force43 || lw * 3 < lh * 4) {       // 4:3 inside the screen
        if (lw * 3 >= lh * 4) { rh = lh; rw = lh * 4 / 3; }
        else { rw = lw; rh = lw * 3 / 4; }
    }
    if (const char* sc = std::getenv("NFSU2_RENDER_SCALE")) {
        const double f = std::atof(sc);
        if (f >= 0.25 && f < 1.0) { rw = static_cast<uint32_t>(rw * f); rh = static_cast<uint32_t>(rh * f); }
    }
    parseSize(std::getenv("NFSU2_RENDER"), rw, rh);
    rw = std::max<uint32_t>(320, rw & ~1u); rh = std::max<uint32_t>(240, rh & ~1u);
    config.targetWidth = rw; config.targetHeight = rh;
    // Wider than 4:3: the widescreen fix (hor+ FOV, HUD at the edges, mirror, radar, FMVs).
    config.widescreen = rw * 3 > rh * 4 + 3;
    if (const char* ws = std::getenv("NFSU2_WIDESCREEN")) config.widescreen = std::atoi(ws) != 0;
}

int main(int argc, char** argv) {
    R36SHostConfig config;
    config.gameDir = findGamePath(argc, argv);
    if (const char* envLang = std::getenv("NFSU2_LANG")) {
        if (envLang[0]) config.language = envLang;
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--windowed") config.fullscreen = false;
        else if (arg == "--cap30") config.frameLimit = 30;
        else if (arg == "--cap60") config.frameLimit = 60;
        else if (arg.rfind("--cap=", 0) == 0) config.frameLimit = static_cast<uint32_t>(std::atoi(arg.c_str() + 6));
        else if (arg == "--auto-advance") config.autoAdvance = true;
        else if (arg.rfind("--lang=", 0) == 0) config.language = arg.substr(7);
    }

    std::signal(SIGTERM, signalHandler);
    std::signal(SIGINT, signalHandler);

    std::printf("====================================================\n");
    std::printf(" NFSU2 Recompiled - R36S / Linux ARM64 PortMaster   \n");
    std::printf(" Game Directory: %s                                \n", config.gameDir.c_str());
    std::printf(" Frame Rate Cap: %u FPS                            \n", config.frameLimit);
    std::printf(" Language:       %s                                \n", config.language.c_str());
    std::printf("====================================================\n");

    // Initialize SDL2
    Uint32 sdlFlags = SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK;
    if (SDL_Init(sdlFlags) < 0) {
        std::fprintf(stderr, "Failed to initialize SDL2: %s\n", SDL_GetError());
        return 1;
    }
    chooseResolution(config);
    g_nfsScreenRotation = config.rotation;
    {
        // set by ports/r36s/compat/glibc_compat.c (absent in builds linked without it)
        extern int compat_newer_libm __attribute__((weak));
        if (&compat_newer_libm)
            std::printf("[NFSU2] glibc compat: %s libm functions\n", compat_newer_libm ? "newer" : "GLIBC_2.17");
    }
    std::printf("[NFSU2] screen %ux%u%s, game renders %ux%u (%s)\n", config.screenWidth, config.screenHeight,
                config.rotation == 90 ? " rotated 90" : (config.rotation == 270 ? " rotated 270" : ""),
                config.targetWidth, config.targetHeight,
                config.widescreen ? "widescreen" : "4:3");

    // Load gamecontrollerdb if present in local directory or PortMaster path
    const char* dbPaths[] = {"gamecontrollerdb.txt", "gamedata/gamecontrollerdb.txt", "/roms/ports/PortMaster/gamecontrollerdb.txt"};
    for (const char* db : dbPaths) {
        if (std::filesystem::exists(db)) {
            SDL_GameControllerAddMappingsFromFile(db);
            std::printf("Loaded SDL GameController mappings: %s\n", db);
            break;
        }
    }

    // Open first available controller
    SDL_GameController* controller = nullptr;
    for (int i = 0; i < SDL_NumJoysticks() && !controller; ++i) controller = openPad(i);

    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    // Create SDL2 window with OpenGL ES 2.0 support
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    // NFSU2 uses an A8R8G8B8 back buffer and relies on destination alpha: the race minimap is clipped to
    // its circle by drawing a mask into framebuffer alpha and blending the map with DESTALPHA.
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

    // In KMSDRM, SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN automatically renders full-panel.
    // Never use SDL_WINDOW_FULLSCREEN_DESKTOP as it triggers DRM CRTC mode re-probing which blanks the screen.
    Uint32 windowFlags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;

    g_window = SDL_CreateWindow(
        "Need for Speed: Underground 2",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        static_cast<int>(config.screenWidth), static_cast<int>(config.screenHeight),
        windowFlags
    );

    if (!g_window) {
        std::fprintf(stderr, "SDL window with an alpha channel failed (%s); retrying without\n", SDL_GetError());
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
        g_window = SDL_CreateWindow(
            "Need for Speed: Underground 2",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            static_cast<int>(config.screenWidth), static_cast<int>(config.screenHeight),
            windowFlags
        );
    }
    if (!g_window) {
        std::fprintf(stderr, "Failed to create SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    setD3D9Compatibility(true, 2);

#if defined(__linux__)
    // Rockchip RK3326 4x Cortex-A35 optimization:
    // Pin worker threads to Cores 1..3, keeping Core 0 free for OS interrupts and audio daemon
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset);
    CPU_SET(2, &cpuset);
    CPU_SET(3, &cpuset);
    sched_setaffinity(0, sizeof(cpu_set_t), &cpuset);
#endif

    setD3D9HostWindow(static_cast<void*>(g_window));
    setD3D9RenderSize(config.targetWidth, config.targetHeight);
    acknowledgeD3D9Surface(true);

    // Test guest virtual memory reservation
    try {
        testGuestMemory();
        std::printf("[NFSU2] Guest memory 4GB virtual space initialized.\n");
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[NFSU2] Guest memory initialization error: %s\n", ex.what());
        SDL_DestroyWindow(g_window);
        SDL_Quit();
        return 1;
    }

    // Configure guest runtime settings
    setGuestDisplaySize(config.targetWidth, config.targetHeight);
    setGuestResolution(config.targetWidth, config.targetHeight);
    setGuestFrameLimit(config.frameLimit);
    setGuestLanguage(config.language.c_str());
    setGuestWidescreen(config.widescreen);

    // Start guest boot thread
    std::string exePath = config.gameDir + "/SPEED2.EXE";
    if (!std::filesystem::exists(exePath)) {
        exePath = config.gameDir + "/speed2.exe";
    }

    if (std::getenv("NFSU2_VKBD_SHOW")) vkbd::state().visible = true;  // smoke tests: show the overlay

    std::printf("[NFSU2] Launching guest executable: %s\n", exePath.c_str());
    g_guestWorker.start(exePath);

    // Main event loop
    const auto frameDuration = std::chrono::microseconds(1000000 / (config.frameLimit ? config.frameLimit : 60));
    auto lastFrameTime = std::chrono::steady_clock::now();


    uint32_t autoTick = 0;
    while (g_running) {
        if (g_guestWorker.isFinished()) {
            std::printf("[NFSU2] Guest runtime terminated (%s). Exiting main loop.\n", g_guestWorker.status().c_str());
            g_running = false;
            break;
        }
        auto frameStart = std::chrono::steady_clock::now();

        // NFSU2_KEY_SCRIPT="start;frame:key[:frames],..." (smoke tests): from frame `start` on, the automatic menu
        // navigation stops and only these key taps reach the game (key: enter esc up down left right space tab,
        // or a hex DIK scan code; held for `frames`, default 4).
        static const char* keyScript = std::getenv("NFSU2_KEY_SCRIPT");
        static uint32_t keyScriptStart = keyScript ? static_cast<uint32_t>(std::atoi(keyScript)) : 0;
        if (keyScript && getD3D9PresentCount() >= keyScriptStart) {
            struct Tap { uint32_t frame, scan, len; };
            static std::vector<Tap> taps = [] {
                std::vector<Tap> v;
                const char* p = std::strchr(keyScript, ';');
                std::string all = p ? p + 1 : "";
                size_t pos = 0;
                while (pos < all.size()) {
                    size_t end = all.find(',', pos); if (end == std::string::npos) end = all.size();
                    std::string item = all.substr(pos, end - pos); pos = end + 1;
                    unsigned f = 0, len = 4; char name[16] = {};
                    if (std::sscanf(item.c_str(), "%u:%15[^:]:%u", &f, name, &len) < 2) continue;
                    const std::string n = name;
                    uint32_t scan = n == "enter" ? 0x1c : n == "esc" ? 0x01 : n == "up" ? 0xc8 : n == "down" ? 0xd0 :
                                    n == "left" ? 0xcb : n == "right" ? 0xcd : n == "space" ? 0x39 : n == "tab" ? 0x0f :
                                    static_cast<uint32_t>(std::strtoul(name, nullptr, 16));
                    v.push_back({f, scan, len});
                }
                std::printf("[NFSU2] key script: %zu taps from frame %u\n", v.size(), keyScriptStart);
                return v;
            }();
            static bool released = false;
            if (!released) {   // let go of whatever the automatic navigation was holding
                for (unsigned sc : {0x1cu, 0x39u, 0x01u, 0xcbu}) setGuestKey(sc, false);
                g_pad.buttons = 0; g_pad.hatX = g_pad.hatY = 0; syncPadToGuest();
                released = true;
            }
            const uint32_t fr = getD3D9PresentCount();
            for (const Tap& t : taps) {
                if (fr == t.frame) { setGuestKey(t.scan, true); std::printf("[NFSU2] key script %u: %02x down\n", fr, t.scan); }
                if (fr == t.frame + t.len) setGuestKey(t.scan, false);
            }
        } else if (config.autoAdvance) {
            // Profile name: erase the default "NOMBRE" and type "R36s" (Shift+R tests capitals through
            // TranslateMessage; the field holds 7 characters).
            struct ScriptKey { unsigned frame, scan; bool down; };
            static const ScriptKey kNameScript[] = {
                {1590, 0x0e, true}, {1594, 0x0e, false}, {1598, 0x0e, true}, {1602, 0x0e, false},
                {1606, 0x0e, true}, {1610, 0x0e, false}, {1614, 0x0e, true}, {1618, 0x0e, false},
                {1622, 0x0e, true}, {1626, 0x0e, false}, {1630, 0x0e, true}, {1634, 0x0e, false},
                {1640, 0x2a, true}, {1643, 0x13, true}, {1651, 0x13, false}, {1654, 0x2a, false},
                {1660, 0x04, true}, {1668, 0x04, false}, {1672, 0x07, true}, {1680, 0x07, false},
                {1684, 0x1f, true}, {1692, 0x1f, false},
            };
            uint32_t frames = getD3D9PresentCount();
            for (const ScriptKey& k : kNameScript) if (frames == k.frame) setGuestKey(k.scan, k.down);
            if (frames >= 30 && frames < 1100) {
                // Skip only intro FMVs and copyright; do not auto-navigate the frontend.
                if ((frames % 10) == 0) {
                    setGuestKey(0x1c, true); // Enter
                    setGuestKey(0x39, true); // Space
                    setGuestKey(0x01, true); // Escape
                    g_pad.buttons = (1 << 0) | (1 << 1);
                    syncPadToGuest();
                } else if ((frames % 10) == 5) {
                    setGuestKey(0x1c, false);
                    setGuestKey(0x39, false);
                    setGuestKey(0x01, false);
                    g_pad.buttons = 0;
                    syncPadToGuest();
                }
            } else if (frames >= 1440 && frames < 1445) {
                // Pass the title prompt; the profile dialog follows it.
                setGuestKey(0x1c, true); // Enter
                g_pad.buttons = (1 << 0);
                syncPadToGuest();
            } else if (frames >= 1520 && frames < 1525) {
                // Choose “Sí” on the first-run profile prompt.
                setGuestKey(0x1c, false);
                setGuestKey(0xcb, true); // Left Arrow
                g_pad.hatX = -1;
                g_pad.hatY = 0;
                g_pad.buttons = 0;
                syncPadToGuest();
            } else if (frames >= 1560 && frames < 1565) {
                // Confirm profile creation after moving focus to “Sí”.
                setGuestKey(0xcb, false);
                g_pad.hatX = 0;
                setGuestKey(0x1c, true); // Enter
                g_pad.buttons = (1 << 0);
                syncPadToGuest();
            } else if (frames >= 1620 && frames < 1625) {
                g_pad.buttons = 0;
                syncPadToGuest();
            } else if (frames >= 1640 && frames < 1645) {
                g_pad.buttons = 0;
                syncPadToGuest();
            } else if (frames >= 1660 && frames < 1665) {
                g_pad.buttons = 0;
                syncPadToGuest();
            } else if (frames >= 1680 && frames < 1685) {
                g_pad.buttons = 0;
                syncPadToGuest();
            } else if (frames >= 1700 && frames < 1705) {
                setGuestKey(0x1c, true); // Enter
                g_pad.buttons = (1 << 0);
                syncPadToGuest();
            } else if (frames >= 2020 && frames < 2025) {
                // Leave the profile-save confirmation and park in the frontend.
                setGuestKey(0x1c, true); // Enter
                g_pad.buttons = (1 << 0);
                syncPadToGuest();
            } else if (frames >= 1820 && frames < 1825) {
                // Accept saving the default profile created by the headless flow.
                setGuestKey(0x1c, false);
                setGuestKey(0xcb, true); // Left Arrow -> “Sí”
                g_pad.hatX = -1;
                g_pad.buttons = 0;
                syncPadToGuest();
            } else if (frames >= 1860 && frames < 1865) {
                setGuestKey(0xcb, false);
                g_pad.hatX = 0;
                setGuestKey(0x1c, true); // Enter
                g_pad.buttons = (1 << 0);
                syncPadToGuest();
            } else if (frames >= 1100) {
                // Leave the frontend alone between the explicit prompt inputs.
                setGuestKey(0x1c, false);
                setGuestKey(0x39, false);
                setGuestKey(0x01, false);
                setGuestKey(0xcb, false);
                g_pad.hatX = 0;
                g_pad.hatY = 0;
                g_pad.buttons = 0;
                syncPadToGuest();
            }
            // NFSU2_AUTODRIVE=<frame>: from that frame hold the throttle and weave a little, so smoke runs see a
            // moving camera (the race starts ~2400 frames into an --auto-advance run).
            static const int autoDrive = getenv("NFSU2_AUTODRIVE") ? atoi(getenv("NFSU2_AUTODRIVE")) : 0;
            if (autoDrive && frames >= (uint32_t)autoDrive) {
                setGuestKey(0xc8, true);
                const uint32_t ph = (frames / 45) % 4;
                setGuestKey(0xcb, ph == 1);
                setGuestKey(0xcd, ph == 3);
            }
        }

        // NFSU2_GCOV_DUMP=<frame>: write the profile counters of a -fprofile-generate build (GCC or clang) at that
        // frame (smoke runs end with SIGKILL, so the exit-time dump never happens). pgo_vm.sh, pgo_clang_vm.sh.
        // Outside the automatic navigation, so key-script runs (driving) can be profiled too.
        {
            static const uint32_t gcovFrame = getenv("NFSU2_GCOV_DUMP") ? static_cast<uint32_t>(atoi(getenv("NFSU2_GCOV_DUMP"))) : 0;
            static bool written = false;
            if (gcovFrame && !written && getD3D9PresentCount() >= gcovFrame && (__gcov_dump || __llvm_profile_write_file)) {
                written = true;
                std::printf("[NFSU2] writing profile counters (frame %u)\n", getD3D9PresentCount()); std::fflush(stdout);
                if (__gcov_dump) __gcov_dump();
                if (__llvm_profile_write_file) __llvm_profile_write_file();
                std::printf("[NFSU2] profile counters written\n"); std::fflush(stdout);
            }
        }

        // NFSU2_TEST_COMBO=1 (smoke tests): Select+R3 at frame 2300, a Select tap at 2350, a long Select at 2400.
        static const bool testCombo = std::getenv("NFSU2_TEST_COMBO") != nullptr;
        // NFSU2_TEST_QUIT=<frame> (smoke tests): Select + Start at that frame.
        static const uint32_t testQuit = std::getenv("NFSU2_TEST_QUIT") ? std::atoi(std::getenv("NFSU2_TEST_QUIT")) : 0;
        if (testQuit) {
            static int step = 0;
            const uint32_t f = getD3D9PresentCount();
            auto push = [](uint8_t b) { SDL_Event e{}; e.type = SDL_CONTROLLERBUTTONDOWN; e.cbutton.button = b; e.cbutton.state = SDL_PRESSED; SDL_PushEvent(&e); };
            if (step == 0 && f >= testQuit) { push(SDL_CONTROLLER_BUTTON_BACK); step = 1; }
            else if (step == 1 && f >= testQuit + 2) { push(SDL_CONTROLLER_BUTTON_START); step = 2; }
        }
        if (testCombo) {
            static uint32_t lastFrame = 0;
            const uint32_t f = getD3D9PresentCount();
            if (f != lastFrame) {
                auto push = [](uint8_t b, bool down) {
                    SDL_Event e{}; e.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
                    e.cbutton.button = b; e.cbutton.state = down ? SDL_PRESSED : SDL_RELEASED; SDL_PushEvent(&e);
                };
                for (uint32_t k = lastFrame + 1; k <= f && lastFrame; ++k) {
                    switch (k) {
                        case 2300: case 2350: case 2400: push(SDL_CONTROLLER_BUTTON_BACK, true); break;
                        case 2302: push(SDL_CONTROLLER_BUTTON_RIGHTSTICK, true); break;
                        case 2304: push(SDL_CONTROLLER_BUTTON_RIGHTSTICK, false); break;
                        case 2306: case 2351: case 2425: push(SDL_CONTROLLER_BUTTON_BACK, false); break;
                    }
                }
                lastFrame = f;
            }
        }

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    g_running = false;
                    break;

                case SDL_CONTROLLERDEVICEADDED:
                    if (!controller) controller = openPad(event.cdevice.which);
                    break;

                case SDL_CONTROLLERDEVICEREMOVED:
                    if (controller && event.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller))) {
                        SDL_GameControllerClose(controller);
                        controller = nullptr;
                        g_gamepad.releaseAll(padSetKey);
                        publishGamepad();
                        setGuestPadConnected(false);
                    }
                    break;

                case SDL_JOYBUTTONDOWN:
                    // Raw button index, to check the mapping on a new device (see preparePadMapping).
                    std::printf("[NFSU2 PAD] raw button %d\n", event.jbutton.button);
                    break;

                case SDL_CONTROLLERAXISMOTION:
                    if (vkbdHandleAxis(event.caxis.axis, normalizeAxis(event.caxis.value))) break;
                    g_gamepad.axis(event.caxis.axis, event.caxis.value, guestDrivingControls(), padSetKey);
                    publishGamepad();
                    break;

                case SDL_CONTROLLERBUTTONDOWN:
                case SDL_CONTROLLERBUTTONUP: {
                    const bool down = (event.type == SDL_CONTROLLERBUTTONDOWN);
                    if (down) {
                        const char* n = SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(event.cbutton.button));
                        std::printf("[NFSU2 PAD] %s\n", n ? n : "?");
                    }
                    // R3 released after Select + R3: nothing for the keyboard to do.
                    if (!down && event.cbutton.button == SDL_CONTROLLER_BUTTON_RIGHTSTICK && g_select.combo) break;
                    if (selectHandleButton(event.cbutton.button, down)) break;
                    const bool wasVisible = vkbd::state().visible.load();
                    if (vkbdHandleButton(event.cbutton.button, down)) {
                        // Opening the keyboard: release whatever the game saw held.
                        if (!wasVisible && vkbd::state().visible.load()) { g_gamepad.releaseAll(padSetKey); publishGamepad(); }
                        break;
                    }
                    g_gamepad.button(event.cbutton.button, down, guestDrivingControls(), padSetKey);
                    publishGamepad();
                    break;
                }

                case SDL_KEYDOWN:
                case SDL_KEYUP: {
                    bool down = (event.type == SDL_KEYDOWN);
                    unsigned scan = scanFromKeycode(event.key.keysym.sym);
                    if (scan) {
                        setGuestKey(scan, down);
                    }
                    if (event.key.keysym.sym == SDLK_F10 && down) {
                        g_running = false;
                    }
                    break;
                }
            }
        }

        // Virtual keyboard: release injected keys when their hold time is over.
        vkbdFlushPending(false);
        selectTick();
        if (g_quitByCombo) g_running = false;
        // Remap held buttons when the game switches between menus and driving.
        if (g_gamepad.refresh(guestDrivingControls(), padSetKey)) publishGamepad();

        // Check if D3D9 backend requested surface
        if (d3d9RequestsSurface()) {
            acknowledgeD3D9Surface(true);
        }

        // Keep event loop responsive for KMSDRM display flips while avoiding 100% CPU on Core 0
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    std::printf("[NFSU2] Shutting down...\n");
    quiesceGuestFilesForExit(3000);
    // Saves are closed and the card is synced. Tearing down the guest threads, the render thread and the
    // display from here sometimes crashed or hung (players then cut the power: card damage), and nothing
    // is left to write, so end the process now: the kernel releases the display, audio and files.
    if (!std::getenv("NFSU2_FULL_SHUTDOWN")) {
        std::printf("[NFSU2] Clean exit.\n");
        std::_Exit(0);
    }
    requestGuestRuntimeStop();
    shutdownD3D9Bridge();

    if (controller) {
        SDL_GameControllerClose(controller);
    }
    if (g_window) {
        SDL_DestroyWindow(g_window);
    }
    SDL_Quit();

    std::printf("[NFSU2] Clean exit.\n");
    ::sync();
    std::_Exit(0);
}
