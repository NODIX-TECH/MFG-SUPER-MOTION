// lf_luma.inl - small grey copies of the picture: an area-averaged luma the
// loop compares from frame to frame (scene cuts, repeated frames), and the
// luma NVIDIA Optical Flow works on.

// The area-average luma pass.
static bool LumaPipelineReady()
{
    if (lfLuma.pso) return true;
    ID3DBlob *blob = nullptr, *err = nullptr;
    HRESULT status = CachedCompile(kLumaHlsl, sizeof(kLumaHlsl) - 1, "lf-luma.hlsl",
                            nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, &blob, &err);
    if (FAILED(status)) { Trace("[luma] shader did not compile: %s", err ? (char *)err->GetBufferPointer() : "?"); return false; }
    D3D12_DESCRIPTOR_RANGE r0 = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0 };
    D3D12_DESCRIPTOR_RANGE r1 = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0 };
    D3D12_ROOT_PARAMETER prm_desc[3] = {};
    prm_desc[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    prm_desc[0].Constants.Num32BitValues = 4; prm_desc[0].Constants.ShaderRegister = 0;
    prm_desc[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    prm_desc[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    prm_desc[1].DescriptorTable.NumDescriptorRanges = 1; prm_desc[1].DescriptorTable.pDescriptorRanges = &r0;
    prm_desc[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    prm_desc[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    prm_desc[2].DescriptorTable.NumDescriptorRanges = 1; prm_desc[2].DescriptorTable.pDescriptorRanges = &r1;
    prm_desc[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = 3; root_desc.pParameters = prm_desc;
    ID3DBlob *sig = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    { Trace("[luma] root signature did not serialize"); return false; }
    if (FAILED(lfCore.device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                          __uuidof(ID3D12RootSignature),
                                          reinterpret_cast<void **>(&lfLuma.root))))
    { Trace("[luma] root signature refused"); return false; }
    sig->Release();
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = lfLuma.root;
    pd.CS.pShaderBytecode = blob->GetBufferPointer(); pd.CS.BytecodeLength = blob->GetBufferSize();
    if (FAILED(lfCore.device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState),
                                                 reinterpret_cast<void **>(&lfLuma.pso))))
    { Trace("[luma] pipeline refused"); return false; }
    blob->Release();
    Trace("[luma] area-average pipeline ready");
    return true;
}

// The small luma at width x height: its memory and the texture behind it.
static bool LumaStart(UINT width, UINT height)
{
    LumaStop();
    if (width == 0 || height == 0) { Trace("[luma] channel stopped"); return true; }
    const size_t need = static_cast<size_t>(width) * height;
    lfLuma.view = static_cast<BYTE *>(VirtualAlloc(nullptr, need, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (lfLuma.view == nullptr) { Trace("[luma] %zu bytes could not be had (%lu)", need, GetLastError()); return false; }
    lfLuma.size = need; lfLuma.w = width; lfLuma.h = height;
    if (!LumaPipelineReady()) return false;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
                                    D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    if (FAILED(lfCore.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&lfLuma.heap)))) return false;

    D3D12_HEAP_PROPERTIES vram_heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC tex_desc = {};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = width; tex_desc.Height = height; tex_desc.DepthOrArraySize = 1; tex_desc.MipLevels = 1;
    tex_desc.Format = DXGI_FORMAT_R8_UNORM; tex_desc.SampleDesc.Count = 1;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(lfCore.device->CreateCommittedResource(&vram_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                              __uuidof(ID3D12Resource),
                                              reinterpret_cast<void **>(&lfLuma.target))))
    { Trace("[luma] no target texture"); return false; }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT rows = 0;
    UINT64 row_size = 0, total_bytes = 0;
    lfCore.device->GetCopyableFootprints(&tex_desc, 0, 1, 0, &fp, &rows, &row_size, &total_bytes);
    if (rows != height || row_size < width)
    { Trace("[luma] the copy layout is %ux%u, row %llu - not what was planned", rows, height, (unsigned long long)row_size); return false; }
    lfLuma.pitch = static_cast<UINT>(fp.Footprint.RowPitch);

    D3D12_HEAP_PROPERTIES readback_heap = { D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC buf_desc = {};
    buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Width = static_cast<UINT64>(lfLuma.pitch) * height; buf_desc.Height = 1; buf_desc.DepthOrArraySize = 1; buf_desc.MipLevels = 1;
    buf_desc.SampleDesc.Count = 1; buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(lfCore.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              __uuidof(ID3D12Resource),
                                              reinterpret_cast<void **>(&lfLuma.readback))))
    { Trace("[luma] no read-back buffer"); return false; }
    lfLuma.mapped = true;
    Trace("[luma] %ux%u in use (%zu bytes, row %u)", width, height, need, lfLuma.pitch);
    return true;
}

// Free the small luma.
static void LumaStop()
{
    FlowLumaStop();
    if (lfLuma.heap) { lfLuma.heap->Release(); lfLuma.heap = nullptr; }
    lfLuma.mapped = false;
    if (lfLuma.view) { VirtualFree(lfLuma.view, 0, MEM_RELEASE); lfLuma.view = nullptr; }

    if (lfLuma.target) { lfLuma.target->Release(); lfLuma.target = nullptr; }
    if (lfLuma.readback) { lfLuma.readback->Release(); lfLuma.readback = nullptr; }
    lfLuma.size = 0;
    lfLuma.w = lfLuma.h = 0;
    lfLuma.pitch = 0;
}

// Average the picture down into the small luma.
static bool LumaFromPicture()
{
    if (!lfLuma.mapped || !lfCap.rgba) return false;
    if (!CmdOpen()) return false;
    GpuStageOpen(GS_LUMA);

    D3D12_RESOURCE_BARRIER to_srv = Barrier(lfCap.rgba, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    lfCore.cmd->ResourceBarrier(1, &to_srv);

    const UINT step_bytes = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_at = lfLuma.heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC view_desc = {};
    view_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view_desc.Texture2D.MipLevels = 1;
    lfCore.device->CreateShaderResourceView(lfCap.rgba, &view_desc, cpu_at);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.Format = DXGI_FORMAT_R8_UNORM; uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu2 = cpu_at; cpu2.ptr += step_bytes;
    lfCore.device->CreateUnorderedAccessView(lfLuma.target, nullptr, &uav_desc, cpu2);
    ID3D12DescriptorHeap *heap_list[] = { lfLuma.heap };
    lfCore.cmd->SetDescriptorHeaps(1, heap_list);
    lfCore.cmd->SetComputeRootSignature(lfLuma.root);
    lfCore.cmd->SetPipelineState(lfLuma.pso);
    const UINT sizes[4] = { lfCap.w, lfCap.h, lfLuma.w, lfLuma.h };
    lfCore.cmd->SetComputeRoot32BitConstants(0, 4, sizes, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE g0 = lfLuma.heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE g1 = g0; g1.ptr += step_bytes;
    lfCore.cmd->SetComputeRootDescriptorTable(1, g0);
    lfCore.cmd->SetComputeRootDescriptorTable(2, g1);
    lfCore.cmd->Dispatch((lfLuma.w + 7) / 8, (lfLuma.h + 7) / 8, 1);

    if (FlowLumaReady(lfCap.w, lfCap.h))
    {
        D3D12_CPU_DESCRIPTOR_HANDLE ocpu = lfLuma.flow_heap->GetCPUDescriptorHandleForHeapStart();
        lfCore.device->CreateShaderResourceView(lfCap.rgba, &view_desc, ocpu);
        D3D12_CPU_DESCRIPTOR_HANDLE ocpu2 = ocpu;
        ocpu2.ptr += step_bytes;
        D3D12_UNORDERED_ACCESS_VIEW_DESC oud = {};
        oud.Format = DXGI_FORMAT_R8_UNORM;
        oud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        lfCore.device->CreateUnorderedAccessView(lfLuma.flow_tex, nullptr, &oud, ocpu2);
        ID3D12DescriptorHeap *oheaps[] = { lfLuma.flow_heap };
        lfCore.cmd->SetDescriptorHeaps(1, oheaps);
        const UINT osizes[4] = { lfCap.w, lfCap.h, lfLuma.flow_w, lfLuma.flow_h };
        lfCore.cmd->SetComputeRoot32BitConstants(0, 4, osizes, 0);
        D3D12_GPU_DESCRIPTOR_HANDLE og0 = lfLuma.flow_heap->GetGPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE og1 = og0;
        og1.ptr += step_bytes;
        lfCore.cmd->SetComputeRootDescriptorTable(1, og0);
        lfCore.cmd->SetComputeRootDescriptorTable(2, og1);
        lfCore.cmd->Dispatch((lfLuma.flow_w + 7) / 8, (lfLuma.flow_h + 7) / 8, 1);
        lfCore.cmd->SetDescriptorHeaps(1, heap_list);
    }

    D3D12_RESOURCE_BARRIER to_copy = Barrier(lfLuma.target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                D3D12_RESOURCE_STATE_COPY_SOURCE);
    lfCore.cmd->ResourceBarrier(1, &to_copy);
    D3D12_TEXTURE_COPY_LOCATION from = {}, into = {};
    from.pResource = lfLuma.target; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex = 0;
    into.pResource = lfLuma.readback; into.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    into.PlacedFootprint.Footprint.Width = lfLuma.w;
    into.PlacedFootprint.Footprint.Height = lfLuma.h;
    into.PlacedFootprint.Footprint.Depth = 1;
    into.PlacedFootprint.Footprint.RowPitch = lfLuma.pitch;
    into.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
    lfCore.cmd->CopyTextureRegion(&into, 0, 0, 0, &from, nullptr);

    D3D12_RESOURCE_BARRIER back_uav = Barrier(lfCap.rgba, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_BARRIER back_uav2 = Barrier(lfLuma.target, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_BARRIER backs[2] = { back_uav, back_uav2 };
    lfCore.cmd->ResourceBarrier(2, backs);
    GpuStageClose(GS_LUMA);
    const UINT64 fence = CmdSubmit();
    if (!GpuStageWait(GS_LUMA, fence, 10000)) { Trace("[luma] waited too long for the card"); return false; }

    BYTE *mapped = nullptr;
    D3D12_RANGE rr = { 0, static_cast<SIZE_T>(lfLuma.pitch) * lfLuma.h };
    if (FAILED(lfLuma.readback->Map(0, &rr, reinterpret_cast<void **>(&mapped)))) return false;
    for (UINT y = 0; y < lfLuma.h; ++y)
        memcpy(lfLuma.view + static_cast<size_t>(y) * lfLuma.w,
               mapped + static_cast<size_t>(y) * lfLuma.pitch, lfLuma.w);
    lfLuma.readback->Unmap(0, nullptr);
    return true;
}

// How much smaller the optical flow's luma is than the picture (LF_OFA_SCALE).
static UINT FlowLumaDivisor()
{
    static int cached = -1;
    if (cached < 0)
    {
        char text[8] = {};
        const DWORD len = GetEnvironmentVariableA("LF_OFA_SCALE", text, sizeof(text));
        const int value = (len > 0 && len < sizeof(text)) ? atoi(text) : 2;
        cached = (value >= 0 && value <= 16) ? value : 2;
    }
    return static_cast<UINT>(cached);
}

// The luma texture NVIDIA Optical Flow reads, at its size.
static bool FlowLumaReady(UINT capture_w, UINT capture_h)
{
    const UINT divisor = FlowLumaDivisor();
    if (divisor == 0 || capture_w == 0 || capture_h == 0 || !OfaWanted()) { FlowLumaStop(); return false; }

    // Frames in between made at half the picture or less also bound the
    // estimated motion; otherwise quality is not reduced simply because a
    // game has a high capture resolution.
    double max_w = 1280.0, max_h = 720.0;
    if (FgRequested() && lfOut.sent_passthrough && FlowShare() <= 50) { max_w = 640.0; max_h = 360.0; }
    const double scale = std::min({1.0 / divisor, max_w / capture_w, max_h / capture_h});
    UINT w = std::max(4u, static_cast<UINT>(capture_w * scale) & ~3u);
    UINT hgt = std::max(4u, static_cast<UINT>(capture_h * scale) & ~3u);
    if (lfLuma.flow_tex != nullptr && lfLuma.flow_w == w && lfLuma.flow_h == hgt) return true;
    FlowLumaStop();
    lfLuma.flow_tex = NewTexture(w, hgt, DXGI_FORMAT_R8_UNORM, true);
    if (lfLuma.flow_tex == nullptr) { Trace("[ofa] flow luma %ux%u did not come up", w, hgt); return false; }
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 2;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(lfCore.device->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap),
                                           reinterpret_cast<void **>(&lfLuma.flow_heap))))
    { FlowLumaStop(); return false; }

    D3D12_RESOURCE_BARRIER to_uav = Barrier(lfLuma.flow_tex, D3D12_RESOURCE_STATE_COMMON,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    lfCore.cmd->ResourceBarrier(1, &to_uav);
    lfLuma.flow_w = w;
    lfLuma.flow_h = hgt;
    Trace("[ofa] flow luma %ux%u (source %ux%u divided by %u)", w, hgt, capture_w, capture_h, divisor);
    return true;
}

// Free the optical flow's luma.
static void FlowLumaStop()
{
    if (lfLuma.flow_tex != nullptr) { lfLuma.flow_tex->Release(); lfLuma.flow_tex = nullptr; }
    if (lfLuma.flow_heap != nullptr) { lfLuma.flow_heap->Release(); lfLuma.flow_heap = nullptr; }
    lfLuma.flow_w = lfLuma.flow_h = 0;
}
