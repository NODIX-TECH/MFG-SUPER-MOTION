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
    double ad_when[16] = {};
    float ad_phase[16] = {};
    signed char ad_what[16] = {};
    // Set by the card when the mark is reached; FgDoneWatch writes done_at then.
    HANDLE done_event = nullptr;
    std::atomic<UINT64> watched{0};
    unsigned code = 0;        // tests: the frame's own number, read from the picture (tests\game.cpp)
    // Where nothing moved near (lf_fg_still.inl): the slot's own tile map, and
    // whether any tile of it is at rest.
    winrt::com_ptr<ID3D12Resource> still;
    bool protect = false;
    // Round 90, SMHUD (sm_hud.h): where the game's HUD is, found by the network for this pair - kept as the real
    // frame has it in every frame in between; hud_on: the map holds this pair's.
    winrt::com_ptr<ID3D12Resource> hud;
    bool hud_on = false;
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
    unsigned room = 0;          // the frames in between a pair has places for (FgRoom, when they were made)
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
#include "sm_hud.h"

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
// Round 95 (round 58's, lost with the return to 1.0.3 in round 83): places for a pair's frames in between - the
// multiplier's, and for SMFG at a fixed multiplier's own even beat (fixed_beat) two more, five at most: a pair a
// little longer than the game's average, or one whose beats fall clear of both real frames, has a beat more than the
// multiplier's count, and SMFG makes a frame for each. (DLSS MFG rebuilt: each beat goes by the frame of its own
// nearest it and needs no more than its multiplier's.)
static unsigned FgRoom() { return EvenOn() && MsmSelected() ? std::min(5u, g_fg_count + 2u) : g_fg_count; }

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
static constexpr unsigned kAdaptiveMost = 16;   // beats a pair at most (a pair far longer than the game's average has many)
struct AdaptivePlan { unsigned n = 0, made = 0; double when[kAdaptiveMost] = {}; float phase[kAdaptiveMost] = {}; signed char what[kAdaptiveMost] = {}; };
static double g_adaptive_next = 0.0;          // the next beat, the frame clock's seconds; 0: none yet
// Round 93 - a Present is not sent the pair's work on its heels. When the presenter's next Present is due, and when
// its last one went out (QPC seconds); the producer holds a pair's work back while a Present is a millisecond or two
// away (FgClearOfPresent) - sent just before it, that work was on the card when the flip came and the flip waited for
// it: with SMAI (4.8 ms of the network a pair) the real frame of every pair reached the screen 1.4 ms after its
// Present where the frames in between took 0.5 (PresentMon, tests\smooth\pmtypes.py). SM_PRESENT_CLEAR=0: not held.
static std::atomic<double> g_fg_present_due{0.0}, g_fg_present_went{0.0};
static void FgClearOfPresent()
{
    static const bool off = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_PRESENT_CLEAR", v, sizeof(v)) > 0 && v[0] == '0'; }();
    if (off) return;
    const double due = g_fg_present_due.load(), now = LatencyNow();
    if (due <= now - 0.0002 || due - now > 0.0020) return;
    // (Until it went out, and three tenths of a millisecond more for the card to take it; never past 3 ms.)
    while (LatencyNow() < due + 0.0015)
    {
        const double went = g_fg_present_went.load();
        if (went >= due - 0.0005) { while (LatencyNow() < went + 0.0003) YieldProcessor(); return; }
        SwitchToThread();
    }
}
static double AdaptiveTarget();               // below (it needs the screen's refresh)
// Round 58, back in round 95: a fixed multiplier's own beat (FgPresent sets it) - the game's average rate times the
// multiplier, frames a second; 0 none. (1.1.0: or a whole number of a fixed refresh's - FitBeat.)
static std::atomic<double> g_fixed_beat{0.0};
// Round 92: the most a pair is shown with when the generator places no more by their moments (DLSS MFG through the
// RTX 20/30 compatibility package, Intel's library: three) - 0 none. What the app is told runs.
static unsigned g_fg_shown_most = 0;
static unsigned FgExtraFrames() { return g_fg_shown_most ? std::min(g_fg_count, g_fg_shown_most) : g_fg_count; }
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
// Round 92 (tests\smooth - where each frame shown lands between its two real frames): with two made for x5 / x6, the
// others "going by the nearest" showed what moves its own way (a character, a car - the triangles of the test) at the
// nearest made frame's moment: x6 landed at 0, 1/3, 1/3, 2/3, 2/3, 0.88 - six pictures a pair and three moments, a
// staircase worse than x3 itself (DLSS MFG x6 at 33 FPS: moments a quarter off 77%, held 18%). Every frame is made
// now: 38% / 7%, at about 2 ms more of the card a pair. SM_FG_EVERY=0: two, as before (to compare).
static unsigned FgMadeCount(unsigned count, bool rebuilt)
{
    static const bool two = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_FG_EVERY", v, sizeof(v)) > 0 && v[0] == '0'; }();
    return (rebuilt && two && count >= 4) ? 2u : count;
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
    // Round 94: one piece by default. The captured frame's conversion sent ahead of the generator's list was a piece
    // of work of its own, and each piece takes the card from the game whatever it holds (tests\smooth\pieces64: a
    // 64x64 copy sent 600 times a second took a GPU-bound GTA V from 91 frames a second to 71 - a third of a
    // millisecond each). The Witcher 3 at 1440p, SMFG x3: 59.8 frames of the game a second pipelined (5 of 300 pairs
    // late), 61.5 in one piece (0 of 310 late); DLSS MFG x4 55.6 -> 56.7. SM_SERIAL=0: pipelined, as before.
    static const bool pipelined = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_SERIAL", v, sizeof(v)) > 0 && v[0] == '0'; }();
    return pipelined && !MotionGiven();
}

// Round 85: how many of a pair's pictures the presenter keeps drawn ahead of the one it shows next. Drawn one by one
// just before its moment (1, as before), a frame's drawing was the moment's own lateness: at the realtime class it
// went first and took the card from the game every time (GTA V, x4: 80 FPS alone, 44 under SMFG, 3.6 - 6.7% of its
// frames spikes), below it waited behind the game (drawing 0.7 ms became 3 ms, the screen a quarter less even). Drawn
// ahead into pictures of the presenter's own (the swap chain's buffers may be written only in their turn: one written
// ahead removed the device, 0x887A0005), at the moment only a copy onto the back buffer is left, the Present sent
// behind it. One as before: DLSS MFG (it may refuse a pair's frames after they are drawn), two cards (the relay),
// Spout, and while the frames are read back (a test's dumps, SM_DIAG's time stamps). SM_DRAW_AHEAD=<n> (to compare;
// 1: as before).
// Round 85b: and a pair with four frames in between or more (x5, x6): there each frame drawn ahead is a copy more on
// the card - GTA V at x6, measured against the engine before in the same minutes, 6% fewer frames and 1 - 3 ms more
// delay - and x6 had not the spikes x4 had (0.2 - 1.5% before). (The copy sent in the next frame's drawing instead,
// one piece of work a frame: the gate failed, 3.1% spikes - a longer piece breaks into the game for longer.)
static unsigned FgDrawAhead(bool reading_back, unsigned between)
{
    // Round 97: one, for SMFG too. Drawn ahead a frame in between was two pieces of work and two copies of the whole
    // picture - into a picture of the presenter's, then onto the back buffer - where one of each does: The Witcher 3
    // at 1440p, x4, 60.1 frames of the game a second drawn ahead (13 pieces a pair), 61.8 - 62.4 drawn straight (10),
    // the frames on the screen as even either way (2.4 - 2.9% a quarter off against 1.3 - 2.9%). What round 85 drew
    // ahead for - a frame's drawing being its moment's own lateness - is no longer so: each is drawn as soon as the
    // one before it has been presented, by a thread at the highest priority (round 95). SM_DRAW_AHEAD=3: as before.
    static const int asked = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_DRAW_AHEAD", v, sizeof(v)) > 0 ? atoi(v) : 1; }();
    // (Round 85d: SMFG only - the spikes were SMFG's; XeSS MFG had none before, and drawn ahead its late pairs came to
    // 3.3 - 5.5% in three gates against 3% allowed. DLSS MFG: above.)
    if (asked <= 1 || reading_back || between >= 4 || !MsmSelected() || lfRelay.on || SpoutWanted()) return 1;
    return std::min(static_cast<unsigned>(asked), 3u);
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
        slot.hud = nullptr; slot.hud_on = false;
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
    // (Round 101: also where the triangles read the motion for DLSS MFG or XeSS MFG - TriMotion works in SMFG's pictures.)
    if (MsmSelected() || g_msm.full_w) MsmClose();
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
    // Round 95: the thread that puts every frame on the screen at its moment ran at the priority of any other, and
    // waited out the last eight tenths of a millisecond by giving its turn away (SwitchToThread) - beside a game
    // that keeps every core busy, a turn given away comes back when the game lets it: the time between two Presents
    // moved 5% or more for one frame in ten (The Witcher 3, x3 - tests\smooth\flat.py) where Lossless Scaling's moved
    // 2%. Highest priority (as FSR's frame generation gives its presenter), and the last quarter of a millisecond
    // spent looking at the clock. SM_PRESENTER_PLAIN=1: as before (to compare).
    static const bool presenter_plain = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_PRESENTER_PLAIN", v, sizeof(v)) > 0 && v[0] == '1'; }();
    if (!presenter_plain) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
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
            {
                const unsigned loop = g_pieces[0].exchange(0), show = g_pieces[1].exchange(0), cap = g_pieces[2].exchange(0), flips = g_pieces[3].exchange(0);
                Trace("[pieces] sent to the card in these %u pairs: %u by the loop, %u by the presenter, %u by the capture, %u Presents - %.1f a pair",
                      pace.pairs, loop, show, cap, flips, static_cast<double>(loop + show + cap + flips) / pace.pairs);
            }
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
            else if (presenter_plain || left > 0.00025) SwitchToThread();
            else YieldProcessor();
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
        const HRESULT closed = list->Close();
        if (FAILED(closed)) { Trace("[dlssg] a drawing's list did not close (0x%08X)", (unsigned)closed); return false; }
        ID3D12CommandList *commands[] = {list.get()};
        if (after_mark && lfCore.show_queue != lfCore.direct_queue) lfCore.show_queue->Wait(lfCore.timeline, after_mark);
        XeAfter(lfCore.show_queue, after_taken);
        lfCore.show_queue->ExecuteCommandLists(1, commands);
        ++g_pieces[1];
        if (FAILED(lfCore.show_queue->Signal(fence.get(), ++value))) return false;
        SpoutAfter(lfCore.show_queue);
        alloc_mark[alloc_at] = value;
        if (!wait) return true;
        if (SUCCEEDED(fence->SetEventOnCompletion(value, event)) &&
            WaitForSingleObject(event, 2000) == WAIT_OBJECT_0 && fence->GetCompletedValue() >= value) return true;
        Trace("[dlssg] a drawing was not done in two seconds (fence %llu of %llu; the card: 0x%08X)", fence->GetCompletedValue(), value,
              (unsigned)lfCore.device->GetDeviceRemovedReason());
        return false;
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
    // Round 85: pictures drawn ahead of their turn (FgDrawAhead), in turn - the copy of one onto the back buffer is
    // always sent before it is drawn into again. At rest as a back buffer is (COMMON, the same as PRESENT).
    winrt::com_ptr<ID3D12Resource> ahead_pictures[4];
    unsigned ahead_next = 0;
    auto aheadPicture = [&](ID3D12Resource *like) -> ID3D12Resource * {
        winrt::com_ptr<ID3D12Resource> &own = ahead_pictures[ahead_next++ % 4];
        D3D12_RESOURCE_DESC want = like->GetDesc();
        if (own)
        {
            const D3D12_RESOURCE_DESC have = own->GetDesc();
            if (have.Width == want.Width && have.Height == want.Height && have.Format == want.Format) return own.get();
            own = nullptr;
        }
        want.Alignment = 0;
        want.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        const HRESULT made = lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &want, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                                    IID_PPV_ARGS(own.put()));
        if (FAILED(made)) { Trace("[dlssg] no picture to draw a frame ahead into (0x%08X)", (unsigned)made); own = nullptr; }
        return own.get();
    };
    ID3D12Resource *drawn_into = nullptr;   // the last draw's own picture; null: the back buffer itself
    // ahead: drawn ahead of the frame shown next - into a picture of the presenter's own (aheadPicture), not the back
    // buffer; wait: for the card to be through with it before going on.
    auto draw = [&](ID3D12Resource *source, bool synthetic, float phase, float made_phase, bool raw, unsigned ahead, bool wait) {
        winrt::com_ptr<ID3D12Resource> bb;
        drawn_into = nullptr;
        if (FAILED(ShowBackBuffer(IID_PPV_ARGS(bb.put())))) return false;
        if (ahead > 0)
        {
            ID3D12Resource *own = aheadPicture(bb.get());
            if (!own) return false;
            bb.copy_from(own);
            drawn_into = own;
        }
        if (!take()) return false;
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
        // Round 90, SMHUD (sm_hud.h): this pair's HUD map - the frame is finished beside the back buffer, then the HUD
        // pass takes it from there to a picture of its own, which is shown.
        // (SM_HUD_NOSHOW=1, tests: the network runs, its maps are not used - what the pass on the frames costs, apart.)
        static const bool hud_no_show = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_HUD_NOSHOW", v, sizeof(v)) && v[0] == '1'; }();
        const bool hud_here = synthetic && showing && showing->hud_on && showing->hud && real && real->full && smhud::Showable() && !hud_no_show;
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
                     bd.Format == td.Format && RebuildDrawReady(bd.Format) && !(hud_here && smhud::DrawFailed());
            read_back = direct && dumps;
            RebuildShow(list.get(), source, real->full.get(), real->low.get(), before->full.get(), before->low.get(),
                        mapped ? showing->still.get() : nullptr, target, phase, made_phase, showing->prior, showing->sequence, &marks,
                        direct ? bb.get() : nullptr, alloc_at);
            if (mapped) ++still_frames;
            if (direct) through_target = false;
            // (SMHUD: the HUD drawn over it there, the HUD's pixels only.)
            if (direct && hud_here)
            {
                FgReal *hud_before = showing->before && showing->before->full ? showing->before.get() : real;
                smhud::Draw(list.get(), bb.get(), hud_before->full.get(), real->full.get(), showing->hud.get(), phase);
            }
        } else if (synthetic && fits && mapped && !real->low) {
            FgStillShow(list.get(), source, real->full.get(), showing->still.get(), target);
            ++still_frames;
        } else if (synthetic && g_fg_scale.display) {
            target = g_fg_scale.display.get();
            FgScaleRecord(list.get(), source, D3D12_RESOURCE_STATE_COPY_SOURCE, target, true);
        } else through_target = false;
        // (SMHUD: from the finished frame - the target, or the generator's own at the picture's size - to its picture.)
        ID3D12Resource *hud_into = nullptr;
        if (hud_here && !direct)
        {
            FgReal *hud_before = showing->before && showing->before->full ? showing->before.get() : real;
            hud_into = smhud::Show(list.get(), through_target ? target : source,
                                   through_target ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_SOURCE,
                                   hud_before->full.get(), real->full.get(), showing->hud.get(), phase);
            // (Done in place on the target: it is shown from there, as always.)
            if (through_target && hud_into == target) hud_into = nullptr;
        }
        if (hud_into) {
            auto ready = Barrier(hud_into, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &ready);
            ShowBlit(list.get(), bb.get(), hud_into);
            auto rest = Barrier(hud_into, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &rest);
        } else if (through_target) {
            auto ready = Barrier(target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &ready);
            ShowBlit(list.get(), bb.get(), target);
            auto rest = Barrier(target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &rest);
        } else if (!direct) ShowBlit(list.get(), bb.get(), source);
        shown_from = hud_into ? hud_into : through_target ? target : direct ? (read_back ? bb.get() : nullptr) : source;
        shown_state = hud_into || through_target ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : direct || raw ? D3D12_RESOURCE_STATE_PRESENT : D3D12_RESOURCE_STATE_COPY_SOURCE;
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
        if (!send(wait || stamps)) return false;
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
    // Round 85: a picture drawn ahead (aheadPicture) onto the back buffer, not waited for - the Present goes behind it.
    auto copyOnto = [&](ID3D12Resource *picture) {
        winrt::com_ptr<ID3D12Resource> bb;
        if (FAILED(ShowBackBuffer(IID_PPV_ARGS(bb.put()))) || !take()) return false;
        D3D12_RESOURCE_BARRIER in[2] = { Barrier(bb.get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
                                         Barrier(picture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE) };
        list->ResourceBarrier(2, in);
        list->CopyResource(bb.get(), picture);
        D3D12_RESOURCE_BARRIER out[2] = { Barrier(bb.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT),
                                          Barrier(picture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON) };
        list->ResourceBarrier(2, out);
        return send(false);
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
        if (FAILED(pr)) { Trace("[dlssg] the Present was refused (0x%08X)", (unsigned)pr); return false; }
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
    // Round 95, the beat: a pair's real frame is shown only when a beat falls on it; the next pair's beats go on from
    // it. When no next pair comes - the game stopped: a menu, a pause, a stall - the screen was left on a frame in
    // between, short of the game's last picture (tests\smooth: "held 247 beats" over the stand-in's pause, the picture
    // 0.2 of a pair behind), and what stands still is looked at. The last pair's real frame, kept until the next
    // pair is taken up or two beats have gone by without one: then it is shown.
    std::shared_ptr<FgReal> tail;
    double tail_moment = 0.0, tail_source = 0.0, tail_beat = 0.0;
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
            // (Round 95: a pair shown by the beat whose real frame no beat fell on - `tail`: that frame is shown after
            // all when nothing follows within two beats of its time.)
            const auto patience = std::chrono::microseconds(!tail ? 250000LL :
                std::clamp(static_cast<long long>((tail_moment + pace.delay + 2.0 * tail_beat - LatencyNow()) * 1e6), 500LL, 250000LL));
            const bool ready = g_fg.wake.wait_for(lock, patience, [] {
                if (g_fg.stop) return true;
                for (auto &s : g_fg.slots) if (s.state == 2) return true;
                return false;
            });
            if (g_fg.stop) break;
            if (!ready && tail)
            {
                lock.unlock();
                const std::shared_ptr<FgReal> last = std::move(tail);
                tail.reset();
                showing = nullptr;
                if (last->full && draw(last->full.get(), false, 1.0f, 1.0f, false, 0, true)) flip(false, tail_source);
                continue;
            }
            if (!ready) { lock.unlock(); reportCadence(); continue; }
            tail.reset();
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
        // Round 91 (tests\smooth: GitHub #83's "as if MFG switched off and on"): a pair with nothing in between - the
        // first after the game rested, after a cut, the generator starting fresh - forgot the delay and showed its
        // real frame at once; the next pair was shown at the delay again, so that frame stood the delay longer than
        // any other: 57 - 63 ms on the screen after a pause, measured with every generator. The delay is the pipe's,
        // not the picture's: it is kept, and such a real frame is shown at its moment + the delay like any other.
        const bool keep_beat = !chosen->interpolate && pace.delay > 0.0 && !dumped;
        if (!chosen->interpolate && !keep_beat) pace.Forget();
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
            // Round 93: a frame of the game that came late (a hitch: 60 ms where the others take 25) is that pair's own
            // lateness, not a need of the pipe's. Counted as one, the delay went up 12 ms for the next 96 pairs - two and
            // a half seconds of every picture shown later, then let go at once: the game slower, then faster, after
            // every hitch of its own (tests\smooth, the delay's line). What the pair is longer than the usual by (a
            // quarter over it and more) is left out of what it asks; its frames come as soon as they can (slip, below).
            // SM_HITCH_NEED=1: counted, as before (to compare).
            static const bool hitch_counts = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_HITCH_NEED", v, sizeof(v)) > 0 && v[0] == '1'; }();
            // (Only a hitch by itself: a game whose frames are long again and again - 12, 21, 33 ms in turn - asks that
            // of the pipe for good, and left out there every third pair came late. The second long pair within 96 is
            // a pattern, counted from then on as before.)
            static UINT64 last_long = 0;            // the long pair before this one (its number)
            const bool is_long = pace.usual > 0.0 && span > 1.25 * pace.usual;
            const bool pattern = is_long && last_long != 0 && chosen->sequence - last_long < 96;
            if (is_long) last_long = chosen->sequence;
            const double longer = is_long && !pattern && !hitch_counts ? span - 1.25 * pace.usual : 0.0;
            const double own = longer * double(chosen->count) / double(chosen->count + 1);
            if (pace.delay > 0.0 && need > pace.delay + std::max(0.05, 2.0 * span)) { scheduled = false; ++pace.late; ++pace.pairs; }
            else
            {
                const bool late_before = need > pace.delay && pace.delay > 0.0;
                const bool counted = pace.Took(need - own, step);
                if (own > 0.0 && late_before && !counted) ++pace.late;
            }
            // SM_DIAG=1: the first pairs' needs, and the delay they leave.
            static const bool diag = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_DIAG", v, sizeof(v)) > 0 && v[0] == '1'; }();
            if (diag && pace.count <= 16)
                Trace("[diag] pair %llu: span %.1f ms, done %.1f ms after the frame before, drawn %.1f ms later, need %.1f ms -> delay %.1f ms%s",
                      chosen->sequence, span * 1000.0, (done - chosen->prev_moment) * 1000.0, (drawn - done) * 1000.0, need * 1000.0,
                      pace.delay * 1000.0, scheduled ? "" : " (not scheduled)");
        };
        // (Adaptive MFG: the place between the two real frames of the beat being shown; -1 none.)
        double ad_phase_now = -1.0;
        // Round 85: the pair's pictures in the order they are shown - drawn ahead of their moments, as many as
        // FgDrawAhead says, each into the swap chain's buffer it is shown from, and presented at their moments.
        // pass_at: past it (plus the delay, and what the pair is let slip) the picture is passed over instead of
        // drawn - 0: never; slip_phase: its place between the real frames for that slip, -1: none.
        struct Planned { ID3D12Resource *picture; bool synthetic; double moment, source_time; unsigned index; double ad_phase, pass_at, slip_phase; };
        // One drawn and not yet shown: when its drawing was asked and came back, its mark on the presenter's fence.
        struct Drawn { size_t item; double began, drawn; UINT64 mark; bool waited; unsigned ahead; float phase; ID3D12Resource *picture; };
        auto prepare = [&](const Planned &f, unsigned ahead, Drawn &d) {
            ad_phase_now = f.ad_phase;
            ID3D12Resource *picture = f.picture;
            const bool synthetic = f.synthetic;
            const unsigned index = f.index;
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
            if (synthetic && XeSelected() && chosen->denied.load()) { ++denied_frames; return 0; }
            const bool raw = synthetic && XeSelected() && !(chosen->real && picture == chosen->real->low.get());
            // Drawn ahead, it is not waited for here: its moment is a frame or more away.
            const bool wait = ahead == 0 && !quick;
            if (!draw(picture, synthetic, phase, made_phase, raw, ahead, wait)) { g_fg.failed = true; return -1; }
            if (synthetic) dump(chosen->sequence - 1, index + 1);
            d = { 0, began, LatencyNow(), value, wait || stamps, ahead, phase, drawn_into };
            stood("drawing a frame (waiting for the card)", began);
            return 1;
        };
        auto present = [&](const Planned &f, const Drawn &d) {
            ad_phase_now = f.ad_phase;
            const bool synthetic = f.synthetic;
            const double moment = f.moment, source_time = f.source_time;
            const unsigned index = f.index;
            const float phase = d.phase;
            const double began = d.began;
            double drawn = d.drawn;
            // Drawn ahead: as a rule the card was through with it long before now. (A quick pair's Present goes behind
            // its drawing on the card by itself, as before.)
            if (!d.waited && !quick && fence->GetCompletedValue() < d.mark)
            {
                FgAwaitPatient(fence.get(), d.mark);
                drawn = LatencyNow();
            }
            // Round 91 (tests\smooth, PresentMon): a picture drawn ahead was copied onto the back buffer at its moment, and
            // that copy took its turn on the card behind whatever was there - the next pair's work, the capture's -
            // so the screen showed it 1.4 ms after its Present when late (0.45 when not), in a pattern that came back
            // every pair: SMFG 9.4% of the gaps on the screen a quarter off, DLSS MFG (drawn into the back buffer
            // right after the frame before) 3%. Now it goes onto the back buffer at once, while its moment is still to
            // come; at the moment only the Present is left, with nothing on the card before it. SM_COPY_EARLY=0: at
            // the moment, as before (to compare).
            static const bool copy_early = [] { char v[4] = {}; return !(GetEnvironmentVariableA("SM_COPY_EARLY", v, sizeof(v)) && v[0] == '0'); }();
            bool copied = false;
            if (copy_early && d.picture && !quick)
            {
                if (!copyOnto(d.picture)) { g_fg.failed = true; return; }
                copied = true;
            }
            const bool led = first;
            if (first) { first = false; schedule(began, drawn, ad_phase_now >= 0.0 ? moment : 0.0); }
            // The generator said, once its work was done, that this pair has
            // no frames in between to show: the next picture is drawn over it.
            // (Only one at a time is drawn where that can come after the drawing: DLSS MFG, FgDrawAhead.)
            if (synthetic && d.ahead == 0 && chosen->denied.load()) { ++denied_frames; return; }
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
                    // (Round 96, Ultra Smooth: not made up within the pair - its frames stay a step apart and every
                    // frame after it is that much later: the delay is what the late frame left it, and comes back
                    // slowly (FgPace::Took). Made up, the pair's frames stood closer than a step - the picture faster
                    // for a pair after every frame that came late.)
                    if (SmoothOn() && late_by > 0.0005 && late_by < span) pace.delay += late_by;
                    else if (late_by > 0.0005 && late_by < span) { slip = late_by; slip_from = phase_now; }
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
                else
                {
                    // (1.1.0: on a fit beat, half a refresh after a refresh - FitSnap, lf_vrr.inl.)
                    const double at = g_fixed_beat.load() > 0.0 ? FitSnap(when) : when;
                    g_fg_present_due = at; if (!waitUntil(at)) return;
                }
            }
            else if (keep_beat && !synthetic)
            {
                // (Round 91: its moment + the delay, as every frame; already past, or far off - a start - at once.)
                const double when = moment + pace.delay - pace.call, ahead = when - LatencyNow();
                if (ahead > 0.0 && ahead <= 0.25 && !waitUntil(when)) return;
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
            // Drawn ahead: onto the back buffer now, the Present behind it on the card (round 91: unless done already).
            if (d.picture && !copied && !copyOnto(d.picture)) { g_fg.failed = true; return; }
            if (!flip(synthetic, source_time)) g_fg.failed = true;
            else if (synthetic) { ++made_shown; ++given_shown; }
            g_fg_present_went = LatencyNow();
            wave(moment);
            stood("the Present", called);
            // (What drawing takes is learnt from frames drawn just before their moments only.)
            pace.Took(led || d.ahead ? -1.0 : drawn - began, called - asked, LatencyNow() - called, synthetic);
        };
        // The plan run: drawn up to `window` ahead of the one shown next; one is as before - drawn, then shown.
        auto run = [&](const std::vector<Planned> &plan, unsigned window) {
            std::deque<Drawn> ready;
            size_t next = 0;
            while (!g_fg.stop && !g_fg.failed)
            {
                while (next < plan.size() && ready.size() < window && !g_fg.stop && !g_fg.failed)
                {
                    const Planned &f = plan[next];
                    // Too late to be told from the next one: shown now it would only push that one back (the
                    // two-at-once of before).
                    if (f.pass_at > 0.0 && LatencyNow() > f.pass_at + pace.delay + (f.slip_phase >= 0.0 ? slipped(f.slip_phase) : 0.0))
                    { ++pace.skipped; ++late_frames; ++next; continue; }
                    Drawn d = {};
                    const int got = prepare(f, static_cast<unsigned>(ready.size()), d);
                    if (got < 0) return;
                    if (got > 0) { d.item = next; ready.push_back(d); }
                    ++next;
                }
                if (ready.empty()) return;
                const Drawn d = ready.front();
                ready.pop_front();
                present(plan[d.item], d);
            }
        };
        std::vector<Planned> plan;
        plan.reserve(8);
        if (irregular) { pace.skipped += chosen->count; odd_frames += chosen->count; }
        // Adaptive MFG: each beat at its own moment - a frame in between made for it, or a real frame near it.
        if (chosen->ad_on && chosen->interpolate && !irregular)
        {
            const double beat_fps = AdaptiveTarget();
            const double beat = beat_fps > 0.0 ? 1.0 / beat_fps : step;
            CounterGameFrame(chosen->source_time);
            for (unsigned b = 0; b < chosen->adaptive; ++b)
            {
                const signed char what = chosen->ad_what[b];
                const double when = chosen->ad_when[b];
                // (DLSS MFG: any of its frames - prepare() takes the one nearest the beat.)
                ID3D12Resource *picture = what >= 0 ? chosen->interpolated[MsmSelected() ? what : 0].get() : what == -1 ? chosen->real->full.get() :
                                          chosen->before ? chosen->before->full.get() : nullptr;
                if (!picture) continue;
                plan.push_back({ picture, what >= 0, when, what >= 0 ? 0.0 : chosen->source_time, what >= 0 ? static_cast<unsigned>(what) : 0u,
                                 chosen->ad_phase[b], dumped ? 0.0 : when + 0.75 * beat, -1.0 });
            }
        }
        else if (chosen->interpolate && !irregular)
        {
            for (unsigned index = 0; index < chosen->count; ++index)
            {
                const double moment = chosen->prev_moment + step * (index + 1);
                // (A quick pair's frames are a millisecond and a half apart: up to three of them behind, a frame
                // is still shown in its turn. Further behind it is passed over, and the count a pair follows what
                // reaches the screen - more real frames with fewer between them, not pairs dropped whole: measured,
                // 97 pairs with three each against 67 with five, the same 400 frames a second on a 200 Hz screen.)
                plan.push_back({ chosen->interpolated[index].get(), true, moment, 0.0, index, -1.0,
                                 dumped ? 0.0 : moment + (quick ? 3.0 : 0.75) * step, double(index + 1) / double(chosen->count + 1) });
            }
        }
        if (!(chosen->ad_on && chosen->interpolate && !irregular))
            plan.push_back({ chosen->real->full.get(), false, chosen->moment, chosen->source_time, 0, -1.0, 0.0, -1.0 });
        run(plan, FgDrawAhead(stamps != nullptr || dumping, plan.empty() ? 0u : static_cast<unsigned>(plan.size() - 1)));
        if (chosen->ad_on && chosen->interpolate && !irregular && chosen->real && chosen->real->full &&
            (plan.empty() || plan.back().synthetic || plan.back().picture != chosen->real->full.get()))
        {
            const double beat_fps = AdaptiveTarget();
            tail = chosen->real;
            tail_moment = chosen->moment;
            tail_source = chosen->source_time;
            tail_beat = beat_fps > 0.0 ? 1.0 / beat_fps : step;
        }
        ad_phase_now = -1.0;
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
        g_fg.format == format && g_fg.room == FgRoom() && g_fg.thread.joinable()) return true;
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
            Trace("[dlssg] Ada temporal kernels: %u containers, count gate %s; %s", patched.containers, patched.gate ? "opened" : "not found",
                  patched.applied ? "MFG up to 6x enabled" : "unrecognized runtime, limited to 2x");
        }
    }
    // Round 85: XeSS-FG on the tensor cores - through the NGX core started on the device (NgxStart), where
    // the compatibility package takes the evaluation over (lf_dlssg_compat.inl, FgXessTensor).
    if (NgxSelected() && !g_fg.initialized && FgXessTensor())
    {
        if (!lfCore.ngx_ready) { Trace("[dlssg] unavailable: XeSS on the tensor cores needs the NGX core, which did not start"); return false; }
        // (The library defines the overload with a non-const parameter block; the block is only read.)
        g_fg.create = reinterpret_cast<NrCreateFn>(static_cast<NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList *, NVSDK_NGX_Feature, NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **)>(&NVSDK_NGX_D3D12_CreateFeature));
        g_fg.release = &NVSDK_NGX_D3D12_ReleaseFeature;
        g_fg_evaluate = &NVSDK_NGX_D3D12_EvaluateFeature;
        if (!g_fg.params && NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&g_fg.params))) return false;
        g_fg.initialized = true;
        Trace("[dlssg] the generator goes through the NGX core (XeSS-FG on the tensor cores)");
        // (Round 86, RTX 40: its runtime takes two frames a pair from an Ada card - the package's copy is opened before the
        // core opens it, and adapted as the engine's own copy is (lf_dlssg_ada.h). Not run on an RTX 40 here; its log says.)
        if (FgComputeCapability() == 89 && !g_compat_runtime.empty())
        {
            if (HMODULE runtime = LoadLibraryW(g_compat_runtime.c_str()))
            {
                auto base = reinterpret_cast<BYTE *>(runtime);
                auto nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + reinterpret_cast<IMAGE_DOS_HEADER *>(base)->e_lfanew);
                const auto patched = lf_ada::Apply(base, nt->OptionalHeader.SizeOfImage, 89);
                if (!patched.applied) g_fg_count = std::min(g_fg_count, 1u);
                Trace("[dlssg] XeSS on the tensor cores, RTX 40: the package's runtime adapted (%u containers, count gate %s) - %s",
                      patched.containers, patched.gate ? "opened" : "not found", patched.applied ? "up to 4x" : "2x");
            }
            else Trace("[dlssg] XeSS on the tensor cores, RTX 40: the package's runtime did not open (%lu) - 2x", GetLastError());
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
    g_fg.room = FgRoom();
    for (unsigned i = 0; i < g_fg.room; ++i)
    {
        g_fg.output[i].attach(NewTexture(w, height, format, true));
        if (!g_fg.output[i]) return false;
    }
    for (auto &slot : g_fg.slots)
        for (unsigned i = 0; i < g_fg.room; ++i)
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
    for (unsigned i = 0; i < g_fg.room; ++i)
    {
        auto to_copy = Barrier(g_fg.output[i].get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        lfCore.cmd->ResourceBarrier(1, &to_copy);
    }
    for (auto &slot : g_fg.slots) for (unsigned i = 0; i < g_fg.room; ++i)
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
    static constexpr UINT kMarks = 24;
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
// Round 85 (GitHub #73, "a multiplier of 2.5"): the game's own rate the decimal multiplier counts from - FgPresent's
// evened-out pace, followed a tenth of the way a pair and only when it moved more than 1.5%: the target must not
// wander with every frame of the game, or the beat itself would.
static double g_ratio_rate = 0.0;
// (Round 58, back in round 95: g_fixed_beat - with AdaptiveTarget's declaration, above: the presenter reads it too.)

// Round 86: each new frame's light as a whole (the mean of the small luma, sm_loop.inl) and the one before's - SMFG
// compares shapes rather than brightness where the whole picture got lighter or darker between a pair's two frames
// (msm_fg.h, Apart; MsmAsk::light).
static double g_light_now = 0.0, g_light_before = 0.0;
static void LightSeen(const BYTE *view, UINT w, UINT h)
{
    double sum = 0.0;
    size_t count = 0;
    for (UINT y = 0; y < h; y += 2)
    {
        const BYTE *row = view + static_cast<size_t>(y) * w;
        for (UINT x = 0; x < w; x += 2) { sum += row[x]; ++count; }
    }
    g_light_before = g_light_now;
    g_light_now = count ? sum / count : 0.0;
}
static float LightChange() { return g_light_before > 2.0 && g_light_now > 0.0 ? static_cast<float>(g_light_now / g_light_before - 1.0) : 0.0f; }
static double AdaptiveTarget()
{
    const int want = smOpt.adaptive.load();
    if (want <= 0)
    {
        // The decimal multiplier: Adaptive MFG at the game's rate times it, never past the screen.
        const int tenths = smOpt.ratio.load();
        // (Round 95: a fixed multiplier's own even beat, when it has one - FgPresent sets it.)
        if (tenths <= 10) return g_fixed_beat.load();
        if (g_ratio_rate <= 0.0) return 0.0;
        const double target = g_ratio_rate * tenths / 10.0, screen = OutputRefresh();
        return screen >= 24.0 ? std::min(target, screen * 0.97) : target;
    }
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
    // Round 95 - what rounds 59 to 81 had learnt about the beat and the return to 1.0.3 (round 83) left behind:
    //  - did the pair before end on its real frame? Then this pair does not begin on it as well - with a game's frames
    //    of uneven length the last beat of one pair and the first of the next took the same real frame: the picture
    //    stood for a beat and leapt two. That beat gets a frame of its own (round 59).
    //  - a pair with more beats than it has places for frames (a game's frame far longer than its average): the beat
    //    goes by with nothing new rather than the real frame early - shown early it was the world a beat ahead and
    //    then the same picture again (round 61).
    // SM_BEAT_PLAIN=1: the plain beat of before (to compare).
    static const bool plain = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_BEAT_PLAIN", v, sizeof(v)) > 0 && v[0] == '1'; }();
    static bool ended_real = false;
    if (g_adaptive_next <= before - beat || g_adaptive_next > end + 4.0 * beat) { g_adaptive_next = before + beat; ended_real = false; }
    // (Round 81's "a fixed multiplier's real frame on the beat nearest it, half a beat either way" is NOT back: tried
    // here first, the triangles of tests\smooth had 39.7% of the picture's steps a quarter of a beat off their moment
    // at a steady 40 FPS x3, against 1.8% - a real frame shown up to half a beat from where it belongs is the world
    // out of step, each pair another way. A beat within 0.08 of a pair of a real frame shows it; every other one a
    // frame made for that very moment.)
    // (A fixed multiplier at its limit's beat: within a fifth of a beat at most - at four beats a pair 0.08 of the pair
    // is a third of a beat, and the triangles had those pictures a quarter of a beat off their moment.)
    const bool own_beat = !plain && smOpt.adaptive.load() <= 0 && smOpt.ratio.load() <= 10;
    const float near_real = own_beat ? std::min(kAdaptiveNear, static_cast<float>(0.2 * beat / span)) : kAdaptiveNear;
    // (SMFG: the places it has - never more than the generator was made for: round 85, x2.5 asked as x3 laid a third
    // frame on a generator of two and the card's list was refused, 0x80070057. DLSS MFG rebuilt: as many beats as the
    // plan holds - each is rebuilt at its own moment from the frame of the generator's nearest it.)
    const unsigned room = plain ? std::min(5u, g_fg_count) : MsmSelected() ? std::min(5u, std::max(1u, g_fg.room)) : kAdaptiveMost;
    while (g_adaptive_next <= end + 1e-6 && p.n < kAdaptiveMost)
    {
        const float t = static_cast<float>((g_adaptive_next - before) / span);
        signed char what = -1;
        if (t <= near_real && !(!plain && ended_real && p.n == 0 && p.made < room)) what = -2;
        else if (t < 1.0f - near_real && p.made < room) what = static_cast<signed char>(p.made++);
        else if (!plain && t < 1.0f - near_real) { g_adaptive_next += beat; continue; }
        p.when[p.n] = g_adaptive_next; p.phase[p.n] = t; p.what[p.n] = what; ++p.n;
        g_adaptive_next += beat;
    }
    ended_real = p.n > 0 && p.what[p.n - 1] == -1;
}

// Round 91 (tests\smooth): a pair's whole work on the card went in one piece - the motion search and every frame in
// between, 2.4 ms with DLSS MFG, 3.3 with SMFG at 1440p - and the presenter's copy and Present, which come every
// 8 ms at x3/40 FPS, waited behind it whenever their moment fell inside it: frames reached the screen 0 to 3 ms late,
// steadily ordered in the Presents and not on the screen (SMFG 7%, DLSS 3.6% of the moments a quarter off their
// beat, in the order of their pieces' length). In pieces - the search, then each frame in between - the card takes
// the presenter's work up between two of them. SM_SPLIT=0 / 1: one piece / pieces (to compare).
static bool FgSplitWanted()
{
    static const int wanted = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_SPLIT", v, sizeof(v)) ? v[0] - '0' : 0; }();
    return wanted == 1;
}
static void FgSplit()
{
    if (!FgSplitWanted()) return;
    CmdSubmit();
    CmdOpen();
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
    // (Round 88r, the owner: "Limit to refresh rate" is simply a Frame limit at the screen's refresh - 200 on a 200 Hz
    // screen, as if set by hand: ShownCap, lf_settings.inl. Nothing of its own here any more - its 0.97 of the refresh
    // and VRR's ceiling had it at 188, and round 88q's whole counts left x6 far under 200.)
    const bool to_screen = false;
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
    // Round 58, back in round 95 (the owner: "Lossless Scaling makes the game itself smoother; I do not want all these
    // oscillations in the frame time line"): a fixed multiplier at an even beat of its own (fixed_beat). Each pair had
    // the multiplier's count laid evenly over its own length, so every change of the game's frame time was shown, times
    // the multiplier - The Witcher 3, x3: the game's frames 15.6 to 19.2 ms, the Presents 5.30 to 6.31 ms and a step
    // of 9% between neighbours at every tenth of them, where Lossless Scaling's were 3.91 to 4.25 with 2%
    // (tests\smooth\flat.py). At a beat that follows only the game's average rate - the mean of its last fifteen
    // frames as they came, so the beat moves a fifteenth of a frame's change each frame and never at once - each frame
    // is made for its own moment as Adaptive MFG's are. Where frames in between can be made for any moment: SMFG, and
    // DLSS MFG rebuilt at the picture's size.
    // ONLY where the multiplier asks more than a limit lets through (Frame limit, Limit to refresh rate, VRR's): there
    // the pairs took turns with 3 and 4 frames - x4 of 56 on a 200 Hz screen, gaps of 5.9 and 4.45 ms side by side,
    // 11.6% of the frames on the screen a quarter off; on the limit's own beat 0.94%, with 0.25 long gaps a second
    // for 1.44 and 15.7 ms behind the game for 23.2 (The Witcher 3, DLSS MFG). Under the limit a pair keeps the
    // multiplier's count at its own even places, every real frame at its own moment: the beat run free there had a
    // real frame only where a beat fell on one (see AdaptiveLay), and the frame clock made even is what keeps the
    // line flat (lf_fg_pace.inl, Trust).
    {
        static double spans[15] = {};
        static unsigned spans_filled = 0, spans_next = 0;
        // (SMFG only. DLSS MFG rebuilt goes by the frame of its own nearest each beat, and what moves its own way - a
        // character, a car, the triangles of tests\smooth - then stands at that frame's moment, not the beat's: at
        // the limit's beat 21.7% of the picture's steps a quarter of a beat off, 3.9% with SMFG. Over a limit DLSS MFG
        // and XeSS MFG get the largest whole count that fits instead - below, `whole_fit`.)
        const bool any_moment = MsmSelected();
        if (!EvenOn() || smOpt.adaptive.load() > 0 || smOpt.ratio.load() > 10 || !interpolate || every <= 0.0 || !any_moment || g_fg_count == 0)
        {
            spans_filled = spans_next = 0;
            g_fixed_beat = 0.0;
        }
        else
        {
            // (A frame far from the pace - a stall, a load - is counted as the pace: one hitch does not move the beat.)
            spans[spans_next] = std::clamp(interval, 0.6 * every, 1.7 * every);
            spans_next = (spans_next + 1) % 15;
            if (spans_filled < 15) ++spans_filled;
            double sum = 0.0;
            for (unsigned i = 0; i < spans_filled; ++i) sum += spans[i];
            // (DLSS MFG on an RTX 20/30 places its frames by their moments up to x4 - round 92: x5 / x6 asked is x4's beat.)
            unsigned shown = g_fg_count;
            if (NgxSelected() && !g_xess_tensor && shown > 3) { const int sm = FgComputeCapability(); if (sm == 75 || sm == 86) shown = 3; }
            const double asked = double(shown + 1) * spans_filled / sum;
            double most = 0.0;
            for (const int limit : { ShownCap(), PresentCap() })
                if (limit > 0) most = most > 0.0 ? std::min(most, double(limit)) : double(limit);
            // (Over the limit, and back under it only when 4% under: a game at the edge does not go from one way of
            // showing its frames to the other pair by pair.)
            static bool over = false;
            over = most > 0.0 && spans_filled >= 8 && asked > most * (over ? 0.96 : 1.0);
            static bool was_over = false;     // (1.1.0: not g_fixed_beat - a fit beat sets it under the limit too)
            if (over != was_over)
                Trace(over ? "[beat] x%u of the game's %.1f frames a second is more than the %.0f a second allowed: shown at that beat, each frame made for its own moment"
                           : "[beat] x%u of the game's %.1f frames a second fits under the limit again: the multiplier's count in each pair",
                      shown + 1, spans_filled / sum, most);
            was_over = over;
            // (1.1.0: on a screen at its fixed refresh, a whole number of refreshes - FitBeat, lf_vrr.inl.)
            double fit = FitActive() && spans_filled >= 8 ? FitBeat(over ? most : asked) : 0.0;
            static bool fitting = false;
            if ((fit > 0.0) != fitting)
            {
                fitting = fit > 0.0;
                if (fitting) Trace("[beat] the screen stays at its fixed %.0f Hz: x%u of the game's %.1f frames a second shown at %.1f a second - every frame %.0f refreshes",
                                   OutputRefresh(), shown + 1, spans_filled / sum, fit, OutputRefresh() / fit);
                else Trace("[beat] the multiplier's own beat again (the screen follows the frames, or the fit is off)");
            }
            g_fixed_beat = fit > 0.0 ? fit : over ? most : 0.0;
        }
    }
    // (The decimal multiplier's rate, AdaptiveTarget.)
    if (interpolate && every > 0.0)
    {
        const double rate = 1.0 / every;
        if (g_ratio_rate <= 0.0) g_ratio_rate = rate;
        else if (std::fabs(rate - g_ratio_rate) > 0.015 * g_ratio_rate) g_ratio_rate += 0.1 * (rate - g_ratio_rate);
    }
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
        // Round 95, "Even pacing" (fixed_beat) with a generator that makes its frames at a pair's even places only
        // (DLSS MFG, XeSS MFG), a fixed multiplier and a limit it does not fit under: the largest whole count that
        // does, the same for every pair - x4 of 56 on a 200 Hz screen is x3, 168 frames a second 5.9 ms apart, each
        // at its own moment. Filled exactly (below) the pairs took turns with 3 and 4: 200 a second, 5.9 and 4.45 ms
        // apart side by side - the owner's "all these oscillations". It goes up when one more has fitted for twenty
        // pairs, down when it has not for eight.
        const bool whole_fit = capped && EvenOn() && !MsmSelected() && smOpt.adaptive.load() <= 0 && smOpt.ratio.load() <= 10 &&
                               room < double(g_fg_count + 1) + 0.06;
        static unsigned fitted = 0;
        static int fit_more = 0, fit_fewer = 0;
        if (!whole_fit) { fitted = 0; fit_more = fit_fewer = 0; }
        if (whole_fit)
        {
            auto fit = [](double frames) { return static_cast<unsigned>(std::clamp(static_cast<int>(std::floor(frames)) - 1, 0, static_cast<int>(g_fg_count))); };
            // (Round 96: a count stays until it is 3% over the limit, and one more is taken only when it has fitted
            // for forty pairs - The Witcher 3 between 64 and 71 FPS on a 200 Hz screen stood at x3's edge, 66.7, and
            // went from x3 to x2 and back: 190 frames a second, then 142.)
            // (One more frame a pair makes the game itself slower - The Witcher 3 at 1440p: 68.6 FPS under x2, 61.5 under
            // x3 - so there is more room after the step up than before it: counted in at 7%, less than it is. Without
            // it a session that began at x2 stayed there, 137 frames a second, where x3 at 186 fits.)
            const unsigned up = fit(room * 1.07 - 0.03), down = fit(room + 0.09);
            if (fitted == 0 || fitted > g_fg_count) { fitted = std::max(1u, fit(room + 0.09)); fit_more = fit_fewer = 0; }
            fit_more = up > fitted ? fit_more + 1 : 0;
            fit_fewer = down < fitted ? fit_fewer + 1 : 0;
            if (fit_more >= 40) { fitted = up; fit_more = fit_fewer = 0; ++g_steady.changes; }
            else if (fit_fewer >= 8) { fitted = std::max(1u, down); fit_more = fit_fewer = 0; ++g_steady.changes; }
            count = fitted;
            g_fg.extra_budget = 0;
            held = 0;
        }
        else if (capped)
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
        // (DLSS MFG rebuilt: its beats may be more than the frames it makes - each goes by the nearest of them.)
        count = MsmSelected() ? plan.made : std::min(plan.made, g_fg_count);
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
    // (Round 92: rebuilt too - Intel's library makes three a pair at most, and x5 / x6 shown from three landed at 1/4,
    // 1/2, 3/4 in five places, 91% of the moments a quarter off: x4's own frames at their own moments are smoother than
    // x6 made of them. With SM_FG_EVERY=0, as before.)
    static const bool xe_two = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_FG_EVERY", v, sizeof(v)) > 0 && v[0] == '0'; }();
    // (Round 92: DLSS MFG through the RTX 20/30 compatibility package places its frames by their moments up to x4 -
    // 0.25 / 0.50 / 0.73 after the rebuild - and not past it: x5 at 0.21 / 0.34 / 0.52 / 0.59 / 0.75 where 0.2 / 0.4 /
    // 0.6 / 0.8 were asked, x6 likewise, every made frame or two - motion 28 - 36% a quarter off against x4's 3.6%.
    // There x5 / x6 asked is shown as x4. RTX 40 / 50 (their own route, not measured here) keep what was asked.
    // SM_DLSS_ABOVE_X4=1: as asked on every card, to compare.)
    // Round 98 (the owner, trying r97: "x6 does not work with XeSS MFG and DLSS MFG"): x5 and x6 are back, shown as asked.
    //  - DLSS MFG: every frame made, as round 92 first had it (moments a quarter off 38% on the triangles at x6
    //    where x4 has 3.6% - what moves its own way is not where its moment says; the picture as a whole, rebuilt
    //    from the two real frames, is). SM_DLSS_X4_CAP=1: shown as x4, as rounds 92 to 97 did.
    //  - XeSS MFG, Intel's library (three a pair at most) and the tensor cores' (the package refuses more): where
    //    the frames are rebuilt at the picture's size, each of the five is rebuilt at its own moment and goes by
    //    the nearest of the three made for what moves its own way; shown as made (Flow 100) a pair has three.
    static const bool dlss_above = [] { char v[4] = {}; return !(GetEnvironmentVariableA("SM_DLSS_X4_CAP", v, sizeof(v)) > 0 && v[0] == '1'); }();
    if (NgxSelected() && !g_xess_tensor && !dlss_above && count > 3)
    {
        const int sm = FgComputeCapability();
        if (sm == 75 || sm == 86)
        {
            static bool said = false;
            if (!said) { said = true; Trace("[dlssg] DLSS MFG on this RTX %s card places its frames by their moments up to x4: x%u asked is shown as x4, each frame at its own moment",
                                            sm == 75 ? "20" : "30", count + 1); }
            count = 3;
            // (What the app is told - not under Adaptive MFG, whose beats lay the frames: AdaptiveLay, below.)
            g_fg_shown_most = smOpt.adaptive.load() > 0 || smOpt.ratio.load() > 10 ? 0u : 3u;
        }
    }
    if (XeSelected() && !rebuilt && count > XeMost() && XeMost() > 0)
    {
        static bool said = false;
        if (!said) { said = true; Trace("[xess] Intel's library makes %u frames in between a pair at most: x%u asked is shown as x%u, each frame at its own moment", XeMost(), count + 1, XeMost() + 1); }
        count = XeMost();
        g_fg_shown_most = AdaptiveTarget() > 0.0 ? 0u : XeMost();
    }
    // (The tensor cores' XeSS-FG: three made at most; shown as made, three shown.)
    if (FgXessTensor() && !rebuilt && count > 3) { count = 3; g_fg_shown_most = AdaptiveTarget() > 0.0 ? 0u : 3u; }
    const unsigned made = XeSelected() ? std::min(FgMadeCount(count, rebuilt), XeMost()) : FgXessTensor() ? std::min(FgMadeCount(count, rebuilt), 3u) : FgMadeCount(count, rebuilt);
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
    // Round 101, the owner: "Triangle Mesh" for DLSS MFG and XeSS MFG too - the motion read by the triangles' search
    // (msm_tri.h, TriMotion: the mesh's corners, 3DRS) and handed to the generator as its motion picture, in place of
    // the picture of noughts it is given when the motion is left to it. Not where the engine's own field was picked
    // (MotionGiven), and not across a cut or a rest: there is no pair then.
    ID3D12Resource *tri_motion = nullptr;
    if (!MsmSelected() && !MotionGiven() && smOpt.msm_tri.load() != 0 && evaluated && before && !restart)
    {
        const int pitch = smOpt.tri_pitch.load(), rounds = smOpt.tri_rounds.load(), fine = smOpt.tri_fine.load(), full = smOpt.tri_full.load(), top = smOpt.tri_top.load();
        if (pitch > 0 && static_cast<UINT>(pitch) != g_tri.pitch) { g_tri.pitch = static_cast<UINT>(pitch); TriClose(); MsmForget(); }
        if (rounds > 0) g_tri.rounds = static_cast<float>(rounds);
        if (fine >= 0) g_tri.fine_rounds = fine;
        if (full >= 0) g_tri.full_rounds = full;
        if (top > 0) g_tri.top = top;
        if (smOpt.tri_exact.load() >= 0) g_tri.exact_rounds = smOpt.tri_exact.load();
        if (smOpt.tri_relax.load() >= 0) g_tri.relax_rounds = smOpt.tri_relax.load();
        tri_motion = TriMotion(lfCore.cmd, before->full.get(), real->full.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, g_fg.full_w, g_fg.full_h, g_fg.format,
                               FlowShare(), v.work_w, v.work_h);
        if (tri_motion) { ep.pMVecs = tri_motion; FgCostMark(lfCore.cmd, "tri motion"); }
    }
    else if (!MsmSelected() && g_msm.full_w) MsmForget();       // (the next pair of theirs starts anew)
    ID3D12Resource *const xe_motion = MotionGiven() ? ep.pMVecs : tri_motion;
    if (XeSelected() && evaluated) XeFeed(lfCore.cmd, model_color, model_state, MotionGiven() || tri_motion || restart ? nullptr : g_fg_prior.moved);
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
            ask.light = LightChange();
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
            // Round 52, SMFG AI (msm_ai.h): the network makes them, on the card's AI units - SMFG when it cannot.
            // Round 88c, SMAI (the owner, after trying round 88's "SMFG Net only helps SMFG": slower, and no better - a
            // generator of its own instead, picked in the app beside SMFG, DLSS MFG and XeSS MFG): msm_ai 1 is SMAI -
            // SMFG Net makes the frames, as in round 87. The helper stays for tests only: SM_AI_HELP=1 (its motion offered
            // to SMFG's blocks, msmai::Words, MsmAsk::net).
            bool made = false;
            const int ai_mode = smOpt.msm_ai.load();
            // Round 99, the owner's triangles (msm_tri.h, "Triangle Mesh" in the app): SMFG's pair and frames made by a mesh of
            // triangles instead - not while a network makes them (SMAI).
            {
                const bool tri = smOpt.msm_tri.load() != 0 && ai_mode == 0;
                if (tri != (g_msm.tune.tri > 0.0f)) { g_msm.tune.tri = tri ? 1.0f : 0.0f; MsmForget(); }
                if (tri)
                {
                    // (Its own numbers, where the app gave any: a mesh of another size is made anew.)
                    const int pitch = smOpt.tri_pitch.load(), rounds = smOpt.tri_rounds.load(), fine = smOpt.tri_fine.load(), full = smOpt.tri_full.load(),
                              home = smOpt.tri_home.load(), top = smOpt.tri_top.load();
                    if (pitch > 0 && static_cast<UINT>(pitch) != g_tri.pitch) { g_tri.pitch = static_cast<UINT>(pitch); TriClose(); MsmForget(); }
                    if (rounds > 0) g_tri.rounds = static_cast<float>(rounds);
                    if (fine >= 0) g_tri.fine_rounds = fine;
                    if (full >= 0) g_tri.full_rounds = full;
                    if (home >= 0) g_tri.home = static_cast<float>(home);
                    if (top > 0) g_tri.top = top;
                    if (smOpt.tri_exact.load() >= 0) g_tri.exact_rounds = smOpt.tri_exact.load();
                    if (smOpt.tri_relax.load() >= 0) g_tri.relax_rounds = smOpt.tri_relax.load();
                }
                if (tri && !TriOn()) TriReady(g_fg.full_w, g_fg.full_h, g_fg.format);
            }
            static const bool ai_helps = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_AI_HELP", v, sizeof(v)) && v[0] == '1'; }();
            // (Round 88b, the helper: SMFG's own search goes to the card with the network's input, before the network -
            // the card works on it while this thread hands the network its work; the rest of SMFG's pair after it.)
            bool searched = false;
            if (ai_mode == 1 && ai_helps && !msmai::Failed())
            {
                UINT search_w = 0, search_h = 0, blocks_w = 0, blocks_h = 0;
                ID3D12Resource *const words = MsmNetWords(search_w, search_h, blocks_w, blocks_h);
                ID3D12GraphicsCommandList *list = lfCore.cmd;
                ask.net = msmai::Words(list, before->full.get(), real->full.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, words, search_w, search_h,
                                       blocks_w, blocks_h, g_fg.full_w, g_fg.full_h, g_fg.format,
                                       [] { return CmdSubmit() != 0; }, [] { return CmdOpen(); },
                                       [&] {
                                           MsmPairSearch(lfCore.cmd, before->full.get(), real->full.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, ask);
                                           FgCostMark(lfCore.cmd, "msm search");
                                       }, searched);
                if (ask.net) FgCostMark(lfCore.cmd, "ai");
                else msmai::Forget();
            }
            else if (ai_mode && !msmai::Failed())
            {
                ID3D12Resource *outputs[msmai::kMost] = {};
                for (unsigned index = 0; index < evaluated && index < msmai::kMost; ++index) outputs[index] = g_fg.output[index].get();
                // (Round 87: each at its moment - Adaptive MFG's beat - as SMFG 1.1 below; SMFG Net draws at any.)
                float moments[msmai::kMost] = {};
                for (unsigned index = 0; index < msmai::kMost; ++index)
                {
                    moments[index] = static_cast<float>(index + 1) / static_cast<float>(evaluated + 1);
                    for (unsigned b = 0; b < plan.n; ++b) if (plan.what[b] == static_cast<signed char>(index)) moments[index] = plan.phase[b];
                }
                ID3D12GraphicsCommandList *list = lfCore.cmd;
                // (Round 88d, SMAI 1.0: SMFG's marks - read-outs and what stays on the screen held where they are - when
                // "Keep still pixels" is on, found from the network's own motion: msmai::MarksAsk, MsmMarksFromWords.)
                msmai::MarksAsk marks_ask;
                const bool with_marks = ai_mode == 1 && ask.marks;
                if (with_marks)
                {
                    marks_ask.words = MsmNetWords(marks_ask.search_w, marks_ask.search_h, marks_ask.blocks_w, marks_ask.blocks_h);
                    marks_ask.holds = MsmMarkHolds();
                    marks_ask.make = [&](ID3D12GraphicsCommandList *l) {
                        return MsmMarksFromWords(l, before->full.get(), real->full.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, ask);
                    };
                }
                FgClearOfPresent();
                made = msmai::Frames(list, before->full.get(), real->full.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, outputs,
                                     D3D12_RESOURCE_STATE_COPY_SOURCE, std::min(evaluated, msmai::kMost), g_fg.full_w, g_fg.full_h, g_fg.format,
                                     [] { return CmdSubmit() != 0; }, [] { return CmdOpen(); }, moments, with_marks ? &marks_ask : nullptr);
                if (made) { MsmForget(); FgCostMark(lfCore.cmd, "ai"); }
            }
            if (!made)
            {
                if (!ask.net) msmai::Forget();
                if (searched) MsmPairFinish(lfCore.cmd, before->full.get(), real->full.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, ask);
                else { g_msm_mark = FgCostDiag() ? &FgCostMark : nullptr; MsmPair(lfCore.cmd, before->full.get(), real->full.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, ask); }
                FgCostMark(lfCore.cmd, "msm pair");
                FgSplit();
                for (unsigned index = 0; index < evaluated; ++index)
                {
                    // (Adaptive MFG: at the moment of its beat.)
                    float moment = static_cast<float>(index + 1) / static_cast<float>(evaluated + 1);
                    for (unsigned b = 0; b < plan.n; ++b) if (plan.what[b] == static_cast<signed char>(index)) moment = plan.phase[b];
                    MsmFrame(lfCore.cmd, before->full.get(), real->full.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, g_fg.output[index].get(),
                             D3D12_RESOURCE_STATE_COPY_SOURCE, moment);
                    FgCostMark(lfCore.cmd, index == 0 ? "eval1" : index == 1 ? "eval2" : "eval+");
                    if (index + 1 < evaluated) FgSplit();
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
        if (!NVSDK_NGX_FAILED(result) && index + 1 < evaluated) FgSplit();
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
    // Round 90, SMHUD (sm_hud.h, "hud_ai" - every generator): where the game's HUD is in this pair, into the slot's
    // own map, for each of its frames in between to show it as the real frame has it.
    slot->hud_on = interpolate && before && smhud::Pair(before->full.get(), real->full.get(), g_fg.full_w, g_fg.full_h, g_fg.format, slot->hud);
    if (slot->hud_on) FgCostMark(lfCore.cmd, "hud");
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
    if (!(MsmSelected() && smOpt.msm_ai.load())) FgClearOfPresent();     // (SMAI: held before its network went, above)
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
        // SM_STAMPS=1 (tests, round 93): every frame's stamp as the capture gave it, against the clock's moment for it.
        {
            static const bool stamps = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_STAMPS", v, sizeof(v)) > 0 && v[0] == '1'; }();
            if (stamps) Trace("[stamp-at] %.6f %.6f", raw, taken);
            if (stamps) Trace("[stamp] span %.2f ms, taken %.2f ms after the stamp, the clock's moment %.2f ms after it, pace %.2f ms; one span off by %.2f ms, eight together by %.2f",
                              chained ? (source - source_before) * 1000.0 : 0.0, (taken - raw) * 1000.0, (slot->moment - raw) * 1000.0, g_fg_clock.every * 1000.0,
                              std::sqrt(g_fg_clock.tr_one) * 1000.0, std::sqrt(g_fg_clock.tr_eight) * 1000.0);
        }
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
    // (XeSS-FG on the tensor cores makes three frames a pair at most - the package refuses more: FgPresent keeps what
    // is MADE to three; round 98: what is shown is what was asked, where the frames are rebuilt.)
    g_fg.failed = false;
    lfOut.force_next = true;
    lfShow.stale = true;
    Trace("[dlssg] asked for %ux", count + 1);
}
