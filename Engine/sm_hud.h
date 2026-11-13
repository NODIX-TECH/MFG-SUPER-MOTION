// sm_hud.h - SMHUD (round 90, the owner: "make it an option like Tensor cores - it works for every kind of MFG").
//
// A small network (ai\smhud.onnx: 105 thousand numbers, learnt in ai learning\smhud on made-up HUDs drawn over the
// owner's recordings) is run ONCE A PAIR on the two real frames, at half their size (960x544 at 1080p: 1.1 to 1.4 ms
// of an RTX 3060 through DirectML). It says for every place whether it is the game's HUD - text, numbers, the
// minimap's marks, bars, icons, the crosshair, subtitles, an FPS counter - or the world. Its answer is a map at a
// quarter of the picture's size, kept in the pair's slot (FgSlot::hud).
//
// Every frame in between of the pair - whichever generator made it, DLSS MFG, XeSS MFG or MSM FG - then goes through
// one pass on its way to the screen (Show): where the map says HUD, the pixel is the real frame's own (the one
// before up to half way, the one after from there), elsewhere the generator's. A HUD is never moved with the world,
// drawn twice, smeared or torn.
//
// It is what "Keep still pixels" does by the rule "the same in both real frames", for what that rule misses: a HUD
// over a world that does not move (the rule cannot tell them apart) and a HUD that changes (a counter, a timer, a
// bar). Measured on the made-up HUDs kept apart: the rule found 28% of the HUD and took 30% of the world for HUD;
// SMHUD 70% and 2.6%. Off ("hud_ai=0") nothing of this runs.
//
// The network is set up on a thread of its own the first time it is asked for, for the picture's size (and anew if
// that changes); meanwhile, and if anything is missing or refused on this card, frames go as without it and the log
// says why.

namespace smhud
{
    static constexpr UINT kTables = 64;     // descriptor tables in turn, each side (passes in flight)
    static constexpr UINT kSlotsEach = 5;   // a table: 4 read, 1 written

    static const char kHlsl[] = R"(
cbuffer K : register(b0) { float4 a; float4 b; };
Texture2D<float4> T0 : register(t0);
Texture2D<float4> T1 : register(t1);
Texture2D<float4> T2 : register(t2);
Buffer<float> Map : register(t3);
RWBuffer<float> Plane : register(u0);
SamplerState Lin : register(s0);

// Prep: a real frame into the network's input, three planes. a: content w, h (the picture made small), network w, h;
// b.x: linear light (1: HDR, made like the picture the network learnt from), b.y: taps across (the mean of n x n).
[numthreads(8, 8, 1)]
void PrepMain(uint3 id : SV_DispatchThreadID)
{
    const uint nw = (uint)a.z, nh = (uint)a.w;
    if (id.x >= nw || id.y >= nh) return;
    const float2 c = min(float2(id.xy), a.xy - 1.0);             // past the content: its edge goes on
    const int n = max(1, (int)b.y);
    float3 v = 0;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
            v += T0.SampleLevel(Lin, (c + (float2(i, j) + 0.5) / n) / a.xy, 0).rgb;
    v /= n * n;
    if (b.x > 0.5) { v = max(v, 0); v = pow(v / (1.0 + v), 1.0 / 2.2); }
    v = saturate(v);
    const uint plane = nw * nh, at = id.y * nw + id.x;
    Plane[at] = v.r; Plane[plane + at] = v.g; Plane[2 * plane + at] = v.b;
}
)";

    static const char kShowHlsl[] = R"(
cbuffer K : register(b0) { float4 a; float4 b; };
Texture2D<float4> Made : register(t0);
Texture2D<float4> Before : register(t1);
Texture2D<float4> After : register(t2);
Buffer<float> Map : register(t3);
RWTexture2D<float4> Into : register(u0);

// The map at a place (its own pixels), bilinear, its edge going on. b: map w, h.
float MapAt(float2 p)
{
    const int mw = (int)b.x, mh = (int)b.y;
    p = clamp(p, float2(0, 0), float2(mw - 1, mh - 1));
    const int x0 = (int)floor(p.x), y0 = (int)floor(p.y), x1 = min(x0 + 1, mw - 1), y1 = min(y0 + 1, mh - 1);
    const float2 f = p - float2(x0, y0);
    return lerp(lerp(Map[y0 * mw + x0], Map[y0 * mw + x1], f.x), lerp(Map[y1 * mw + x0], Map[y1 * mw + x1], f.x), f.y);
}

// Show: a frame in between with the HUD of its nearer real frame. a: picture w, h, the map's pixels a picture
// pixel (x, y); b: map w, h, the moment (0 the frame before, 1 the one after), SM_HUD_SHOW (1: the HUD painted red).
[numthreads(8, 8, 1)]
void ShowMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)a.x || id.y >= (uint)a.y) return;
    const float m = MapAt((float2(id.xy) + 0.5) * a.zw - 0.5);
    float3 c = Made.Load(int3(id.xy, 0)).rgb;
    // (A soft step: the network's own edge, a pixel or so of the map, is kept - no hard seam around a glyph.)
    const float w = smoothstep(0.3, 0.7, m);
    if (w > 0.0)
    {
        const float3 own = b.z < 0.5 ? Before.Load(int3(id.xy, 0)).rgb : After.Load(int3(id.xy, 0)).rgb;
        c = lerp(c, own, w);
        if (b.w > 0.5) c = lerp(c, float3(1, 0, 0), 0.6 * w);
    }
    Into[id.xy] = float4(c, 1.0);
}

// The same in place (the frame is a picture of the engine's own, written as unordered access): only the HUD's pixels
// are read and written - the rest of the picture is left as it is.
[numthreads(8, 8, 1)]
void InPlaceMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= (uint)a.x || id.y >= (uint)a.y) return;
    const float w = smoothstep(0.3, 0.7, MapAt((float2(id.xy) + 0.5) * a.zw - 0.5));
    if (w <= 0.0) return;
    const float3 own = b.z < 0.5 ? Before.Load(int3(id.xy, 0)).rgb : After.Load(int3(id.xy, 0)).rgb;
    float3 c = w >= 1.0 ? own : lerp(Into[id.xy].rgb, own, w);
    if (b.w > 0.5) c = lerp(c, float3(1, 0, 0), 0.6 * w);
    Into[id.xy] = float4(c, 1.0);
}

// Drawn over a frame already on the back buffer (the rebuild's own drawing straight onto it): one triangle over the
// whole picture; the HUD's pixels only, blended by the map (the rest discarded - nothing written there).
float4 HudVS(uint v : SV_VertexID) : SV_Position
{
    const float2 uv = float2((v << 1) & 2, v & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 HudPS(float4 pos : SV_Position) : SV_Target
{
    const float w = smoothstep(0.3, 0.7, MapAt(pos.xy * a.zw - 0.5));
    if (w <= 0.0) discard;
    const int3 at = int3(pos.xy, 0);
    float3 own = b.z < 0.5 ? Before.Load(at).rgb : After.Load(at).rgb;
    if (b.w > 0.5) own = lerp(own, float3(1, 0, 0), 0.6);
    return float4(own, w);
}
)";

    struct Buffer
    {
        winrt::com_ptr<ID3D12Resource> res;
        UINT count = 0;
        void *alloc = nullptr;
        OrtValue *value = nullptr;
    };

    static struct State
    {
        // 0 never asked, 1 being set up (its thread), 2 ready, -1 not available here (why: the log).
        std::atomic<int> stage{0};
        std::thread starter;
        // The libraries and DirectML: once.
        bool libraries = false;
        HMODULE ort = nullptr, dml = nullptr;
        const OrtApi *api = nullptr;
        const OrtDmlApi *dml_api = nullptr;
        OrtEnv *env = nullptr;
        IUnknown *dml_device = nullptr;
        OrtMemoryInfo *gpu_mem = nullptr;
        OrtRunOptions *run = nullptr;
        winrt::com_ptr<ID3D12RootSignature> root;
        winrt::com_ptr<ID3D12PipelineState> prep, show, in_place;
        // Drawn over the back buffer (presenter only): its pipeline for the buffer's format, its view.
        winrt::com_ptr<ID3D12PipelineState> draw;
        winrt::com_ptr<ID3D12DescriptorHeap> draw_view;
        DXGI_FORMAT draw_format = DXGI_FORMAT_UNKNOWN;
        bool draw_failed = false;
        winrt::com_ptr<ID3D12DescriptorHeap> heap, show_heap;
        UINT table = 0, show_table = 0;
        // For one picture size: the session, its input and answer, bound once.
        OrtSession *session = nullptr;
        OrtIoBinding *binding = nullptr;
        Buffer in[2], out;
        UINT full_w = 0, full_h = 0, cw = 0, ch = 0, nw = 0, nh = 0, mw = 0, mh = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        std::vector<Buffer> old;            // a size before (kept: the card may still be at them)
        // The presenter's own picture each frame in between is drawn into (its thread only).
        winrt::com_ptr<ID3D12Resource> into;
        // For the log.
        double run_ms = 0.0; unsigned pairs = 0, shown = 0; ULONGLONG told = 0;
        bool said_off = false;
        ~State() { if (starter.joinable()) starter.detach(); }   // (a joinable thread at the end would be a crash on the way out)
    } g;

    static bool Fail(const char *what, OrtStatus *status = nullptr)
    {
        std::string why = what;
        if (status && g.api) { why += ": "; why += g.api->GetErrorMessage(status); g.api->ReleaseStatus(status); }
        Trace("[hud] SMHUD is not available (%s) - frames in between go without it", why.c_str());
        g.stage = -1;
        return false;
    }
#define HUD_TRY(call, what) do { if (OrtStatus *st_ = (call)) return Fail(what, st_); } while (0)

    static std::wstring Folder()
    {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring dir(self);
        dir.resize(dir.find_last_of(L'\\') + 1);
        return dir + L"ai\\";
    }

    static bool Libraries()
    {
        if (g.libraries) return true;
        const std::wstring dir = Folder();
        if (GetFileAttributesW((dir + L"smhud.onnx").c_str()) == INVALID_FILE_ATTRIBUTES) return Fail("ai\\smhud.onnx is missing");
        g.dml = LoadLibraryExW((dir + L"DirectML.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        g.ort = g.dml ? LoadLibraryExW((dir + L"onnxruntime.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) : nullptr;
        if (!g.dml || !g.ort) return Fail("ai\\DirectML.dll or ai\\onnxruntime.dll is missing");
        auto base = reinterpret_cast<const OrtApiBase *(ORT_API_CALL *)()>(GetProcAddress(g.ort, "OrtGetApiBase"));
        auto make_device = reinterpret_cast<HRESULT(WINAPI *)(ID3D12Device *, UINT, REFIID, void **)>(GetProcAddress(g.dml, "DMLCreateDevice"));
        if (!base || !make_device) return Fail("the libraries are not the ones expected");
        g.api = base()->GetApi(ORT_API_VERSION);
        if (!g.api) return Fail("ONNX Runtime too old");
        HUD_TRY(g.api->GetExecutionProviderApi("DML", ORT_API_VERSION, reinterpret_cast<const void **>(&g.dml_api)), "DirectML in ONNX Runtime");
        static const GUID kDmlDevice = {0x6dbd6437, 0x96fd, 0x423f, {0xa9, 0x8c, 0xae, 0x5e, 0x7c, 0x2a, 0x57, 0x3f}};
        if (FAILED(make_device(lfCore.device, 0, kDmlDevice, reinterpret_cast<void **>(&g.dml_device))) || !g.dml_device)
            return Fail("DirectML on this card");
        HUD_TRY(g.api->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "smhud", &g.env), "its environment");
        HUD_TRY(g.api->CreateMemoryInfo("DML", OrtDeviceAllocator, 0, OrtMemTypeDefault, &g.gpu_mem), "its memory");
        HUD_TRY(g.api->CreateRunOptions(&g.run), "its run options");
        g.api->AddRunConfigEntry(g.run, "disable_synchronize_execution_providers", "1");
        // The two passes: 8 numbers; 4 read (3 pictures, the map), 1 written (the planes or the picture).
        D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0}};
        ranges[1].OffsetInDescriptorsFromTableStart = 4;
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 8;
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
        auto compile = [&](const char *text, size_t size, const char *name, const char *entry, winrt::com_ptr<ID3D12PipelineState> &into) {
            winrt::com_ptr<ID3DBlob> code, errs;
            if (FAILED(CachedCompile(text, size, name, nullptr, nullptr, entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errs.put())))
                return Fail(errs ? static_cast<const char *>(errs->GetBufferPointer()) : "a pass did not compile");
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
            pso.pRootSignature = g.root.get();
            pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
            if (FAILED(lfCore.device->CreateComputePipelineState(&pso, IID_PPV_ARGS(into.put())))) return Fail("a pass's pipeline");
            return true;
        };
        if (!compile(kHlsl, sizeof(kHlsl) - 1, "smhud-prep.hlsl", "PrepMain", g.prep) ||
            !compile(kShowHlsl, sizeof(kShowHlsl) - 1, "smhud-show.hlsl", "ShowMain", g.show) ||
            !compile(kShowHlsl, sizeof(kShowHlsl) - 1, "smhud-show.hlsl", "InPlaceMain", g.in_place)) return false;
        D3D12_DESCRIPTOR_HEAP_DESC heap = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kTables * kSlotsEach, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
        if (FAILED(lfCore.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(g.heap.put()))) ||
            FAILED(lfCore.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(g.show_heap.put())))) return Fail("its descriptor heaps");
        g.libraries = true;
        return true;
    }

    static bool Make(Buffer &b, UINT count, const int64_t *shape)
    {
        b.count = count;
        const UINT64 bytes = UINT64(count) * 2;
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
        HUD_TRY(g.dml_api->CreateGPUAllocationFromD3DResource(b.res.get(), &b.alloc), "the network's view of a buffer");
        HUD_TRY(g.api->CreateTensorWithDataAsOrtValue(g.gpu_mem, b.alloc, bytes, shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16, &b.value),
                "a tensor on the card");
        return true;
    }

    // The network for pictures of full_w x full_h (its thread): the session with its sizes fixed, its buffers, one
    // first run (DirectML sets its work up there - a hitch if done on the engine's thread).
    static bool SetUp(UINT full_w, UINT full_h, DXGI_FORMAT format)
    {
        const double t0 = LatencyNow();
        if (!Libraries()) return false;
        if (g.binding) { g.api->ReleaseIoBinding(g.binding); g.binding = nullptr; }
        if (g.session) { g.api->ReleaseSession(g.session); g.session = nullptr; }
        for (Buffer *b : {&g.in[0], &g.in[1], &g.out}) if (b->res) { g.old.push_back(*b); *b = Buffer{}; }
        g.full_w = full_w; g.full_h = full_h; g.format = format;
        // Half the picture, 540 lines at most: what it learnt from (1080-line recordings at half their size) - a game's
        // HUD grows with the screen, so a 1440p or 4K picture made 540 lines has it as large as it learnt it, and the
        // network costs what it does at 1080p (round 90: at 1280x736 for 1440p a game filling the card lost 11%).
        const double scale = std::min(0.5, 540.0 / std::max(1u, full_h));
        g.cw = std::max(32u, UINT(std::lround(full_w * scale))); g.ch = std::max(32u, UINT(std::lround(full_h * scale)));
        g.nw = (g.cw + 31) / 32 * 32; g.nh = (g.ch + 31) / 32 * 32;
        g.mw = g.nw / 2; g.mh = g.nh / 2;
        OrtSessionOptions *options = nullptr;
        HUD_TRY(g.api->CreateSessionOptions(&options), "its options");
        g.api->DisableMemPattern(options);
        g.api->SetSessionExecutionMode(options, ORT_SEQUENTIAL);
        g.api->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL);
        g.api->AddFreeDimensionOverrideByName(options, "height", g.nh);
        g.api->AddFreeDimensionOverrideByName(options, "width", g.nw);
        g.api->AddFreeDimensionOverrideByName(options, "half_height", g.mh);
        g.api->AddFreeDimensionOverrideByName(options, "half_width", g.mw);
        if (OrtStatus *st = g.dml_api->SessionOptionsAppendExecutionProvider_DML1(options, reinterpret_cast<IDMLDevice *>(g.dml_device), lfCore.direct_queue))
        { g.api->ReleaseSessionOptions(options); return Fail("DirectML on the engine's queue", st); }
        OrtStatus *made = g.api->CreateSession(g.env, (Folder() + L"smhud.onnx").c_str(), options, &g.session);
        g.api->ReleaseSessionOptions(options);
        if (made) return Fail("the network on this card", made);
        const int64_t image[4] = {1, 3, g.nh, g.nw}, map[4] = {1, 1, g.mh, g.mw};
        if (!Make(g.in[0], 3 * g.nw * g.nh, image) || !Make(g.in[1], 3 * g.nw * g.nh, image) || !Make(g.out, g.mw * g.mh, map)) return false;
        HUD_TRY(g.api->CreateIoBinding(g.session, &g.binding), "binding");
        HUD_TRY(g.api->BindInput(g.binding, "a", g.in[0].value), "binding its input a");
        HUD_TRY(g.api->BindInput(g.binding, "b", g.in[1].value), "binding its input b");
        HUD_TRY(g.api->BindOutput(g.binding, "hud", g.out.value), "binding its answer");
        const double t1 = LatencyNow();
        HUD_TRY(g.api->RunWithBinding(g.session, g.run, g.binding), "its first run");
        DXGI_ADAPTER_DESC1 card = {};
        if (lfCard.adapter) lfCard.adapter->GetDesc1(&card);
        Trace("[hud] SMHUD on %ls: the HUD found once a pair at %ux%u (the picture %ux%u made small), its map %ux%u - frames in between show it "
              "as the real frame has it (%.1f s to set up, first run %.0f ms)", card.Description, g.nw, g.nh, full_w, full_h, g.mw, g.mh,
              LatencyNow() - t0, (LatencyNow() - t1) * 1000.0);
        return true;
    }

    // The producer's question each pair: ready for pictures of this size? Not yet: its thread is started (or started
    // anew for a new size) and the pair goes without.
    static bool Ready(UINT full_w, UINT full_h, DXGI_FORMAT format)
    {
        const int stage = g.stage.load();
        if (stage == -1 || stage == 1) return false;
        if (stage == 2 && g.full_w == full_w && g.full_h == full_h && g.format == format) return true;
        if (g.starter.joinable()) g.starter.join();
        g.stage = 1;
        g.starter = std::thread([full_w, full_h, format] { if (SetUp(full_w, full_h, format)) g.stage = 2; });
        return false;
    }

    static D3D12_GPU_DESCRIPTOR_HANDLE Table(ID3D12DescriptorHeap *heap, UINT &turn, ID3D12Resource *t0, ID3D12Resource *t1,
                                             ID3D12Resource *t2, ID3D12Resource *map, UINT map_count, ID3D12Resource *out, bool out_buffer, UINT out_count)
    {
        const UINT each = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const UINT first = (turn++ % kTables) * kSlotsEach;
        D3D12_CPU_DESCRIPTOR_HANDLE at = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap->GetGPUDescriptorHandleForHeapStart();
        at.ptr += SIZE_T(first) * each; gpu.ptr += UINT64(first) * each;
        D3D12_SHADER_RESOURCE_VIEW_DESC tex = {};
        tex.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        tex.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        tex.Texture2D.MipLevels = 1;
        for (ID3D12Resource *t : {t0, t1, t2})
        {
            tex.Format = t ? t->GetDesc().Format : DXGI_FORMAT_R8G8B8A8_UNORM;
            lfCore.device->CreateShaderResourceView(t, &tex, at); at.ptr += each;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC buf = {};
        buf.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        buf.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        buf.Format = DXGI_FORMAT_R16_FLOAT;
        buf.Buffer.NumElements = map ? map_count : 1;
        lfCore.device->CreateShaderResourceView(map, &buf, at); at.ptr += each;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        if (out_buffer) { uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; uav.Format = DXGI_FORMAT_R16_FLOAT; uav.Buffer.NumElements = out_count; }
        else { uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; uav.Format = out->GetDesc().Format; }
        lfCore.device->CreateUnorderedAccessView(out, nullptr, &uav, at);
        return gpu;
    }

    static void Dispatch(ID3D12GraphicsCommandList *list, ID3D12DescriptorHeap *heap, ID3D12PipelineState *pso, D3D12_GPU_DESCRIPTOR_HANDLE table,
                         const float k[8], UINT w, UINT h)
    {
        ID3D12DescriptorHeap *heaps[] = {heap};
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(g.root.get());
        list->SetPipelineState(pso);
        list->SetComputeRoot32BitConstants(0, 8, k, 0);
        list->SetComputeRootDescriptorTable(1, table);
        list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    }

    // The producer, once a pair, on the engine's list (lfCore.cmd, open): the HUD of `before` -> `after` (the real
    // frames at the picture's size, COPY_SOURCE at rest) into `map` (the slot's own; made here when it has none or
    // the wrong size; COMMON at rest). The list is sent for the network to run behind it, and opened again.
    // True: the map holds this pair's HUD.
    static bool Pair(ID3D12Resource *before, ID3D12Resource *after, UINT full_w, UINT full_h, DXGI_FORMAT format,
                     winrt::com_ptr<ID3D12Resource> &map)
    {
        if (!smOpt.hud_ai.load())
        {
            if (g.stage.load() == 2 && !g.said_off) { g.said_off = true; Trace("[hud] SMHUD off"); }
            return false;
        }
        if (!before || !after || !Ready(full_w, full_h, format)) return false;
        g.said_off = false;
        const UINT64 bytes = (UINT64(g.mw) * g.mh * 2 + 255) & ~255ull;
        if (!map || map->GetDesc().Width != bytes)
        {
            map = nullptr;
            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = bytes;
            desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                              IID_PPV_ARGS(map.put())))) { map = nullptr; return false; }
        }
        ID3D12GraphicsCommandList *list = lfCore.cmd;
        const D3D12_RESOURCE_STATES rest = D3D12_RESOURCE_STATE_COPY_SOURCE, read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        D3D12_RESOURCE_BARRIER in[2] = {Barrier(before, rest, read), Barrier(after, rest, read)};
        list->ResourceBarrier(before == after ? 1 : 2, in);
        const float taps = std::clamp(std::ceil(float(g.full_w) / g.cw - 0.01f), 1.0f, 4.0f);
        const float k[8] = {float(g.cw), float(g.ch), float(g.nw), float(g.nh), g.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1.0f : 0.0f, taps, 0, 0};
        ID3D12Resource *frames[2] = {before, after};
        for (int i = 0; i < 2; ++i)
            Dispatch(list, g.heap.get(), g.prep.get(),
                     Table(g.heap.get(), g.table, frames[i], nullptr, nullptr, nullptr, 0, g.in[i].res.get(), true, g.in[i].count), k, g.nw, g.nh);
        D3D12_RESOURCE_BARRIER out[2] = {Barrier(before, read, rest), Barrier(after, read, rest)};
        list->ResourceBarrier(before == after ? 1 : 2, out);
        // (The network runs on the same queue, behind what was sent; the map is copied out behind it.)
        CmdSubmit();
        const double t0 = LatencyNow();
        OrtStatus *st = g.api->RunWithBinding(g.session, g.run, g.binding);
        g.run_ms += (LatencyNow() - t0) * 1000.0;
        if (!CmdOpen()) { if (st) g.api->ReleaseStatus(st); return false; }
        if (st) { Fail("its run", st); return false; }
        D3D12_RESOURCE_BARRIER to[2] = {Barrier(g.out.res.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                        Barrier(map.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)};
        lfCore.cmd->ResourceBarrier(2, to);
        lfCore.cmd->CopyBufferRegion(map.get(), 0, g.out.res.get(), 0, UINT64(g.mw) * g.mh * 2);
        D3D12_RESOURCE_BARRIER back[2] = {Barrier(g.out.res.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                                          Barrier(map.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON)};
        lfCore.cmd->ResourceBarrier(2, back);
        ++g.pairs;
        const ULONGLONG now = GetTickCount64();
        if (now - g.told > 10000)
        {
            if (g.told) Trace("[hud] SMHUD: %u pairs, %u frames in between with the HUD kept; handing the network its work %.2f ms a pair (the engine's thread)",
                              g.pairs, g.shown, g.run_ms / std::max(1u, g.pairs));
            g.told = now; g.pairs = g.shown = 0; g.run_ms = 0.0;
        }
        return true;
    }

    // The presenter's question: may a pair's map be used (the network set up, the map of this size)?
    static bool Showable() { return g.stage.load() == 2; }

    // The presenter, for a frame in between: `made` (the picture's size, in `made_state`) with the HUD of the real
    // frame nearer to `phase` (`before`, `after`: COPY_SOURCE at rest) where the pair's `map` says, into a picture of
    // the presenter's own - returned in UNORDERED_ACCESS, to be shown from. Null: not done (sizes not the map's).
    static ID3D12Resource *Show(ID3D12GraphicsCommandList *list, ID3D12Resource *made, D3D12_RESOURCE_STATES made_state, ID3D12Resource *before,
                                ID3D12Resource *after, ID3D12Resource *map, float phase)
    {
        if (!Showable() || !made || !before || !after || !map) return nullptr;
        const auto md = made->GetDesc();
        if (md.Width != g.full_w || md.Height != g.full_h || before->GetDesc().Width != g.full_w || after->GetDesc().Width != g.full_w ||
            map->GetDesc().Width < UINT64(g.mw) * g.mh * 2) return nullptr;
        static const bool paint = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_HUD_SHOW", v, sizeof(v)) && v[0] == '1'; }();
        const float k[8] = {float(g.full_w), float(g.full_h), float(g.cw) / g.full_w * 0.5f, float(g.ch) / g.full_h * 0.5f,
                            float(g.mw), float(g.mh), phase, paint ? 1.0f : 0.0f};
        // In place: the frame is the engine's own picture at rest as unordered access (the presenter's target) and the
        // card reads such pictures back as it writes them - only the HUD's pixels are touched, nothing copied.
        if (made_state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        {
            static DXGI_FORMAT checked = DXGI_FORMAT_UNKNOWN;
            static bool loads = false;
            if (checked != md.Format)
            {
                checked = md.Format;
                D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {md.Format};
                loads = SUCCEEDED(lfCore.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
                        (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) && (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
                Trace("[hud] SMHUD: frames in between of format %d %s", int(md.Format), loads ? "touched in place, at the HUD only" : "copied through a picture of its own");
            }
            if (loads)
            {
                const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, rest = D3D12_RESOURCE_STATE_COPY_SOURCE;
                D3D12_RESOURCE_BARRIER in[3];
                UINT n = 0;
                in[n++] = Barrier(before, rest, read);
                if (after != before) in[n++] = Barrier(after, rest, read);
                in[n++] = Barrier(map, D3D12_RESOURCE_STATE_COMMON, read);
                list->ResourceBarrier(n, in);
                Dispatch(list, g.show_heap.get(), g.in_place.get(),
                         Table(g.show_heap.get(), g.show_table, nullptr, before, after, map, g.mw * g.mh, made, false, 0), k, g.full_w, g.full_h);
                D3D12_RESOURCE_BARRIER out[3];
                for (UINT i = 0; i < n; ++i) { out[i] = in[i]; std::swap(out[i].Transition.StateBefore, out[i].Transition.StateAfter); }
                list->ResourceBarrier(n, out);
                ++g.shown;
                return made;
            }
        }
        if (!g.into || g.into->GetDesc().Width != md.Width || g.into->GetDesc().Height != md.Height || g.into->GetDesc().Format != md.Format)
        {
            g.into = nullptr;
            // (Written as unordered access: a card that cannot store this format so leaves the frames as they are.)
            D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {md.Format};
            if (FAILED(lfCore.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
                !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
            {
                static bool said = false;
                if (!said) { said = true; Trace("[hud] SMHUD: this card cannot write pictures of format %d as the pass does - the HUD is not kept", int(md.Format)); }
                return nullptr;
            }
            g.into.attach(NewTexture(UINT(md.Width), md.Height, md.Format, true));
            if (!g.into) return nullptr;
            auto first = Barrier(g.into.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &first);
        }
        const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, rest = D3D12_RESOURCE_STATE_COPY_SOURCE;
        D3D12_RESOURCE_BARRIER in[4];
        UINT n = 0;
        if (made_state != read) in[n++] = Barrier(made, made_state, read);
        if (before != made) in[n++] = Barrier(before, rest, read);
        if (after != before && after != made) in[n++] = Barrier(after, rest, read);
        in[n++] = Barrier(map, D3D12_RESOURCE_STATE_COMMON, read);
        list->ResourceBarrier(n, in);
        Dispatch(list, g.show_heap.get(), g.show.get(),
                 Table(g.show_heap.get(), g.show_table, made, before, after, map, g.mw * g.mh, g.into.get(), false, 0), k, g.full_w, g.full_h);
        D3D12_RESOURCE_BARRIER out[4];
        for (UINT i = 0; i < n; ++i) { out[i] = in[i]; std::swap(out[i].Transition.StateBefore, out[i].Transition.StateAfter); }
        list->ResourceBarrier(n, out);
        ++g.shown;
        return g.into.get();
    }

    // The presenter, for a frame in between the rebuild drew straight onto the back buffer (`onto`, the picture's size,
    // COPY_DEST - left so): the HUD of the real frame nearer to `phase` drawn over it where the pair's `map` says.
    // False: not done (the format, the sizes) - the caller then keeps the frame off the back buffer next time.
    static bool Draw(ID3D12GraphicsCommandList *list, ID3D12Resource *onto, ID3D12Resource *before, ID3D12Resource *after,
                     ID3D12Resource *map, float phase)
    {
        if (!Showable() || !onto || !before || !after || !map || g.draw_failed) return false;
        const auto od = onto->GetDesc();
        if (od.Width != g.full_w || od.Height != g.full_h || map->GetDesc().Width < UINT64(g.mw) * g.mh * 2) return false;
        if (!g.draw || g.draw_format != od.Format)
        {
            auto fail = [](const char *what) { g.draw_failed = true; g.draw = nullptr; Trace("[hud] SMHUD: not drawn over the back buffer (%s) - frames in between are finished beside it", what); return false; };
            winrt::com_ptr<ID3DBlob> vs, ps, errors;
            if (FAILED(CachedCompile(kShowHlsl, sizeof(kShowHlsl) - 1, "smhud-show.hlsl", nullptr, nullptr, "HudVS", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, vs.put(), errors.put())))
                return fail(errors ? static_cast<const char *>(errors->GetBufferPointer()) : "its vertex shader");
            errors = nullptr;
            if (FAILED(CachedCompile(kShowHlsl, sizeof(kShowHlsl) - 1, "smhud-show.hlsl", nullptr, nullptr, "HudPS", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, ps.put(), errors.put())))
                return fail(errors ? static_cast<const char *>(errors->GetBufferPointer()) : "its pixel shader");
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
            pso.pRootSignature = g.root.get();
            pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
            pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
            auto &blend = pso.BlendState.RenderTarget[0];
            blend.BlendEnable = TRUE;
            blend.SrcBlend = D3D12_BLEND_SRC_ALPHA; blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; blend.BlendOp = D3D12_BLEND_OP_ADD;
            blend.SrcBlendAlpha = D3D12_BLEND_ZERO; blend.DestBlendAlpha = D3D12_BLEND_ONE; blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            blend.LogicOp = D3D12_LOGIC_OP_NOOP;
            blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pso.SampleMask = UINT_MAX;
            pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pso.NumRenderTargets = 1;
            pso.RTVFormats[0] = od.Format;
            pso.SampleDesc.Count = 1;
            g.draw = nullptr;
            if (FAILED(lfCore.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(g.draw.put())))) return fail("its pipeline");
            if (!g.draw_view)
            {
                D3D12_DESCRIPTOR_HEAP_DESC view = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE};
                if (FAILED(lfCore.device->CreateDescriptorHeap(&view, IID_PPV_ARGS(g.draw_view.put())))) return fail("its view");
            }
            g.draw_format = od.Format;
            Trace("[hud] SMHUD: drawn over frames in between built straight onto the back buffer (format %d)", int(od.Format));
        }
        static const bool paint = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_HUD_SHOW", v, sizeof(v)) && v[0] == '1'; }();
        const float k[8] = {float(g.full_w), float(g.full_h), float(g.cw) / g.full_w * 0.5f, float(g.ch) / g.full_h * 0.5f,
                            float(g.mw), float(g.mh), phase, paint ? 1.0f : 0.0f};
        const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, rest = D3D12_RESOURCE_STATE_COPY_SOURCE;
        D3D12_RESOURCE_BARRIER in[4];
        UINT n = 0;
        in[n++] = Barrier(onto, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        in[n++] = Barrier(before, rest, read);
        if (after != before) in[n++] = Barrier(after, rest, read);
        in[n++] = Barrier(map, D3D12_RESOURCE_STATE_COMMON, read);
        list->ResourceBarrier(n, in);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.draw_view->GetCPUDescriptorHandleForHeapStart();
        lfCore.device->CreateRenderTargetView(onto, nullptr, rtv);
        // (The table's written picture: none here - a view of nothing in its place.)
        const UINT each = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const UINT first = (g.show_table++ % kTables) * kSlotsEach;
        D3D12_CPU_DESCRIPTOR_HANDLE at = g.show_heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = g.show_heap->GetGPUDescriptorHandleForHeapStart();
        at.ptr += SIZE_T(first) * each; gpu.ptr += UINT64(first) * each;
        D3D12_SHADER_RESOURCE_VIEW_DESC tex = {};
        tex.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        tex.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        tex.Texture2D.MipLevels = 1;
        for (ID3D12Resource *t : {static_cast<ID3D12Resource *>(nullptr), before, after})
        {
            tex.Format = t ? t->GetDesc().Format : DXGI_FORMAT_R8G8B8A8_UNORM;
            lfCore.device->CreateShaderResourceView(t, &tex, at); at.ptr += each;
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC buf = {};
        buf.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        buf.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        buf.Format = DXGI_FORMAT_R16_FLOAT;
        buf.Buffer.NumElements = g.mw * g.mh;
        lfCore.device->CreateShaderResourceView(map, &buf, at); at.ptr += each;
        D3D12_UNORDERED_ACCESS_VIEW_DESC none = {};
        none.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; none.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        lfCore.device->CreateUnorderedAccessView(nullptr, nullptr, &none, at);
        ID3D12DescriptorHeap *heaps[] = {g.show_heap.get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(g.root.get());
        list->SetPipelineState(g.draw.get());
        list->SetGraphicsRoot32BitConstants(0, 8, k, 0);
        list->SetGraphicsRootDescriptorTable(1, gpu);
        D3D12_VIEWPORT viewport = {0, 0, float(od.Width), float(od.Height), 0, 1};
        D3D12_RECT scissor = {0, 0, LONG(od.Width), LONG(od.Height)};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        D3D12_RESOURCE_BARRIER out[4];
        for (UINT i = 0; i < n; ++i) { out[i] = in[i]; std::swap(out[i].Transition.StateBefore, out[i].Transition.StateAfter); }
        list->ResourceBarrier(n, out);
        ++g.shown;
        return true;
    }

    static bool DrawFailed() { return g.draw_failed; }

    // The engine stops: its thread is waited for.
    static void Close() { if (g.starter.joinable()) g.starter.join(); }
}
