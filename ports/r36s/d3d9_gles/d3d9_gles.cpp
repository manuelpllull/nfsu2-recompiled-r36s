// Direct3D 9 -> OpenGL ES 2.0 translation for the R36S port.
//
// This file must behave like a D3D9 driver: it forwards the game's own shaders, constants,
// textures and states. It must not classify meshes, invent materials, read guest memory or
// substitute textures. See HANDOVER_FOR_OPENCODE.md section 6 for the history behind this rule.

#include "d3d9_gles_device.h"
#include "vkbd.h"
#include "vkbd_font.h"

bool guestDrivingControls();  // guest_runtime.cpp: the game is reading driving controls
extern "C" void __gcov_dump(void) __attribute__((weak));  // libgcov, only in -fprofile-generate builds
extern "C" bool nfsParallelGuest();  // guest_runtime.cpp: guest threads run in parallel

#include <algorithm>
#include <cmath>
#include <csignal>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <dlfcn.h>
#if !defined(__ANDROID__)
#include <execinfo.h>
#endif
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <thread>
#include <unistd.h>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_set>


// Global (not in any namespace): d3d9_gles_resources.h and the D3D9 bridge refer to it.
thread_local bool g_glesDeferBufferGL = false;
thread_local bool g_glesDeferTextureGL = false;

// Panel rotation in degrees (0, 90, 270), set by the host before the device exists (main.cpp, NFSU2_ROTATE).
uint32_t g_nfsScreenRotation = 0;
extern "C" void nfs_widescreen_set_resolution(unsigned width, unsigned height);

namespace {
std::mutex g_windowMutex;
void* g_hostWindow = nullptr;
HWND g_windowHandle = nullptr;
uint32_t g_renderWidth = 640;
uint32_t g_renderHeight = 480;

// glGetError after every draw costs a driver round trip per draw; only with NFSU2_GL_CHECK=1 (the
// headless smoke test sets it).
inline bool glCheckEnabled() { static const bool on = std::getenv("NFSU2_GL_CHECK") != nullptr; return on; }
inline void checkGLError(const char* func, int line) {
    if (!glCheckEnabled()) return;
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        static int s_errCount = 0;
        if (s_errCount++ < 50) {
            std::fprintf(stderr, "[GL ERROR 0x%X] in %s at line %d\n", err, func, line);
        }
    }
}

bool hasGLExtension(const char* name) {
    const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (!ext) return false;
    const size_t n = std::strlen(name);
    for (const char* p = ext; (p = std::strstr(p, name)) != nullptr; p += n) {
        if ((p == ext || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return true;
    }
    return false;
}

// NFSU2_SEGV_TRACE=1: print a host backtrace on SIGSEGV (diagnostics; build links with -rdynamic).
struct SegvTraceInstaller {
    SegvTraceInstaller() {
        if (!std::getenv("NFSU2_SEGV_TRACE")) return;
        struct sigaction sa{};
        sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
        sa.sa_sigaction = [](int, siginfo_t* info, void*) {
#if !defined(__ANDROID__)
            void* frames[64];
            int n = backtrace(frames, 64);
            char msg[96];
            int len = std::snprintf(msg, sizeof(msg), "\n[SEGV] fault address %p\n", info ? info->si_addr : nullptr);
            if (len > 0) { ssize_t w = write(2, msg, static_cast<size_t>(len)); (void)w; }
            backtrace_symbols_fd(frames, n, 2);
#else
            (void)info;
#endif
        };
        sigaction(SIGSEGV, &sa, nullptr);
    }
} g_segvTraceInstaller;

// NFSU2_HANG_DUMP=<seconds>: if no frame is presented for that long, print a backtrace of every thread
// (diagnostics for the intermittent boot hang; see HANDOVER 6.9).
std::atomic<int64_t> g_lastPresentMs{0};
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct HangWatchdog {
    HangWatchdog() {
        const char* e = std::getenv("NFSU2_HANG_DUMP");
        if (!e) return;
        const int secs = std::atoi(e) > 0 ? std::atoi(e) : 20;
        struct sigaction sa{};
        sa.sa_flags = SA_RESTART;
        sa.sa_handler = [](int) {
#if !defined(__ANDROID__)
            void* frames[48];
            int n = backtrace(frames, 48);
            char msg[64];
            int len = std::snprintf(msg, sizeof(msg), "\n[HANG] thread %ld:\n", static_cast<long>(syscall(SYS_gettid)));
            if (len > 0) { ssize_t w = write(2, msg, static_cast<size_t>(len)); (void)w; }
            backtrace_symbols_fd(frames, n, 2);
#endif
        };
        sigaction(SIGUSR2, &sa, nullptr);
        g_lastPresentMs = nowMs();
        std::thread([secs] {
            bool dumped = false;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (!dumped && nowMs() - g_lastPresentMs.load() > secs * 1000LL) {
                    dumped = true;
                    std::fprintf(stderr, "[HANG] no Present for %d s, dumping thread backtraces\n", secs);
                    if (DIR* d = opendir("/proc/self/task")) {
                        while (dirent* ent = readdir(d)) {
                            const long tid = std::atol(ent->d_name);
                            if (tid > 0 && tid != static_cast<long>(syscall(SYS_gettid))) {
                                syscall(SYS_tgkill, getpid(), tid, SIGUSR2);
                                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                            }
                        }
                        closedir(d);
                    }
                }
            }
        }).detach();
    }
} g_hangWatchdog;

// NFSU2_PROFILE=<first frame>-<last frame>: sample the program counter of the running thread at 1 kHz
// (ITIMER_PROF) between two presents and write prof_samples.txt (pc + thread id) and prof_maps.txt.
// Draw calls issued since the last Present (counted in traceDraw).
uint32_t g_frameDraws = 0;
// Driver CPU inside glDraw*, measured on one draw in 8 (glsc::driverNs, see gl_state_cache.h).
uint32_t g_drawTick = 0;
struct DrawTimer : glsc::DriverCpuTimer {
    DrawTimer() : glsc::DriverCpuTimer((++g_drawTick & 7u) == 0 ? 8.0 : 0.0) {}
};

constexpr int kProfMax = 1 << 20;
uint64_t g_profPc[kProfMax];
uint32_t g_profTid[kProfMax];
std::atomic<int> g_profCount{0};
std::atomic<bool> g_profOn{false};
void profHandler(int, siginfo_t*, void* uc) {
    if (!g_profOn.load(std::memory_order_relaxed)) return;
    const int i = g_profCount.fetch_add(1, std::memory_order_relaxed);
    if (i >= kProfMax) return;
    g_profPc[i] = static_cast<ucontext_t*>(uc)->uc_mcontext.pc;
    g_profTid[i] = static_cast<uint32_t>(syscall(SYS_gettid));
}
// Wall-clock samples of the game thread (CLOCK_MONOTONIC, 1 kHz, own signal): unlike the CPU-time samples
// they also land while the thread is blocked or waiting for a core, so they show where wall time goes.
// Each sample: pc, lr and two return addresses from the frame-pointer chain (prof_wall.txt).
extern std::atomic<long> g_gameTid;
constexpr int kWallMax = 1 << 16;
constexpr int kWallDepth = 48;
uint64_t g_wall[kWallMax][kWallDepth];
std::atomic<int> g_wallCount{0};
// backtrace() unwinds with the .eh_frame tables, through libc and the signal frame (the frame-pointer chain
// stopped inside libc, which has no frame records). Called once at setup so libgcc is already loaded.
void wallHandler(int, siginfo_t*, void*) {
    if (!g_profOn.load(std::memory_order_relaxed)) return;
    const int i = g_wallCount.fetch_add(1, std::memory_order_relaxed);
    if (i >= kWallMax) return;
#if !defined(__ANDROID__)
    void* frames[kWallDepth + 2] = {};
    const int n = backtrace(frames, kWallDepth + 2);
    for (int k = 0; k < kWallDepth; ++k) g_wall[i][k] = k + 2 < n ? reinterpret_cast<uint64_t>(frames[k + 2]) : 0;  // skip handler + trampoline
#else
    (void)i;
#endif
}
void profileWrite(int first, int last) {
    {
        const int n = std::min(g_wallCount.load(), kWallMax);
        if (FILE* f = std::fopen("prof_wall.txt", "w")) {
            for (int i = 0; i < n; ++i) {
                for (int k = 0; k < kWallDepth; ++k) std::fprintf(f, k ? " %lx" : "%lx", (unsigned long)g_wall[i][k]);
                std::fputc('\n', f);
            }
            std::fclose(f);
        }
        std::printf("[NFSU2 PROFILE] %d wall-clock samples of the game thread written to prof_wall.txt\n", n);
    }
    const int n = std::min(g_profCount.load(), kProfMax);
    if (FILE* f = std::fopen("prof_samples.txt", "w")) {
        for (int i = 0; i < n; ++i) std::fprintf(f, "%lx %u\n", static_cast<unsigned long>(g_profPc[i]), g_profTid[i]);
        std::fclose(f);
    }
    if (FILE* in = std::fopen("/proc/self/maps", "r")) {
        if (FILE* out = std::fopen("prof_maps.txt", "w")) {
            char line[512];
            while (std::fgets(line, sizeof(line), in)) std::fputs(line, out);
            std::fclose(out);
        }
        std::fclose(in);
    }
    std::printf("[NFSU2 PROFILE] %d samples over frames %d-%d written to prof_samples.txt\n", n, first, last);
}
// NFSU2_PROFILE=<first>-<last> (present indices), or NFSU2_PROFILE=race[:<frames>]: start once the game
// has been reading driving controls for 90 frames and sample <frames> frames (default 200). For on-device runs, where the race start frame is not known in advance.
void profileFrame(uint32_t frame, uint32_t draws) {
    static int first = -2, last = -1, autoLen = 0, busyRun = 0;
    if (first == -2) {
        first = -1;
        if (const char* e = std::getenv("NFSU2_PROFILE")) {
            if (std::strncmp(e, "race", 4) == 0) {
                autoLen = e[4] == ':' ? std::atoi(e + 5) : 200;
                if (autoLen <= 0) autoLen = 200;
                first = 0x7fffffff;
            } else {
                std::sscanf(e, "%d-%d", &first, &last);
            }
        }
        if (first >= 0) {
            struct sigaction sa{};
            sa.sa_flags = SA_SIGINFO | SA_RESTART;
            sa.sa_sigaction = profHandler;
            sigaction(SIGPROF, &sa, nullptr);
            struct sigaction wa{};
            wa.sa_flags = SA_SIGINFO | SA_RESTART;
            wa.sa_sigaction = wallHandler;
            sigaction(SIGRTMIN + 3, &wa, nullptr);
#if !defined(__ANDROID__)
            { void* warm[4]; backtrace(warm, 4); }
#endif
        }
    }
    if (first < 0) return;
    if (autoLen && first == 0x7fffffff) {
        // In a race = the game reads driving controls; skip the first 90 frames (countdown, streaming).
        busyRun = guestDrivingControls() && draws > 100 ? busyRun + 1 : 0;
        if (busyRun >= 90) {
            first = static_cast<int>(frame);
            last = first + autoLen;
            std::printf("[NFSU2 PROFILE] race detected (%u draws/frame): sampling frames %d-%d\n", draws, first, last);
        }
    }
    if (static_cast<int>(frame) == first) {
        // One CPU-time timer per thread (1 kHz of that thread's own CPU time), signalled to that thread:
        // a process-wide ITIMER_PROF signal can land on any thread, including ones asleep in futex
        // waits, which made idle threads look busy. llvmpipe (VM software GPU) threads are skipped.
        if (DIR* d = opendir("/proc/self/task")) {
            while (dirent* e = readdir(d)) {
                if (e->d_name[0] == '.') continue;
                const int tid = std::atoi(e->d_name);
                char path[64], name[32] = {};
                std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
                if (FILE* f = std::fopen(path, "r")) { if (!std::fgets(name, sizeof(name), f)) name[0] = 0; std::fclose(f); }
                if (std::strncmp(name, "llvmpipe", 8) == 0) continue;
                const clockid_t clock = static_cast<clockid_t>((~static_cast<clockid_t>(tid) << 3) | 4 | 2);  // MAKE_THREAD_CPUCLOCK(tid, SCHED)
                sigevent ev{};
                ev.sigev_notify = 4;  // SIGEV_THREAD_ID
                ev.sigev_signo = SIGPROF;
                ev._sigev_un._tid = tid;
                timer_t timer;
                if (timer_create(clock, &ev, &timer) == 0) {
                    itimerspec its{};
                    its.it_interval.tv_nsec = 1000000;
                    its.it_value.tv_nsec = 1000000;
                    timer_settime(timer, 0, &its, nullptr);
                }
            }
            closedir(d);
        }
        if (const long gameTid = g_gameTid.load()) {
            sigevent ev{};
            ev.sigev_notify = 4;  // SIGEV_THREAD_ID
            ev.sigev_signo = SIGRTMIN + 3;
            ev._sigev_un._tid = static_cast<int>(gameTid);
            timer_t timer;
            if (timer_create(CLOCK_MONOTONIC, &ev, &timer) == 0) {
                itimerspec its{};
                its.it_interval.tv_nsec = 1000000;
                its.it_value.tv_nsec = 1000000;
                timer_settime(timer, 0, &its, nullptr);
            }
        }
        g_profOn = true;
    }
    if (static_cast<int>(frame) == last) {
        g_profOn = false;
        // Written in the background: formatting ~28k stacks took the presenting thread over a second
        // (a visible freeze in the v14 device test).
        std::thread([] { profileWrite(first, last); }).detach();
    }
}

// Every 120 frames: where the process's CPU went (all threads, by name), page faults and memory. On the
// R36S the game thread's wall time per frame was ~2x its CPU time in a race; this shows whether the rest
// is other threads holding the guest machine, the GL driver, or paging (1 GB RAM + swap on the SD card).
// The host thread that presents (the game's main thread).
std::atomic<long> g_gameTid{0};
// Wall time spent waiting, per frame (NFSU2 WAIT line): 0 game thread waiting for the previous Present,
// 1 game thread in synchronous D3D9 calls, 2 game thread waiting for the guest machine, 3 render thread
// inside SwapWindow, 4 render thread idle (queue empty); 5 count of synchronous calls.
std::atomic<uint64_t> g_waitNs[14];
std::atomic<uint64_t> g_waitFrameNs[14];  // the same, reset every frame (hitch lines)
// Critical sections the game thread waited for (address -> ns), top ones logged with the WAIT line.
std::mutex g_csWaitMutex;
std::map<uint32_t, uint64_t> g_csWait;
extern "C" void nfsReportFreeLockedCalls(unsigned frames);
extern "C" void nfsCsWait(uint32_t address, uint64_t ns) { std::lock_guard<std::mutex> l(g_csWaitMutex); g_csWait[address] += ns; }
extern "C" void nfsWaitAdd(int kind, uint64_t ns) { g_waitNs[kind].fetch_add(ns, std::memory_order_relaxed); g_waitFrameNs[kind].fetch_add(ns, std::memory_order_relaxed); }
extern "C" bool nfsIsGameThread() { static thread_local long tid = syscall(SYS_gettid); return tid == g_gameTid.load(std::memory_order_relaxed); }

// R36S speed model for headless runs (see HANDOVER 6.17). Per frame on the game thread:
//   portable = game-thread CPU - CPU inside the GL driver (game code, bridge and layer: runs the same code on
//              the device, K times slower; K measured from device logs: ~13 for race logic)
//   driver   = draws * c (the Mali driver's per-draw cost on the device; llvmpipe's here is not comparable)
// The other guest threads mostly run on timers, so their cost is per second (B = CPU ms per wall ms,
// scaled by Kb). The guest machine runs one guest thread at a time, so on the device a frame takes
//   T = (K*portable + c*draws) / (1 - Kb*B).
// llvmpipe threads are excluded (on the R36S rasterization is done by the Mali GPU).
// Overrides: NFSU2_R36S_K, NFSU2_R36S_KBG, NFSU2_R36S_DRAW_US.
// NFSU2_R36S_EMULATE=1 (smoke test): while driving, stretch each frame to the R36S model's frame time, so
// the game sees the device's frame time. Its simulation steps with the real frame time (up to 100 ms =
// 180 ticks, ~3x the 60 ticks of a 30 fps frame), and that work only shows up when frames are as slow as
// on the device. Runs on the game thread when there is a render thread (rt::tryDefer, Present), so that
// render-thread work and synchronous calls are not inflated by the pacing sleep.
std::atomic<double> g_r36sFrameMs{0};
bool g_paceOnGameThread = false;
extern "C" void nfsSetPaceOnGameThread(bool on) { g_paceOnGameThread = on; }
extern "C" void nfsEmulatePace() {
    static const bool emulate = [] { const char* e = std::getenv("NFSU2_R36S_EMULATE"); return e && e[0] == '1'; }();
    if (!emulate) return;
    static std::chrono::steady_clock::time_point lastFrame{};
    const auto now = std::chrono::steady_clock::now();
    const double ms = g_r36sFrameMs.load(std::memory_order_relaxed);
    if (ms > 0 && guestDrivingControls() && lastFrame.time_since_epoch().count()) {
        const auto target = lastFrame + std::chrono::microseconds(static_cast<long long>(ms * 1000.0));
        if (target > now) std::this_thread::sleep_until(target);
    }
    lastFrame = std::chrono::steady_clock::now();
}
double envNum(const char* name, double def) { const char* e = std::getenv(name); const double v = e ? std::atof(e) : 0; return v > 0 ? v : def; }
void r36sModel(double gameMsPerFrame, double renderMsPerFrame, double drawMsPerFrame, double draws, double backgroundFraction,
               double freeFraction, double wallMsPerFrame) {
    static const double K = envNum("NFSU2_R36S_K", 13.0), Kb = envNum("NFSU2_R36S_KBG", 7.5), drawUs = envNum("NFSU2_R36S_DRAW_US", 40.0);
    const double kb = Kb * backgroundFraction;
    double game, render = 0;
    if (renderMsPerFrame > 0) {
        // Render thread: D3D9 calls run there, in parallel with the game thread (one frame in flight).
        game = K * gameMsPerFrame;
        render = K * std::max(0.0, renderMsPerFrame - drawMsPerFrame) + draws * drawUs / 1000.0;
    } else {
        game = K * std::max(0.0, gameMsPerFrame - drawMsPerFrame) + draws * drawUs / 1000.0;
    }
    // Guest threads that take turns with the game thread (all of them, or all but the free-running sound
    // thread with parallel guest threads) stretch its frame; the free one needs its own core only.
    const double kf = Kb * freeFraction;
    const double game2 = kb < 0.95 ? game / (1.0 - kb) : 1e9;
    const double frame = std::max(game2, render);
    const double t = kf < 1.0 ? frame : frame * kf;
    g_r36sFrameMs.store(t, std::memory_order_relaxed);
    std::printf("[R36S model] game thread %.0f ms/frame, render thread %.0f ms/frame (%.0f draws x%.0f us), threads taking turns"
                " %.0f%% / in parallel %.0f%% of a core -> %.0f ms/frame = %.1f fps on an R36S (host %.1f fps)\n",
                game, render, draws, drawUs, kb * 100.0, kf * 100.0, t, 1000.0 / t, 1000.0 / wallMsPerFrame);
}

// GPU load for the [NFSU2 GPU] line (never used for rendering). Sources, the first that works:
//   fdinfo: Panfrost/DRM per-process engine busy time (/proc/self/fdinfo/<drm fd>: drm-engine-*: N ns),
//           exact GPU time of this process;
//   devfreq: /sys/class/devfreq/<gpu>/load ("NN@freq", Rockchip kernels), sampled every 10 frames;
//   kbase:  /sys/devices/platform/<gpu>/utilisation (Mali blob driver), sampled every 10 frames.
namespace {
struct GpuLoad {
    int kind = -1;              // -1 not probed, 0 none, 1 fdinfo, 2 devfreq, 3 kbase
    std::string path, freqPath, what;
    std::vector<std::string> fdinfos;
    std::map<std::string, uint64_t> lastNs;
    double sum = 0, mn = 1e9, mx = 0; int n = 0;
};
GpuLoad& gpuLoad() { static GpuLoad g; return g; }
bool readSmall(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char buf[4096]; size_t k = std::fread(buf, 1, sizeof buf - 1, f); std::fclose(f);
    buf[k] = 0; out = buf; return k > 0;
}
std::map<std::string, uint64_t> readEngines(const std::vector<std::string>& files) {
    std::map<std::string, uint64_t> ns;
    for (const std::string& p : files) {
        std::string t; if (!readSmall(p, t)) continue;
        size_t pos = 0;
        while ((pos = t.find("drm-engine-", pos)) != std::string::npos) {
            size_t colon = t.find(':', pos); if (colon == std::string::npos) break;
            const std::string name = t.substr(pos + 11, colon - pos - 11);
            ns[name] += std::strtoull(t.c_str() + colon + 1, nullptr, 10);
            pos = colon;
        }
    }
    return ns;
}
void gpuProbe(GpuLoad& g) {
    g.kind = 0;
    if (DIR* d = opendir("/proc/self/fdinfo")) {             // Panfrost / any DRM driver with fdinfo stats
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            const std::string p = std::string("/proc/self/fdinfo/") + e->d_name;
            std::string t;
            if (readSmall(p, t) && t.find("drm-engine-") != std::string::npos) g.fdinfos.push_back(p);
        }
        closedir(d);
        if (!g.fdinfos.empty()) { g.kind = 1; g.what = "drm fdinfo engine time"; g.lastNs = readEngines(g.fdinfos); return; }
    }
    if (DIR* d = opendir("/sys/class/devfreq")) {             // Rockchip devfreq load ("NN@freqHz")
        while (dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name.find("gpu") == std::string::npos) continue;
            const std::string base = "/sys/class/devfreq/" + name;
            std::string t;
            if (readSmall(base + "/load", t)) { g.kind = 2; g.path = base + "/load"; g.freqPath = base + "/cur_freq"; g.what = g.path; break; }
            if (g.freqPath.empty()) g.freqPath = base + "/cur_freq";
        }
        closedir(d);
        if (g.kind == 2) return;
    }
    const char* kb[] = {"/sys/devices/platform/ff400000.gpu/utilisation", "/sys/devices/platform/fde60000.gpu/utilisation",
                        "/sys/class/misc/mali0/device/utilisation"};
    for (const char* p : kb) { std::string t; if (readSmall(p, t)) { g.kind = 3; g.path = p; g.what = p; return; } }
}
// Called once per presented frame.
void gpuLoadFrame(uint32_t frame) {
    static const bool off = std::getenv("NFSU2_GPU_LOAD") && std::getenv("NFSU2_GPU_LOAD")[0] == '0';
    if (off) return;
    GpuLoad& g = gpuLoad();
    if (g.kind < 0) {
        if (frame < 60) return;    // the GL context and its DRM fd exist by now
        gpuProbe(g);
        std::printf("[NFSU2 GPU] load source: %s\n", g.kind ? g.what.c_str() : "none found (frequency only)");
    }
    if ((g.kind == 2 || g.kind == 3) && frame % 10 == 0) {
        std::string t;
        if (readSmall(g.path, t)) {
            const double v = std::atof(t.c_str());
            g.sum += v; g.n++; g.mn = std::min(g.mn, v); g.mx = std::max(g.mx, v);
        }
    }
}
// GPU time per frame, measured directly: on 8 frames out of every 240 the presenting thread waits for the GPU to
// finish the frame (glFinish) before swapping. "finish" is the GPU work still outstanding once the frame's commands
// are issued; "swap after finish" is then only the wait for the display (vsync). Without the probe that sum shows as
// SwapWindow time and cannot be told apart. NFSU2_GPU_PROBE=0 disables it.
struct GpuFinishProbe { double finishMs = 0, swapMs = 0; int frames = 0; };
GpuFinishProbe& gpuFinishProbe() { static GpuFinishProbe p; return p; }
bool gpuFinishProbeFrame(uint32_t frame) {
    static const bool off = std::getenv("NFSU2_GPU_PROBE") && std::getenv("NFSU2_GPU_PROBE")[0] == '0';
    return !off && frame > 300 && frame % 240 < 8;
}
// Called with the PERF line (every `frames` frames).
void gpuLoadReport(uint32_t frames, double wallMsPerFrame) {
    GpuFinishProbe& probe = gpuFinishProbe();
    if (probe.frames) {
        std::printf("[NFSU2 GPU] probe over %d frames: GPU finish %.1f ms/frame after the commands were issued, swap after finish %.1f ms/frame\n",
                    probe.frames, probe.finishMs / probe.frames, probe.swapMs / probe.frames);
        probe = GpuFinishProbe{};
    }
    GpuLoad& g = gpuLoad();
    if (g.kind < 0) return;
    std::string freq;
    if (!g.freqPath.empty() && readSmall(g.freqPath, freq)) freq = " at " + std::to_string(std::atoll(freq.c_str()) / 1000000) + " MHz";
    else freq.clear();
    if (g.kind == 1) {
        auto now = readEngines(g.fdinfos);
        std::string parts; double busiest = 0;
        for (auto& kv : now) {
            const double ms = (kv.second - g.lastNs[kv.first]) / 1e6 / frames;
            busiest = std::max(busiest, ms);
            char b[96]; std::snprintf(b, sizeof b, "%s%s %.1f ms/frame", parts.empty() ? "" : ", ", kv.first.c_str(), ms); parts += b;
        }
        g.lastNs = now;
        std::printf("[NFSU2 GPU] busy %s; busiest engine %.0f%% of the frame%s\n", parts.c_str(),
                    wallMsPerFrame > 0 ? 100.0 * busiest / wallMsPerFrame : 0.0, freq.c_str());
    } else if (g.n) {
        std::printf("[NFSU2 GPU] load %.0f%% (min %.0f, max %.0f over %d samples)%s\n", g.sum / g.n, g.mn, g.mx, g.n, freq.c_str());
        g.sum = 0; g.n = 0; g.mn = 1e9; g.mx = 0;
    } else if (!freq.empty()) {
        std::printf("[NFSU2 GPU] load n/a%s\n", freq.c_str());
    }
}
}  // namespace

void perfThreadsReport(uint32_t frames, double wallMsPerFrame, double drawMsPerFrame, double drawsPerFrame) {
    struct ThreadCpu { std::string name; double ms; };
    double gameMs = 0, backgroundMs = 0, renderMs = 0, freeMs = 0;
    static std::map<int, double> lastTicks;
    static double lastProc = 0;
    static long lastMaj = -1, lastMin = 0;
    const double tickMs = 1000.0 / sysconf(_SC_CLK_TCK);
    std::vector<ThreadCpu> rows;
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            const int tid = std::atoi(e->d_name);
            char path[64];
            std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
            FILE* f = std::fopen(path, "r");
            if (!f) continue;
            char buf[512] = {};
            const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
            std::fclose(f);
            buf[n] = 0;
            char* open = std::strchr(buf, '(');
            char* close = std::strrchr(buf, ')');
            if (!open || !close) continue;
            std::string name(open + 1, close);
            unsigned long ut = 0, st = 0;
            // fields after ")": state(3) ... utime(14) stime(15)
            std::sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st);
            const double ticks = static_cast<double>(ut + st);
            auto it = lastTicks.find(tid);
            const double delta = it == lastTicks.end() ? 0.0 : ticks - it->second;
            lastTicks[tid] = ticks;
            if (delta > 0) {
                const double ms = delta * tickMs / frames;
                rows.push_back({name + "/" + std::to_string(tid), ms});
                if (tid == g_gameTid.load()) gameMs += ms;
                else if (name == "render") renderMs += ms;
                else if (nfsParallelGuest() && (name.find("-726117") != std::string::npos || name.find("-7263c7") != std::string::npos)) freeMs += ms;  // free-running sound thread
                // Other guest threads ("g<id>-<entry>") take turns with the game thread under the machine;
                // host threads (SDL events/audio, GPU driver) have cores of their own.
                else if (name.size() > 2 && name[0] == 'g' && name[1] >= '0' && name[1] <= '9') backgroundMs += ms;
            }
        }
        closedir(d);
    }
    std::sort(rows.begin(), rows.end(), [](const ThreadCpu& a, const ThreadCpu& b) { return a.ms > b.ms; });
    timespec pc{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &pc);
    const double proc = pc.tv_sec * 1e3 + pc.tv_nsec / 1e6;
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    long rssKb = 0, swapKb = 0;
    if (FILE* f = std::fopen("/proc/self/status", "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            std::sscanf(line, "VmRSS: %ld", &rssKb);
            std::sscanf(line, "VmSwap: %ld", &swapKb);
        }
        std::fclose(f);
    }
    long hugeKb = 0;
    if (FILE* f = std::fopen("/proc/self/smaps_rollup", "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), f)) std::sscanf(line, "AnonHugePages: %ld", &hugeKb);
        std::fclose(f);
    }
    // CPU clock and SoC temperature: a throttling device shows up here (R36S thermal limits).
    long curKhz = 0, tempMilli = 0;
    if (FILE* f = std::fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r")) { if (std::fscanf(f, "%ld", &curKhz) != 1) curKhz = 0; std::fclose(f); }
    if (FILE* f = std::fopen("/sys/class/thermal/thermal_zone0/temp", "r")) { if (std::fscanf(f, "%ld", &tempMilli) != 1) tempMilli = 0; std::fclose(f); }
    long memAvailKb = 0;
    if (FILE* f = std::fopen("/proc/meminfo", "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), f)) std::sscanf(line, "MemAvailable: %ld", &memAvailKb);
        std::fclose(f);
    }
    if (lastMaj >= 0) {
        std::string top;
        double soft = 0;
        int shown = 0;
        for (const ThreadCpu& r : rows) {
            if (r.name.rfind("llvmpipe", 0) == 0 || r.name.rfind("gallium", 0) == 0) { soft += r.ms; continue; }
            if (shown++ >= 7) continue;
            char b[96];
            std::snprintf(b, sizeof(b), "%s%s %.1f", shown > 1 ? ", " : "", r.name.c_str(), r.ms);
            top += b;
        }
        if (soft > 0) { char b[64]; std::snprintf(b, sizeof(b), "; software GPU threads %.1f", soft); top += b; }
        {
            static uint64_t lr = 0, li = 0;
            const uint64_t r = glsc::requested(), i = glsc::issued();
            std::printf("[NFSU2 PERF] GL state calls %.0f/frame requested, %.0f/frame reached the driver\n",
                        double(r - lr) / frames, double(i - li) / frames);
            lr = r; li = i;
        }
        std::printf("[NFSU2 PERF] process CPU %.1f ms/frame; threads (ms/frame): %s; major faults %.2f/frame, minor %.0f/frame;"
                    " RSS %ld MB (huge pages %ld MB), swap %ld MB, MemAvailable %ld MB; textures GL %lld MB, CPU copies %lld MB, relocks %lld"
                    " (readback failed %lld); cpu0 %ld MHz, %.1f C\n",
                    (proc - lastProc) / frames, top.c_str(), double(ru.ru_majflt - lastMaj) / frames,
                    double(ru.ru_minflt - lastMin) / frames, rssKb / 1024, hugeKb / 1024, swapKb / 1024, memAvailKb / 1024,
                    static_cast<long long>(d3d9_gles::g_texGLBytes.load() >> 20), static_cast<long long>(d3d9_gles::g_texShadowBytes.load() >> 20),
                    static_cast<long long>(d3d9_gles::g_texRelocks.load()), static_cast<long long>(d3d9_gles::g_texReadbackFails.load()),
                    curKhz / 1000, tempMilli / 1000.0);
    }
    {
        static int64_t lu = 0, lb = 0, lw = 0, ld = 0;
        using namespace d3d9_gles;
        const int64_t u = g_bufUnlocks.load(), b = g_bufUnlockBytes.load(), w = g_bufUnlockWholeBytes.load(), d = g_bufDynamicUnlocks.load();
        if (lastMaj >= 0)
            std::printf("[NFSU2 PERF] buffer updates %.0f/frame (%.0f dynamic), %.1f KB/frame written into buffers totalling %.1f MB/frame\n",
                        double(u - lu) / frames, double(d - ld) / frames, double(b - lb) / frames / 1024.0, double(w - lw) / frames / 1048576.0);
        lu = u; lb = b; lw = w; ld = d;
    }
    if (lastMaj >= 0 && wallMsPerFrame > 0) r36sModel(gameMs, renderMs, drawMsPerFrame, drawsPerFrame, backgroundMs / wallMsPerFrame, freeMs / wallMsPerFrame, wallMsPerFrame);
    lastProc = proc;
    lastMaj = ru.ru_majflt;
    lastMin = ru.ru_minflt;
}

// Optional per-frame draw trace: NFSU2_TRACE_FRAME=<present index>
int traceFrame() {
    static int s = [] { const char* e = std::getenv("NFSU2_TRACE_FRAME"); return e ? std::atoi(e) : -1; }();
    return s;
}
}  // namespace

// Global WSI functions called by guest_runtime / d3d9_bridge
extern "C" {
#if defined(NFS_R36S)
IDirect3D9* Direct3DCreate9(UINT sdk) {
    std::printf("[D3D9 GLES] Direct3DCreate9 initialized for OpenGL ES 2.0 / Mali-G31 (SDK=%u)\n", sdk);
    return new d3d9_gles::IDirect3D9GLES();
}

void dxvkAndroidSetWindow(void* window) {
    std::lock_guard<std::mutex> lock(g_windowMutex);
    g_hostWindow = window;
    g_windowHandle = reinterpret_cast<HWND>(window);
}

HWND dxvkAndroidGetWindowHandle() {
    std::lock_guard<std::mutex> lock(g_windowMutex);
    return g_windowHandle;
}

void dxvkAndroidSetRenderSize(uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(g_windowMutex);
    g_renderWidth = width;
    g_renderHeight = height;
}
#else
IDirect3D9* Direct3DCreate9_GLES(UINT sdk) {
    std::printf("[D3D9 GLES] Direct3DCreate9 initialized for OpenGL ES 2.0 / 3.0 (SDK=%u)\n", sdk);
    return new d3d9_gles::IDirect3D9GLES();
}

void glesAndroidSetWindow(void* window) {
    std::lock_guard<std::mutex> lock(g_windowMutex);
    g_hostWindow = window;
    g_windowHandle = reinterpret_cast<HWND>(window);
}

HWND glesAndroidGetWindowHandle() {
    std::lock_guard<std::mutex> lock(g_windowMutex);
    return g_windowHandle;
}

void glesAndroidSetRenderSize(uint32_t width, uint32_t height) {
    std::lock_guard<std::mutex> lock(g_windowMutex);
    g_renderWidth = width;
    g_renderHeight = height;
}
#endif
}

extern "C" ptrdiff_t g_mem_base;
bool guestDrivingControls();

namespace d3d9_gles {

// =============================================================================
// IDirect3D9GLES
// =============================================================================

STDMETHODIMP IDirect3D9GLES::GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) {
    if (!pIdentifier) return D3DERR_INVALIDCALL;
    std::memset(pIdentifier, 0, sizeof(*pIdentifier));
    std::snprintf(pIdentifier->Driver, sizeof(pIdentifier->Driver), "nv4_disp.dll");
    std::snprintf(pIdentifier->Description, sizeof(pIdentifier->Description), "NVIDIA GeForce 6600 GT");
    std::snprintf(pIdentifier->DeviceName, sizeof(pIdentifier->DeviceName), "\\\\.\\DISPLAY1");
    pIdentifier->VendorId = 0x10DE;
    pIdentifier->DeviceId = 0x0140;
    pIdentifier->SubSysId = 0x00000000;
    pIdentifier->Revision = 0x00A2;
    pIdentifier->DriverVersion.HighPart = 0x0006000E;
    pIdentifier->DriverVersion.LowPart = 0x000A1A2B;
    return D3D_OK;
}

STDMETHODIMP IDirect3D9GLES::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) {
    if (!pMode) return D3DERR_INVALIDCALL;
    pMode->Width = g_renderWidth ? g_renderWidth : 640;
    pMode->Height = g_renderHeight ? g_renderHeight : 480;
    pMode->RefreshRate = 60;
    pMode->Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
}

STDMETHODIMP IDirect3D9GLES::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode) {
    return EnumAdapterModes(Adapter, D3DFMT_X8R8G8B8, 0, pMode);
}

STDMETHODIMP IDirect3D9GLES::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) {
    if (!pCaps) return D3DERR_INVALIDCALL;
    std::memset(pCaps, 0, sizeof(*pCaps));
    pCaps->DeviceType = DeviceType;
    pCaps->AdapterOrdinal = Adapter;
    pCaps->Caps2 = D3DCAPS2_CANRENDERWINDOWED | D3DCAPS2_FULLSCREENGAMMA;
    pCaps->PresentationIntervals = D3DPRESENT_INTERVAL_IMMEDIATE | D3DPRESENT_INTERVAL_ONE;
    pCaps->DevCaps = D3DDEVCAPS_EXECUTESYSTEMMEMORY | D3DDEVCAPS_EXECUTEVIDEOMEMORY |
                     D3DDEVCAPS_TLVERTEXSYSTEMMEMORY | D3DDEVCAPS_TLVERTEXVIDEOMEMORY |
                     D3DDEVCAPS_HWTRANSFORMANDLIGHT | D3DDEVCAPS_CANRENDERAFTERFLIP;
    pCaps->PrimitiveMiscCaps = D3DPMISCCAPS_CULLNONE | D3DPMISCCAPS_CULLCW | D3DPMISCCAPS_CULLCCW |
                               D3DPMISCCAPS_COLORWRITEENABLE | D3DPMISCCAPS_BLENDOP;
    pCaps->RasterCaps = D3DPRASTERCAPS_ZTEST | D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE |
                        D3DPRASTERCAPS_MIPMAPLODBIAS | D3DPRASTERCAPS_SCISSORTEST |
                        D3DPRASTERCAPS_DEPTHBIAS | D3DPRASTERCAPS_SLOPESCALEDEPTHBIAS;
    pCaps->ZCmpCaps = 0xFF;
    pCaps->SrcBlendCaps = 0x1FFF;
    pCaps->DestBlendCaps = 0x1FFF;
    pCaps->AlphaCmpCaps = 0xFF;
    pCaps->ShadeCaps = D3DPSHADECAPS_COLORGOURAUDRGB | D3DPSHADECAPS_SPECULARGOURAUDRGB |
                       D3DPSHADECAPS_ALPHAGOURAUDBLEND | D3DPSHADECAPS_FOGGOURAUD;
    pCaps->TextureCaps = D3DPTEXTURECAPS_PERSPECTIVE | D3DPTEXTURECAPS_POW2 | D3DPTEXTURECAPS_NONPOW2CONDITIONAL |
                         D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_CUBEMAP | D3DPTEXTURECAPS_MIPMAP |
                         D3DPTEXTURECAPS_MIPCUBEMAP | D3DPTEXTURECAPS_PROJECTED;
    pCaps->CubeTextureFilterCaps = pCaps->TextureFilterCaps =
        D3DPTFILTERCAPS_MINFPOINT | D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFPOINT |
        D3DPTFILTERCAPS_MAGFLINEAR | D3DPTFILTERCAPS_MIPFPOINT | D3DPTFILTERCAPS_MIPFLINEAR |
        D3DPTFILTERCAPS_MINFANISOTROPIC | D3DPTFILTERCAPS_MAGFANISOTROPIC;
    pCaps->TextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP |
                                D3DPTADDRESSCAPS_INDEPENDENTUV;
    pCaps->MaxTextureWidth = 2048;
    pCaps->MaxTextureHeight = 2048;
    pCaps->MaxVolumeExtent = 0;
    pCaps->MaxTextureRepeat = 8192;
    pCaps->MaxTextureAspectRatio = 2048;
    // Same as DXVK/Windows drivers. NFSU2 derives its world-texture filtering from this value: with
    // MaxAnisotropy = 1 it explicitly selects POINT min/mag filtering (blocky roads).
    pCaps->MaxAnisotropy = 16;
    pCaps->MaxVertexW = 1e10f;
    pCaps->GuardBandLeft = -8192.0f;
    pCaps->GuardBandTop = -8192.0f;
    pCaps->GuardBandRight = 8192.0f;
    pCaps->GuardBandBottom = 8192.0f;
    pCaps->StencilCaps = 0xFF;
    pCaps->FVFCaps = 8;
    pCaps->TextureOpCaps = 0x03FFFFFF & ~(D3DTEXOPCAPS_BUMPENVMAP | D3DTEXOPCAPS_BUMPENVMAPLUMINANCE);
    pCaps->MaxTextureBlendStages = 8;
    pCaps->MaxSimultaneousTextures = 8;
    pCaps->VertexProcessingCaps = D3DVTXPCAPS_TEXGEN | D3DVTXPCAPS_DIRECTIONALLIGHTS | D3DVTXPCAPS_LOCALVIEWER;
    pCaps->MaxActiveLights = 8;
    pCaps->MaxUserClipPlanes = 0;
    pCaps->MaxVertexBlendMatrices = 0;
    pCaps->MaxPrimitiveCount = 0xFFFFF;
    pCaps->MaxVertexIndex = 0xFFFFFF;
    pCaps->MaxStreams = 16;
    pCaps->MaxStreamStride = 256;
    pCaps->VertexShaderVersion = D3DVS_VERSION(3, 0);
    pCaps->MaxVertexShaderConst = 256;
    pCaps->PixelShaderVersion = D3DPS_VERSION(3, 0);
    pCaps->PixelShader1xMaxValue = 8.0f;
    pCaps->DeclTypes = D3DDTCAPS_UBYTE4 | D3DDTCAPS_UBYTE4N | D3DDTCAPS_SHORT2N | D3DDTCAPS_SHORT4N |
                       D3DDTCAPS_USHORT2N | D3DDTCAPS_USHORT4N;
    pCaps->NumSimultaneousRTs = 1;
    pCaps->MaxPointSize = 64.0f;
    return D3D_OK;
}

STDMETHODIMP IDirect3D9GLES::CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
                                          DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
                                          IDirect3DDevice9** ppReturnedDeviceInterface) {
    if (!ppReturnedDeviceInterface || !pPresentationParameters) return D3DERR_INVALIDCALL;
    SDL_Window* window = g_hostWindow ? reinterpret_cast<SDL_Window*>(g_hostWindow) : nullptr;
    std::printf("[D3D9 GLES] Creating IDirect3DDevice9GLES (%ux%u, windowed=%d, autodepth=%d fmt=%u)\n",
                pPresentationParameters->BackBufferWidth, pPresentationParameters->BackBufferHeight,
                pPresentationParameters->Windowed, pPresentationParameters->EnableAutoDepthStencil,
                static_cast<unsigned>(pPresentationParameters->AutoDepthStencilFormat));
    *ppReturnedDeviceInterface = new IDirect3DDevice9GLES(this, window, *pPresentationParameters);
    return D3D_OK;
}

// =============================================================================
// IDirect3DDevice9GLES: construction, defaults
// =============================================================================

// =============================================================================
// Screen scaler blit, and the presenter thread
// =============================================================================
// Scales the back-buffer texture onto the window (letterboxed; a rotated panel swaps width and height) and, in
// smoke tests, saves what the panel shows. Runs on the render thread, or on the presenter thread.
// scaleBlit talks to the driver directly: the glsc:: state cache (gl_state_cache.h) is one global shadow for
// the render thread's context, and the presenter thread (another context) must neither read nor update it.
// The render-thread caller invalidates the cache afterwards.
#pragma push_macro("glEnable")
#undef glEnable
#pragma push_macro("glDisable")
#undef glDisable
#pragma push_macro("glBlendFunc")
#undef glBlendFunc
#pragma push_macro("glBlendFuncSeparate")
#undef glBlendFuncSeparate
#pragma push_macro("glBlendEquation")
#undef glBlendEquation
#pragma push_macro("glBlendEquationSeparate")
#undef glBlendEquationSeparate
#pragma push_macro("glBlendColor")
#undef glBlendColor
#pragma push_macro("glDepthFunc")
#undef glDepthFunc
#pragma push_macro("glDepthMask")
#undef glDepthMask
#pragma push_macro("glDepthRangef")
#undef glDepthRangef
#pragma push_macro("glColorMask")
#undef glColorMask
#pragma push_macro("glCullFace")
#undef glCullFace
#pragma push_macro("glFrontFace")
#undef glFrontFace
#pragma push_macro("glPolygonOffset")
#undef glPolygonOffset
#pragma push_macro("glViewport")
#undef glViewport
#pragma push_macro("glScissor")
#undef glScissor
#pragma push_macro("glStencilFuncSeparate")
#undef glStencilFuncSeparate
#pragma push_macro("glStencilOpSeparate")
#undef glStencilOpSeparate
#pragma push_macro("glStencilMask")
#undef glStencilMask
#pragma push_macro("glClearColor")
#undef glClearColor
#pragma push_macro("glClearDepthf")
#undef glClearDepthf
#pragma push_macro("glClearStencil")
#undef glClearStencil
#pragma push_macro("glActiveTexture")
#undef glActiveTexture
#pragma push_macro("glBindTexture")
#undef glBindTexture
#pragma push_macro("glDeleteTextures")
#undef glDeleteTextures
#pragma push_macro("glBindBuffer")
#undef glBindBuffer
#pragma push_macro("glDeleteBuffers")
#undef glDeleteBuffers
#pragma push_macro("glBindFramebuffer")
#undef glBindFramebuffer
#pragma push_macro("glDeleteFramebuffers")
#undef glDeleteFramebuffers
#pragma push_macro("glBindRenderbuffer")
#undef glBindRenderbuffer
#pragma push_macro("glDeleteRenderbuffers")
#undef glDeleteRenderbuffers
#pragma push_macro("glUseProgram")
#undef glUseProgram
#pragma push_macro("glDeleteProgram")
#undef glDeleteProgram
#pragma push_macro("glEnableVertexAttribArray")
#undef glEnableVertexAttribArray
#pragma push_macro("glDisableVertexAttribArray")
#undef glDisableVertexAttribArray
#pragma push_macro("glVertexAttribPointer")
#undef glVertexAttribPointer
#pragma push_macro("glVertexAttrib4f")
#undef glVertexAttrib4f
#pragma push_macro("glPixelStorei")
#undef glPixelStorei
static void scaleBlit(GLuint tex, float bw, float bh, int screenW, int screenH, GLuint& prog, GLint& texLoc, uint32_t frame) {
    if (!prog) {
        const char* vs =
            "attribute vec2 a_pos; attribute vec2 a_uv; varying highp vec2 v_uv;\n"
            "void main(){ gl_Position = vec4(a_pos, 0.0, 1.0); v_uv = a_uv; }\n";
        const char* fs =
            "precision mediump float; uniform sampler2D u_tex; varying highp vec2 v_uv;\n"
            "void main(){ gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb, 1.0); }\n";
        GLuint v = 0, f = 0;
        d3d9_gles::compileGLShader(GL_VERTEX_SHADER, vs, v, "scaler VS");
        d3d9_gles::compileGLShader(GL_FRAGMENT_SHADER, fs, f, "scaler PS");
        prog = glCreateProgram();
        glAttachShader(prog, v);
        glAttachShader(prog, f);
        glBindAttribLocation(prog, 0, "a_pos");
        glBindAttribLocation(prog, 1, "a_uv");
        glLinkProgram(prog);
        texLoc = glGetUniformLocation(prog, "u_tex");
    }
    // Fit the image into the screen as the player sees it (a rotated panel swaps width and height),
    // keeping its aspect: black bars on the sides or top and bottom.
    const bool rot = g_nfsScreenRotation == 90 || g_nfsScreenRotation == 270;
    const float lw = static_cast<float>(rot ? screenH : screenW), lh = static_cast<float>(rot ? screenW : screenH);
    float ex = 1.0f, ey = 1.0f;
    if (bw / bh > lw / lh) ey = (lw / lh) / (bw / bh); else ex = (bw / bh) / (lw / lh);
    struct V { float x, y, u, v; };
    V q[4] = {{-ex, -ey, 0, 0}, {ex, -ey, 1, 0}, {-ex, ey, 0, 1}, {ex, ey, 1, 1}};
    for (V& p : q) {
        const float x = p.x, y = p.y;
        if (g_nfsScreenRotation == 90) { p.x = y; p.y = -x; }
        else if (g_nfsScreenRotation == 270) { p.x = -y; p.y = x; }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, screenW, screenH);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (ex < 1.0f || ey < 1.0f) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(texLoc, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    for (GLuint i = 2; i < 16; ++i) glDisableVertexAttribArray(i);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(V), &q[0].x);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(V), &q[0].u);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    // Smoke tests: what the panel shows (after scaling), next to the back-buffer captures.
    static const bool diag = std::getenv("NFSU2_DIAG_FRAMES") != nullptr;
    if (diag && (frame == 1250 || frame == 1800 || frame == 2300)) {
        std::vector<uint8_t> px(static_cast<size_t>(screenW) * screenH * 4);
        glReadPixels(0, 0, screenW, screenH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        char path[64];
        std::snprintf(path, sizeof path, "diag_screen_%u.ppm", frame);
        if (FILE* fp = std::fopen(path, "wb")) {
            std::fprintf(fp, "P6\n%d %d\n255\n", screenW, screenH);
            for (int y = screenH - 1; y >= 0; --y)
                for (int x = 0; x < screenW; ++x) std::fwrite(&px[(static_cast<size_t>(y) * screenW + x) * 4], 1, 3, fp);
            std::fclose(fp);
        }
    }
}

#pragma pop_macro("glEnable")
#pragma pop_macro("glDisable")
#pragma pop_macro("glBlendFunc")
#pragma pop_macro("glBlendFuncSeparate")
#pragma pop_macro("glBlendEquation")
#pragma pop_macro("glBlendEquationSeparate")
#pragma pop_macro("glBlendColor")
#pragma pop_macro("glDepthFunc")
#pragma pop_macro("glDepthMask")
#pragma pop_macro("glDepthRangef")
#pragma pop_macro("glColorMask")
#pragma pop_macro("glCullFace")
#pragma pop_macro("glFrontFace")
#pragma pop_macro("glPolygonOffset")
#pragma pop_macro("glViewport")
#pragma pop_macro("glScissor")
#pragma pop_macro("glStencilFuncSeparate")
#pragma pop_macro("glStencilOpSeparate")
#pragma pop_macro("glStencilMask")
#pragma pop_macro("glClearColor")
#pragma pop_macro("glClearDepthf")
#pragma pop_macro("glClearStencil")
#pragma pop_macro("glActiveTexture")
#pragma pop_macro("glBindTexture")
#pragma pop_macro("glDeleteTextures")
#pragma pop_macro("glBindBuffer")
#pragma pop_macro("glDeleteBuffers")
#pragma pop_macro("glBindFramebuffer")
#pragma pop_macro("glDeleteFramebuffers")
#pragma pop_macro("glBindRenderbuffer")
#pragma pop_macro("glDeleteRenderbuffers")
#pragma pop_macro("glUseProgram")
#pragma pop_macro("glDeleteProgram")
#pragma pop_macro("glEnableVertexAttribArray")
#pragma pop_macro("glDisableVertexAttribArray")
#pragma pop_macro("glVertexAttribPointer")
#pragma pop_macro("glVertexAttrib4f")
#pragma pop_macro("glPixelStorei")

#ifndef __ANDROID__
// Presenter thread (NFSU2_PRESENT_THREAD=0 turns it off).
//
// Without it the render thread ends every frame with SDL_GL_SwapWindow, and on the R36S's Mali that call
// waits until the GPU has finished the whole frame (17-21 ms of an idle CPU in the device logs, swap
// 0.3 ms when the GPU is already idle). The next frame's draw calls could not start before, so a frame cost
// CPU time + GPU time (~24 + 19 ms) instead of the larger of the two.
// Now the game draws into one of N off-screen back buffers; at Present the render thread flushes, puts a
// fence behind the frame and hands the buffer to this thread, then starts the next frame in the next
// buffer. This thread owns the window: it waits for the fence, scales the buffer onto the window and swaps
// (blocking as long as the GPU needs). The render context is made surfaceless (EGL_KHR_surfaceless_context);
// the two contexts share textures. Any missing piece leaves the old path in place.
namespace presenter {
constexpr int kMaxSlots = 3;
// readFence: signalled when the GPU has finished the presenter's blit out of the slot.
struct Slot { GLuint tex = 0, fbo = 0; bool busy = false; void* readFence = nullptr; };
struct Item { int slot; GLuint tex; void* fence; float bw, bh; int sw, sh; uint32_t frame; };
typedef void* (*PfnCur)(void);
typedef unsigned (*PfnMakeCurrent)(void*, void*, void*, void*);
typedef const char* (*PfnQueryString)(void*, int);
typedef void* (*PfnFenceSync)(GLenum, GLbitfield);
typedef GLenum (*PfnClientWait)(void*, GLbitfield, uint64_t);
typedef void (*PfnWaitSync)(void*, GLbitfield, uint64_t);
typedef void (*PfnDeleteSync)(void*);

struct State {
    bool tried = false, on = false;
    int nSlots = 2, cur = 0;
    Slot slots[kMaxSlots];
    SDL_Window* window = nullptr;
    SDL_Window* hidden = nullptr;   // test mode only (NFSU2_PRESENT_HIDDEN=1): where the render context sits instead of EGL surfaceless
    SDL_GLContext ctx1 = nullptr, ctx2 = nullptr;
    void* dpy = nullptr;
    void* eglCtx1 = nullptr;
    PfnMakeCurrent eglMakeCurrent = nullptr;
    PfnFenceSync fenceSync = nullptr;
    PfnClientWait clientWait = nullptr;
    PfnWaitSync waitSync = nullptr;           // optional, else clientWait
    PfnDeleteSync deleteSync = nullptr;
    std::thread th;
    std::mutex m;
    std::condition_variable cvItem, cvFree;
    std::deque<Item> items;
    bool stop = false, ready = false, failed = false;
    std::atomic<uint64_t> waitSlotNs{0}, gpuWaitNs{0}, blitSwapNs{0}, frames{0}, fenceTimeouts{0};
} S;

bool wanted() {
    const char* e = std::getenv("NFSU2_PRESENT_THREAD");
    return !(e && e[0] == '0');
}
bool active() { return S.on; }
int slotCount() { return S.nSlots; }
GLuint currentFbo() { return S.slots[S.cur].fbo; }
GLuint currentTex() { return S.slots[S.cur].tex; }

static double ms(uint64_t ns, uint64_t n) { return n ? ns / 1e6 / n : 0.0; }

static void threadMain() {
    pthread_setname_np(pthread_self(), "present");
    if (SDL_GL_MakeCurrent(S.window, S.ctx2) != 0) {
        std::lock_guard<std::mutex> l(S.m);
        S.failed = true;
        S.cvFree.notify_all();
        return;
    }
    SDL_GL_SetSwapInterval(1);
    GLuint prog = 0;
    GLint texLoc = -1;
    {
        std::lock_guard<std::mutex> l(S.m);
        S.ready = true;
        S.cvFree.notify_all();
    }
    uint64_t lastFrames = 0;
    for (;;) {
        Item it;
        {
            std::unique_lock<std::mutex> l(S.m);
            S.cvItem.wait(l, [] { return S.stop || !S.items.empty(); });
            if (S.items.empty()) break;
            it = S.items.front();
            S.items.pop_front();
        }
        using clk = std::chrono::steady_clock;
        const auto t0 = clk::now();
        if (it.fence) {
            const GLenum r = S.clientWait(it.fence, 1 /* GL_SYNC_FLUSH_COMMANDS_BIT */, 2000000000ull);
            if (r == 0x911B /* GL_TIMEOUT_EXPIRED */ || r == 0x911D /* GL_WAIT_FAILED */) ++S.fenceTimeouts;
            S.deleteSync(it.fence);
        }
        const auto t1 = clk::now();
        scaleBlit(it.tex, it.bw, it.bh, it.sw, it.sh, prog, texLoc, it.frame);
        SDL_GL_SwapWindow(S.window);
        // The blit is only queued: the render thread waits on this fence before reusing the slot.
        void* readFence = S.fenceSync(0x9117 /* GL_SYNC_GPU_COMMANDS_COMPLETE */, 0);
        glFlush();
        if (!readFence) glFinish();
        const auto t2 = clk::now();
        {
            std::lock_guard<std::mutex> l(S.m);
            S.slots[it.slot].readFence = readFence;
            S.slots[it.slot].busy = false;
        }
        S.cvFree.notify_all();
        S.gpuWaitNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        S.blitSwapNs += std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count();
        const uint64_t n = ++S.frames;
        if (n - lastFrames >= 300) {
            const uint64_t d = n - lastFrames;
            std::printf("[NFSU2 PRESENT] %llu frames, ms/frame: render thread waited for a free buffer %.1f, presenter waited for the GPU %.1f, blit + swap %.1f; fence timeouts %llu\n",
                        static_cast<unsigned long long>(d), ms(S.waitSlotNs.exchange(0), d), ms(S.gpuWaitNs.exchange(0), d), ms(S.blitSwapNs.exchange(0), d),
                        static_cast<unsigned long long>(S.fenceTimeouts.exchange(0)));
            lastFrames = n;
        }
    }
    SDL_GL_MakeCurrent(S.window, nullptr);
}

// Runs once, on the thread that owns the GL context (render thread). false = old path, with the reason logged.
bool init(SDL_Window* window, SDL_GLContext ctx1, int swapEffect) {
    if (S.tried) return S.on;
    S.tried = true;
    if (!wanted()) { std::printf("[NFSU2 PRESENT] presenter thread off (NFSU2_PRESENT_THREAD=0)\n"); return false; }
    auto fail = [](const char* why) { std::printf("[NFSU2 PRESENT] presenter thread not used: %s\n", why); return false; };
    // eglGetProcAddress only has to know extension functions (older Mali drivers return null for core EGL and
    // GLES3 names), so fall back to the libraries SDL already loaded.
    auto lookup = [](const char* name) -> void* {
        if (void* p = SDL_GL_GetProcAddress(name)) return p;
        for (const char* lib : {"libEGL.so.1", "libEGL.so", "libGLESv2.so.2", "libGLESv2.so"})
            if (void* h = dlopen(lib, RTLD_LAZY | RTLD_NOLOAD)) if (void* p = dlsym(h, name)) return p;
        return nullptr;
    };
    auto eglGetCurrentDisplay = reinterpret_cast<PfnCur>(lookup("eglGetCurrentDisplay"));
    auto eglGetCurrentContext = reinterpret_cast<PfnCur>(lookup("eglGetCurrentContext"));
    auto eglQueryString = reinterpret_cast<PfnQueryString>(lookup("eglQueryString"));
    S.eglMakeCurrent = reinterpret_cast<PfnMakeCurrent>(lookup("eglMakeCurrent"));
    S.fenceSync = reinterpret_cast<PfnFenceSync>(lookup("glFenceSync"));
    S.clientWait = reinterpret_cast<PfnClientWait>(lookup("glClientWaitSync"));
    S.waitSync = reinterpret_cast<PfnWaitSync>(lookup("glWaitSync"));
    S.deleteSync = reinterpret_cast<PfnDeleteSync>(lookup("glDeleteSync"));
    // Test mode for machines without EGL (the VM's SDL uses GLX): the render context sits on a hidden window.
    const bool hiddenMode = std::getenv("NFSU2_PRESENT_HIDDEN") != nullptr;
    if (!S.fenceSync || !S.clientWait || !S.deleteSync) return fail("no GL sync objects (needs OpenGL ES 3)");
    if (!hiddenMode) {
        if (!eglGetCurrentDisplay || !eglGetCurrentContext || !eglQueryString || !S.eglMakeCurrent) return fail("no EGL entry points");
        S.dpy = eglGetCurrentDisplay();
        S.eglCtx1 = eglGetCurrentContext();
        if (!S.dpy || !S.eglCtx1) return fail("not an EGL context");
        const char* ext = eglQueryString(S.dpy, 0x3055 /* EGL_EXTENSIONS */);
        if (!ext || !std::strstr(ext, "EGL_KHR_surfaceless_context")) return fail("EGL_KHR_surfaceless_context missing");
    }
    {   // a fence must work on this context
        while (glGetError() != GL_NO_ERROR) {}
        void* f = S.fenceSync(0x9117 /* GL_SYNC_GPU_COMMANDS_COMPLETE */, 0);
        glFlush();
        const bool ok = f && glGetError() == GL_NO_ERROR;
        if (f) { S.clientWait(f, 1, 1000000000ull); S.deleteSync(f); }
        if (!ok) return fail("glFenceSync does not work on this context");
    }
    int n = 2;
    if (const char* e = std::getenv("NFSU2_PRESENT_SLOTS")) n = std::atoi(e);
    S.nSlots = n < 2 ? 2 : (n > kMaxSlots ? kMaxSlots : n);
    S.window = window;
    S.ctx1 = ctx1;
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
    S.ctx2 = SDL_GL_CreateContext(window);   // becomes current on this thread
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
    SDL_GL_MakeCurrent(window, ctx1);
    if (!S.ctx2) return fail("could not create the second context");
    auto rollback = [&]() {   // give the window back to the render context
        SDL_GL_MakeCurrent(nullptr, nullptr);
        SDL_GL_MakeCurrent(window, ctx1);
        SDL_GL_DeleteContext(S.ctx2);
        S.ctx2 = nullptr;
        if (S.hidden) { SDL_DestroyWindow(S.hidden); S.hidden = nullptr; }
    };
    if (hiddenMode) {   // test mode: the render context sits on a hidden window
        S.hidden = SDL_CreateWindow("hidden", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
        if (!S.hidden || SDL_GL_MakeCurrent(S.hidden, ctx1) != 0) { rollback(); return fail("test mode: hidden window failed"); }
    } else if (S.eglMakeCurrent(S.dpy, nullptr, nullptr, S.eglCtx1) != 1) {   // render context without the window
        rollback();
        return fail("eglMakeCurrent without a surface failed");
    }
    S.stop = S.ready = S.failed = false;
    S.th = std::thread(threadMain);
    {
        std::unique_lock<std::mutex> l(S.m);
        S.cvFree.wait_for(l, std::chrono::seconds(5), [] { return S.ready || S.failed; });
    }
    if (!S.ready) {
        { std::lock_guard<std::mutex> l(S.m); S.stop = true; }
        S.cvItem.notify_all();
        if (S.th.joinable()) S.th.join();
        rollback();
        return fail("the presenter thread could not take the window");
    }
    S.on = true;
    std::printf("[NFSU2 PRESENT] presenter thread on: %d back buffers, swap effect %d (NFSU2_PRESENT_THREAD=0 turns it off)\n", S.nSlots, swapEffect);
    return true;
}

void setSlots(const GLuint* tex, const GLuint* fbo, int n) {
    for (int i = 0; i < n; ++i) { S.slots[i].tex = tex[i]; S.slots[i].fbo = fbo[i]; S.slots[i].busy = false; }
    S.cur = 0;
}

// Waits until the presenter has finished every frame handed to it.
void drain() {
    if (!S.on) return;
    std::unique_lock<std::mutex> l(S.m);
    S.cvFree.wait_for(l, std::chrono::seconds(10), [] {
        if (!S.items.empty()) return false;
        for (int i = 0; i < S.nSlots; ++i) if (S.slots[i].busy) return false;
        return true;
    });
}

void destroySlots() {
    for (int i = 0; i < kMaxSlots; ++i) {
        if (S.slots[i].readFence) {
            S.clientWait(S.slots[i].readFence, 0, 1000000000ull);
            S.deleteSync(S.slots[i].readFence);
        }
        if (S.slots[i].fbo) glDeleteFramebuffers(1, &S.slots[i].fbo);
        if (S.slots[i].tex) glDeleteTextures(1, &S.slots[i].tex);
        S.slots[i] = Slot();
    }
    S.cur = 0;
}

// End of frame on the render thread: hand the finished buffer over and switch to the next free one.
void submit(float bw, float bh, int sw, int sh, uint32_t frame) {
    using clk = std::chrono::steady_clock;
    Slot& s = S.slots[S.cur];
    void* fence = S.fenceSync(0x9117, 0);
    glFlush();
    if (!fence) glFinish();
    {
        std::lock_guard<std::mutex> l(S.m);
        s.busy = true;
        S.items.push_back(Item{S.cur, s.tex, fence, bw, bh, sw, sh, frame});
    }
    S.cvItem.notify_one();
    S.cur = (S.cur + 1) % S.nSlots;
    const auto t0 = clk::now();
    void* readFence = nullptr;
    {
        std::unique_lock<std::mutex> l(S.m);
        if (!S.cvFree.wait_for(l, std::chrono::seconds(5), [] { return !S.slots[S.cur].busy; })) {
            std::printf("[NFSU2 PRESENT] WARNING: the presenter did not release a buffer within 5 s\n");
            S.slots[S.cur].busy = false;
        }
        readFence = S.slots[S.cur].readFence;
        S.slots[S.cur].readFence = nullptr;
    }
    // Wait on the GPU, not the CPU, for the presenter's blit out of this slot.
    if (readFence) {
        if (S.waitSync) {
            S.waitSync(readFence, 0, 0xFFFFFFFFFFFFFFFFull /* GL_TIMEOUT_IGNORED */);
        } else {
            const GLenum r = S.clientWait(readFence, 0, 1000000000ull);
            if (r == 0x911B /* GL_TIMEOUT_EXPIRED */ || r == 0x911D /* GL_WAIT_FAILED */) ++S.fenceTimeouts;
        }
        S.deleteSync(readFence);
    }
    S.waitSlotNs += std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count();
}

void shutdown() {
    if (!S.on) return;
    drain();
    {
        std::lock_guard<std::mutex> l(S.m);
        S.stop = true;
    }
    S.cvItem.notify_all();
    if (S.th.joinable()) S.th.join();
    S.on = false;
    SDL_GL_MakeCurrent(nullptr, nullptr);    // the render context takes the window back
    SDL_GL_MakeCurrent(S.window, S.ctx1);
    if (S.ctx2) SDL_GL_DeleteContext(S.ctx2);
    S.ctx2 = nullptr;
    if (S.hidden) { SDL_DestroyWindow(S.hidden); S.hidden = nullptr; }
}
}  // namespace presenter
#endif

IDirect3DDevice9GLES::IDirect3DDevice9GLES(IDirect3D9GLES* parentD3D, SDL_Window* window, const D3DPRESENT_PARAMETERS& params)
    : m_parentD3D(parentD3D), m_sdlWindow(window), m_presentParams(params) {
    if (m_parentD3D) m_parentD3D->AddRef();
    glesCurrentDevice() = this;

#ifdef __ANDROID__
    ANativeWindow* aWindow = g_hostWindow ? reinterpret_cast<ANativeWindow*>(g_hostWindow) : nullptr;
    if (aWindow) {
        m_eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (m_eglDisplay == EGL_NO_DISPLAY) {
            std::fprintf(stderr, "[D3D9 GLES] eglGetDisplay failed\n");
        } else {
            EGLint major = 0, minor = 0;
            if (!eglInitialize(m_eglDisplay, &major, &minor)) {
                std::fprintf(stderr, "[D3D9 GLES] eglInitialize failed\n");
            } else {
                const EGLint attribs[] = {
                    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                    EGL_RED_SIZE, 8,
                    EGL_GREEN_SIZE, 8,
                    EGL_BLUE_SIZE, 8,
                    EGL_ALPHA_SIZE, 8,
                    EGL_DEPTH_SIZE, 24,
                    EGL_STENCIL_SIZE, 8,
                    EGL_NONE
                };
                EGLint numConfigs = 0;
                if (!eglChooseConfig(m_eglDisplay, attribs, &m_eglConfig, 1, &numConfigs) || numConfigs < 1) {
                    const EGLint fallbackAttribs[] = {
                        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                        EGL_RED_SIZE, 8,
                        EGL_GREEN_SIZE, 8,
                        EGL_BLUE_SIZE, 8,
                        EGL_DEPTH_SIZE, 24,
                        EGL_STENCIL_SIZE, 8,
                        EGL_NONE
                    };
                    eglChooseConfig(m_eglDisplay, fallbackAttribs, &m_eglConfig, 1, &numConfigs);
                }

                EGLint format = 0;
                eglGetConfigAttrib(m_eglDisplay, m_eglConfig, EGL_NATIVE_VISUAL_ID, &format);
                ANativeWindow_setBuffersGeometry(aWindow, 0, 0, format);
                m_eglSurface = eglCreateWindowSurface(m_eglDisplay, m_eglConfig, aWindow, nullptr);

                const EGLint ctxAttribs[] = {
                    EGL_CONTEXT_CLIENT_VERSION, 2,
                    EGL_NONE
                };
                m_eglContext = eglCreateContext(m_eglDisplay, m_eglConfig, EGL_NO_CONTEXT, ctxAttribs);
                if (m_eglContext != EGL_NO_CONTEXT) {
                    eglMakeCurrent(m_eglDisplay, m_eglSurface, m_eglSurface, m_eglContext);
                    eglSwapInterval(m_eglDisplay, 1);
                    std::printf("[D3D9 GLES] EGL & GLES 2.0 Context initialized successfully on Android!\n");
                    std::printf("  GL_VENDOR:   %s\n", glGetString(GL_VENDOR));
                    std::printf("  GL_RENDERER: %s\n", glGetString(GL_RENDERER));
                    std::printf("  GL_VERSION:  %s\n", glGetString(GL_VERSION));
                    __android_log_print(ANDROID_LOG_INFO, "NFSU2", "[D3D9 GLES] GPU: %s | %s | %s",
                        glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
                } else {
                    std::fprintf(stderr, "[D3D9 GLES] eglCreateContext failed: 0x%X\n", eglGetError());
                    __android_log_print(ANDROID_LOG_ERROR, "NFSU2", "[D3D9 GLES] eglCreateContext failed: 0x%X", eglGetError());
                }
            }
        }
    }
#else
    if (m_sdlWindow) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);  // destination alpha (see main.cpp)
        m_glContext = SDL_GL_CreateContext(m_sdlWindow);
        if (!m_glContext) {
            SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
            m_glContext = SDL_GL_CreateContext(m_sdlWindow);
        }
        if (m_glContext) {
            SDL_GL_MakeCurrent(m_sdlWindow, m_glContext);
            SDL_GL_SetSwapInterval(1);
            std::printf("[D3D9 GLES] GLES 2.0 Context initialized successfully!\n");
            std::printf("  GL_VENDOR:   %s\n", glGetString(GL_VENDOR));
            std::printf("  GL_RENDERER: %s\n", glGetString(GL_RENDERER));
            std::printf("  GL_VERSION:  %s\n", glGetString(GL_VERSION));
            GLint alphaBits = 0, depthBits = 0, stencilBits = 0;
            glGetIntegerv(GL_ALPHA_BITS, &alphaBits);
            glGetIntegerv(GL_DEPTH_BITS, &depthBits);
            glGetIntegerv(GL_STENCIL_BITS, &stencilBits);
            std::printf("  framebuffer: alpha=%d depth=%d stencil=%d%s\n", alphaBits, depthBits, stencilBits,
                        alphaBits ? "" : "  (WARNING: no destination alpha: minimap clipping will be wrong)");
        } else {
            std::fprintf(stderr, "[D3D9 GLES] Failed to create SDL GLES context: %s\n", SDL_GetError());
        }
    }
#endif

    GLESCaps& caps = glesCaps();
    caps.npot = hasGLExtension("GL_OES_texture_npot") || hasGLExtension("GL_ARB_texture_non_power_of_two");
    caps.depth24Stencil8 = hasGLExtension("GL_OES_packed_depth_stencil") || hasGLExtension("GL_EXT_packed_depth_stencil");
    caps.elementIndexUint = hasGLExtension("GL_OES_element_index_uint");
    caps.blendMinMax = hasGLExtension("GL_EXT_blend_minmax");
    caps.halfFloatVertex = hasGLExtension("GL_OES_vertex_half_float");
    glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &m_maxAttribs);
    if (m_maxAttribs > 16) m_maxAttribs = 16;
    std::printf("[D3D9 GLES] caps: npot=%d d24s8=%d uint=%d minmax=%d half=%d attribs=%d\n", caps.npot,
                caps.depth24Stencil8, caps.elementIndexUint, caps.blendMinMax, caps.halfFloatVertex, m_maxAttribs);

    const UINT w = params.BackBufferWidth ? params.BackBufferWidth : 640;
    const UINT h = params.BackBufferHeight ? params.BackBufferHeight : 480;
    m_defaultBackBuffer = new IDirect3DSurface9GLES(w, h, params.BackBufferFormat);
    m_defaultDepthBuffer = new IDirect3DSurface9GLES(w, h, params.AutoDepthStencilFormat);
    m_currentRenderTarget = m_defaultBackBuffer;
    m_currentDepthBuffer = params.EnableAutoDepthStencil ? m_defaultDepthBuffer : nullptr;
    m_rtWidth = w;
    m_rtHeight = h;

    glGenFramebuffers(1, &m_fbo);
    createBackBufferTarget(w, h);
    nfs_widescreen_set_resolution(w, h);

    // Unbound samplers read opaque black, like an unbound D3D9 sampler on common hardware.
    const uint8_t black[4] = {0, 0, 0, 255};
    glGenTextures(1, &m_dummy2D);
    glBindTexture(GL_TEXTURE_2D, m_dummy2D);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenTextures(1, &m_dummyCube);
    glBindTexture(GL_TEXTURE_CUBE_MAP, m_dummyCube);
    for (int f = 0; f < 6; ++f) {
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    resetStateDefaults();
    bindRenderTarget();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
}

void IDirect3DDevice9GLES::resetStateDefaults() {
    std::memset(m_rs, 0, sizeof(m_rs));
    m_rs[D3DRS_ZENABLE] = m_presentParams.EnableAutoDepthStencil ? D3DZB_TRUE : D3DZB_FALSE;
    m_rs[D3DRS_FILLMODE] = D3DFILL_SOLID;
    m_rs[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
    m_rs[D3DRS_ZWRITEENABLE] = TRUE;
    m_rs[D3DRS_ALPHATESTENABLE] = FALSE;
    m_rs[D3DRS_LASTPIXEL] = TRUE;
    m_rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    m_rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    m_rs[D3DRS_CULLMODE] = D3DCULL_CCW;
    m_rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    m_rs[D3DRS_ALPHAREF] = 0;
    m_rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    m_rs[D3DRS_ALPHABLENDENABLE] = FALSE;
    m_rs[D3DRS_FOGENABLE] = FALSE;
    m_rs[D3DRS_SPECULARENABLE] = FALSE;
    m_rs[D3DRS_FOGCOLOR] = 0;
    m_rs[D3DRS_FOGTABLEMODE] = D3DFOG_NONE;
    m_rs[D3DRS_FOGSTART] = 0;
    float one = 1.0f;
    std::memcpy(&m_rs[D3DRS_FOGEND], &one, 4);
    std::memcpy(&m_rs[D3DRS_FOGDENSITY], &one, 4);
    m_rs[D3DRS_STENCILENABLE] = FALSE;
    m_rs[D3DRS_STENCILFAIL] = D3DSTENCILOP_KEEP;
    m_rs[D3DRS_STENCILZFAIL] = D3DSTENCILOP_KEEP;
    m_rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
    m_rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
    m_rs[D3DRS_STENCILREF] = 0;
    m_rs[D3DRS_STENCILMASK] = 0xFFFFFFFF;
    m_rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
    m_rs[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFF;
    m_rs[D3DRS_CLIPPING] = TRUE;
    m_rs[D3DRS_LIGHTING] = TRUE;
    m_rs[D3DRS_COLORVERTEX] = TRUE;
    m_rs[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
    m_rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
    m_rs[D3DRS_FOGVERTEXMODE] = D3DFOG_NONE;
    m_rs[D3DRS_COLORWRITEENABLE] = 0xF;
    m_rs[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
    m_rs[D3DRS_SCISSORTESTENABLE] = FALSE;
    m_rs[D3DRS_TWOSIDEDSTENCILMODE] = FALSE;
    m_rs[D3DRS_CCW_STENCILFAIL] = D3DSTENCILOP_KEEP;
    m_rs[D3DRS_CCW_STENCILZFAIL] = D3DSTENCILOP_KEEP;
    m_rs[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
    m_rs[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
    m_rs[D3DRS_BLENDFACTOR] = 0xFFFFFFFF;
    m_rs[D3DRS_SEPARATEALPHABLENDENABLE] = FALSE;
    m_rs[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
    m_rs[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
    m_rs[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;

    for (int s = 0; s < kMaxTSS; ++s) {
        std::memset(m_tss[s], 0, sizeof(m_tss[s]));
        m_tss[s][D3DTSS_COLOROP] = (s == 0) ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        m_tss[s][D3DTSS_COLORARG1] = D3DTA_TEXTURE;
        m_tss[s][D3DTSS_COLORARG2] = D3DTA_CURRENT;
        m_tss[s][D3DTSS_ALPHAOP] = (s == 0) ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        m_tss[s][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        m_tss[s][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        m_tss[s][D3DTSS_TEXCOORDINDEX] = s;
        m_tss[s][D3DTSS_COLORARG0] = D3DTA_CURRENT;
        m_tss[s][D3DTSS_ALPHAARG0] = D3DTA_CURRENT;
        m_tss[s][D3DTSS_RESULTARG] = D3DTA_CURRENT;
        m_tss[s][D3DTSS_TEXTURETRANSFORMFLAGS] = D3DTTFF_DISABLE;
    }
    for (int s = 0; s < kMaxStages; ++s) {
        std::memset(m_ss[s], 0, sizeof(m_ss[s]));
        m_ss[s][D3DSAMP_ADDRESSU] = D3DTADDRESS_WRAP;
        m_ss[s][D3DSAMP_ADDRESSV] = D3DTADDRESS_WRAP;
        m_ss[s][D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
        m_ss[s][D3DSAMP_MAGFILTER] = D3DTEXF_POINT;
        m_ss[s][D3DSAMP_MINFILTER] = D3DTEXF_POINT;
        m_ss[s][D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
        m_ss[s][D3DSAMP_MAXANISOTROPY] = 1;
    }

    m_viewport = {0, 0, m_rtWidth, m_rtHeight, 0.0f, 1.0f};
    m_scissorRect = {0, 0, static_cast<LONG>(m_rtWidth), static_cast<LONG>(m_rtHeight)};
}

IDirect3DDevice9GLES::~IDirect3DDevice9GLES() {
    if (m_fbo) glDeleteFramebuffers(1, &m_fbo);
    if (m_dummy2D) glDeleteTextures(1, &m_dummy2D);
    if (m_dummyCube) glDeleteTextures(1, &m_dummyCube);
    destroyBackBufferTarget();
    if (m_defaultBackBuffer) m_defaultBackBuffer->Release();
    if (m_defaultDepthBuffer) m_defaultDepthBuffer->Release();
#ifdef __ANDROID__
    if (m_eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (m_eglContext != EGL_NO_CONTEXT) {
            eglDestroyContext(m_eglDisplay, m_eglContext);
            m_eglContext = EGL_NO_CONTEXT;
        }
        if (m_eglSurface != EGL_NO_SURFACE) {
            eglDestroySurface(m_eglDisplay, m_eglSurface);
            m_eglSurface = EGL_NO_SURFACE;
        }
        eglTerminate(m_eglDisplay);
        m_eglDisplay = EGL_NO_DISPLAY;
    }
#else
    presenter::shutdown();
    if (m_glContext && m_sdlWindow) SDL_GL_DeleteContext(m_glContext);
#endif
    if (m_parentD3D) m_parentD3D->Release();
}

void IDirect3DDevice9GLES::noteUnimplemented(const char* what) {
    static std::set<std::string> s_seen;
    if (s_seen.insert(what).second) {
        std::printf("[D3D9 GLES] NOTE: game called unimplemented %s (frame %u)\n", what, getD3D9PresentCount());
    }
}

STDMETHODIMP IDirect3DDevice9GLES::GetDirect3D(IDirect3D9** ppD3D9) {
    if (!ppD3D9) return D3DERR_INVALIDCALL;
    *ppD3D9 = m_parentD3D;
    if (m_parentD3D) m_parentD3D->AddRef();
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::GetDeviceCaps(D3DCAPS9* pCaps) {
    return m_parentD3D ? m_parentD3D->GetDeviceCaps(0, D3DDEVTYPE_HAL, pCaps) : E_FAIL;
}

STDMETHODIMP IDirect3DDevice9GLES::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode) {
    if (!pMode) return D3DERR_INVALIDCALL;
    pMode->Width = m_presentParams.BackBufferWidth;
    pMode->Height = m_presentParams.BackBufferHeight;
    pMode->RefreshRate = 60;
    pMode->Format = m_presentParams.BackBufferFormat;
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* pParameters) {
    if (!pParameters) return D3DERR_INVALIDCALL;
    pParameters->AdapterOrdinal = 0;
    pParameters->DeviceType = D3DDEVTYPE_HAL;
    pParameters->hFocusWindow = reinterpret_cast<HWND>(m_sdlWindow);
    pParameters->BehaviorFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING;
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** pSwapChain) {
    if (!pSwapChain || !pPresentationParameters) return D3DERR_INVALIDCALL;
    *pSwapChain = new IDirect3DSwapChain9GLES(pPresentationParameters->BackBufferWidth, pPresentationParameters->BackBufferHeight, pPresentationParameters->BackBufferFormat);
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain) {
    if (!pSwapChain) return D3DERR_INVALIDCALL;
    *pSwapChain = new IDirect3DSwapChain9GLES(m_presentParams.BackBufferWidth, m_presentParams.BackBufferHeight, m_presentParams.BackBufferFormat);
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) {
    if (pPresentationParameters) m_presentParams = *pPresentationParameters;
    {
        const UINT w = m_presentParams.BackBufferWidth ? m_presentParams.BackBufferWidth : 640;
        const UINT h = m_presentParams.BackBufferHeight ? m_presentParams.BackBufferHeight : 480;
        if (!m_defaultBackBuffer || m_defaultBackBuffer->getWidth() != w || m_defaultBackBuffer->getHeight() != h) {
            std::printf("[D3D9 GLES] Reset to %ux%u\n", w, h);
            destroyBackBufferTarget();
            if (m_defaultBackBuffer) m_defaultBackBuffer->Release();
            if (m_defaultDepthBuffer) m_defaultDepthBuffer->Release();
            m_defaultBackBuffer = new IDirect3DSurface9GLES(w, h, m_presentParams.BackBufferFormat);
            m_defaultDepthBuffer = new IDirect3DSurface9GLES(w, h, m_presentParams.AutoDepthStencilFormat);
            createBackBufferTarget(w, h);
        }
        nfs_widescreen_set_resolution(w, h);
    }
    m_currentRenderTarget = m_defaultBackBuffer;
    m_currentDepthBuffer = m_presentParams.EnableAutoDepthStencil ? m_defaultDepthBuffer : nullptr;
    m_fboDirty = true;
    resetStateDefaults();
    bindRenderTarget();
    return D3D_OK;
}

// -----------------------------------------------------------------------------
// Virtual keyboard overlay (see vkbd.h). Drawn into the default framebuffer just before the swap.
// -----------------------------------------------------------------------------
namespace {
struct OverlayVertex { float x, y; uint8_t r, g, b, a; float u, v; };

void overlayQuad(std::vector<OverlayVertex>& out, float x0, float y0, float x1, float y1, uint32_t rgba,
                 float u0 = -1, float v0 = -1, float u1 = -1, float v1 = -1) {
    const uint8_t r = rgba >> 24, g = (rgba >> 16) & 0xFF, b = (rgba >> 8) & 0xFF, a = rgba & 0xFF;
    const OverlayVertex q[6] = {{x0, y0, r, g, b, a, u0, v0}, {x1, y0, r, g, b, a, u1, v0}, {x1, y1, r, g, b, a, u1, v1},
                                {x0, y0, r, g, b, a, u0, v0}, {x1, y1, r, g, b, a, u1, v1}, {x0, y1, r, g, b, a, u0, v1}};
    out.insert(out.end(), q, q + 6);
}

void overlayText(std::vector<OverlayVertex>& out, float x, float y, const char* text, uint32_t rgba, float scale = 1.0f) {
    const float cell = static_cast<float>(kVkbdCell);
    for (const char* p = text; *p; ++p) {
        const int c = static_cast<unsigned char>(*p);
        if (c >= 33 && c < 128) {
            const int i = c - 32;
            const float u0 = (i % 16) * cell / kVkbdFontW, v0 = (i / 16) * cell / kVkbdFontH;
            overlayQuad(out, x, y, x + cell * scale, y + cell * scale, rgba, u0, v0, u0 + cell / kVkbdFontW, v0 + cell / kVkbdFontH);
        }
        x += 9.0f * scale;  // proportional-ish advance for the bold 13px font
    }
}

float overlayTextWidth(const char* text, float scale = 1.0f) { return std::strlen(text) * 9.0f * scale + 7.0f * scale; }

// Draws overlay triangles (virtual units: virtW x 480, scaled to the framebuffer) with the overlay font.
void submitOverlay(const std::vector<OverlayVertex>& verts, int fbW, int fbH, GLuint fbo, float virtW) {
    static GLuint prog = 0, tex = 0;
    static GLint scaleLoc = -1;
    if (!prog) {
        const char* vs =
            "attribute vec2 a_pos; attribute vec4 a_col; attribute vec2 a_uv; uniform vec2 u_scale;\n"
            "varying vec4 v_col; varying vec2 v_uv;\n"
            "void main(){ gl_Position = vec4(a_pos.x*u_scale.x-1.0, 1.0-a_pos.y*u_scale.y, 0.0, 1.0); v_col=a_col; v_uv=a_uv; }\n";
        const char* fs =
            "precision mediump float; uniform sampler2D u_tex; varying vec4 v_col; varying vec2 v_uv;\n"
            "void main(){ float a = v_uv.x < 0.0 ? 1.0 : texture2D(u_tex, v_uv).a; gl_FragColor = vec4(v_col.rgb, v_col.a*a); }\n";
        GLuint v = 0, f = 0;
        d3d9_gles::compileGLShader(GL_VERTEX_SHADER, vs, v, "vkbd VS");
        d3d9_gles::compileGLShader(GL_FRAGMENT_SHADER, fs, f, "vkbd PS");
        prog = glCreateProgram();
        glAttachShader(prog, v);
        glAttachShader(prog, f);
        glBindAttribLocation(prog, 0, "a_pos");
        glBindAttribLocation(prog, 1, "a_col");
        glBindAttribLocation(prog, 2, "a_uv");
        glLinkProgram(prog);
        scaleLoc = glGetUniformLocation(prog, "u_scale");
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, kVkbdFontW, kVkbdFontH, 0, GL_ALPHA, GL_UNSIGNED_BYTE, kVkbdFont);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, fbW, fbH);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glBlendEquation(GL_FUNC_ADD);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(prog);
    glUniform2f(scaleLoc, 2.0f / virtW, 2.0f / 480.0f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(prog, "u_tex"), 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    for (GLuint i = 0; i < 3; ++i) glEnableVertexAttribArray(i);
    for (GLuint i = 3; i < 16; ++i) glDisableVertexAttribArray(i);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(OverlayVertex), &verts[0].x);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(OverlayVertex), &verts[0].r);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(OverlayVertex), &verts[0].u);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size()));
    for (GLuint i = 0; i < 3; ++i) glDisableVertexAttribArray(i);
}

void drawVirtualKeyboard(int fbW, int fbH, GLuint fbo) {
    // Geometry in 640x480 virtual units, scaled to the framebuffer.
    std::vector<OverlayVertex> verts;
    verts.reserve(4096);
    const float unit = 58.0f, keyH = 34.0f, gap = 4.0f, left = 30.0f, top = 268.0f;
    overlayQuad(verts, 20, 236, 620, 470, 0x101820E0u);
    overlayQuad(verts, 20, 236, 620, 238, 0x9FE04FFFu);
    overlayText(verts, 30, 242, "B:type  Y:delete  X:shift  START:enter  A/L3/R3:close", 0xD8E8D0FFu, 0.9f);
    const auto& rows = vkbd::layout();
    const vkbd::State& st = vkbd::state();
    const bool upper = st.shift.load();
    for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
        float x = left;
        for (int c = 0; c < static_cast<int>(rows[r].size()); ++c) {
            const vkbd::Key& k = rows[r][c];
            const float w = k.width * unit - gap;
            const float y = top + r * (keyH + gap);
            const bool sel = (r == st.row.load() && c == st.col.load());
            const bool active = (k.special == vkbd::kShift && upper);
            overlayQuad(verts, x, y, x + w, y + keyH, sel ? 0x9FE04FFFu : (active ? 0x4A6A30FFu : 0x2C3440FFu));
            char label[8] = {0};
            switch (k.special) {
                case vkbd::kShift: std::snprintf(label, sizeof(label), upper ? "ABC" : "abc"); break;
                case vkbd::kSpace: std::snprintf(label, sizeof(label), "SPACE"); break;
                case vkbd::kDelete: std::snprintf(label, sizeof(label), "DEL"); break;
                case vkbd::kEnter: std::snprintf(label, sizeof(label), "OK"); break;
                default: label[0] = upper ? k.upper : k.lower; break;
            }
            const float tw = overlayTextWidth(label);
            overlayText(verts, x + (w - tw) * 0.5f + 3.0f, y + (keyH - 16.0f) * 0.5f, label, sel ? 0x101820FFu : 0xF0F0F0FFu);
            x += k.width * unit;
        }
    }

    submitOverlay(verts, fbW, fbH, fbo, 640.0f);
}

// Frame counter (R3 + Select, or NFSU2_SHOW_FPS=1): frames per second and average frame time over the last
// half second, top-left, in 480-line units so it keeps its shape on wide screens.
std::atomic<bool> g_fpsOverlay{std::getenv("NFSU2_SHOW_FPS") && std::getenv("NFSU2_SHOW_FPS")[0] == '1'};
void drawFpsCounter(int fbW, int fbH, GLuint fbo) {
    using clk = std::chrono::steady_clock;
    static clk::time_point windowStart = clk::now(), last = clk::now();
    static int frames = 0;
    static double worstMs = 0;
    static char text[48] = "-- FPS";
    static char text2[48] = "";
    const clk::time_point now = clk::now();
    const double frameMs = std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
    ++frames;
    worstMs = std::max(worstMs, frameMs);
    const double span = std::chrono::duration<double>(now - windowStart).count();
    if (span >= 0.5) {
        std::snprintf(text, sizeof text, "%.1f FPS", frames / span);
        std::snprintf(text2, sizeof text2, "%.1f ms  max %.0f", 1000.0 * span / frames, worstMs);
        frames = 0; worstMs = 0; windowStart = now;
    }
    const float virtW = 480.0f * static_cast<float>(fbW) / static_cast<float>(fbH);
    std::vector<OverlayVertex> verts;
    verts.reserve(256);
    const float w = std::max(overlayTextWidth(text), overlayTextWidth(text2, 0.75f)) + 4.0f;
    overlayQuad(verts, 6, 6, 6 + w, 6 + 32, 0x000000A0u);
    overlayText(verts, 10, 8, text, 0xFFFF40FFu);
    overlayText(verts, 10, 24, text2, 0xE0E0E0FFu, 0.75f);
    submitOverlay(verts, fbW, fbH, fbo, virtW);
}
}  // namespace

static std::atomic<uint32_t> g_d3d9PresentCount{0};

// The bridge's render thread calls this from the game's own thread (the one issuing D3D9 calls).
extern "C" void d3d9NoteGameThread() {
    if (g_gameTid.load()) return;
    g_gameTid = static_cast<long>(syscall(SYS_gettid));
    pthread_setname_np(pthread_self(), "game");
}

#if defined(NFS_R36S)
extern "C" uint32_t getD3D9PresentCount() {
    return g_d3d9PresentCount.load(std::memory_order_relaxed);
}
#else
extern "C" uint32_t glesGetD3D9PresentCount() {
    return g_d3d9PresentCount.load(std::memory_order_relaxed);
}
#endif

STDMETHODIMP IDirect3DDevice9GLES::Present(const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion) {
#ifdef __ANDROID__
    if (!g_hostWindow) return D3D_OK;
#else
    if (!m_sdlWindow) return D3D_OK;
#endif
    const uint32_t frame = ++g_d3d9PresentCount;
    if (frame == 1 && !g_gameTid.load()) {  // no render thread: the presenting thread is the game's
        g_gameTid = static_cast<long>(syscall(SYS_gettid));
        pthread_setname_np(pthread_self(), "game");
    }
    g_lastPresentMs = nowMs();
    const uint32_t frameDraws = g_frameDraws;
    g_frameDraws = 0;
    gpuLoadFrame(frame);
    profileFrame(frame, frameDraws);
    {
        // Hitches: a frame much slower than the recent average, with what it spent time on (render thread:
        // shader compiles, program links, texture uploads; game thread: the waits of the WAIT line).
        using clk = std::chrono::steady_clock;
        static clk::time_point last{};
        static double ema = 33.0;
        static int printed = 0;
        const clk::time_point now = clk::now();
        double ns[4]; uint32_t n[4]; double w[14];
        for (int i = 0; i < 4; ++i) { ns[i] = g_hitchNs[i].exchange(0) / 1e6; n[i] = g_hitchN[i].exchange(0); }
        for (int i = 0; i < 14; ++i) w[i] = g_waitFrameNs[i].exchange(0) / 1e6;
        if (last.time_since_epoch().count()) {
            const double ms = std::chrono::duration<double, std::milli>(now - last).count();
            if (frame > 120 && ms > 50.0 && ms > 1.6 * ema && printed < 600) {
                ++printed;
                std::printf("[NFSU2 HITCH] frame=%u %.0f ms (avg %.0f), %u draws; render: compile %u/%.1f ms, link %u/%.1f ms, tex upload %u/%.1f ms, tex create %u/%.1f ms, swap %.1f;"
                            " game: sync %.1f (%.0f calls), machine %.1f, cs %.1f, sleep %.1f, events %.1f, file %.1f (%.0f KB)\n",
                            frame, ms, ema, frameDraws, n[0], ns[0], n[1], ns[1], n[2], ns[2], n[3], ns[3], w[3], w[1], w[5] * 1e6, w[2], w[6], w[7], w[8], w[10], w[11] * 1e6 / 1024.0);
            }
            ema = ema * 0.9 + std::min(ms, 3 * ema) * 0.1;
        }
        last = now;
    }
    // Profile-guided builds: NFSU2_GCOV_DUMP_FRAME=n writes the instrumented guest code's counters at
    // frame n (the smoke test ends with SIGKILL, so the normal at-exit dump never runs).
    if (__gcov_dump) {
        static const int dumpFrame = [] { const char* e = std::getenv("NFSU2_GCOV_DUMP_FRAME"); return e ? std::atoi(e) : -1; }();
        if (static_cast<int>(frame) == dumpFrame) { __gcov_dump(); std::printf("[NFSU2] guest profile counters written\n"); }
    }
    // Diagnostics: NFSU2_SLOW_RACE_MS=n adds n ms to every frame while driving, to see how the game's
    // per-frame work changes when frames are slow (fixed-step simulation catching up?).
    static const int slowRaceMs = [] { const char* e = std::getenv("NFSU2_SLOW_RACE_MS"); return e ? std::atoi(e) : 0; }();
    if (slowRaceMs > 0 && guestDrivingControls()) std::this_thread::sleep_for(std::chrono::milliseconds(slowRaceMs));
    if (!g_paceOnGameThread) nfsEmulatePace();
    {
        // CPU time of the presenting (game) thread vs wall time, averaged over 120 frames. This is the
        // number that matters for the R36S: the VM's GPU is software (llvmpipe), the device's is a Mali.
        static double cpuAcc = 0, wallAcc = 0, glAcc = 0, drawAcc = 0;
        drawAcc += frameDraws;
        static timespec lastCpu{}, lastWall{};
        timespec cpu{}, wall{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
        clock_gettime(CLOCK_MONOTONIC, &wall);
        if (lastWall.tv_sec) {
            cpuAcc += (cpu.tv_sec - lastCpu.tv_sec) * 1e3 + (cpu.tv_nsec - lastCpu.tv_nsec) / 1e6;
            wallAcc += (wall.tv_sec - lastWall.tv_sec) * 1e3 + (wall.tv_nsec - lastWall.tv_nsec) / 1e6;
        }
        lastCpu = cpu;
        lastWall = wall;
        if (frame % 120 == 0 && wallAcc > 0) {
            std::printf("[NFSU2 PERF] frame=%u game-thread CPU %.2f ms/frame, wall %.2f ms/frame (%.1f fps), %.0f draws/frame,"
                        " %.2f ms/frame of CPU inside the GL driver (presenting thread)\n", frame,
                        cpuAcc / 120.0, wallAcc / 120.0, 120000.0 / wallAcc, drawAcc / 120.0, glsc::driverNs() / 1e6 / 120.0);
            { auto& pc = d3d9_gles::programBinaryCache();
              if (pc.ok) std::printf("[NFSU2 PERF] program binary cache: %u loaded, %u saved, %u rejected\n", pc.loaded, pc.saved, pc.rejected); }
            perfThreadsReport(120, wallAcc / 120.0, glsc::driverNs() / 1e6 / 120.0, drawAcc / 120.0);
            gpuLoadReport(120, wallAcc / 120.0);
            {
                double w[14];
                for (int i = 0; i < 14; ++i) w[i] = g_waitNs[i].exchange(0) / 1e6 / 120.0;
                // Run-queue delay of the game thread (ready to run but no core given): /proc/<tid>/schedstat.
                double runqMs = 0, schedCpuMs = 0;
                {
                    static double lastRunq = -1;
                    char path[64];
                    std::snprintf(path, sizeof(path), "/proc/self/task/%ld/schedstat", g_gameTid.load());
                    if (FILE* f = std::fopen(path, "r")) {
                        unsigned long long cpuNs = 0, waitNs = 0;
                        static double lastCpu = -1;
                        if (std::fscanf(f, "%llu %llu", &cpuNs, &waitNs) == 2) {
                            if (lastRunq >= 0) runqMs = (waitNs - lastRunq) / 1e6 / 120.0;
                            if (lastCpu >= 0) schedCpuMs = (cpuNs - lastCpu) / 1e6 / 120.0;
                            lastRunq = static_cast<double>(waitNs);
                            lastCpu = static_cast<double>(cpuNs);
                        }
                        std::fclose(f);
                    }
                }
                // Whole-process file I/O per frame (/proc/self/io: rchar = bytes read, read_bytes = from storage).
                double rcharKb = 0, diskKb = 0;
                {
                    static double lastRchar = -1, lastDisk = -1;
                    if (FILE* f = std::fopen("/proc/self/io", "r")) {
                        char key[64]; unsigned long long v; double rchar = -1, disk = -1;
                        while (std::fscanf(f, "%63s %llu", key, &v) == 2) {
                            if (!std::strcmp(key, "rchar:")) rchar = double(v);
                            if (!std::strcmp(key, "read_bytes:")) disk = double(v);
                        }
                        std::fclose(f);
                        if (lastRchar >= 0 && rchar >= 0) rcharKb = (rchar - lastRchar) / 1024.0 / 120.0;
                        if (lastDisk >= 0 && disk >= 0) diskKb = (disk - lastDisk) / 1024.0 / 120.0;
                        lastRchar = rchar; lastDisk = disk;
                    }
                }
                std::printf("[NFSU2 WAIT] game thread: CPU %.1f (schedstat), previous Present %.1f, sync calls %.1f (%.1f calls), guest machine %.1f,"
                            " critical sections %.1f, Sleep %.1f, events/threads %.1f, frame cap %.1f, file reads %.1f (%.0f KB), run queue %.1f ms/frame;"
                            " render thread: SwapWindow %.1f, idle %.1f ms/frame; process reads %.0f KB/frame (%.0f KB from storage);"
                            " sound thread waited for the guest machine %.1f ms/frame (%.0f times)\n",
                            schedCpuMs, w[0], w[1], w[5] * 1e6, w[2], w[6], w[7], w[8], w[9], w[10], w[11] * 1e6 / 1024.0, runqMs, w[3], w[4], rcharKb, diskKb, w[12], w[13] * 1e6);
                nfsReportFreeLockedCalls(120);
                std::vector<std::pair<uint64_t, uint32_t>> cs;
                { std::lock_guard<std::mutex> l(g_csWaitMutex); for (auto& e : g_csWait) cs.push_back({e.second, e.first}); g_csWait.clear(); }
                std::sort(cs.rbegin(), cs.rend());
                if (!cs.empty()) {
                    std::printf("[NFSU2 WAIT] critical sections (ms/frame):");
                    for (size_t i = 0; i < cs.size() && i < 4; ++i) std::printf(" %08x=%.1f", cs[i].second, cs[i].first / 1e6 / 120.0);
                    std::printf("\n");
                }
            }
            if (g_mem_base && std::getenv("NFSU2_SIM_TRACE")) {
                auto rf = [](uint32_t a) { float f; std::memcpy(&f, reinterpret_cast<const void*>(g_mem_base + a), 4); return f; };
                auto ri = [](uint32_t a) { uint32_t v; std::memcpy(&v, reinterpret_cast<const void*>(g_mem_base + a), 4); return v; };
                std::printf("[NFSU2 SIM] 7fb700=%u 7fb714=%g 865558=%g 865148=%g 865198=%g 865140=%u 86514c=%u 8650d0=%u 8650d4=%u\n",
                            ri(0x7fb700), rf(0x7fb714), rf(0x865558), rf(0x865148), rf(0x865198), ri(0x865140), ri(0x86514c), ri(0x8650d0), ri(0x8650d4));
            }
            cpuAcc = wallAcc = drawAcc = 0;
            glsc::driverNs() = 0;
        }
        (void)glAcc;
    }
    if (vkbd::state().visible.load()) {
        const UINT w = m_presentParams.BackBufferWidth ? m_presentParams.BackBufferWidth : 640;
        const UINT h = m_presentParams.BackBufferHeight ? m_presentParams.BackBufferHeight : 480;
        drawVirtualKeyboard(static_cast<int>(w), static_cast<int>(h), m_bbFbo);
        // The overlay used its own program, attributes and bindings: force the next draw to re-apply.
        m_currentProgram = nullptr;
        for (bool& e : m_attribEnabled) e = false;
        m_fboDirty = true;
    }
    if (g_fpsOverlay.load(std::memory_order_relaxed)) {
        const UINT w = m_presentParams.BackBufferWidth ? m_presentParams.BackBufferWidth : 640;
        const UINT h = m_presentParams.BackBufferHeight ? m_presentParams.BackBufferHeight : 480;
        drawFpsCounter(static_cast<int>(w), static_cast<int>(h), m_bbFbo);
        m_currentProgram = nullptr;
        for (bool& e : m_attribEnabled) e = false;
        m_fboDirty = true;
    }
    // Diagnostics only (never used for rendering): the game's detected performance level and the
    // graphics options it derived (see HANDOVER section 6.6).
    if (frame == 1500 && std::getenv("NFSU2_DIAG_FRAMES") && g_mem_base) {
        auto rd = [](uint32_t a) { return *reinterpret_cast<const uint32_t*>(g_mem_base + a); };
        std::printf("[NFSU2 OPTIONS] perfLevel=%u cpuMHz=%u vendor=0x%X device=0x%X texFilter=%u (max %u)\n",
                    rd(0x87099c), rd(0x86ff28), rd(0x86c548), rd(0x86c54c), rd(0x870d18), rd(0x876530));
        std::printf("[NFSU2 OPTIONS] cb8..d24:");
        for (uint32_t a = 0x870cb8; a <= 0x870d24; a += 4) std::printf(" %X=%u", a & 0xFFF, rd(a));
        std::printf("\n[NFSU2 OPTIONS] caps 4f0..530:");
        for (uint32_t a = 0x8764f0; a <= 0x876530; a += 4) std::printf(" %X=%u", a & 0xFFF, rd(a));
        std::printf("\n");
    }
    static const bool captureDiagnosticFrames = std::getenv("NFSU2_DIAG_FRAMES") != nullptr;
    // Frames 0..800 every 10: the boot/loading screen, the EA Games / THX / legal-screen intro quads and
    // the front-end menus all live there. The old sparse list started at frame 30 and jumped 400 -> 600, so
    // those screens (and the two quad siblings that only draw during the intro) were never captured.
    if (captureDiagnosticFrames &&
        ((frame <= 800 && frame % 10 == 0) ||
         (frame <= 1800 && (frame == 30 || frame == 60 || frame == 180 || frame == 400 || frame == 600 ||
                            frame == 800 || frame == 1000 || frame == 1150 || frame == 1250 || frame == 1400 ||
                            frame == 1600 || frame == 1800)) ||
         (frame > 1800 && frame <= 2450 && (frame % 50 == 0 || frame == 2401)))) {
        const UINT w = m_presentParams.BackBufferWidth ? m_presentParams.BackBufferWidth : 640;
        const UINT h = m_presentParams.BackBufferHeight ? m_presentParams.BackBufferHeight : 480;
        std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, m_bbFbo);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        m_fboDirty = true;
        char path[128];
        std::snprintf(path, sizeof(path), "diag_frame_%u.ppm", frame);
        if (FILE* fp = std::fopen(path, "wb")) {
            std::fprintf(fp, "P6\n%u %u\n255\n", w, h);
            for (int y = static_cast<int>(h) - 1; y >= 0; --y) {
                for (UINT x = 0; x < w; ++x) {
                    const size_t idx = (static_cast<size_t>(y) * w + x) * 4;
                    std::fputc(pixels[idx], fp);
                    std::fputc(pixels[idx + 1], fp);
                    std::fputc(pixels[idx + 2], fp);
                }
            }
            std::fclose(fp);
            std::printf("[NFSU2 DIAG] Saved diagnostic screenshot: %s\n", path);
        }
        bindRenderTarget();
    }
#ifndef __ANDROID__
    if (m_bbFbo && presenter::active()) {
        const auto s0 = std::chrono::steady_clock::now();
        presenter::submit(static_cast<float>(m_defaultBackBuffer->getWidth()), static_cast<float>(m_defaultBackBuffer->getHeight()), m_screenW, m_screenH, frame);
        m_bbFbo = presenter::currentFbo();
        m_bbTex = presenter::currentTex();
        m_fboDirty = true;
        nfsWaitAdd(3, std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - s0).count());
        return D3D_OK;
    }
#endif
    if (m_bbFbo) presentBackBuffer();
    {
        glsc::DriverCpuTimer timer;
        const bool probeGpu = gpuFinishProbeFrame(frame);
        auto t0 = std::chrono::steady_clock::now();
        if (probeGpu) {
            glFinish();
            const auto finished = std::chrono::steady_clock::now();
            gpuFinishProbe().finishMs += std::chrono::duration<double, std::milli>(finished - t0).count();
            t0 = finished;
        }
#ifdef __ANDROID__
        if (m_eglDisplay != EGL_NO_DISPLAY && m_eglSurface != EGL_NO_SURFACE) {
            eglSwapBuffers(m_eglDisplay, m_eglSurface);
        }
#else
        SDL_GL_SwapWindow(m_sdlWindow);
#endif
        const auto swapped = std::chrono::steady_clock::now();
        if (probeGpu) { gpuFinishProbe().swapMs += std::chrono::duration<double, std::milli>(swapped - t0).count(); ++gpuFinishProbe().frames; }
        nfsWaitAdd(3, std::chrono::duration_cast<std::chrono::nanoseconds>(swapped - t0).count());
    }
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer) {
    if (!ppBackBuffer) return D3DERR_INVALIDCALL;
    *ppBackBuffer = m_defaultBackBuffer;
    m_defaultBackBuffer->AddRef();
    return D3D_OK;
}

// =============================================================================
// Resources
// =============================================================================

STDMETHODIMP IDirect3DDevice9GLES::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle) {
    HitchTimer hitch(3);
    if (!ppTexture) return D3DERR_INVALIDCALL;
    static const bool s_log = std::getenv("NFSU2_DIAG_FRAMES") != nullptr;
    if (s_log) {
        static std::mutex s_seenMutex;   // textures are also created on the game thread (bridge: async CreateTexture)
        std::lock_guard<std::mutex> lock(s_seenMutex);
        static std::set<std::pair<DWORD, DWORD>> s_seen;
        if (s_seen.insert({static_cast<DWORD>(Format), Usage}).second) {
            std::printf("[D3D9 TEX] first CreateTexture fmt=0x%08X usage=0x%lX (%ux%u levels=%u pool=%u)\n",
                        static_cast<unsigned>(Format), static_cast<unsigned long>(Usage), Width, Height, Levels,
                        static_cast<unsigned>(Pool));
        }
    }
    *ppTexture = new IDirect3DTexture9GLES(Width, Height, Levels, Usage, Format);
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle) {
    if (!ppCubeTexture) return D3DERR_INVALIDCALL;
    std::printf("[D3D9 TEX] CreateCubeTexture fmt=0x%08X edge=%u levels=%u usage=0x%lX\n",
                static_cast<unsigned>(Format), EdgeLength, Levels, static_cast<unsigned long>(Usage));
    *ppCubeTexture = new IDirect3DCubeTexture9GLES(EdgeLength, Levels, Usage, Format);
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle) {
    if (!ppVertexBuffer) return D3DERR_INVALIDCALL;
    *ppVertexBuffer = new IDirect3DVertexBuffer9GLES(Length, Usage, FVF, Pool);
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle) {
    if (!ppIndexBuffer) return D3DERR_INVALIDCALL;
    *ppIndexBuffer = new IDirect3DIndexBuffer9GLES(Length, Usage, Format, Pool);
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) {
    if (!ppSurface) return D3DERR_INVALIDCALL;
    std::printf("[D3D9 RT] CreateRenderTarget %ux%u fmt=%u\n", Width, Height, static_cast<unsigned>(Format));
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, Width, Height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    *ppSurface = new IDirect3DSurface9GLES(Width, Height, Format, tex, 0, GL_TEXTURE_2D);
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle) {
    if (!ppSurface) return D3DERR_INVALIDCALL;
    *ppSurface = new IDirect3DSurface9GLES(Width, Height, Format);
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::StretchRect(IDirect3DSurface9* pSourceSurface, const RECT* pSourceRect, IDirect3DSurface9* pDestSurface, const RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter) {
    noteUnimplemented("StretchRect");
    return D3D_OK;
}

// =============================================================================
// Render targets
// =============================================================================

// -----------------------------------------------------------------------------
// Screen scaler: a back buffer that is not the window
// -----------------------------------------------------------------------------
void IDirect3DDevice9GLES::destroyBackBufferTarget() {
#ifndef __ANDROID__
    if (presenter::active()) {
        presenter::drain();
        presenter::destroySlots();
        m_bbFbo = m_bbTex = 0;
    }
#endif
    if (m_bbFbo) glDeleteFramebuffers(1, &m_bbFbo);
    if (m_bbTex) glDeleteTextures(1, &m_bbTex);
    if (m_bbDepth) glDeleteRenderbuffers(1, &m_bbDepth);
    m_bbFbo = m_bbTex = m_bbDepth = 0;
    m_fboDirty = true;
}

void IDirect3DDevice9GLES::createBackBufferTarget(UINT width, UINT height) {
    destroyBackBufferTarget();
    int sw = 0, sh = 0;
#ifdef __ANDROID__
    if (g_hostWindow) {
        sw = ANativeWindow_getWidth(reinterpret_cast<ANativeWindow*>(g_hostWindow));
        sh = ANativeWindow_getHeight(reinterpret_cast<ANativeWindow*>(g_hostWindow));
    }
#else
    if (m_sdlWindow) SDL_GL_GetDrawableSize(m_sdlWindow, &sw, &sh);
#endif
    m_screenW = sw > 0 ? sw : static_cast<int>(width);
    m_screenH = sh > 0 ? sh : static_cast<int>(height);
    const bool forced = std::getenv("NFSU2_FORCE_SCALER") != nullptr;
    // With the presenter thread the game always draws into off-screen buffers (the thread scales them onto the window).
#ifndef __ANDROID__
    if (m_sdlWindow && m_glContext) presenter::init(m_sdlWindow, m_glContext, static_cast<int>(m_presentParams.SwapEffect));
    const bool ring = presenter::active();
    const int count = ring ? presenter::slotCount() : 1;
#else
    const bool ring = false;
    const int count = 1;
#endif
    if (!forced && !ring && g_nfsScreenRotation == 0 && m_screenW == static_cast<int>(width) && m_screenH == static_cast<int>(height)) {
        std::printf("[D3D9 GLES] back buffer %ux%u = screen: drawing straight into the window\n", width, height);
        return;
    }
    // Depth/stencil like the window's: the default depth surface's renderbuffer, else one of our own.
    GLuint rb = m_defaultDepthBuffer ? m_defaultDepthBuffer->getGLRenderbuffer() : 0;
    bool stencil = rb && m_defaultDepthBuffer->hasStencil();
    if (!rb) {
        glGenRenderbuffers(1, &m_bbDepth);
        glBindRenderbuffer(GL_RENDERBUFFER, m_bbDepth);
        stencil = glesCaps().depth24Stencil8;
        glRenderbufferStorage(GL_RENDERBUFFER, stencil ? 0x88F0 /* GL_DEPTH24_STENCIL8_OES */ : GL_DEPTH_COMPONENT16, width, height);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        rb = m_bbDepth;
    }
    GLuint texs[3] = {0, 0, 0}, fbos[3] = {0, 0, 0};
    GLenum st = GL_FRAMEBUFFER_COMPLETE;
    for (int i = 0; i < count && st == GL_FRAMEBUFFER_COMPLETE; ++i) {
        glGenTextures(1, &texs[i]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texs[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &fbos[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texs[i], 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stencil ? rb : 0);
        st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st == GL_FRAMEBUFFER_COMPLETE) {
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        std::fprintf(stderr, "[D3D9 GLES] scaler framebuffer incomplete (0x%X): drawing straight into the %dx%d window\n", st, m_screenW, m_screenH);
        for (int i = 0; i < 3; ++i) {
            if (fbos[i]) glDeleteFramebuffers(1, &fbos[i]);
            if (texs[i]) glDeleteTextures(1, &texs[i]);
        }
        if (m_bbDepth) { glDeleteRenderbuffers(1, &m_bbDepth); m_bbDepth = 0; }
        m_fboDirty = true;
#ifndef __ANDROID__
        if (ring) presenter::shutdown();
#endif
        return;
    }
    m_bbTex = texs[0];
    m_bbFbo = fbos[0];
#ifndef __ANDROID__
    if (ring) presenter::setSlots(texs, fbos, count);
#endif
    m_fboDirty = true;
    std::printf("[D3D9 GLES] back buffer %ux%u scaled onto the %dx%d screen (rotation %u)%s\n", width, height, m_screenW, m_screenH, g_nfsScreenRotation,
                ring ? ", shown by the presenter thread" : "");
}

void IDirect3DDevice9GLES::presentBackBuffer() {
    static GLuint prog = 0;
    static GLint texLoc = -1;
    scaleBlit(m_bbTex, static_cast<float>(m_defaultBackBuffer->getWidth()), static_cast<float>(m_defaultBackBuffer->getHeight()),
              m_screenW, m_screenH, prog, texLoc, g_d3d9PresentCount);
    glsc::invalidate();   // the scaler talks to the driver directly
    // The scaler used its own program, attributes and bindings: force the next draw to re-apply them.
    m_currentProgram = nullptr;
    for (bool& e : m_attribEnabled) e = false;
    m_fboDirty = true;
}

void IDirect3DDevice9GLES::bindRenderTarget() {
    IDirect3DSurface9GLES* rt = m_currentRenderTarget ? m_currentRenderTarget : m_defaultBackBuffer;
    const bool offscreen = rt != m_defaultBackBuffer && rt->getGLTexture() != 0;
    m_rtFlip = offscreen;
    m_rtWidth = rt->getWidth();
    m_rtHeight = rt->getHeight();
    if (!offscreen) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_bbFbo);  // 0: the window itself
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, rt->getTarget(), rt->getGLTexture(), rt->getLevel());
        GLuint rb = (m_currentDepthBuffer && m_currentDepthBuffer != m_defaultDepthBuffer) ? m_currentDepthBuffer->getGLRenderbuffer() : 0;
        // The default depth buffer belongs to the window; an offscreen target paired with it gets the
        // default depth surface's own renderbuffer when it has one.
        if (!rb && m_currentDepthBuffer) rb = m_currentDepthBuffer->getGLRenderbuffer();
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
        const bool stencil = rb && m_currentDepthBuffer && m_currentDepthBuffer->hasStencil();
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stencil ? rb : 0);
        GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            static int s_n = 0;
            if (s_n++ < 10) std::fprintf(stderr, "[D3D9 GLES] Framebuffer incomplete: 0x%X (rt %ux%u depth=%p)\n", st, m_rtWidth, m_rtHeight, static_cast<void*>(m_currentDepthBuffer));
        }
    }
    m_fboDirty = false;
}

STDMETHODIMP IDirect3DDevice9GLES::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) {
    if (RenderTargetIndex != 0) return pRenderTarget ? D3DERR_INVALIDCALL : D3D_OK;
    if (!pRenderTarget) return D3DERR_INVALIDCALL;
    m_currentRenderTarget = static_cast<IDirect3DSurface9GLES*>(pRenderTarget);
    static int s_rtCount = 0;
    if (s_rtCount++ < 20 && m_currentRenderTarget != m_defaultBackBuffer) {
        std::printf("[D3D9 RT] SetRenderTarget offscreen tex=%u level=%u target=0x%X %ux%u (frame %u)\n",
                    m_currentRenderTarget->getGLTexture(), m_currentRenderTarget->getLevel(),
                    m_currentRenderTarget->getTarget(), m_currentRenderTarget->getWidth(),
                    m_currentRenderTarget->getHeight(), getD3D9PresentCount());
    }
    bindRenderTarget();
    // D3D9: setting a render target resets the viewport and scissor rect to the full target.
    m_viewport = {0, 0, m_rtWidth, m_rtHeight, 0.0f, 1.0f};
    m_scissorRect = {0, 0, static_cast<LONG>(m_rtWidth), static_cast<LONG>(m_rtHeight)};
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget) {
    if (!ppRenderTarget || RenderTargetIndex != 0) return D3DERR_INVALIDCALL;
    *ppRenderTarget = m_currentRenderTarget;
    if (m_currentRenderTarget) m_currentRenderTarget->AddRef();
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) {
    m_currentDepthBuffer = static_cast<IDirect3DSurface9GLES*>(pNewZStencil);
    if (m_rtFlip) bindRenderTarget();
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface) {
    if (!ppZStencilSurface) return D3DERR_INVALIDCALL;
    *ppZStencilSurface = m_currentDepthBuffer;
    if (!m_currentDepthBuffer) return D3DERR_NOTFOUND;
    m_currentDepthBuffer->AddRef();
    return D3D_OK;
}

void IDirect3DDevice9GLES::applyViewportAndScissor() {
    const D3DVIEWPORT9& vp = m_viewport;
    const GLint y = m_rtFlip ? static_cast<GLint>(vp.Y) : static_cast<GLint>(m_rtHeight) - static_cast<GLint>(vp.Y + vp.Height);
    glViewport(vp.X, y, vp.Width, vp.Height);
    glDepthRangef(vp.MinZ, vp.MaxZ);
    if (m_rs[D3DRS_SCISSORTESTENABLE]) {
        glEnable(GL_SCISSOR_TEST);
        const RECT& r = m_scissorRect;
        const GLint sy = m_rtFlip ? r.top : static_cast<GLint>(m_rtHeight) - r.bottom;
        glScissor(r.left, sy, std::max<LONG>(0, r.right - r.left), std::max<LONG>(0, r.bottom - r.top));
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
}

STDMETHODIMP IDirect3DDevice9GLES::Clear(DWORD Count, const D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) {
    if (m_fboDirty) bindRenderTarget();
    GLbitfield mask = 0;
    if (Flags & D3DCLEAR_TARGET) {
        glClearColor(((Color >> 16) & 0xFF) / 255.0f, ((Color >> 8) & 0xFF) / 255.0f, (Color & 0xFF) / 255.0f,
                     ((Color >> 24) & 0xFF) / 255.0f);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if ((Flags & D3DCLEAR_ZBUFFER) && m_currentDepthBuffer) {
        glClearDepthf(Z);
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    if ((Flags & D3DCLEAR_STENCIL) && m_currentDepthBuffer) {
        glClearStencil(static_cast<GLint>(Stencil));
        mask |= GL_STENCIL_BUFFER_BIT;
    }
    if (!mask) return D3D_OK;
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glEnable(GL_SCISSOR_TEST);
    // D3D9 clears the viewport rectangle (optionally intersected with rects, and with the scissor).
    auto clearRect = [&](LONG l, LONG t, LONG r, LONG b) {
        l = std::max<LONG>(l, m_viewport.X);
        t = std::max<LONG>(t, m_viewport.Y);
        r = std::min<LONG>(r, m_viewport.X + m_viewport.Width);
        b = std::min<LONG>(b, m_viewport.Y + m_viewport.Height);
        if (m_rs[D3DRS_SCISSORTESTENABLE]) {
            l = std::max(l, m_scissorRect.left); t = std::max(t, m_scissorRect.top);
            r = std::min(r, m_scissorRect.right); b = std::min(b, m_scissorRect.bottom);
        }
        if (r <= l || b <= t) return;
        const GLint y = m_rtFlip ? t : static_cast<GLint>(m_rtHeight) - b;
        glScissor(l, y, r - l, b - t);
        { glsc::DriverCpuTimer timer; glClear(mask); }
    };
    if (Count && pRects) {
        for (DWORD i = 0; i < Count; ++i) clearRect(pRects[i].x1, pRects[i].y1, pRects[i].x2, pRects[i].y2);
    } else {
        clearRect(0, 0, static_cast<LONG>(m_rtWidth), static_cast<LONG>(m_rtHeight));
    }
    return D3D_OK;
}

// =============================================================================
// Transforms, viewport
// =============================================================================

STDMETHODIMP IDirect3DDevice9GLES::SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) {
    if (!pMatrix) return D3DERR_INVALIDCALL;
    Matrix4 m;
    std::memcpy(m.m, pMatrix->m, sizeof(D3DMATRIX));
    if (m_recording) { m_recording->transforms[State] = m; return D3D_OK; }
    // Fixed-function transforms only. They never touch vertex shader constants (D3D9 semantics).
    if (State == D3DTS_WORLD) m_worldMatrix = m;
    else if (State == D3DTS_VIEW) m_viewMatrix = m;
    else if (State == D3DTS_PROJECTION) m_projMatrix = m;
    else m_otherTransforms[State] = m;
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) {
    if (!pMatrix) return D3DERR_INVALIDCALL;
    const Matrix4* m = nullptr;
    if (State == D3DTS_WORLD) m = &m_worldMatrix;
    else if (State == D3DTS_VIEW) m = &m_viewMatrix;
    else if (State == D3DTS_PROJECTION) m = &m_projMatrix;
    else {
        auto it = m_otherTransforms.find(State);
        static const Matrix4 id = Matrix4::identity();
        m = (it != m_otherTransforms.end()) ? &it->second : &id;
    }
    std::memcpy(pMatrix->m, m->m, sizeof(D3DMATRIX));
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) {
    if (!pMatrix) return D3DERR_INVALIDCALL;
    D3DMATRIX cur;
    GetTransform(State, &cur);
    Matrix4 a, b;
    std::memcpy(a.m, cur.m, sizeof(D3DMATRIX));
    std::memcpy(b.m, pMatrix->m, sizeof(D3DMATRIX));
    Matrix4 r = Matrix4::multiply(b, a);
    return SetTransform(State, reinterpret_cast<const D3DMATRIX*>(r.m));
}

STDMETHODIMP IDirect3DDevice9GLES::SetViewport(const D3DVIEWPORT9* pViewport) {
    if (!pViewport) return D3DERR_INVALIDCALL;
    if (m_recording) { m_recording->hasViewport = true; m_recording->viewport = *pViewport; return D3D_OK; }
    m_viewport = *pViewport;
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::GetViewport(D3DVIEWPORT9* pViewport) {
    if (!pViewport) return D3DERR_INVALIDCALL;
    *pViewport = m_viewport;
    return D3D_OK;
}

// =============================================================================
// States
// =============================================================================

STDMETHODIMP IDirect3DDevice9GLES::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
    if (State >= kMaxRS) return D3D_OK;
    if (m_recording) { m_recording->rs[State] = Value; return D3D_OK; }
    m_rs[State] = Value;
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) {
    if (!pValue) return D3DERR_INVALIDCALL;
    *pValue = (State < kMaxRS) ? m_rs[State] : 0;
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture) {
    if (Stage >= kMaxStages || !ppTexture) return D3DERR_INVALIDCALL;
    *ppTexture = m_boundTextures[Stage];
    if (m_boundTextures[Stage]) m_boundTextures[Stage]->AddRef();
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) {
    if (Stage >= D3DVERTEXTEXTURESAMPLER0) { noteUnimplemented("vertex texture fetch"); return D3D_OK; }
    if (Stage >= kMaxStages) return D3DERR_INVALIDCALL;
    if (m_recording) { m_recording->textures[Stage] = pTexture; return D3D_OK; }
    m_boundTextures[Stage] = pTexture;
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue) {
    if (Stage >= kMaxTSS || !pValue || Type >= kMaxTSSType) return D3DERR_INVALIDCALL;
    *pValue = m_tss[Stage][Type];
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
    if (Stage >= kMaxTSS || Type >= kMaxTSSType) return D3DERR_INVALIDCALL;
    if (m_recording) { m_recording->tss[Stage][Type] = Value; return D3D_OK; }
    m_tss[Stage][Type] = Value;
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue) {
    if (Sampler >= kMaxStages || !pValue || Type >= kMaxSS) return D3DERR_INVALIDCALL;
    *pValue = m_ss[Sampler][Type];
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
    if (Sampler >= D3DVERTEXTEXTURESAMPLER0) return D3D_OK;
    if (Sampler >= kMaxStages || Type >= kMaxSS) return D3DERR_INVALIDCALL;
    // Sampler state belongs to the stage; it is applied to whatever texture is bound at draw time.
    if (m_recording) { m_recording->ss[Sampler][Type] = Value; return D3D_OK; }
    m_ss[Sampler][Type] = Value;
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::SetScissorRect(const RECT* pRect) {
    if (!pRect) return D3DERR_INVALIDCALL;
    if (m_recording) { m_recording->hasScissor = true; m_recording->scissor = *pRect; return D3D_OK; }
    m_scissorRect = *pRect;
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::GetScissorRect(RECT* pRect) {
    if (!pRect) return D3DERR_INVALIDCALL;
    *pRect = m_scissorRect;
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::CreateVertexDeclaration(const D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl) {
    if (!ppDecl || !pVertexElements) return D3DERR_INVALIDCALL;
    *ppDecl = new IDirect3DVertexDeclaration9GLES(pVertexElements);
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) {
    auto* d = static_cast<IDirect3DVertexDeclaration9GLES*>(pDecl);
    if (m_recording) { m_recording->hasDecl = true; m_recording->decl = d; m_recording->hasFVF = false; return D3D_OK; }
    m_boundDecl = d;
    m_boundFVF = 0;
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) {
    if (!ppDecl) return D3DERR_INVALIDCALL;
    *ppDecl = m_boundDecl;
    if (m_boundDecl) m_boundDecl->AddRef();
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::SetFVF(DWORD FVF) {
    if (m_recording) { m_recording->hasFVF = true; m_recording->fvf = FVF; m_recording->hasDecl = false; return D3D_OK; }
    m_boundFVF = FVF;
    m_boundDecl = nullptr;
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::GetFVF(DWORD* pFVF) {
    if (!pFVF) return D3DERR_INVALIDCALL;
    *pFVF = m_boundFVF;
    return D3D_OK;
}

// =============================================================================
// Shaders and constants
// =============================================================================

static size_t shaderByteLength(const DWORD* fn) {
    const DWORD* cur = fn + 1;  // skip version token
    while (*cur != 0x0000FFFF) {
        if ((*cur & 0xFFFF) == 0xFFFE) {  // comment block
            cur += 1 + ((*cur >> 16) & 0x7FFF);
            continue;
        }
        ++cur;
    }
    return static_cast<size_t>(cur - fn + 1) * sizeof(DWORD);
}

STDMETHODIMP IDirect3DDevice9GLES::CreateVertexShader(const DWORD* pFunction, IDirect3DVertexShader9** ppShader) {
    if (!ppShader || !pFunction) return D3DERR_INVALIDCALL;
    auto* vs = new IDirect3DVertexShader9GLES();
    if (!vs->init(GL_VERTEX_SHADER, pFunction, shaderByteLength(pFunction))) {
        vs->Release();
        return E_FAIL;
    }
    *ppShader = vs;
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::SetVertexShader(IDirect3DVertexShader9* pShader) {
    auto* s = static_cast<IDirect3DVertexShader9GLES*>(pShader);
    if (m_recording) { m_recording->hasVS = true; m_recording->vs = s; return D3D_OK; }
    m_boundVS = s;
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::GetVertexShader(IDirect3DVertexShader9** ppShader) {
    if (!ppShader) return D3DERR_INVALIDCALL;
    *ppShader = m_boundVS;
    if (m_boundVS) m_boundVS->AddRef();
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::SetVertexShaderConstantF(UINT StartRegister, const float* pConstantData, UINT Vector4fCount) {
    if (!pConstantData) return D3DERR_INVALIDCALL;
    if (m_recording) {
        for (UINT i = 0; i < Vector4fCount; ++i) {
            std::array<float, 4> v;
            std::memcpy(v.data(), pConstantData + i * 4, 16);
            m_recording->vsF[StartRegister + i] = v;
        }
        return D3D_OK;
    }
    m_pipelineManager.setVSConstantF(StartRegister, pConstantData, Vector4fCount);
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) {
    m_pipelineManager.getVSConstantF(StartRegister, pConstantData, Vector4fCount);
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::SetVertexShaderConstantI(UINT StartRegister, const int* pConstantData, UINT Vector4iCount) {
    if (!pConstantData) return D3DERR_INVALIDCALL;
    if (m_recording) {
        for (UINT i = 0; i < Vector4iCount; ++i) {
            std::array<int, 4> v;
            std::memcpy(v.data(), pConstantData + i * 4, 16);
            m_recording->vsI[StartRegister + i] = v;
        }
        return D3D_OK;
    }
    m_pipelineManager.setVSConstantI(StartRegister, pConstantData, Vector4iCount);
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::SetVertexShaderConstantB(UINT StartRegister, const BOOL* pConstantData, UINT BoolCount) {
    if (!pConstantData) return D3DERR_INVALIDCALL;
    if (m_recording) {
        for (UINT i = 0; i < BoolCount; ++i) m_recording->vsB[StartRegister + i] = pConstantData[i];
        return D3D_OK;
    }
    m_pipelineManager.setVSConstantB(StartRegister, pConstantData, BoolCount);
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::CreatePixelShader(const DWORD* pFunction, IDirect3DPixelShader9** ppShader) {
    if (!ppShader || !pFunction) return D3DERR_INVALIDCALL;
    auto* ps = new IDirect3DPixelShader9GLES();
    if (!ps->init(GL_FRAGMENT_SHADER, pFunction, shaderByteLength(pFunction))) {
        ps->Release();
        return E_FAIL;
    }
    *ppShader = ps;
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::SetPixelShader(IDirect3DPixelShader9* pShader) {
    auto* s = static_cast<IDirect3DPixelShader9GLES*>(pShader);
    if (m_recording) { m_recording->hasPS = true; m_recording->ps = s; return D3D_OK; }
    m_boundPS = s;
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::GetPixelShader(IDirect3DPixelShader9** ppShader) {
    if (!ppShader) return D3DERR_INVALIDCALL;
    *ppShader = m_boundPS;
    if (m_boundPS) m_boundPS->AddRef();
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::SetPixelShaderConstantF(UINT StartRegister, const float* pConstantData, UINT Vector4fCount) {
    if (!pConstantData) return D3DERR_INVALIDCALL;
    if (m_recording) {
        for (UINT i = 0; i < Vector4fCount; ++i) {
            std::array<float, 4> v;
            std::memcpy(v.data(), pConstantData + i * 4, 16);
            m_recording->psF[StartRegister + i] = v;
        }
        return D3D_OK;
    }
    m_pipelineManager.setPSConstantF(StartRegister, pConstantData, Vector4fCount);
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount) {
    m_pipelineManager.getPSConstantF(StartRegister, pConstantData, Vector4fCount);
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::SetPixelShaderConstantI(UINT StartRegister, const int* pConstantData, UINT Vector4iCount) {
    if (!pConstantData) return D3DERR_INVALIDCALL;
    if (m_recording) {
        for (UINT i = 0; i < Vector4iCount; ++i) {
            std::array<int, 4> v;
            std::memcpy(v.data(), pConstantData + i * 4, 16);
            m_recording->psI[StartRegister + i] = v;
        }
        return D3D_OK;
    }
    m_pipelineManager.setPSConstantI(StartRegister, pConstantData, Vector4iCount);
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::SetPixelShaderConstantB(UINT StartRegister, const BOOL* pConstantData, UINT BoolCount) {
    if (!pConstantData) return D3DERR_INVALIDCALL;
    if (m_recording) {
        for (UINT i = 0; i < BoolCount; ++i) m_recording->psB[StartRegister + i] = pConstantData[i];
        return D3D_OK;
    }
    m_pipelineManager.setPSConstantB(StartRegister, pConstantData, BoolCount);
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride) {
    if (StreamNumber >= 16) return D3DERR_INVALIDCALL;
    StreamBinding b{static_cast<IDirect3DVertexBuffer9GLES*>(pStreamData), OffsetInBytes, Stride};
    if (m_recording) { m_recording->streams[StreamNumber] = b; return D3D_OK; }
    m_streams[StreamNumber] = b;
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* pOffsetInBytes, UINT* pStride) {
    if (StreamNumber >= 16 || !ppStreamData) return D3DERR_INVALIDCALL;
    *ppStreamData = m_streams[StreamNumber].buffer;
    if (m_streams[StreamNumber].buffer) m_streams[StreamNumber].buffer->AddRef();
    if (pOffsetInBytes) *pOffsetInBytes = m_streams[StreamNumber].offset;
    if (pStride) *pStride = m_streams[StreamNumber].stride;
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::SetIndices(IDirect3DIndexBuffer9* pIndexData) {
    auto* ib = static_cast<IDirect3DIndexBuffer9GLES*>(pIndexData);
    if (m_recording) { m_recording->hasIB = true; m_recording->ib = ib; return D3D_OK; }
    m_boundIB = ib;
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::GetIndices(IDirect3DIndexBuffer9** ppIndexData) {
    if (!ppIndexData) return D3DERR_INVALIDCALL;
    *ppIndexData = m_boundIB;
    if (m_boundIB) m_boundIB->AddRef();
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) {
    if (!ppQuery) return (Type == D3DQUERYTYPE_EVENT) ? D3D_OK : D3DERR_NOTAVAILABLE;
    *ppQuery = new IDirect3DQuery9GLES();
    return D3D_OK;
}

// =============================================================================
// State blocks
// =============================================================================

IDirect3DStateBlock9GLES::IDirect3DStateBlock9GLES(IDirect3DDevice9GLES* device, D3DSTATEBLOCKTYPE type, bool recorded)
    : m_device(device), m_type(type) {
    if (!recorded && m_device) m_device->captureAll(m_data, type);
}

STDMETHODIMP IDirect3DStateBlock9GLES::GetDevice(IDirect3DDevice9** ppDevice) {
    if (!ppDevice) return D3DERR_INVALIDCALL;
    *ppDevice = m_device;
    if (m_device) m_device->AddRef();
    return D3D_OK;
}

// Capture refreshes the values of exactly the states this block contains.
STDMETHODIMP IDirect3DStateBlock9GLES::Capture() {
    if (!m_device) return D3D_OK;
    IDirect3DDevice9GLES& d = *m_device;
    auto& b = m_data;
    for (auto& kv : b.rs) kv.second = d.m_rs[kv.first];
    for (int s = 0; s < IDirect3DDevice9GLES::kMaxStages; ++s) for (auto& kv : b.ss[s]) kv.second = d.m_ss[s][kv.first];
    for (int s = 0; s < IDirect3DDevice9GLES::kMaxTSS; ++s) for (auto& kv : b.tss[s]) kv.second = d.m_tss[s][kv.first];
    for (auto& kv : b.textures) kv.second = d.m_boundTextures[kv.first];
    if (b.hasVS) b.vs = d.m_boundVS;
    if (b.hasPS) b.ps = d.m_boundPS;
    for (auto& kv : b.vsF) d.m_pipelineManager.getVSConstantF(kv.first, kv.second.data(), 1);
    for (auto& kv : b.psF) d.m_pipelineManager.getPSConstantF(kv.first, kv.second.data(), 1);
    for (auto& kv : b.vsI) d.m_pipelineManager.getVSConstantI(kv.first, kv.second.data(), 1);
    for (auto& kv : b.psI) d.m_pipelineManager.getPSConstantI(kv.first, kv.second.data(), 1);
    for (auto& kv : b.vsB) d.m_pipelineManager.getVSConstantB(kv.first, &kv.second, 1);
    for (auto& kv : b.psB) d.m_pipelineManager.getPSConstantB(kv.first, &kv.second, 1);
    if (b.hasDecl) b.decl = d.m_boundDecl;
    if (b.hasFVF) b.fvf = d.m_boundFVF;
    for (auto& kv : b.streams) kv.second = d.m_streams[kv.first];
    if (b.hasIB) b.ib = d.m_boundIB;
    if (b.hasViewport) b.viewport = d.m_viewport;
    if (b.hasScissor) b.scissor = d.m_scissorRect;
    for (auto& kv : b.transforms) d.GetTransform(static_cast<D3DTRANSFORMSTATETYPE>(kv.first), reinterpret_cast<D3DMATRIX*>(kv.second.m));
    return D3D_OK;
}

STDMETHODIMP IDirect3DStateBlock9GLES::Apply() {
    if (m_device) m_device->applyBlock(m_data);
    return D3D_OK;
}

void IDirect3DDevice9GLES::captureAll(StateBlockData& b, D3DSTATEBLOCKTYPE type) {
    const bool all = type == D3DSBT_ALL;
    const bool pixel = all || type == D3DSBT_PIXELSTATE;
    const bool vertex = all || type == D3DSBT_VERTEXSTATE;
    for (DWORD i = 0; i < kMaxRS; ++i) b.rs[i] = m_rs[i];
    if (pixel) {
        for (int s = 0; s < kMaxStages; ++s) for (DWORD t = 1; t < kMaxSS; ++t) b.ss[s][t] = m_ss[s][t];
        for (int s = 0; s < kMaxTSS; ++s) for (DWORD t = 1; t < kMaxTSSType; ++t) b.tss[s][t] = m_tss[s][t];
        b.hasPS = true; b.ps = m_boundPS;
        for (UINT r = 0; r < kMaxFloatRegs; ++r) { std::array<float, 4> v; m_pipelineManager.getPSConstantF(r, v.data(), 1); b.psF[r] = v; }
        for (UINT r = 0; r < kMaxIntRegs; ++r) { std::array<int, 4> v; m_pipelineManager.getPSConstantI(r, v.data(), 1); b.psI[r] = v; }
        for (UINT r = 0; r < kMaxBoolRegs; ++r) { BOOL v; m_pipelineManager.getPSConstantB(r, &v, 1); b.psB[r] = v; }
    }
    if (vertex) {
        b.hasVS = true; b.vs = m_boundVS;
        for (UINT r = 0; r < kMaxFloatRegs; ++r) { std::array<float, 4> v; m_pipelineManager.getVSConstantF(r, v.data(), 1); b.vsF[r] = v; }
        for (UINT r = 0; r < kMaxIntRegs; ++r) { std::array<int, 4> v; m_pipelineManager.getVSConstantI(r, v.data(), 1); b.vsI[r] = v; }
        for (UINT r = 0; r < kMaxBoolRegs; ++r) { BOOL v; m_pipelineManager.getVSConstantB(r, &v, 1); b.vsB[r] = v; }
        if (m_boundDecl) { b.hasDecl = true; b.decl = m_boundDecl; } else { b.hasFVF = true; b.fvf = m_boundFVF; }
    }
    if (all) {
        for (DWORD s = 0; s < kMaxStages; ++s) b.textures[s] = m_boundTextures[s];
        for (UINT s = 0; s < 16; ++s) b.streams[s] = m_streams[s];
        b.hasIB = true; b.ib = m_boundIB;
        b.hasViewport = true; b.viewport = m_viewport;
        b.hasScissor = true; b.scissor = m_scissorRect;
        b.transforms[D3DTS_WORLD] = m_worldMatrix;
        b.transforms[D3DTS_VIEW] = m_viewMatrix;
        b.transforms[D3DTS_PROJECTION] = m_projMatrix;
    }
}

void IDirect3DDevice9GLES::applyBlock(const StateBlockData& b) {
    for (const auto& kv : b.rs) SetRenderState(static_cast<D3DRENDERSTATETYPE>(kv.first), kv.second);
    for (int s = 0; s < kMaxStages; ++s) for (const auto& kv : b.ss[s]) SetSamplerState(s, static_cast<D3DSAMPLERSTATETYPE>(kv.first), kv.second);
    for (int s = 0; s < kMaxTSS; ++s) for (const auto& kv : b.tss[s]) SetTextureStageState(s, static_cast<D3DTEXTURESTAGESTATETYPE>(kv.first), kv.second);
    for (const auto& kv : b.textures) SetTexture(kv.first, kv.second);
    if (b.hasVS) SetVertexShader(b.vs);
    if (b.hasPS) SetPixelShader(b.ps);
    for (const auto& kv : b.vsF) SetVertexShaderConstantF(kv.first, kv.second.data(), 1);
    for (const auto& kv : b.psF) SetPixelShaderConstantF(kv.first, kv.second.data(), 1);
    for (const auto& kv : b.vsI) SetVertexShaderConstantI(kv.first, kv.second.data(), 1);
    for (const auto& kv : b.psI) SetPixelShaderConstantI(kv.first, kv.second.data(), 1);
    for (const auto& kv : b.vsB) SetVertexShaderConstantB(kv.first, &kv.second, 1);
    for (const auto& kv : b.psB) SetPixelShaderConstantB(kv.first, &kv.second, 1);
    if (b.hasDecl) SetVertexDeclaration(b.decl);
    if (b.hasFVF) SetFVF(b.fvf);
    for (const auto& kv : b.streams) SetStreamSource(kv.first, kv.second.buffer, kv.second.offset, kv.second.stride);
    if (b.hasIB) SetIndices(b.ib);
    if (b.hasViewport) SetViewport(&b.viewport);
    if (b.hasScissor) SetScissorRect(&b.scissor);
    for (const auto& kv : b.transforms) SetTransform(static_cast<D3DTRANSFORMSTATETYPE>(kv.first), reinterpret_cast<const D3DMATRIX*>(kv.second.m));
}

STDMETHODIMP IDirect3DDevice9GLES::BeginStateBlock() {
    if (m_recording) return D3DERR_INVALIDCALL;
    m_recording = new StateBlockData();
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::EndStateBlock(IDirect3DStateBlock9** ppSB) {
    if (!ppSB || !m_recording) return D3DERR_INVALIDCALL;
    auto* sb = new IDirect3DStateBlock9GLES(this, D3DSBT_ALL, true);
    {
        static int s_n = 0;
        const StateBlockData& r = *m_recording;
        size_t ssn = 0, tssn = 0;
        for (int i = 0; i < kMaxStages; ++i) ssn += r.ss[i].size();
        for (int i = 0; i < kMaxTSS; ++i) tssn += r.tss[i].size();
        if (s_n++ < 40) {
            std::printf("[D3D9 SB] recorded #%d: rs=%zu ss=%zu tss=%zu tex=%zu vs=%d ps=%d vsF=%zu psF=%zu decl=%d fvf=%d streams=%zu vp=%d xf=%zu (frame %u)\n",
                        s_n, r.rs.size(), ssn, tssn, r.textures.size(), r.hasVS, r.hasPS, r.vsF.size(), r.psF.size(),
                        r.hasDecl, r.hasFVF, r.streams.size(), r.hasViewport, r.transforms.size(), getD3D9PresentCount());
        }
    }
    sb->data() = std::move(*m_recording);
    delete m_recording;
    m_recording = nullptr;
    *ppSB = sb;
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) {
    if (!ppSB) return D3DERR_INVALIDCALL;
    std::printf("[D3D9 SB] CreateStateBlock type=%d (frame %u)\n", static_cast<int>(Type), getD3D9PresentCount());
    *ppSB = new IDirect3DStateBlock9GLES(this, Type, false);
    return D3D_OK;
}

// =============================================================================
// Draw path
// =============================================================================

static void fvfToElements(DWORD fvf, std::vector<D3DVERTEXELEMENT9>& out) {
    out.clear();
    WORD off = 0;
    auto add = [&](BYTE type, BYTE usage, BYTE index, WORD size) {
        out.push_back(D3DVERTEXELEMENT9{0, off, type, D3DDECLMETHOD_DEFAULT, usage, index});
        off = static_cast<WORD>(off + size);
    };
    const DWORD pos = fvf & D3DFVF_POSITION_MASK;
    if (pos == D3DFVF_XYZRHW) {
        add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITIONT, 0, 16);
    } else if (pos == D3DFVF_XYZW) {
        add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_POSITION, 0, 16);
    } else if (pos != 0) {
        add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 12);
        if (pos >= D3DFVF_XYZB1 && pos <= D3DFVF_XYZB5) {
            const int betas = static_cast<int>((pos - D3DFVF_XYZB1) / 2 + 1);
            off = static_cast<WORD>(off + betas * 4);
        }
    }
    if (fvf & D3DFVF_NORMAL) add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0, 12);
    if (fvf & D3DFVF_PSIZE) add(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_PSIZE, 0, 4);
    if (fvf & D3DFVF_DIFFUSE) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0, 4);
    if (fvf & D3DFVF_SPECULAR) add(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1, 4);
    const DWORD texCount = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (DWORD i = 0; i < texCount && i < 8; ++i) {
        const DWORD fmt = (fvf >> (16 + i * 2)) & 3;
        if (fmt == D3DFVF_TEXTUREFORMAT2) add(D3DDECLTYPE_FLOAT2, D3DDECLUSAGE_TEXCOORD, static_cast<BYTE>(i), 8);
        else if (fmt == D3DFVF_TEXTUREFORMAT3) add(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_TEXCOORD, static_cast<BYTE>(i), 12);
        else if (fmt == D3DFVF_TEXTUREFORMAT4) add(D3DDECLTYPE_FLOAT4, D3DDECLUSAGE_TEXCOORD, static_cast<BYTE>(i), 16);
        else add(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_TEXCOORD, static_cast<BYTE>(i), 4);
    }
}

const std::vector<D3DVERTEXELEMENT9>& IDirect3DDevice9GLES::currentElements() {
    if (m_boundDecl) return m_boundDecl->getElements();
    auto it = m_fvfElements.find(m_boundFVF);
    if (it == m_fvfElements.end()) {
        std::vector<D3DVERTEXELEMENT9> v;
        fvfToElements(m_boundFVF, v);
        v.push_back(D3DVERTEXELEMENT9{0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0});
        it = m_fvfElements.emplace(m_boundFVF, std::move(v)).first;
    }
    return it->second;
}

static bool usageMatches(MOJOSHADER_usage su, int sidx, const D3DVERTEXELEMENT9& e) {
    const bool shaderPos = su == MOJOSHADER_USAGE_POSITION || su == MOJOSHADER_USAGE_POSITIONT;
    const bool elemPos = e.Usage == D3DDECLUSAGE_POSITION || e.Usage == D3DDECLUSAGE_POSITIONT;
    if (shaderPos && elemPos) return sidx == e.UsageIndex;
    return static_cast<int>(su) == static_cast<int>(e.Usage) && sidx == e.UsageIndex;
}

static GLenum glBlend(DWORD b, bool& usesConstant) {
    switch (b) {
        case D3DBLEND_ZERO: return GL_ZERO;
        case D3DBLEND_ONE: return GL_ONE;
        case D3DBLEND_SRCCOLOR: return GL_SRC_COLOR;
        case D3DBLEND_INVSRCCOLOR: return GL_ONE_MINUS_SRC_COLOR;
        case D3DBLEND_SRCALPHA: return GL_SRC_ALPHA;
        case D3DBLEND_INVSRCALPHA: return GL_ONE_MINUS_SRC_ALPHA;
        case D3DBLEND_DESTALPHA: return GL_DST_ALPHA;
        case D3DBLEND_INVDESTALPHA: return GL_ONE_MINUS_DST_ALPHA;
        case D3DBLEND_DESTCOLOR: return GL_DST_COLOR;
        case D3DBLEND_INVDESTCOLOR: return GL_ONE_MINUS_DST_COLOR;
        case D3DBLEND_SRCALPHASAT: return GL_SRC_ALPHA_SATURATE;
        case D3DBLEND_BLENDFACTOR: usesConstant = true; return GL_CONSTANT_COLOR;
        case D3DBLEND_INVBLENDFACTOR: usesConstant = true; return GL_ONE_MINUS_CONSTANT_COLOR;
        default: return GL_ONE;
    }
}

static GLenum glBlendEq(DWORD op) {
    switch (op) {
        case D3DBLENDOP_SUBTRACT: return GL_FUNC_SUBTRACT;
        case D3DBLENDOP_REVSUBTRACT: return GL_FUNC_REVERSE_SUBTRACT;
        case D3DBLENDOP_MIN: return glesCaps().blendMinMax ? 0x8007 /* GL_MIN_EXT */ : GL_FUNC_ADD;
        case D3DBLENDOP_MAX: return glesCaps().blendMinMax ? 0x8008 /* GL_MAX_EXT */ : GL_FUNC_ADD;
        default: return GL_FUNC_ADD;
    }
}

static GLenum glStencilOpFor(DWORD op) {
    switch (op) {
        case D3DSTENCILOP_ZERO: return GL_ZERO;
        case D3DSTENCILOP_REPLACE: return GL_REPLACE;
        case D3DSTENCILOP_INCRSAT: return GL_INCR;
        case D3DSTENCILOP_DECRSAT: return GL_DECR;
        case D3DSTENCILOP_INVERT: return GL_INVERT;
        case D3DSTENCILOP_INCR: return GL_INCR_WRAP;
        case D3DSTENCILOP_DECR: return GL_DECR_WRAP;
        default: return GL_KEEP;
    }
}

void IDirect3DDevice9GLES::applyRenderStates() {
    // Culling: D3D9 and GL both define winding as seen on screen. Rendering into a texture is
    // Y-flipped (vpFlip), which mirrors the winding.
    const DWORD cull = m_rs[D3DRS_CULLMODE];
    if (cull == D3DCULL_NONE) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glFrontFace(m_rtFlip ? GL_CCW : GL_CW);
        glCullFace(cull == D3DCULL_CW ? GL_FRONT : GL_BACK);
    }

    const bool depth = m_rs[D3DRS_ZENABLE] != D3DZB_FALSE && m_currentDepthBuffer != nullptr;
    if (depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthFunc(convertCompareFunc(static_cast<D3DCMPFUNC>(m_rs[D3DRS_ZFUNC])));
    glDepthMask(m_rs[D3DRS_ZWRITEENABLE] ? GL_TRUE : GL_FALSE);

    float bias, slope;
    std::memcpy(&bias, &m_rs[D3DRS_DEPTHBIAS], 4);
    std::memcpy(&slope, &m_rs[D3DRS_SLOPESCALEDEPTHBIAS], 4);
    if (bias != 0.0f || slope != 0.0f) {
        const bool d16 = m_currentDepthBuffer && m_currentDepthBuffer->getFormat() == D3DFMT_D16;
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(slope, bias * (d16 ? 65535.0f : 16777215.0f));
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }

    if (m_rs[D3DRS_ALPHABLENDENABLE]) {
        glEnable(GL_BLEND);
        bool usesConstant = false;
        DWORD src = m_rs[D3DRS_SRCBLEND], dst = m_rs[D3DRS_DESTBLEND];
        if (src == D3DBLEND_BOTHSRCALPHA) { src = D3DBLEND_SRCALPHA; dst = D3DBLEND_INVSRCALPHA; }
        else if (src == D3DBLEND_BOTHINVSRCALPHA) { src = D3DBLEND_INVSRCALPHA; dst = D3DBLEND_SRCALPHA; }
        if (m_rs[D3DRS_SEPARATEALPHABLENDENABLE]) {
            glBlendFuncSeparate(glBlend(src, usesConstant), glBlend(dst, usesConstant),
                                glBlend(m_rs[D3DRS_SRCBLENDALPHA], usesConstant), glBlend(m_rs[D3DRS_DESTBLENDALPHA], usesConstant));
            glBlendEquationSeparate(glBlendEq(m_rs[D3DRS_BLENDOP]), glBlendEq(m_rs[D3DRS_BLENDOPALPHA]));
        } else {
            glBlendFunc(glBlend(src, usesConstant), glBlend(dst, usesConstant));
            glBlendEquation(glBlendEq(m_rs[D3DRS_BLENDOP]));
        }
        if (usesConstant) {
            const DWORD c = m_rs[D3DRS_BLENDFACTOR];
            glBlendColor(((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, ((c >> 24) & 0xFF) / 255.0f);
        }
    } else {
        glDisable(GL_BLEND);
    }

    const DWORD cw = m_rs[D3DRS_COLORWRITEENABLE];
    glColorMask((cw & 1) ? GL_TRUE : GL_FALSE, (cw & 2) ? GL_TRUE : GL_FALSE, (cw & 4) ? GL_TRUE : GL_FALSE, (cw & 8) ? GL_TRUE : GL_FALSE);

    if (m_rs[D3DRS_STENCILENABLE] && m_currentDepthBuffer) {
        glEnable(GL_STENCIL_TEST);
        const GLint ref = static_cast<GLint>(m_rs[D3DRS_STENCILREF] & 0xFF);
        const GLuint mask = m_rs[D3DRS_STENCILMASK] & 0xFF;
        glStencilMask(m_rs[D3DRS_STENCILWRITEMASK] & 0xFF);
        glStencilFuncSeparate(GL_FRONT, convertCompareFunc(static_cast<D3DCMPFUNC>(m_rs[D3DRS_STENCILFUNC])), ref, mask);
        glStencilOpSeparate(GL_FRONT, glStencilOpFor(m_rs[D3DRS_STENCILFAIL]), glStencilOpFor(m_rs[D3DRS_STENCILZFAIL]), glStencilOpFor(m_rs[D3DRS_STENCILPASS]));
        if (m_rs[D3DRS_TWOSIDEDSTENCILMODE]) {
            glStencilFuncSeparate(GL_BACK, convertCompareFunc(static_cast<D3DCMPFUNC>(m_rs[D3DRS_CCW_STENCILFUNC])), ref, mask);
            glStencilOpSeparate(GL_BACK, glStencilOpFor(m_rs[D3DRS_CCW_STENCILFAIL]), glStencilOpFor(m_rs[D3DRS_CCW_STENCILZFAIL]), glStencilOpFor(m_rs[D3DRS_CCW_STENCILPASS]));
        } else {
            glStencilFuncSeparate(GL_BACK, convertCompareFunc(static_cast<D3DCMPFUNC>(m_rs[D3DRS_STENCILFUNC])), ref, mask);
            glStencilOpSeparate(GL_BACK, glStencilOpFor(m_rs[D3DRS_STENCILFAIL]), glStencilOpFor(m_rs[D3DRS_STENCILZFAIL]), glStencilOpFor(m_rs[D3DRS_STENCILPASS]));
        }
    } else {
        glDisable(GL_STENCIL_TEST);
    }
}

static GLint glWrap(DWORD a) {
    switch (a) {
        case D3DTADDRESS_MIRROR: return GL_MIRRORED_REPEAT;
        case D3DTADDRESS_CLAMP: case D3DTADDRESS_BORDER: case D3DTADDRESS_MIRRORONCE: return GL_CLAMP_TO_EDGE;
        default: return GL_REPEAT;
    }
}

void IDirect3DDevice9GLES::applySamplerParams(int stage, IDirect3DBaseTexture9* tex) {
    GLTextureObject* obj = nullptr;
    if (tex->GetType() == D3DRTYPE_TEXTURE) obj = static_cast<IDirect3DTexture9GLES*>(tex)->glObject();
    else if (tex->GetType() == D3DRTYPE_CUBETEXTURE) obj = static_cast<IDirect3DCubeTexture9GLES*>(tex)->glObject();
    if (!obj) return;
    const bool npotLimited = !obj->pot && !glesCaps().npot;
    GLint wrapS = npotLimited ? GL_CLAMP_TO_EDGE : glWrap(m_ss[stage][D3DSAMP_ADDRESSU]);
    GLint wrapT = npotLimited ? GL_CLAMP_TO_EDGE : glWrap(m_ss[stage][D3DSAMP_ADDRESSV]);
    if (obj->target == GL_TEXTURE_CUBE_MAP) wrapS = wrapT = GL_CLAMP_TO_EDGE;
    const DWORD minF = m_ss[stage][D3DSAMP_MINFILTER];
    const DWORD magF = m_ss[stage][D3DSAMP_MAGFILTER];
    const DWORD mipF = obj->mipmapped() ? m_ss[stage][D3DSAMP_MIPFILTER] : D3DTEXF_NONE;
    const bool minPoint = (minF == D3DTEXF_POINT || minF == D3DTEXF_NONE);
    GLint glMin;
    if (mipF == D3DTEXF_NONE) glMin = minPoint ? GL_NEAREST : GL_LINEAR;
    else if (mipF == D3DTEXF_POINT) glMin = minPoint ? GL_NEAREST_MIPMAP_NEAREST : GL_LINEAR_MIPMAP_NEAREST;
    else glMin = minPoint ? GL_NEAREST_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_LINEAR;
    const GLint glMag = (magF == D3DTEXF_POINT || magF == D3DTEXF_NONE) ? GL_NEAREST : GL_LINEAR;
    SamplerParamCache& c = obj->cache;
    const GLenum t = obj->target;
    if (c.wrapS != wrapS) { glTexParameteri(t, GL_TEXTURE_WRAP_S, wrapS); c.wrapS = wrapS; }
    if (c.wrapT != wrapT) { glTexParameteri(t, GL_TEXTURE_WRAP_T, wrapT); c.wrapT = wrapT; }
    if (c.minF != glMin) { glTexParameteri(t, GL_TEXTURE_MIN_FILTER, glMin); c.minF = glMin; }
    if (c.magF != glMag) { glTexParameteri(t, GL_TEXTURE_MAG_FILTER, glMag); c.magF = glMag; }
}

void IDirect3DDevice9GLES::applyTextures(const ShaderVariant* psVariant, bool ffpPS, int ffpStages) {
    auto bindStage = [&](int stage, bool wantCube) {
        glActiveTexture(GL_TEXTURE0 + stage);
        IDirect3DBaseTexture9* tex = m_boundTextures[stage];
        const bool isCube = tex && tex->GetType() == D3DRTYPE_CUBETEXTURE;
        const bool is2D = tex && tex->GetType() == D3DRTYPE_TEXTURE;
        if (wantCube) {
            glBindTexture(GL_TEXTURE_CUBE_MAP, isCube ? static_cast<IDirect3DCubeTexture9GLES*>(tex)->getGLTexture() : m_dummyCube);
        } else {
            glBindTexture(GL_TEXTURE_2D, is2D ? static_cast<IDirect3DTexture9GLES*>(tex)->getGLTexture() : m_dummy2D);
        }
        if ((wantCube && isCube) || (!wantCube && is2D)) applySamplerParams(stage, tex);
    };
    if (psVariant) {
        for (const auto& s : psVariant->samplers) {
            if (s.stage < kMaxStages) bindStage(s.stage, s.type == MOJOSHADER_SAMPLER_CUBE);
        }
    } else if (ffpPS) {
        for (int i = 0; i < ffpStages; ++i) {
            IDirect3DBaseTexture9* tex = m_boundTextures[i];
            if (tex) bindStage(i, tex->GetType() == D3DRTYPE_CUBETEXTURE);
        }
    }
    glActiveTexture(GL_TEXTURE0);
}

bool IDirect3DDevice9GLES::prepareDraw(const void* upData, UINT upStride, INT baseVertex, uint32_t instanceLayout, bool* instancingUnavailable) {
    if (m_fboDirty) bindRenderTarget();
    const std::vector<D3DVERTEXELEMENT9>& elems = currentElements();

    bool hasPositionT = false;
    for (const auto& e : elems) {
        if (e.Stream == 0xFF) break;
        if (e.Usage == D3DDECLUSAGE_POSITIONT) hasPositionT = true;
    }

    // ---- vertex stage ----
    // D3D9: pre-transformed vertices (XYZRHW / POSITIONT) bypass the vertex shader.
    const bool programmableVS = m_boundVS && !hasPositionT;
    const ShaderVariant* vsVar = nullptr;
    GLuint vsHandle = 0;
    // attribute location for each element (-1 = unused)
    int elemLoc[32];
    uint32_t bgraMask = 0;
    int elemCount = 0;
    for (const auto& e : elems) {
        if (e.Stream == 0xFF || elemCount >= 32) break;
        elemLoc[elemCount++] = -1;
    }
    if (programmableVS) {
        const ShaderVariant* base = m_boundVS->base();
        if (!base) return false;
        for (int i = 0; i < elemCount; ++i) {
            const auto& e = elems[i];
            for (size_t a = 0; a < base->attributes.size(); ++a) {
                if (usageMatches(base->attributes[a].usage, base->attributes[a].index, e)) {
                    elemLoc[i] = static_cast<int>(a);
                    if (e.Type == D3DDECLTYPE_D3DCOLOR) bgraMask |= 1u << a;
                    break;
                }
            }
        }
        if (instanceLayout) {
            vsVar = m_boundVS->getVariant(bgraMask | (instanceLayout << 16) | kInstancedVariantBit);
            if (!vsVar || vsVar->attributes.size() >= static_cast<size_t>(m_maxAttribs)) { if (instancingUnavailable) *instancingUnavailable = true; return false; }
        } else {
            vsVar = m_boundVS->getVariant(bgraMask);
        }
        if (!vsVar) return false;
        vsHandle = vsVar->glShader;
    } else {
        if (instanceLayout) { if (instancingUnavailable) *instancingUnavailable = true; return false; }
        uint32_t key = hasPositionT ? kFFPVS_RHW : 0;
        for (int i = 0; i < elemCount; ++i) {
            const auto& e = elems[i];
            if (e.Usage == D3DDECLUSAGE_POSITION || e.Usage == D3DDECLUSAGE_POSITIONT) {
                if (e.UsageIndex == 0) elemLoc[i] = kFFPPos;
            } else if (e.Usage == D3DDECLUSAGE_COLOR && e.UsageIndex < 2) {
                elemLoc[i] = e.UsageIndex == 0 ? kFFPDiffuse : kFFPSpecular;
                if (e.Type == D3DDECLTYPE_D3DCOLOR) key |= e.UsageIndex == 0 ? kFFPVS_DiffuseBGRA : kFFPVS_SpecularBGRA;
            } else if (e.Usage == D3DDECLUSAGE_TEXCOORD && e.UsageIndex < 4) {
                elemLoc[i] = kFFPTex0 + e.UsageIndex;
            }
        }
        if (!hasPositionT && m_rs[D3DRS_LIGHTING]) noteUnimplemented("fixed-function lighting (D3DRS_LIGHTING)");
        vsHandle = m_pipelineManager.getFFPVertexShader(key);
    }

    // ---- fog (D3D9 fixed-function fog after the pixel shader) ----
    uint32_t fogMode = kFogNone;
    if (m_rs[D3DRS_FOGENABLE] && !(m_boundPS && m_boundPS->majorVersion() >= 3)) {
        switch (m_rs[D3DRS_FOGTABLEMODE]) {
            case D3DFOG_LINEAR: fogMode = kFogLinear; break;
            case D3DFOG_EXP: fogMode = kFogExp; break;
            case D3DFOG_EXP2: fogMode = kFogExp2; break;
            default: fogMode = kFogVertex; break;
        }
        if (fogMode == kFogVertex && !programmableVS && !hasPositionT && m_rs[D3DRS_FOGVERTEXMODE] != D3DFOG_NONE)
            noteUnimplemented("fixed-function vertex fog (FOGVERTEXMODE)");
    }

    // ---- pixel stage ----
    const ShaderVariant* psVar = nullptr;
    GLuint psHandle = 0;
    int ffpStages = 0;
    if (m_boundPS) {
        uint32_t cubeMask = 0;
        for (int s = 0; s < kMaxStages; ++s) {
            if (m_boundTextures[s] && m_boundTextures[s]->GetType() == D3DRTYPE_CUBETEXTURE) cubeMask |= 1u << s;
        }
        psVar = m_boundPS->getVariant(cubeMask | (fogMode << kFogShift));
        if (!psVar) return false;
        psHandle = psVar->glShader;
    } else {
        FFPStage st[kMaxTSS];
        for (int i = 0; i < kMaxTSS; ++i) {
            if (m_tss[i][D3DTSS_COLOROP] == D3DTOP_DISABLE) break;
            FFPStage& f = st[ffpStages++];
            f.colorOp = m_tss[i][D3DTSS_COLOROP];
            f.colorArg0 = m_tss[i][D3DTSS_COLORARG0];
            f.colorArg1 = m_tss[i][D3DTSS_COLORARG1];
            f.colorArg2 = m_tss[i][D3DTSS_COLORARG2];
            f.alphaOp = m_tss[i][D3DTSS_ALPHAOP];
            f.alphaArg0 = m_tss[i][D3DTSS_ALPHAARG0];
            f.alphaArg1 = m_tss[i][D3DTSS_ALPHAARG1];
            f.alphaArg2 = m_tss[i][D3DTSS_ALPHAARG2];
            f.resultArg = m_tss[i][D3DTSS_RESULTARG];
            f.texCoordIndex = m_tss[i][D3DTSS_TEXCOORDINDEX] & 0xFFFF;
            f.transformFlags = m_tss[i][D3DTSS_TEXTURETRANSFORMFLAGS];
            if ((m_tss[i][D3DTSS_TEXCOORDINDEX] & 0xFFFF0000) != 0) noteUnimplemented("TSS texcoord generation (TCI_*)");
            if ((f.transformFlags & 0xFF) != D3DTTFF_DISABLE) noteUnimplemented("texture transform matrices");
            IDirect3DBaseTexture9* tex = (i < kMaxStages) ? m_boundTextures[i] : nullptr;
            f.texType = !tex ? 0 : (tex->GetType() == D3DRTYPE_CUBETEXTURE ? 2 : 1);
            if (ffpStages >= 4) break;
        }
        // Key of the generated program: the stage descriptions plus specular enable.
        std::string key(reinterpret_cast<const char*>(st), sizeof(FFPStage) * ffpStages);
        key.push_back(m_rs[D3DRS_SPECULARENABLE] ? 1 : 0);
        key.push_back(static_cast<char>(fogMode));
        static std::unordered_map<std::string, std::string> s_srcCache;
        auto it = s_srcCache.find(key);
        if (it == s_srcCache.end()) it = s_srcCache.emplace(key, ffpPixelSource(st, ffpStages, m_rs[D3DRS_SPECULARENABLE] != 0, fogMode)).first;
        psHandle = m_pipelineManager.getFFPPixelShader(it->second);
    }

    LinkedProgram* lp = m_pipelineManager.getProgram(vsHandle, psHandle, programmableVS ? vsVar : nullptr, psVar);
    if (!lp || (instanceLayout && lp->instanceRegistersLoc == -1)) {
        if (instanceLayout && instancingUnavailable) *instancingUnavailable = true;
        return false;
    }
    if (lp != m_currentProgram) {
        glUseProgram(lp->program);
        m_currentProgram = lp;
    }
    m_pipelineManager.commitShaderConstants(*lp);

    const float flip = m_rtFlip ? -1.0f : 1.0f;
    if (lp->vpFlipLoc != -1 && lp->lastFlip != flip) { glUniform1f(lp->vpFlipLoc, flip); lp->lastFlip = flip; }
    {
        const DWORD func = m_rs[D3DRS_ALPHAFUNC];
        const float f = (m_rs[D3DRS_ALPHATESTENABLE] && func != D3DCMP_ALWAYS) ? static_cast<float>(func) : 0.0f;
        const float ref = static_cast<float>(m_rs[D3DRS_ALPHAREF] & 0xFF);
        if (lp->alphaTestLoc != -1 && (lp->lastAlpha[0] != f || lp->lastAlpha[1] != ref)) {
            glUniform4f(lp->alphaTestLoc, f, ref, 0.0f, 0.0f);
            lp->lastAlpha[0] = f;
            lp->lastAlpha[1] = ref;
        }
    }
    if (!programmableVS) {
        if (hasPositionT) {
            const float vw = static_cast<float>(m_viewport.Width ? m_viewport.Width : 1);
            const float vh = static_cast<float>(m_viewport.Height ? m_viewport.Height : 1);
            const float sx = 2.0f / vw, sy = -2.0f / vh;
            const float r[4] = {sx, sy, (0.5f - m_viewport.X) * sx - 1.0f, (0.5f - m_viewport.Y) * sy + 1.0f};
            if (lp->rhwLoc != -1 && std::memcmp(r, lp->lastRhw, sizeof(r)) != 0) {
                glUniform4fv(lp->rhwLoc, 1, r);
                std::memcpy(lp->lastRhw, r, sizeof(r));
            }
        } else if (lp->wvpLoc != -1) {
            Matrix4 wvp = Matrix4::multiply(Matrix4::multiply(m_worldMatrix, m_viewMatrix), m_projMatrix);
            if (std::memcmp(wvp.m, lp->lastWvp, sizeof(lp->lastWvp)) != 0) {
                glUniformMatrix4fv(lp->wvpLoc, 1, GL_FALSE, &wvp.m[0][0]);
                std::memcpy(lp->lastWvp, wvp.m, sizeof(lp->lastWvp));
            }
        }
    }
    if (fogMode != kFogNone && lp->fogColorLoc != -1) {
        const DWORD c = m_rs[D3DRS_FOGCOLOR];
        float f[8] = {((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, 1.0f, 0, 0, 0, 0};
        std::memcpy(&f[4], &m_rs[D3DRS_FOGSTART], 4);
        std::memcpy(&f[5], &m_rs[D3DRS_FOGEND], 4);
        std::memcpy(&f[6], &m_rs[D3DRS_FOGDENSITY], 4);
        static const float drawDist = [] { const char* e = std::getenv("NFSU2_DRAW_DIST"); return e ? static_cast<float>(std::atof(e)) : 0.0f; }();
        f[7] = drawDist;
        if (drawDist > 0 && lp->fogRadLoc != -1) {
            const float vw = static_cast<float>(m_viewport.Width ? m_viewport.Width : 1), vh = static_cast<float>(m_viewport.Height ? m_viewport.Height : 1);
            const float p00 = m_projMatrix.m[0][0] != 0 ? m_projMatrix.m[0][0] : 1.0f, p11 = m_projMatrix.m[1][1] != 0 ? m_projMatrix.m[1][1] : 1.0f;
            const float r[4] = {2.0f / (vw * p00), (1.0f + 2.0f * m_viewport.X / vw) / p00, 2.0f / (vh * p11), (1.0f + 2.0f * m_viewport.Y / vh) / p11};
            if (std::memcmp(r, lp->lastFogRad, sizeof(r)) != 0) { glUniform4fv(lp->fogRadLoc, 1, r); std::memcpy(lp->lastFogRad, r, sizeof(r)); }
        }
        if (std::memcmp(f, lp->lastFog, sizeof(f)) != 0) {
            glUniform4fv(lp->fogColorLoc, 1, f);
            if (lp->fogParamsLoc != -1) glUniform4fv(lp->fogParamsLoc, 1, f + 4);
            std::memcpy(lp->lastFog, f, sizeof(f));
        }
    }
    if (!m_boundPS && lp->tfactorLoc != -1) {
        const DWORD c = m_rs[D3DRS_TEXTUREFACTOR];
        const float tf[4] = {((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, ((c >> 24) & 0xFF) / 255.0f};
        if (std::memcmp(tf, lp->lastTFactor, sizeof(tf)) != 0) {
            glUniform4fv(lp->tfactorLoc, 1, tf);
            std::memcpy(lp->lastTFactor, tf, sizeof(tf));
        }
    }

    // ---- vertex attributes ----
    bool used[16] = {};
    for (int i = 0; i < elemCount; ++i) {
        const int loc = elemLoc[i];
        if (loc < 0 || loc >= m_maxAttribs || used[loc]) continue;
        const auto& e = elems[i];
        const uint8_t* ptr = nullptr;
        GLsizei stride = 0;
        if (upData && e.Stream == 0) {
            glBindBuffer(GL_ARRAY_BUFFER, 0);
            ptr = static_cast<const uint8_t*>(upData) + e.Offset + static_cast<intptr_t>(baseVertex) * upStride;
            stride = static_cast<GLsizei>(upStride);
        } else {
            const StreamBinding& sb = m_streams[e.Stream];
            if (!sb.buffer) continue;
            const int64_t off = static_cast<int64_t>(sb.offset) + e.Offset + static_cast<int64_t>(baseVertex) * sb.stride;
            if (off < 0) continue;
            glBindBuffer(GL_ARRAY_BUFFER, sb.buffer->getVBO());
            ptr = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(off));
            stride = static_cast<GLsizei>(sb.stride);
        }
        GLint size = 4;
        GLenum type = GL_FLOAT;
        GLboolean norm = GL_FALSE;
        switch (e.Type) {
            case D3DDECLTYPE_FLOAT1: size = 1; break;
            case D3DDECLTYPE_FLOAT2: size = 2; break;
            case D3DDECLTYPE_FLOAT3: size = 3; break;
            case D3DDECLTYPE_FLOAT4: size = 4; break;
            case D3DDECLTYPE_D3DCOLOR: type = GL_UNSIGNED_BYTE; norm = GL_TRUE; break;
            case D3DDECLTYPE_UBYTE4: type = GL_UNSIGNED_BYTE; break;
            case D3DDECLTYPE_UBYTE4N: type = GL_UNSIGNED_BYTE; norm = GL_TRUE; break;
            case D3DDECLTYPE_SHORT2: size = 2; type = GL_SHORT; break;
            case D3DDECLTYPE_SHORT4: type = GL_SHORT; break;
            case D3DDECLTYPE_SHORT2N: size = 2; type = GL_SHORT; norm = GL_TRUE; break;
            case D3DDECLTYPE_SHORT4N: type = GL_SHORT; norm = GL_TRUE; break;
            case D3DDECLTYPE_USHORT2N: size = 2; type = GL_UNSIGNED_SHORT; norm = GL_TRUE; break;
            case D3DDECLTYPE_USHORT4N: type = GL_UNSIGNED_SHORT; norm = GL_TRUE; break;
            case D3DDECLTYPE_FLOAT16_2: size = 2; type = 0x8D61; break;  // GL_HALF_FLOAT_OES
            case D3DDECLTYPE_FLOAT16_4: type = 0x8D61; break;
            default:
                noteUnimplemented("vertex element type UDEC3/DEC3N");
                continue;
        }
        if (!m_attribEnabled[loc]) { glEnableVertexAttribArray(loc); m_attribEnabled[loc] = true; }
        glVertexAttribPointer(loc, size, type, norm, stride, ptr);
        used[loc] = true;
    }
    for (int loc = 0; loc < m_maxAttribs; ++loc) {
        if (used[loc]) continue;
        if (m_attribEnabled[loc]) { glDisableVertexAttribArray(loc); m_attribEnabled[loc] = false; }
        if (!programmableVS && loc == kFFPDiffuse) glVertexAttrib4f(loc, 1.0f, 1.0f, 1.0f, 1.0f);
        else glVertexAttrib4f(loc, 0.0f, 0.0f, 0.0f, 1.0f);
    }

    applyTextures(psVar, !m_boundPS, ffpStages);
    applyRenderStates();
    applyViewportAndScissor();
    return true;
}

// Draw index within the traced frame and optional skip range (NFSU2_SKIP_DRAWS=a-b), diagnostics only.
static int g_traceDrawIndex = 0;
// NFSU2_SKIP_MATCH=<kind>:<prims> skips matching draws in the traced frame (e.g. "DIP:162").
static bool traceSkipCurrent(const char* kind, UINT prims) {
    static int lo = -1, hi = -1;
    static std::string mkind;
    static int mprims = -1;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char* e = std::getenv("NFSU2_SKIP_DRAWS")) std::sscanf(e, "%d-%d", &lo, &hi);
        if (const char* e = std::getenv("NFSU2_SKIP_MATCH")) {
            std::string m(e);
            size_t c = m.find(':');
            if (c != std::string::npos) { mkind = m.substr(0, c); mprims = std::atoi(m.c_str() + c + 1); }
        }
    }
    if (!mkind.empty() && mkind == kind && static_cast<int>(prims) == mprims) return true;
    return lo >= 0 && g_traceDrawIndex >= lo && g_traceDrawIndex <= hi;
}

void IDirect3DDevice9GLES::traceDraw(const char* kind, UINT prims) {
    const int tf = traceFrame();
    static uint32_t s_lastFrame = 0;
    if (getD3D9PresentCount() != s_lastFrame) { s_lastFrame = getD3D9PresentCount(); g_traceDrawIndex = 0; }
    ++g_traceDrawIndex;
    ++g_frameDraws;
    if (tf < 0 || static_cast<int>(getD3D9PresentCount()) != tf) return;
    std::printf("[TRACE f=%d #%d] %s prims=%u vs=%p ps=%p fvf=0x%X decl=%p rt=%s %ux%u vp=%u,%u %ux%u z=%u zw=%u blend=%u(%u,%u) at=%u(%u) cull=%u fog=%u\n",
                tf, g_traceDrawIndex, kind, prims, m_boundVS ? static_cast<void*>(static_cast<ShaderBase*>(m_boundVS)) : nullptr, m_boundPS ? static_cast<void*>(static_cast<ShaderBase*>(m_boundPS)) : nullptr, m_boundFVF,
                static_cast<void*>(m_boundDecl), m_rtFlip ? "tex" : "bb", m_rtWidth, m_rtHeight, m_viewport.X,
                m_viewport.Y, m_viewport.Width, m_viewport.Height, m_rs[D3DRS_ZENABLE], m_rs[D3DRS_ZWRITEENABLE],
                m_rs[D3DRS_ALPHABLENDENABLE], m_rs[D3DRS_SRCBLEND], m_rs[D3DRS_DESTBLEND], m_rs[D3DRS_ALPHATESTENABLE],
                m_rs[D3DRS_ALPHAREF], m_rs[D3DRS_CULLMODE], m_rs[D3DRS_FOGENABLE]);
    std::printf("    stencil=%lu func=%lu ref=%lu mask=%lx wmask=%lx ops=%lu/%lu/%lu cw=%lx afunc=%lu zfunc=%lu blendop=%lu sep=%lu\n",
                (unsigned long)m_rs[D3DRS_STENCILENABLE], (unsigned long)m_rs[D3DRS_STENCILFUNC], (unsigned long)m_rs[D3DRS_STENCILREF],
                (unsigned long)m_rs[D3DRS_STENCILMASK], (unsigned long)m_rs[D3DRS_STENCILWRITEMASK], (unsigned long)m_rs[D3DRS_STENCILFAIL],
                (unsigned long)m_rs[D3DRS_STENCILZFAIL], (unsigned long)m_rs[D3DRS_STENCILPASS], (unsigned long)m_rs[D3DRS_COLORWRITEENABLE],
                (unsigned long)m_rs[D3DRS_ALPHAFUNC], (unsigned long)m_rs[D3DRS_ZFUNC], (unsigned long)m_rs[D3DRS_BLENDOP],
                (unsigned long)m_rs[D3DRS_SEPARATEALPHABLENDENABLE]);
    if (m_rs[D3DRS_FOGENABLE]) {
        float fs, fe, fd;
        std::memcpy(&fs, &m_rs[D3DRS_FOGSTART], 4); std::memcpy(&fe, &m_rs[D3DRS_FOGEND], 4); std::memcpy(&fd, &m_rs[D3DRS_FOGDENSITY], 4);
        std::printf("    fog table=%lu vertex=%lu range=%lu start=%g end=%g density=%g color=%08lx\n",
                    (unsigned long)m_rs[D3DRS_FOGTABLEMODE], (unsigned long)m_rs[D3DRS_FOGVERTEXMODE], (unsigned long)m_rs[D3DRS_RANGEFOGENABLE],
                    fs, fe, fd, (unsigned long)m_rs[D3DRS_FOGCOLOR]);
    }
    for (int s = 0; s < 4; ++s) {
        IDirect3DBaseTexture9* t = m_boundTextures[s];
        if (!t) continue;
        D3DSURFACE_DESC d{};
        if (t->GetType() == D3DRTYPE_TEXTURE) static_cast<IDirect3DTexture9GLES*>(t)->GetLevelDesc(0, &d);
        else if (t->GetType() == D3DRTYPE_CUBETEXTURE) static_cast<IDirect3DCubeTexture9GLES*>(t)->GetLevelDesc(0, &d);
        static const bool s_dump = std::getenv("NFSU2_TRACE_DUMP") != nullptr;
        if (s_dump && t->GetType() == D3DRTYPE_TEXTURE) {
            static std::set<void*> s_done;
            auto* tt = static_cast<IDirect3DTexture9GLES*>(t);
            IDirect3DSurface9GLES* sf = tt->getSurfaceLevel(0);
            if (sf && s_done.insert(t).second && !sf->getCpuBuffer().empty()) {
                char fn[96];
                std::snprintf(fn, sizeof(fn), "tex_%p_%ux%u_f%u.pam", static_cast<void*>(t), sf->getWidth(), sf->getHeight(), static_cast<unsigned>(sf->getFormat()));
                if (FILE* f = std::fopen(fn, "wb")) {
                    std::fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", sf->getWidth(), sf->getHeight());
                    std::vector<uint8_t> row(sf->getWidth() * 4);
                    const UINT bpp = d3dFormatBytesPerPixel(sf->getFormat());
                    for (UINT y = 0; y < sf->getHeight(); ++y) {
                        convertRowToRGBA8(sf->getFormat(), sf->getCpuBuffer().data() + static_cast<size_t>(y) * sf->getWidth() * bpp, row.data(), sf->getWidth());
                        std::fwrite(row.data(), 1, row.size(), f);
                    }
                    std::fclose(f);
                }
                std::printf("    (dumped %s)\n", fn);
            }
        }
        std::printf("    tex%d %s %ux%u fmt=%u lv=%lu min=%lu mag=%lu mip=%lu addr=%lu,%lu\n", s,
                    t->GetType() == D3DRTYPE_CUBETEXTURE ? "cube" : "2d", d.Width, d.Height, static_cast<unsigned>(d.Format),
                    static_cast<unsigned long>(t->GetLevelCount()), static_cast<unsigned long>(m_ss[s][D3DSAMP_MINFILTER]),
                    static_cast<unsigned long>(m_ss[s][D3DSAMP_MAGFILTER]), static_cast<unsigned long>(m_ss[s][D3DSAMP_MIPFILTER]),
                    static_cast<unsigned long>(m_ss[s][D3DSAMP_ADDRESSU]), static_cast<unsigned long>(m_ss[s][D3DSAMP_ADDRESSV]));
    }
    if (!m_boundPS) {
        for (int s = 0; s < 2; ++s) {
            std::printf("    tss%d cop=%lu ca1=%lu ca2=%lu aop=%lu aa1=%lu aa2=%lu tci=%lu\n", s,
                        static_cast<unsigned long>(m_tss[s][D3DTSS_COLOROP]), static_cast<unsigned long>(m_tss[s][D3DTSS_COLORARG1]),
                        static_cast<unsigned long>(m_tss[s][D3DTSS_COLORARG2]), static_cast<unsigned long>(m_tss[s][D3DTSS_ALPHAOP]),
                        static_cast<unsigned long>(m_tss[s][D3DTSS_ALPHAARG1]), static_cast<unsigned long>(m_tss[s][D3DTSS_ALPHAARG2]),
                        static_cast<unsigned long>(m_tss[s][D3DTSS_TEXCOORDINDEX]));
        }
    }
}

STDMETHODIMP IDirect3DDevice9GLES::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) {
    if (!PrimitiveCount) return D3D_OK;
    traceDraw("DP", PrimitiveCount);
    if (traceFrame() >= 0 && static_cast<int>(getD3D9PresentCount()) == traceFrame() && traceSkipCurrent("DP", PrimitiveCount)) return D3D_OK;
    if (!prepareDraw(nullptr, 0, 0)) return D3D_OK;
    { DrawTimer timer;
    glDrawArrays(convertPrimitiveType(PrimitiveType), StartVertex, primitiveCountToVertexCount(PrimitiveType, PrimitiveCount)); }
    checkGLError("DrawPrimitive", __LINE__);
    return D3D_OK;
}

// Diagnostics (NFSU2_DRAW_STATS=1): screen-space size of each DrawIndexedPrimitive with a vertex shader,
// from the vertex range's bounding box transformed by c0..c3 (clip = dot(pos, c_i)): how many draws are
// off screen or only a few pixels big (candidates for culling before the game spends work on them).
static void drawStats(IDirect3DDevice9GLES* d, const float* c, const D3DVERTEXELEMENT9* pos, IDirect3DVertexBuffer9GLES* vb, UINT vbOffset, UINT stride,
                      INT base, UINT minV, UINT numV, UINT prims) {
    static uint32_t frame = 0, n = 0, off = 0, behind = 0, crossing = 0, px[6] = {}, bad = 0, primsAll = 0, primsSmall = 0;
    const uint32_t now = getD3D9PresentCount();
    if (now != frame) {
        if (frame && now % 120 == 0 && n) {
            std::printf("[NFSU2 DRAWSTATS] per frame: %u DIP, offscreen %u, behind %u, crossing near %u, <0.5px %u, <1px %u, <2px %u, <4px %u, <8px %u, <16px %u, unknown %u; prims %u (%u in <4px)\n",
                        n / 120, off / 120, behind / 120, crossing / 120, px[0] / 120, px[1] / 120, px[2] / 120, px[3] / 120, px[4] / 120, px[5] / 120, bad / 120, primsAll / 120, primsSmall / 120);
            n = off = behind = crossing = bad = primsAll = primsSmall = 0; for (auto& v : px) v = 0;
        }
        frame = now;
    }
    ++n; primsAll += prims;
    if (!vb || pos->Type != D3DDECLTYPE_FLOAT3 || !stride) { ++bad; return; }
    const uint8_t* data = vb->cpuData();
    const size_t first = size_t(vbOffset) + (size_t(base) + minV) * stride + pos->Offset;
    if (base < 0 || first + size_t(numV ? numV - 1 : 0) * stride + 12 > vb->cpuLength()) { ++bad; return; }
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (UINT i = 0; i < numV; ++i) {
        float p[3]; std::memcpy(p, data + first + size_t(i) * stride, 12);
        for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], p[k]); mx[k] = std::max(mx[k], p[k]); }
    }
    float nx0 = 1e30f, nx1 = -1e30f, ny0 = 1e30f, ny1 = -1e30f; int wpos = 0, outR = 0, outL = 0, outT = 0, outB = 0, far_ = 0;
    for (int k = 0; k < 8; ++k) {
        const float p[4] = {k & 1 ? mx[0] : mn[0], k & 2 ? mx[1] : mn[1], k & 4 ? mx[2] : mn[2], 1.0f};
        float clip[4];
        for (int r = 0; r < 4; ++r) clip[r] = p[0] * c[r * 4] + p[1] * c[r * 4 + 1] + p[2] * c[r * 4 + 2] + p[3] * c[r * 4 + 3];
        if (clip[0] > clip[3]) ++outR; if (clip[0] < -clip[3]) ++outL; if (clip[1] > clip[3]) ++outT; if (clip[1] < -clip[3]) ++outB; if (clip[2] > clip[3]) ++far_;
        if (clip[3] > 1e-4f) { ++wpos; const float x = clip[0] / clip[3], y = clip[1] / clip[3]; nx0 = std::min(nx0, x); nx1 = std::max(nx1, x); ny0 = std::min(ny0, y); ny1 = std::max(ny1, y); }
    }
    if (outR == 8 || outL == 8 || outT == 8 || outB == 8 || far_ == 8) { ++off; return; }
    if (wpos == 0) { ++behind; return; }
    if (wpos < 8) { ++crossing; return; }
    const float size = std::max((nx1 - nx0) * 320.0f, (ny1 - ny0) * 240.0f);
    const float lim[6] = {0.5f, 1, 2, 4, 8, 16};
    for (int k = 0; k < 6; ++k) if (size < lim[k]) ++px[k];
    if (size < 4) primsSmall += prims;
}

STDMETHODIMP IDirect3DDevice9GLES::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount) {
    if (!m_boundIB) return D3DERR_INVALIDCALL;
    if (!PrimitiveCount) return D3D_OK;
    static const bool s_drawStats = std::getenv("NFSU2_DRAW_STATS") != nullptr;
    if (s_drawStats && m_boundVS && m_boundDecl) {
        for (const auto& e : m_boundDecl->getElements()) {
            if (e.Stream == 0xFF) break;
            if (e.Usage == D3DDECLUSAGE_POSITION && e.UsageIndex == 0) {
                float c[16]; m_pipelineManager.getVSConstantF(0, c, 4);
                const StreamBinding& sb = m_streams[e.Stream];
                drawStats(this, c, &e, sb.buffer, sb.offset, sb.stride, BaseVertexIndex, MinVertexIndex, NumVertices, PrimitiveCount);
                break;
            }
        }
    }
    traceDraw("DIP", PrimitiveCount);
    if (traceFrame() >= 0 && static_cast<int>(getD3D9PresentCount()) == traceFrame() && traceSkipCurrent("DIP", PrimitiveCount)) return D3D_OK;
    if (!prepareDraw(nullptr, 0, BaseVertexIndex)) return D3D_OK;
    const bool i16 = m_boundIB->getFormat() == D3DFMT_INDEX16;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_boundIB->getIBO());
    { DrawTimer timer;
    glDrawElements(convertPrimitiveType(PrimitiveType), primitiveCountToVertexCount(PrimitiveType, PrimitiveCount),
                   i16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                   reinterpret_cast<const void*>(static_cast<uintptr_t>(StartIndex * (i16 ? 2 : 4)))); }
    checkGLError("DrawIndexedPrimitive", __LINE__);
    return D3D_OK;
}

// GLES 3 entry points for instancing (the Mali-G31 driver gives an ES 3.2 context; Mesa too), loaded on first use.
void IDirect3DDevice9GLES::initInstancing() {
    m_instancing.initialized = true;
    static const bool off = std::getenv("NFSU2_INSTANCING") && std::getenv("NFSU2_INSTANCING")[0] == '0';
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    if (off || !version || !std::strstr(version, "OpenGL ES 3")) {
        std::printf("[D3D9 GLES] instanced world draws %s\n", off ? "off (NFSU2_INSTANCING=0)" : "unavailable (no OpenGL ES 3 context)");
        return;
    }
#ifdef __ANDROID__
    m_instancing.drawElementsInstanced = reinterpret_cast<void (*)(GLenum, GLsizei, GLenum, const void*, GLsizei)>(eglGetProcAddress("glDrawElementsInstanced"));
    m_instancing.vertexAttribDivisor = reinterpret_cast<void (*)(GLuint, GLuint)>(eglGetProcAddress("glVertexAttribDivisor"));
#else
    m_instancing.drawElementsInstanced = reinterpret_cast<void (*)(GLenum, GLsizei, GLenum, const void*, GLsizei)>(SDL_GL_GetProcAddress("glDrawElementsInstanced"));
    m_instancing.vertexAttribDivisor = reinterpret_cast<void (*)(GLuint, GLuint)>(SDL_GL_GetProcAddress("glVertexAttribDivisor"));
#endif
    if (!m_instancing.drawElementsInstanced || !m_instancing.vertexAttribDivisor) { std::printf("[D3D9 GLES] instanced world draws unavailable (entry points)\n"); return; }
    { GLint vectors = 0; glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS, &vectors);
      if (const char* e = std::getenv("NFSU2_TEST_UNIFORM_VECTORS")) vectors = std::min<GLint>(vectors, std::atoi(e));   // test: a smaller GPU (draws split)
      maxVertexUniformVectors() = vectors; }
    float numbers[kMaxInstancesPerDraw];
    for (int i = 0; i < kMaxInstancesPerDraw; ++i) numbers[i] = static_cast<float>(i);
    glGenBuffers(1, &m_instancing.instanceNumbers);
    glBindBuffer(GL_ARRAY_BUFFER, m_instancing.instanceNumbers);
    glBufferData(GL_ARRAY_BUFFER, sizeof numbers, numbers, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_instancing.available = true;
    std::printf("[D3D9 GLES] instanced world draws on (%s; %d vertex uniform vectors: up to %d instances of 8 registers, %d of 12)\n", version,
                maxVertexUniformVectors(), instancesPerDraw(8), instancesPerDraw(12));
}

bool IDirect3DDevice9GLES::drawIndexedInstanced(D3DPRIMITIVETYPE type, INT baseVertex, UINT minVertex, UINT numVertices, UINT startIndex, UINT primitives,
                                               const uint16_t* registers, uint32_t count, uint32_t instances, const float* data) {
    if (!m_instancing.initialized) initInstancing();
    if (!m_instancing.available || !m_boundIB || !count || count > static_cast<uint32_t>(kMaxInstanceRegisters) || !instances ||
        instances > static_cast<uint32_t>(kMaxInstancesPerDraw)) return false;
    if (!primitives) return true;
    const uint32_t perDraw = static_cast<uint32_t>(instancesPerDraw(static_cast<int>(count)));
    if (perDraw < 2) return false;
    const uint32_t layout = instanceLayoutId(registers, count);
    if (layout > 0x7FFFu) return false;
    traceDraw("DIPI", primitives);
    bool unavailable = false;
    if (!prepareDraw(nullptr, 0, baseVertex, layout, &unavailable)) return !unavailable;   // nothing to draw, as DrawIndexedPrimitive
    LinkedProgram* lp = m_currentProgram;
    const GLuint instanceNumberLocation = static_cast<GLuint>(lp->vs->attributes.size());
    glBindBuffer(GL_ARRAY_BUFFER, m_instancing.instanceNumbers);
    glEnableVertexAttribArray(instanceNumberLocation);
    glVertexAttribPointer(instanceNumberLocation, 1, GL_FLOAT, GL_FALSE, 0, nullptr);
    m_instancing.vertexAttribDivisor(instanceNumberLocation, 1);
    const bool i16 = m_boundIB->getFormat() == D3DFMT_INDEX16;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_boundIB->getIBO());
    for (uint32_t done = 0; done < instances; done += perDraw) {   // as many per draw as the shader's nfs_inst array holds
        const uint32_t n = std::min(perDraw, instances - done);
        glUniform4fv(lp->instanceRegistersLoc, static_cast<GLsizei>(n * count), data + size_t(done) * count * 4);
        DrawTimer timer;
        m_instancing.drawElementsInstanced(convertPrimitiveType(type), primitiveCountToVertexCount(type, primitives), i16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT,
                                           reinterpret_cast<const void*>(static_cast<uintptr_t>(startIndex * (i16 ? 2 : 4))), static_cast<GLsizei>(n));
    }
    m_instancing.vertexAttribDivisor(instanceNumberLocation, 0);
    glDisableVertexAttribArray(instanceNumberLocation);
    m_attribEnabled[instanceNumberLocation] = false;
    checkGLError("drawIndexedInstanced", __LINE__);
    // The register file as drawing them one by one would leave it.
    for (uint32_t j = 0; j < count; ++j) m_pipelineManager.setVSConstantF(registers[j], data + (size_t(instances - 1) * count + j) * 4, 1);
    ++m_instancing.draws; m_instancing.instances += instances;
    (void)minVertex; (void)numVertices;
    return true;
}

STDMETHODIMP IDirect3DDevice9GLES::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) {
    if (!pVertexStreamZeroData) return D3DERR_INVALIDCALL;
    if (!PrimitiveCount) return D3D_OK;
    traceDraw("DPUP", PrimitiveCount);
    if (traceFrame() >= 0 && static_cast<int>(getD3D9PresentCount()) == traceFrame() && traceSkipCurrent("DPUP", PrimitiveCount)) return D3D_OK;
    if (!prepareDraw(pVertexStreamZeroData, VertexStreamZeroStride, 0)) return D3D_OK;
    { DrawTimer timer;
    glDrawArrays(convertPrimitiveType(PrimitiveType), 0, primitiveCountToVertexCount(PrimitiveType, PrimitiveCount)); }
    checkGLError("DrawPrimitiveUP", __LINE__);
    // D3D9: the stream 0 source and index buffer are unset after a UP draw.
    m_streams[0] = StreamBinding{};
    return D3D_OK;
}

STDMETHODIMP IDirect3DDevice9GLES::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void* pIndexData, D3DFORMAT IndexDataFormat, const void* pVertexStreamZeroData, UINT VertexStreamZeroStride) {
    if (!pVertexStreamZeroData || !pIndexData) return D3DERR_INVALIDCALL;
    if (!PrimitiveCount) return D3D_OK;
    traceDraw("DIPUP", PrimitiveCount);
    if (traceFrame() >= 0 && static_cast<int>(getD3D9PresentCount()) == traceFrame() && traceSkipCurrent("DIPUP", PrimitiveCount)) return D3D_OK;
    if (!prepareDraw(pVertexStreamZeroData, VertexStreamZeroStride, 0)) return D3D_OK;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    { DrawTimer timer;
    glDrawElements(convertPrimitiveType(PrimitiveType), primitiveCountToVertexCount(PrimitiveType, PrimitiveCount),
                   IndexDataFormat == D3DFMT_INDEX16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, pIndexData); }
    checkGLError("DrawIndexedPrimitiveUP", __LINE__);
    m_streams[0] = StreamBinding{};
    m_boundIB = nullptr;
    return D3D_OK;
}

}  // namespace d3d9_gles

// R3 + Select (main.cpp): show/hide the frame counter. Returns the new state.
bool toggleD3D9FpsCounter() {
    const bool on = !d3d9_gles::g_fpsOverlay.load();
    d3d9_gles::g_fpsOverlay = on;
    std::printf("[NFSU2] Frame counter %s\n", on ? "on" : "off");
    return on;
}

// Bridge (d3d9_render_thread.inc): a DXT level decoded by a worker thread, uploaded without LockRect/UnlockRect.
bool glesUploadDecodedLevel(IDirect3DTexture9* texture, uint32_t level, const uint8_t* rgba, const uint8_t* rgb565) {
    if (!texture || texture->GetType() != D3DRTYPE_TEXTURE) return false;
    return static_cast<d3d9_gles::IDirect3DTexture9GLES*>(texture)->uploadDecodedLevel(level, rgba, rgb565);
}

// Bridge (d3d9_render_thread.inc): one instanced draw for consecutive replayed world items with the same mesh.
// dip: primitive type, base vertex, min vertex, vertex count, start index, primitive count.
bool glesDrawIndexedInstanced(IDirect3DDevice9* device, const uint32_t* dip, const uint16_t* registers, uint32_t count, uint32_t instances, const float* data) {
    return static_cast<d3d9_gles::IDirect3DDevice9GLES*>(device)->drawIndexedInstanced(static_cast<D3DPRIMITIVETYPE>(dip[0]), static_cast<INT>(dip[1]), dip[2], dip[3],
                                                                                        dip[4], dip[5], registers, count, instances, data);
}
