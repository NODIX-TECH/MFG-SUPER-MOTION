// msm_ai.h - SMFG AI (round 52): SMFG's frames in between made by a neural network on the card's AI units.
//
// What SMFG 1.1 does by searching blocks (msm_fg.h), this does with RIFE (Practical-RIFE 4.25 lite, MIT,
// hzwer/Practical-RIFE): a small network that, given two frames and a moment between them, says where every pixel
// of the frame at that moment comes from in each of the two (its "flow") and how much of each to take (its "mask").
// Its convolutions run through ONNX Runtime with DirectML - Microsoft's way to a card's matrix units on any maker's
// card: NVIDIA's tensor cores (RTX), AMD's AI accelerators (RDNA 3 and later), Intel's XMX (Arc); a card without
// them runs them on its shaders. In half floats, the matrix units' own.
//
// Measured before it was built (tests\msmtruth.exe truth=1, ai\score.py; RTX 3060, 2560x1440): the share of pixels
// clearly wrong - street tilting 1.44% (SMFG 1.1) -> 1.31%, driving 1.95% -> 1.66%, a photograph rushing past a car
// standing on the screen 0.52% -> 0.21%; pixels off by themselves (a picture "coming apart") 3 to 10 times fewer.
//
// The network whole, through DirectML, took 24 ms a frame on an RTX 3060: its convolutions are cheap, but between
// them it moves, scales and joins pictures at full size, which DirectML does slowly. So it is cut (ai\split_rife.py,
// checked to give the network's own flow and mask to the last bit): DirectML runs only the convolutions - an
// encoder once a real frame (head) and five stages at 1/32, 1/16, 1/8, 1/4 and the whole of the network's size
// (block0..4) - and the engine does the rest in its own passes:
//   Prep   each real frame into the network's size (kModelW x kModelH, the picture fitted in, the rest its edge).
//   Gather a stage's input: both frames and their features moved by the flow so far, the moment, the mask and the
//          features so far, and the flow itself - all scaled down to the stage's size (as the network's own
//          bilinear resize does).
//   Up     a stage's output scaled up to the network's size: its flow added to the flow so far, its mask, its
//          features.
//   Warp   each frame in between at the picture's full size: both real frames moved by the flow (scaled up) and
//          mixed by the mask. Nothing of the frames is made small - only where things go.
// 6.4 ms of the RTX 3060 a frame in between where the whole network took 24 (the encoder 1.7 of it, once a frame).
// Live (tests\pace.ps1, x2, a stand-in game sharing the card): 10 ms of the card a pair against SMFG 1.1's 1.7 - the game
// kept 69 FPS of 148 (SMFG 1.1: 144) at a light load, 50 of 84 at a heavy one: for strong cards, x2 or x3.
//
// The engine's list is sent before the network runs (it runs after it on the same queue), and the passes between
// its stages go in small lists of this file's own, sent in turn on that queue. Files (the engine's folder, ai\):
// onnxruntime.dll (MIT), DirectML.dll (Microsoft's, may be shipped with an application), smfg-ai-head.onnx and
// smfg-ai-block0..4.onnx (RIFE's network, its licence beside them). Any of them missing, or refused on this card:
// SMFG 1.1 makes the frames, and the log says why.

#include "include/ort/onnxruntime_c_api.h"
#include "include/ort/dml_provider_factory.h"

namespace msmai
{
    static constexpr UINT kModelW = 1280, kModelH = 768;    // the size the network was cut for (multiples of 128)
    static constexpr UINT kMost = 5;                          // frames in between a pair at most (x6)
    static constexpr UINT kStages = 5;
    static constexpr UINT kScale[kStages] = { 32, 16, 8, 4, 1 };
    static constexpr UINT kTables = 512;                      // descriptor tables in turn (passes in flight)
    static constexpr UINT kSlots = 15;                        // a table: 2 frames, 8 buffers read, 4 written, 1 picture written
    static constexpr UINT kLists = 32;                        // this file's own lists in turn

    static const char kHlsl[] = R"(
cbuffer K : register(b0) { float4 a; float4 b; float4 c; float4 d; };
Texture2D<float4> Frame0 : register(t0);
Texture2D<float4> Frame1 : register(t1);
Buffer<float> B0 : register(t2);
Buffer<float> B1 : register(t3);
Buffer<float> B2 : register(t4);
Buffer<float> B3 : register(t5);
Buffer<float> B4 : register(t6);
Buffer<float> B5 : register(t7);
Buffer<float> B6 : register(t8);
Buffer<float> B7 : register(t9);
RWBuffer<float> U0 : register(u0);
RWBuffer<float> U1 : register(u1);
RWBuffer<float> U2 : register(u2);
RWBuffer<float> U3 : register(u3);
RWTexture2D<float4> Into : register(u4);
SamplerState Lin : register(s0);

// Prep. a: content w, h (the picture inside the network's size), network w, h; b: frame w, h, taps across, linear
// light (1). Writes U0: three planes of the network's size.
[numthreads(8, 8, 1)]
void PrepMain(uint3 id : SV_DispatchThreadID)
{
    const uint mw = (uint)a.z, mh = (uint)a.w;
    if (id.x >= mw || id.y >= mh) return;
    const float cx = min((float)id.x, a.x - 1.0), cy = min((float)id.y, a.y - 1.0);
    const int n = max(1, (int)b.z);
    float3 sum = 0;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
        {
            const float2 at = float2((cx + (i + 0.5) / n) / a.x, (cy + (j + 0.5) / n) / a.y);
            sum += Frame0.SampleLevel(Lin, at, 0).rgb;
        }
    float3 v = sum / (n * n);
    if (b.w > 0.5) { v = max(v, 0); v = pow(v / (1.0 + v), 1.0 / 2.2); }
    v = saturate(v);
    const uint plane = mw * mh, at = id.y * mw + id.x;
    U0[at] = v.r; U0[plane + at] = v.g; U0[2 * plane + at] = v.b;
}

// The network's size, for Gather and Up.
#define MW ((int)a.x)
#define MH ((int)a.y)
#define PL (MW * MH)
#define AT(x, y) ((y) * MW + (x))

// Bilinear, at a place in pixels of the network's size, its edge going on (grid_sample: border, corners aligned).
#define SAMPLER(name, buf) \
float name(int plane, float2 p) \
{ \
    p = clamp(p, float2(0, 0), float2(MW - 1, MH - 1)); \
    const int x0 = (int)floor(p.x), y0 = (int)floor(p.y); \
    const int x1 = min(x0 + 1, MW - 1), y1 = min(y0 + 1, MH - 1); \
    const float fx = p.x - x0, fy = p.y - y0; \
    const int o = plane * PL; \
    return lerp(lerp(buf[o + AT(x0, y0)], buf[o + AT(x1, y0)], fx), lerp(buf[o + AT(x0, y1)], buf[o + AT(x1, y1)], fx), fy); \
}
SAMPLER(Img0, B0)
SAMPLER(Img1, B1)
SAMPLER(Feat0, B2)
SAMPLER(Feat1, B3)

// Gather (a stage's input). a: network w, h, stage w, h; b: scale s, first stage (1), moment t.
// Reads B0, B1 the frames (3 planes), B2, B3 their features (4), B4 the flow so far (4), B5 the mask so far (1),
// B6 the features so far (8). Writes U0: 15 planes (the first stage) or 28, of the stage's size.
void Source(int2 q, bool first, float t, float s, inout float v[28])
{
    const float2 at = (float2)q;
    if (first)
    {
        int k = 0;
        [unroll] for (int i = 0; i < 3; ++i) v[k++] = Img0(i, at);
        [unroll] for (int i = 0; i < 3; ++i) v[k++] = Img1(i, at);
        [unroll] for (int i = 0; i < 4; ++i) v[k++] = Feat0(i, at);
        [unroll] for (int i = 0; i < 4; ++i) v[k++] = Feat1(i, at);
        v[k] = t;
        return;
    }
    const int o = AT(q.x, q.y);
    const float4 fl = float4(B4[o], B4[PL + o], B4[2 * PL + o], B4[3 * PL + o]);
    const float2 p0 = at + fl.xy, p1 = at + fl.zw;
    int k = 0;
    [unroll] for (int i = 0; i < 3; ++i) v[k++] = Img0(i, p0);
    [unroll] for (int i = 0; i < 3; ++i) v[k++] = Img1(i, p1);
    [unroll] for (int i = 0; i < 4; ++i) v[k++] = Feat0(i, p0);
    [unroll] for (int i = 0; i < 4; ++i) v[k++] = Feat1(i, p1);
    v[k++] = t;
    v[k++] = B5[o];
    [unroll] for (int i = 0; i < 8; ++i) v[k++] = B6[i * PL + o];
    v[k++] = fl.x / s; v[k++] = fl.y / s; v[k++] = fl.z / s; v[k] = fl.w / s;
}
[numthreads(8, 8, 1)]
void GatherMain(uint3 id : SV_DispatchThreadID)
{
    const int sw = (int)a.z, sh = (int)a.w;
    if ((int)id.x >= sw || (int)id.y >= sh) return;
    const float s = b.x;
    const bool first = b.y > 0.5;
    const int channels = first ? 15 : 28;
    // (A resize to 1/s, bilinear, corners not aligned - the network's own F.interpolate.)
    const float sx = max((id.x + 0.5) * s - 0.5, 0.0), sy = max((id.y + 0.5) * s - 0.5, 0.0);
    const int x0 = min((int)floor(sx), MW - 1), y0 = min((int)floor(sy), MH - 1);
    const int x1 = min(x0 + 1, MW - 1), y1 = min(y0 + 1, MH - 1);
    const float fx = sx - x0, fy = sy - y0;
    float v00[28], v10[28], v01[28], v11[28];
    [unroll] for (int i = 0; i < 28; ++i) { v00[i] = 0; v10[i] = 0; v01[i] = 0; v11[i] = 0; }
    Source(int2(x0, y0), first, b.z, s, v00);
    Source(int2(x1, y0), first, b.z, s, v10);
    Source(int2(x0, y1), first, b.z, s, v01);
    Source(int2(x1, y1), first, b.z, s, v11);
    const int plane = sw * sh, at = id.y * sw + id.x;
    [unroll] for (int i = 0; i < 28; ++i)
        if (i < channels) U0[i * plane + at] = lerp(lerp(v00[i], v10[i], fx), lerp(v01[i], v11[i], fx), fy);
}

// Up (a stage's output into the network's size). a: network w, h, stage w, h; b: scale s, first stage (1).
// Reads B0: the stage's 13 planes. Writes U0 the flow (4 planes: added to, or anew at the first stage), U1 the mask,
// U2 the features (8).
[numthreads(8, 8, 1)]
void UpMain(uint3 id : SV_DispatchThreadID)
{
    if ((int)id.x >= MW || (int)id.y >= MH) return;
    const int sw = (int)a.z, sh = (int)a.w;
    const float s = b.x;
    const float sx = max((id.x + 0.5) / s - 0.5, 0.0), sy = max((id.y + 0.5) / s - 0.5, 0.0);
    const int x0 = min((int)floor(sx), sw - 1), y0 = min((int)floor(sy), sh - 1);
    const int x1 = min(x0 + 1, sw - 1), y1 = min(y0 + 1, sh - 1);
    const float fx = sx - x0, fy = sy - y0;
    const int plane = sw * sh;
    float v[13];
    [unroll] for (int i = 0; i < 13; ++i)
    {
        const int o = i * plane;
        v[i] = lerp(lerp(B0[o + y0 * sw + x0], B0[o + y0 * sw + x1], fx), lerp(B0[o + y1 * sw + x0], B0[o + y1 * sw + x1], fx), fy);
    }
    const int o = AT(id.x, id.y);
    const bool first = b.y > 0.5;
    [unroll] for (int i = 0; i < 4; ++i) U0[i * PL + o] = (first ? 0.0 : U0[i * PL + o]) + v[i] * s;
    U1[o] = v[4];
    [unroll] for (int i = 0; i < 8; ++i) U2[i * PL + o] = v[5 + i];
}

// Warp (a frame in between at full size). a: frame w, h, content w, h; b: network w, h.
// Reads B0 the flow (4 planes of the network's size), B1 the mask (1, before its sigmoid).
float FlowAt(int plane, int x, int y)
{
    x = clamp(x, 0, (int)a.z - 1); y = clamp(y, 0, (int)a.w - 1);
    return B0[plane * (int)b.x * (int)b.y + y * (int)b.x + x];
}
float MaskAt(int x, int y)
{
    x = clamp(x, 0, (int)a.z - 1); y = clamp(y, 0, (int)a.w - 1);
    return B1[y * (int)b.x + x];
}
[numthreads(8, 8, 1)]
void WarpMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)a.x || id.y >= (uint)a.y) return;
    const float kx = a.z / a.x, ky = a.w / a.y;
    const float mx = (id.x + 0.5) * kx - 0.5, my = (id.y + 0.5) * ky - 0.5;
    const int x0 = (int)floor(mx), y0 = (int)floor(my);
    const float fx = mx - x0, fy = my - y0;
    float4 f; float m;
    {
        float4 v00 = float4(FlowAt(0, x0, y0), FlowAt(1, x0, y0), FlowAt(2, x0, y0), FlowAt(3, x0, y0));
        float4 v10 = float4(FlowAt(0, x0 + 1, y0), FlowAt(1, x0 + 1, y0), FlowAt(2, x0 + 1, y0), FlowAt(3, x0 + 1, y0));
        float4 v01 = float4(FlowAt(0, x0, y0 + 1), FlowAt(1, x0, y0 + 1), FlowAt(2, x0, y0 + 1), FlowAt(3, x0, y0 + 1));
        float4 v11 = float4(FlowAt(0, x0 + 1, y0 + 1), FlowAt(1, x0 + 1, y0 + 1), FlowAt(2, x0 + 1, y0 + 1), FlowAt(3, x0 + 1, y0 + 1));
        f = lerp(lerp(v00, v10, fx), lerp(v01, v11, fx), fy);
        m = lerp(lerp(MaskAt(x0, y0), MaskAt(x0 + 1, y0), fx), lerp(MaskAt(x0, y0 + 1), MaskAt(x0 + 1, y0 + 1), fx), fy);
    }
    m = 1.0 / (1.0 + exp(-m));
    // The flow is in pixels of the network's size: in pixels of the frame, scaled up.
    const float2 up = float2(1.0 / kx, 1.0 / ky);
    const float2 p = id.xy + 0.5;
    const float2 inv = float2(1.0 / a.x, 1.0 / a.y);
    const float4 c0 = Frame0.SampleLevel(Lin, (p + f.xy * up) * inv, 0);
    const float4 c1 = Frame1.SampleLevel(Lin, (p + f.zw * up) * inv, 0);
    Into[id.xy] = float4(lerp(c1.rgb, c0.rgb, m), 1.0);
}
)";

    // A buffer on the card; for the network, also its view of it (half floats).
    struct Buffer
    {
        winrt::com_ptr<ID3D12Resource> res;
        DXGI_FORMAT format = DXGI_FORMAT_R16_FLOAT;
        UINT count = 0;                     // numbers in it
        void *alloc = nullptr;              // DirectML's view of it
        OrtValue *value = nullptr;
    };

    static struct State
    {
        bool tried = false, failed = false;
        std::string why;
        HMODULE ort = nullptr, dml = nullptr;
        const OrtApi *api = nullptr;
        const OrtDmlApi *dml_api = nullptr;
        OrtEnv *env = nullptr;
        OrtSession *head = nullptr, *stage[kStages] = {};
        OrtMemoryInfo *gpu_mem = nullptr;
        OrtRunOptions *run = nullptr;
        IUnknown *dml_device = nullptr;
        winrt::com_ptr<ID3D12RootSignature> root;
        winrt::com_ptr<ID3D12PipelineState> prep, gather, up, warp;
        winrt::com_ptr<ID3D12DescriptorHeap> heap;
        UINT table = 0;
        // The network's buffers (half floats): the two real frames and their features; each stage's input and output.
        Buffer img[2], feat[2], x[kStages], tmp[kStages];
        // The engine's own (floats): the flow and mask of each frame in between, the features between stages.
        Buffer flow[kMost], mask[kMost], between;
        // This file's own lists.
        winrt::com_ptr<ID3D12CommandAllocator> allocs[kLists];
        winrt::com_ptr<ID3D12GraphicsCommandList> lists[kLists];
        winrt::com_ptr<ID3D12Fence> fence;
        UINT64 marks[kLists] = {}, fenced = 0;
        UINT list_at = 0;
        HANDLE event = nullptr;
        ID3D12Resource *last_after = nullptr;   // the real frame the last pair ended on (its input in last_slot)
        UINT last_slot = 0;
        UINT full_w = 0, full_h = 0, sw = 0, sh = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        // For the log.
        double cpu_ms = 0.0; unsigned pairs = 0; ULONGLONG told = 0;
    } g;

    static bool Fail(const char *what, OrtStatus *status = nullptr)
    {
        g.failed = true;
        g.why = what;
        if (status && g.api) { g.why += ": "; g.why += g.api->GetErrorMessage(status); g.api->ReleaseStatus(status); }
        Trace("[ai] SMFG AI is not available (%s) - SMFG 1.1 makes the frames", g.why.c_str());
        return false;
    }
#define AI_TRY(call, what) do { if (OrtStatus *st_ = (call)) return Fail(what, st_); } while (0)

    static std::wstring Folder()
    {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring dir(self);
        dir.resize(dir.find_last_of(L'\\') + 1);
        return dir + L"ai\\";
    }

    // A buffer of `count` numbers; `shape` given: the network's half floats, with its view of them.
    static bool Make(Buffer &b, UINT count, const int64_t *shape = nullptr, size_t dims = 0)
    {
        b.count = count;
        b.format = shape ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R32_FLOAT;
        const UINT64 bytes = UINT64(count) * (shape ? 2 : 4);
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = (bytes + 255) & ~255ull;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                          IID_PPV_ARGS(b.res.put()))))
            return Fail("a buffer on the card");
        if (!shape) return true;
        AI_TRY(g.dml_api->CreateGPUAllocationFromD3DResource(b.res.get(), &b.alloc), "the network's view of a buffer");
        AI_TRY(g.api->CreateTensorWithDataAsOrtValue(g.gpu_mem, b.alloc, bytes, shape, dims, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16, &b.value),
               "a tensor on the card");
        return true;
    }

    static bool Session(const std::wstring &file, OrtSession **into)
    {
        OrtSessionOptions *options = nullptr;
        AI_TRY(g.api->CreateSessionOptions(&options), "its options");
        g.api->DisableMemPattern(options);
        g.api->SetSessionExecutionMode(options, ORT_SEQUENTIAL);
        g.api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL);
        if (OrtStatus *st = g.dml_api->SessionOptionsAppendExecutionProvider_DML1(options, reinterpret_cast<IDMLDevice *>(g.dml_device), lfCore.direct_queue))
        { g.api->ReleaseSessionOptions(options); return Fail("DirectML on the engine's queue", st); }
        if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) { g.api->ReleaseSessionOptions(options); return Fail("a file of the network is missing in ai\\"); }
        OrtStatus *made = g.api->CreateSession(g.env, file.c_str(), options, into);
        g.api->ReleaseSessionOptions(options);
        if (made) return Fail("the network on this card", made);
        return true;
    }

    // Everything once: the libraries, the network on this card, the passes. False: SMFG 1.1 goes on.
    static bool Start()
    {
        if (g.tried) return !g.failed;
        g.tried = true;
        const std::wstring dir = Folder();
        // (DirectML first, by its full path: the one beside the runtime, not an older one of Windows'.)
        g.dml = LoadLibraryExW((dir + L"DirectML.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        g.ort = g.dml ? LoadLibraryExW((dir + L"onnxruntime.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) : nullptr;
        if (!g.dml || !g.ort) return Fail("ai\\DirectML.dll or ai\\onnxruntime.dll is missing");
        auto base = reinterpret_cast<const OrtApiBase *(ORT_API_CALL *)()>(GetProcAddress(g.ort, "OrtGetApiBase"));
        auto make_device = reinterpret_cast<HRESULT(WINAPI *)(ID3D12Device *, UINT, REFIID, void **)>(GetProcAddress(g.dml, "DMLCreateDevice"));
        if (!base || !make_device) return Fail("the libraries are not the ones expected");
        g.api = base()->GetApi(ORT_API_VERSION);
        if (!g.api) return Fail("ONNX Runtime too old");
        AI_TRY(g.api->GetExecutionProviderApi("DML", ORT_API_VERSION, reinterpret_cast<const void **>(&g.dml_api)), "DirectML in ONNX Runtime");
        // {6dbd6437-96fd-423f-a98c-ae5e7c2a573f}: IDMLDevice
        static const GUID kDmlDevice = {0x6dbd6437, 0x96fd, 0x423f, {0xa9, 0x8c, 0xae, 0x5e, 0x7c, 0x2a, 0x57, 0x3f}};
        if (FAILED(make_device(lfCore.device, 0, kDmlDevice, reinterpret_cast<void **>(&g.dml_device))) || !g.dml_device)
            return Fail("DirectML on this card");
        AI_TRY(g.api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "smfg-ai", &g.env), "its environment");
        const double t0 = LatencyNow();
        if (!Session(dir + L"smfg-ai-head.onnx", &g.head)) return false;
        for (UINT i = 0; i < kStages; ++i)
            if (!Session(dir + L"smfg-ai-block" + std::to_wstring(i) + L".onnx", &g.stage[i])) return false;
        AI_TRY(g.api->CreateMemoryInfo("DML", OrtDeviceAllocator, 0, OrtMemTypeDefault, &g.gpu_mem), "its memory");
        AI_TRY(g.api->CreateRunOptions(&g.run), "its run options");
        g.api->AddRunConfigEntry(g.run, "disable_synchronize_execution_providers", "1");
        const UINT plane = kModelW * kModelH;
        const int64_t image[4] = {1, 3, kModelH, kModelW}, features[4] = {1, 4, kModelH, kModelW};
        for (int i = 0; i < 2; ++i)
            if (!Make(g.img[i], 3 * plane, image, 4) || !Make(g.feat[i], 4 * plane, features, 4)) return false;
        for (UINT i = 0; i < kStages; ++i)
        {
            const int64_t w = kModelW / kScale[i], h = kModelH / kScale[i];
            const int64_t in[4] = {1, i == 0 ? 15 : 28, h, w}, out[4] = {1, 13, h, w};
            if (!Make(g.x[i], UINT(in[1] * h * w), in, 4) || !Make(g.tmp[i], UINT(13 * h * w), out, 4)) return false;
        }
        for (UINT k = 0; k < kMost; ++k) if (!Make(g.flow[k], 4 * plane) || !Make(g.mask[k], plane)) return false;
        if (!Make(g.between, 8 * plane)) return false;
        // The passes.
        D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 10, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 5, 0}};
        ranges[1].OffsetInDescriptorsFromTableStart = 10;
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 16;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {2, ranges};
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxAnisotropy = 1; sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS; sampler.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC desc = {2, params, 1, &sampler};
        winrt::com_ptr<ID3DBlob> signature, errors;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, signature.put(), errors.put())) ||
            FAILED(lfCore.device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(g.root.put()))))
            return Fail("its root signature");
        for (auto pass : { std::make_pair("PrepMain", &g.prep), std::make_pair("GatherMain", &g.gather), std::make_pair("UpMain", &g.up),
                           std::make_pair("WarpMain", &g.warp) })
        {
            winrt::com_ptr<ID3DBlob> code;
            errors = nullptr;
            if (FAILED(CachedCompile(kHlsl, sizeof(kHlsl) - 1, "smfg-ai.hlsl", nullptr, nullptr, pass.first, "cs_5_0",
                                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put())))
                return Fail(errors ? static_cast<const char *>(errors->GetBufferPointer()) : "a pass did not compile");
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
            pso.pRootSignature = g.root.get();
            pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
            if (FAILED(lfCore.device->CreateComputePipelineState(&pso, IID_PPV_ARGS(pass.second->put())))) return Fail("a pass's pipeline");
        }
        D3D12_DESCRIPTOR_HEAP_DESC heap = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kTables * kSlots, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
        if (FAILED(lfCore.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(g.heap.put())))) return Fail("its descriptor heap");
        for (UINT i = 0; i < kLists; ++i)
            if (FAILED(lfCore.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(g.allocs[i].put()))) ||
                FAILED(lfCore.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocs[i].get(), nullptr, IID_PPV_ARGS(g.lists[i].put()))) ||
                FAILED(g.lists[i]->Close()))
                return Fail("its command lists");
        if (FAILED(lfCore.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(g.fence.put())))) return Fail("its fence");
        g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        DXGI_ADAPTER_DESC1 card = {};
        if (lfCard.adapter) lfCard.adapter->GetDesc1(&card);
        Trace("[ai] SMFG AI ready on %ls: RIFE 4.25 lite (MIT) - its convolutions through DirectML in half floats, on the card's matrix "
              "units (tensor cores, AI accelerators, XMX) where it has them; the rest the engine's own passes; at %ux%u (%.1f s to set up)",
              card.Description, kModelW, kModelH, LatencyNow() - t0);
        return true;
    }

    // A pass: what it reads and writes, its numbers.
    struct Pass
    {
        ID3D12Resource *frame[2] = {};
        const Buffer *read[8] = {}, *write[4] = {};
        ID3D12Resource *into = nullptr;
        float k[16] = {};
        UINT w = 0, h = 0;
    };
    static void Record(ID3D12GraphicsCommandList *list, ID3D12PipelineState *state, const Pass &p)
    {
        const UINT each = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const UINT first = (g.table++ % kTables) * kSlots;
        D3D12_CPU_DESCRIPTOR_HANDLE at = g.heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = g.heap->GetGPUDescriptorHandleForHeapStart();
        at.ptr += static_cast<SIZE_T>(first) * each;
        gpu.ptr += static_cast<UINT64>(first) * each;
        D3D12_SHADER_RESOURCE_VIEW_DESC tex = {};
        tex.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        tex.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        tex.Texture2D.MipLevels = 1;
        tex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        for (int i = 0; i < 2; ++i, at.ptr += each) lfCore.device->CreateShaderResourceView(p.frame[i], p.frame[i] ? nullptr : &tex, at);
        for (int i = 0; i < 8; ++i, at.ptr += each)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC buf = {};
            buf.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            buf.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            buf.Format = p.read[i] ? p.read[i]->format : DXGI_FORMAT_R32_FLOAT;
            buf.Buffer.NumElements = p.read[i] ? p.read[i]->count : 1;
            lfCore.device->CreateShaderResourceView(p.read[i] ? p.read[i]->res.get() : nullptr, &buf, at);
        }
        for (int i = 0; i < 4; ++i, at.ptr += each)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC buf = {};
            buf.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            buf.Format = p.write[i] ? p.write[i]->format : DXGI_FORMAT_R32_FLOAT;
            buf.Buffer.NumElements = p.write[i] ? p.write[i]->count : 1;
            lfCore.device->CreateUnorderedAccessView(p.write[i] ? p.write[i]->res.get() : nullptr, nullptr, &buf, at);
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC utex = {};
        utex.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        utex.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        lfCore.device->CreateUnorderedAccessView(p.into, nullptr, p.into ? nullptr : &utex, at);
        ID3D12DescriptorHeap *heaps[] = { g.heap.get() };
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(g.root.get());
        list->SetPipelineState(state);
        list->SetComputeRoot32BitConstants(0, 16, p.k, 0);
        list->SetComputeRootDescriptorTable(1, gpu);
        list->Dispatch((p.w + 7) / 8, (p.h + 7) / 8, 1);
    }

    // Buffers read as shader resources between being written as unordered access (the state the network wants
    // them in): to be read, and back.
    static void Readable(ID3D12GraphicsCommandList *list, std::initializer_list<const Buffer *> buffers, bool to)
    {
        D3D12_RESOURCE_BARRIER b[16];
        UINT n = 0;
        for (const Buffer *r : buffers)
            b[n++] = to ? Barrier(r->res.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                        : Barrier(r->res.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(n, b);
    }

    // This file's next list, open; and sent.
    static ID3D12GraphicsCommandList *Open()
    {
        const UINT i = g.list_at++ % kLists;
        if (g.fence->GetCompletedValue() < g.marks[i])
        {
            g.fence->SetEventOnCompletion(g.marks[i], g.event);
            WaitForSingleObject(g.event, 2000);
        }
        g.allocs[i]->Reset();
        g.lists[i]->Reset(g.allocs[i].get(), nullptr);
        return g.lists[i].get();
    }
    static bool Send(ID3D12GraphicsCommandList *list)
    {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList *lists[] = { list };
        lfCore.direct_queue->ExecuteCommandLists(1, lists);
        g.marks[(g.list_at - 1) % kLists] = ++g.fenced;
        return SUCCEEDED(lfCore.direct_queue->Signal(g.fence.get(), g.fenced));
    }

    static bool Run(OrtSession *session, const char *in, const Buffer &input, const char *out, const Buffer &output)
    {
        OrtIoBinding *binding = nullptr;
        AI_TRY(g.api->CreateIoBinding(session, &binding), "binding");
        OrtStatus *st = g.api->BindInput(binding, in, input.value);
        if (!st) st = g.api->BindOutput(binding, out, output.value);
        if (!st) st = g.api->RunWithBinding(session, g.run, binding);
        g.api->ReleaseIoBinding(binding);
        if (st) return Fail("the network's run", st);
        return true;
    }

    // The real frame into the network's input `slot` (frames readable as shader resources).
    static void Prep(ID3D12GraphicsCommandList *list, ID3D12Resource *frame, UINT slot)
    {
        Pass p;
        p.frame[0] = frame;
        p.write[0] = &g.img[slot];
        const float taps = std::ceil(std::max(float(g.full_w) / g.sw, float(g.full_h) / g.sh) - 0.01f);
        const bool linear = g.format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        const float k[16] = { float(g.sw), float(g.sh), float(kModelW), float(kModelH), float(g.full_w), float(g.full_h), std::clamp(taps, 1.0f, 4.0f),
                              linear ? 1.0f : 0.0f };
        memcpy(p.k, k, sizeof(k));
        p.w = kModelW; p.h = kModelH;
        Record(list, g.prep.get(), p);
    }

    // The next pair does not go on from the last one (a cut, a rest, a frame let go): its frame before is prepared anew.
    static void Forget() { g.last_after = nullptr; }

    // The network's size for pictures of this size: the picture fitted in.
    static void Size(UINT full_w, UINT full_h, DXGI_FORMAT format)
    {
        if (g.full_w == full_w && g.full_h == full_h && g.format == format) return;
        const float k = std::min(1.0f, std::min(float(kModelW) / full_w, float(kModelH) / full_h));
        g.full_w = full_w; g.full_h = full_h; g.format = format;
        g.sw = std::max(16u, std::min(kModelW, static_cast<UINT>(std::lround(full_w * k))));
        g.sh = std::max(16u, std::min(kModelH, static_cast<UINT>(std::lround(full_h * k))));
        g.last_after = nullptr;
        Trace("[ai] the picture %ux%u is looked at as %ux%u of the network's %ux%u", full_w, full_h, g.sw, g.sh, kModelW, kModelH);
    }

    // One frame in between's flow and mask (into flow[k], mask[k]): the five stages, each with the engine's passes
    // around it in a list of this file's own.
    static bool Stages(UINT k, UINT b_slot, UINT a_slot, float moment)
    {
        for (UINT i = 0; i < kStages; ++i)
        {
            const float s = float(kScale[i]);
            const UINT w = kModelW / kScale[i], h = kModelH / kScale[i];
            ID3D12GraphicsCommandList *list = Open();
            // The stage before's output, scaled up (the flow added to).
            if (i > 0)
            {
                Readable(list, { &g.tmp[i - 1] }, true);
                Pass up;
                up.read[0] = &g.tmp[i - 1];
                up.write[0] = &g.flow[k]; up.write[1] = &g.mask[k]; up.write[2] = &g.between;
                const float ku[16] = { float(kModelW), float(kModelH), float(kModelW / kScale[i - 1]), float(kModelH / kScale[i - 1]),
                                       float(kScale[i - 1]), i == 1 ? 1.0f : 0.0f };
                memcpy(up.k, ku, sizeof(ku));
                up.w = kModelW; up.h = kModelH;
                Record(list, g.up.get(), up);
                Readable(list, { &g.tmp[i - 1] }, false);
                D3D12_RESOURCE_BARRIER all = {};
                all.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                list->ResourceBarrier(1, &all);
            }
            // This stage's input.
            Readable(list, { &g.img[b_slot], &g.img[a_slot], &g.feat[b_slot], &g.feat[a_slot], &g.flow[k], &g.mask[k], &g.between }, true);
            Pass gather;
            gather.read[0] = &g.img[b_slot]; gather.read[1] = &g.img[a_slot];
            gather.read[2] = &g.feat[b_slot]; gather.read[3] = &g.feat[a_slot];
            gather.read[4] = &g.flow[k]; gather.read[5] = &g.mask[k]; gather.read[6] = &g.between;
            gather.write[0] = &g.x[i];
            const float kg[16] = { float(kModelW), float(kModelH), float(w), float(h), s, i == 0 ? 1.0f : 0.0f, moment };
            memcpy(gather.k, kg, sizeof(kg));
            gather.w = w; gather.h = h;
            Record(list, g.gather.get(), gather);
            Readable(list, { &g.img[b_slot], &g.img[a_slot], &g.feat[b_slot], &g.feat[a_slot], &g.flow[k], &g.mask[k], &g.between }, false);
            if (!Send(list)) return Fail("a list of its own");
            if (!Run(g.stage[i], "x", g.x[i], "tmp", g.tmp[i])) return false;
        }
        // The last stage's output: the frame's flow and mask.
        ID3D12GraphicsCommandList *list = Open();
        Readable(list, { &g.tmp[kStages - 1] }, true);
        Pass up;
        up.read[0] = &g.tmp[kStages - 1];
        up.write[0] = &g.flow[k]; up.write[1] = &g.mask[k]; up.write[2] = &g.between;
        const float ku[16] = { float(kModelW), float(kModelH), float(kModelW), float(kModelH), 1.0f, 0.0f };
        memcpy(up.k, ku, sizeof(ku));
        up.w = kModelW; up.h = kModelH;
        Record(list, g.up.get(), up);
        Readable(list, { &g.tmp[kStages - 1] }, false);
        return Send(list) || Fail("a list of its own");
    }

    // A pair's frames in between. `submit` sends the engine's list recorded so far (the network runs after it on the
    // same queue); `reopen` gives a new one to record into (false: the card is lost). into[k]: frame k+1 of count,
    // written as an unordered-access picture and left in into_rest. False: nothing was made (SMFG 1.1 should).
    template <typename Submit, typename Reopen>
    static bool Frames(ID3D12GraphicsCommandList *&list, ID3D12Resource *before, ID3D12Resource *after, D3D12_RESOURCE_STATES rest,
                       ID3D12Resource *const *into, D3D12_RESOURCE_STATES into_rest, UINT count, UINT full_w, UINT full_h,
                       DXGI_FORMAT format, Submit submit, Reopen reopen)
    {
        if (!Start() || count == 0) return false;
        count = std::min(count, kMost);
        Size(full_w, full_h, format);
        const double t0 = LatencyNow();
        const bool readable = (rest & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0;
        auto open = [&](D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
            if (readable) return;
            D3D12_RESOURCE_BARRIER b[2] = { Barrier(before, from, to), Barrier(after, from, to) };
            list->ResourceBarrier(2, b);
        };
        // The real frames into the network's size, in the engine's list. The frame before is the last pair's frame
        // after when the pairs follow one another: its input and features are there.
        open(rest, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        UINT b_slot = 0;
        const bool known = g.last_after == before && before != nullptr;
        if (known) b_slot = g.last_slot;
        else Prep(list, before, b_slot);
        const UINT a_slot = 1u - b_slot;
        Prep(list, after, a_slot);
        D3D12_RESOURCE_BARRIER all = {};
        all.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        list->ResourceBarrier(1, &all);
        open(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, rest);
        g.last_after = after; g.last_slot = a_slot;
        if (!submit()) return false;
        // Their features (the encoder), then each frame in between's flow and mask.
        bool ok = (known || Run(g.head, "img", g.img[b_slot], "f", g.feat[b_slot])) && Run(g.head, "img", g.img[a_slot], "f", g.feat[a_slot]);
        for (UINT k = 0; ok && k < count; ++k) ok = Stages(k, b_slot, a_slot, float(k + 1) / float(count + 1));
        if (!reopen() || !ok) { g.last_after = nullptr; return false; }
        // Each frame in between at full size, in the engine's list.
        open(rest, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for (UINT k = 0; k < count; ++k)
        {
            Readable(list, { &g.flow[k], &g.mask[k] }, true);
            D3D12_RESOURCE_BARRIER to = Barrier(into[k], into_rest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &to);
            Pass p;
            p.frame[0] = before; p.frame[1] = after;
            p.read[0] = &g.flow[k]; p.read[1] = &g.mask[k];
            p.into = into[k];
            const float kk[16] = { float(full_w), float(full_h), float(g.sw), float(g.sh), float(kModelW), float(kModelH) };
            memcpy(p.k, kk, sizeof(kk));
            p.w = full_w; p.h = full_h;
            Record(list, g.warp.get(), p);
            D3D12_RESOURCE_BARRIER back = Barrier(into[k], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, into_rest);
            list->ResourceBarrier(1, &back);
            Readable(list, { &g.flow[k], &g.mask[k] }, false);
        }
        open(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, rest);
        g.cpu_ms += (LatencyNow() - t0) * 1000.0; ++g.pairs;
        const ULONGLONG now = GetTickCount64();
        if (now - g.told >= 10000 && g.pairs)
        {
            Trace("[ai] %u pairs: %.2f ms of the engine's time a pair to send the network's work (the card's own time is in [card])", g.pairs, g.cpu_ms / g.pairs);
            g.told = now; g.cpu_ms = 0.0; g.pairs = 0;
        }
        return true;
    }

    static bool Failed() { return g.tried && g.failed; }
}
