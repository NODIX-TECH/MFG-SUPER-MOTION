// lf_dlssg.inl - DLSS MFG on a captured picture: NVIDIA's frame generation
// runtime (nvngx_dlssg.dll) given the captured colour and a flat depth plane,
// its frames in between shown by a presenter of its own (lf_fg_pace.inl says
// when). The motion it finds itself; a field of the engine's is handed to it
// only when the app asks for one (sm_options.h, MotionGiven). RTX 20/30 go through the compatibility
// package (lf_dlssg_compat.inl), RTX 40 through the Ada kernel adaptation
// (lf_dlssg_ada.h), RTX 50 through the runtime as it is.
//
// Two generators, one presenter (round 12). Everything here but the calls
// into NGX is the engine's own and serves XeSS MFG as well (xe_mfg.inl): the
// real frames and their small copies, the pairs and their places, how many
// frames in between a pair gets, the presenter with its pacing, VRR, HDR and
// Low Latency, the rebuilding of small frames, what stays still. Where a
// pair's frames in between are asked for, the generator picked at the start
// is asked - three places, each marked "XeSS MFG": EnsureFg (what it needs to
// run), FgPresent (the frames themselves), CloseFgResources.
//
// Three generators (round 16): MSM FG, the engine's own (msm_fg.h), at the
// same three places, each marked "MSM FG". Its frames are made at the
// picture's own size from the two real frames, in the pair's own list, into
// the pictures DLSS MFG's go into - so from there on a pair of its frames is
// a pair of DLSS MFG's made at "Flow" 100: shown as they are, nothing rebuilt.
#include "lf_dlssg_compat.inl"
#include "lf_dlssg_ada.h"

// Round 45, frames.log (frame_log=1 - the app asks for it in its development build only - or SM_FRAME_LOG=1): every
// frame shown, one line each - when (seconds since the engine began), the gap since the one before, a real frame or one
// in between, its pair, how far behind its moment it was shown - and the engine's own lines among them ("#"), so
// that a stutter is seen beside what the engine said just then. A gap over twice the usual one and over one and a half
// refreshes is marked "<<< HITCH".
static FILE *g_frames_file = nullptr;
static std::mutex g_frames_lock;
static LONGLONG g_frames_t0 = 0, g_frames_hz = 0;
static double FramesNow()
{
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    return double(now.QuadPart - g_frames_t0) / double(g_frames_hz);
}
static void FramesOpen()
{
    wchar_t root[MAX_PATH] = {};
    EngineDataPath(root);
    if (!root[0]) return;
    LARGE_INTEGER f, now; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&now);
    g_frames_hz = f.QuadPart; g_frames_t0 = now.QuadPart;
    // (The run before kept as frames.prev.log - round 47: a run the person spoke of was gone with the next start.)
    MoveFileExW((std::wstring(root) + L"\\frames.log").c_str(), (std::wstring(root) + L"\\frames.prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    g_frames_file = _wfsopen((std::wstring(root) + L"\\frames.log").c_str(), L"w", _SH_DENYWR);
    if (g_frames_file) fputs("    time(s)       gap    kind      pair    behind its moment\n", g_frames_file);
}
static void FramesLine(const char *pixel_format, ...)
{
    if (!g_frames_file) return;
    char text[1024];
    va_list args;
    va_start(args, pixel_format);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, pixel_format, args);
    va_end(args);
    std::lock_guard<std::mutex> hold(g_frames_lock);
    fputs(text, g_frames_file);
    fputc('\n', g_frames_file);
    fflush(g_frames_file);
}
#include "lf_fg_scale.inl"
#include "lf_fg_pace.inl"
static HANDLE g_fg_waitable = nullptr;
static UINT64 g_fg_present_fence = 0;
// Experimental desktop DLSS-G. Capture has no engine depth: use a flat plane
// and the estimated motion field. Keep this opt-in; UI/occlusions can distort.
static NrEvaluateFn g_fg_evaluate = nullptr;
static NVSDK_NGX_Result FgEvaluateBridge(ID3D12GraphicsCommandList *list,
    NVSDK_NGX_Handle *handle, NVSDK_NGX_Parameter *params, PFN_NVSDK_NGX_ProgressCallback cb)
{
    __try { return g_fg_evaluate(list, handle, params, cb); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Trace("[dlssg] unavailable: evaluate exception 0x%08X", GetExceptionCode()); return NVSDK_NGX_Result_Fail; }
}
#define NVSDK_NGX_D3D12_EvaluateFeature_C FgEvaluateBridge
#include "include/nvsdk_ngx_helpers_dlssg.h"
#undef NVSDK_NGX_D3D12_EvaluateFeature_C

// The picture's movement from the real frame before a pair to the one after,
// in 6 x 4 blocks, each as a share of the picture's width (sm_guard.inl finds
// it for the pair at hand; the producer copies it into the pair's slot, and
// sm_rebuild.inl starts its own search from it).
struct RebuildPrior
{
    static constexpr int kAcross = 6, kDown = 4;
    float moved[kAcross * kDown][2] = {};
};
static RebuildPrior g_fg_prior;

// A real frame as the generator and the presenter need it: at the picture's
// own size (COPY_SOURCE at rest), and - when frames in between are made
// smaller - at their size (NON_PIXEL_SHADER_RESOURCE at rest). A pair's
// presenter needs the real frame before it too (sm_rebuild.inl), so the
// frames are shared: one is free again when nobody holds it but the pool.
struct FgReal {
    winrt::com_ptr<ID3D12Resource> full, low;
};
struct FgSlot {
    std::shared_ptr<FgReal> real, before;
    winrt::com_ptr<ID3D12Resource> interpolated[5];
    RebuildPrior prior;       // how the picture moved from `before` to `real`
    unsigned count = 1;
    unsigned made = 1;        // of them the generator's own (FgMadeCount); the rest go by the nearest of those
    int state = 0; // free, writing, ready, presenting; protected by mutex
    UINT64 sequence = 0;
    bool interpolate = false;
    double interval = 0.016;
    double source_time = 0;
    // The moment of the game this real frame stands for and the one before
    // it (QPC seconds; lf_fg_pace.inl), and when the pair was handed over.
    double moment = 0, prev_moment = 0;
    double ready_at = 0;
    // The pair is handed to the presenter as soon as its work is sent to the
    // card, not when it is done (FgPresent): the mark the work is done at, the
    // time it was (0 until then), and whether the generator then said its
    // frames in between are not to be shown.
    UINT64 mark = 0;
    // XeSS MFG: its frames in between are in the pair's pictures once the
    // library's own queue reaches this (xe_mfg.inl); 0 with DLSS MFG, whose
    // frames are part of the work `mark` stands for.
    UINT64 taken = 0;
    std::atomic<double> done_at{0.0};
    std::atomic<bool> denied{false};
    // Round 53, Adaptive MFG (AdaptiveLay): the pair's pictures at the output's own even beat - for each, its moment
    // (absolute, the frame clock's), its place between the two real frames, and what it is (a frame in between, by
    // its index; -1 the real frame after, -2 the real frame before). 0: the pair is shown as always.
    bool ad_on = false;       // shown by the beat (none of its beats: nothing of it is shown)
    unsigned adaptive = 0;
    double ad_when[8] = {};
    float ad_phase[8] = {};
    signed char ad_what[8] = {};
    // Set by the card when the mark is reached; FgDoneWatch writes done_at then.
    HANDLE done_event = nullptr;
    std::atomic<UINT64> watched{0};
    unsigned code = 0;        // tests: the frame's own number, read from the picture (tests\game.cpp)
    // Where nothing moved near (lf_fg_still.inl): the slot's own tile map, and
    // whether any tile of it is at rest.
    winrt::com_ptr<ID3D12Resource> still;
    bool protect = false;
};
static struct FgState {
    HMODULE module = nullptr;
    bool initialized = false;
    NrCreateFn create = nullptr;
    NrReleaseFn release = nullptr;
    NVSDK_NGX_Parameter *params = nullptr;
    NVSDK_NGX_Handle *feature = nullptr;
    StagedTexture depth;
    winrt::com_ptr<ID3D12Resource> motion;
    std::shared_ptr<FgReal> reals[6];     // the pool: four pairs in flight, each with the frame before it, and the next
    std::shared_ptr<FgReal> latest;       // the real frame of the pair before (the loop's thread)
    winrt::com_ptr<ID3D12Resource> output[5];
    winrt::com_ptr<ID3D12Resource> disable, disable_readback;
    UINT w = 0, height = 0, mw = 0, mh = 0;
    UINT full_w = 0, full_h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    FgSlot slots[4];
    std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    std::atomic<bool> stop{false}, failed{false};
    UINT64 sequence = 0;
    std::chrono::steady_clock::time_point last;
    bool history = false;
    double source_time = 0, extra_budget = 0;
    std::atomic<unsigned> captured{0};
    ~FgState() { stop = true; wake.notify_all(); if (thread.joinable()) thread.join(); }
} g_fg;
#include "lf_fg_still.inl"
#include "sm_rebuild.inl"
#include "msm_fg.h"
#include "msm_ai.h"

// Never free textures still referenced by submitted GPU work on a timeout.
static bool FgAwait(ID3D12Fence *fence, UINT64 value, DWORD timeout)
{
    if (AwaitMark(fence, value, timeout)) return true;
    if (lfCore.device && SUCCEEDED(lfCore.device->GetDeviceRemovedReason())) {
        Trace("[dlssg] unavailable: GPU work timed out; stopping this worker safely");
        TerminateProcess(GetCurrentProcess(), ERROR_TIMEOUT);
    }
    return false;
}

// The presenter's own waits for the card - for a list to be through before its allocator is used again, for a
// pair's pictures to have been read before they are let go. A card that is busy with something else for a few
// seconds - a game loading, a window switched, a driver thinking - has not stopped: it is waited for ten
// seconds, said once in the log, and then gone on without (the worst that follows is one pair's pictures handed
// back while a list still reads them). The engine is not ended over it.
static bool FgAwaitPatient(ID3D12Fence *fence, UINT64 value)
{
    if (AwaitMark(fence, value, 2000)) return true;
    if (fence->GetCompletedValue() == UINT64_MAX) return false;
    const double from = LatencyNow();
    if (AwaitMark(fence, value, 8000))
    {
        Trace("[stall] the card took %.1f s over a pair's pictures - waited for", 2.0 + LatencyNow() - from);
        return true;
    }
    Trace("[stall] the card has not finished a pair's pictures in ten seconds - gone on without it");
    return false;
}

static unsigned g_fg_count = 1, g_fg_requested_count = 0, g_fg_runtime_max = 5;

// Round 53, Adaptive MFG (users' request: Lossless Scaling's and NVIDIA's own). The frames are shown at the output's
// own even beat - a target's frames a second (the screen's refresh unless given), whatever the game draws: a game at
// 70 on a 165 Hz screen is 2.36 frames for each of its own, not x2 or x3. Each picture's moment is a beat of that
// target, and what is shown then is made for that very moment between the two real frames (SMFG makes a frame at
// any moment, msm_fg.h); a beat that falls this near a real frame shows the real frame itself. Before, a limit filled
// a pair with a whole count spread over that pair - 1, 2, 1, 2 frames in between, so the steps between frames shown
// were 8.3 and 5.6 ms in turn at 150 FPS of a 60 FPS game; on the beat they are 6.7 ms, all of them.
// DLSS MFG and XeSS MFG make their frames only at even shares of a pair: with them the target is a limit, filled
// as before (FgPresent: `capped`).
static constexpr float kAdaptiveNear = 0.08f;
struct AdaptivePlan { unsigned n = 0, made = 0; double when[8] = {}; float phase[8] = {}; signed char what[8] = {}; };
static double g_adaptive_next = 0.0;          // the next beat, the frame clock's seconds; 0: none yet
static double AdaptiveTarget();               // below (it needs the screen's refresh)
static unsigned FgExtraFrames() { return g_fg_count; }
// The pair at hand moved further than the generator follows (sm_guard.inl):
// it gets no frames in between. Set by the loop before each FgPresent.
static bool g_fg_too_fast = false;
// LF_FG_TRACE (tests\pace.ps1): the number the stand-in game wrote into its
// frame - sixteen bars along the bottom edge, white a one - read from the
// small luma of the picture at hand.
static unsigned FgFrameCode()
{
    static const bool tracing = [] { char v[4] = {}; return GetEnvironmentVariableA("LF_FG_TRACE", v, sizeof(v)) > 0; }();
    if (!tracing || !lfLuma.mapped || lfLuma.view == nullptr || lfLuma.w < 80 || lfLuma.h < 45) return 0;
    unsigned code = 0;
    const BYTE *row = lfLuma.view + static_cast<size_t>(lfLuma.h - 2) * lfLuma.w;
    for (unsigned bit = 0; bit < 16; ++bit)
        if (row[(2 * bit + 1) * lfLuma.w / 80] > 128) code |= 1u << bit;
    return code;
}
// LF_FG_DUMP: which pairs are written out - a few, each with the real frame
// before it (tests\jelly.cpp).
// A real frame found no place free (FgPresent): told to the presenter.
static std::atomic<bool> g_fg_starved{false};

static bool FgDumpWanted(UINT64 n) { return n == 30 || n == 90 || n == 150 || n == 300; }

// How many of a pair's frames in between the generator itself makes (round 8).
//
// Every one of them, until the picture was built from the two real frames
// wherever those agree (sm_rebuild.inl): since then a frame of the
// generator's is only asked about what moves its own way and what appears or
// goes - and each costs the game far more than its 0.8 ms on the card (a
// light game at 185 FPS came down to 61 pairs a second at x6, where the work
// counted on the card would leave 80: every evaluation is a round of its own
// between the card's two programming interfaces). So from four frames in
// between on (x5, x6) the generator makes two, a third and two thirds of the
// way - its own x3 - and the others go by the nearest of those for what
// moves its own way, the last one by the real frame after: at x6 such a
// thing steps a third of its way every second frame shown, evenly, while the
// picture itself - everything the two real frames agree about - moves with
// every frame. SM_FG_EVERY=1 (to compare): every frame from the generator.
static unsigned FgMadeCount(unsigned count, bool rebuilt)
{
    static const bool every = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_FG_EVERY", v, sizeof(v)) > 0 && v[0] == '1'; }();
    return (rebuilt && !every && count >= 4) ? 2u : count;
}

// Which of the generator's frames a frame in between goes by (0..made-1), or
// -1: the real frame after. And the moment that one stands for.
static int FgMadeFor(unsigned index, unsigned count, unsigned made)
{
    if (made == 0 || made == count) return static_cast<int>(index);
    // x6 with two: in twos - a third, a third, two thirds, two thirds, the frame after.
    if (count == 5 && made == 2) return index < 2 ? 0 : index < 4 ? 1 : -1;
    const double phase = double(index + 1) / (count + 1);
    int nearest = 0;
    double least = 1e9;
    for (unsigned g = 0; g < made; ++g)
    {
        const double off = std::abs(double(g + 1) / (made + 1) - phase);
        if (off < least) { least = off; nearest = static_cast<int>(g); }
    }
    return nearest;
}

// The loop does not wait for a pair's work on the card before it goes for
// the game's next frame (round 5). It did: frame taken, generator asked,
// waited for, and only then the next frame looked for - so what a pair took
// on the card, with every wait behind the game's own work in it, was time no
// frame of the game could be taken in. Read off a session's log: a game that
// kept its 92 FPS under the engine had every second frame taken at x6 (46
// pairs a second) and every third with frames in between made at the
// picture's size (30). Now the pair's work is taken up when the next pair is
// about to be recorded (FgCollect): the next frame is captured, and its small
// luma read and judged, while the card makes the frames in between of this
// one. One generator's list is in flight at most.
//
// Not with a motion field of the engine's handed to the generator: the
// optical flow has a queue of its own, and what it writes for the next frame
// must not reach a list still waiting. SM_SERIAL=1: as it was (to compare).
static struct FgPending
{
    UINT64 mark = 0;            // the work's mark on the queue's timeline; 0: none in flight
    FgSlot *slot = nullptr;     // the pair it is for
    UINT64 sequence = 0;
    unsigned evaluated = 0;
} g_fg_pending;
static unsigned g_fg_still_before = 0;      // tiles at rest in the pair before (lf_fg_still.inl)

// How many frames in between a pair can really be shown. Asked for more than
// reach the screen - a game at 92 with x6 is 552 frames a second, and a frame
// takes the presenter the better part of two milliseconds - the ones that
// came too late were left out, now this one, now that one: a ragged pace, and
// every one of them made for nothing. So the presenter counts each second
// what it was given and what it showed, and when a tenth and more is left
// out for two seconds running the pairs get one frame fewer; when eight
// seconds went by with all of them shown, one more is tried again, and if
// that fails it is left alone for a minute. A whole count, the same for
// every pair, that the screen gets all of - and every frame of the game
// under it.
static std::atomic<unsigned> g_fg_count_cap{0};     // most frames in between a pair; 0: as asked
static std::atomic<unsigned> g_fg_overrun{0};       // frames of pairs that gave way unseen (FgPresent), since the presenter last looked

static bool FgPipelined()
{
    static const bool serial = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_SERIAL", v, sizeof(v)) > 0 && v[0] == '1'; }();
    return !serial && !MotionGiven();
}

// The pending pair's work: waited for, and what it left read - whether the
// generator said its frames are not to be shown, what it cost, how many
// tiles were at rest. False: the card did not answer.
static bool FgCollect()
{
    const FgPending p = g_fg_pending;
    if (p.mark == 0) return true;
    g_fg_pending.mark = 0;
    const double from = LatencyNow();
    if (!FgAwait(lfCore.timeline, p.mark, 30000)) return false;
    g_steady.fence_us += static_cast<unsigned>(std::clamp((LatencyNow() - from) * 1e6, 0.0, 1e6));
    FgCostRead();
    g_fg_still_before = FgStillCounted();
    bool allowed = true;
    BYTE *disabled = nullptr;
    D3D12_RANGE read = {0, p.evaluated * 4}, written = {0, 0};
    // (NGX's own word on a pair. XeSS MFG says it when it is asked: FgPresent.)
    if (NgxSelected() && p.evaluated && g_fg.disable_readback &&
        SUCCEEDED(g_fg.disable_readback->Map(0, &read, reinterpret_cast<void **>(&disabled))))
    {
        for (unsigned i = 0; i < p.evaluated; ++i) allowed = allowed && disabled[i * 4] == 0;
        g_fg.disable_readback->Unmap(0, &written);
    }
    if (!allowed && p.slot && p.slot->sequence == p.sequence) p.slot->denied = true;
    return true;
}

// When a pair's work was done, to the presenter (it paces by it): the card
// sets the slot's event at the pair's mark, and the time is written here.
static HANDLE g_fg_done_stop = nullptr;
static std::thread g_fg_done_thread;
static void FgDoneWatch()
{
    HANDLE handles[5] = {};
    for (int i = 0; i < 4; ++i) handles[i] = g_fg.slots[i].done_event;
    handles[4] = g_fg_done_stop;
    for (;;)
    {
        const DWORD got = WaitForMultipleObjects(5, handles, FALSE, INFINITE);
        if (got < WAIT_OBJECT_0 || got >= WAIT_OBJECT_0 + 4) return;
        FgSlot &slot = g_fg.slots[got - WAIT_OBJECT_0];
        const UINT64 mark = slot.watched.load();
        if (mark && lfCore.timeline && lfCore.timeline->GetCompletedValue() >= mark && slot.done_at.load() == 0.0)
            slot.done_at = LatencyNow();
    }
}

static void FgDoneWatchStart()
{
    if (g_fg_done_thread.joinable()) return;
    if (!g_fg_done_stop) g_fg_done_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (auto &slot : g_fg.slots)
        if (!slot.done_event) slot.done_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_fg_done_stop) return;
    for (auto &slot : g_fg.slots) if (!slot.done_event) return;
    ResetEvent(g_fg_done_stop);
    g_fg_done_thread = std::thread(FgDoneWatch);
}

static void FgDoneWatchStop()
{
    if (!g_fg_done_thread.joinable()) return;
    SetEvent(g_fg_done_stop);
    g_fg_done_thread.join();
}

static bool FgRequested()
{
    return smOpt.mfg.load() != 0 && !g_fg.failed;
}

static void StopFgPresentation()
{
    // Nothing a list still on the card uses is let go under it.
    FgCollect();
    // XeSS MFG: nor is a frame the library shows late copied into a picture that is gone.
    if (XeSelected()) XeForget();
    if (MsmSelected()) MsmForget();
    FgDoneWatchStop();
    g_fg.stop = true;
    g_fg.wake.notify_all();
    if (g_fg.thread.joinable()) g_fg.thread.join();
    // Hand the swapchain back to the ordinary present path: default latency,
    // the waitable handle dies with the swapchain, not with us.
    if (lfShow.chain != nullptr)
    {
        IDXGISwapChain2 *sc2 = nullptr;
        if (SUCCEEDED(lfShow.chain->QueryInterface(
                __uuidof(IDXGISwapChain2), reinterpret_cast<void **>(&sc2)))
            && sc2 != nullptr)
        {
            sc2->SetMaximumFrameLatency(LowLatencyWanted() ? 1 : 3);
            sc2->Release();
        }
    }
    if (g_fg_waitable && g_fg_waitable != lfShow.quick_waitable) CloseHandle(g_fg_waitable);
    g_fg_waitable = nullptr;
    for (auto &slot : g_fg.slots)
    {
        slot.real.reset(); slot.before.reset(); slot.still = nullptr; slot.protect = false;
        for (auto &image : slot.interpolated) image = nullptr;
        slot.state = 0;
    }
    g_fg.latest.reset();
    for (auto &real : g_fg.reals) real.reset();
    FgStillClose();
    RebuildClose();
    g_fg.history = false;
    g_fg.source_time = g_fg.extra_budget = 0;
}

static void CloseFgResources()
{
    CmdCarryFlush();
    StopFgPresentation();
    if (g_fg.feature) { g_fg.release(g_fg.feature); g_fg.feature = nullptr; }
    // XeSS MFG: the library's swap chain goes before the pictures its frames were copied into.
    if (XeSelected()) XeClose();
    // MSM FG: what it works in (its passes stay).
    if (MsmSelected()) MsmClose();
    if (g_fg.depth.gpu_tex) { g_fg.depth.gpu_tex->Release(); g_fg.depth.gpu_tex = nullptr; }
    if (g_fg.depth.staging) { g_fg.depth.staging->Release(); g_fg.depth.staging = nullptr; }
    for (auto &image : g_fg.output) image = nullptr;
    g_fg_scale.display = nullptr;
    g_fg.disable = nullptr; g_fg.disable_readback = nullptr;
    g_fg.w = g_fg.height = 0;
}

static void ShutdownFg()
{
    CloseFgResources();
    if (g_fg.params) { NVSDK_NGX_D3D12_DestroyParameters(g_fg.params); g_fg.params = nullptr; }
    g_fg.initialized = false;
    g_fg.motion = nullptr;
}

static void FgStopIfFailed()
{
    // Join a failed presenter before the ordinary path owns the swap chain.
    if (g_fg.failed && (g_fg.feature || XeLive() || g_fg.thread.joinable())) CloseFgResources();
}

// The presenter has its own command list/allocator/fence. The producer never
// waits for frame pacing. Slots being displayed cannot be recycled by it.
static void FgPresenter()
{
    g_fg_count_cap = 0;
    g_fg_overrun = 0;
    // Four allocators in turn: a frame's list may still be on the card when
    // the next one is recorded (`quick`, below), and an allocator is not to be
    // reset under a list of its own that is.
    winrt::com_ptr<ID3D12CommandAllocator> allocs[4];
    UINT64 alloc_mark[4] = {};
    unsigned alloc_at = 0;
    ID3D12CommandAllocator *alloc = nullptr;
    winrt::com_ptr<ID3D12GraphicsCommandList> list;
    winrt::com_ptr<ID3D12Fence> fence;
    bool allocated = true;
    for (auto &one : allocs)
        allocated = allocated && SUCCEEDED(lfCore.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(one.put())));
    alloc = allocs[0].get();
    if (!allocated ||
        FAILED(lfCore.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(list.put()))) ||
        FAILED(lfCore.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put()))))
    { Trace("[dlssg] unavailable: presenter allocation failed"); g_fg.failed = true; return; }
    list->Close();
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) { Trace("[dlssg] unavailable: presenter event allocation failed"); g_fg.failed = true; return; }
    UINT64 value = 0, shown = 0, generated = 0;
    // LF_FG_TRACE=<file>: one line per Present - when, which pair, the moment
    // of the game the picture stands for - for tests/screen_fg_pacing.py.
    FILE *trace = nullptr;
    {
        char trace_path[MAX_PATH] = {};
        const DWORD len = GetEnvironmentVariableA("LF_FG_TRACE", trace_path, sizeof(trace_path));
        if (len > 0 && len < sizeof(trace_path)) fopen_s(&trace, trace_path, "w");
        // (Round 38: and each Present's number, and the latest the screen says it showed - its number and when -
        // so that a test reads when frames reached the screen, not only when they were handed over.)
        if (trace) fputs("t,generated,content,sequence,ready,source,code,id,shown_id,shown_at,phase,pair_code\n", trace);
    }
    double trace_content = 0.0, trace_ready = 0.0, trace_phase = 1.0;   // (trace_phase: the place between the pair's real frames)
    UINT64 trace_sequence = 0;
    unsigned trace_code = 0;
    FgPace pace;
    // SM_DIAG=1: what drawing a frame costs on the card (a real one, one in
    // between), by time stamps at the two ends of its list.
    winrt::com_ptr<ID3D12QueryHeap> stamps;
    winrt::com_ptr<ID3D12Resource> stamps_back;
    UINT64 stamp_rate = 0;
    double draw_cost[2] = {};
    unsigned draw_count[2] = {};
    // Of a frame in between: the pair's own movement (once a pair), follow, choose, build, onto the back buffer.
    double part_cost[5] = {};
    unsigned part_frames = 0, part_pairs = 0;
    {
        char v[4] = {};
        if (GetEnvironmentVariableA("SM_DIAG", v, sizeof(v)) > 0 && v[0] == '1')
        {
            D3D12_QUERY_HEAP_DESC qd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kRebuildMarks, 0 };
            D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC bd = {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = 8 * kRebuildMarks; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
            bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(lfCore.device->CreateQueryHeap(&qd, IID_PPV_ARGS(stamps.put()))) ||
                FAILED(lfCore.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                              IID_PPV_ARGS(stamps_back.put()))) ||
                FAILED(lfCore.show_queue->GetTimestampFrequency(&stamp_rate)) || stamp_rate == 0)
                stamps = nullptr;
        }
    }
    unsigned reports = 0;
    FgSlot *showing = nullptr;       // the pair on its way to the screen
    unsigned still_frames = 0;       // frames in between that took still parts from their real frame
    // Since the last [steady] line: frames in between shown, passed over for being late, of pairs far longer
    // than the ones around them, refused by the generator; pairs dropped; times the delay was learnt anew.
    unsigned made_shown = 0, late_frames = 0, odd_frames = 0, denied_frames = 0, dropped_pairs = 0, relearnt = 0;
    // What the screen gets of what is asked (g_fg_count_cap): this second's frames in between given and
    // shown, the count the pairs had, seconds in a row short or whole, and when one more may be tried.
    unsigned given = 0, given_shown = 0, given_pairs = 0, pair_count = 0, short_seconds = 0, whole_seconds = 0, quiet_seconds = 0;
    ULONGLONG try_more_after = 0, cap_raised_at = 0, try_wait = 60000;
    auto report =std::chrono::steady_clock::now();
    auto reportCadence = [&] {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - report).count();
        if (elapsed < 1.0) return;
        const unsigned captured = g_fg.captured.exchange(0);
        Trace("[dlssg] displayed %.1f FPS (%llu real + %llu generated in %.2fs); experimental flat-depth guides",
              shown / elapsed, shown - generated, generated, elapsed);
        Trace("[dlssg] cadence: source=%u real=%llu generated=%llu seconds=%.4f multiplier=%u",
              captured, shown - generated, generated, elapsed, g_fg_count + 1);
        shown = generated = 0; report = now;
        {
            // The first seconds of a run are not like the rest (the generator
            // is made while its first pairs wait): nothing is decided on them.
            const unsigned unseen = g_fg_overrun.exchange(0);
            const unsigned asked_now = given + unseen;
            const ULONGLONG tick = GetTickCount64();
            const unsigned cap = g_fg_count_cap.load();
            // (Round 45: only where the screen is to be the limit - "Limit to refresh rate", VSync, a frame limit. Without
            // them the count is the multiplier's: x6 asked is x6 presented, as Lossless Scaling has it.)
            const bool screen_bound = smOpt.screen_limit.load() != 0 || SyncEvery() > 0 || ShownCap() > 0;
            if (!screen_bound && cap != 0) g_fg_count_cap = 0;
            if (screen_bound && ++quiet_seconds > 3 && asked_now >= 20 && pair_count >= 1)
            {
                // Short: a fourteenth and more left out, two seconds running.
                const unsigned missing_now = asked_now - std::min(asked_now, given_shown);
                const bool just_raised = cap_raised_at != 0 && tick - cap_raised_at < 4000;
                const bool is_short = missing_now * 14 >= asked_now;
                const bool is_whole = missing_now * 50 <= asked_now;
                short_seconds = is_short ? short_seconds + 1 : 0;
                whole_seconds = is_whole ? whole_seconds + 1 : 0;
                if (pair_count > 1 && short_seconds >= 2)
                {
                    // As many as were shown a pair, in whole frames - at once, not one
                    // step every two seconds.
                    const unsigned reached = given_pairs ? static_cast<unsigned>(0.97 * given_shown / given_pairs) : pair_count - 1;
                    const unsigned next = std::clamp(reached, 1u, pair_count - 1);
                    g_fg_count_cap = next;
                    if (just_raised) { try_more_after = tick + try_wait; try_wait = std::min<ULONGLONG>(try_wait * 2, 300000); }
                    else try_more_after = tick + 8000;
                    cap_raised_at = 0;
                    short_seconds = whole_seconds = 0;
                    Trace("[dlssg] %u of %u frames in between reached the screen this second: the pairs get %u each from here "
                          "(x%u asked)", given_shown, asked_now, next, g_fg_count + 1);
                }
                else if (cap != 0 && whole_seconds >= 8 && tick >= try_more_after)
                {
                    const unsigned next = cap + 1;
                    g_fg_count_cap = next >= g_fg_count ? 0 : next;
                    cap_raised_at = tick;
                    whole_seconds = 0;
                    Trace("[dlssg] every frame in between reached the screen for eight seconds: %u each are tried again", next);
                }
                if (cap != 0 && cap >= g_fg_count) g_fg_count_cap = 0;
            }
            given = given_shown = given_pairs = 0;
        }
        if (++reports % 5 == 0 && pace.pairs)
        {
            Trace("[dlssg] pacing: every frame %.1f ms after its moment (Queue target %u), %u of %u pairs came late, "
                  "%u frames in between passed over, %u pairs dropped; drawing takes %.2f ms (real) and %.2f ms "
                  "(in between), the Present %.2f ms after %.2f ms waiting for the screen",
                  pace.delay * 1000.0, FgQueueTarget(), pace.late, pace.pairs, pace.skipped, pace.dropped,
                  pace.lead[0] * 1000.0, pace.lead[1] * 1000.0, pace.call * 1000.0, pace.waited * 1000.0);
            if (still_frames) Trace("[dlssg] still pixels: %u frames in between kept their still parts from the real frame", still_frames);
            if (draw_count[0] && draw_count[1])
                Trace("[cost] drawing on the card: a real frame %.2f ms, a frame in between %.2f ms", draw_cost[0] / draw_count[0],
                      draw_cost[1] / draw_count[1]);
            if (part_frames && part_pairs)
                Trace("[cost] of a frame in between: follow %.2f ms, choose %.2f, build %.2f, onto the back buffer %.2f; and once a "
                      "pair its own movement %.2f ms", part_cost[1] / part_frames, part_cost[2] / part_frames, part_cost[3] / part_frames,
                      part_cost[4] / part_frames, part_cost[0] / part_pairs);
            for (double &cost : part_cost) cost = 0.0;
            part_frames = part_pairs = 0;
            draw_cost[0] = draw_cost[1] = 0.0; draw_count[0] = draw_count[1] = 0;
            pace.pairs = pace.late = pace.skipped = pace.dropped = 0;
            still_frames = 0;
            // Everything that kept a frame in between from the screen since
            // the last of these lines; nothing is said while nothing did.
            const unsigned pairs = g_steady.pairs.exchange(0), asked = g_steady.asked.exchange(0);
            const unsigned rested = g_steady.rested.exchange(0), fast = g_steady.too_fast.exchange(0), cut = g_steady.cut.exchange(0);
            const unsigned repeats = g_steady.repeats.exchange(0), let_go = g_steady.let_go.exchange(0);
            const unsigned no_room = g_steady.no_room.exchange(0), changes = g_steady.changes.exchange(0);
            const unsigned missing = asked > made_shown ? asked - made_shown : 0;
            if (pairs && (missing * 50 > asked || rested || fast || cut > 1 || no_room || changes || relearnt || repeats * 10 > pairs))
                Trace("[steady] %u pairs, %u of %u frames in between shown. Pairs without any: %u a cut or a pause, %u the game "
                      "fills the screen, %u the picture too fast. Frames in between left out: %u late, %u of pairs far longer "
                      "than usual, %u the generator's own no, %u with pairs dropped whole (%u). The count a pair changed %u "
                      "times, the delay was learnt anew %u times, %u real frames found no room. Desktop frames not the game's: "
                      "%u (%u let go unseen)",
                      pairs, made_shown, asked, cut, rested, fast, late_frames, odd_frames, denied_frames,
                      missing > late_frames + odd_frames + denied_frames ? missing - late_frames - odd_frames - denied_frames : 0,
                      dropped_pairs, changes, relearnt, no_room, repeats, let_go);
            made_shown = late_frames = odd_frames = denied_frames = dropped_pairs = relearnt = 0;
            // Does the loop keep up with the game?
            const unsigned taken = g_steady.taken.exchange(0), waiting = g_steady.waiting.exchange(0), desktop = g_steady.desktop.exchange(0);
            const unsigned busy_us = g_steady.busy_us.exchange(0), fence_us = g_steady.fence_us.exchange(0);
            static const bool keep_told = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_DIAG", v, sizeof(v)) > 0 && v[0] == '1'; }();
            if (taken && (keep_told || waiting * 2 > taken))
                Trace("[keep] %u frames of the game taken, %u of them were already there when the loop came for them; the desktop "
                      "drew %u meanwhile; a frame keeps the loop busy %.1f ms, %.1f of it waiting for the generator's work",
                      taken, waiting, desktop, busy_us / 1000.0 / taken, fence_us / 1000.0 / taken);
        }
    };
    // Until a moment (QPC seconds): the timer for most of it, the last half
    // millisecond in turns. False when the presenter is being stopped.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
    auto waitUntil = [&](double when) {
        for (;;)
        {
            if (g_fg.stop) return false;
            const double left = when - LatencyNow();
            if (left <= 0.0) return true;
            if (left > 0.0008)
            {
                LARGE_INTEGER due;
                due.QuadPart = -static_cast<LONGLONG>((std::min(left, 0.05) - 0.0005) * 1e7);
                if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 60);
                else Sleep(left > 0.003 ? 1 : 0);
            }
            else SwitchToThread();
        }
    };
    // Exactly one consumer and one wait per Present, including the first.
    // An extra startup wait consumes a permit without submitting a frame.
    // Bound waits so a hidden/stalled output cannot freeze capture or shutdown.
    auto waitForPresent = [&] {
        if (!g_fg_waitable) return;
        const DWORD most = static_cast<DWORD>(std::clamp(1500.0 / std::max(ShownRefresh(), 24.0), 4.0, 63.0));
        if (WaitForSingleObject(g_fg_waitable, most) != WAIT_OBJECT_0)
            Trace("[dlssg] waitable timeout after %lu ms; attempting present", most);
    };
    // A frame goes to the screen in two steps (round 28): drawn into the back
    // buffer as soon as the one before it has been presented, and presented
    // when its time comes. The copy waits for the card - under a game that
    // fills it, 1 to 5 ms, never the same twice - and done at the last moment
    // that wait was each frame's own lateness.
    // LF_FG_DUMP: the frames in between as they reach the screen, for the
    // pairs the producer dumps the real frames of (FgDump).
    ID3D12Resource *shown_from = nullptr;       // what the last frame drawn was copied to the back buffer from
    D3D12_RESOURCE_STATES shown_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    // A pair's pictures are drawn after the work that makes them (its mark):
    // the card goes from the one to the other by itself. Sent only once that
    // work was done - the loop woken, the presenter woken, its list sent - the
    // card had long gone back to the game, and the drawing was one more piece
    // of work by itself (lf_gpu_core.inl, CmdCarry, says what each costs).
    UINT64 after_mark = 0;
    UINT64 after_taken = 0;     // XeSS MFG: and after the library has drawn the pair's frames (xe_mfg.inl)
    // A pair with a frame every three and a half milliseconds or less (x6 of
    // 60 and more) is not waited for frame by frame: each wait is the card's
    // turn coming round - a millisecond under a game - before a Present of
    // four tenths, six times a pair, and at 80 pairs a second the presenter
    // was the limit: frames came late, were passed over, and the count a pair
    // was cut for it (a light game at x6: 65 pairs with four or three each
    // where the card had room for 80 with five). The list is sent, the
    // Present asked for behind it - the card takes them in that order by
    // itself - and the pair is waited for once, whole, before its pictures
    // are let go. Slower pairs keep the wait: there it is what makes a
    // frame's moment on the screen its own.
    bool quick = false;
    // The next allocator, free of the list it last held.
    auto take = [&]() {
        alloc_at = (alloc_at + 1) % 4;
        alloc = allocs[alloc_at].get();
        if (alloc_mark[alloc_at] && fence->GetCompletedValue() < alloc_mark[alloc_at]) FgAwaitPatient(fence.get(), alloc_mark[alloc_at]);
        return SUCCEEDED(alloc->Reset()) && SUCCEEDED(list->Reset(alloc, nullptr));
    };
    auto send = [&](bool wait = true) {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList *commands[] = {list.get()};
        if (after_mark && lfCore.show_queue != lfCore.direct_queue) lfCore.show_queue->Wait(lfCore.timeline, after_mark);
        XeAfter(lfCore.show_queue, after_taken);
        lfCore.show_queue->ExecuteCommandLists(1, commands);
        if (FAILED(lfCore.show_queue->Signal(fence.get(), ++value))) return false;
        SpoutAfter(lfCore.show_queue);
        alloc_mark[alloc_at] = value;
        if (!wait) return true;
        return SUCCEEDED(fence->SetEventOnCompletion(value, event)) &&
               WaitForSingleObject(event, 2000) == WAIT_OBJECT_0 && fence->GetCompletedValue() >= value;
    };
    auto dump = [&](UINT64 sequence, unsigned index) {
        char dir[MAX_PATH] = {};
        if (!shown_from || !FgDumpWanted(sequence) || !GetEnvironmentVariableA("LF_FG_DUMP", dir, sizeof(dir))) return;
        const auto desc = shown_from->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        UINT rows; UINT64 rowbytes, bytes;
        lfCore.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowbytes, &bytes);
        D3D12_HEAP_PROPERTIES heap = {}; heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = bytes; bd.Height = 1; bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        winrt::com_ptr<ID3D12Resource> rb;
        if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                          IID_PPV_ARGS(rb.put()))) ||
            !take()) return;
        auto pre = Barrier(shown_from, shown_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (shown_state != D3D12_RESOURCE_STATE_COPY_SOURCE) list->ResourceBarrier(1, &pre);
        D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
        from.pResource = shown_from; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = rb.get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = fp;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        auto post = Barrier(shown_from, D3D12_RESOURCE_STATE_COPY_SOURCE, shown_state);
        if (shown_state != D3D12_RESOURCE_STATE_COPY_SOURCE) list->ResourceBarrier(1, &post);
        if (!send()) return;
        BYTE *data = nullptr; D3D12_RANGE read = {0, static_cast<SIZE_T>(bytes)}, written = {0, 0};
        if (FAILED(rb->Map(0, &read, reinterpret_cast<void **>(&data)))) return;
        char path[MAX_PATH]; sprintf_s(path, "%s/fg-%llu-%u.raw", dir, sequence, index);
        FILE *file = nullptr;
        if (!fopen_s(&file, path, "wb") && file) {
            for (UINT y = 0; y < rows; ++y) fwrite(data + size_t(y) * fp.Footprint.RowPitch, 1, size_t(rowbytes), file);
            fclose(file);
        }
        rb->Unmap(0, &written);
    };
    // A frame goes to the screen in two steps (round 28): drawn into the back
    // buffer as soon as the one before it has been presented, and presented
    // when its time comes. The copy waits for the card - under a game that
    // fills it, 1 to 5 ms, never the same twice - and done at the last moment
    // that wait was each frame's own lateness.
    // phase: where a frame in between lies between its two real frames.
    // raw (XeSS MFG): source is a picture the library drew its frame into (xe_tap.h) - at rest as a back
    // buffer is, not as a copy source like every other picture here: turned for this drawing, and turned back.
    auto draw = [&](ID3D12Resource *source, bool synthetic, float phase, float made_phase, bool raw) {
        winrt::com_ptr<ID3D12Resource> bb;
        if (FAILED(ShowBackBuffer(IID_PPV_ARGS(bb.put()))) ||
            !take()) return false;
        if (stamps) list->EndQuery(stamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        RebuildMarks marks = { stamps.get(), 1, false };
        auto pre = Barrier(bb.get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &pre);
        if (raw)
        {
            auto turned = Barrier(source, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &turned);
        }
        // A frame in between is brought to the picture's size into a target
        // of that size: rebuilt from the real frames around it when it was
        // made smaller (sm_rebuild.inl), its still parts taken from its real
        // frame when it was made at that size (lf_fg_still.inl), or plainly
        // scaled when neither is there.
        // All end in ShowBlit (round 31): a copy when the back buffer is the
        // picture's size, scaled when it is the whole screen under a window
        // of its own size (lf_window_fill.inl) - a plain copy between two
        // sizes is not a thing the card does.
        ID3D12Resource *target = g_fg_scale.display ? g_fg_scale.display.get() : g_fg_still.display.get();
        FgReal *real = showing ? showing->real.get() : nullptr;
        const bool fits = target && real && real->full && target->GetDesc().Width == real->full->GetDesc().Width &&
                          target->GetDesc().Height == real->full->GetDesc().Height;
        const bool mapped = showing && showing->protect && showing->still && g_fg_still.ready;
        bool through_target = true, direct = false, read_back = false;
        if (synthetic && fits && real->low && g_rebuild.ready) {
            FgReal *before = showing->before && showing->before->low ? showing->before.get() : real;
            // Straight onto the back buffer when it is the picture's own size
            // and nothing is done to the picture on its way there. While a
            // test reads the frames back (LF_FG_DUMP) they are built beside
            // it as before and read from there - or, with SM_DUMP_DIRECT=1,
            // drawn straight onto it all the same and read back from it: the
            // two must give the same picture.
            static const bool dumps = [] { char v[4] = {}; return GetEnvironmentVariableA("LF_FG_DUMP", v, sizeof(v)) != 0; }();
            static const bool dump_direct = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_DUMP_DIRECT", v, sizeof(v)) > 0 && v[0] == '1'; }();
            const auto bd = bb->GetDesc();
            const auto td = target->GetDesc();
            // (Nor while it is sharpened on its way: that is done from a picture of its own, ShowBlit.)
            // (Nor while it is shared as a Spout sender: that is taken from where ShowBlit puts it.)
            direct = (!dumps || dump_direct) && !lfShow.upscaling && !SharpenInBlit() && !SpoutWanted() && bd.Width == td.Width && bd.Height == td.Height &&
                     bd.Format == td.Format && RebuildDrawReady(bd.Format);
            read_back = direct && dumps;
            RebuildShow(list.get(), source, real->full.get(), real->low.get(), before->full.get(), before->low.get(),
                        mapped ? showing->still.get() : nullptr, target, phase, made_phase, showing->prior, showing->sequence, &marks,
                        direct ? bb.get() : nullptr, alloc_at);
            if (mapped) ++still_frames;
            if (direct) through_target = false;
        } else if (synthetic && fits && mapped && !real->low) {
            FgStillShow(list.get(), source, real->full.get(), showing->still.get(), target);
            ++still_frames;
        } else if (synthetic && g_fg_scale.display) {
            target = g_fg_scale.display.get();
            FgScaleRecord(list.get(), source, D3D12_RESOURCE_STATE_COPY_SOURCE, target, true);
        } else through_target = false;
        if (through_target) {
            auto ready = Barrier(target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &ready);
            ShowBlit(list.get(), bb.get(), target);
            auto rest = Barrier(target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &rest);
        } else if (!direct) ShowBlit(list.get(), bb.get(), source);
        shown_from = through_target ? target : direct ? (read_back ? bb.get() : nullptr) : source;
        shown_state = through_target ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : direct || raw ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_COPY_SOURCE;
        if (raw)
        {
            auto back = Barrier(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
            list->ResourceBarrier(1, &back);
        }
        const D3D12_RESOURCE_STATES bb_state = D3D12_RESOURCE_STATE_COPY_DEST;
        // (LF_SHOW_PROOF_AFTER=1: the proof is taken with the counter and the banner on it - a test of those.)
        static const bool proof_after = [] { char v[4] = {}; return GetEnvironmentVariableA("LF_SHOW_PROOF_AFTER", v, sizeof(v)) > 0 && v[0] == '1'; }();
        if (!proof_after) ShowProof(list.get(), bb.get(), !synthetic);
        CounterStamp(list.get(), bb.get());
        BannerStamp(list.get(), bb.get());
        if (proof_after) ShowProof(list.get(), bb.get(), !synthetic);
        auto post = Barrier(bb.get(), bb_state, D3D12_RESOURCE_STATE_PRESENT);
        list->ResourceBarrier(1, &post);
        const bool parted = marks.next == kRebuildMarks - 1;       // every step of a rebuilt frame left its stamp
        if (stamps)
        {
            while (marks.next < kRebuildMarks) list->EndQuery(stamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, marks.next++);
            list->ResolveQueryData(stamps.get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, kRebuildMarks, stamps_back.get(), 0);
        }
        // (A frame read back - for its cost, or by a test - is waited for.)
        if (!send(!quick || stamps)) return false;
        if (stamps)
        {
            UINT64 *at = nullptr;
            D3D12_RANGE read = {0, 8 * kRebuildMarks}, none = {0, 0};
            if (SUCCEEDED(stamps_back->Map(0, &read, reinterpret_cast<void **>(&at))))
            {
                const UINT last = kRebuildMarks - 1;
                if (at[last] >= at[0]) { draw_cost[synthetic ? 1 : 0] += double(at[last] - at[0]) * 1000.0 / double(stamp_rate); ++draw_count[synthetic ? 1 : 0]; }
                if (parted && synthetic)
                {
                    for (UINT i = 0; i < last; ++i)
                        if (at[i + 1] >= at[i]) part_cost[i] += double(at[i + 1] - at[i]) * 1000.0 / double(stamp_rate);
                    ++part_frames;
                    if (marks.pair_made) ++part_pairs;
                }
                stamps_back->Unmap(0, &none);
            }
        }
        return true;
    };
    // Round 38, "waves": how far behind the game's world each frame is at the moment the screen showed it (the
    // swap chain's own word, after each Present: the last one it showed and when) - the same in smooth motion; a
    // swing over a fraction of a second is the picture speeding up and slowing down. Its slow part (each against
    // the mean of the frames within a quarter of a second about it) and quick part, said every five seconds in
    // the log: what a player's own screen and game make of the pacing, for the pacing to be put right by.
    struct WaveNote { UINT id; double content; };
    WaveNote wave_sent[64] = {};
    unsigned wave_next = 0;
    std::vector<std::pair<double, double>> wave_seen;      // (on the screen, behind the world)
    UINT wave_last_shown = 0;
    ULONGLONG wave_told = GetTickCount64();
    LARGE_INTEGER wave_rate = {};
    QueryPerformanceFrequency(&wave_rate);
    auto wave = [&](double content) {
        if (!lfShow.chain || content <= 0.0) return;
        UINT id = 0;
        lfShow.chain->GetLastPresentCount(&id);
        wave_sent[wave_next++ % 64] = { id, content };
        DXGI_FRAME_STATISTICS st = {};
        if (SUCCEEDED(lfShow.chain->GetFrameStatistics(&st)) && st.PresentCount != wave_last_shown)
        {
            wave_last_shown = st.PresentCount;
            for (const WaveNote &w : wave_sent)
                if (w.id == st.PresentCount && w.content > 0.0)
                {
                    const double at = double(st.SyncQPCTime.QuadPart) / double(wave_rate.QuadPart);
                    if (wave_seen.size() < 4000) wave_seen.emplace_back(at, at - w.content);
                    break;
                }
        }
        if (GetTickCount64() - wave_told < 5000) return;
        wave_told = GetTickCount64();
        if (wave_seen.size() >= 50)
        {
            double mean = 0.0;
            for (const auto &s : wave_seen) mean += s.second;
            mean /= wave_seen.size();
            double slow = 0.0, quick = 0.0;
            size_t from = 0;
            for (size_t i = 0; i < wave_seen.size(); ++i)
            {
                while (wave_seen[i].first - wave_seen[from].first > 0.125) ++from;
                double sum = 0.0;
                size_t n = 0;
                for (size_t j = from; j < wave_seen.size() && wave_seen[j].first - wave_seen[i].first <= 0.125; ++j) { sum += wave_seen[j].second; ++n; }
                const double smooth = sum / n;
                slow += (smooth - mean) * (smooth - mean);
                quick += (wave_seen[i].second - smooth) * (wave_seen[i].second - smooth);
            }
            Trace("[waves] behind the game's world on the screen: %.1f ms on average; it swings %.2f ms slowly and %.2f ms quickly (%zu frames seen shown)",
                  mean * 1000.0, std::sqrt(slow / wave_seen.size()) * 1000.0, std::sqrt(quick / wave_seen.size()) * 1000.0, wave_seen.size());
        }
        wave_seen.clear();
    };
    auto flip = [&](bool synthetic, double source_time) {
        const HRESULT pr = ShowPresentNow(synthetic ? 0.0 : source_time);
        if (pr == DXGI_STATUS_MODE_CHANGED || pr == DXGI_STATUS_MODE_CHANGE_IN_PROGRESS)
        {
            // The mode changed under us. The present did not happen; treat
            // it as a benign skip - the pipeline's resize path rebuilds the
            // swapchain when the size follows, and the next frame presents
            // normally. Presenting into the old surface until then is what
            // froze the overlay black (v1.10-review H2).
            Trace("[dlssg] present reports a mode change - skipping a frame");
            lfShow.stale = true;
            return true;
        }
        if (pr == DXGI_ERROR_WAS_STILL_DRAWING) return true;
        if (pr == DXGI_STATUS_OCCLUDED)
        {
            // The window is hidden (minimised target): benign.
            return true;
        }
        if (FAILED(pr)) return false;
        ShowRevealOnce();
        ++shown;
        if (synthetic) ++generated;
        if (g_frames_file)
        {
            static double last = 0.0, usual = 0.0;
            const double now = FramesNow(), gap = last > 0.0 ? (now - last) * 1000.0 : 0.0;
            last = now;
            const bool hitch = usual > 0.0 && gap > 2.0 * usual && gap > 1500.0 / std::max(OutputRefresh(), 30.0);
            if (gap > 0.0 && gap < 250.0) usual = usual <= 0.0 ? gap : usual + 0.05 * (gap - usual);
            FramesLine("%10.4f  %7.2f ms  %s  %8llu  %7.2f ms%s", now, gap, synthetic ? "made" : "REAL", trace_sequence,
                       (LatencyNow() - trace_content) * 1000.0, hitch ? "   <<< HITCH" : "");
        }
        if (trace)
        {
            UINT id = 0;
            DXGI_FRAME_STATISTICS st = {};
            if (lfShow.chain) lfShow.chain->GetLastPresentCount(&id);
            static LARGE_INTEGER frequency = {};
            if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
            const bool known = lfShow.chain && SUCCEEDED(lfShow.chain->GetFrameStatistics(&st));
            fprintf(trace, "%.6f,%d,%.6f,%llu,%.6f,%.6f,%u,%u,%u,%.6f,%.4f,%u\n", LatencyNow(), synthetic ? 1 : 0, trace_content,
                    trace_sequence, trace_ready, source_time, synthetic ? 0u : trace_code, id, known ? st.PresentCount : 0u,
                    known ? double(st.SyncQPCTime.QuadPart) / double(frequency.QuadPart) : 0.0, trace_phase, trace_code);
        }
        return true;
    };
    // Whatever holds the presenter for a fifth of a second is said, with where it stood.
    auto stood = [&](const char *where, double since) {
        const double took = LatencyNow() - since;
        if (took < 0.2) return;
        Trace("[stall] the presenter stood %.0f ms in %s", took * 1000.0, where);
    };
    // Pairs sent without a wait (quick) are let go once the card is through
    // with them - their pictures are not handed back to the generator under a
    // list that still reads them - and not waited for before the next pair is
    // begun: at 100 pairs a second that wait, the card finishing six
    // drawings, came out of the next pair's first frames, which were then
    // late and passed over.
    std::vector<std::pair<FgSlot *, UINT64>> held_pairs;
    auto release = [&](bool wait) {
        while (!held_pairs.empty())
        {
            if (fence->GetCompletedValue() < held_pairs.front().second)
            {
                if (!wait) return;
                FgAwaitPatient(fence.get(), held_pairs.front().second);
            }
            std::lock_guard<std::mutex> lock(g_fg.mutex);
            FgSlot *done = held_pairs.front().first;
            done->state = 0;
            done->real.reset(); done->before.reset();
            held_pairs.erase(held_pairs.begin());
        }
    };
    while (!g_fg.stop && !g_fg.failed)
    {
        const double round_began = LatencyNow();
        release(false);
        if (!held_pairs.empty())
        {
            // Nothing waits to be shown: then they are waited for, so the producer finds room.
            bool waiting = false;
            {
                std::lock_guard<std::mutex> lock(g_fg.mutex);
                for (auto &s : g_fg.slots) if (s.state == 2) waiting = true;
            }
            if (!waiting) release(true);
        }
        stood("letting a pair's pictures go (waiting for the card)", round_began);
        FgSlot *chosen = nullptr;
        {
            std::unique_lock<std::mutex> lock(g_fg.mutex);
            const bool ready = g_fg.wake.wait_for(lock, std::chrono::milliseconds(250), [] {
                if (g_fg.stop) return true;
                for (auto &s : g_fg.slots) if (s.state == 2) return true;
                return false;
            });
            if (g_fg.stop) break;
            if (!ready) { lock.unlock(); reportCadence(); continue; }
            // In order (lf_fg_pace.inl): the oldest pair first. Only a pair
            // whose real frame's time has already gone by while a newer one
            // waits is passed over - the presenter fell a whole pair behind.
            unsigned waiting = 0;
            for (auto &s : g_fg.slots)
                if (s.state == 2) { ++waiting; if (!chosen || s.sequence < chosen->sequence) chosen = &s; }
            while (waiting > 1 && pace.delay > 0.0 && chosen->interpolate &&
                   LatencyNow() > chosen->moment + pace.delay)
            {
                chosen->state = 0;
                chosen->real.reset(); chosen->before.reset();
                ++pace.dropped;
                ++dropped_pairs;
                --waiting;
                chosen = nullptr;
                for (auto &s : g_fg.slots)
                    if (s.state == 2 && (!chosen || s.sequence < chosen->sequence)) chosen = &s;
            }
            chosen->state = 3;
        }
        if (g_fg_starved.exchange(false) && pace.delay > 0.0)
        {
            static ULONGLONG told = 0;
            if (GetTickCount64() - told >= 5000)
            {
                told = GetTickCount64();
                Trace("[dlssg] the pairs waiting to be shown left no room for a new frame (shown %.1f ms after their moment) - "
                      "the delay is learnt anew", pace.delay * 1000.0);
            }
            pace.Forget();
            ++relearnt;
            quiet_seconds = 0;
        }
        trace_sequence = chosen->sequence;
        trace_ready = chosen->ready_at;
        trace_code = chosen->code;
        // Every picture at its moment + the delay. NGX already produced this
        // complete pair; a queued next pair never cancels its remaining
        // phases (x4 otherwise often became x3). Sequence gaps in
        // presentation do not invalidate NGX's input history.
        const double span = chosen->moment - chosen->prev_moment;
        const double step = span / (chosen->count + 1);
        showing = chosen;
        if (chosen->interpolate) { given += chosen->count; pair_count = chosen->count; ++given_pairs; }
        // A pair whose pictures a test asked for (LF_FG_DUMP) is drawn whole, late or not:
        // writing them out is what makes it late.
        static const bool dumping = [] { char v[4] = {}; return GetEnvironmentVariableA("LF_FG_DUMP", v, sizeof(v)) != 0 || GetLastError() == ERROR_INSUFFICIENT_BUFFER; }();
        const bool dumped = dumping && FgDumpWanted(chosen->sequence - 1);
        const bool irregular = !dumped && chosen->interpolate && span > 0.0 && pace.Irregular(span);
        // Does the screen have a refresh for each of the pair's frames?
        const bool fits = span <= 0.0 || (chosen->count + 1) <= 1.02 * span * OutputRefresh();
        bool scheduled = chosen->interpolate && span > 0.0 && !irregular;
        pace.cover_all = chosen->ad_on && chosen->interpolate;
        quick = chosen->interpolate && step > 0.0 && step < 0.0035 && !dumped;
        if (!chosen->interpolate) pace.Forget();
        after_mark = chosen->mark;
        after_taken = chosen->taken;
        // What the pair asks of the delay is known once its first picture is
        // drawn: from when its own work was done, and what drawing took then.
        bool first = true;
        // Round 46: a pair that comes late (a frame of the game later than usual - GTA V's came 11 to 40 ms apart, 24 on
        // average) had its late frames shown at once, one or two milliseconds apart, and the rest at their times: the
        // picture stood 8 to 14 ms and then leapt - a user's "slow motion for a moment, and back, again and again"
        // (frames.log: one a second). Now the lateness of its first frame shown is spread over the rest of the pair: each
        // frame after it is that much less late, the real frame on time - the motion goes on evenly instead of leaping.
        double slip = 0.0, slip_from = 0.0;
        auto slipped = [&](double phase) { return slip > 0.0 && slip_from < 1.0 ? slip * std::max(0.0, (1.0 - phase) / (1.0 - slip_from)) : 0.0; };
        // (first_at: the moment of the pair's first picture shown - Adaptive MFG's first beat, which may lie anywhere in
        // the pair, round 56: counted as one step in, the delay came out short by the rest and a pair in eight was
        // late, its frames two at once and then a hole of 22 ms; a fixed count's first is one step in, as before.)
        auto schedule = [&](double began, double drawn, double first_at) {
            const double done_told = chosen->done_at.load();
            const double done = done_told > 0.0 ? std::min(done_told, drawn) : std::max(began, drawn - pace.lead[chosen->count ? 1 : 0]);
            pace.Took(drawn - std::max(began, done), chosen->count != 0);
            if (!scheduled) return;
            const double first_step = first_at > 0.0 ? std::max(0.0, first_at - chosen->prev_moment) : step;
            const double need = done - chosen->prev_moment - first_step + pace.lead[chosen->count ? 1 : 0] + pace.call + 0.0003;
            // A stall (the game loading, the card taken away for a moment):
            // shown as it comes, and not taken for the delay to keep.
            if (pace.delay > 0.0 && need > pace.delay + std::max(0.05, 2.0 * span)) { scheduled = false; ++pace.late; ++pace.pairs; }
            else pace.Took(need, step);
            // SM_DIAG=1: the first pairs' needs, and the delay they leave.
            static const bool diag = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_DIAG", v, sizeof(v)) > 0 && v[0] == '1'; }();
            if (diag && pace.count <= 16)
                Trace("[diag] pair %llu: span %.1f ms, done %.1f ms after the frame before, drawn %.1f ms later, need %.1f ms -> delay %.1f ms%s",
                      chosen->sequence, span * 1000.0, (done - chosen->prev_moment) * 1000.0, (drawn - done) * 1000.0, need * 1000.0,
                      pace.delay * 1000.0, scheduled ? "" : " (not scheduled)");
        };
        // (Adaptive MFG: the place between the two real frames of the beat being shown; -1 none.)
        double ad_phase_now = -1.0;
        auto show = [&](ID3D12Resource *picture, bool synthetic, double moment, double source_time, unsigned index) {
            const double began = LatencyNow();
            const float phase = ad_phase_now >= 0.0 ? float(ad_phase_now) : synthetic ? float(index + 1) / float(chosen->count + 1) : 1.0f;
            // The generator's frame it goes by: its own, or the nearest there is.
            float made_phase = phase;
            // (Adaptive MFG with DLSS MFG: the generator's frame nearest the beat, or the real frame after.)
            if (synthetic && ad_phase_now >= 0.0 && !MsmSelected() && chosen->made && chosen->real && chosen->real->low)
            {
                int by = -1;
                double least = 1.0 - ad_phase_now;
                for (unsigned g = 0; g < chosen->made; ++g)
                {
                    const double off = std::abs(double(g + 1) / (chosen->made + 1) - ad_phase_now);
                    if (off < least) { least = off; by = static_cast<int>(g); }
                }
                picture = by < 0 ? chosen->real->low.get() : chosen->interpolated[by].get();
                made_phase = by < 0 ? 1.0f : float(by + 1) / float(chosen->made + 1);
            }
            else if (synthetic && chosen->made && chosen->made != chosen->count && chosen->real && chosen->real->low)
            {
                const int by = FgMadeFor(index, chosen->count, chosen->made);
                picture = by < 0 ? chosen->real->low.get() : chosen->interpolated[by].get();
                made_phase = by < 0 ? 1.0f : float(by + 1) / float(chosen->made + 1);
            }
            // XeSS MFG says at once when a pair has no frames in between (xe_mfg.inl, XeMake): there is nothing
            // of the library's to draw then.
            if (synthetic && XeSelected() && chosen->denied.load()) { ++denied_frames; return; }
            const bool raw = synthetic && XeSelected() && !(chosen->real && picture == chosen->real->low.get());
            if (!draw(picture, synthetic, phase, made_phase, raw)) { g_fg.failed = true; return; }
            if (synthetic) dump(chosen->sequence - 1, index + 1);
            const double drawn = LatencyNow();
            stood("drawing a frame (waiting for the card)", began);
            const bool led = first;
            if (first) { first = false; schedule(began, drawn, ad_phase_now >= 0.0 ? moment : 0.0); }
            // The generator said, once its work was done, that this pair has
            // no frames in between to show: the next picture is drawn over it.
            if (synthetic && chosen->denied.load()) { ++denied_frames; return; }
            // A frame's moment far ahead of now is no moment to wait for (round 11: a session's log showed the
            // presenter standing for two seconds while frames kept being taken - "very bad lag" - and nothing in
            // it that said where). It is shown now, and the delay learnt anew.
            if (scheduled)
            {
                // The first frame of the pair shown late: its lateness is what the rest of the pair is let slip (no
                // more than the pair is long - beyond that it is a stall, shown as it comes).
                const double phase_now = ad_phase_now >= 0.0 ? ad_phase_now : synthetic ? double(index + 1) / double(chosen->count + 1) : 1.0;
                if (led && synthetic)
                {
                    const double late_by = LatencyNow() - (moment + pace.delay - pace.call);
                    if (late_by > 0.0005 && late_by < span) { slip = late_by; slip_from = phase_now; }
                }
                const double when = moment + pace.delay - pace.call + slipped(phase_now), ahead = when - LatencyNow();
                if (ahead > 0.25)
                {
                    static ULONGLONG told = 0;
                    if (GetTickCount64() - told >= 5000)
                    {
                        told = GetTickCount64();
                        Trace("[stall] a frame's moment lay %.0f ms ahead (delay %.1f ms, pair %llu) - shown now, the delay is learnt anew",
                              ahead * 1000.0, pace.delay * 1000.0, chosen->sequence);
                    }
                    pace.Forget();
                    scheduled = false;
                }
                else if (!waitUntil(when)) return;
            }
            const double asked = LatencyNow();
            // A pair with more frames than the screen has refreshes for it
            // (the screen filled with a whole count, or "Limit to refresh
            // rate" off): none waits for the screen to take the one before -
            // the screen shows the newest at each refresh.
            // (VSync: every frame has a turn of the screen's to wait for.)
            if (lfShow.vrr || SyncEvery() > 0 || (smOpt.screen_limit.load() != 0 && fits)) waitForPresent();
            if (g_fg.stop) return;
            trace_content = moment;
            trace_phase = phase;
            const double called = LatencyNow();
            if (!flip(synthetic, source_time)) g_fg.failed = true;
            else if (synthetic) { ++made_shown; ++given_shown; }
            wave(moment);
            stood("the Present", called);
            pace.Took(led ? -1.0 : drawn - began, called - asked, LatencyNow() - called, synthetic);
        };
        if (irregular) { pace.skipped += chosen->count; odd_frames += chosen->count; }
        // Adaptive MFG: each beat at its own moment - a frame in between made for it, or a real frame near it.
        if (chosen->ad_on && chosen->interpolate && !irregular)
        {
            const double beat_fps = AdaptiveTarget();
            const double beat = beat_fps > 0.0 ? 1.0 / beat_fps : step;
            CounterGameFrame(chosen->source_time);
            for (unsigned b = 0; b < chosen->adaptive && !g_fg.stop && !g_fg.failed; ++b)
            {
                const signed char what = chosen->ad_what[b];
                const double when = chosen->ad_when[b];
                // (DLSS MFG: any of its frames - show() takes the one nearest the beat.)
                ID3D12Resource *picture = what >= 0 ? chosen->interpolated[MsmSelected() ? what : 0].get() : what == -1 ? chosen->real->full.get() :
                                          chosen->before ? chosen->before->full.get() : nullptr;
                if (!picture) continue;
                if (!dumped && LatencyNow() > when + pace.delay + 0.75 * beat) { ++pace.skipped; ++late_frames; continue; }
                ad_phase_now = chosen->ad_phase[b];
                show(picture, what >= 0, when, what >= 0 ? 0.0 : chosen->source_time, what >= 0 ? static_cast<unsigned>(what) : 0u);
            }
            ad_phase_now = -1.0;
        }
        else if (chosen->interpolate && !irregular)
        {
            for (unsigned index = 0; index < chosen->count && !g_fg.stop && !g_fg.failed; ++index)
            {
                const double moment = chosen->prev_moment + step * (index + 1);
                // Too late to be told from the next one: shown now it would
                // only push that one back (the two-at-once of before).
                // (A quick pair's frames are a millisecond and a half apart: up to three of them behind, a frame
                // is still shown in its turn. Further behind it is passed over, and the count a pair follows what
                // reaches the screen - more real frames with fewer between them, not pairs dropped whole: measured,
                // 97 pairs with three each against 67 with five, the same 400 frames a second on a 200 Hz screen.)
                if (!dumped && LatencyNow() > moment + pace.delay + slipped(double(index + 1) / double(chosen->count + 1)) + (quick ? 3.0 : 0.75) * step) { ++pace.skipped; ++late_frames; continue; }
                show(chosen->interpolated[index].get(), true, moment, 0.0, index);
            }
        }
        if (!g_fg.stop && !g_fg.failed && !(chosen->ad_on && chosen->interpolate && !irregular))
            show(chosen->real->full.get(), false, chosen->moment, chosen->source_time, 0);
        showing = nullptr;
        after_mark = 0;
        after_taken = 0;
        // Nothing of the pair is let go under a list still on the card.
        if (quick && value && fence->GetCompletedValue() < value) held_pairs.emplace_back(chosen, value);
        else
        {
            std::lock_guard<std::mutex> lock(g_fg.mutex);
            chosen->state = 0;
            chosen->real.reset(); chosen->before.reset();
        }
        quick = false;
        reportCadence();
    }
    if (value && fence->GetCompletedValue() < value) FgAwait(fence.get(), value, 30000);
    release(true);
    CloseHandle(event);
    if (timer) CloseHandle(timer);
    if (trace) fclose(trace);
    if (g_fg.failed) Trace("[dlssg] unavailable: presenter failed; ordinary output retained");
}

// LF_NGX_LOG=1 (round 42): the generator library's lines about the driver's profile and presets into the engine's
// log, 2 all of them (lf_dlssg.inl hands this to the library when it starts it).
static int NgxLibraryLogWanted()
{
    char v[8] = {};
    const DWORD len = GetEnvironmentVariableA("LF_NGX_LOG", v, sizeof(v));
    return len > 0 && len < sizeof(v) ? (v[0] == '2' ? 2 : v[0] == '1' ? 1 : 0) : 0;
}
static void NVSDK_CONV NgxLibraryLog(const char *message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
{
    if (!message) return;
    char line[512] = {};
    strncpy_s(line, message, _TRUNCATE);
    for (size_t n = strlen(line); n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'); --n) line[n - 1] = 0;
    char lower[512] = {};
    for (size_t i = 0; line[i]; ++i) lower[i] = static_cast<char>(tolower(static_cast<unsigned char>(line[i])));
    if (NgxLibraryLogWanted() >= 2 || strstr(lower, "preset") || strstr(lower, "drs") || strstr(lower, "override"))
        Trace("[ngx] %s", line);
}

static NVSDK_NGX_Result FgInitSafe(NrInitFn init, const wchar_t *cache, NVSDK_NGX_Parameter *params)
{
    __try { return init(0x1000000ULL, cache, lfCore.device, NVSDK_NGX_Version_API, params); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NVSDK_NGX_Result_Fail; }
}
static NVSDK_NGX_Result FgCreateSafe(ID3D12GraphicsCommandList *list, NVSDK_NGX_Parameter *params, NVSDK_NGX_Handle **out)
{
    __try { return g_fg.create(list, NVSDK_NGX_Feature_FrameGeneration, params, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NVSDK_NGX_Result_Fail; }
}
static bool EnsureFg(Pipeline &v, DXGI_FORMAT format)
{
    if (NgxSelected() && !lfCore.ngx_ready) { Trace("[dlssg] unavailable: NGX is not initialized on this adapter"); return false; }
    const UINT full_w = v.stretched ? v.screen_w : v.work_w, full_h = v.stretched ? v.screen_h : v.work_h;
    // "Flow" in the app: frames in between are made at this share of the
    // picture and brought back to its size from the real frames around them
    // (sm_rebuild.inl); real frames keep the source's resolution. From 95 on
    // they are made at the picture's own size, as the generator leaves them.
    // Never under 504 lines: measured on an RTX 3060, the generator's first
    // frame in between costs 1.02 ms at 896x504 and more below it (1.72 at
    // 768x432, 1.29 at 640x360) - and 1.74 at 1280x720, 2.34 at 1920x1080,
    // 3.67 at 2560x1440. Sizes in whole eights of a pixel.
    // MSM FG: its frames are made at the picture's own size whatever "Flow" says - that is the size it looks
    // for the motion at (msm_fg.h).
    const double share = MsmSelected() ? 1.0 : std::clamp(FlowShare() / 100.0, 0.10, 1.0);
    const UINT least = std::min(full_h, 504u);
    const UINT lines = share >= 0.95 ? full_h : std::min(full_h, std::max(least, static_cast<UINT>(std::lround(full_h * share / 8.0)) * 8u));
    const double scale = lines < full_h ? double(lines) / full_h : 1.0;
    const UINT w = scale < 1 ? (UINT(std::round(full_w * scale / 8.0)) * 8u) : full_w;
    const UINT height = scale < 1 ? lines : full_h;
    if ((XeSelected() ? XeLive() : MsmSelected() ? MsmLive(full_w, full_h, format, FlowShare()) : g_fg.feature != nullptr) &&
        g_fg.w == w && g_fg.height == height && g_fg.mw == v.work_w &&
        g_fg.mh == v.work_h && g_fg.full_w == full_w && g_fg.full_h == full_h &&
        g_fg.format == format && g_fg.thread.joinable()) return true;
    CloseFgResources();
    // XeSS MFG: nothing of NGX. Its own swap chain is made further down
    // (XeReady), once the pictures it copies into are there.
    if (NgxSelected() && !g_fg.module)
    {
        // The BYO library folder next to the worker, then the worker's own
        // directory: users drop nvngx_dlssg.dll into native\libraries\ to
        // pick the build they want, and native\ stays the bundled fallback.
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (auto slash = wcsrchr(path, L'\\')) *(slash + 1) = 0;
        wchar_t directory[MAX_PATH]; wcscpy_s(directory, path);
        if (!FgPrepareCompatibility(directory)) return false;
        wchar_t libraries[MAX_PATH]; wcscpy_s(libraries, directory);
        wcscat_s(libraries, L"libraries\\");
        wchar_t lib_path[MAX_PATH]; wcscpy_s(lib_path, libraries);
        wcscat_s(lib_path, L"nvngx_dlssg.dll");
        if (GetFileAttributesW(lib_path) != INVALID_FILE_ATTRIBUTES)
        {
            // The BYO file is verified (NVIDIA signature, machine-root
            // chain, product name) before it is mapped - a writable folder
            // next to the executable is otherwise the easiest DLL plant.
            if (TrustUserDll(lib_path, "nvngx_dlssg.dll"))
            {
                g_fg.module = LoadLibraryW(lib_path);
                if (g_fg.module)
                    Trace("[dlssg] FG runtime from native\\libraries\\ (BYO, "
                        "verified NVIDIA signature)");
            }
            else
                Trace("[dlssg] BYO refused: nvngx_dlssg.dll is not a "
                    "NVIDIA-signed runtime - falling back to the bundled "
                    "copy (%ls)", lib_path);
        }
        if (!g_fg.module)
        {
            wcscpy_s(path, directory);
            wcscat_s(path, L"nvngx_dlssg.dll");
            g_fg.module = LoadLibraryW(path);
        }
        if (!g_fg.module) { Trace("[dlssg] unavailable: nvngx_dlssg.dll load failed (Windows %lu)", GetLastError()); return false; }
        if (FgComputeCapability() == 89) {
            auto base = reinterpret_cast<BYTE *>(g_fg.module);
            auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
            auto nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
            const auto patched = lf_ada::Apply(base, nt->OptionalHeader.SizeOfImage, 89);
            g_fg_runtime_max = patched.applied ? 5u : 1u;
            g_fg_count = std::min(g_fg_count, g_fg_runtime_max);
            Trace("[dlssg] Ada temporal kernels: %u containers; %s", patched.containers,
                  patched.applied ? "MFG up to 6x enabled" : "unrecognized runtime, limited to 2x");
        }
    }
    if (NgxSelected() && !g_fg.initialized)
    {
        auto init = reinterpret_cast<NrInitFn>(GetProcAddress(g_fg.module, "NVSDK_NGX_D3D12_Init_Ext"));
        g_fg.create = reinterpret_cast<NrCreateFn>(GetProcAddress(g_fg.module, "NVSDK_NGX_D3D12_CreateFeature"));
        g_fg.release = reinterpret_cast<NrReleaseFn>(GetProcAddress(g_fg.module, "NVSDK_NGX_D3D12_ReleaseFeature"));
        g_fg_evaluate = reinterpret_cast<NrEvaluateFn>(GetProcAddress(g_fg.module, "NVSDK_NGX_D3D12_EvaluateFeature"));
        if (!init || !g_fg.create || !g_fg.release || !g_fg_evaluate) return false;
        if (!g_fg.params && NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&g_fg.params))) return false;
        wchar_t cache[MAX_PATH] = {};
        EngineDataPath(cache);
        // LF_NGX_LOG=1 (round 42): the library's own lines about the driver's profile and the preset it applied go
        // into the engine's log ("ReportOverrideStates: render preset (requested=, effective=)"); 2: all its lines.
        if (NgxLibraryLogWanted() > 0)
        {
            g_fg.params->Set("Log.Callback", reinterpret_cast<void *>(&NgxLibraryLog));
            g_fg.params->Set("Minimum.Logging.Level", static_cast<unsigned>(NVSDK_NGX_LOGGING_LEVEL_VERBOSE));
        }
        const auto result = FgInitSafe(init, cache, g_fg.params);
        Trace("[dlssg] Init_Ext -> 0x%08X", result);
        if (NVSDK_NGX_FAILED(result)) { Trace("[dlssg] unavailable: runtime initialization 0x%08X", result); return false; }
        g_fg.initialized = true;
    }
    if (NgxSelected())
    {
        auto p = g_fg.params;
        p->Reset();
        p->Set("CreationNodeMask", 1u); p->Set("VisibilityNodeMask", 1u);
        p->Set("Width", w); p->Set("Height", height);
        p->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, (unsigned)format);
        p->Set(NVSDK_NGX_DLSSG_Parameter_InternalWidth, v.work_w);
        p->Set(NVSDK_NGX_DLSSG_Parameter_InternalHeight, v.work_h);
        p->Set(NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 0u);
        if (!CmdOpen()) return false;
        const auto result = FgCreateSafe(lfCore.cmd, p, &g_fg.feature);
        if (!FgAwait(lfCore.timeline, CmdSubmit(), 30000) || NVSDK_NGX_FAILED(result) || !g_fg.feature)
        { Trace("[dlssg] unavailable: CreateFeature 0x%08X (adapter/driver/runtime support)", result); return false; }
    }
    if (!StagedCreate(g_fg.depth, v.work_w, v.work_h, DXGI_FORMAT_R32_FLOAT, v.work_w * 4)) return false;
    std::vector<float> depth(static_cast<size_t>(v.work_w) * v.work_h, 0.5f);
    if (!StagedFill(g_fg.depth, reinterpret_cast<BYTE *>(depth.data()), v.work_w * 4, v.work_h) || !CmdOpen()) return false;
    StagedCopy(lfCore.cmd, g_fg.depth);
    auto b = Barrier(g_fg.depth.gpu_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    lfCore.cmd->ResourceBarrier(1, &b);
    if (!FgAwait(lfCore.timeline, CmdSubmit(), 30000)) return false;
    for (unsigned i = 0; i < g_fg_count; ++i)
    {
        g_fg.output[i].attach(NewTexture(w, height, format, true));
        if (!g_fg.output[i]) return false;
    }
    for (auto &slot : g_fg.slots)
        for (unsigned i = 0; i < g_fg_count; ++i)
        {
            slot.interpolated[i].attach(NewTexture(w, height, format, true));
            if (!slot.interpolated[i]) return false;
        }
    // The real frames: at the picture's size, and small when frames in
    // between are made smaller (the generator's input, and what a frame in
    // between is compared with when it is rebuilt).
    for (auto &real : g_fg.reals)
    {
        real = std::make_shared<FgReal>();
        real->full.attach(NewTexture(full_w, full_h, format, false));
        if (scale < 1) real->low.attach(NewTexture(w, height, format, true));
        if (!real->full || (scale < 1 && !real->low)) return false;
    }
    if (!CmdOpen()) return false;
    for (unsigned i = 0; i < g_fg_count; ++i)
    {
        auto to_copy = Barrier(g_fg.output[i].get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        lfCore.cmd->ResourceBarrier(1, &to_copy);
    }
    for (auto &slot : g_fg.slots) for (unsigned i = 0; i < g_fg_count; ++i)
    {
        auto to_copy = Barrier(slot.interpolated[i].get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        lfCore.cmd->ResourceBarrier(1, &to_copy);
    }
    for (auto &real : g_fg.reals)
    {
        auto to_copy = Barrier(real->full.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        lfCore.cmd->ResourceBarrier(1, &to_copy);
        if (real->low)
        {
            auto to_read = Barrier(real->low.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            lfCore.cmd->ResourceBarrier(1, &to_read);
        }
    }
    if (!FgAwait(lfCore.timeline, CmdSubmit(), 30000)) return false;
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 20; buffer.Height = 1; buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1; buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap = {}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(g_fg.disable.put())))) return false;
    heap.Type = D3D12_HEAP_TYPE_READBACK; buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(g_fg.disable_readback.put())))) return false;
    if (scale < 1) {
        if (!FgScaleReady(full_w, full_h, format)) return false;
        if (!CmdOpen()) return false;
        auto display_write = Barrier(g_fg_scale.display.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        lfCore.cmd->ResourceBarrier(1, &display_write);
        if (!FgAwait(lfCore.timeline, CmdSubmit(), 30000)) return false;
        // Small frames in between get their detail back from the real frames
        // (sm_rebuild.inl); without it they are scaled as they are.
        RebuildReady(w, height, full_w, full_h);
    }
    // What does not move stays put (lf_fg_still.inl): the two real frames are
    // compared at the size frames in between are made at, in tiles of about
    // 16 pixels of the picture. Not having it is not a failure of the
    // generator.
    FgStillReady(w, height, static_cast<UINT>(std::lround(16.0 * scale)), full_w, full_h, v.work_w, v.work_h, format,
                 !g_fg_scale.display);
    // XeSS MFG: the library's swap chain at the size frames in between are made at.
    if (XeSelected() && !XeReady(w, height, format)) return false;
    // MSM FG: its passes, and what they work in for pictures of this size.
    if (MsmSelected() && !MsmReady(full_w, full_h, format, FlowShare())) return false;
    g_fg.w = w; g_fg.height = height; g_fg.full_w = full_w; g_fg.full_h = full_h;
    g_fg.mw = v.work_w; g_fg.mh = v.work_h; g_fg.format = format;
    g_fg.stop = false;
    // R11 refined: latency 1 belongs to the FG presenter while it owns the
    // present loop. The ordinary NR path presents Present(0,0) per frame and
    // never consumes the waitable - with latency 1 the swapchain would hold
    // a single queued present and every ordinary present would block or drop
    // against the compositor (the fullscreen flicker). The waitable handle
    // is taken here, from the same thread that will wait on it.
    if (lfShow.chain != nullptr)
    {
        IDXGISwapChain2 *sc2 = nullptr;
        if (SUCCEEDED(lfShow.chain->QueryInterface(
                __uuidof(IDXGISwapChain2), reinterpret_cast<void **>(&sc2)))
            && sc2 != nullptr)
        {
            sc2->SetMaximumFrameLatency(1);
            g_fg_waitable = lfShow.quick_waitable ? lfShow.quick_waitable : sc2->GetFrameLatencyWaitableObject();
            sc2->Release();
            Trace("[dlssg] the presenter is paced by the compositor (latency 1)");
        }
    }
    g_fg.captured = 0;
    g_fg.thread = std::thread(FgPresenter);
    FgDoneWatchStart();
    Trace("[dlssg] %s %ux enabled at %ux%u, format=%u; flat depth, %s", GeneratorName(), g_fg_count + 1, w, height, format,
          MotionGiven() ? "the engine's motion field" : "the motion left to the generator");
    return true;
}

// Opt-in pixel regression capture, never enabled in ordinary runs.
static void FgDump(ID3D12Resource *source, D3D12_RESOURCE_STATES state, unsigned index)
{
    char dir[MAX_PATH] = {};
    // A few pairs, each with the real frame before it (testsjelly.cpp).
    auto wanted = [](UINT64 n) { return n == 30 || n == 90 || n == 150 || n == 300; };
    if (!GetEnvironmentVariableA("LF_FG_DUMP", dir, sizeof(dir)) ||
        (!wanted(g_fg.sequence) && !(wanted(g_fg.sequence + 1) && index == 0))) return;
    auto desc = source->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT rows; UINT64 rowbytes, bytes;
    lfCore.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowbytes, &bytes);
    D3D12_HEAP_PROPERTIES heap = {}; heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes; bd.Height = 1; bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    winrt::com_ptr<ID3D12Resource> rb;
    if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(rb.put()))) || !CmdOpen()) return;
    auto pre = Barrier(source, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) lfCore.cmd->ResourceBarrier(1, &pre);
    D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
    from.pResource = source; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = rb.get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = fp;
    lfCore.cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    auto post = Barrier(source, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) lfCore.cmd->ResourceBarrier(1, &post);
    if (!FgAwait(lfCore.timeline, CmdSubmit(), 30000)) return;
    BYTE *data = nullptr; D3D12_RANGE read = {0, static_cast<SIZE_T>(bytes)}, written = {0, 0};
    if (FAILED(rb->Map(0, &read, reinterpret_cast<void **>(&data)))) return;
    char path[MAX_PATH]; sprintf_s(path, "%s/fg-%llu-%u.raw", dir, g_fg.sequence, index);
    FILE *file = nullptr;
    if (!fopen_s(&file, path, "wb") && file) {
        for (UINT y = 0; y < rows; ++y) fwrite(data + size_t(y) * fp.Footprint.RowPitch, 1, size_t(rowbytes), file);
        fclose(file);
    }
    rb->Unmap(0, &written);
}

// SM_DIAG=1: what one pair costs on the card, step by step - time stamps
// taken inside the pair's own command list, read back after it has run.
static struct FgCost
{
    static constexpr UINT kMarks = 12;
    ID3D12QueryHeap *heap = nullptr;
    ID3D12Resource *readback = nullptr;
    UINT64 rate = 0;
    int on = -1;
    UINT used = 0;
    const char *labels[kMarks] = {};
    double sums[kMarks] = {};
    unsigned pairs = 0;
    ULONGLONG since = 0;
    // The same, over a longer while: told once ten seconds in and every five
    // minutes after, whatever the switches - what the generator costs on the
    // card this runs on is the one figure every other choice here was made
    // from, and it was only ever measured on an RTX 3060.
    int diag = 0;
    double long_sums[kMarks] = {};
    unsigned long_pairs = 0, told = 0;
    ULONGLONG long_since = 0;
} g_fg_cost;

// The time stamps are taken on every pair (they cost nothing to speak of);
// SM_DIAG=1 has them told every two seconds, step by step.
static bool FgCostOn()
{
    FgCost &c = g_fg_cost;
    if (c.on < 0)
    {
        char v[4] = {};
        c.diag = GetEnvironmentVariableA("SM_DIAG", v, sizeof(v)) > 0 && v[0] == '1' ? 1 : 0;
        c.on = 1;
        D3D12_QUERY_HEAP_DESC qd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, FgCost::kMarks, 0 };
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = sizeof(UINT64) * FgCost::kMarks; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(lfCore.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&c.heap))) ||
            FAILED(lfCore.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                          nullptr, IID_PPV_ARGS(&c.readback))) ||
            FAILED(lfCore.direct_queue->GetTimestampFrequency(&c.rate)) || c.rate == 0)
            c.on = 0;
    }
    return c.on == 1;
}

static bool FgCostDiag() { return FgCostOn() && g_fg_cost.diag == 1; }

static void FgCostMark(ID3D12GraphicsCommandList *list, const char *label)
{
    FgCost &c = g_fg_cost;
    if (!FgCostOn() || c.used >= FgCost::kMarks) return;
    c.labels[c.used] = label;
    list->EndQuery(c.heap, D3D12_QUERY_TYPE_TIMESTAMP, c.used++);
}

static void FgCostResolve(ID3D12GraphicsCommandList *list)
{
    FgCost &c = g_fg_cost;
    if (FgCostOn() && c.used > 1) list->ResolveQueryData(c.heap, D3D12_QUERY_TYPE_TIMESTAMP, 0, c.used, c.readback, 0);
}

// After the pair's commands have run.
static void FgCostRead()
{
    FgCost &c = g_fg_cost;
    if (!FgCostOn() || c.used < 2) { c.used = 0; return; }
    UINT64 *stamps = nullptr;
    D3D12_RANGE read = { 0, sizeof(UINT64) * c.used }, none = { 0, 0 };
    if (SUCCEEDED(c.readback->Map(0, &read, reinterpret_cast<void **>(&stamps))))
    {
        for (UINT i = 1; i < c.used; ++i)
            if (stamps[i] >= stamps[i - 1])
            {
                const double took = double(stamps[i] - stamps[i - 1]) * 1000.0 / double(c.rate);
                c.sums[i] += took;
                c.long_sums[i] += took;
            }
        c.readback->Unmap(0, &none);
        ++c.pairs;
        ++c.long_pairs;
    }
    const UINT used = c.used;
    c.used = 0;
    const ULONGLONG tick = GetTickCount64();
    if (c.since == 0) c.since = tick;
    if (c.long_since == 0) c.long_since = tick;
    auto told = [&](const double *sums, unsigned pairs, char *line, size_t room) {
        int at = 0;
        double total = 0.0;
        line[0] = 0;
        for (UINT i = 1; i < used; ++i)
        {
            at += _snprintf_s(line + at, room - at, _TRUNCATE, "%s %.2f, ", c.labels[i], sums[i] / pairs);
            total += sums[i] / pairs;
        }
        return total;
    };
    char line[512] = {};
    if (c.long_pairs >= 200 && tick - c.long_since >= (c.told ? 300000u : 10000u))
    {
        const double total = told(c.long_sums, c.long_pairs, line, sizeof(line));
        Trace("[card] CUDA SM%d: a pair's work on the card with frames in between made at %ux%u - %stotal %.2f ms (over %u pairs)",
              FgComputeCapability(), g_fg.w, g_fg.height, line, total, c.long_pairs);
        for (double &s : c.long_sums) s = 0.0;
        c.long_pairs = 0;
        c.long_since = tick;
        ++c.told;
    }
    if (tick - c.since < 2000 || c.pairs == 0) return;
    if (c.diag)
    {
        const double total = told(c.sums, c.pairs, line, sizeof(line));
        Trace("[cost] a pair on the card: %stotal %.2f ms (%u pairs in %.1f s)", line, total, c.pairs, (tick - c.since) / 1000.0);
    }
    for (double &s : c.sums) s = 0.0;
    c.pairs = 0;
    c.since = tick;
}

// Adaptive MFG's frames a second: the target given, or (1) the screen's refresh - a little under it, as a limit at
// the refresh is kept (no slack at all and every hiccup costs a frame). 0: off.
static double AdaptiveTarget()
{
    const int want = smOpt.adaptive.load();
    if (want <= 0) return 0.0;
    const double refresh = OutputRefresh();
    if (want == 1) return refresh >= 24.0 ? refresh * 0.97 : 0.0;
    return double(want);
}

// The beats that fall in this pair - from the real frame before (the frame clock's moment, not yet moved on to this
// one) to it, the game's pace `every` later: for each, its place between them and what is shown. The beat goes on
// from pair to pair; a pause or a cut starts it anew one beat after the frame before.
// (Round 56: `after`, the frame clock's moment for this frame, as it will be given - the pair's own length. Laid over
// the pace instead, a pair longer than it left beats no pair had (a hole of 17 to 40 ms, one pair in five on the
// fixture), a shorter one beats past its own real frame.)
static void AdaptiveLay(AdaptivePlan &p, double every, double after)
{
    const double fps = AdaptiveTarget();
    const double before = g_fg_clock.moment;
    const double span = after > before ? after - before : every > 0.0 ? every : g_fg_clock.every;
    if (fps <= 0.0 || before <= 0.0 || span <= 0.0) { g_adaptive_next = 0.0; return; }
    const double beat = 1.0 / fps, end = before + span;
    if (g_adaptive_next <= before - beat || g_adaptive_next > end + 4.0 * beat) g_adaptive_next = before + beat;
    while (g_adaptive_next <= end + 1e-6 && p.n < 8)
    {
        const float t = static_cast<float>((g_adaptive_next - before) / span);
        signed char what = -1;
        if (t <= kAdaptiveNear) what = -2;
        else if (t < 1.0f - kAdaptiveNear && p.made < 5) what = static_cast<signed char>(p.made++);
        p.when[p.n] = g_adaptive_next; p.phase[p.n] = t; p.what[p.n] = what; ++p.n;
        g_adaptive_next += beat;
    }
}

static bool FgPresent(Pipeline &v, ID3D12Resource *color, D3D12_RESOURCE_STATES state)
{
    if (!FgRequested()) { StopFgPresentation(); return false; }
    if (!EnsureFg(v, color->GetDesc().Format))
    { Trace("[dlssg] unavailable: the runtime could not create frame generation; ordinary output retained"); g_fg.failed = true; CloseFgResources(); return false; }
    const auto now = std::chrono::steady_clock::now();
    const double taken = LatencyNow();
    const double source = (lfCap.window_live || lfCap.live) ? lfCap.stamp : 0.0;
    if (g_fg.history && source > 0 && source == g_fg.source_time && !lfShow.history_restart) return true;
    const double interval = g_fg.history && source > g_fg.source_time && g_fg.source_time > 0
        ? source - g_fg.source_time : (g_fg.history ? std::chrono::duration<double>(now - g_fg.last).count() : 0.016);
    const bool interpolate = g_fg.history && !lfShow.history_restart && interval < 0.2;
    g_fg.last = now;
    const double source_before = g_fg.source_time;
    g_fg.source_time = source;
    ++g_fg.captured;
    // How many frames in between this pair gets. Every frame of the game is
    // shown (lf_fg_pace.inl says why they come first); the question is how
    // many go between two of them.
    //
    // While the screen is the limit: as many as fill it, a whole number, the
    // same for every pair - a game at 75 on a 200 Hz screen gets two (225
    // shown, the screen showing the newest at each refresh), never more than
    // the multiplier asks. Until round 4 it was exactly the room the screen
    // had left, one and two thirds there, the pairs taking turns: 2, 2, 1, 2,
    // 2, 1 ... - and a pair with one frame fewer among pairs with two is a
    // step of the picture twice as long as its neighbours: the motion "works,
    // then breaks up, then comes back". Near a whole number it was worst: a
    // game held at 100 on a 200 Hz screen had room for 0.94, so every
    // seventeenth pair went without. A count that changes only when the
    // game's own rate has moved for good is an even pace.
    //
    // With a limit of its own (Frame limit in the app, VRR's range, VSync's
    // one frame a turn) the frames shown must stay under it, and the room is
    // filled exactly as before.
    // "Limit to refresh rate" off in the app: the multiplier's own count,
    // whatever the screen shows.
    // (Round 45: VRR no longer puts the screen's limit on the count by itself - "Limit to refresh rate" does: lf_vrr.inl, VrrCap.)
    const bool to_screen = smOpt.screen_limit.load() != 0;
    const double refresh = OutputRefresh();
    double target = to_screen ? refresh : 0.0;
    // A little under the refresh rate: at exactly one frame a refresh there is
    // no slack at all, and every hiccup costs a frame.
    if (target >= 24.0) target *= 0.97;
    bool capped = false;
    // (Adaptive MFG: its target is the limit - with DLSS MFG and XeSS MFG filled as any limit; SMFG goes by the beat,
    // below.)
    if (const double beat_fps = AdaptiveTarget(); beat_fps > 0.0) { target = beat_fps; capped = true; }
    for (const int limit : { ShownCap(), PresentCap() })
        if (limit > 0) { target = target > 0 ? std::min(target, double(limit)) : double(limit); capped = true; }
    // The game's own pace, evened out: the capture's stamps lie on the
    // screen's refresh grid (a 130 FPS game on 200 Hz comes as 5, 10, 5, 10 ms).
    static double every = 0.0;
    static bool resting = false;
    static int off_pace = 0;         // frames in a row far above (+) or below (-) that pace
    if (!interpolate) { every = 0.0; off_pace = 0; g_fg.extra_budget = 0; }
    else if (every <= 0.0) every = interval;
    else
    {
        // One frame far off the pace (a hitch) moves it little; three in a
        // row the same way are another pace altogether (a limit set, a
        // heavier scene) and are taken at once.
        const int side = interval > every * 1.7 ? 1 : interval < every * 0.6 ? -1 : 0;
        off_pace = side == 0 ? 0 : (side > 0) == (off_pace > 0) ? off_pace + side : side;
        if (off_pace >= 3 || off_pace <= -3) { every = interval; off_pace = 0; }
        else every += 0.1 * (std::clamp(interval, every * 0.6, every * 1.7) - every);
    }
    // Game FPS given in the app: the frames in between a pair are counted from it, not from the frames' times.
    if (interpolate && GivenFrameTime() > 0.0) every = GivenFrameTime();
    unsigned count = g_fg_count;
    bool rest_now = false;
    double room = 0.0;
    static unsigned held = 0;           // frames in between a pair while the screen is the limit; 0: not chosen yet
    static int more = 0, fewer = 0;     // pairs in a row that asked for another count
    static int other_mind = 0;          // pairs in a row that would rest while frames are made, or the other way
    if (!interpolate) { held = 0; more = fewer = other_mind = 0; }
    if (source > 0 && target > 0 && every > 0.0)
    {
        room = target * every;                      // frames the screen shows for each frame of the game
        // A game that fills the screen by itself: its own frames are shown as
        // they are and NGX rests. With the screen as the only limit that is a
        // game at the screen's own rate, no sooner (until round 4 it was from
        // four fifths of it on: a game playing around 150 on a 200 Hz screen
        // had its frames in between come and go with every lighter scene -
        // and one asked for x4 that was shown nothing in between at 160 looks
        // broken, whatever the screen could have shown of it). Under a limit
        // of its own, where frames in between may only fill the room left:
        // when that room is under a quarter of a frame. Neither on the word
        // of one pair.
        const bool would_rest = capped ? room < (resting ? 1.4 : 1.25) : refresh * every < (resting ? 1.12 : 1.03);
        other_mind = would_rest != resting ? other_mind + 1 : 0;
        rest_now = other_mind >= 24 ? would_rest : resting;
        if (rest_now != resting) other_mind = 0;
        if (capped)
        {
            g_fg.extra_budget += std::clamp(room - 1.0, 0.0, double(g_fg_count));
            count = std::min(g_fg_count, static_cast<unsigned>(g_fg.extra_budget + 0.0001));
            g_fg.extra_budget = std::max(0.0, g_fg.extra_budget - count);
            held = 0;
        }
        else
        {
            // Frames a pair has to hold to fill the screen (its real frame
            // among them); a twentieth short still counts as filled. The
            // count goes up when the screen has been short for twenty pairs,
            // down when one fewer has filled it for sixty.
            const double fill = 0.95 * refresh * every;
            auto whole = [](double frames) {
                return std::clamp(static_cast<int>(std::ceil(frames)) - 1, 1, static_cast<int>(g_fg_count));
            };
            const unsigned up = static_cast<unsigned>(whole(fill - 0.06)), down = static_cast<unsigned>(whole(fill + 0.06));
            if (held == 0 || held > g_fg_count) { held = static_cast<unsigned>(whole(fill)); more = fewer = 0; }
            more = up > held ? more + 1 : 0;
            fewer = down < held ? fewer + 1 : 0;
            if (more >= 20) { held = up; more = fewer = 0; ++g_steady.changes; }
            else if (fewer >= 60) { held = down; more = fewer = 0; ++g_steady.changes; }
            count = held;
        }
        if (rest_now) { count = 0; g_fg.extra_budget = 0; }
    }
    else held = 0;
    {
        static ULONGLONG told = 0;
        static double told_room = -1.0;
        static unsigned told_count = ~0u;
        const ULONGLONG tick = GetTickCount64();
        const bool news = rest_now != resting || (!capped && !rest_now && count != told_count) ||
                          (capped && tick - told >= 5000 && std::fabs(room - told_room) > 0.15);
        if (room > 0.0 && tick - told >= 1000 && news)
        {
            told = tick; told_room = room; told_count = count;
            if (rest_now) Trace("[dlssg] the game draws %.0f frames a second and the screen shows %.0f: no room for frames in "
                                "between - its own frames are shown", 1.0 / every, target);
            else if (capped) Trace("[dlssg] the game draws %.0f frames a second and %.0f may be shown: %.2f frames in between each "
                                   "(x%u asked)", 1.0 / every, target, std::clamp(room - 1.0, 0.0, double(g_fg_count)), g_fg_count + 1);
            else Trace("[dlssg] the game draws %.0f frames a second and the screen shows %.0f: %u frames in between each, %.0f a "
                       "second (x%u asked)", 1.0 / every, refresh, count, (count + 1) / every, g_fg_count + 1);
        }
    }
    resting = rest_now;
    if (const unsigned cap = g_fg_count_cap.load()) count = std::min(count, cap);
    // More frames than the screen's refreshes (round 45, lf_vrr.inl, ShowPresentNow): a twentieth over to say so, a
    // twentieth under to say it no longer is - a game near the edge does not flip it pair by pair.
    {
        const double refresh = OutputRefresh();
        const bool before = g_present_above.load();
        const bool wanted = interpolate && !to_screen && every > 0.0 && refresh >= 24.0 &&
                           double(count + 1) / every > refresh * (before ? 0.95 : 1.05);
        // (Round 47: and only once it has been so a while - a second over the screen's rate, three under it. A game
        // near the edge, x4 of 40 to 50 frames on 200 Hz, went from one to the other every two seconds, and each
        // change of how frames are presented is felt.)
        static double other_since = 0.0;
        const double now_s = LatencyNow();
        if (wanted == before) other_since = 0.0;
        else if (other_since == 0.0) other_since = now_s;
        const bool above = wanted != before && other_since > 0.0 && now_s - other_since >= (wanted ? 1.0 : 3.0) ? wanted : before;
        if (above != before)
        {
            other_since = 0.0;
            g_present_above = above;
            Trace(above ? "[sync] %.0f frames a second asked of a %.0f Hz screen: the frames are presented the moment they are due (tear lines; Limit to refresh rate keeps them under it)"
                        : "[sync] %.0f frames a second fit the %.0f Hz screen: presented at its refreshes again", double(count + 1) / every, refresh);
        }
    }
    const bool idle = rest_now || g_fg_too_fast;
    if (idle) { count = 0; g_fg.extra_budget = 0; }
    // Adaptive MFG with SMFG: the frames in between this pair has are its beats' (AdaptiveLay).
    // (Round 56: and with DLSS MFG where its frames are rebuilt at the picture's size - each beat is rebuilt at its own
    // moment from the generator's frame nearest it (FgPresenter, show). Filled as a limit instead, its frames stood at
    // the pair's even places, 1 to 9 ms apart on the screen. XeSS MFG stays a limit: its library's own count rules.)
    AdaptivePlan plan;
    const bool beat_rebuilt = !MsmSelected() && !XeSelected() && g_rebuild.ready && g_rebuild.repairs && g_fg.w < g_fg.full_w;
    const bool by_beat = !idle && interpolate && (MsmSelected() || beat_rebuilt) && AdaptiveTarget() > 0.0;
    if (by_beat)
    {
        // (The moment the clock will give this frame below, read off a copy of it - nothing moves it before then.)
        FgClock probe = g_fg_clock;
        const bool chained = source > 0 && source_before > 0 && source > source_before;
        const double after = probe.Moment(source > 0 ? source : taken, chained ? source - source_before : 0.0, OutputRefresh());
        AdaptiveLay(plan, every, after);
        count = plan.made;
    }
    else g_adaptive_next = 0.0;
    ++g_steady.pairs;
    g_steady.asked += interpolate ? count : 0;
    if (!interpolate) ++g_steady.cut;
    else if (rest_now) ++g_steady.rested;
    else if (g_fg_too_fast) ++g_steady.too_fast;
    // NGX history is advanced even in a real-only group - but not while there
    // is no room for frames in between at all (the game alone fills the
    // screen) or the picture moves too fast for them: then NGX rests, and
    // starts from a reset when frames are wanted again.
    // The generator's own frames of them (FgMadeCount): only where frames in between are rebuilt.
    const bool rebuilt = g_rebuild.ready && g_rebuild.repairs && g_fg.w < g_fg.full_w;
    // XeSS MFG: the library makes three frames a pair in time at most, one
    // without its unlock plugin (xe_mfg.inl). Where frames in between are
    // rebuilt the others go by those, as x5 and x6 do with DLSS MFG; where
    // they are shown as the library made them, a pair has no more than that.
    if (XeSelected() && !rebuilt) count = std::min(count, XeMost());
    const unsigned made = XeSelected() ? std::min(FgMadeCount(count, rebuilt), XeMost()) : FgMadeCount(count, rebuilt);
    // (MSM FG has no history to keep going: a pair without frames in between asks it for none.)
    unsigned evaluated = idle ? 0u : MsmSelected() ? made : std::max(1u, made);
    // (XeSS MFG: more than the pair wants, where asking for fewer would halt the library - XeCount says why.
    // The pair is then shown by the nearest of them, as a pair with fewer made than shown is.)
    if (XeSelected() && evaluated) evaluated = std::min(XeCount(evaluated, !rebuilt), g_fg_count);
    static bool ngx_rested = false;
    auto p = g_fg.params;
    if (p) p->Reset();
    NVSDK_NGX_DLSSG_Opt_Eval_Params opt = {};
    for (int i = 0; i < 4; ++i)
        opt.cameraViewToClip[i][i] = opt.clipToCameraView[i][i] = opt.clipToLensClip[i][i] =
        opt.clipToPrevClip[i][i] = opt.prevClipToClip[i][i] = 1.0f;
    opt.mvecScale[0] = 1.f / v.work_w; opt.mvecScale[1] = 1.f / v.work_h;
    opt.cameraUp[1] = opt.cameraRight[0] = opt.cameraFwd[2] = 1;
    opt.cameraNear = 0.1f; opt.cameraFar = 1000; opt.cameraFOV = 1;
    opt.cameraAspectRatio = float(g_fg.w) / g_fg.height;
    opt.cameraMotionIncluded = opt.orthoProjection = opt.motionVectorsDilated = true;
    opt.colorBuffersHDR = g_fg.format == DXGI_FORMAT_R10G10B10A2_UNORM;
    opt.reset = !interpolate || ngx_rested;
    const bool restart = opt.reset;         // XeSS MFG is told the same
    ngx_rested = idle;
    opt.mvecsSubrectSize = opt.depthSubrectSize = {v.work_w, v.work_h};
    opt.backbufferSubrectSize = opt.outputInterpSubrectSize = {g_fg.w, g_fg.height};
    NVSDK_NGX_D3D12_DLSSG_Eval_Params ep = {};
    ep.pBackbuffer = color;
    ep.pMVecs = !lfOut.sent_passthrough && g_fg.motion ? g_fg.motion.get() : v.motion.gpu_tex;
    ep.pDepth = g_fg.depth.gpu_tex;
    // The real frame's own place: one nobody holds but the pool. The one
    // before it stays with the loop until this one takes its place.
    std::shared_ptr<FgReal> real;
    {
        std::lock_guard<std::mutex> lock(g_fg.mutex);
        for (auto &candidate : g_fg.reals) if (candidate && candidate.use_count() == 1) { real = candidate; break; }
    }
    // Every one in flight (the presenter far behind): this frame is let go,
    // and the presenter learns its delay anew - one that holds more pairs
    // than there is room for only ever leaves every other frame out.
    if (!real) { g_fg_starved = true; ++g_steady.no_room; return true; }
    const std::shared_ptr<FgReal> before = interpolate ? g_fg.latest : nullptr;
    // The pair before: its work is waited for here, when this one is about
    // to be recorded - not before the frame was taken.
    if (!FgCollect()) { g_fg.failed = true; CloseFgResources(); return false; }
    if (!CmdOpen()) { g_fg.failed = true; CloseFgResources(); return false; }
    g_fg_cost.used = 0;
    FgCostMark(lfCore.cmd, "start");
    // A motion field of the engine's is put at rest where the captured picture
    // did not change since the real frame before (lf_fg_still.inl). After a
    // cut or a pause there is no "before" to compare with.
    if (before && MotionGiven()) ep.pMVecs = FgStillCalm(lfCore.cmd, v.picture.gpu_tex, before->full.get(), ep.pMVecs);
    FgCostMark(lfCore.cmd, "calm");
    // The picture into its place, and small beside it when frames in between
    // are made smaller: that is what the generator is given.
    {
        D3D12_RESOURCE_BARRIER in[] = {
            Barrier(color, state, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Barrier(real->full.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
        lfCore.cmd->ResourceBarrier(2, in);
        // "Upscaling" sharpens: the real frame here, once, where frames in between are rebuilt from it at the
        // picture's own size (lf_window_fill.inl says why) - not where the output is scaled, where frames in
        // between are shown as the generator made them, or where a motion field of the engine's is put at rest
        // by comparing with this very picture (FgStillCalm).
        // (MSM FG's frames are made of the real ones' own pixels: sharpened here, they are sharp as well.)
        const bool sharpened = SharpenAsked() && (rebuilt || MsmSelected()) && !lfShow.upscaling && !MotionGiven() &&
                               SharpenInto(lfCore.cmd, real->full.get(), color);
        g_up.in_real = sharpened;
        if (!sharpened) lfCore.cmd->CopyResource(real->full.get(), color);
        D3D12_RESOURCE_BARRIER out[] = {
            Barrier(color, D3D12_RESOURCE_STATE_COPY_SOURCE, state),
            Barrier(real->full.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE)};
        lfCore.cmd->ResourceBarrier(2, out);
    }
    ID3D12Resource *model_color = color;
    D3D12_RESOURCE_STATES model_state = state;
    if (real->low) {
        auto write = Barrier(real->low.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        lfCore.cmd->ResourceBarrier(1, &write);
        FgScaleRecord(lfCore.cmd, color, state, real->low.get(), false);
        auto ready = Barrier(real->low.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        lfCore.cmd->ResourceBarrier(1, &ready);
        model_color = real->low.get();
        model_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        ep.pBackbuffer = model_color;
    }
    FgCostMark(lfCore.cmd, "scale");
    bool allow_interpolation = true;
    if (p) p->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, static_cast<unsigned long long>(g_fg.sequence + 1));
    opt.multiFrameCount = evaluated;
    // XeSS MFG: the real frame goes into the library's back buffer here, in
    // this list; its frames in between are asked for once the list is sent
    // (XeMake, below) - the library makes them in a swap chain of its own.
    // (With it goes how the picture moved since the frame before - the library asks to be told - unless the
    // engine's own motion field was picked in the app: then that is handed over.)
    ID3D12Resource *const xe_motion = MotionGiven() ? ep.pMVecs : nullptr;
    if (XeSelected() && evaluated) XeFeed(lfCore.cmd, model_color, model_state, MotionGiven() || restart ? nullptr : g_fg_prior.moved);
    // MSM FG: the pair's own work once, then each frame in between at its moment - from the two real frames,
    // into the pictures DLSS MFG's frames go into. Without a frame before (a cut, a rest) there is no pair, and
    // the next one starts anew.
    if (MsmSelected())
    {
        if (evaluated && before)
        {
            // What it is told beside the two frames: whether read-outs are kept still ("Keep still pixels"), which
            // search ("Motion estimation"), and - Adaptive Motion Learning - where the hand's movement says the
            // picture went.
            MsmAsk ask;
            ask.marks = smOpt.still.load() != 0;
            ask.fast = smOpt.msm_search.load() != 0;
            float across = 0.0f, down = 0.0f;
            if (LearnExpect(across, down)) { ask.expected_x = across * static_cast<float>(g_fg.full_w); ask.expected_y = down * static_cast<float>(g_fg.full_w); }
            // (Round 53: else the engine's own wide look for where the picture as a whole went - a fast turn, sm_guard.inl.)
            else if (GuardWideShift(across, down)) { ask.expected_x = across * static_cast<float>(g_fg.full_w); ask.expected_y = down * static_cast<float>(g_fg.full_w); }
            // (And the read-outs it has learnt for this game, whenever there is a new map of them.)
            {
                static std::vector<unsigned char> cells;
                int cells_w = 0, cells_h = 0;
                if (LearnHudTake(cells, cells_w, cells_h)) MsmHud(cells, cells_w, cells_h);
            }
            // Round 52, SMFG AI (msm_ai.h): the network makes them, on the card's AI units - SMFG 1.1 when it cannot.
            bool made = false;
            if (smOpt.msm_ai.load() && !msmai::Failed())
            {
                ID3D12Resource *outputs[msmai::kMost] = {};
                for (unsigned index = 0; index < evaluated && index < msmai::kMost; ++index) outputs[index] = g_fg.output[index].get();
                ID3D12GraphicsCommandList *list = lfCore.cmd;
                made = msmai::Frames(list, before->full.get(), real->full.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, outputs,
                                     D3D12_RESOURCE_STATE_COPY_SOURCE, std::min(evaluated, msmai::kMost), g_fg.full_w, g_fg.full_h, g_fg.format,
                                     [] { return CmdSubmit() != 0; }, [] { return CmdOpen(); });
                if (made) { MsmForget(); FgCostMark(lfCore.cmd, "ai"); }
            }
            if (!made)
            {
                msmai::Forget();
                MsmPair(lfCore.cmd, before->full.get(), real->full.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, ask);
                FgCostMark(lfCore.cmd, "msm pair");
                for (unsigned index = 0; index < evaluated; ++index)
                {
                    // (Adaptive MFG: at the moment of its beat.)
                    float moment = static_cast<float>(index + 1) / static_cast<float>(evaluated + 1);
                    for (unsigned b = 0; b < plan.n; ++b) if (plan.what[b] == static_cast<signed char>(index)) moment = plan.phase[b];
                    MsmFrame(lfCore.cmd, before->full.get(), real->full.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, g_fg.output[index].get(),
                             D3D12_RESOURCE_STATE_COPY_SOURCE, moment);
                    FgCostMark(lfCore.cmd, index == 0 ? "eval1" : index == 1 ? "eval2" : "eval+");
                }
            }
        }
        else { MsmForget(); msmai::Forget(); }
    }
    for (unsigned index = 0; index < evaluated && NgxSelected(); ++index)
    {
        opt.multiFrameIndex = index + 1;
        ep.pOutputInterpFrame = g_fg.output[index].get();
        ep.pOutputDisableInterpolation = g_fg.disable.get();
        auto pre = Barrier(model_color, model_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (model_state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) lfCore.cmd->ResourceBarrier(1, &pre);
        auto out_pre = Barrier(g_fg.output[index].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        lfCore.cmd->ResourceBarrier(1, &out_pre);
        const auto result = NGX_D3D12_EVALUATE_DLSSG(lfCore.cmd, g_fg.feature, p, &ep, &opt);
        auto disable_pre = Barrier(g_fg.disable.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        lfCore.cmd->ResourceBarrier(1, &disable_pre);
        lfCore.cmd->CopyBufferRegion(g_fg.disable_readback.get(), index * 4, g_fg.disable.get(), 0, 4);
        auto disable_post = Barrier(g_fg.disable.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        lfCore.cmd->ResourceBarrier(1, &disable_post);
        auto out_post = Barrier(g_fg.output[index].get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        lfCore.cmd->ResourceBarrier(1, &out_post);
        auto post = Barrier(model_color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, model_state);
        if (model_state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) lfCore.cmd->ResourceBarrier(1, &post);
        FgCostMark(lfCore.cmd, index == 0 ? "eval1" : index == 1 ? "eval2" : "eval+");
        if (NVSDK_NGX_FAILED(result))
        {
            FgAwait(lfCore.timeline, CmdSubmit(), 30000);
            if (g_fg_count > 1) {
                const unsigned next = g_fg_count > 3 ? 3 : 1;
                Trace("[dlssg] multiplier x%u refused 0x%08X; retrying x%u", g_fg_count + 1, result, next + 1);
                CloseFgResources(); g_fg_count = next; return false;
            }
            Trace("[dlssg] unavailable: Evaluate 0x%08X; ordinary output retained", result);
            g_fg.failed = true; CloseFgResources(); return false;
        }
    }
    FgSlot *slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_fg.mutex);
        for (auto &s : g_fg.slots) if (s.state == 0) { slot = &s; break; }
        // None free: the oldest pair still waiting gives way (the presenter
        // shows them in order).
        if (!slot)
        {
            for (auto &s : g_fg.slots) if (s.state == 2 && (!slot || s.sequence < slot->sequence)) slot = &s;
            if (slot) g_fg_overrun += slot->count + 1;
        }
        if (!slot)
        {
            // Starvation: every slot is mid-flight. The submitted work still
            // ends here - record its token, or PresentFrame hands the defer
            // contract a STALE g_fg_present_fence from an earlier frame and
            // the token-order guard kills the worker (the blink-out class).
            const UINT64 fence = CmdSubmit();
            // XeSS MFG: the library sees the frame all the same - it is its "frame before" from here on too.
            UINT64 unwanted = 0;
            if (XeSelected() && evaluated)
                XeMake(evaluated, nullptr, xe_motion, g_fg.depth.gpu_tex, v.work_w, v.work_h, restart, interval, &unwanted);
            FgAwait(lfCore.timeline, fence, 30000);
            g_fg_present_fence = fence;
            g_fg.latest = real;       // what NGX holds as "the frame before" from here on
            return true;
        }
        slot->state = 1;
    }
    // The reserved slot is invisible to the presenter until it is handed over,
    // below, once this list is sent.
    // Hand it the generated textures and reuse its old buffers next frame.
    // (XeSS MFG: the frames stay where the library draws them - XeMake says in which pictures, below.)
    for (unsigned i = 0; i < evaluated && !XeSelected(); ++i)
        std::swap(g_fg.output[i], slot->interpolated[i]);
    // Which tiles are at rest between the real frame before and this one, at
    // the size frames in between are made at, into the slot's own map.
    // (MSM FG: not asked - a pixel the two real frames have alike it leaves as it is by itself, msm_fg.h.)
    if (before && !MsmSelected())
    {
        if (real->low && before->low)
            FgStillTiles(lfCore.cmd, real->low.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                         before->low.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, slot->still.get());
        else
            FgStillTiles(lfCore.cmd, real->full.get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                         before->full.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, slot->still.get());
    }
    else g_fg_still.counted = false;
    FgCostMark(lfCore.cmd, "copy+tiles");
    FgCostResolve(lfCore.cmd);
    const double wait_from = LatencyNow();
    const UINT64 fg_fence = CmdSubmit();
    // XeSS MFG: now the library is asked - the frame is on its way into its
    // back buffer - and says which of its pictures the pair's frames in
    // between are drawn into (xe_tap.h): those are the pair's pictures. Not
    // made (the library's own no): the pair goes without, as when NGX says
    // so.
    UINT64 xe_taken = 0;
    bool xe_refused = false;
    if (XeSelected() && evaluated)
    {
        ID3D12Resource *into[5] = {};
        const bool came = XeMake(evaluated, into, xe_motion, g_fg.depth.gpu_tex, v.work_w, v.work_h, restart, interval, &xe_taken);
        for (unsigned i = 0; i < evaluated && came; ++i) slot->interpolated[i].copy_from(into[i]);
        // (Also the first pair after a rest: the library starts anew there and makes nothing.)
        xe_refused = !came;
    }
    FgDump(color, state, 0);     // the frames in between are written out as they are shown (FgPresenter)
    // The pair goes to the presenter now, with the mark its work is done at:
    // its first picture is drawn right behind that work (FgPresenter, send).
    // What is only known once the work is done - whether any tile is at rest,
    // whether the generator wants its frames shown at all - follows.
    {
        std::lock_guard<std::mutex> lock(g_fg.mutex);
        slot->sequence = ++g_fg.sequence;
        slot->count = count;
        slot->made = evaluated;
        slot->real = real;
        slot->before = before;
        slot->prior = g_fg_prior;
        slot->code = FgFrameCode();
        slot->source_time = (lfCap.window_live || lfCap.live) ? lfCap.stamp : 0.0;
        slot->mark = fg_fence;
        slot->taken = xe_taken;
        slot->watched = fg_fence;
        slot->done_at = 0.0;
        slot->denied = xe_refused;
        slot->ready_at = LatencyNow();
        // Without a time stamp of the capture's the moment is when it was taken.
        const double raw = source > 0 ? source : taken;
        const bool chained = interpolate && source > 0 && source_before > 0 && source > source_before;
        slot->moment = g_fg_clock.Moment(raw, chained ? source - source_before : 0.0, OutputRefresh());
        slot->prev_moment = chained && g_fg_clock.before > 0.0 ? g_fg_clock.before : slot->moment - interval;
        slot->interpolate = interpolate;
        slot->ad_on = by_beat && g_adaptive_next > 0.0;
        slot->adaptive = slot->ad_on ? plan.n : 0u;
        for (unsigned b = 0; b < plan.n; ++b) { slot->ad_when[b] = plan.when[b]; slot->ad_phase[b] = plan.phase[b]; slot->ad_what[b] = plan.what[b]; }
        // The pair's still map is given to a frame rebuilt from the real ones
        // whatever it holds; a frame made at the picture's size goes through
        // it only when the pair before had tiles at rest.
        slot->protect = interpolate && before != nullptr && !MsmSelected() && (real->low != nullptr || g_fg_still_before > 0);
        // Keep all requested phases evenly spaced at the display's attainable
        // rate. A fast incoming pair must not compress x4 into tiny deadlines
        // which cancel phases while the compositor is still busy.
        const double paced = target > 0 ? std::max(interval, (count + 1) / target) : interval;
        slot->interval = std::clamp(paced, 0.004, 0.2);
        slot->state = 2;
    }
    g_fg.history = true;
    g_fg.latest = real;
    if (slot->done_event && fg_fence)
    {
        ResetEvent(slot->done_event);
        // (XeSS MFG: the pair's work is done when the library's frames are in their place.)
        if (xe_taken) XeTell(xe_taken, slot->done_event);
        else lfCore.timeline->SetEventOnCompletion(fg_fence, slot->done_event);
    }
    g_fg.wake.notify_one();
    g_fg_present_fence = fg_fence;
    g_fg_pending.mark = fg_fence;
    g_fg_pending.slot = slot;
    g_fg_pending.sequence = g_fg.sequence;
    g_fg_pending.evaluated = evaluated;
    // SM_DIAG=1: how long the pair waits for the optical flow, and then for
    // the card to run the generator.
    if (FgCostDiag() && !FgPipelined())
    {
        static double copy_sum = 0.0, flow_sum = 0.0, list_sum = 0.0, lead_sum = 0.0;
        static unsigned waits = 0;
        static ULONGLONG told = 0;
        double copy_done = wait_from, flow_done = wait_from;
        if (ofa.copied_mark) { AwaitMark(lfCore.timeline, ofa.copied_mark, 2000); copy_done = LatencyNow(); }
        if (ofa.fence && ofa.value) { AwaitMark(ofa.fence.get(), ofa.value, 2000); flow_done = LatencyNow(); }
        AwaitMark(lfCore.timeline, fg_fence, 30000);
        lead_sum += wait_from - ofa.asked_at;
        copy_sum += copy_done - wait_from;
        flow_sum += flow_done - copy_done;
        list_sum += LatencyNow() - flow_done;
        ++waits;
        if (GetTickCount64() - told >= 2000)
        {
            Trace("[cost] a pair: %.2f ms from asking for the optical flow to sending the generator's list, then waits %.2f ms for "
                  "the flow's input copy, %.2f ms for the optical flow, %.2f ms for the generator's list",
                  lead_sum * 1000.0 / waits, copy_sum * 1000.0 / waits, flow_sum * 1000.0 / waits, list_sum * 1000.0 / waits);
            copy_sum = flow_sum = list_sum = lead_sum = 0.0; waits = 0; told = GetTickCount64();
        }
    }
    // Waited for here only when the next frame's work could reach this list.
    if (!FgPipelined() && !FgCollect()) { g_fg.failed = true; CloseFgResources(); return false; }
    return true;
}

// The multiplier picked in the app (frames in between: 1..5 for x2..x6). A
// change recreates the generator's feature only; a failure before is forgotten
// so the new choice is tried.
static void FgConfigure(unsigned count)
{
    count = std::clamp(count, 1u, 5u);
    if (g_fg_requested_count == count) return;
    CloseFgResources();
    g_fg_requested_count = count;
    g_fg_count = std::min(count, g_fg_runtime_max);
    g_fg.failed = false;
    lfOut.force_next = true;
    lfShow.stale = true;
    Trace("[dlssg] asked for %ux", count + 1);
}
