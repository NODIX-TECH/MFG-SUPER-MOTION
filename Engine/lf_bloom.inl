// lf_bloom.inl - Bloom (round 86, the owner's request; the idea of LumeniteFX's anamorphic bloom, in our own code):
// the bright parts of the picture glow into what is around them, round or - "Anamorphic" - drawn out sideways as a
// cinema lens does.
//
// Where: on each REAL frame, the moment it is in the pipeline (ShowPassThrough), before anything reads it - so the
// frames in between of every generator (DLSS MFG and XeSS MFG make theirs from this very picture, SMFG from its copy)
// carry the same glow as the real ones, and nothing flickers between a lit and an unlit picture. Once a real frame,
// whatever the multiplier.
//
// How (compute passes, 8x8):
//   Pre      the picture at half size, 13 reads, only what is brighter than the threshold (a soft knee - no edge to
//            where the glow begins), in linear light (an SDR picture's values taken to it by 2.2; scRGB as it is);
//   Down     halved again, 13 reads each time, down to 1/64 (or 8 pixels);
//   Up       from the smallest up: each size its own plus the one below, read by a tent of nine (drawn out sideways
//            by "Anamorphic");
//   Mix      the picture plus strength x the glow, back to its own encoding, into a picture of the engine's own,
//            copied back over it.
// HDR10 (PQ, R10G10B10A2) is left as it is: light cannot be added to PQ's numbers - said once in the log.

static const char kBloomHlsl[] = R"hlsl(
cbuffer Numbers : register(b0)
{
    float2 inv_in;      // one texel of what is read
    float2 inv_out;     // one texel of what is written
    float4 k;           // Pre: threshold, knee, linear (1: the picture is linear already); Up: wide; Mix: strength, linear
};
Texture2D<float4> t0 : register(t0);
Texture2D<float4> t1 : register(t1);
RWTexture2D<float4> out0 : register(u0);
SamplerState lin : register(s0);

float3 Linear(float3 c, float is_linear) { return is_linear > 0.5f ? c : pow(max(c, 0.0f), 2.2f); }
float3 Encoded(float3 c, float is_linear) { return is_linear > 0.5f ? c : pow(max(c, 0.0f), 1.0f / 2.2f); }

// Thirteen reads, the weights of a smooth halving (no shimmer where a small bright thing moves).
float3 Down13(Texture2D<float4> t, float2 uv, float2 px, float is_linear)
{
    const float3 a = Linear(t.SampleLevel(lin, uv + px * float2(-2, -2), 0).rgb, is_linear), b = Linear(t.SampleLevel(lin, uv + px * float2(0, -2), 0).rgb, is_linear);
    const float3 c = Linear(t.SampleLevel(lin, uv + px * float2(2, -2), 0).rgb, is_linear), d = Linear(t.SampleLevel(lin, uv + px * float2(-2, 0), 0).rgb, is_linear);
    const float3 e = Linear(t.SampleLevel(lin, uv, 0).rgb, is_linear), f = Linear(t.SampleLevel(lin, uv + px * float2(2, 0), 0).rgb, is_linear);
    const float3 g = Linear(t.SampleLevel(lin, uv + px * float2(-2, 2), 0).rgb, is_linear), h = Linear(t.SampleLevel(lin, uv + px * float2(0, 2), 0).rgb, is_linear);
    const float3 i = Linear(t.SampleLevel(lin, uv + px * float2(2, 2), 0).rgb, is_linear), j = Linear(t.SampleLevel(lin, uv + px * float2(-1, -1), 0).rgb, is_linear);
    const float3 l = Linear(t.SampleLevel(lin, uv + px * float2(1, -1), 0).rgb, is_linear), m = Linear(t.SampleLevel(lin, uv + px * float2(-1, 1), 0).rgb, is_linear);
    const float3 n = Linear(t.SampleLevel(lin, uv + px * float2(1, 1), 0).rgb, is_linear);
    return e * 0.125f + (a + c + g + i) * 0.03125f + (b + d + f + h) * 0.0625f + (j + l + m + n) * 0.125f;
}

[numthreads(8, 8, 1)]
void PreMain(uint3 id : SV_DispatchThreadID)
{
    const float2 uv = (id.xy + 0.5f) * inv_out;
    const float3 c = Down13(t0, uv, inv_in, k.z);
    // Soft knee: from threshold - knee up the glow grows smoothly, past threshold + knee it is all of what is over.
    const float bright = max(c.r, max(c.g, c.b));
    const float soft = clamp(bright - k.x + k.y, 0.0f, 2.0f * k.y);
    const float over = max(soft * soft / (4.0f * k.y + 1e-5f), bright - k.x);
    out0[id.xy] = float4(c * (over / max(bright, 1e-5f)), 1.0f);
}

[numthreads(8, 8, 1)]
void DownMain(uint3 id : SV_DispatchThreadID)
{
    const float2 uv = (id.xy + 0.5f) * inv_out;
    out0[id.xy] = float4(Down13(t0, uv, inv_in, 1.0f), 1.0f);
}

// t0: this size's own, t1: the glow of the size below. k.x: how far sideways the tent reaches (1 round; more: wide).
[numthreads(8, 8, 1)]
void UpMain(uint3 id : SV_DispatchThreadID)
{
    const float2 uv = (id.xy + 0.5f) * inv_out;
    const float2 d = inv_in * float2(k.x, 1.0f);
    float3 s = t1.SampleLevel(lin, uv, 0).rgb * 4.0f;
    s += (t1.SampleLevel(lin, uv + float2(-d.x, 0), 0).rgb + t1.SampleLevel(lin, uv + float2(d.x, 0), 0).rgb +
          t1.SampleLevel(lin, uv + float2(0, -d.y), 0).rgb + t1.SampleLevel(lin, uv + float2(0, d.y), 0).rgb) * 2.0f;
    s += t1.SampleLevel(lin, uv + float2(-d.x, -d.y), 0).rgb + t1.SampleLevel(lin, uv + float2(d.x, -d.y), 0).rgb +
         t1.SampleLevel(lin, uv + float2(-d.x, d.y), 0).rgb + t1.SampleLevel(lin, uv + float2(d.x, d.y), 0).rgb;
    out0[id.xy] = float4(t0.Load(int3(id.xy, 0)).rgb + s / 16.0f, 1.0f);
}

// t0: the picture, t1: the glow at half size. k.x: strength, k.y: linear.
[numthreads(8, 8, 1)]
void MixMain(uint3 id : SV_DispatchThreadID)
{
    const float4 p = t0.Load(int3(id.xy, 0));
    const float2 uv = (id.xy + 0.5f) * inv_out;
    const float3 glow = t1.SampleLevel(lin, uv, 0).rgb;
    out0[id.xy] = float4(Encoded(Linear(p.rgb, k.y) + k.x * glow, k.y), p.a);
}
)hlsl";

static struct BloomState
{
    static constexpr UINT kLevels = 6, kRing = 3, kPerFrame = 2 * kLevels + 1;
    winrt::com_ptr<ID3D12RootSignature> rs;
    winrt::com_ptr<ID3D12PipelineState> pre, down, up, mix;
    winrt::com_ptr<ID3D12DescriptorHeap> heap;
    winrt::com_ptr<ID3D12Resource> down_tex[kLevels], up_tex[kLevels], mixed;
    UINT lw[kLevels] = {}, lh[kLevels] = {}, levels = 0, w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT ring = 0;
    bool failed = false, told_pq = false, told_on = false;
    bool fresh = false;     // the working pictures were just made (COMMON): turned to UAVs on the next list
} g_bloom;

static bool BloomWanted() { return smOpt.bloom.load() > 0; }

static bool BloomBuild()
{
    BloomState &b = g_bloom;
    if (b.mix) return true;
    if (b.failed) return false;
    auto fail = [&](const char *what) { b.failed = true; Trace("[bloom] unavailable: %s", what); return false; };
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 2;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 1; ranges[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[0].Constants.Num32BitValues = 8;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges; params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX; samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 2; rsd.pParameters = params; rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &samp;
    winrt::com_ptr<ID3DBlob> blob, errors;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), errors.put())) ||
        FAILED(lfCore.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(b.rs.put()))))
        return fail("its root signature");
    struct { const char *entry; winrt::com_ptr<ID3D12PipelineState> *pso; } passes[] = {
        {"PreMain", &b.pre}, {"DownMain", &b.down}, {"UpMain", &b.up}, {"MixMain", &b.mix} };
    for (auto &pass : passes)
    {
        winrt::com_ptr<ID3DBlob> code; errors = nullptr;
        if (FAILED(CachedCompile(kBloomHlsl, sizeof(kBloomHlsl) - 1, "bloom.hlsl", nullptr, nullptr, pass.entry, "cs_5_0",
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(), errors.put())))
        { b.mix = nullptr; return fail(errors ? static_cast<const char *>(errors->GetBufferPointer()) : "a pass did not compile"); }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = b.rs.get(); pd.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        if (FAILED(lfCore.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(pass.pso->put())))) { b.mix = nullptr; return fail("a pass's pipeline"); }
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, BloomState::kRing * BloomState::kPerFrame * 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    if (FAILED(lfCore.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(b.heap.put())))) { b.mix = nullptr; return fail("its descriptor heap"); }
    return true;
}

static bool BloomTargets(UINT w, UINT h, DXGI_FORMAT format)
{
    BloomState &b = g_bloom;
    if (b.mixed && b.w == w && b.h == h && b.format == format) return true;
    // (Another size: the loop has waited for everything it sent before it builds for one - sm_loop.inl.)
    for (auto &t : b.down_tex) t = nullptr;
    for (auto &t : b.up_tex) t = nullptr;
    b.mixed = nullptr; b.w = b.h = 0; b.levels = 0;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {format};
    if (FAILED(lfCore.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
        !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
    { Trace("[bloom] the picture's format %d cannot be written by a compute pass here - no bloom", static_cast<int>(format)); return false; }
    UINT lw = w, lh = h;
    for (UINT i = 0; i < BloomState::kLevels; ++i)
    {
        lw = std::max(1u, (lw + 1) / 2); lh = std::max(1u, (lh + 1) / 2);
        if (i > 0 && (lw < 8 || lh < 8)) break;
        b.down_tex[i].attach(NewTexture(lw, lh, DXGI_FORMAT_R16G16B16A16_FLOAT, true));
        b.up_tex[i].attach(NewTexture(lw, lh, DXGI_FORMAT_R16G16B16A16_FLOAT, true));
        if (!b.down_tex[i] || !b.up_tex[i]) return false;
        b.lw[i] = lw; b.lh[i] = lh; ++b.levels;
    }
    b.mixed.attach(NewTexture(w, h, format, true));
    if (!b.mixed) return false;
    b.w = w; b.h = h; b.format = format; b.fresh = true;
    return true;
}

// The picture (at `state`, left so) with its glow, on the open list. False: no bloom (off, unsupported).
static bool BloomApply(ID3D12GraphicsCommandList *list, ID3D12Resource *picture, D3D12_RESOURCE_STATES state)
{
    BloomState &b = g_bloom;
    if (!BloomWanted() || !picture) { b.told_on = false; return false; }
    const D3D12_RESOURCE_DESC pd = picture->GetDesc();
    if (pd.Format == DXGI_FORMAT_R10G10B10A2_UNORM)
    {
        if (!b.told_pq) { b.told_pq = true; Trace("[bloom] the picture is HDR10 (PQ): light cannot be added to its numbers - no bloom"); }
        return false;
    }
    if (!BloomBuild() || !BloomTargets(static_cast<UINT>(pd.Width), pd.Height, pd.Format)) return false;
    const float is_linear = pd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1.0f : 0.0f;
    const float strength = smOpt.bloom.load() / 100.0f;
    const float wide = 1.0f + smOpt.bloom_wide.load() / 100.0f * 3.0f;
    // Bright enough to glow: in SDR from linear 0.35 (0.62 as shown - a dusk sky was all under 0.6), in linear HDR from 0.8.
    const float threshold = is_linear > 0.5f ? 0.8f : 0.35f, knee = threshold * 0.5f;
    if (!b.told_on)
    {
        b.told_on = true;
        Trace("[bloom] on: strength %d%%, %s, %u sizes from %ux%u (%s)", smOpt.bloom.load(), wide > 1.01f ? "anamorphic" : "round",
              b.levels, b.lw[0], b.lh[0], is_linear > 0.5f ? "linear HDR" : "SDR");
    }
    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    b.ring = (b.ring + 1) % BloomState::kRing;
    UINT slot = b.ring * BloomState::kPerFrame;
    ID3D12DescriptorHeap *heaps[] = {b.heap.get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(b.rs.get());
    auto run = [&](ID3D12PipelineState *pso, ID3D12Resource *in0, ID3D12Resource *in1, ID3D12Resource *out, UINT ow, UINT oh, UINT iw, UINT ih,
                   float k0, float k1, float k2) {
        auto cpu = b.heap->GetCPUDescriptorHandleForHeapStart(); cpu.ptr += SIZE_T(slot) * 3 * stride;
        auto gpu = b.heap->GetGPUDescriptorHandleForHeapStart(); gpu.ptr += UINT64(slot) * 3 * stride;
        ++slot;
        for (ID3D12Resource *in : {in0, in1})
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = in ? in->GetDesc().Format : DXGI_FORMAT_R16G16B16A16_FLOAT; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
            lfCore.device->CreateShaderResourceView(in, &srv, cpu); cpu.ptr += stride;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {}; uav.Format = out->GetDesc().Format; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        lfCore.device->CreateUnorderedAccessView(out, nullptr, &uav, cpu);
        const float numbers[8] = {1.0f / iw, 1.0f / ih, 1.0f / ow, 1.0f / oh, k0, k1, k2, 0.0f};
        list->SetPipelineState(pso);
        list->SetComputeRoot32BitConstants(0, 8, numbers, 0);
        list->SetComputeRootDescriptorTable(1, gpu);
        list->Dispatch((ow + 7) / 8, (oh + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER done = {}; done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; done.UAV.pResource = out;
        list->ResourceBarrier(1, &done);
    };
    auto to = [&](ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES into) {
        if (from == into) return; auto t = Barrier(r, from, into); list->ResourceBarrier(1, &t); };
    const auto UA = D3D12_RESOURCE_STATE_UNORDERED_ACCESS, READ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // The working pictures rest as UAVs; each is turned to be read when the next pass reads it, and back after.
    if (b.fresh)
    {
        b.fresh = false;
        std::vector<D3D12_RESOURCE_BARRIER> made;
        for (UINT i = 0; i < b.levels; ++i) { made.push_back(Barrier(b.down_tex[i].get(), D3D12_RESOURCE_STATE_COMMON, UA)); made.push_back(Barrier(b.up_tex[i].get(), D3D12_RESOURCE_STATE_COMMON, UA)); }
        made.push_back(Barrier(b.mixed.get(), D3D12_RESOURCE_STATE_COMMON, UA));
        list->ResourceBarrier(static_cast<UINT>(made.size()), made.data());
    }
    to(picture, state, READ);
    run(b.pre.get(), picture, nullptr, b.down_tex[0].get(), b.lw[0], b.lh[0], b.w, b.h, threshold, knee, is_linear);
    for (UINT i = 1; i < b.levels; ++i)
    {
        to(b.down_tex[i - 1].get(), UA, READ);
        run(b.down.get(), b.down_tex[i - 1].get(), nullptr, b.down_tex[i].get(), b.lw[i], b.lh[i], b.lw[i - 1], b.lh[i - 1], 0, 0, 0);
        to(b.down_tex[i - 1].get(), READ, UA);
    }
    // The smallest size's glow is its own; then each size up: its own plus the one below.
    const UINT last = b.levels - 1;
    {
        D3D12_RESOURCE_BARRIER rd[2] = {Barrier(b.down_tex[last].get(), UA, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                        Barrier(b.up_tex[last].get(), UA, D3D12_RESOURCE_STATE_COPY_DEST)};
        list->ResourceBarrier(2, rd);
        list->CopyResource(b.up_tex[last].get(), b.down_tex[last].get());
        D3D12_RESOURCE_BARRIER back[2] = {Barrier(b.down_tex[last].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, UA),
                                          Barrier(b.up_tex[last].get(), D3D12_RESOURCE_STATE_COPY_DEST, UA)};
        list->ResourceBarrier(2, back);
    }
    for (int i = static_cast<int>(last) - 1; i >= 0; --i)
    {
        to(b.down_tex[i].get(), UA, READ); to(b.up_tex[i + 1].get(), UA, READ);
        run(b.up.get(), b.down_tex[i].get(), b.up_tex[i + 1].get(), b.up_tex[i].get(), b.lw[i], b.lh[i], b.lw[i + 1], b.lh[i + 1], wide, 0, 0);
        to(b.down_tex[i].get(), READ, UA); to(b.up_tex[i + 1].get(), READ, UA);
    }
    to(b.up_tex[0].get(), UA, READ);
    run(b.mix.get(), picture, b.up_tex[0].get(), b.mixed.get(), b.w, b.h, b.lw[0], b.lh[0], strength, is_linear, 0);
    to(b.up_tex[0].get(), READ, UA);
    // Back over the picture.
    D3D12_RESOURCE_BARRIER copy_in[2] = {Barrier(b.mixed.get(), UA, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                         Barrier(picture, READ, D3D12_RESOURCE_STATE_COPY_DEST)};
    list->ResourceBarrier(2, copy_in);
    list->CopyResource(picture, b.mixed.get());
    D3D12_RESOURCE_BARRIER copy_out[2] = {Barrier(b.mixed.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, UA),
                                          Barrier(picture, D3D12_RESOURCE_STATE_COPY_DEST, state)};
    list->ResourceBarrier(2, copy_out);
    return true;
}
