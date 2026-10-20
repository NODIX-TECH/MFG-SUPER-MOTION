// lf_fg_still.inl - what does not move stays put under DLSS MFG (round 30).
//
// The report: DLSS MFG is very good while the whole screen moves, but on a
// page - a browser with a video in it - text flickers and lines are pulled
// about for an instant. Two things did it:
//
//   1. The motion DLSS MFG is given is an estimate over the whole picture, and
//      it is kept dense on purpose. Around a small moving area
//      the estimate spills onto text that never moved, and NGX then warps that
//      text in its frames in between: a line pulled, and back on the next
//      real frame.
//   2. With "MFG resolution" under the screen's, a frame in between is made
//      small and scaled up while a real one is shown as it is: still text is
//      sharp, soft, sharp, soft - a flicker at the frame rate.
//
// Both are the same mistake: treating as moving what is the same in the two
// real frames. So the two real frames are compared (the source's, before the
// network: its result shimmers even on a still page), and
//
//   - a motion field handed to NGX (only when the app asks for one) is put at
//     rest wherever the picture did not change (FgStillCalm);
//   - where nothing changed for some distance around (tiles of about 16
//     pixels of the picture, grown by `grow` tiles so a small thing passing
//     between two real frames is never cut out of the frame in between), the
//     frame in between shows the real frame's own pixels - sharp, and exactly
//     still (FgStillShow, and the same map in sm_rebuild.inl).
//
// The two frames are compared at the size frames in between are made at -
// the small copies the generator is given anyway - so a map costs a
// sixteenth of what it did at Flow 25, and no copy of "the picture before" is
// kept: the real frame before is still there (lf_dlssg.inl, FgReal).
//
// A picture that moves everywhere has no such tiles: the count comes back
// zero and nothing is added to its frames at all. "Keep still pixels" in the
// app turns it off (the generator is made again when it changes).

static bool FgStillWanted()
{
    return smOpt.still.load() != 0;
}

static struct FgStillState
{
    winrt::com_ptr<ID3D12RootSignature> root, show_root;
    winrt::com_ptr<ID3D12PipelineState> calm, tiles_pso, grow, show;
    winrt::com_ptr<ID3D12DescriptorHeap> heap, show_heap;
    winrt::com_ptr<ID3D12Resource> tiles, tiles_x;
    winrt::com_ptr<ID3D12Resource> calm_motion;
    winrt::com_ptr<ID3D12Resource> count, count_back;
    winrt::com_ptr<ID3D12Resource> display;       // the presenter's target when frames are made at the screen's size
    UINT w = 0, h = 0;            // the two frames compared (the small copies, or the picture itself)
    UINT tile = 16;               // pixels of them a tile
    UINT spread = 6;              // tiles a change spreads
    UINT tw = 0, th = 0, mw = 0, mh = 0;
    bool ready = false, failed = false, counted = false;
} g_fg_still;

static void FgStillClose()
{
    FgStillState &s = g_fg_still;
    s.tiles = nullptr; s.tiles_x = nullptr; s.calm_motion = nullptr;
    s.count = nullptr; s.count_back = nullptr; s.display = nullptr;
    s.w = s.h = s.tw = s.th = s.mw = s.mh = 0;
    s.ready = s.counted = false;
}

static const char kStillHlsl[] = R"hlsl(
Texture2D<float4> before : register(t0);
Texture2D<float4> now : register(t1);
Texture2D<float2> motion : register(t2);
RWTexture2D<float2> calm : register(u0);
RWTexture2D<uint> counter : register(u1);
cbuffer C : register(b0) { uint outW; uint outH; uint picW; uint picH; float threshold; int radius; uint along; uint spare; };

float Changed(int2 lo, int2 hi)
{
    int2 last = int2(picW, picH) - 1;
    float worst = 0;
    [loop] for (int y = lo.y; y <= hi.y; ++y)
    [loop] for (int x = lo.x; x <= hi.x; ++x)
    {
        int3 p = int3(clamp(int2(x, y), int2(0, 0), last), 0);
        float3 d = abs(now.Load(p).rgb - before.Load(p).rgb);
        worst = max(worst, max(d.r, max(d.g, d.b)));
    }
    return worst;
}

// The motion at rest where the picture is the same in both real frames: the
// pixels a motion texel stands for, and one more around them.
[numthreads(8, 8, 1)]
void CalmMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= outW || id.y >= outH) return;
    int2 lo = int2(id.xy * uint2(picW, picH) / uint2(outW, outH)) - 1;
    int2 hi = int2((id.xy + 1) * uint2(picW, picH) / uint2(outW, outH));
    calm[id.xy] = Changed(lo, hi) > threshold ? motion.Load(int3(id.xy, 0)) : float2(0, 0);
}
)hlsl";

static const char kStillTilesHlsl[] = R"hlsl(
Texture2D<float4> before : register(t0);
Texture2D<float4> now : register(t1);
RWTexture2D<float> tiles : register(u0);
cbuffer C : register(b0) { uint outW; uint outH; uint picW; uint picH; float threshold; int radius; uint along; uint spare; };

// One tile of `spare` x `spare` pixels: 1 when any of them changed.
[numthreads(8, 8, 1)]
void TilesMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= outW || id.y >= outH) return;
    int2 last = int2(picW, picH) - 1;
    int side = int(spare);
    int2 base = int2(id.xy) * side;
    float worst = 0;
    [loop] for (int y = 0; y < side; ++y)
    [loop] for (int x = 0; x < side; ++x)
    {
        int3 p = int3(clamp(base + int2(x, y), int2(0, 0), last), 0);
        float3 d = abs(now.Load(p).rgb - before.Load(p).rgb);
        worst = max(worst, max(d.r, max(d.g, d.b)));
    }
    tiles[id.xy] = worst > threshold ? 1.0 : 0.0;
}
)hlsl";

static const char kStillGrowHlsl[] = R"hlsl(
Texture2D<float> src : register(t0);
RWTexture2D<float> dst : register(u0);
RWTexture2D<uint> counter : register(u1);
cbuffer C : register(b0) { uint outW; uint outH; uint picW; uint picH; float threshold; int radius; uint along; uint spare; };

// "Changed" spreads `radius` tiles along x, then along y. The second pass
// counts the tiles left untouched - the ones a frame in between may take from
// the real frame.
[numthreads(8, 8, 1)]
void GrowMain(uint3 id : SV_DispatchThreadID)
{
    if (along == 0 && id.x == 0 && id.y == 0) counter[uint2(0, 0)] = 0;
    if (id.x >= outW || id.y >= outH) return;
    int2 last = int2(outW, outH) - 1;
    float m = 0;
    [loop] for (int i = -radius; i <= radius; ++i)
    {
        int2 q = clamp(int2(id.xy) + (along == 0 ? int2(i, 0) : int2(0, i)), int2(0, 0), last);
        m = max(m, src.Load(int3(q, 0)));
    }
    dst[id.xy] = m;
    if (along == 1 && m < 0.5) InterlockedAdd(counter[uint2(0, 0)], 1u);
}
)hlsl";

// The presenter's pass for a frame in between: the real frame's own pixels
// where the tiles say nothing moved near, the frame in between elsewhere
// (scaled to the screen exactly as lf_fg_scale.inl does), and a soft step
// from one to the other.
static const char kStillShowHlsl[] = R"hlsl(
Texture2D<float4> made : register(t0);
Texture2D<float4> real : register(t1);
Texture2D<float> moved : register(t2);
RWTexture2D<float4> dst : register(u0);
SamplerState lin : register(s0);
cbuffer C : register(b0) { uint outW; uint outH; };

float3 Made(uint2 at)
{
    uint w, h; made.GetDimensions(w, h);
    if (w == outW && h == outH) return made.Load(int3(at, 0)).rgb;
    float2 ratio = float2(w, h) / float2(outW, outH), inv = 1.0 / float2(w, h);
    float2 p = (float2(at) + 0.5) * ratio;
    if (ratio.x > 1 || ratio.y > 1)
    {
        float2 q = ratio * 0.25;
        float3 c = made.SampleLevel(lin, (p + float2(-q.x, -q.y)) * inv, 0).rgb;
        c += made.SampleLevel(lin, (p + float2(q.x, -q.y)) * inv, 0).rgb;
        c += made.SampleLevel(lin, (p + float2(-q.x, q.y)) * inv, 0).rgb;
        c += made.SampleLevel(lin, (p + q) * inv, 0).rgb;
        return c * 0.25;
    }
    p -= 0.5;
    float2 b = floor(p), f = p - b, f2 = f * f, f3 = f2 * f;
    float2 a0 = -0.5 * f + f2 - 0.5 * f3;
    float2 a1 = 1 - 2.5 * f2 + 1.5 * f3;
    float2 a2 = 0.5 * f + 2 * f2 - 1.5 * f3;
    float2 a3 = -0.5 * f2 + 0.5 * f3, a12 = a1 + a2;
    float2 p0 = b - 0.5, p12 = b + 0.5 + a2 / a12, p3 = b + 2.5;
    float xs[3] = {p0.x, p12.x, p3.x}, ys[3] = {p0.y, p12.y, p3.y};
    float wx[3] = {a0.x, a12.x, a3.x}, wy[3] = {a0.y, a12.y, a3.y};
    float3 c = 0;
    [unroll] for (int y = 0; y < 3; ++y) [unroll] for (int x = 0; x < 3; ++x)
        c += made.SampleLevel(lin, float2(xs[x], ys[y]) * inv, 0).rgb * wx[x] * wy[y];
    int2 low = int2(b), limit = int2(w, h) - 1;
    float3 n0 = made.Load(int3(clamp(low, int2(0, 0), limit), 0)).rgb;
    float3 n1 = made.Load(int3(clamp(low + int2(1, 0), int2(0, 0), limit), 0)).rgb;
    float3 n2 = made.Load(int3(clamp(low + int2(0, 1), int2(0, 0), limit), 0)).rgb;
    float3 n3 = made.Load(int3(clamp(low + 1, int2(0, 0), limit), 0)).rgb;
    return clamp(c, min(min(n0, n1), min(n2, n3)), max(max(n0, n1), max(n2, n3)));
}

[numthreads(8, 8, 1)]
void ShowMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= outW || id.y >= outH) return;
    float near = moved.SampleLevel(lin, (float2(id.xy) + 0.5) / float2(outW, outH), 0);
    float3 own = real.Load(int3(id.xy, 0)).rgb;
    float3 between = Made(id.xy);
    // A calm endpoint tile is not proof that nothing crossed it in between.
    // Keep a generated feature absent from the nearby real picture; a shifted
    // static glyph still finds its colour nearby and keeps the original repair.
    float3 gap = abs(between - own);
    if (near < 1.0 && max(gap.r, max(gap.g, gap.b)) > 0.04)
    {
        bool found = false;
        int2 last = int2(outW, outH) - 1;
        [loop] for (int n = 0; n < 16 && !found; ++n)
        {
            float turn = float(n & 7) * 0.7853982;
            int2 offset = int2(round(float2(cos(turn), sin(turn)) * (n < 8 ? 4.0 : 8.0)));
            float3 has = real.Load(int3(clamp(int2(id.xy) + offset, int2(0, 0), last), 0)).rgb;
            float3 off = abs(has - between);
            found = max(off.r, max(off.g, off.b)) < 0.08;
        }
        if (!found) near = 1.0;
    }
    if (near <= 0.004) { dst[id.xy] = float4(own, 1); return; }
    dst[id.xy] = float4(lerp(own, between, smoothstep(0.0, 1.0, near)), 1);
}
)hlsl";

static bool FgStillPipelines()
{
    FgStillState &s = g_fg_still;
    if (s.calm && s.tiles_pso && s.grow && s.show) return true;
    auto compile = [](const char *text, size_t size, const char *name, const char *entry, winrt::com_ptr<ID3DBlob> &code) {
        winrt::com_ptr<ID3DBlob> errors;
        if (FAILED(CachedCompile(text, size, name, nullptr, nullptr, entry, "cs_5_0", 0, 0, code.put(), errors.put())))
        { Trace("[dlssg] still pixels: %s: %s", name, errors ? (char *)errors->GetBufferPointer() : "compile failed"); return false; }
        return true;
    };
    winrt::com_ptr<ID3DBlob> calm, tiles, grow, show, signature, errors;
    if (!compile(kStillHlsl, sizeof(kStillHlsl) - 1, "lf-fg-still-calm.hlsl", "CalmMain", calm) ||
        !compile(kStillTilesHlsl, sizeof(kStillTilesHlsl) - 1, "lf-fg-still-tiles.hlsl", "TilesMain", tiles) ||
        !compile(kStillGrowHlsl, sizeof(kStillGrowHlsl) - 1, "lf-fg-still-grow.hlsl", "GrowMain", grow) ||
        !compile(kStillShowHlsl, sizeof(kStillShowHlsl) - 1, "lf-fg-still-show.hlsl", "ShowMain", show)) return false;
    // The producer's three passes: eight numbers, three pictures in, two out.
    {
        D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0}};
        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 8;
        for (UINT i = 0; i < 2; ++i)
        {
            params[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[i + 1].DescriptorTable = {1, &ranges[i]};
        }
        D3D12_ROOT_SIGNATURE_DESC desc = {3, params};
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, signature.put(), errors.put())) ||
            FAILED(lfCore.device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                      IID_PPV_ARGS(s.root.put())))) return false;
        auto make = [&](winrt::com_ptr<ID3DBlob> &code, winrt::com_ptr<ID3D12PipelineState> &into) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
            pso.pRootSignature = s.root.get();
            pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
            return SUCCEEDED(lfCore.device->CreateComputePipelineState(&pso, IID_PPV_ARGS(into.put())));
        };
        if (!make(calm, s.calm) || !make(tiles, s.tiles_pso) || !make(grow, s.grow)) return false;
        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 4 * 5;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(lfCore.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(s.heap.put())))) return false;
    }
    // The presenter's pass: its own signature and heap, on its own thread.
    {
        signature = nullptr; errors = nullptr;
        D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0}};
        D3D12_ROOT_PARAMETER params[3] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 2;
        for (UINT i = 0; i < 2; ++i)
        {
            params[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[i + 1].DescriptorTable = {1, &ranges[i]};
        }
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxAnisotropy = 1; sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC desc = {3, params, 1, &sampler};
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, signature.put(), errors.put())) ||
            FAILED(lfCore.device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                      IID_PPV_ARGS(s.show_root.put())))) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = s.show_root.get();
        pso.CS = {show->GetBufferPointer(), show->GetBufferSize()};
        if (FAILED(lfCore.device->CreateComputePipelineState(&pso, IID_PPV_ARGS(s.show.put())))) return false;
        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 4;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(lfCore.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(s.show_heap.put())))) return false;
    }
    return true;
}

// Everything for two frames of w x h compared in tiles of `tile` pixels, a
// picture of full_w x full_h, motion of mw x mh; each slot's own tile map into
// `slots`. Called by EnsureFg with the device idle; the textures leave it in
// the states they rest in. `own_display`: frames are made at the picture's
// size, so the presenter has no scaled target yet and gets one here.
static bool FgStillReady(UINT w, UINT h, UINT tile, UINT full_w, UINT full_h, UINT mw, UINT mh, DXGI_FORMAT show_format,
                         bool own_display)
{
    FgStillState &s = g_fg_still;
    FgStillClose();
    winrt::com_ptr<ID3D12Resource> *slots[_countof(g_fg.slots)];
    for (size_t i = 0; i < _countof(g_fg.slots); ++i) { slots[i] = &g_fg.slots[i].still; g_fg.slots[i].protect = false; }
    if (!FgStillWanted() || s.failed) return false;
    if (!FgStillPipelines()) { s.failed = true; Trace("[dlssg] still pixels: off - its passes could not be made"); return false; }
    s.w = w; s.h = h; s.mw = mw; s.mh = mh;
    s.tile = std::max(2u, tile);
    // A thing that moves less than about a hundred pixels of the picture a
    // frame is never cut out of a frame in between; a larger screen keeps the
    // distance.
    s.spread = static_cast<UINT>(std::clamp(static_cast<int>(full_h / 240), 4, 12));
    s.tw = (w + s.tile - 1) / s.tile; s.th = (h + s.tile - 1) / s.tile;
    s.tiles.attach(NewTexture(s.tw, s.th, DXGI_FORMAT_R8_UNORM, true));
    s.tiles_x.attach(NewTexture(s.tw, s.th, DXGI_FORMAT_R8_UNORM, true));
    s.calm_motion.attach(NewTexture(mw, mh, DXGI_FORMAT_R16G16_FLOAT, true));
    s.count.attach(NewTexture(1, 1, DXGI_FORMAT_R32_UINT, true));
    if (own_display) s.display.attach(NewTexture(full_w, full_h, show_format, true));
    for (auto *slot : slots) slot->attach(NewTexture(s.tw, s.th, DXGI_FORMAT_R8_UNORM, true));
    D3D12_HEAP_PROPERTIES back = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 512; buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bool whole = s.tiles && s.tiles_x && s.calm_motion && s.count && (!own_display || s.display) &&
        SUCCEEDED(lfCore.device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                         nullptr, IID_PPV_ARGS(s.count_back.put())));
    for (auto *slot : slots) whole = whole && *slot;
    if (!whole || !CmdOpen())
    {
        FgStillClose();
        for (auto *slot : slots) *slot = nullptr;
        Trace("[dlssg] still pixels: off - no memory for its maps");
        return false;
    }
    std::vector<D3D12_RESOURCE_BARRIER> to;
    to.push_back(Barrier(s.tiles.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    to.push_back(Barrier(s.tiles_x.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    to.push_back(Barrier(s.calm_motion.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    to.push_back(Barrier(s.count.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    if (s.display) to.push_back(Barrier(s.display.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    for (auto *slot : slots)
        to.push_back(Barrier(slot->get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    lfCore.cmd->ResourceBarrier(static_cast<UINT>(to.size()), to.data());
    if (!AwaitMark(lfCore.timeline, CmdSubmit(), 30000)) { FgStillClose(); return false; }
    s.ready = true;
    Trace("[dlssg] still pixels: what does not move is shown as the real frame has it (%ux%u tiles of %u pixels at %ux%u)",
          s.tw, s.th, s.tile, w, h);
    return true;
}

// A view of each kind into the heap, in order.
static D3D12_GPU_DESCRIPTOR_HANDLE FgStillBind(UINT set, ID3D12Resource *in0, ID3D12Resource *in1, ID3D12Resource *in2,
                                               ID3D12Resource *out0, ID3D12Resource *out1)
{
    FgStillState &s = g_fg_still;
    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = s.heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = s.heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += SIZE_T(set) * 5 * stride; gpu.ptr += UINT64(set) * 5 * stride;
    for (ID3D12Resource *in : {in0, in1, in2})
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = in->GetDesc().Format; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
        lfCore.device->CreateShaderResourceView(in, &srv, cpu); cpu.ptr += stride;
    }
    for (ID3D12Resource *out : {out0, out1})
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.Format = out->GetDesc().Format; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        lfCore.device->CreateUnorderedAccessView(out, nullptr, &uav, cpu); cpu.ptr += stride;
    }
    return gpu;
}

static void FgStillPass(ID3D12GraphicsCommandList *list, ID3D12PipelineState *pso, D3D12_GPU_DESCRIPTOR_HANDLE table,
                        UINT out_w, UINT out_h, float threshold, int radius, UINT along)
{
    FgStillState &s = g_fg_still;
    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ID3D12DescriptorHeap *heaps[] = {s.heap.get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(s.root.get());
    list->SetPipelineState(pso);
    UINT numbers[8] = {out_w, out_h, s.w, s.h, 0, static_cast<UINT>(radius), along, s.tile};
    memcpy(&numbers[4], &threshold, sizeof(float));
    list->SetComputeRoot32BitConstants(0, 8, numbers, 0);
    list->SetComputeRootDescriptorTable(1, table);
    table.ptr += UINT64(3) * stride;
    list->SetComputeRootDescriptorTable(2, table);
    list->Dispatch((out_w + 7) / 8, (out_h + 7) / 8, 1);
}

// More than a level and a half of 255 is a change; less is the same pixel.
static constexpr float kStillThreshold = 1.5f / 255.0f;

// The producer, before NGX, when a motion field of the engine's is handed to
// it: the motion at rest where the picture is the same as in the real frame
// before. picture: the captured one, in NON_PIXEL_SHADER_RESOURCE; before:
// the real frame before it at the same size, in COPY_SOURCE. Returns the
// motion to hand NGX.
static ID3D12Resource *FgStillCalm(ID3D12GraphicsCommandList *list, ID3D12Resource *picture, ID3D12Resource *before,
                                   ID3D12Resource *motion)
{
    FgStillState &s = g_fg_still;
    if (!s.ready || before == nullptr) return motion;
    const auto pd = picture->GetDesc(), bd = before->GetDesc(), md = motion->GetDesc();
    if (pd.Width != bd.Width || pd.Height != bd.Height || pd.Format != bd.Format || md.Width != s.mw || md.Height != s.mh ||
        pd.Format != DXGI_FORMAT_R8G8B8A8_UNORM) return motion;
    D3D12_RESOURCE_BARRIER in[] = {
        Barrier(s.calm_motion.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(before, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    list->ResourceBarrier(2, in);
    // The pass reads the picture's own size from its numbers.
    const UINT cw = s.w, ch = s.h;
    s.w = static_cast<UINT>(pd.Width); s.h = pd.Height;
    FgStillPass(list, s.calm.get(), FgStillBind(0, before, picture, motion, s.calm_motion.get(), s.count.get()),
                s.mw, s.mh, kStillThreshold, 0, 0);
    s.w = cw; s.h = ch;
    D3D12_RESOURCE_BARRIER out[] = {
        Barrier(s.calm_motion.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(before, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE)};
    list->ResourceBarrier(2, out);
    return s.calm_motion.get();
}

// The producer, with a slot in hand: which tiles changed between the real
// frame before and this one (both at the compared size, in the states given),
// grown, into the slot's own map; how many were left is copied out to be read
// after the fence (FgStillCounted). No frame before: nothing is counted.
static void FgStillTiles(ID3D12GraphicsCommandList *list, ID3D12Resource *now, D3D12_RESOURCE_STATES now_state,
                         ID3D12Resource *before, D3D12_RESOURCE_STATES before_state, ID3D12Resource *slot_map)
{
    FgStillState &s = g_fg_still;
    s.counted = false;
    if (!s.ready || slot_map == nullptr || now == nullptr || before == nullptr) return;
    const auto nd = now->GetDesc(), bd = before->GetDesc();
    if (nd.Width != s.w || nd.Height != s.h || bd.Width != s.w || bd.Height != s.h) return;
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    std::vector<D3D12_RESOURCE_BARRIER> in, out;
    if (now_state != read) { in.push_back(Barrier(now, now_state, read)); out.push_back(Barrier(now, read, now_state)); }
    if (before_state != read) { in.push_back(Barrier(before, before_state, read)); out.push_back(Barrier(before, read, before_state)); }
    if (!in.empty()) list->ResourceBarrier(static_cast<UINT>(in.size()), in.data());
    const int grow = static_cast<int>(s.spread);
    FgStillPass(list, s.tiles_pso.get(), FgStillBind(1, before, now, now, s.tiles.get(), s.count.get()),
                s.tw, s.th, kStillThreshold, 0, 0);
    D3D12_RESOURCE_BARRIER a[] = {
        Barrier(s.tiles.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    list->ResourceBarrier(1, a);
    FgStillPass(list, s.grow.get(), FgStillBind(2, s.tiles.get(), s.tiles.get(), s.tiles.get(), s.tiles_x.get(), s.count.get()),
                s.tw, s.th, 0.0f, grow, 0);
    D3D12_RESOURCE_BARRIER b[3] = {
        Barrier(s.tiles.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(s.tiles_x.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(slot_map, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    list->ResourceBarrier(3, b);
    D3D12_RESOURCE_BARRIER settled = {};
    settled.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    settled.UAV.pResource = s.count.get();
    list->ResourceBarrier(1, &settled);
    FgStillPass(list, s.grow.get(), FgStillBind(3, s.tiles_x.get(), s.tiles_x.get(), s.tiles_x.get(), slot_map, s.count.get()),
                s.tw, s.th, 0.0f, grow, 1);
    D3D12_RESOURCE_BARRIER c[3] = {
        Barrier(s.tiles_x.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(slot_map, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(s.count.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)};
    list->ResourceBarrier(3, c);
    D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
    from.pResource = s.count.get(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = s.count_back.get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Footprint = {DXGI_FORMAT_R32_UINT, 1, 1, 1, 256};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    auto rest = Barrier(s.count.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(1, &rest);
    if (!out.empty()) list->ResourceBarrier(static_cast<UINT>(out.size()), out.data());
    s.counted = true;
}

// After the producer's fence: how many tiles the frames in between may take
// from the real frame. Zero - a picture moving everywhere - and the presenter
// does nothing more than before.
static unsigned FgStillCounted()
{
    FgStillState &s = g_fg_still;
    if (!s.ready || !s.counted) return 0;
    s.counted = false;
    unsigned *value = nullptr;
    D3D12_RANGE read = {0, 4}, none = {0, 0};
    if (FAILED(s.count_back->Map(0, &read, reinterpret_cast<void **>(&value)))) return 0;
    const unsigned tiles = *value;
    s.count_back->Unmap(0, &none);
    return tiles;
}

// The presenter: a frame in between (`made`, COPY_SOURCE) with the real
// frame's pixels (`real`, COPY_SOURCE) where `map` says nothing moved near,
// into `target` (UNORDERED_ACCESS, the screen's size).
static void FgStillShow(ID3D12GraphicsCommandList *list, ID3D12Resource *made, ID3D12Resource *real, ID3D12Resource *map,
                        ID3D12Resource *target)
{
    FgStillState &s = g_fg_still;
    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = s.show_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = s.show_heap->GetGPUDescriptorHandleForHeapStart();
    for (ID3D12Resource *in : {made, real, map})
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = in->GetDesc().Format; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
        lfCore.device->CreateShaderResourceView(in, &srv, cpu); cpu.ptr += stride;
    }
    const auto td = target->GetDesc();
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = td.Format; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    lfCore.device->CreateUnorderedAccessView(target, nullptr, &uav, cpu);
    D3D12_RESOURCE_BARRIER in[] = {
        Barrier(made, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(real, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    list->ResourceBarrier(2, in);
    ID3D12DescriptorHeap *heaps[] = {s.show_heap.get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(s.show_root.get());
    list->SetPipelineState(s.show.get());
    const UINT size[] = {static_cast<UINT>(td.Width), td.Height};
    list->SetComputeRoot32BitConstants(0, 2, size, 0);
    list->SetComputeRootDescriptorTable(1, gpu);
    gpu.ptr += UINT64(3) * stride;
    list->SetComputeRootDescriptorTable(2, gpu);
    list->Dispatch((size[0] + 7) / 8, (size[1] + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER out[] = {
        Barrier(made, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Barrier(real, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE)};
    list->ResourceBarrier(2, out);
}
