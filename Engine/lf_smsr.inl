// lf_smsr.inl - "SMSR 1.0" as the Upscaler (lf_window_fill.inl): the part of a picture that is scaled onto its screen
// made twice as large by our own network (smsr.h) before the scaling step takes it the rest of the way (or back
// down, where the screen is less than twice as large). 8-bit pictures only - it was trained on them; an HDR one is
// scaled as with "Off". It runs on every frame shown, as the scaling does.
//
// Its working pictures: three times four of 16-bit floats at the part's size (the head's, and two the body goes back
// and forth between), and the result, R8G8B8A8 at twice the size, COPY_SOURCE at rest (the scaling step reads it
// as it reads a captured frame). The network's numbers in one constant buffer, written once.
#include <array>
#include "smsr.h"
#include "smsr_weights.h"

static struct SmsrState
{
    ID3D12RootSignature *rs = nullptr;
    ID3D12PipelineState *head = nullptr, *body[3] = {}, *tail = nullptr;
    ID3D12DescriptorHeap *heap = nullptr;
    UINT ring = 0;
    ID3D12Resource *weights = nullptr;
    ID3D12Resource *feat[3][4] = {};          // NON_PIXEL_SHADER_RESOURCE at rest
    ID3D12Resource *out = nullptr;            // COPY_SOURCE at rest
    UINT w = 0, h = 0;
    bool failed = false, said = false;
    // SM_SMSR_TIME=1 (tests): each pass's time on the card - start, head, three body, tail - said every 60 runs.
    int time_on = -1;
    ID3D12QueryHeap *stamps = nullptr;
    ID3D12Resource *stamps_back = nullptr;
    bool stamps_pending = false;
    double pass_ms[5] = {};
    int timed = 0;
} g_smsr;

static void SmsrStamp(ID3D12GraphicsCommandList *list, UINT at)
{
    if (g_smsr.time_on == 1) list->EndQuery(g_smsr.stamps, D3D12_QUERY_TYPE_TIMESTAMP, at);
}

static void SmsrTimeTake()
{
    if (g_smsr.time_on < 0)
    {
        char v[4] = {};
        g_smsr.time_on = GetEnvironmentVariableA("SM_SMSR_TIME", v, sizeof(v)) > 0 && v[0] == '1' ? 1 : 0;
        if (g_smsr.time_on)
        {
            D3D12_QUERY_HEAP_DESC qd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 6, 0 };
            D3D12_HEAP_PROPERTIES rb = { D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC bd = {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 6 * sizeof(UINT64); bd.Height = 1; bd.DepthOrArraySize = 1;
            bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(lfCore.device->CreateQueryHeap(&qd, IID_PPV_ARGS(&g_smsr.stamps))) ||
                FAILED(lfCore.device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_smsr.stamps_back))))
                g_smsr.time_on = 0;
        }
    }
    if (g_smsr.time_on != 1 || !g_smsr.stamps_pending) return;
    // (The run before is done by now where this is used - Super Video waits for each frame.)
    UINT64 *t = nullptr;
    D3D12_RANGE r = { 0, 6 * sizeof(UINT64) }, none = { 0, 0 };
    UINT64 rate = 0;
    if (SUCCEEDED(lfCore.direct_queue->GetTimestampFrequency(&rate)) && rate && SUCCEEDED(g_smsr.stamps_back->Map(0, &r, reinterpret_cast<void **>(&t))))
    {
        for (int i = 0; i < 5; ++i) g_smsr.pass_ms[i] += double(t[i + 1] - t[i]) * 1000.0 / double(rate);
        g_smsr.stamps_back->Unmap(0, &none);
        if (++g_smsr.timed % 60 == 0)
        {
            const double *m = g_smsr.pass_ms;
            Trace("[smsr] on the card, %d runs: head %.2f ms, body %.2f / %.2f / %.2f, tail %.2f - %.2f ms a frame", g_smsr.timed,
                  m[0] / 60, m[1] / 60, m[2] / 60, m[3] / 60, m[4] / 60, (m[0] + m[1] + m[2] + m[3] + m[4]) / 60);
            for (double &x : g_smsr.pass_ms) x = 0.0;
        }
    }
    g_smsr.stamps_pending = false;
}

static constexpr UINT kSmsrSets = 16, kSmsrTable = 13;    // 9 SRVs, 4 UAVs a pass; five passes a run
static constexpr DXGI_FORMAT kSmsrFeat = DXGI_FORMAT_R16G16B16A16_FLOAT, kSmsrOut = DXGI_FORMAT_R8G8B8A8_UNORM;

static void SmsrFreeTargets()
{
    for (auto &set : g_smsr.feat) for (auto *&t : set) if (t) { t->Release(); t = nullptr; }
    if (g_smsr.out) { g_smsr.out->Release(); g_smsr.out = nullptr; }
    g_smsr.w = g_smsr.h = 0;
}

static bool SmsrBuild()
{
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? static_cast<CompileFn>(&CachedCompile) : nullptr;
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    auto serialize = d3d12 ? reinterpret_cast<SerializeRootFn>(GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
    if (!compile || !serialize) { Trace("[smsr] compiler unavailable"); return false; }
    const char *entries[5] = { "SmsrHeadMain", "SmsrBody0Main", "SmsrBody1Main", "SmsrBody2Main", "SmsrTailMain" };
    ID3DBlob *codes[5] = {};
    for (int i = 0; i < 5; ++i)
    {
        ID3DBlob *errors = nullptr;
        const HRESULT hr = compile(kSmsrHlsl, sizeof(kSmsrHlsl) - 1, "smsr.hlsl", nullptr, nullptr, entries[i], "cs_5_0",
                                   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &codes[i], &errors);
        if (FAILED(hr) || !codes[i])
        {
            Trace("[smsr] %s failed 0x%08X: %s", entries[i], hr, errors ? static_cast<const char *>(errors->GetBufferPointer()) : "(no log)");
            if (errors) errors->Release();
            for (auto *c : codes) if (c) c->Release();
            return false;
        }
        if (errors) errors->Release();
    }
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 9;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 4; ranges[1].OffsetInDescriptorsFromTableStart = 9;
    D3D12_ROOT_PARAMETER params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0; params[0].Constants.Num32BitValues = 8;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 2; params[2].DescriptorTable.pDescriptorRanges = ranges;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 3; rsd.pParameters = params;
    ID3DBlob *blob = nullptr, *errors = nullptr;
    HRESULT hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (errors) errors->Release();
    if (SUCCEEDED(hr)) hr = lfCore.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_smsr.rs));
    if (blob) blob->Release();
    ID3D12PipelineState **psos[5] = { &g_smsr.head, &g_smsr.body[0], &g_smsr.body[1], &g_smsr.body[2], &g_smsr.tail };
    for (int i = 0; i < 5 && SUCCEEDED(hr); ++i)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = g_smsr.rs;
        pd.CS.pShaderBytecode = codes[i]->GetBufferPointer(); pd.CS.BytecodeLength = codes[i]->GetBufferSize();
        hr = lfCore.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(psos[i]));
    }
    for (auto *c : codes) c->Release();
    if (FAILED(hr)) { Trace("[smsr] pipeline failed 0x%08X", hr); return false; }
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = kSmsrSets * 5 * kSmsrTable;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(lfCore.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_smsr.heap)))) { Trace("[smsr] no descriptor heap"); return false; }
    // The network's numbers: a constant buffer the card reads where it lies (upload heap, written once - each pass
    // reads every number once for all its pixels).
    D3D12_HEAP_PROPERTIES up = { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (sizeof(kSmsrWeights) + 255) & ~size_t(255); bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void *mapped = nullptr;
    if (FAILED(lfCore.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&g_smsr.weights))) ||
        FAILED(g_smsr.weights->Map(0, nullptr, &mapped)))
    { Trace("[smsr] no room for its numbers"); return false; }
    memcpy(mapped, kSmsrWeights, sizeof(kSmsrWeights));
    g_smsr.weights->Unmap(0, nullptr);
    Trace("[smsr] SMSR 1.0 ready: %u numbers, five passes", unsigned(sizeof(kSmsrWeights) / sizeof(float)));
    return true;
}

// Picked while running, its passes are compiled on a thread of their own (about ten seconds the first time - the
// picture would stand still that long); the frames meanwhile are scaled as with "Off". 0 not begun, 1 compiling, 2 done.
static std::atomic<int> g_smsr_compiled{0};
static void SmsrCompileAside()
{
    int was = 0;
    if (!g_smsr_compiled.compare_exchange_strong(was, 1)) return;
    try
    {
        std::thread([] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            for (const char *entry : { "SmsrHeadMain", "SmsrBody0Main", "SmsrBody1Main", "SmsrBody2Main", "SmsrTailMain" })
            {
                ID3DBlob *code = nullptr, *errors = nullptr;
                CachedCompile(kSmsrHlsl, sizeof(kSmsrHlsl) - 1, "smsr.hlsl", nullptr, nullptr, entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
                if (code) code->Release();
                if (errors) errors->Release();
            }
            g_smsr_compiled = 2;
        }).detach();
    }
    catch (...) { g_smsr_compiled = 2; }
}

// Super Video: made ready before its first frame (it waits; every frame of a video is to be SMSR's).
static bool SmsrPrepare()
{
    if (!g_smsr.head && !g_smsr.failed && !SmsrBuild()) g_smsr.failed = true;
    g_smsr_compiled = 2;
    return !g_smsr.failed;
}

static bool SmsrTargets(UINT w, UINT h)
{
    if (g_smsr.out && g_smsr.w == w && g_smsr.h == h) return true;
    SmsrFreeTargets();
    D3D12_HEAP_PROPERTIES def = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = h; td.DepthOrArraySize = 1; td.MipLevels = 1; td.SampleDesc.Count = 1;
    td.Format = kSmsrFeat; td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for (auto &set : g_smsr.feat)
        for (auto *&t : set)
            if (FAILED(lfCore.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&t))))
            { t = nullptr; SmsrFreeTargets(); Trace("[smsr] %ux%u working pictures failed", w, h); return false; }
    td.Width = w * 2; td.Height = h * 2; td.Format = kSmsrOut;
    if (FAILED(lfCore.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&g_smsr.out))))
    { g_smsr.out = nullptr; SmsrFreeTargets(); Trace("[smsr] %ux%u result failed", w * 2, h * 2); return false; }
    g_smsr.w = w; g_smsr.h = h;
    return true;
}

// Round 88l: in play, SMSR runs once a real frame, as it is taken (lf_desktop.inl): the picture the engine works on is
// then twice the captured size (sm_loop.inl, SessionBuild), the generators make their frames at that size, and the
// screen takes every frame as it is. (Run on every frame shown, as 88k did, x4 at 1440p was four runs of 6 ms each
// for each real frame: 111 frames shown of 368, a step error of 11 ms - it stuttered.) Super Video, which waits for
// every frame, still runs it on its way out (UpscalePasses).
static bool g_smsr_capture = false;

// Is it to be done as the frames are taken, for a session: SMSR picked, a window scaled onto its screen at least a
// quarter larger, an 8-bit picture (not HDR kept), not DLSS 5 MANAGER's frames - and its passes ready (compiled
// here the first time, about ten seconds once; from the cache after that).
static bool SmsrPrepare();
static bool SmsrAtCapture(UINT cap_w, UINT cap_h, const RECT &monitor, bool fill, bool dlss5)
{
    if (smOpt.upscaler.load() != 5 || !fill || dlss5 || HdrSwitchOn()) return false;
    if (cap_w < 16 || cap_h < 16 || cap_w > 4096 || cap_h > 4096) return false;
    const LONG mw = monitor.right - monitor.left, mh = monitor.bottom - monitor.top;
    // Round 91 (a user's mail: an RX 9070 XT game, frames made on a Radeon 780M; a 2564x1500 window on a 3840x2160
    // screen): SMSR makes a frame twice as large, and at capture every frame in between is then made at that size -
    // 5128x3000, a third larger than the screen, scaled back down on the way to it: the 780M at 100%, the game's
    // frames taken at 11 a second where Lossless Scaling held 40 x3. SMSR is for a window about half its screen; from
    // a quarter larger up to 1.75 the engine's own scaler brings it to the screen, and frames in between are made at
    // the window's size.
    if (!(double(mw) >= 1.75 * cap_w || double(mh) >= 1.75 * cap_h))
    {
        static bool said = false;
        if (!said && (double(mw) >= 1.25 * cap_w || double(mh) >= 1.25 * cap_h))
        {
            said = true;
            Trace("[smsr] the window %ux%u is less than 1.75 times smaller than its %ldx%ld screen: SMSR (twice as large) would make "
                  "a picture larger than the screen to make every frame in between at - the engine's own scaler brings it to the screen instead",
                  cap_w, cap_h, mw, mh);
        }
        return false;
    }
    return SmsrPrepare();
}

// Is SMSR to be run on this one: picked, an 8-bit picture going onto an 8-bit screen, made at least a quarter larger.
static bool SmsrWanted(int kind, DXGI_FORMAT src_format, DXGI_FORMAT out_format, LONG srw, LONG srh, LONG dsw, LONG dsh)
{
    if (kind != 5 || g_smsr.failed || g_smsr_capture) return false;     // (done as the frame was taken)
    const auto eight = [](DXGI_FORMAT f) { return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM; };
    if (!eight(src_format) || !eight(out_format)) return false;
    if (srw < 16 || srh < 16 || srw > 4096 || srh > 4096) return false;
    // (Round 91: as at capture - twice as large only where the screen is about that; else the own scaler.)
    return double(dsw) >= 1.75 * srw || double(dsh) >= 1.75 * srh;
}

// The part s of src (COPY_SOURCE, left so) made twice as large: the result (COPY_SOURCE), 2 x its size, or nullptr
// (not done - the caller scales src as it is). The caller holds g_up.gpu.
static ID3D12Resource *SmsrRun(ID3D12GraphicsCommandList *list, ID3D12Resource *src, DXGI_FORMAT src_format, const RECT &s)
{
    if (!g_smsr.head && !g_smsr.failed)
    {
        SmsrCompileAside();
        if (g_smsr_compiled.load() != 2) return nullptr;
        if (!SmsrBuild()) g_smsr.failed = true;
    }
    if (g_smsr.failed) return nullptr;
    const UINT w = UINT(s.right - s.left), h = UINT(s.bottom - s.top);
    if (!SmsrTargets(w, h)) return nullptr;
    SmsrTimeTake();
    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT set = g_smsr.ring++ % kSmsrSets;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu0 = g_smsr.heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu0 = g_smsr.heap->GetGPUDescriptorHandleForHeapStart();
    cpu0.ptr += SIZE_T(set) * 5 * kSmsrTable * stride;
    gpu0.ptr += UINT64(set) * 5 * kSmsrTable * stride;
    // A pass's table: reads (nullptr: an empty view), then writes.
    auto table = [&](int pass, ID3D12Resource *const reads[8], ID3D12Resource *const writes[4], DXGI_FORMAT write_format) {
        D3D12_CPU_DESCRIPTOR_HANDLE at = cpu0;
        at.ptr += SIZE_T(pass) * kSmsrTable * stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        for (int i = 0; i < 9; ++i, at.ptr += stride)
        {
            ID3D12Resource *r = i < 8 ? reads[i] : src;
            sv.Format = i < 8 ? kSmsrFeat : src_format;
            lfCore.device->CreateShaderResourceView(r, &sv, at);
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv = {};
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D; uv.Format = write_format;
        for (int i = 0; i < 4; ++i, at.ptr += stride) lfCore.device->CreateUnorderedAccessView(writes[i], nullptr, &uv, at);
        D3D12_GPU_DESCRIPTOR_HANDLE g = gpu0;
        g.ptr += UINT64(pass) * kSmsrTable * stride;
        return g;
    };
    ID3D12Resource *const none[8] = {};
    auto reads4 = [&](int a, int b) {   // feature set a at t0..t3, set b (or none) at t4..t7
        std::array<ID3D12Resource *, 8> r = {};
        for (int i = 0; i < 4; ++i) { r[i] = g_smsr.feat[a][i]; r[4 + i] = b >= 0 ? g_smsr.feat[b][i] : nullptr; }
        return r;
    };
    auto to = [&](ID3D12Resource *const set4[4], D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES into) {
        D3D12_RESOURCE_BARRIER b[4];
        for (int i = 0; i < 4; ++i) b[i] = Barrier(set4[i], from, into);
        list->ResourceBarrier(4, b);
    };
    const D3D12_RESOURCE_STATES R = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, U = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_BARRIER in = Barrier(src, D3D12_RESOURCE_STATE_COPY_SOURCE, R);
    list->ResourceBarrier(1, &in);
    ID3D12DescriptorHeap *heaps[] = { g_smsr.heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_smsr.rs);
    list->SetComputeRootConstantBufferView(1, g_smsr.weights->GetGPUVirtualAddress());
    UINT32 numbers[8] = { UINT32(s.left), UINT32(s.top), w, h, 0, UINT32(std::clamp(smOpt.smsr_strength.load(), 0, 200)), 0, 0 };
    const UINT gx = (w + 7) / 8, gy = (h + 7) / 8;
    SmsrStamp(list, 0);
    // Head: the picture into set 0.
    to(g_smsr.feat[0], R, U);
    list->SetComputeRootDescriptorTable(2, table(0, none, g_smsr.feat[0], kSmsrFeat));
    list->SetComputeRoot32BitConstants(0, 8, numbers, 0);
    list->SetPipelineState(g_smsr.head);
    list->Dispatch(gx, gy, 1);
    to(g_smsr.feat[0], U, R);
    SmsrStamp(list, 1);
    // Body: 0 -> 1, 1 -> 2, 2 -> 1.
    const int from_set[3] = { 0, 1, 2 }, into_set[3] = { 1, 2, 1 };
    for (int layer = 0; layer < 3; ++layer)
    {
        list->SetPipelineState(g_smsr.body[layer]);
        to(g_smsr.feat[into_set[layer]], R, U);
        list->SetComputeRootDescriptorTable(2, table(1 + layer, reads4(from_set[layer], -1).data(), g_smsr.feat[into_set[layer]], kSmsrFeat));
        numbers[4] = UINT32(layer);
        list->SetComputeRoot32BitConstants(0, 8, numbers, 0);
        list->Dispatch(gx, gy, 1);
        to(g_smsr.feat[into_set[layer]], U, R);
        SmsrStamp(list, 2 + layer);
    }
    // Tail: set 1 and the head's set 0, with the picture, into the result.
    D3D12_RESOURCE_BARRIER o = Barrier(g_smsr.out, D3D12_RESOURCE_STATE_COPY_SOURCE, U);
    list->ResourceBarrier(1, &o);
    ID3D12Resource *const outs[4] = { g_smsr.out, g_smsr.out, g_smsr.out, g_smsr.out };
    list->SetComputeRootDescriptorTable(2, table(4, reads4(1, 0).data(), outs, kSmsrOut));
    numbers[4] = 0;
    list->SetComputeRoot32BitConstants(0, 8, numbers, 0);
    list->SetPipelineState(g_smsr.tail);
    list->Dispatch(gx, gy, 1);
    D3D12_RESOURCE_BARRIER done[2] = { Barrier(g_smsr.out, U, D3D12_RESOURCE_STATE_COPY_SOURCE), Barrier(src, R, D3D12_RESOURCE_STATE_COPY_SOURCE) };
    list->ResourceBarrier(2, done);
    SmsrStamp(list, 5);
    if (g_smsr.time_on == 1) { list->ResolveQueryData(g_smsr.stamps, D3D12_QUERY_TYPE_TIMESTAMP, 0, 6, g_smsr.stamps_back, 0); g_smsr.stamps_pending = true; }
    if (!g_smsr.said) { g_smsr.said = true; Trace("[smsr] first frame: %ux%u made %ux%u by SMSR 1.0", w, h, w * 2, h * 2); }
    return g_smsr.out;
}
