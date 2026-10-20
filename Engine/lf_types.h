// lf_types.h - the engine's own types and constants: the device core, the
// function pointer types of the runtimes it loads, the timing records, the
// pipeline state, and the luma shader that lives in a C string.
#pragma once

// The device and its one command list, recorded on a ring of three
// allocators; the timeline fence says when an allocator may be reused.
struct GpuCore
{
    ID3D12Device              *device;
    ID3D12CommandQueue        *direct_queue;
    ID3D12CommandQueue        *show_queue;      // the output window's own (lf_gpu_core.inl)
    ID3D12GraphicsCommandList *cmd;
    static const int           kRing = 3;
    ID3D12CommandAllocator    *allocators[kRing];
    UINT64                     allocator_mark[kRing];
    int                        ring_slot;
    ID3D12Fence               *timeline;
    HANDLE                     timeline_event;
    UINT64                     timeline_value;
    bool                       ngx_ready;
    NVSDK_NGX_Parameter       *ngx_params;
    NVSDK_NGX_Handle          *ngx_feature;
};
static GpuCore lfCore;

using NrInitFn = NVSDK_NGX_Result (NVSDK_CONV *)(unsigned long long, const wchar_t *, ID3D12Device *, NVSDK_NGX_Version, const NVSDK_NGX_Parameter *);

using NrCreateFn = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList *, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);

using NrEvaluateFn = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback);

using NrReleaseFn = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Handle *);

static constexpr unsigned kNvapiInit = 0x0150E828u;

static constexpr unsigned kNvapiEnumGpus  = 0xE5AC921Fu;

static constexpr unsigned kNvapiArchInfo   = 0xD8265D24u;

static constexpr unsigned kArchTuring    = 0x160u;

static constexpr unsigned kArchAmpere    = 0x170u;

static constexpr unsigned kArchHopper    = 0x180u;

static constexpr unsigned kArchAda       = 0x190u;

static constexpr unsigned kArchBlackwell = 0x1B0u;

struct ArchRecord
{
    unsigned version, architecture, implementation, revision;
};

using NvQueryFn = void *(__cdecl *)(unsigned);

using NvArchFn = int (__cdecl *)(void *, ArchRecord *);

static const int         kArchSlots = 64;

struct FrameTiming {
    double acquire_start, acquired, present_call, source_time;
    UINT64 source_qpc, capture;
    const char *kind = "none";
    UINT64 fence;
    bool reported;
};

enum GpuStage { GS_CONVERT, GS_LUMA, GS_MOVE, GS_SHOW, GS_COUNT, GS_NONE = -1 };

static const char *kStageLabel[GS_COUNT] = { "convert", "luma", "motion", "show" };

typedef HRESULT (WINAPI *D3d12CreateFn)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);

typedef HRESULT (WINAPI *DxgiFactoryFn)(REFIID, void **);

struct StagedTexture
{
    ID3D12Resource *gpu_tex = nullptr;
    ID3D12Resource *staging = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    UINT row_count = 0;
    UINT64 row_bytes = 0;
};

struct Pipeline
{
    // work: the size the motion field is estimated and handed to the generator
    // at. screen: the picture's own size when it differs (stretched).
    UINT work_w = 0, work_h = 0;
    UINT screen_w = 0, screen_h = 0;
    bool stretched = false;
    bool picture_new = true;     // still in the COPY_DEST it was made in
    StagedTexture picture, motion;
    bool fed = false;
};

// ShowStart flag: the output window stays visible to screen capture.
static constexpr uint32_t kScreenCapturable = 0x1u;

typedef HRESULT(WINAPI *CompileFn)(LPCVOID, SIZE_T, LPCSTR, const void *, void *,
                                         LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

typedef HRESULT(WINAPI *SerializeRootFn)(const D3D12_ROOT_SIGNATURE_DESC *,
                                                          D3D_ROOT_SIGNATURE_VERSION,
                                                          ID3DBlob **, ID3DBlob **);

static const char kLumaHlsl[] = R"hlsl(
Texture2D<float4> picture : register(t0);
RWTexture2D<float> lumaOut : register(u0);
cbuffer Extent : register(b0) { uint picW; uint picH; uint lumaW; uint lumaH; };

[numthreads(8, 8, 1)]
void CSMain(uint3 cell : SV_DispatchThreadID)
{
    if (cell.x >= lumaW || cell.y >= lumaH) return;
    float2 span = float2(picW, picH) / float2(lumaW, lumaH);
    float2 from = float2(cell.xy) * span;
    float2 to = min(from + span, float2(picW, picH));
    uint left = (uint)floor(from.x), top = (uint)floor(from.y);
    uint right = min((uint)ceil(to.x), picW), bottom = min((uint)ceil(to.y), picH);
    float acc = 0.0f;
    for (uint row = top; row < bottom; ++row)
        for (uint col = left; col < right; ++col)
        {
            float4 texel = picture.Load(int3(col, row, 0));
            float cover = max(0.0f, min(to.x, col + 1.0f) - max(from.x, (float)col))
                        * max(0.0f, min(to.y, row + 1.0f) - max(from.y, (float)row));
            acc += cover * dot(texel.rgb, float3(0.299f, 0.587f, 0.114f));
        }
    lumaOut[cell.xy] = acc / max((to.x - from.x) * (to.y - from.y), 1e-6f);
}
)hlsl";

enum { PHASE_TAKE, PHASE_SOURCE, PHASE_FEED, PHASE_SHOW, PHASE_WHOLE, PHASE_COUNT };

enum class BridgeResult { Done, NewSize, NewFormat, Error };

namespace wgc_ns = winrt::Windows::Graphics::Capture;

namespace wgdx_ns = winrt::Windows::Graphics::DirectX;

struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
IDxgiAccessBridge : ::IUnknown
{
    virtual HRESULT __stdcall GetInterface(GUID const &id, void **object) = 0;
};

struct FrameDrop
{
    std::mutex mutex;
    wgc_ns::Direct3D11CaptureFrame latest{nullptr};
    winrt::handle arrived{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::atomic<bool> latest_only{false};
    ~FrameDrop() { try { if (latest) latest.Close(); } catch (...) {} }
};

struct WgcLink
{
    wgc_ns::GraphicsCaptureItem item{nullptr};
    wgc_ns::Direct3D11CaptureFramePool pool{nullptr};
    wgc_ns::GraphicsCaptureSession session{nullptr};
    wgdx_ns::Direct3D11::IDirect3DDevice device{nullptr};
    bool hdr = false;
    std::shared_ptr<FrameDrop> mailbox;
    winrt::event_token arrival_token{};
};

static const char *kPhaseLabel[PHASE_COUNT] =
    { "take", "source", "feed", "show", "whole" };

static const double kShowEdges[] = { 5.0, 12.0, 20.0, 28.0, 40.0 };
