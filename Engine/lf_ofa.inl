// lf_ofa.inl - NVIDIA Optical Flow (the hardware engine on RTX cards): a
// session per size, the luma pair in, the motion field out, spread to the
// size the rest of the pipeline wants. Off on cards without it.
#include "include/nvofa/nvOpticalFlowD3D12.h"

static bool OfaWanted()
{
    // "NVIDIA Optical Flow" picked in the app.
    return smOpt.motion.load() == 0;
}

static struct OfaSession {
    HMODULE library = nullptr;
    NV_OF_D3D12_API_FUNCTION_LIST api{};
    NvOFHandle session = nullptr;
    winrt::com_ptr<ID3D12Fence> fence;
    UINT64 value = 0;
    winrt::com_ptr<ID3D12Resource> inputs[2], flow, cost, backward, reverse, backward_cost, mfg_motion;
    NvOFGPUBufferHandle registered[6] = {};
    winrt::com_ptr<ID3D12RootSignature> root;
    winrt::com_ptr<ID3D12PipelineState> expand;
    winrt::com_ptr<ID3D12DescriptorHeap> heap;
    UINT width = 0, height = 0, grid = 4, current = 0;
    bool valid = false, failed = false, bidirectional = false;
    unsigned sequence = 0;
    UINT64 copied_mark = 0;     // SM_DIAG: the input copy's mark and when the estimate was asked for
    double asked_at = 0.0;
} ofa;

static void OfaStop()
{
    auto &f = ofa;

    if (f.session) {
        if (lfCore.timeline) AwaitMark(lfCore.timeline, lfCore.timeline_value, 30000);
        if (f.fence) AwaitMark(f.fence.get(), f.value, 30000);
        for (auto &buffer : f.registered) if (buffer) {
            NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 p{}; p.hOFGpuBuffer = buffer;
            f.api.nvOFUnregisterResourceD3D12(&p); buffer = nullptr;
        }
    }

    for (auto &input : f.inputs) input = nullptr;
    f.flow = nullptr; f.cost = nullptr; f.backward = nullptr; f.reverse = nullptr;
    f.backward_cost = nullptr; f.mfg_motion = nullptr; f.bidirectional = false;
    f.root = nullptr; f.expand = nullptr;
    f.heap = nullptr;
    if (f.session) { f.api.nvOFDestroy(f.session); f.session = nullptr; }
    f.fence = nullptr; f.value = 0;
    f.width = f.height = f.current = 0; f.valid = false;
    if (f.library) { FreeLibrary(f.library); f.library = nullptr; }
    f.api = {};
}

static bool OfaFailed(const char *operation, NV_OF_STATUS answer)
{
    char detail[512] = {}; uint32_t size = sizeof(detail);
    if (ofa.session && ofa.api.nvOFGetLastError)
        ofa.api.nvOFGetLastError(ofa.session, detail, &size);
    Trace("[ofa] NVIDIA optical flow unavailable: %s (%u) %s; the engine's own GPU estimate takes over", operation, unsigned(answer), detail);
    ofa.failed = true;
    OfaStop();
    return false;
}

static void OfaClearLatch()
{
    ofa.failed = false;
}

static std::vector<uint32_t> OfaCaps(NV_OF_CAPS capability)
{
    auto &f = ofa; uint32_t count = 0;
    if (f.api.nvOFGetCaps(f.session, capability, nullptr, &count) != NV_OF_SUCCESS || count > 128) return {};
    std::vector<uint32_t> values(count);
    if (!count || f.api.nvOFGetCaps(f.session, capability, values.data(), &count) != NV_OF_SUCCESS) return {};
    return values;
}

static bool OfaTakesFormat(NV_OF_BUFFER_USAGE usage, DXGI_FORMAT format)
{
    auto &f = ofa; uint32_t count = 0;
    if (f.api.nvOFGetSurfaceFormatCountD3D12(f.session, usage, NV_OF_MODE_OPTICALFLOW, &count) != NV_OF_SUCCESS || count > 128) return false;
    std::vector<DXGI_FORMAT> formats(count);
    if (!count || f.api.nvOFGetSurfaceFormatD3D12(f.session, usage, NV_OF_MODE_OPTICALFLOW, formats.data()) != NV_OF_SUCCESS) return false;
    return std::find(formats.begin(), formats.end(), format) != formats.end();
}

static const char kOfaGrow[] = R"hlsl(
Texture2D<int2> ofaField : register(t0);
RWTexture2D<float2> motionOut : register(u0);
cbuffer Grow : register(b0) { uint outW, outH, picW, picH, grid, restart, dropSubpixel; };

float2 FieldAt(int2 at, int2 last) { return float2(ofaField.Load(int3(clamp(at, 0, last), 0))); }

[numthreads(8, 8, 1)]
void CSMain(uint3 cell : SV_DispatchThreadID)
{
    if (cell.x >= outW || cell.y >= outH) return;
    uint fieldW, fieldH;
    ofaField.GetDimensions(fieldW, fieldH);
    float2 pos = (float2(cell.xy) + .5) * float2(picW, picH) / float2(outW, outH) / grid - .5;
    int2 base = int2(floor(pos));
    float2 along = frac(pos);
    int2 last = int2(fieldW - 1, fieldH - 1);
    float2 v = lerp(lerp(FieldAt(base, last), FieldAt(base + int2(1, 0), last), along.x),
                    lerp(FieldAt(base + int2(0, 1), last), FieldAt(base + 1, last), along.x), along.y);
    v = v / 32.0 * float2(outW, outH) / float2(picW, picH);
    motionOut[cell.xy] = restart || !all(isfinite(v)) || (dropSubpixel && dot(v, v) < .25) ? float2(0, 0) : v;
}
)hlsl";

static bool OfaReady(UINT width, UINT height)
{
    auto &f = ofa;
    if (f.failed) return false;
    const bool both = false;   // DLSS MFG takes one field: this frame to the one before
    if (f.session && f.width == width && f.height == height && f.bidirectional == both) return true;
    if (f.session) Trace("[ofa] closing the %ux%u session (two-way %u -> %u)", f.width, f.height, f.bidirectional ? 1u : 0u, both ? 1u : 0u);
    OfaStop();
    Trace("[ofa] session closed");
    f.library = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!f.library) return OfaFailed("driver API missing", NV_OF_ERR_OF_NOT_AVAILABLE);
    auto create = reinterpret_cast<decltype(&NvOFAPICreateInstanceD3D12)>(GetProcAddress(f.library, "NvOFAPICreateInstanceD3D12"));
    if (!create) return OfaFailed("D3D12 entry point missing", NV_OF_ERR_OF_NOT_AVAILABLE);
    auto answer = create(NV_OF_API_VERSION, &f.api);
    if (answer != NV_OF_SUCCESS) return OfaFailed("API version", answer);
    if (!f.api.nvCreateOpticalFlowD3D12 || !f.api.nvOFInit || !f.api.nvOFDestroy ||
        !f.api.nvOFGetCaps || !f.api.nvOFGetSurfaceFormatCountD3D12 ||
        !f.api.nvOFGetSurfaceFormatD3D12 || !f.api.nvOFRegisterResourceD3D12 ||
        !f.api.nvOFUnregisterResourceD3D12 || !f.api.nvOFExecuteD3D12)
        return OfaFailed("incomplete API table", NV_OF_ERR_OF_NOT_AVAILABLE);
    answer = f.api.nvCreateOpticalFlowD3D12(lfCore.device, &f.session);
    if (answer != NV_OF_SUCCESS) return OfaFailed("create session", answer);
    auto grids = OfaCaps(NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES);
    auto minw = OfaCaps(NV_OF_CAPS_WIDTH_MIN), minh = OfaCaps(NV_OF_CAPS_HEIGHT_MIN);
    auto maxw = OfaCaps(NV_OF_CAPS_WIDTH_MAX), maxh = OfaCaps(NV_OF_CAPS_HEIGHT_MAX);
    if (grids.empty() || minw.empty() || minh.empty() || maxw.empty() || maxh.empty() ||
        width < minw[0] || height < minh[0] || width > maxw[0] || height > maxh[0])
        return OfaFailed("unsupported input dimensions", NV_OF_ERR_UNSUPPORTED_FEATURE);
    f.grid = 4u;
    for (unsigned wanted : { 2u, 4u, 1u })
        if (std::find(grids.begin(), grids.end(), wanted) != grids.end()) { f.grid = wanted; break; }
    if (f.grid != 1 && f.grid != 2 && f.grid != 4)
        return OfaFailed("unsupported output grid", NV_OF_ERR_UNSUPPORTED_FEATURE);
    if (!OfaTakesFormat(NV_OF_BUFFER_USAGE_INPUT, DXGI_FORMAT_R8_UNORM) ||
        !OfaTakesFormat(NV_OF_BUFFER_USAGE_OUTPUT, DXGI_FORMAT_R16G16_SINT) ||
        !OfaTakesFormat(NV_OF_BUFFER_USAGE_COST, DXGI_FORMAT_R8_UINT))
        return OfaFailed("unsupported flow/gray/cost format", NV_OF_ERR_UNSUPPORTED_FEATURE);
    NV_OF_INIT_PARAMS params{};
    params.width = width; params.height = height;
    params.outGridSize = static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(f.grid);
    params.mode = NV_OF_MODE_OPTICALFLOW; params.perfLevel = NV_OF_PERF_LEVEL_FAST;

    char preset[16] = {};
    GetEnvironmentVariableA("LF_NVOFA_PRESET", preset, sizeof(preset));
    if (strcmp(preset, "medium") == 0) params.perfLevel = NV_OF_PERF_LEVEL_MEDIUM;
    else if (strcmp(preset, "slow") == 0) params.perfLevel = NV_OF_PERF_LEVEL_SLOW;
    params.predDirection = both ? NV_OF_PRED_DIRECTION_BOTH : NV_OF_PRED_DIRECTION_FORWARD;

    char cost_env[8] = {};
    const bool want_cost = GetEnvironmentVariableA("LF_NVOFA_COST", cost_env,
                                                    sizeof(cost_env)) > 0
                           && cost_env[0] == '1';
    params.enableOutputCost = want_cost ? NV_OF_TRUE : NV_OF_FALSE;
    params.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
    answer = f.api.nvOFInit(f.session, &params);
    if (answer != NV_OF_SUCCESS) return OfaFailed("initialize", answer);
    if (FAILED(lfCore.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(f.fence.put()))))
        return OfaFailed("create fence", NV_OF_ERR_OUT_OF_MEMORY);
    const UINT fw = (width + f.grid - 1) / f.grid, fh = (height + f.grid - 1) / f.grid;
    for (auto &input : f.inputs) input.attach(NewTexture(width, height, DXGI_FORMAT_R8_UNORM, false));
    f.flow.attach(NewTexture(fw, fh, DXGI_FORMAT_R16G16_SINT, true));
    if (want_cost) f.cost.attach(NewTexture(fw, fh, DXGI_FORMAT_R8_UINT, true));
    if (both) {
        f.backward.attach(NewTexture(fw, fh, DXGI_FORMAT_R16G16_SINT, true));
        if (want_cost) f.backward_cost.attach(NewTexture(fw, fh, DXGI_FORMAT_R8_UINT, true));
    }
    if (!f.inputs[0] || !f.inputs[1] || !f.flow || (want_cost && !f.cost) ||
        (both && (!f.backward || (want_cost && !f.backward_cost))))
        return OfaFailed("allocate buffers", NV_OF_ERR_OUT_OF_MEMORY);
    ID3D12Resource *resources[] = {f.inputs[0].get(), f.inputs[1].get(), f.flow.get(), f.cost.get(), f.backward.get(), f.backward_cost.get()};
    for (unsigned i = 0; i < _countof(resources); ++i) {
        if (!resources[i]) continue;
        NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 p{};
        p.resource = resources[i]; p.hOFGpuBuffer = &f.registered[i];
        p.inputFencePoint = {lfCore.timeline, lfCore.timeline_value};
        p.outputFencePoint = {f.fence.get(), ++f.value};
        answer = f.api.nvOFRegisterResourceD3D12(f.session, &p);
        if (answer != NV_OF_SUCCESS) { --f.value; return OfaFailed("register buffer", answer); }
        if (!AwaitMark(f.fence.get(), f.value, 30000)) return OfaFailed("register fence", NV_OF_ERR_GENERIC);
    }
    D3D12_DESCRIPTOR_RANGE table[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1}};
    D3D12_ROOT_PARAMETER roots[2] = {};
    roots[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; roots[0].Constants = {0,0,7};
    roots[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; roots[1].DescriptorTable = {2,table};
    D3D12_ROOT_SIGNATURE_DESC res_desc = {2,roots,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    winrt::com_ptr<ID3DBlob> blob, diag;
    if (FAILED(D3D12SerializeRootSignature(&res_desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.put(), diag.put())) ||
        FAILED(lfCore.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(f.root.put()))))
        return OfaFailed("root signature", NV_OF_ERR_GENERIC);
    blob = nullptr; diag = nullptr;
    if (FAILED(CachedCompile(kOfaGrow, sizeof(kOfaGrow)-1, nullptr, nullptr, nullptr, "CSMain", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.put(), diag.put())))
        return OfaFailed("expand shader", NV_OF_ERR_GENERIC);
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{}; pd.pRootSignature = f.root.get(); pd.CS = {blob->GetBufferPointer(),blob->GetBufferSize()};
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,4,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    if (FAILED(lfCore.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(f.expand.put()))) ||
        FAILED(lfCore.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(f.heap.put())))) return OfaFailed("expand pipeline", NV_OF_ERR_GENERIC);
    f.width = width; f.height = height; f.bidirectional = both;
    Trace("[ofa] running: %ux%u, grid %u, two-way %u, cost %u; D3D12 driver path", width, height, f.grid, both, want_cost);
    Trace("[ofa] speed %s", params.perfLevel == NV_OF_PERF_LEVEL_FAST ? "fast" :
        (params.perfLevel == NV_OF_PERF_LEVEL_MEDIUM ? "medium" : "slow"));
    return true;
}

static void OfaDump(Pipeline &v);

static bool OfaRun(Pipeline &pipe, bool reset, UINT64 *sent_mark)
{
    auto &f = ofa;
    if (sent_mark) *sent_mark = 0;
    const bool own_gray = lfLuma.flow_tex != nullptr;
    ID3D12Resource *source = own_gray ? lfLuma.flow_tex : lfLuma.target;
    const UINT source_w = own_gray ? lfLuma.flow_w : lfLuma.w;
    const UINT source_h = own_gray ? lfLuma.flow_h : lfLuma.h;
    if (!lfLuma.mapped || source == nullptr || !OfaReady(source_w, source_h)) return false;
    const bool new_reverse = f.bidirectional && (!f.reverse || !f.mfg_motion || f.reverse->GetDesc().Width != pipe.work_w || f.reverse->GetDesc().Height != pipe.work_h);
    if (new_reverse) {
        if (!AwaitMark(lfCore.timeline, lfCore.timeline_value, 30000)) return OfaFailed("resize reverse fence", NV_OF_ERR_GENERIC);
        f.reverse.attach(NewTexture(pipe.work_w, pipe.work_h, DXGI_FORMAT_R16G16_FLOAT, true));
        f.mfg_motion.attach(NewTexture(pipe.work_w, pipe.work_h, DXGI_FORMAT_R16G16_FLOAT, false));
        if (!f.reverse || !f.mfg_motion) return OfaFailed("bidirectional motion allocation", NV_OF_ERR_OUT_OF_MEMORY);
    }

    char fail_at[16] = {};
    if (GetEnvironmentVariableA("LF_NVOFA_TEST_FAIL_AT", fail_at, sizeof(fail_at)) &&
        f.sequence == strtoul(fail_at, nullptr, 10))
        return OfaFailed("injected execute failure", NV_OF_ERR_GENERIC);
    if (!CmdOpen()) return OfaFailed("begin input copy", NV_OF_ERR_GENERIC);
    auto barrier = [](ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) { auto t=Barrier(r,a,b);lfCore.cmd->ResourceBarrier(1,&t); };
    if (new_reverse) {
        barrier(f.reverse.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(f.mfg_motion.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    barrier(source, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (unsigned i = 0; i < 2; ++i) if (i == f.current || !f.valid || reset) {
        barrier(f.inputs[i].get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        lfCore.cmd->CopyResource(f.inputs[i].get(), source);
        barrier(f.inputs[i].get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    }
    barrier(source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const UINT64 copied = CmdSubmit(); if (!copied) return OfaFailed("submit input copy", NV_OF_ERR_GENERIC);
    NV_OF_FENCE_POINT ready = {lfCore.timeline,copied}, complete = {f.fence.get(),++f.value};
    NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in{};

    in.inputFrame = f.registered[f.current]; in.referenceFrame = f.registered[1-f.current];
    in.disableTemporalHints = reset || !f.valid ? NV_OF_TRUE : NV_OF_FALSE;
    in.numFencePoints = 1; in.fencePoint = &ready;
    NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out{};
    out.outputBuffer = f.registered[2];
    out.bwdOutputBuffer = f.registered[4];
    out.bwdOutputCostBuffer = f.registered[5];

    out.outputCostBuffer = f.cost ? f.registered[3] : nullptr;
    out.fencePoint = &complete;
    auto answer = f.api.nvOFExecuteD3D12(f.session, &in, &out);
    if (answer != NV_OF_SUCCESS) { --f.value; return OfaFailed("execute", answer); }
    f.copied_mark = copied; f.asked_at = LatencyNow();
    if (FAILED(lfCore.direct_queue->Wait(f.fence.get(),f.value)) || !CmdOpen())
        return OfaFailed("wait for optical flow", NV_OF_ERR_GENERIC);
    barrier(f.flow.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(pipe.motion.gpu_tex, pipe.fed ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto cpu_at=f.heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC view_desc{}; view_desc.Format=DXGI_FORMAT_R16G16_SINT;view_desc.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view_desc.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view_desc.Texture2D.MipLevels=1;
    lfCore.device->CreateShaderResourceView(f.flow.get(),&view_desc,cpu_at);cpu_at.ptr+=lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc{};uav_desc.Format=DXGI_FORMAT_R16G16_FLOAT;uav_desc.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    lfCore.device->CreateUnorderedAccessView(pipe.motion.gpu_tex,nullptr,&uav_desc,cpu_at);
    ID3D12DescriptorHeap *heap_list[]={f.heap.get()};lfCore.cmd->SetDescriptorHeaps(1,heap_list);
    lfCore.cmd->SetComputeRootSignature(f.root.get());lfCore.cmd->SetPipelineState(f.expand.get());
    UINT constants[]={pipe.work_w,pipe.work_h,f.width,f.height,f.grid,UINT(reset || !f.valid),0u};
    lfCore.cmd->SetComputeRoot32BitConstants(0,7,constants,0);lfCore.cmd->SetComputeRootDescriptorTable(1,f.heap->GetGPUDescriptorHandleForHeapStart());
    lfCore.cmd->Dispatch((pipe.work_w+7)/8,(pipe.work_h+7)/8,1);
    if (f.bidirectional) {
        barrier(f.backward.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(f.reverse.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const UINT step_bytes = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto reverse_cpu = f.heap->GetCPUDescriptorHandleForHeapStart(); reverse_cpu.ptr += 2 * step_bytes;
        lfCore.device->CreateShaderResourceView(f.backward.get(), &view_desc, reverse_cpu); reverse_cpu.ptr += step_bytes;
        lfCore.device->CreateUnorderedAccessView(f.reverse.get(), nullptr, &uav_desc, reverse_cpu);
        auto reverse_gpu = f.heap->GetGPUDescriptorHandleForHeapStart(); reverse_gpu.ptr += 2 * step_bytes;
        lfCore.cmd->SetComputeRootDescriptorTable(1, reverse_gpu);
        lfCore.cmd->Dispatch((pipe.work_w+7)/8,(pipe.work_h+7)/8,1);
        barrier(f.reverse.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(f.backward.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    }
    if (f.bidirectional) {
        // The matching, unmodified pair of forward/backward fields.
        barrier(pipe.motion.gpu_tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(f.mfg_motion.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        lfCore.cmd->CopyResource(f.mfg_motion.get(), pipe.motion.gpu_tex);
        barrier(f.mfg_motion.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(pipe.motion.gpu_tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    } else
        barrier(pipe.motion.gpu_tex,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(f.flow.get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    const UINT64 done=CmdSubmit();if(!done)return OfaFailed("submit expansion", NV_OF_ERR_GENERIC);
    pipe.fed=true;f.valid=true;f.current=1-f.current;
    if (sent_mark) *sent_mark=done;
    else if (!AwaitMark(lfCore.timeline,done,30000)) return OfaFailed("expansion fence", NV_OF_ERR_GENERIC);
    OfaDump(pipe); ++f.sequence;
    return true;
}

static void OfaDump(Pipeline &pipe)
{
    char folder[MAX_PATH]={}; if(!GetEnvironmentVariableA("LF_NVOFA_DUMP",folder,MAX_PATH)) return;
    auto &f=ofa;
    auto option = [](const char *name, int fallback) {
        char value[16] = {};
        return GetEnvironmentVariableA(name, value, sizeof(value)) ? atoi(value) : fallback;
    };
    const int first = std::max(0, option("LF_NVOFA_DUMP_START", 0));
    const int count = option("LF_NVOFA_DUMP_COUNT", 0);
    const int width = option("LF_NVOFA_DUMP_WIDTH", 0);
    if (f.sequence < unsigned(first) || (count > 0 && f.sequence >= unsigned(first + count)) ||
        (width > 0 && f.width != unsigned(width))) return;
    std::vector<std::pair<ID3D12Resource *, const char *>> resources = {
        {f.flow.get(), "flow"}, {f.cost.get(), "cost"}, {pipe.motion.gpu_tex, "motion"}};
    if (option("LF_NVOFA_DUMP_PAIRS", 0)) {
        resources.push_back({f.inputs[1-f.current].get(), "current"});
        resources.push_back({f.inputs[f.current].get(), "previous"});
        resources.push_back({f.reverse.get(), "reverse"});
        char path[MAX_PATH]; sprintf_s(path, "%s/pair-%04u.json", folder, f.sequence);
        FILE *meta = nullptr;
        if (!fopen_s(&meta, path, "w") && meta) {
            fprintf(meta, "{\"input\":[%u,%u],\"motion\":[%u,%u],\"grid\":%u}",
                f.width, f.height, pipe.work_w, pipe.work_h, f.grid); fclose(meta);
        }
    }
    for (auto pair : resources) {
        if (!pair.first) continue;
        auto desc=pair.first->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows;UINT64 row_bytes,bytes;
        lfCore.device->GetCopyableFootprints(&desc,0,1,0,&fp,&rows,&row_bytes,&bytes);
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC res_desc{};res_desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;res_desc.Width=bytes;res_desc.Height=1;res_desc.DepthOrArraySize=res_desc.MipLevels=1;res_desc.SampleDesc.Count=1;res_desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        winrt::com_ptr<ID3D12Resource> readback_heap;if(FAILED(lfCore.device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&res_desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(readback_heap.put()))) || !CmdOpen())return;
        const auto state = pair.first == pipe.motion.gpu_tex || pair.first == f.reverse.get()
            ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COMMON;
        auto barriers_in=Barrier(pair.first,state,D3D12_RESOURCE_STATE_COPY_SOURCE);lfCore.cmd->ResourceBarrier(1,&barriers_in);
        D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=pair.first;a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;b.pResource=readback_heap.get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint=fp;
        lfCore.cmd->CopyTextureRegion(&b,0,0,0,&a,nullptr);
        auto barriers_out=Barrier(pair.first,D3D12_RESOURCE_STATE_COPY_SOURCE,state);lfCore.cmd->ResourceBarrier(1,&barriers_out);
        if(!AwaitMark(lfCore.timeline,CmdSubmit(),30000))return;
        BYTE *data=nullptr;D3D12_RANGE read={0,SIZE_T(bytes)},written={0,0};if(FAILED(readback_heap->Map(0,&read,reinterpret_cast<void**>(&data))))return;
        char path[MAX_PATH];sprintf_s(path,"%s/%s-%04u.bin",folder,pair.second,f.sequence);FILE *file=nullptr;
        if(!fopen_s(&file,path,"wb") && file) {for(UINT y=0;y<rows;++y)fwrite(data+SIZE_T(y)*fp.Footprint.RowPitch,1,SIZE_T(row_bytes),file);fclose(file);}
        readback_heap->Unmap(0,&written);
    }
}
