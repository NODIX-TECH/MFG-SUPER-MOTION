// lf_estimate.inl - the engine's own motion estimate (lf_estimate_shader.h):
// its seven kernels, compiled once and kept in the shader cache. It finds the
// motion where NVIDIA Optical Flow is not there, fails, or "GPU estimate" is
// picked in the app (lf_own_motion.inl records the passes).
#include "lf_estimate_shader.h"

static struct EstimateState
{
    ID3D12RootSignature  *est_rs = nullptr;
    ID3D12PipelineState  *est_pso[7] = {};   // EstBase EstHalf FlowCoarse FlowRefine FlowSpread FlowFinal FlowCheck
    bool                  est_ready = false, est_failed = false;
} g_flow;

static bool EnsureFlowEstimate()
{
    if (g_flow.est_ready) return true;
    if (g_flow.est_failed) return false;
    g_flow.est_failed = true;
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? static_cast<CompileFn>(&CachedCompile) : nullptr;
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    auto serialize = d3d12 ? reinterpret_cast<SerializeRootFn>(
                                 GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
    if (compile == nullptr || serialize == nullptr) return false;
    const char *entries[7] = { "EstBase", "EstHalf", "FlowCoarse", "FlowRefine", "FlowSpread", "FlowFinal", "FlowCheck" };
    ID3DBlob *codes[7] = {};
    HRESULT results[7] = {};
    ID3DBlob *logs[7] = {};
    auto build = [&](int i) {
        results[i] = compile(kFlowEstimateHlsl, sizeof(kFlowEstimateHlsl) - 1, "flow_estimate.hlsl", nullptr, nullptr,
                             entries[i], "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &codes[i], &logs[i]);
    };
    {
        std::vector<std::thread> builders;
        bool started[7] = {};
        for (int i = 1; i < 7; ++i)
        {
            try { builders.emplace_back(build, i); started[i] = true; }
            catch (...) {}
        }
        build(0);
        for (auto &b : builders) b.join();
        for (int i = 1; i < 7; ++i) if (!started[i]) build(i);
    }
    bool compiled = true;
    for (int i = 0; i < 7; ++i)
        if (FAILED(results[i]) || codes[i] == nullptr)
        {
            Trace("[motion] estimate kernel %s failed 0x%08X: %s", entries[i], results[i],
                logs[i] ? static_cast<const char *>(logs[i]->GetBufferPointer()) : "(no log)");
            compiled = false;
        }
    for (ID3DBlob *l : logs) if (l) l->Release();
    auto release_codes = [&]() { for (ID3DBlob *&c : codes) if (c) { c->Release(); c = nullptr; } };
    if (!compiled) { release_codes(); return false; }

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 5;               // the two frames, the outside motion x2, the pyramid
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 4;               // the pyramid, the working motion, the result x2
    ranges[1].OffsetInDescriptorsFromTableStart = 5;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 14;    // cbuffer EC
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 2;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &samp;
    ID3DBlob *rs_blob = nullptr, *errors = nullptr;
    HRESULT hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors);
    if (errors) errors->Release();
    if (FAILED(hr) || rs_blob == nullptr) { release_codes(); return false; }
    hr = lfCore.device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                    __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(&g_flow.est_rs));
    rs_blob->Release();
    for (int i = 0; i < 7 && SUCCEEDED(hr); ++i)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = g_flow.est_rs;
        pd.CS.pShaderBytecode = codes[i]->GetBufferPointer();
        pd.CS.BytecodeLength = codes[i]->GetBufferSize();
        hr = lfCore.device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(&g_flow.est_pso[i]));
    }
    release_codes();
    if (FAILED(hr))
    {
        Trace("[motion] estimate pipeline failed 0x%08X", hr);
        for (ID3D12PipelineState *&p : g_flow.est_pso) if (p) { p->Release(); p = nullptr; }
        if (g_flow.est_rs) { g_flow.est_rs->Release(); g_flow.est_rs = nullptr; }
        return false;
    }
    g_flow.est_failed = false;
    g_flow.est_ready = true;
    return true;
}

// How far the smallest level looks, by how long the pair is: a slow source
// moves further between two frames, and at a fast one the wide searches would
// only cost (at a 60 FPS source they would come out of the game's time).
static UINT EstimateRadius(double interval)
{
    return interval >= 1.0 / 20.0 ? 6u : (interval >= 1.0 / 40.0 ? 4u : 3u);
}
