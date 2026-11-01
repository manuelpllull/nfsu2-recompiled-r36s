#include <set>
#include <condition_variable>
#include "guest_runtime.h"
#include "android_resolution.h"
#include "guest_memory.h"
#include "guest_heap.h"
#include "runtime_log.h"
#include "frame_pacer.h"
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
#include "d3d9_bridge.h"
#include "nfs_widescreen.h"
#include "nfs_exe_addr.h"   // v10 addresses -> the lifted exe's (config/exe_functions.txt)
#endif
#include <array>
#include <utility>
#include <unordered_map>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <fstream>
#include <iterator>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <pthread.h>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <cerrno>
#include <deque>
#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#elif defined(NFS_SDL_AUDIO)
#include <SDL2/SDL.h>
#endif
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

extern "C" {
#include "recomp_types.h"
#include "pe_format.h"
#include "nfs_runtime.h"

// Guest CPU state: per host thread when RECOMP_TLS is __thread (R36S parallel guest threads, see
// HANDOVER 6.18), otherwise one global CPU swapped by State::save/load at machine handovers.
#ifdef RECOMP_TLS_STRUCT
// One struct (recomp_types.h, RECOMP_TLS_STRUCT): registers, flags, x87 window (fp_sp starts in the middle
// of the 256 slots), FPU control word, segments, MMX. Positional: eax..ebp, flags, cur_func, fs/gs base,
// icall_va, fp_top, fp_sp, fpu_cw.
static_assert(sizeof(((RecompCpu*)0)->st_win)/sizeof(double)==RECOMP_X87_SLOTS,"x87 window size");
RECOMP_TLS RecompCpu g_cpu={0,0,0,0,0,0,0,0, 0,0,0,0, 0,0,0,0, 0,RECOMP_X87_SLOTS/2, 0x027f};
#define nfs_icall_va (g_cpu.icall_va)
#else
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_ebp;
#ifndef RECOMP_X87_WINDOW
double g_st[8];
#endif
RECOMP_TLS int g_fp_top;
RECOMP_TLS uint16_t g_fpu_cw = 0x027f;
RECOMP_TLS uint16_t g_seg_cs, g_seg_ds, g_seg_es, g_seg_fs, g_seg_gs, g_seg_ss;
RECOMP_TLS uint64_t g_mm[8];
RECOMP_TLS uint32_t g_fs_base, g_gs_base, g_cur_func;
RECOMP_TLS uint32_t nfs_icall_va;  // VA of the indirect call in flight (arm64_guest.h)
#endif
RECOMP_TLS V128 g_xmm[8];
RECOMP_TLS uint32_t g_mxcsr = 0x1f80;
ptrdiff_t g_mem_base;
uint32_t g_icall_trace[ICALL_TRACE_SIZE], g_icall_from[ICALL_TRACE_SIZE];
uint32_t g_icall_trace_idx, g_icall_count;
}

namespace {
std::atomic<uint32_t> displayWidth{1280},displayHeight{720};
std::atomic<uint32_t> renderWidth{0},renderHeight{0};
std::atomic<unsigned> frameLimit{0};
std::string hex(uint32_t va) {
    std::ostringstream out; out << "0x" << std::hex << std::setw(8) << std::setfill('0') << va;
    return out.str();
}
struct GuestStop : std::runtime_error { using std::runtime_error::runtime_error; };
struct State {
    uint32_t eax{}, ecx{}, edx{}, esp{}, ebx{}, esi{}, edi{}, ebp{}, fs{}, gs{}, cur{};
    uint32_t flags[4]{}, mxcsr{0x1f80};
    uint16_t segments[6]{}, cw{0x027f};
    double st[8]{}; int top{};
    uint64_t mm[8]{}; V128 xmm[8]{};
    void save() {
        eax=g_eax; ecx=g_ecx; edx=g_edx; esp=g_esp; ebx=g_ebx; esi=g_esi; edi=g_edi; ebp=g_ebp;
        fs=g_fs_base; gs=g_gs_base; cur=g_cur_func; mxcsr=g_mxcsr;
        flags[0]=g_flag_k; flags[1]=g_flag_a; flags[2]=g_flag_b; flags[3]=g_flag_cf;
        segments[0]=g_seg_cs; segments[1]=g_seg_ds; segments[2]=g_seg_es;
        segments[3]=g_seg_fs; segments[4]=g_seg_gs; segments[5]=g_seg_ss;
        std::memcpy(st,g_st,sizeof st); top=g_fp_top; cw=g_fpu_cw;
        std::memcpy(mm,g_mm,sizeof mm); std::memcpy(xmm,g_xmm,sizeof xmm);
    }
    void load() const {
        g_eax=eax; g_ecx=ecx; g_edx=edx; g_esp=esp; g_ebx=ebx; g_esi=esi; g_edi=edi; g_ebp=ebp;
        g_fs_base=fs; g_gs_base=gs; g_cur_func=cur; g_mxcsr=mxcsr;
        g_flag_k=flags[0]; g_flag_a=flags[1]; g_flag_b=flags[2]; g_flag_cf=flags[3];
        g_seg_cs=segments[0]; g_seg_ds=segments[1]; g_seg_es=segments[2];
        g_seg_fs=segments[3]; g_seg_gs=segments[4]; g_seg_ss=segments[5];
        std::memcpy(g_st,st,sizeof st); g_fp_top=top; g_fpu_cw=cw;
        std::memcpy(g_mm,mm,sizeof mm); std::memcpy(g_xmm,xmm,sizeof xmm);
    }
};
struct Import { std::string name; recomp_func_t function{}; uint64_t calls{}; bool lockFree{}; recomp_func_t freeFunction{}; };
// R36S (NFS_PARALLEL_GUEST, guest CPU state thread-local through RECOMP_TLS): guest threads run lifted
// code in parallel and hold the machine only inside the runtime (imports, overrides, D3D9 sync calls).
// Before, one guest thread ran at a time, so the game's 100 Hz sound thread and the main thread took
// turns on one core. Enabled right before the game's entry point; NFSU2_PARALLEL_GUEST=0 disables it.
bool parallelGuest=false;
// Which guest threads run without the machine ("free"): only those whose entry point is listed in
// NFSU2_PARALLEL_THREADS (default NFS_SOUND_THREAD_ENTRY: 726117 in v1.0, 7263c7 in v1.2, the game's 100 Hz
// sound/streaming thread). The game's main
// thread and its other workers keep taking turns under the machine: x86 orders stores for every other
// core (TSO), ARM does not, and lifted code has no barriers, so threads that hand data to each other
// through plain memory flags (the movie decoder did: a garbage pointer during the EA logo) must not run
// at the same time. The sound thread talks to the game through Win32 mutexes (barriers in the runtime).
thread_local bool freeRunning=false;
std::vector<uint32_t> freeThreadEntries;
// Entry point of the game's 100 Hz sound/streaming thread, one per SPEED2.EXE (exe_addr_map.py maps 0x00726117
// in v1.0 to 0x007263C7 in v1.2). With the v1.0 address on v1.2 the thread never ran free: it took turns with the
// game thread under the machine, which then waited ~6 ms per frame for it (~25 fps instead of ~27) and the
// sound thread was starved.
#if defined(NFS_EXE_VARIANT) && NFS_EXE_VARIANT==2
#define NFS_SOUND_THREAD_ENTRY "7263c7,726117"
#else
#define NFS_SOUND_THREAD_ENTRY "726117,7263c7"
#endif
std::unique_ptr<GuestMemory> memory;
std::unique_ptr<GuestHeaps> heaps;
// The guest machine lock: FIFO by ticket and recursive per thread, with the
// std::recursive_mutex interface the scheduler code already uses.
//
// std::recursive_mutex is not fair. The game thread releases the machine
// around imports and every 500 us, then asks for it again at once and usually
// wins; the guest audio mixer thread waited behind it, filled its DirectSound
// ring too late, and the mixer replayed stale data ("the sound loops"). The
// same starvation made the Windows build stutter until native32 got a ticket
// lock. Here every unlock hands the machine to the longest waiter.
std::atomic<uint64_t> statMachineLocks{0},statMachineWaits{0},statSyncWaits{0},statSyncNotifies{0};
extern "C" void nfsWaitAdd(int kind,uint64_t ns);
extern "C" bool nfsIsGameThread();
extern "C" void nfsCsWait(uint32_t address,uint64_t ns);
#if !defined(NFS_R36S)   // the R36S build has the real ones (src/runtime/fx_cache.c)
extern "C" int nfs_fx_cap_depth = 0;
extern "C" void nfs_fx_capture(uint32_t, const uint32_t*, uint32_t, int) {}
#endif
class MachineMutex {
    std::mutex m_;
    std::condition_variable cv_;
    uint64_t next_=0,serving_=0;
    std::thread::id owner_{};
    unsigned depth_=0;
public:
    void lock(){
        const auto me=std::this_thread::get_id();
        std::unique_lock<std::mutex> l(m_);
        if(owner_==me){++depth_;return;}
        const uint64_t ticket=next_++;
        ++statMachineLocks;
        if(serving_!=ticket){
            ++statMachineWaits;
            const auto t0=std::chrono::steady_clock::now();
            cv_.wait(l,[&]{return serving_==ticket;});
            const uint64_t waited=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t0).count();
            if(nfsIsGameThread())nfsWaitAdd(2,waited);
            else if(freeRunning){nfsWaitAdd(12,waited);nfsWaitAdd(13,1);}
        }
        owner_=me;depth_=1;
    }
    // True when another thread holds a ticket (is queued for the machine). Called by the owner.
    bool hasWaiters(){std::lock_guard<std::mutex> l(m_);return next_!=serving_+1;}
    bool try_lock(){
        const auto me=std::this_thread::get_id();
        std::lock_guard<std::mutex> l(m_);
        if(owner_==me){++depth_;return true;}
        if(next_!=serving_)return false;
        ++next_;owner_=me;depth_=1;return true;
    }
    // Parallel guest threads (R36S): the owner check, and a full release/re-acquire around guest code.
    bool heldByMe(){std::lock_guard<std::mutex> l(m_);return owner_==std::this_thread::get_id();}
    unsigned releaseAll(){
        unsigned depth;
        {
            std::lock_guard<std::mutex> l(m_);
            if(owner_!=std::this_thread::get_id())return 0;
            depth=depth_;depth_=0;owner_={};++serving_;
            if(next_==serving_)return depth;
        }
        cv_.notify_all();return depth;
    }
    void reacquire(unsigned depth){
        if(!depth)return;
        lock();
        std::lock_guard<std::mutex> l(m_);depth_=depth;
    }
    void unlock(){
        {
            std::lock_guard<std::mutex> l(m_);
            if(--depth_)return;
            owner_={};++serving_;
            if(next_==serving_)return;          // nobody waiting
        }
        cv_.notify_all();
    }
};
MachineMutex machine;
thread_local std::unique_lock<MachineMutex>* machineLease=nullptr;
thread_local uint32_t guestThreadId=1;
std::vector<Import> imports;
std::map<std::string,uint32_t> modules;
thread_local uint32_t lastError=0;
uint32_t imageBase{}, imageSize{}, entry{};
uint32_t resourceRva{},resourceSize{};
// Set and read under the machine lock with no yield in between (lookup -> call):
// plain globals, not thread_local, which cost a TLS descriptor call per COM call.
thread_local uint32_t lastImport{};
std::string hostGameRoot;
std::string guestLanguage="Spanish";
bool guestWidescreen=true;
std::atomic<bool> stopRequested{false};
std::atomic<bool> guestPaused{false};
std::chrono::steady_clock::time_point bootDeadline;
std::string guestThreadFault;
bool timerTestActive=false;std::vector<uint32_t> timerTestCalls;
constexpr uint32_t timerTestAddress=0x7b000010u;
bool generatedTestActive=false;unsigned generatedTestCalls=0;
constexpr uint32_t generatedTestTarget=0x7b000020u;
uint32_t read32(uint32_t va);
// Stall diagnostics: where each guest thread last handed the machine over and
// what it was waiting for, dumped when the game stops presenting frames.
struct GuestThreadTrace {
    std::atomic<uint32_t> func{0},handle{0},esp{0};std::atomic<const char*> wait{nullptr};
    std::array<std::atomic<uint32_t>,16> held{};  // critical sections this thread owns (outermost entries)
    void hold(uint32_t cs){for(auto& slot:held){uint32_t empty=0;if(slot.compare_exchange_strong(empty,cs))return;}}
    void release(uint32_t cs){for(auto& slot:held){uint32_t value=cs;if(slot.compare_exchange_strong(value,0))return;}}
    bool holdsLocks() const {
        for(const auto& slot:held){if(slot.load(std::memory_order_acquire)!=0)return true;}
        return false;
    }
    bool holds(uint32_t cs) const {
        if(!cs)return false;
        for(const auto& slot:held){if(slot.load(std::memory_order_acquire)==cs)return true;}
        return false;
    }
};
std::mutex threadTracesMutex;
std::map<uint32_t,std::shared_ptr<GuestThreadTrace>> threadTraces;
thread_local std::shared_ptr<GuestThreadTrace> threadTrace;
GuestThreadTrace& currentTrace(){
    if(!threadTrace){threadTrace=std::make_shared<GuestThreadTrace>();std::lock_guard<std::mutex> lock(threadTracesMutex);threadTraces[guestThreadId]=threadTrace;}
    return *threadTrace;
}
void forgetThreadTrace(){std::lock_guard<std::mutex> lock(threadTracesMutex);threadTraces.erase(guestThreadId);threadTrace.reset();}
struct WaitScope {
    WaitScope(const char* what,uint32_t handle){auto& trace=currentTrace();trace.wait=what;trace.handle=handle;}
    ~WaitScope(){currentTrace().wait=nullptr;}
};
std::atomic<int64_t> lastPresentMs{0};
int64_t steadyMs(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void startStallWatch(){
    static std::once_flag started;
    std::call_once(started,[]{std::thread([]{
        int64_t reportedFor=0;
        while(!stopRequested.load()){
            std::this_thread::sleep_for(std::chrono::seconds(2));
            int64_t last=lastPresentMs.load(),idle=steadyMs()-last;
            if(guestPaused.load()||idle<12000||reportedFor==last)continue;
            reportedFor=last;
            NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU2","Guest stall: no frame for %lld s; guest threads:",static_cast<long long>(idle/1000));
            std::lock_guard<std::mutex> lock(threadTracesMutex);
            for(auto& [id,trace]:threadTraces){
                const char* wait=trace->wait.load();uint32_t handle=trace->handle.load();
                std::string detail,held,callers;
                // Read-only peeks at guest memory from this host thread: stable while the guest is stuck.
                if(wait&&!std::strcmp(wait,"EnterCriticalSection")&&memory&&memory->readable(handle,24))
                    detail=" (owner thread "+std::to_string(read32(handle+12))+", count "+std::to_string(read32(handle+8))+")";
                for(auto& slot:trace->held)if(uint32_t cs=slot.load())held+=" "+hex(cs);
                uint32_t esp=trace->esp.load();
                for(unsigned i=0,found=0;memory&&i<256&&found<10;++i){
                    if(!memory->readable(esp+i*4,4))break;
                    uint32_t value=read32(esp+i*4);
                    if(value>=0x401000u&&value<0x75d000u&&recomp_lookup(value)==nullptr){callers+=" "+hex(value);++found;}
                }
                NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU2","  thread %u at %08x %s %08x%s; holds:%s; stack:%s",id,trace->func.load(),
                    wait?wait:"running",wait?handle:0u,detail.c_str(),held.empty()?" none":held.c_str(),callers.c_str());
            }
        }
    }).detach();});
}
void yieldGuestMachine(unsigned milliseconds=0,std::chrono::steady_clock::time_point deadline={}) {
    if(parallelGuest&&!machine.heldByMe()){if(milliseconds)std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));return;}
    if(!machineLease||!machineLease->owns_lock())throw GuestStop("Guest scheduler ownership missing");
    // Nothing queued for the machine: a plain yield would only cost an unlock, a syscall and a relock.
    if(!milliseconds&&!machine.hasWaiters())return;
    if(!milliseconds&&currentTrace().holdsLocks())return;
    {auto& trace=currentTrace();trace.func=g_cur_func;trace.esp=g_esp;}
    State saved;saved.save();machineLease->unlock();
    if(deadline!=std::chrono::steady_clock::time_point{})std::this_thread::sleep_until(deadline);
    else if(milliseconds)std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));else std::this_thread::yield();
    machineLease->lock();saved.load();
}
// Blocking waits for guest synchronization objects. Guest threads only touch events, mutexes and
// critical sections while holding the machine, so: read the generation under the machine, test the
// condition, then waitGuestSync(generation) releases the machine and sleeps until any sync object
// changes (notifyGuestSync) or the timeout expires. This replaces 1 ms polling loops that kept every
// worker waking up and contending for the machine lock (see HANDOVER 6.10).
std::mutex guestSyncMutex;
std::condition_variable guestSyncCv;
std::atomic<uint64_t> guestSyncGeneration{0};
// Threads inside waitGuestSync, changed under guestSyncMutex like the generation: a notify that sees none skips the
// futex wake (the game releases its mutexes and leaves critical sections ~100 times a frame, mostly unwatched); a
// waiter that registers later sees the new generation before it sleeps.
unsigned guestSyncWaiters=0;
uint64_t statSyncNotifiesUnwatched=0;
void notifyGuestSync(){
    ++statSyncNotifies;
    bool watched;
    {std::lock_guard<std::mutex> l(guestSyncMutex);guestSyncGeneration.fetch_add(1);watched=guestSyncWaiters!=0;if(!watched)++statSyncNotifiesUnwatched;}
    if(watched)guestSyncCv.notify_all();
}
// ready (optional) is evaluated under guestSyncMutex without the machine: writers change the object
// under the machine and then call notifyGuestSync (which takes guestSyncMutex), so the change is visible.
// Waiters whose own object did not change keep sleeping instead of queueing for the machine.
template<class Ready>
void waitGuestSync(uint64_t seen,unsigned maxMilliseconds,Ready ready){
    ++statSyncWaits;
    if(parallelGuest&&!machine.heldByMe()){
        std::unique_lock<std::mutex> l(guestSyncMutex);
        ++guestSyncWaiters;
        guestSyncCv.wait_for(l,std::chrono::milliseconds(maxMilliseconds),[&]{return stopRequested.load()||(guestSyncGeneration.load()!=seen&&ready());});
        --guestSyncWaiters;
        return;
    }
    if(!machineLease||!machineLease->owns_lock())throw GuestStop("Guest scheduler ownership missing");
    State saved;saved.save();machineLease->unlock();
    {
        std::unique_lock<std::mutex> l(guestSyncMutex);
        ++guestSyncWaiters;
        guestSyncCv.wait_for(l,std::chrono::milliseconds(maxMilliseconds),[&]{return stopRequested.load()||(guestSyncGeneration.load()!=seen&&ready());});
        --guestSyncWaiters;
    }
    machineLease->lock();saved.load();
}
void waitGuestSync(uint64_t seen,unsigned maxMilliseconds){waitGuestSync(seen,maxMilliseconds,[]{return true;});}
void checkGuestProgress() {
    while(guestPaused.load()&&!stopRequested.load())yieldGuestMachine(20);  // sleeps when not holding the machine
    if(stopRequested.load())throw GuestStop("Guest execution cancelled");
    // Imports and lifted back edges can reach this hook many times per frame.
    // Keep cancellation immediate, but amortize full register saves and native
    // scheduler handovers over a short time slice rather than every call.
    // One time slice for whichever guest thread holds the machine.
    static thread_local unsigned checkpoints=0;
    static thread_local auto nextYield=std::chrono::steady_clock::time_point::min();
    if((++checkpoints&63u)==0){
        auto now=std::chrono::steady_clock::now();
        if(bootDeadline!=std::chrono::steady_clock::time_point::max()&&now>bootDeadline)throw GuestStop("CRT bring-up time budget reached at "+hex(g_cur_func));
        // Time slicing only matters while threads take turns; parallel guest code holds no machine.
        if(now>=nextYield&&(!parallelGuest||machine.heldByMe())){
            if(!currentTrace().holdsLocks()){
                if(machine.hasWaiters())yieldGuestMachine();
                nextYield=std::chrono::steady_clock::now()+std::chrono::microseconds(500);
            }
        }
    }
    if(!guestThreadFault.empty())throw GuestStop(guestThreadFault);
}
constexpr uint32_t importBase = 0x7f000000u;
constexpr uint32_t stackBase = 0x20000000u, stackSize = 16u << 20, tib = 0x21000000u;

void* ptr(uint32_t va, uint64_t bytes) { return memory->address(va, bytes); }
uint32_t read32(uint32_t va) { uint32_t v; std::memcpy(&v, ptr(va,4),4); return v; }
void write32(uint32_t va, uint32_t v) { std::memcpy(ptr(va,4),&v,4); }
uint32_t arg(uint32_t index) { return read32(g_esp + 4 + index*4); }
void ret(uint32_t value, uint32_t argc) { g_eax=value; g_esp+=4+argc*4; }
thread_local uint32_t stackTib=0, stackLow=0, stackHigh=0, stackMinEsp=UINT32_MAX;
thread_local bool stackOutsideLogged=false;
void sampleGuestStack() {
    if (!g_fs_base || !memory || !memory->readable(g_fs_base, 12)) return;
    if (stackTib != g_fs_base) {
        stackTib = g_fs_base;
        stackLow = read32(g_fs_base + 8);
        stackHigh = read32(g_fs_base + 4);
        stackMinEsp = stackHigh;
        stackOutsideLogged = false;
    }
    if (g_esp < stackLow || g_esp > stackHigh) {
        if (!stackOutsideLogged) {
            stackOutsideLogged = true;
            NFS_RUNTIME_LOG(ANDROID_LOG_ERROR, "NFSU2", "Guest ESP outside TIB stack: esp=%08x range=%08x-%08x tid=%u",
                            g_esp, stackLow, stackHigh, guestThreadId);
        }
        return;
    }
    if (g_esp < stackMinEsp) stackMinEsp = g_esp;
}
std::string guestString(uint32_t address) {
    if(!address) return {};
    std::string s;
    for(uint32_t i=0;i<32768;++i) {
        char c=*static_cast<char*>(ptr(address+i,1));
        if(!c) return s;
        s.push_back(c);
    }
    throw GuestStop("Unterminated guest string at "+hex(address));
}
std::string lower(std::string s) {
    std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return char(std::tolower(c));}); return s;
}
uint32_t allocate(uint32_t bytes) {
    uint32_t result=heaps->alloc(GuestHeaps::process,8,bytes);
    if(!result) throw GuestStop("Guest allocator exhausted");
    return result;
}
void unsupported() {
    uint32_t index = (lastImport-importBase)/16;
    throw GuestStop("HLE pendiente: " + imports.at(index).name + " desde " + hex(g_cur_func));
}
void getVersion() {
    uint32_t out = arg(0), bytes=read32(out);
    if (bytes != 148 && bytes != 156) { ret(0,1); return; }
    std::memset(ptr(out+4,bytes-4),0,bytes-4);
    write32(out+4,5); write32(out+8,1); write32(out+12,2600); write32(out+16,2);
    if (bytes==156) { auto* p=static_cast<unsigned char*>(ptr(out+148,8)); p[6]=1; }
    ret(1,1);
}
// guest_modules.inc (game DLLs run as lifted code) needs callGuest, so it comes later.
uint32_t loadGuestModule(const std::string& name);
bool freeGuestModule(uint32_t handle);
uint32_t guestModuleHandle(const std::string& name);
bool guestModuleExport(uint32_t handle,const std::string& name,uint32_t& va);
bool isGuestModule(uint32_t handle);
void moduleHandle() {
    if(!arg(0)) { ret(imageBase,1); return; }
    auto name=lower(guestString(arg(0)));
    auto separator=name.find_last_of("/\\"); if(separator!=std::string::npos) name=name.substr(separator+1);
    if(name=="speed2.exe") { ret(imageBase,1); return; }
    if(name.find('.')==std::string::npos) name+=".dll";
    if(uint32_t loaded=guestModuleHandle(name)) { ret(loaded,1); return; }
    auto module=modules.find(name);
    static std::set<std::string> loggedLookups;
    if(loggedLookups.insert(name).second)
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Module lookup %s",name.c_str());
    if(module==modules.end()) lastError=126;
    ret(module==modules.end()?0:module->second,1);
}
void heapCreate() {
    ret(heaps->create(arg(0),arg(1),arg(2)),3);
}
void heapAlloc() { ret(heaps->alloc(arg(0),arg(1),arg(2)),3); }
void heapFree() { ret(heaps->free(arg(0),arg(2)),3); }
void heapRealloc() { ret(heaps->realloc(arg(0),arg(1),arg(2),arg(3)),4); }
void heapSize() { ret(heaps->size(arg(0),arg(2)),3); }
void heapDestroy() { ret(heaps->destroy(arg(0)),1); }
void getProcessHeap() { ret(GuestHeaps::process,0); }
void getProcAddress();
thread_local uint32_t qpcStreak=0;  // consecutive QueryPerformanceCounter reads (guest_kernel.inc)
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
thread_local uint32_t d3dToken{};
void direct3DCreate(){ret(createGuestD3D9(arg(0)),1);}
void freeD3DGuest(uint32_t address){heaps->free(GuestHeaps::process,address);}
void serviceGuestSound();
// D3D9 methods read at most args[0..9]: copy 12.
extern "C" int nfs_fx_cap_depth;
extern "C" void nfs_fx_capture(uint32_t token,const uint32_t* args,uint32_t count,int deferred);
void d3dDispatch(){uint32_t args[32];std::memcpy(args,ptr(g_esp+4,48),48);uint32_t count=0;uint32_t token=d3dToken;
    if(__builtin_expect(nfs_fx_cap_depth!=0,0))nfs_fx_capture(token,nullptr,0,0);  // effect state memo: not replayable
    // Fast path: a queued (or filtered) call other than Present returns straight away.
    if(parallelGuest&&token!=0x7e002110u){uint32_t result;
        if(dispatchGuestD3D9TryDefer(token,args,&count,&result)){qpcStreak=0;ret(result,count);return;}}
    qpcStreak=0;  // drawing: not a busy-wait (see performanceCounter)
    static uint64_t bridgeCalls=0;static std::chrono::nanoseconds bridgeTime{},presentTime{};
    // Time one call in 64 (and every Present) so the statistic does not cost
    // two clock reads on each of the ~40k calls in a race frame; the stack-depth
    // sample (a guest memory map lookup) follows the same rate.
    bool timed=(++bridgeCalls&63u)==0||token==0x7e002110u;
    if(timed&&!parallelGuest)sampleGuestStack();  // reads the guest page map: not without the machine
    std::chrono::steady_clock::time_point bridgeStart;if(timed)bridgeStart=std::chrono::steady_clock::now();uint32_t result;
    try{
        if(!parallelGuest)result=dispatchGuestD3D9(token,args,&count);
        else if(token!=0x7e002110u||!dispatchGuestD3D9TryDefer(token,args,&count,&result)){  // others: tried above
            // Synchronous D3D9 call: the bridge allocates guest memory (heaps): needs the machine.
            if(!freeRunning)result=dispatchGuestD3D9(token,args,&count);
            else{
                machine.lock();
                try{result=dispatchGuestD3D9(token,args,&count);}catch(...){machine.unlock();throw;}
                machine.unlock();
            }
        }
    }catch(...){
        NFS_RUNTIME_LOG(ANDROID_LOG_ERROR,"NFSU2","D3D9 guest caller=%08x stack=%08x token=%08x self=%08x",g_cur_func,g_esp,token,args[0]);throw;}
    if(timed){auto spent=std::chrono::steady_clock::now()-bridgeStart;bridgeTime+=token==0x7e002110u?spent:spent*64;presentTime+=token==0x7e002110u?spent:std::chrono::nanoseconds{};}
    if(token==0x7e002110u){
        nfs_widescreen_apply_frame_rate();
        lastPresentMs=steadyMs();
        startStallWatch();
        // Diagnostics: NFSU2_IMPORT_STATS=1 logs the most-called imports (per frame) every 600 frames.
        static const bool importStats=std::getenv("NFSU2_IMPORT_STATS")!=nullptr;
        static unsigned statFrames=0;static std::vector<uint64_t> lastCalls;
        if(importStats&&++statFrames%600==0){
            lastCalls.resize(imports.size());
            std::vector<std::pair<uint64_t,size_t>> rows;
            for(size_t i=0;i<imports.size();++i){rows.push_back({imports[i].calls-lastCalls[i],i});lastCalls[i]=imports[i].calls;}
            std::sort(rows.rbegin(),rows.rend());
            static uint64_t l0=0,l1=0,l2=0,l3=0;
            static uint64_t l4=0;
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","sync stats: machine locks %.1f/frame (%.1f waited), guest sync waits %.1f/frame, notifies %.1f/frame (%.1f with nobody waiting)",
                (statMachineLocks-l0)/600.0,(statMachineWaits-l1)/600.0,(statSyncWaits-l2)/600.0,(statSyncNotifies-l3)/600.0,(statSyncNotifiesUnwatched-l4)/600.0);
            l0=statMachineLocks;l1=statMachineWaits;l2=statSyncWaits;l3=statSyncNotifies;l4=statSyncNotifiesUnwatched;
            for(size_t i=0;i<rows.size()&&i<12&&rows[i].first;++i)
                NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","import stats: %s %.1f/frame%s",imports[rows[i].second].name.c_str(),rows[i].first/600.0,imports[rows[i].second].lockFree?" (lock-free)":"");
        }
    }
    if(token==0x7e002110u&&result==0){
#if defined(NFS_R36S)
        static unsigned appliedLimit=0;static auto nextFrame=std::chrono::steady_clock::now();
        unsigned cap=frameLimit.load(std::memory_order_relaxed);
        auto now=std::chrono::steady_clock::now();
        if(cap!=appliedLimit){appliedLimit=cap;nextFrame=now;}
        // Next deadline one period after the previous one, but never in the past: a frame that ran late
        // starts the next period now. (It used to reset only after falling 200 ms behind and then sleep a
        // whole period: at 5 fps that slept 33 ms every other frame.)
        if(cap){nextFrame=std::max(nextFrame+std::chrono::nanoseconds(1000000000ull/cap),now);
            if(nextFrame>now){std::this_thread::sleep_until(nextFrame);if(nfsIsGameThread())nfsWaitAdd(9,std::chrono::duration_cast<std::chrono::nanoseconds>(nextFrame-now).count());}}
#else
        static FramePacer pacer;
        auto now=std::chrono::steady_clock::now();
        auto deadline=pacer.deadline(now,frameLimit.load(std::memory_order_relaxed));
        // The renderer has returned; guest audio/streaming workers can use the
        // machine while this thread waits. Late frames incur no extra delay.
        if(deadline>now)yieldGuestMachine(0,deadline);
#endif
        static unsigned frames=0;static uint64_t totalFrames=0;static auto since=std::chrono::steady_clock::now();
        ++totalFrames;
        if(++frames==120){serviceGuestSound();auto now=std::chrono::steady_clock::now();double seconds=std::chrono::duration<double>(now-since).count();
            uint32_t stackUsed=(stackHigh>stackMinEsp)?stackHigh-stackMinEsp:0;
            uint32_t stackSize=(stackHigh>stackLow)?stackHigh-stackLow:0;
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Guest D3D9 frame=%llu %.1f FPS over %.2f seconds; bridge ~%.2f ms/frame (present %.2f), %.0f calls/frame; guest stack %u/%u KiB",
                static_cast<unsigned long long>(totalFrames),frames/seconds,seconds,double(bridgeTime.count())/1e6/frames,double(presentTime.count())/1e6/frames,double(bridgeCalls)/frames,stackUsed/1024,stackSize/1024);
            frames=0;since=now;bridgeTime=presentTime=std::chrono::nanoseconds{};bridgeCalls=0;}
    }ret(result,count);}
#endif
// threadStart: a thread procedure may be stdcall or cdecl; Windows exits the thread right after either.
uint32_t callGuest(uint32_t va,const std::vector<uint32_t>& args,uint32_t cleanup=0,bool threadStart=false);
#include "guest_threads.inc"
#include "guest_files.inc"
#include "guest_kernel.inc"
#include "guest_input.inc"
#include "guest_audio.inc"
#include "guest_network.inc"
void testGuestScheduling(){
    State original;original.save();uint32_t savedError=lastError,savedTLS=tlsValues[1087];
    g_eax=0x11223344;g_fs_base=0x21000000;g_st[0]=123.5;g_xmm[7].u64[0]=0x123456789abcdef0ull;
    lastError=111;tlsValues[1087]=0x55;
    bool done=false;std::string failure;
    std::thread worker([&]{
        std::unique_lock<MachineMutex> lease(machine);machineLease=&lease;guestThreadId=9000;
        State fresh;fresh.eax=0xaabbccdd;fresh.fs=0xdead1000;fresh.st[0]=-456.25;fresh.xmm[7].u64[0]=0xfedcba9876543210ull;fresh.load();
        lastError=222;tlsValues[1087]=0xaa;
        try{for(unsigned i=0;i<100;++i){checkGuestProgress();yieldGuestMachine();
            if(g_eax!=0xaabbccdd||g_fs_base!=0xdead1000||g_st[0]!=-456.25||g_xmm[7].u64[0]!=0xfedcba9876543210ull||lastError!=222||tlsValues[1087]!=0xaa)
                throw GuestStop("Guest worker context was not isolated");}}
        catch(const std::exception& error){failure=error.what();}
        forgetThreadTrace();done=true;machineLease=nullptr;
    });
    try{while(!done){checkGuestProgress();yieldGuestMachine(0,std::chrono::steady_clock::now()+std::chrono::milliseconds(1));
        if(g_eax!=0x11223344||g_fs_base!=0x21000000||g_st[0]!=123.5||g_xmm[7].u64[0]!=0x123456789abcdef0ull||lastError!=111||tlsValues[1087]!=0x55)
            failure="Main guest context was not preserved during handover";
    }}catch(const std::exception& error){failure=error.what();}
    machineLease->unlock();worker.join();machineLease->lock();original.load();lastError=savedError;tlsValues[1087]=savedTLS;
    if(!failure.empty())throw GuestStop(failure);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Guest scheduler passed 100 handovers with separate integer, FS, x87, XMM, TLS and last-error state");
}
void testGuestSoundCaps(){
    State saved;saved.save();
    uint32_t output=allocate(96),device=createSoundObject(1);
    write32(output,96);
    uint32_t result=callGuest(soundMethodBase+0x1000+4*16,{device,output});
    bool ok=result==0&&read32(output)==96&&read32(output+4)==0xf1f&&read32(output+8)==100&&
        read32(output+12)==200000&&read32(output+16)==1&&read32(output+20)==0;
    callGuest(soundMethodBase+0x1000+2*16,{device});heaps->free(GuestHeaps::process,output);saved.load();
    if(!ok)throw GuestStop("DirectSound PCM capabilities regression");
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSMW","DirectSound caps passed: mono/stereo, 8/16-bit, continuous rates, software buffers and guest ABI");
}
recomp_func_t resolveFunction(const std::string& dll, const std::string& name) {
    if((dll=="shfolder.dll"||dll=="shell32.dll")&&name=="SHGetFolderPathA")return shellFolderPath;
    if(dll=="dsound.dll"&&(name=="#1"||name=="DirectSoundCreate"))return directSoundCreate;
    if(dll=="dinput8.dll"&&name=="DirectInput8Create")return directInputCreate;
    if(dll=="gdi32.dll"&&name=="DeleteObject")return deleteGdiObject;
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
    if(dll=="d3d9.dll"&&name=="Direct3DCreate9")return direct3DCreate;
#endif
    if(dll=="advapi32.dll"&&name=="RegOpenKeyA")return regOpenKeyA;
    if(dll=="advapi32.dll"&&name=="RegOpenKeyExA")return regOpenKeyExA;
    if(dll=="advapi32.dll"&&name=="RegCloseKey")return regCloseKey;
    if(dll=="advapi32.dll"&&name=="RegQueryValueExA")return regQueryValueExA;
    if(dll=="advapi32.dll"&&name=="RegSetValueExA")return regSetValueExA;
    if(dll=="ws2_32.dll"||dll=="wsock32.dll"){auto found=networkFunctions.find(name);if(found!=networkFunctions.end())return found->second;}
    if(dll=="netapi32.dll"&&name=="Netbios")return netbios;
    if(dll=="tapi32.dll"&&name=="lineInitialize")return lineInitialize;
    if(dll=="tapi32.dll"&&name=="lineShutdown")return lineShutdown;
    if(dll=="user32.dll") {auto found=windowFunctions.find(name);if(found!=windowFunctions.end())return found->second;}
    if(dll=="winmm.dll") {auto found=timerFunctions.find(name);if(found!=timerFunctions.end())return found->second;}
    if (dll == "kernel32.dll") {
        auto resource=resourceFunctions.find(name);if(resource!=resourceFunctions.end())return resource->second;
        auto file=fileFunctions.find(name);if(file!=fileFunctions.end())return file->second;
        auto kernel=kernelFunctions.find(name); if(kernel!=kernelFunctions.end()) return kernel->second;
        if (name=="GetVersionExA") return getVersion;
        if (name=="GetModuleHandleA") return moduleHandle;
        if (name=="HeapCreate") return heapCreate;
        if (name=="HeapAlloc") return heapAlloc;
        if (name=="HeapFree") return heapFree;
        if (name=="HeapReAlloc") return heapRealloc;
        if (name=="HeapSize") return heapSize;
        if (name=="HeapDestroy") return heapDestroy;
        if (name=="GetProcessHeap") return getProcessHeap;
    }
    return unsupported;
}
void getProcAddress() {
    uint32_t module=arg(0),namePointer=arg(1);
    auto found=std::find_if(modules.begin(),modules.end(),[&](auto& m){return m.second==module;});
    auto name=namePointer<=0xffff?"#"+std::to_string(namePointer):guestString(namePointer);
    if(uint32_t va;found==modules.end()&&guestModuleExport(module,name,va)) { ret(va,2); return; }
    if(found==modules.end()) { lastError=isGuestModule(module)?127:126; ret(0,2); return; }
    auto full=found->first+"!"+name;
    for(uint32_t i=0;i<imports.size();++i) if(imports[i].name==full) { ret(importBase+i*16,2); return; }
    // Only advertise exports that are actually implemented, not permissive stubs.
    auto fn=resolveFunction(found->first,name);
    if(fn==unsupported) { lastError=127; ret(0,2); return; }
    uint32_t va=importBase+uint32_t(imports.size())*16;
    imports.push_back({full,fn}); ret(va,2);
}

template<class T> T fileStruct(const std::vector<unsigned char>& file, uint64_t offset) {
    if (offset > file.size() || sizeof(T) > file.size()-offset) throw GuestStop("Truncated PE structure");
    T value{}; std::memcpy(&value,file.data()+offset,sizeof value); return value;
}
void moduleRange(uint32_t size, uint32_t rva, uint64_t bytes) {
    if (rva > size || bytes > uint64_t(size)-rva) throw GuestStop("PE image range invalid");
}
std::string moduleString(uint32_t base, uint32_t size, uint32_t rva) {
    moduleRange(size,rva,1); std::string value;
    for (uint32_t i=rva; i<size && value.size()<512; ++i) {
        char c = *static_cast<char*>(ptr(base+i,1)); if (!c) return value; value.push_back(c);
    }
    throw GuestStop("Unterminated PE import name");
}
void imageRange(uint32_t rva, uint64_t bytes) { moduleRange(imageSize,rva,bytes); }
// Writes an import token into every IAT slot of a mapped image (SPEED2.EXE or a game DLL).
void bindImports(uint32_t base, uint32_t size, uint32_t directoryRva, uint32_t directorySize) {
    moduleRange(size,directoryRva,directorySize);
    bool terminated=false;
    for (uint32_t offset=0; uint64_t(offset)+sizeof(pe_import_descriptor)<=directorySize; offset+=sizeof(pe_import_descriptor)) {
        pe_import_descriptor descriptor{};
        std::memcpy(&descriptor,ptr(base+directoryRva+offset,sizeof descriptor),sizeof descriptor);
        if (!descriptor.Name && !descriptor.FirstThunk) { terminated=true; break; }
        auto dll=moduleString(base,size,descriptor.Name);
        std::transform(dll.begin(),dll.end(),dll.begin(),[](unsigned char c){return char(std::tolower(c));});
        if(!modules.count(dll)) modules[dll]=0x71000000u+uint32_t(modules.size())*0x10000u;
        uint32_t names=descriptor.OriginalFirstThunk ? descriptor.OriginalFirstThunk : descriptor.FirstThunk;
        for (uint32_t i=0;;++i) {
            uint64_t nameRva=uint64_t(names)+i*4ull, iatRva=uint64_t(descriptor.FirstThunk)+i*4ull;
            if (nameRva>UINT32_MAX || iatRva>UINT32_MAX) throw GuestStop("PE thunk overflow");
            moduleRange(size,uint32_t(nameRva),4); moduleRange(size,uint32_t(iatRva),4);
            uint32_t thunk=read32(base+uint32_t(nameRva)); if (!thunk) break;
            std::string name;
            if (thunk&PE_ORDINAL_FLAG) name="#"+std::to_string(thunk&0xffff);
            else { moduleRange(size,thunk,3); name=moduleString(base,size,thunk+2); }
            if (imports.size()>=4096) throw GuestStop("Too many PE imports");
            uint32_t va=importBase+uint32_t(imports.size())*16;
            imports.push_back({dll+"!"+name,resolveFunction(dll,name)});
            write32(base+uint32_t(iatRva),va);
        }
    }
    if (!terminated) throw GuestStop("Unterminated PE import descriptors");
}
extern "C" const char nfs_exe_lifted_from[] __attribute__((weak));   // recomp_variant.c
// The launcher (ports/r36s/package/NFSU2.sh) finds this string in the binary to check the exe before starting.
extern "C" __attribute__((used)) const char nfs_exe_built_for[] = NFS_EXE_BUILT_FOR;
void loadImage(const char* path) {
    std::ifstream stream(path,std::ios::binary);
    if (!stream) throw GuestStop("Cannot open game PE");
    std::vector<unsigned char> file((std::istreambuf_iterator<char>(stream)),{});
    auto dos=fileStruct<pe_dos_header>(file,0);
    if (dos.e_magic!=0x5a4d || dos.e_lfanew<0) throw GuestStop("Invalid DOS header");
    auto nt=fileStruct<pe_nt_headers32>(file,dos.e_lfanew);
    if (nt.Signature!=0x4550 || nt.FileHeader.Machine!=0x14c || nt.OptionalHeader.Magic!=0x10b ||
        nt.FileHeader.SizeOfOptionalHeader<sizeof(pe_opt_header32)) throw GuestStop("Expected x86 PE32");
    auto opt=nt.OptionalHeader; imageBase=opt.ImageBase; imageSize=opt.SizeOfImage;
    resourceRva=opt.DataDirectory[2].VirtualAddress;resourceSize=opt.DataDirectory[2].Size;
    if (imageBase!=0x400000 || (opt.AddressOfEntryPoint!=0x35bcc7 && opt.AddressOfEntryPoint!=0x35b9f1))
        throw GuestStop("PE layout does not match generated NFSU2 code");
    // The lifted code (recomp_variant.c, written by run_lift.py), the native helpers (nfs_exe_addr.h) and the
    // SPEED2.EXE mapped here must all be the same build: a lift of one exe runs the other's data into wrong code.
    {
        const char* lifted = nfs_exe_lifted_from ? nfs_exe_lifted_from : "v10";   // lifts before 2026-10-24: v10
        if (std::strcmp(lifted, NFS_EXE_ID) != 0)
            throw GuestStop(std::string("Native code built for SPEED2.EXE ")+NFS_EXE_ID+" but the lifted code is from "+lifted+
                            ": rebuild after the lift (src/runtime/nfs_exe_addr.h follows src/recomp/gen)");
        if (file.size() != NFS_EXE_SIZE)
            throw GuestStop("SPEED2.EXE is "+std::to_string(file.size())+" bytes, but this build was lifted from the "+
                            std::to_string(NFS_EXE_SIZE)+"-byte "+NFS_EXE_ID+" exe: use that exe, or lift yours (config/exe_variants.txt)");
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","SPEED2.EXE %s (%zu bytes) matches the lifted code",NFS_EXE_ID,file.size());
    }
    if (imageSize < 0x532000) imageSize = 0x532000;
    memory->commit(imageBase,imageSize);
    imageRange(0,opt.SizeOfHeaders);
    if (opt.SizeOfHeaders>file.size()) throw GuestStop("Truncated PE headers");
    std::memcpy(ptr(imageBase,opt.SizeOfHeaders),file.data(),opt.SizeOfHeaders);
    uint64_t sections=uint64_t(dos.e_lfanew)+24+nt.FileHeader.SizeOfOptionalHeader;
    for (uint32_t i=0;i<nt.FileHeader.NumberOfSections;++i) {
        auto sec=fileStruct<pe_section_header>(file,sections+i*sizeof(pe_section_header));
        imageRange(sec.VirtualAddress,std::max(sec.VirtualSize,sec.SizeOfRawData));
        if (uint64_t(sec.PointerToRawData)+sec.SizeOfRawData>file.size()) throw GuestStop("Truncated PE section");
        std::memcpy(ptr(imageBase+sec.VirtualAddress,sec.SizeOfRawData),file.data()+sec.PointerToRawData,sec.SizeOfRawData);
    }
    entry=imageBase+opt.AddressOfEntryPoint;
    if (entry == 0x0075bcc7u && !recomp_lookup(0x0075bcc7u) && recomp_lookup(0x0075b9f1u)) {
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Remapping entry point 0x0075bcc7 -> 0x0075b9f1 (WinMainCRTStartup)");
        entry = 0x0075b9f1u;
    }
    // Guest VAs stay fixed; the 64-bit host base is ADDR's offset. No PE
    // relocation toward the host address and no executable x86 mapping.
    auto directory=opt.DataDirectory[PE_DIR_IMPORT];
    bindImports(imageBase,imageSize,directory.VirtualAddress,directory.Size);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Guest PE mapped: base=%08x size=%08x entry=%08x; %zu imports bound; %u lifted functions",
        imageBase,imageSize,entry,imports.size(),recomp_dispatch_count);
}
uint32_t callGuest(uint32_t va, const std::vector<uint32_t>& args, uint32_t cleanup, bool threadStart) {
    State saved; saved.save();
    // Parallel guest threads: guest code runs without the machine; the runtime re-takes it on the
    // way back in (import/override trampolines). Restored to the same depth on return.
    const unsigned held=parallelGuest&&freeRunning?machine.releaseAll():0;
    try {
        recomp_func_t fn=recomp_lookup_manual(va);if(!fn)fn=recomp_lookup(va);
        if (!fn) fn=recomp_lookup_import(va);
        if (!fn) throw GuestStop("No guest function at " + hex(va));
        for (auto it=args.rbegin();it!=args.rend();++it) { g_esp-=4; write32(g_esp,*it); }
        g_esp-=4; write32(g_esp,RECOMP_RETADDR);
        fn(); g_esp+=cleanup;
        if (g_esp!=saved.esp&&!(threadStart&&g_esp+4u*uint32_t(args.size())==saved.esp)) throw GuestStop("Guest call stack imbalance at " + hex(va));
        uint32_t result=g_eax; machine.reacquire(held); saved.load(); return result;
    } catch (...) { machine.reacquire(held); saved.load(); throw; }
}
#include "guest_modules.inc"
void testLiftedCopy() {
    uint32_t source=allocate(128), dest=allocate(128);
    for (unsigned i=0;i<128;++i) static_cast<unsigned char*>(ptr(source,128))[i]=static_cast<unsigned char>(i^0xa5);
    g_esi=0x11111111; g_edi=0x22222222; g_xmm[7].u64[0]=0xfedcba9876543210ull;
    auto stack=g_esp;
    for (uint32_t length : {0u,1u,3u,4u,7u,63u,127u}) {
        std::memset(ptr(dest,128),0xcc,128);
        auto result=callGuest(0x401000,{dest,source,length},12);
        if (result!=length || std::memcmp(ptr(dest,length),ptr(source,length),length) ||
            static_cast<unsigned char*>(ptr(dest,128))[length]!=0xcc || g_esp!=stack ||
            g_esi!=0x11111111 || g_edi!=0x22222222 || g_xmm[7].u64[0]!=0xfedcba9876543210ull)
            throw GuestStop("Lifted copy/callback state validation failed");
    }
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Real lifted sub_00401000 passed 7 copy/cdecl stack cases on a 64-bit host");
}
void timerTestCallback(){timerTestCalls.push_back(arg(0));ret(0,5);}
void testGuestTimers(){
    auto imported=[&](const std::string& name){for(uint32_t i=0;i<imports.size();++i)if(imports[i].name==name)return importBase+i*16;throw GuestStop("Timer test import unavailable");};
    uint32_t set=imported("winmm.dll!timeSetEvent"),kill=imported("winmm.dll!timeKillEvent");
    auto wait=[&](unsigned count){auto until=std::chrono::steady_clock::now()+std::chrono::seconds(2);while(timerTestCalls.size()<count){if(std::chrono::steady_clock::now()>until)throw GuestStop("Timer callback deadline failed");checkGuestProgress();yieldGuestMachine(1);}};
    timerTestActive=true;timerTestCalls.clear();
    try{
        auto first=callGuest(set,{2,0,timerTestAddress,0,0x100});if(!first)throw GuestStop("Timer creation failed");wait(1);
        if(timerTestCalls[0]!=first||timerHandles.count(first))throw GuestStop("One-shot timer retirement failed");
        auto worker=timerWorker->id;
        auto periodic=callGuest(set,{2,0,timerTestAddress,0,0x101});wait(3);
        if(callGuest(kill,{periodic})||timerHandles.count(periodic))throw GuestStop("Periodic timer cancellation failed");
        size_t after=timerTestCalls.size();auto cancelled=callGuest(set,{20,0,timerTestAddress,0,0x100});
        if(callGuest(kill,{cancelled}))throw GuestStop("Pending timer cancellation failed");
        auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(25);while(std::chrono::steady_clock::now()<until){checkGuestProgress();yieldGuestMachine(1);}
        if(timerTestCalls.size()!=after||!timerHandles.empty()||timerWorker->id!=worker||guestThreads.size()!=1)throw GuestStop("Timer cancellation/worker reuse failed");
        timerTestActive=false;
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Timer worker passed: one-shot, periodic, synchronous cancel, pending cancel, retirement and single-thread reuse");
    }catch(...){timerTestActive=false;for(auto& timer:timerHandles)timer.second->cancelled=true;timerHandles.clear();throw;}
}
void testGuestFileTime(){
    auto imported=[&](const std::string& name){for(uint32_t i=0;i<imports.size();++i)if(imports[i].name==name)return importBase+i*16;throw GuestStop("File-time import unavailable");};
    uint32_t local=imported("kernel32.dll!FileTimeToLocalFileTime"),system=imported("kernel32.dll!FileTimeToSystemTime");
    uint32_t data=allocate(48);uint64_t epoch=116444736000000000ull+1234ull*10000ull;
    std::memset(ptr(data,48),0xa5,48);std::memcpy(ptr(data,8),&epoch,8);
    bool okay=callGuest(local,{data,data+8})==1&&read32(data+8)==uint32_t(epoch)&&read32(data+12)==uint32_t(epoch>>32)
        &&callGuest(system,{data+8,data+16})==1;
    const uint16_t expected[]={1970,1,4,1,0,0,1,234};
    okay=okay&&!std::memcmp(ptr(data+16,16),expected,16)&&read32(data+32)==0xa5a5a5a5u;
    okay=okay&&callGuest(local,{data,data})==0;
    uint64_t invalid=0x8000000000000000ull;std::memcpy(ptr(data,8),&invalid,8);
    okay=okay&&callGuest(system,{data,data+16})==0;
    heaps->free(GuestHeaps::process,data);
    if(!okay)throw GuestStop("Guest file-time conversion self-check failed");
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Guest UTC FILETIME conversions passed: epoch, milliseconds, guards and invalid input");
}
void testGuestCriticalSections(){
    auto imported=[&](const std::string& name){for(uint32_t i=0;i<imports.size();++i)if(imports[i].name=="kernel32.dll!"+name)return importBase+i*16;throw GuestStop("Critical-section import unavailable");};
    uint32_t initialize=imported("InitializeCriticalSection"),enter=imported("EnterCriticalSection"),leave=imported("LeaveCriticalSection"),remove=imported("DeleteCriticalSection");
    uint32_t data=allocate(32);std::memset(ptr(data,32),0xa5,32);
    callGuest(initialize,{data});callGuest(enter,{data});callGuest(enter,{data});
    bool okay=read32(data+8)==2&&read32(data+12)==guestThreadId&&read32(data+24)==0xa5a5a5a5u;
    callGuest(remove,{data});okay=okay&&criticalSections.count(data)&&pendingCriticalDeletes.count(data);
    callGuest(leave,{data});okay=okay&&read32(data+8)==1&&criticalSections.count(data);
    callGuest(leave,{data});okay=okay&&!criticalSections.count(data)&&!pendingCriticalDeletes.count(data)&&read32(data+8)==0;
    callGuest(initialize,{data});callGuest(remove,{data});okay=okay&&!criticalSections.count(data);
    heaps->free(GuestHeaps::process,data);

    // Deadlock cycle resolution test: Thread 1 holds csA and requests csB; Worker 9001 holds csB and requests csA.
    uint32_t csA=allocate(32),csB=allocate(32);
    callGuest(initialize,{csA});callGuest(initialize,{csB});
    callGuest(enter,{csA});
    std::atomic<bool> workerHoldsB{false},workerDone{false};
    std::thread deadlockWorker([&]{
        std::unique_lock<MachineMutex> lease(machine);machineLease=&lease;guestThreadId=9001;
        uint32_t workerStack=allocate(64*1024);
        State fresh;fresh.esp=workerStack+64*1024-64;fresh.load();
        callGuest(enter,{csB});
        workerHoldsB=true;
        notifyGuestSync();
        callGuest(enter,{csA});
        callGuest(leave,{csA});
        callGuest(leave,{csB});
        heaps->free(GuestHeaps::process,workerStack);
        forgetThreadTrace();
        workerDone=true;
        notifyGuestSync();
        machineLease=nullptr;
    });
    while(!workerHoldsB.load()){checkGuestProgress();waitGuestSync(guestSyncGeneration.load(),5);}
    while(true){
        {
            std::lock_guard<std::mutex> lock(threadTracesMutex);
            auto it=threadTraces.find(9001);
            if(it!=threadTraces.end()&&it->second){
                const char* w=it->second->wait.load();
                if(w&&!std::strcmp(w,"EnterCriticalSection")&&it->second->handle.load()==csA)break;
            }
        }
        checkGuestProgress();waitGuestSync(guestSyncGeneration.load(),5);
    }
    callGuest(enter,{csB});
    bool deadlockPassed=read32(csA+12)==guestThreadId&&read32(csB+12)==guestThreadId;
    callGuest(leave,{csB});
    callGuest(leave,{csA});
    machineLease->unlock();
    deadlockWorker.join();
    machineLease->lock();
    callGuest(remove,{csA});callGuest(remove,{csB});
    heaps->free(GuestHeaps::process,csA);heaps->free(GuestHeaps::process,csB);
    okay=okay&&deadlockPassed&&workerDone.load();

    if(!okay)throw GuestStop("Guest critical-section retirement self-check failed");
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Guest critical sections passed recursion, owner, guards, deferred retirement, reinitialization and deadlock resolution");
}
thread_local uint32_t dynamicThunkAddress{};
void generatedTestCallback(){++generatedTestCalls;ret(read32(arg(0)),0);}
void dynamicThunk(){
    uint32_t start=dynamicThunkAddress,pc=start;
    // (parallel guest threads: the lookup already checked the address is committed, see recomp_lookup_manual)
    if((!parallelGuest&&!memory->readable(start,5))||read32(start)!=0x24748b56u||*static_cast<uint8_t*>(ptr(start+4,1))!=8)
        throw GuestStop("Invalid generated callback at "+hex(start));
    uint32_t originalEsi=g_esi,esi=0,eax=g_eax,ecx=g_ecx,originalEsp=g_esp;
    unsigned saved=0,arguments=0,totalArguments=0;bool pending=false,stackCleared=false,popped=false,compared=false;int comparison=0;
    // Direct calls (NFSU2_THUNK_FAST, default on): the sound mixer runs these chains hundreds of times a
    // frame, and callGuest per target (a std::vector of arguments, a full CPU-state save/restore, three
    // lookups and a machine release/re-acquire) cost more than the targets. A generated chain is plain
    // cdecl x86 (push esi; call; add esp), so it calls the lifted target the way the x86 call would: esi
    // pushed, return address, callee pops nothing. The machine is released once for the whole chain (callGuest
    // released it around every call; the interpreter between calls only touches guest memory). Targets with
    // a runtime override (or the self-tests) still go through callGuest.
    static const bool fastCalls=[]{const char* e=std::getenv("NFSU2_THUNK_FAST");return !e||std::atoi(e)!=0;}();
    const unsigned held=fastCalls&&parallelGuest&&freeRunning?machine.releaseAll():0;
    struct Reacquire{unsigned depth;~Reacquire(){machine.reacquire(depth);}} reacquireOnExit{held};
    struct TargetSlot{uint32_t va;recomp_func_t fn;};
    static thread_local TargetSlot targets[256];
    auto callTarget=[&](uint32_t target)->uint32_t{
        if(generatedTestActive&&target==generatedTestTarget)return callGuest(target,{esi},4);
        // The lookup also validates the target (the chain may only call lifted code), once per target.
        TargetSlot& slot=targets[(target>>2)&255];
        if(slot.va!=target){
            const recomp_func_t lifted=recomp_lookup(target);
            if(!lifted)throw GuestStop("Generated callback calls unlifted "+hex(target));
            slot.va=target;slot.fn=nfs_override_lookup(target)?nullptr:lifted;
        }
        if(!fastCalls||generatedTestActive||timerTestActive||!slot.fn)return callGuest(target,{esi},4);
        const uint32_t sp=g_esp,cur=g_cur_func;
        g_eax=eax;g_ecx=ecx;g_esi=esi;
        g_esp=sp-4;write32(g_esp,esi);g_esp-=4;write32(g_esp,RECOMP_RETADDR);
        slot.fn();g_esp+=4;g_cur_func=cur;
        if(g_esp!=sp)throw GuestStop("Guest call stack imbalance at "+hex(target));
        g_esi=originalEsi;
        return g_eax;
    };
    auto byte=[&](){return *static_cast<const uint8_t*>(ptr(pc++,1));};
    auto word=[&](){uint32_t value=read32(pc);pc+=4;return value;};
    for(unsigned steps=0;steps<4096&&pc-start<65536;++steps){
        uint32_t instruction=pc;uint8_t op=byte();
        if(op==0x56){if(!saved){saved=1;}else{++arguments;++totalArguments;pending=true;stackCleared=false;}continue;}
        if(op==0xe8&&pending){int32_t delta=static_cast<int32_t>(word());uint32_t target=pc+delta;
            eax=callTarget(target);pending=false;continue;}
        if(op==0x83){uint8_t form=byte(),delta=byte();
            if(form==0xc6){esi+=int8_t(delta);compared=false;continue;}
            if(form==0xc4&&uint32_t(int32_t(int8_t(delta)))==arguments*4&&!pending){stackCleared=true;arguments=0;compared=false;continue;}
        }
        if(op==0x81){uint8_t form=byte();uint32_t delta=word();
            if(form==0xc6){esi+=delta;compared=false;continue;}
            if(form==0xc4&&delta==arguments*4&&!pending){stackCleared=true;arguments=0;compared=false;continue;}
        }
        if(op==0x89){uint8_t form=byte();uint32_t offset;
            if(form==0x86)offset=word();else if(form==0x46)offset=byte();else throw GuestStop("Generated callback store form pending");
            write32(esi+offset,eax);continue;}
        if(op==0x8b){uint8_t form=byte();
            if(form==0x74||form==0xb4){uint8_t sib=byte();uint32_t offset=form==0x74?uint32_t(int32_t(int8_t(byte()))):word();
                if(sib==0x24&&saved&&offset==4*(saved+arguments)+4){esi=read32(originalEsp+4);continue;}}
            if(form==0x46){eax=read32(esi+byte());continue;}
            if(form==0x06){eax=read32(esi);continue;}
            if(form==0x4e){ecx=read32(esi+uint32_t(int32_t(int8_t(byte()))));continue;}
            if(form==0x8e){ecx=read32(esi+word());continue;}
            if(form==0x0e){ecx=read32(esi);continue;}
            if(form==0xc1){eax=ecx;continue;}
        }
        if(op==0x3b&&byte()==0xc1){comparison=int32_t(eax)<int32_t(ecx)?-1:int32_t(eax)>int32_t(ecx)?1:0;compared=true;continue;}
        if(op==0x0f&&byte()==0xaf){uint8_t form=byte();
            if(form==0x46){eax*=read32(esi+uint32_t(int32_t(int8_t(byte()))));compared=false;continue;}
            if(form==0x86){eax*=read32(esi+word());compared=false;continue;}
            if(form==0x06){eax*=read32(esi);compared=false;continue;}
        }
        if(op>=0x7c&&op<=0x7f&&compared){int8_t delta=int8_t(byte());bool taken=op==0x7c?comparison<0:op==0x7d?comparison>=0:op==0x7e?comparison<=0:comparison>0;
            if(taken){uint32_t target=pc+int32_t(delta);if(target<start||target-start>=65536)throw GuestStop("Generated callback branch outside budget");pc=target;}continue;}
        if(op==0xc6&&byte()==0x46&&byte()==0&&byte()==0){*static_cast<uint8_t*>(ptr(esi,1))=0;continue;}
        if(op==0x03||op==0x2b){uint8_t form=byte();uint32_t offset;
            if(form==0x46)offset=uint32_t(int32_t(int8_t(byte())));else if(form==0x86)offset=word();else if(form==0x06)offset=0;else throw GuestStop("Generated callback arithmetic form pending");
            if(op==0x03)eax+=read32(esi+offset);else eax-=read32(esi+offset);compared=false;continue;}
        if(op==0x5e&&stackCleared&&!pending){popped=true;continue;}
        if(op==0xc3&&popped&&saved&&totalArguments>0&&!arguments){g_eax=eax;g_ecx=ecx;g_esi=originalEsi;g_esp=originalEsp+4;
            static unsigned completed=0;if(++completed<=3)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Generated x86 callback translated: %08x, %u operations",start,steps+1);
            return;}
        uint32_t fault=instruction;
        if(memory->readable(fault,32)){
            const auto* raw=static_cast<const uint8_t*>(ptr(fault,32));std::ostringstream dump;
            for(unsigned i=0;i<32;++i)dump<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(raw[i]);
            NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU2","Generated callback start=%08x offset=%03x bytes=%s",start,fault-start,dump.str().c_str());
        }
        throw GuestStop("Generated callback opcode pending at "+hex(fault));
    }
    NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU2","Generated callback budget reached start=%08x pc=%08x arguments=%u",start,pc,arguments);
    throw GuestStop("Generated callback did not return at "+hex(start));
}
void testGeneratedCallbacks(){
    uint32_t code=allocate(2048),data=allocate(840);std::vector<uint8_t> bytes;
    auto emit=[&](std::initializer_list<uint8_t> values){bytes.insert(bytes.end(),values.begin(),values.end());};
    auto word=[&](uint32_t value){for(unsigned i=0;i<4;++i)bytes.push_back(uint8_t(value>>(i*8)));};
    emit({0x56,0x8b,0x74,0x24,0x08});
    for(unsigned i=0;i<100;++i){emit({0x56,0xe8});word(generatedTestTarget-(code+uint32_t(bytes.size())+4));emit({0x89,0x46,0x04,0x83,0xc6,0x08});}
    emit({0x8b,0xb4,0x24});word(408);emit({0x81,0xc6});word(804);
    emit({0x8b,0x06,0x8b,0x4e,0x04,0x3b,0xc1,0x7f,0x02,0x8b,0xc1,0x0f,0xaf,0x46,0x04,0x89,0x46,0x14,
        0x03,0x46,0x04,0x89,0x46,0x18,0x2b,0x46,0x04,0x89,0x46,0x1c,0x81,0xc4});word(400);emit({0x5e,0xc3});
    std::memcpy(ptr(code,bytes.size()),bytes.data(),bytes.size());std::memset(ptr(data,840),0xa5,840);
    generatedTestActive=true;generatedTestCalls=0;
    try{
        for(auto values:{std::array<int32_t,2>{-4,5},std::array<int32_t,2>{-2,-5},std::array<int32_t,2>{INT32_MAX,2}}){
            write32(data+804,uint32_t(values[0]));write32(data+808,uint32_t(values[1]));
            uint32_t expected=uint32_t(std::max(values[0],values[1]))*uint32_t(values[1]);
            if(callGuest(code,{data},4)!=expected||read32(data+824)!=expected||read32(data+828)!=expected+uint32_t(values[1])||read32(data+832)!=expected||read32(data+836)!=0xa5a5a5a5u)
                throw GuestStop("Generated callback long routine/signed clamp failed");
        }
        if(generatedTestCalls!=300)throw GuestStop("Generated callback call count failed");
    }catch(...){generatedTestActive=false;heaps->free(GuestHeaps::process,code);heaps->free(GuestHeaps::process,data);throw;}
    generatedTestActive=false;heaps->free(GuestHeaps::process,code);heaps->free(GuestHeaps::process,data);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Generated callbacks passed 300 calls, long context reload, signed clamps, 32-bit multiply/add/subtract, stack cleanup and guards");
}
} // namespace
// QueryPerformanceCounter for native code (src/runtime/native_math.c: sub_0043BE50), with the busy-wait
// detection of the import.
extern "C" uint64_t nfsQpcNow(){return qpcRead();}

#ifdef NFS_SERVER_DLL
extern "C" const recomp_dispatch_entry_t server_recomp_dispatch_table[];
extern "C" const uint32_t server_recomp_dispatch_count;
#endif
namespace {
// Every indirect call and virtual method resolves its target here, so use an
// open-addressing table (built once, read-only afterwards) instead of a binary
// search over ~28k entries with a cache miss per step.
struct DispatchHash {
    static constexpr uint32_t slots=1u<<16;
    std::unique_ptr<uint32_t[]> keys{new uint32_t[slots]()};
    std::unique_ptr<recomp_func_t[]> funcs{new recomp_func_t[slots]()};
    static uint32_t slot(uint32_t va){return (va*0x9e3779b1u)>>16;}
    void add(const recomp_dispatch_entry_t* table,uint32_t count){
        for(uint32_t i=0;i<count;++i){
            uint32_t va=table[i].address,s=slot(va);
            if(va-0x7c000000u<0x04000000u)throw GuestStop("Lifted function overlaps reserved native dispatch stubs");
            while(keys[s])s=(s+1)&(slots-1);
            keys[s]=va;funcs[s]=table[i].func;
        }
    }
    DispatchHash(){
        static_assert(slots>2*(27742+1531),"dispatch hash load factor");
        add(recomp_dispatch_table,recomp_dispatch_count);
#ifdef NFS_SERVER_DLL
        add(server_recomp_dispatch_table,server_recomp_dispatch_count);
#endif
        uint32_t used=0;for(uint32_t i=0;i<slots;++i)used+=keys[i]!=0;
        if(used>slots/2)throw GuestStop("Dispatch hash over half full: "+std::to_string(used));
    }
    recomp_func_t find(uint32_t va)const{
        if(!va)return nullptr;
        for(uint32_t s=slot(va);keys[s];s=(s+1)&(slots-1))if(keys[s]==va)return funcs[s];
        return nullptr;
    }
};
}
extern "C" recomp_func_t recomp_lookup(uint32_t va) {
    // Sound/input/D3D COM and import stubs resolve in recomp_lookup_import.
    // Avoid an always-failing hash probe on every native method call. The
    // table constructor checks that no lifted module occupies this range.
    if(va-0x7c000000u<0x04000000u)return nullptr;
    static const DispatchHash hash;
    return hash.find(va);
}
// Parallel guest threads: runtime functions reached from lifted code run under the machine. The lookup
// takes it and returns this trampoline, which runs the function and releases it (the lookup's side
// effects -- method tokens, dynamic thunk address -- stay protected until the function has used them).
thread_local recomp_func_t pendingRuntimeCall=nullptr;
void lockedRuntimeTrampoline(){
    recomp_func_t fn=pendingRuntimeCall;
    try{fn();}catch(...){machine.unlock();throw;}
    machine.unlock();
}
recomp_func_t lookupManualUnlocked(uint32_t va){
    if(timerTestActive&&va==timerTestAddress)return timerTestCallback;
    if(generatedTestActive&&va==generatedTestTarget)return generatedTestCallback;
    if(va==0x0075bcc7u&&recomp_lookup(0x0075b9f1u))return recomp_lookup(0x0075b9f1u);
    if(va>=GuestHeaps::firstArena&&va<GuestHeaps::firstArenaEnd&&memory&&memory->readable(va,5)&&read32(va)==0x24748b56u&&*static_cast<const uint8_t*>(ptr(va+4,1))==8){
        dynamicThunkAddress=va;return dynamicThunk;
    }
    return nfs_override_lookup(va);
}
extern "C" recomp_func_t recomp_lookup_manual(uint32_t va) {
    if(va==0x0075bcc7u&&recomp_lookup(0x0075b9f1u))return recomp_lookup(0x0075b9f1u);
    // Threads that take turns hold the machine already (taking it again would leave the handler's own
    // yields unable to release it): only free-running threads go through the trampoline.
    if(!parallelGuest||!freeRunning)return lookupManualUnlocked(va);
    // Cheap rejection first: most indirect calls are lifted functions or imports.
    const bool arena=va>=GuestHeaps::firstArena&&va<GuestHeaps::firstArenaEnd;
    if(arena&&!timerTestActive&&!generatedTestActive){
        // Generated callbacks in the guest heap: dynamicThunk interprets them and needs no machine (it
        // reads guest memory and calls lifted code). Only "is this page committed" reads the page map,
        // so remember per thread the 64 KB blocks already checked (heap arenas stay committed).
        static thread_local uint32_t checkedBlocks[64]={};
        const uint32_t block=(va>>16)|0x80000000u;uint32_t& slot=checkedBlocks[(va>>16)&63];
        if(slot!=block){
            machine.lock();bool ok;
            try{ok=memory&&memory->readable(va,5);}catch(...){machine.unlock();throw;}
            machine.unlock();
            if(!ok)return nullptr;
            slot=block;
        }
        if(read32(va)==0x24748b56u&&*static_cast<const uint8_t*>(ptr(va+4,1))==8){dynamicThunkAddress=va;return dynamicThunk;}
        return nullptr;
    }
    if(!arena&&!timerTestActive&&!generatedTestActive&&!nfs_override_lookup(va))return nullptr;
    machine.lock();
    recomp_func_t fn;
    try{fn=lookupManualUnlocked(va);}catch(...){machine.unlock();throw;}
    if(!fn){machine.unlock();return nullptr;}
    pendingRuntimeCall=fn;return lockedRuntimeTrampoline;
}
recomp_func_t lookupImportLocked(uint32_t va);
std::unordered_map<uint32_t,uint64_t> freeLockedCalls;
std::mutex freeLockedMutex;
std::string importName(uint32_t va);
extern "C" void nfsReportFreeLockedCalls(unsigned frames){
    std::vector<std::pair<uint64_t,uint32_t>> v;
    {std::lock_guard<std::mutex> l(freeLockedMutex);for(auto& e:freeLockedCalls)v.push_back({e.second,e.first});freeLockedCalls.clear();}
    std::sort(v.rbegin(),v.rend());
    std::string line="[NFSU2 WAIT] sound thread runtime calls needing the machine (per frame):";
    char buf[96];for(size_t i=0;i<v.size()&&i<10;++i){std::snprintf(buf,sizeof buf," %s=%.1f",importName(v[i].second).c_str(),double(v[i].first)/frames);line+=buf;}
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","%s",line.c_str());
}
std::string importName(uint32_t va){
    if(va>=importBase&&!((va-importBase)%16)){uint32_t i=(va-importBase)/16;if(i<imports.size())return imports[i].name;}
    char b[32];std::snprintf(b,sizeof b,"%08x",va);return b;
}
extern "C" recomp_func_t recomp_lookup_import(uint32_t va) {
    if(!parallelGuest||!freeRunning)return lookupImportLocked(va);
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
    // D3D9: queued calls need no machine; d3dDispatch takes it only for synchronous calls.
    if(va>=0x7e001000u&&va<0x7e010000u&&!(va%16)){d3dToken=va;return d3dDispatch;}
#endif
    // DirectSound buffer methods (GetStatus, GetCurrentPosition, Lock, Unlock, ...): soundStateMutex, no machine.
    // QueryInterface/AddRef/Release and the DirectSound object itself allocate guest memory: machine.
    if(va>=soundMethodBase+0x2000&&va<soundMethodBase+0x3000&&!(va%16)&&((va-soundMethodBase-0x2000)/16)>2){soundToken=va;return soundDispatch;}
    if(va>=importBase&&!((va-importBase)%16)){
        uint32_t index=(va-importBase)/16;
        if(index<imports.size()&&imports[index].freeFunction){lastImport=va;return imports[index].freeFunction;}
        if(index<imports.size()&&imports[index].lockFree){lastImport=va;++imports[index].calls;return imports[index].function;}
    }
    machine.lock();
    {std::lock_guard<std::mutex> l(freeLockedMutex);++freeLockedCalls[va];}  // diagnostics: which runtime calls make free threads take the machine
    recomp_func_t fn;
    try{fn=lookupImportLocked(va);}catch(...){machine.unlock();throw;}
    if(!fn){machine.unlock();return nullptr;}
    pendingRuntimeCall=fn;return lockedRuntimeTrampoline;
}
recomp_func_t lookupImportLocked(uint32_t va) {
    checkGuestProgress();
    if(va>=soundMethodBase+0x1000&&va<soundMethodBase+0x3000&&!(va%16)){soundToken=va;return soundDispatch;}
    if(va>=inputMethodBase+0x1000&&va<inputMethodBase+0x5000&&!(va%16)){inputToken=va;return inputDispatch;}  // kinds 1-4: DirectInput, keyboard, mouse, gamepad
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
    if(va>=0x7e001000u&&va<0x7e010000u&&!(va%16)){d3dToken=va;return d3dDispatch;}
#endif
    if (va<importBase || (va-importBase)%16) return nullptr;
    uint32_t index=(va-importBase)/16;
    if (index>=imports.size()) return nullptr;
    lastImport=va;
    if(++imports[index].calls<=3)
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Guest import %s from %08x",imports[index].name.c_str(),g_cur_func);
    return imports[index].function;
}
extern "C" void recomp_unresolved(const char* kind,uint32_t va,uint32_t from) {
    if(from==NFS_VA(0x00725270)&&memory){   // the audio callback dispatcher
        uint32_t node=read32(0x8b446c);
        for(unsigned i=0;i<12&&node&&memory->readable(node,16);++i){
            NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU2","Audio callback node=%08x next=%08x prev=%08x function=%08x data=%08x",node,read32(node),read32(node+4),read32(node+8),read32(node+12));
            node=read32(node);
        }
        if(memory->readable(va,24)){
            auto bytes=static_cast<const uint8_t*>(ptr(va,24));
            NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU2","Unlifted callback bytes %08x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",va,
                bytes[0],bytes[1],bytes[2],bytes[3],bytes[4],bytes[5],bytes[6],bytes[7],bytes[8],bytes[9],bytes[10],bytes[11],bytes[12],bytes[13],bytes[14],bytes[15]);
            if(memory->readable(va,512))for(unsigned part=0;part<8;++part){
                std::ostringstream dump;for(unsigned i=part*64;i<(part+1)*64;++i)dump<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(bytes[i]);
                NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU2","Unlifted callback %03x: %s",part*64,dump.str().c_str());
            }
        }
    }
    throw GuestStop(std::string(kind)+" unresolved "+hex(va)+" from "+hex(from));
}
extern "C" void recomp_unimpl(uint32_t va,const char* what) {
    throw GuestStop("Instruction unsupported at "+hex(va)+": "+what);
}
extern "C" void recomp_dump_trace(const char*) {}
std::array<AndroidRenderMode,6> guestRenderModes{};
bool firstGameResolution=true;
void configureAndroidResolutionModes(){
    guestRenderModes=androidRenderModes(displayWidth.load(),displayHeight.load(),renderWidth.load(),renderHeight.load());
    const uint32_t labels[]={0x78fa1c,0x78fa14,0x78fa08,0x78f9fc,0x78f9f0,0x78f9e4};
    const char* original[]={"640x480","800x600","1024x768","1280x960","1280x1024","1600x1200"};
    for(unsigned i=0;i<6;++i){
        uint32_t capacity=i<2?8:12;
        if(std::strcmp(static_cast<const char*>(ptr(labels[i],capacity)),original[i]))throw GuestStop("Unexpected original resolution label");
        std::memset(ptr(labels[i],capacity),0,capacity);
        if(i<4)std::snprintf(static_cast<char*>(ptr(labels[i],capacity)),capacity,"%u%%",(i+1)*25);
        else std::snprintf(static_cast<char*>(ptr(labels[i],capacity)),capacity,"%ux%u",guestRenderModes[i].width,guestRenderModes[i].height);
        write32(0x800538+i*4,guestRenderModes[i].width);write32(0x800550+i*4,guestRenderModes[i].height);
    }
    write32(0x870d1c,5);
}
#if defined(NFS_R36S)
// GetRacingResolution(int* w, int* h) is sub_005BF2E0 in v10 (renamed per exe by nfs_exe_addr.h; stdcall, ret 8). It returns
// fixed sizes per menu slot (640x480 for slot 0); the R36S build renames the lifted function *_orig
// (build_guest_vm.sh wrap list) and answers with the size main.cpp chose for this screen.
extern "C" void sub_005BF2E0_orig(void);
extern "C" void sub_005BF2E0(void){
    const uint32_t width=renderWidth.load(),height=renderHeight.load();
    if(!width||!height||!memory->writable(arg(0),4)||!memory->writable(arg(1),4)){sub_005BF2E0_orig();return;}
    const uint32_t w=arg(0),h=arg(1);
    write32(w,width);write32(h,height);
    g_eax=w;g_ecx=h;g_esp+=12;   // as the original's slot-0 path: eax/ecx = the pointers, ret 8
    static int logged=0;if(logged++<2)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","GetRacingResolution -> %ux%u",width,height);
}
#endif
extern "C" int nfs_android_resolution(){
#if defined(NFS_R36S)
    return 0;
#else
    uint32_t width=renderWidth.load(),height=renderHeight.load();if(!width||!height)return 0;
    if(firstGameResolution){write32(0x870d1c,5);firstGameResolution=false;}
    uint32_t index=read32(0x870d1c);if(index>=guestRenderModes.size())throw GuestStop("Invalid Android resolution index");
    width=guestRenderModes[index].width;height=guestRenderModes[index].height;
    if(!memory->writable(arg(0),4)||!memory->writable(arg(1),4)){
        NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU2","nfs_android_resolution: non-writable outputs (arg0=0x%08x, arg1=0x%08x)",arg(0),arg(1));
        return 0;
    }
    const uint32_t w=arg(0),h=arg(1);
    write32(w,width);write32(h,height);ret(w,2);g_ecx=h;nfs_widescreen_set_resolution(width,height);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Guest render resolution %ux%u slot=%u via original selector",width,height,index);
    return 1;
#endif
}

std::string connectGuestRuntime(const char* executable) {
    std::unique_lock<MachineMutex> guard(machine);machineLease=&guard;
    if (memory) return "Runtime guest ya inicializado";
    State original; original.save();
    try {
        stopRequested=false;
        // Interactive sessions (Android, R36S, Desktop) stop through player exit or signal cancellation.
        // Optional timeout can be enforced if NFSU2_BOOT_TIMEOUT is explicitly set in seconds (e.g. for CI/benchmarking).
        const char* timeoutEnv = std::getenv("NFSU2_BOOT_TIMEOUT");
        if (timeoutEnv && *timeoutEnv) {
            int seconds = std::atoi(timeoutEnv);
            bootDeadline = (seconds > 0) ? (std::chrono::steady_clock::now() + std::chrono::seconds(seconds))
                                         : std::chrono::steady_clock::time_point::max();
        } else {
            bootDeadline = std::chrono::steady_clock::time_point::max();
        }
        recomp_yield_hook=checkGuestProgress;
        memory=std::make_unique<GuestMemory>();
        hostGameRoot=std::string(executable); hostGameRoot=hostGameRoot.substr(0,hostGameRoot.find_last_of('/'));
        g_mem_base=reinterpret_cast<ptrdiff_t>(memory->base());
        testGuestHeaps(*memory);
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Guest heaps passed ownership, realloc preservation, zero tail, in-place failure and reuse checks");
        heaps=std::make_unique<GuestHeaps>(*memory);
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
        configureD3D9Bridge(memory->base(),allocate,freeD3DGuest);
#endif
        loadImage(executable);
        if(guestWidescreen){
            nfs_widescreen_init(hostGameRoot.c_str());
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Widescreen: %d HUD offsets loaded",nfs_widescreen_hud_entries());
        }else NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Widescreen disabled: original 4:3 geometry");
#if defined(NFS_R36S)
        // Slot 0 of the game's resolution table (originally 640x480) carries the size the host chose for
        // this screen (main.cpp chooseResolution: the panel's native size by default), and is the one used.
        write32(0x870d1c, 0);
        if(renderWidth.load()&&renderHeight.load()){
            const unsigned w=renderWidth.load(),h=renderHeight.load();
            write32(0x800538,w);write32(0x800550,h);
            char* label=static_cast<char*>(ptr(0x78fa1c,8));
            if(!std::strcmp(label,"640x480")){
                char text[16];std::snprintf(text,sizeof text,"%ux%u",w,h);
                std::memset(label,0,8);std::snprintf(label,8,"%s",std::strlen(text)<8?text:"NATIVE");
            }
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Game resolution slot 0: %ux%u",w,h);
        }
#else
        if(renderWidth.load()&&renderHeight.load())configureAndroidResolutionModes();
#endif
        // NFSU2_DEBUG_CAMERA=1: the game's debug camera (DebugCamerasEnabled), read while loading.
        if(const char* e=std::getenv("NFSU2_DEBUG_CAMERA");e&&*e=='1'){*static_cast<uint8_t*>(ptr(0x865098,1))=1;NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Debug camera enabled");}
        memory->commit(stackBase,stackSize); memory->commit(tib,0x1000);
        State fresh; fresh.esp=stackBase+stackSize-64; fresh.fs=tib; fresh.load();
        write32(tib,0xffffffff); write32(tib+4,stackBase+stackSize); write32(tib+8,stackBase); write32(tib+0x18,tib);
        testLiftedCopy();
        testGuestScheduling();
        testGuestInput();
        testGuestSoundCaps();
        testGuestTimers();
        testGuestFileTime();
        testGuestCriticalSections();
        testGeneratedCallbacks();
#if !defined(NFS_R36S)
        if(renderWidth.load()&&renderHeight.load()){
            uint32_t outputs=allocate(12);write32(outputs+8,0xdeadbeef);
            callGuest(NFS_VA(0x005BF2E0),{outputs,outputs+4});
            if(read32(outputs)!=renderWidth.load()||read32(outputs+4)!=renderHeight.load()||read32(outputs+8)!=0xdeadbeef)throw GuestStop("Guest resolution ABI failed");
            for(uint32_t i=0;i<6;++i){write32(0x870d1c,i);callGuest(NFS_VA(0x005BF2E0),{outputs,outputs+4});
                if(read32(outputs)!=guestRenderModes[i].width||read32(outputs+4)!=guestRenderModes[i].height||read32(outputs+8)!=0xdeadbeef)throw GuestStop("Guest resolution slot/guard check failed");}
            write32(0x870d1c,5);firstGameResolution=true;
            heaps->free(GuestHeaps::process,outputs);
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Guest resolution outputs, guard and stdcall stack passed");
        }
#endif
        // Restore a clean CPU state before executing the real CRT entry.
        fresh.load();
        NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Entering actual lifted CRT entry %08x",entry);
#if defined(NFS_PARALLEL_GUEST)
        {
            const char* e=std::getenv("NFSU2_PARALLEL_GUEST");
            if(!(e&&e[0]=='0')){
                // Imports that touch only the calling thread's state or a clock run without the machine.
                static const char* lockFree[]={"!GetLastError","!SetLastError","!TlsGetValue","!TlsSetValue","!GetCurrentThreadId",
                    "!GetTickCount","!timeGetTime","!QueryPerformanceCounter","!QueryPerformanceFrequency"};
                unsigned marked=0;
                for(auto& import:imports)for(const char* suffix:lockFree){
                    const size_t n=std::strlen(suffix);
                    if(import.name.size()>=n&&import.name.compare(import.name.size()-n,n,suffix)==0){import.lockFree=true;++marked;}
                }
                // Mutexes and Sleep: their own locking / no shared state (guest_threads.inc). Free threads only.
                for(auto& import:imports){
                    auto ends=[&](const char* suffix){const size_t n=std::strlen(suffix);return import.name.size()>=n&&import.name.compare(import.name.size()-n,n,suffix)==0;};
                    if(ends("!WaitForSingleObject"))import.freeFunction=waitSingleFree;
                    else if(ends("!ReleaseMutex"))import.freeFunction=releaseMutex;
                    else if(ends("!Sleep"))import.freeFunction=sleepShim;
                    else if(ends("!EnterCriticalSection"))import.freeFunction=enterCritical;
                    else if(ends("!LeaveCriticalSection"))import.freeFunction=leaveCritical;
                    if(import.freeFunction)++marked;
                }
                const char* list=std::getenv("NFSU2_PARALLEL_THREADS");std::string entries=list?list:NFS_SOUND_THREAD_ENTRY;
                for(size_t at=0;at<entries.size();){size_t end=entries.find_first_of(",; ",at);if(end==std::string::npos)end=entries.size();
                    if(end>at)freeThreadEntries.push_back(static_cast<uint32_t>(std::strtoul(entries.substr(at,end-at).c_str(),nullptr,16)));at=end+1;}
                parallelGuest=true;
                NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Parallel guest threads enabled (%u lock-free imports; NFSU2_PARALLEL_GUEST=0 disables)",marked);
            }
        }
#endif
        uint32_t result=callGuest(entry,{});
        joinGuestThreads(guard);machineLease=nullptr;shutdownGuestSound();
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
        shutdownD3D9Bridge();
#endif
        original.load();
        return "Entrada guest terminada: "+std::to_string(result);
    } catch (const std::exception& e) {
        NFS_RUNTIME_LOG(ANDROID_LOG_WARN,"NFSU2","Guest integration boundary: %s",e.what());
        joinGuestThreads(guard);machineLease=nullptr;shutdownGuestSound();
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
        shutdownD3D9Bridge();
#endif
        original.load();
        return std::string("Arranque guest detenido: ")+e.what();
    }
}
void requestGuestRuntimeStop() {stopRequested=true;}
// Quitting (Select+Start, the window closing): wait until the game has no file open for writing (a save or
// profile being written), at most timeoutMs, then fsync whatever is open and sync the whole card. The guest
// is held at a runtime call while the handles are inspected (machine lock), so no write is half done; ending
// the process in the middle of a save, without a flush, is what left saves and card filesystems damaged.
void quiesceGuestFilesForExit(int timeoutMs){
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(timeoutMs);
    int writable=-1;
    for(;;){
        {
            std::unique_lock<MachineMutex> lock(machine);
            writable=0;
            for(auto& entry:fileHandles){
                const int fl=::fcntl(entry.second,F_GETFL);
                if(fl>=0&&(fl&O_ACCMODE)!=O_RDONLY)++writable;
            }
            if(!writable||std::chrono::steady_clock::now()>=deadline){
                for(auto& entry:fileHandles)::fsync(entry.second);
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ::sync();
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Exit: files flushed (%d still open for writing), card synced",writable);
}
void setGuestPaused(bool paused) {guestPaused=paused;}
bool guestDrivingControls() {return inputDrivingMode.load();}
extern "C" int nfsGuestDriving(){return inputDrivingMode.load()?1:0;}
void setGuestPad(const float axes[6],uint32_t buttons,int hatX,int hatY){
    std::lock_guard<std::mutex> lock(inputMutex);
    for(unsigned i=0;i<6;++i)inputPad.axis[i]=std::max(-1.f,std::min(1.f,axes[i]));
    inputPad.buttons=buttons;inputPad.hatX=hatX;inputPad.hatY=hatY;
}
void setGuestPadConnected(bool connected){
    std::lock_guard<std::mutex> lock(inputMutex);
    if(connected!=inputPadConnected)NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Gamepad %s",connected?"connected":"disconnected");
    inputPadConnected=connected;if(!connected)inputPad=InputPad{};
}
bool guestGamepadInUse(){return inputPadAcquired.load()>0;}
void setGuestDisplaySize(unsigned width,unsigned height){displayWidth=width;displayHeight=height;}
void setGuestResolution(unsigned width,unsigned height){
    std::lock_guard<MachineMutex> lock(machine);if(memory)return;
    if(width<320||height<240||width>8192||height>8192){renderWidth=renderHeight=0;return;}
    renderWidth=width;renderHeight=height;
#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
    setD3D9RenderSize(width,height);
#endif
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Selected render resolution %ux%u",width,height);
}
void setGuestFrameLimit(unsigned framesPerSecond){
    frameLimit=(framesPerSecond>=30&&framesPerSecond<=240)?framesPerSecond:0;
    // The game paces itself to 1/60 s; let it run at the cap (uncapped: the 120 Hz display).
    nfs_widescreen_set_frame_rate(frameLimit.load()?frameLimit.load():120);
    NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Android frame limit %u FPS (0 means uncapped)",frameLimit.load());
}
void setGuestKey(unsigned scan,bool down){if(scan>=256)return;std::lock_guard<std::mutex> lock(inputMutex);if(bool(inputKeys[scan])==down)return;inputKeys[scan]=down?0x80:0;recordInputKey(scan,down);enqueueGuestKey(scan,down);}
void clearGuestInput(){std::lock_guard<std::mutex> lock(inputMutex);for(unsigned scan=0;scan<256;++scan)if(inputKeys[scan]){recordInputKey(scan,false);enqueueGuestKey(scan,false);}inputKeys.fill(0);inputMouseButtons.fill(0);inputMouseX=inputMouseY=inputMouseZ=0;}
void setGuestPointer(float x,float y,int phase){
    guestPointerX=std::clamp(x,0.f,1.f);guestPointerY=std::clamp(y,0.f,1.f);
    if(phase!=1){std::lock_guard<std::mutex> lock(inputMutex);inputMouseButtons[0]=phase==0?0x80:0;}
    enqueueGuestPointer(phase);
}
void typeGuestText(const std::vector<uint32_t>& characters){enqueueGuestText(characters);}
void setGuestOnlineServer(const std::string& host){std::lock_guard<MachineMutex> lock(machine);if(!memory&&!host.empty())onlineServer=host;}
void setGuestWidescreen(bool enabled){std::lock_guard<MachineMutex> lock(machine);if(!memory)guestWidescreen=enabled;}
void setGuestLanguage(const char* language){
    std::lock_guard<MachineMutex> lock(machine);if(memory)return;
    if(language){
        if(std::strcmp(language,"English")==0||std::strcmp(language,"english")==0||std::strcmp(language,"en")==0||std::strcmp(language,"English UK")==0||std::strcmp(language,"English US")==0){
            guestLanguage="English UK";
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Selected guest language: %s",guestLanguage.c_str());
            return;
        }
    }
    for(const char* known:{"Spanish","English UK","French","German","Italian","Dutch","Swedish","Danish","Japanese","Korean","Chinese (Traditional)","Thai"})
        if(language&&std::strcmp(known,language)==0){guestLanguage=known;NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","Selected guest language: %s",known);return;}
    guestLanguage="Spanish";
}

// D3D9 bridge (R36S render thread): wait for the render thread with the guest machine released.
void d3d9BridgeWaitWithoutMachine(const std::function<void()>& wait){
    if(!machineLease||!machineLease->owns_lock()||!machine.heldByMe()){wait();return;}
    State saved;saved.save();machineLease->unlock();
    try{wait();}catch(...){machineLease->lock();saved.load();throw;}
    machineLease->lock();saved.load();
}
extern "C" bool nfsParallelGuest(){return parallelGuest;}

#if defined(__ANDROID__) || defined(NFS_D3D9_BACKEND)
// Cacheable D3D9 COM entry: the method comes from the call's VA (nfs_icall_va), not from a lookup side
// effect, so call sites can cache it (arm64_guest.h NFS_ICALL_RESOLVE).
// Indirect calls to D3D9 COM methods land here (cached per call site). Queued/filtered calls (~28k per race
// frame) return without going through d3dDispatch; everything else (Present, synchronous calls) does.
extern "C" uint32_t getD3D9PresentCount();
static void d3dDispatchFromCallVa(){
    const uint32_t token=nfs_icall_va;
    // NFSU2_D3D_GENERIC_STATS=1: which methods still take this generic path (logged every 600 frames)
    static const bool genericStats=std::getenv("NFSU2_D3D_GENERIC_STATS")!=nullptr;
    if(__builtin_expect(genericStats,0)){
        static std::unordered_map<uint32_t,uint64_t> counts;static uint32_t lastReport=0;++counts[token];
        const uint32_t frame=getD3D9PresentCount();
        if(frame>=lastReport+600){lastReport=frame;std::vector<std::pair<uint64_t,uint32_t>> rows;for(auto& c:counts)rows.push_back({c.second,c.first});
            std::sort(rows.rbegin(),rows.rend());std::string line;
            for(size_t i=0;i<rows.size()&&i<12;++i)line+=" k"+std::to_string((rows[i].second-0x7e000000u)/4096)+"/m"+std::to_string(((rows[i].second-0x7e000000u)%4096)/16)+"="+std::to_string(rows[i].first/600);
            NFS_RUNTIME_LOG(ANDROID_LOG_INFO,"NFSU2","[D3D9 generic] calls per frame:%s",line.c_str());counts.clear();}
    }
    if(parallelGuest&&token!=0x7e002110u){
        uint32_t args[12];std::memcpy(args,ptr(g_esp+4,48),48);uint32_t count=0,result;
        if(dispatchGuestD3D9TryDefer(token,args,&count,&result)){
            if(__builtin_expect(nfs_fx_cap_depth!=0,0))nfs_fx_capture(token,args,count,1);
            qpcStreak=0;g_eax=result;g_esp+=4+count*4;return;}
    }
    d3dToken=token;d3dDispatch();}
// Effect state memo (src/runtime/fx_cache.c): replay a recorded deferred device call.
extern "C" int nfsD3dReplay(uint32_t token,const uint32_t* args){
    uint32_t a[12]={};std::memcpy(a,args,9*4);uint32_t count=0,result=0;
    return dispatchGuestD3D9TryDefer(token,a,&count,&result)?1:0;
}
#if defined(NFS_R36S)
// Device method calls from the lifted code, one handler per method (D3D9 device methods 0..118): the arguments stay on
// the guest stack and go straight to the bridge's shortcut (d3d9_render_thread.inc); anything it does not take, and
// calls made while the effect memo captures (nfs_fx_cap_depth), take the generic path.
extern "C" int nfsD3dShortcutsEnabled();
extern "C" int nfsD3dDeviceCallShortcut(uint32_t method,const uint32_t* args,uint32_t* count);
template<uint32_t Method>void d3dDeviceCallShortcut(){
    uint32_t count=0;const auto args=static_cast<const uint32_t*>(ptr(g_esp+4,36));
    if(nfsD3dDeviceCallShortcut(Method,args,&count)){
        if(__builtin_expect(nfs_fx_cap_depth!=0,0))nfs_fx_capture(nfs_icall_va,args,count,1);   // as the generic path
        qpcStreak=0;g_eax=0;g_esp+=4+count*4;return;}
    d3dDispatchFromCallVa();
}
template<std::size_t... Method>constexpr std::array<recomp_func_t,sizeof...(Method)> deviceShortcutTable(std::index_sequence<Method...>){
    return {{&d3dDeviceCallShortcut<uint32_t(Method)>...}};
}
// AddRef (1) / Release (2) of textures, cube textures, vertex and index buffers (kinds 3, 4, 6, 7).
extern "C" int nfsD3dReferenceShortcut(uint32_t kind,uint32_t method,const uint32_t* args,uint32_t* result);
template<uint32_t Kind,uint32_t Method>void d3dReferenceShortcut(){
    uint32_t result=0;const auto args=static_cast<const uint32_t*>(ptr(g_esp+4,36));
    if(nfsD3dReferenceShortcut(Kind,Method,args,&result)){
        if(__builtin_expect(nfs_fx_cap_depth!=0,0))nfs_fx_capture(nfs_icall_va,args,1,1);   // as the generic path
        qpcStreak=0;g_eax=result;g_esp+=8;return;}
    d3dDispatchFromCallVa();
}
recomp_func_t referenceShortcut(uint32_t kind,uint32_t method){
    switch(kind*4+method){
    case 3*4+1:return d3dReferenceShortcut<3,1>;case 3*4+2:return d3dReferenceShortcut<3,2>;
    case 4*4+1:return d3dReferenceShortcut<4,1>;case 4*4+2:return d3dReferenceShortcut<4,2>;
    case 6*4+1:return d3dReferenceShortcut<6,1>;case 6*4+2:return d3dReferenceShortcut<6,2>;
    case 7*4+1:return d3dReferenceShortcut<7,1>;case 7*4+2:return d3dReferenceShortcut<7,2>;
    default:return nullptr;
    }
}
constexpr uint32_t deviceMethodCount=119;
const std::array<recomp_func_t,deviceMethodCount> deviceCallShortcuts=deviceShortcutTable(std::make_index_sequence<deviceMethodCount>());
#endif
extern "C" recomp_func_t recomp_lookup_cacheable(uint32_t va){
    if(va>=0x7e001000u&&va<0x7e010000u&&!(va%16)){
        checkGuestProgress();
#if defined(NFS_R36S)
        const uint32_t kind=(va-0x7e000000u)/4096,method=((va-0x7e000000u)%4096)/16;
        if(kind==2&&method<deviceMethodCount&&nfsD3dShortcutsEnabled())return deviceCallShortcuts[method];
        if((method==1||method==2)&&nfsD3dShortcutsEnabled())if(recomp_func_t shortcut=referenceShortcut(kind,method))return shortcut;
#endif
        return d3dDispatchFromCallVa;
    }
    return nullptr;
}
#else
extern "C" recomp_func_t recomp_lookup_cacheable(uint32_t){return nullptr;}
#endif
