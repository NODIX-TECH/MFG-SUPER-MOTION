// lf_state.h - the engine's state, one struct per subject instead of loose
// globals. Each lives exactly once (lfShow, lfCap, ...); nothing here is
// shared with another process.
#pragma once

// The process: how it was started and whether the card is still with us.
struct RunState
{
    LARGE_INTEGER qpc_rate;
    bool gpu_lost;
};
static RunState lfRun;

// The card the generator, the source and the output all share.
struct CardState
{
    int index = -1;
    IDXGIAdapter3 *adapter = nullptr;
};
static CardState lfCard;

// The architecture answer given to the runtime (see ArchPrepare).
struct ArchState
{
    void             *handles[kArchSlots];
    ArchRecord        cache[kArchSlots];
    int               count;
    bool              patched;
    unsigned          real_group;
    void *hook_site;
};
static ArchState lfArch;

// Card-side timings and per-frame records (LF_PHASE=1).
struct TimingState
{
    FrameTiming stamp;
    UINT64 capture_gen, capture_serial, last_source_qpc;
    char rows[256][480];
    unsigned row_count;
    double submit_sum[GS_COUNT], submit_peak[GS_COUNT];
    double wait_sum[GS_COUNT], wait_peak[GS_COUNT];
    double gpu_sum[GS_COUNT], gpu_peak[GS_COUNT];
    double gap_sum[GS_COUNT], gap_peak[GS_COUNT];
    unsigned submit_n[GS_COUNT], wait_n[GS_COUNT], gpu_n[GS_COUNT], gap_n[GS_COUNT];
    unsigned allocator_waits;
    double allocator_wait_sum, allocator_wait_peak;
    unsigned in_flight_peak;
    int stage_now = GS_NONE;
    int slot_stage[GpuCore::kRing] = { GS_NONE, GS_NONE, GS_NONE };
    UINT64 slot_mark[GpuCore::kRing];
    ID3D12QueryHeap *query_heap;
    ID3D12Resource  *query_readback;
    UINT64           tick_rate;
    int              query_state;
    double           query_total;
    double           query_peak;
    unsigned         query_n;
    UINT64 last_gpu_end;
};
static TimingState lfProf;

// CPU-side step timings and counters (LF_PHASE=1).
struct PhaseState
{
    double sum[PHASE_COUNT];
    double peak[PHASE_COUNT];
    unsigned count[PHASE_COUNT];
    unsigned bins[6];
    unsigned requests;
    unsigned fresh_in;
    unsigned evaluated;
    unsigned fresh_evaluated;
    unsigned processed;
    unsigned idle;
    UINT64 window_start;
    int enabled = -1;
};
static PhaseState lfPhase;

// HDR: what the source delivers and how the result goes back on it.
struct HdrState
{
    bool float_frames = false;
    bool keep = false;
    float paper_white = 1.0f;
};
static HdrState lfHdr;

// What was last shown, so repeats can be skipped. The engine shows the
// source's own picture (nothing reworks it), which the generator and the
// luma pass ask about as "sent_passthrough".
struct OutState
{
    uint32_t still_count = 0;
    bool     still_logged = false;
    bool     sent_passthrough = true;
    bool     force_next = false;
};
static OutState lfOut;

// The output window, its swap chain and its thread.
struct ShowState
{
    bool history_restart = true;       // the generator starts its history again with the next frame
    HWND                       hwnd;
    int                        x = 0;
    int                        y = 0;
    std::atomic<bool>          visible{false};
    std::atomic<bool>          revealed{false};
    RECT                       follow_rect = {};
    IDXGISwapChain3           *chain;
    // The frame-latency waitable: the generator's presenter waits on it, or
    // the capture with Low Latency and no generator (lf_latency.inl).
    HANDLE                     quick_waitable = nullptr;
    bool                       low_latency = false;
    DXGI_COLOR_SPACE_TYPE      color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bool                       color_space_set = false;
    HANDLE                     thread;
    DWORD                      thread_id;
    UINT                       w, h;          // the picture's size
    UINT                       bw, bh;        // the swap chain's: the whole screen under a window scaled onto it
    bool                       upscaling = false;   // the picture is scaled onto the display
    bool                       vrr = false;         // VRR: presents wait for the screen (sync 1), never tear
    bool                       tear_asked = false;  // Sync mode "off" when the window was made (lf_vrr.inl) ...
    bool                       tearing = false;     // ... and its swap chain may show a frame in the middle of a refresh
    bool                       capturable;
    uint32_t                   flags = 0;
    bool                       stale = false;
    volatile LONG              state;
};
static ShowState lfShow;

// The source: duplication, window or screen capture, and the D3D11->D3D12 bridge.
struct CaptureState
{
    HMONITOR monitor = nullptr;
    HdrDisplayState display;
    bool                       window_live = false;
    bool                       window_dxgi = false;
    RECT                       window_crop = {};
    HWND                       window = nullptr;
    bool                    opened_hdr = false;
    bool                    upside_down = false;
    bool                    reopen_tried = false;
    bool                    live = false;
    bool                    picture_changed = false;
    double                  stamp = 0.0;
    UINT                    w = 0, h = 0;
    ID3D11Device           *d11 = nullptr;
    ID3D11DeviceContext    *d11_ctx = nullptr;
    IDXGIOutputDuplication *duplication = nullptr;
    ID3D11Texture2D        *bridge11 = nullptr;
    HANDLE                  bridge_handle = nullptr;
    ID3D12Resource         *bridge12 = nullptr;
    ID3D12Resource         *rgba = nullptr;
    ID3D12Fence            *sync12 = nullptr;
    ID3D11Fence            *sync11 = nullptr;
    HANDLE                  sync_event = nullptr;
    UINT64                  sync_value = 1;
    bool                    has_frame = false;
    bool screen_via_wgc = false;
    ID3D12RootSignature  *swz_root = nullptr;
    ID3D12PipelineState  *swz_pso = nullptr;
    ID3D12DescriptorHeap *swz_heap = nullptr;
    bool luma_ok = true;
    WgcLink *window_link = nullptr;
    WgcLink *screen_link = nullptr;
};
static CaptureState lfCap;

// The small luma (scene cut, repeated frames) and NVIDIA Optical Flow's.
struct LumaState
{
    ID3D12DescriptorHeap *heap = nullptr; // separate from capture conversion
    BYTE                   *view = nullptr;
    size_t                  size = 0;
    UINT                    w = 0, h = 0;
    UINT                    pitch = 0;
    ID3D12Resource         *readback = nullptr;
    ID3D12Resource         *target = nullptr;
    bool                    mapped = false;
    ID3D12RootSignature  *root = nullptr;
    ID3D12PipelineState  *pso = nullptr;
    ID3D12Resource       *flow_tex;
    ID3D12DescriptorHeap *flow_heap;
    UINT                  flow_w, flow_h;
};
static LumaState lfLuma;


// What kept frames in between from the screen, counted by whoever decides it
// and told every few seconds (lf_dlssg.inl, the presenter's [steady] line):
// "it works, then it breaks up, then it comes back" has to be read off the log.
struct SteadyCount
{
    std::atomic<unsigned> pairs{0};       // pairs handed to the presenter
    std::atomic<unsigned> asked{0};       // frames in between they were to get
    std::atomic<unsigned> rested{0};      // pairs with none: the game fills the screen by itself
    std::atomic<unsigned> too_fast{0};    // pairs with none: the picture moved too fast (the guard)
    std::atomic<unsigned> cut{0};         // pairs with none: a cut, a pause, the first after a start
    std::atomic<unsigned> repeats{0};     // desktop frames that were the picture before again (or a corner of another program's)
    std::atomic<unsigned> let_go{0};      // of those, let go untouched: expected
    std::atomic<unsigned> no_room{0};     // real frames that found every place taken
    std::atomic<unsigned> changes{0};     // times the count of frames in between a pair changed
    // Does the loop keep up with the game? (sm_loop.inl, FrameStep)
    std::atomic<unsigned> taken{0};       // new frames of the game taken
    std::atomic<unsigned> waiting{0};     // of those, already on the desktop when the loop came for the next one
    std::atomic<unsigned> desktop{0};     // frames the desktop drew for them (Desktop Duplication's count)
    std::atomic<unsigned> busy_us{0};     // what the loop was busy with them, microseconds
    std::atomic<unsigned> fence_us{0};    // of that, waiting for the generator's work on the card
};
// Frames the desktop drew since the one before (Desktop Duplication); 0 when the source does not say.
static unsigned g_desk_accumulated = 0;
static SteadyCount g_steady;

// The last conversion of a captured frame sent without being waited for
// (lf_desktop.inl): done before the shared texture it reads is written again.
static UINT64 g_convert_mark = 0;
