// lf_pipeline.inl - the textures a captured picture lives in on the card: the
// picture itself at the source's own size, and the motion field at the
// working size (the Flow slider's share of the picture).

// A texture plus the upload buffer that feeds it.
static bool StagedCreate(StagedTexture &staged, UINT w, UINT hgt, DXGI_FORMAT pixel_format, UINT tight_pitch,
                           D3D12_RESOURCE_FLAGS tex_flags)
{
    D3D12_HEAP_PROPERTIES vram_heap = {};
    vram_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC tex_desc = {};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = w; tex_desc.Height = hgt; tex_desc.DepthOrArraySize = 1; tex_desc.MipLevels = 1;
    tex_desc.Format = pixel_format; tex_desc.SampleDesc.Count = 1; tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex_desc.Flags = tex_flags;
    if (FAILED(lfCore.device->CreateCommittedResource(&vram_heap, D3D12_HEAP_FLAG_NONE, &tex_desc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&staged.gpu_tex)))) return false;

    UINT64 total_bytes = 0;
    lfCore.device->GetCopyableFootprints(&tex_desc, 0, 1, 0, &staged.layout, &staged.row_count, &staged.row_bytes, &total_bytes);
    if (staged.row_count != hgt || staged.row_bytes < tight_pitch) return false;
    D3D12_HEAP_PROPERTIES upload_heap = {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buf_desc = {};
    buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Width = total_bytes; buf_desc.Height = 1; buf_desc.DepthOrArraySize = 1; buf_desc.MipLevels = 1;
    buf_desc.SampleDesc.Count = 1; buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return SUCCEEDED(lfCore.device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource),
        reinterpret_cast<void **>(&staged.staging)));
}

// Copy rows from memory into the upload buffer.
static bool StagedFill(StagedTexture &staged, const BYTE *from, UINT from_pitch, UINT hgt)
{
    BYTE *into = nullptr;
    if (FAILED(staged.staging->Map(0, nullptr, reinterpret_cast<void **>(&into)))) return false;
    for (UINT y = 0; y < hgt; ++y)
        memcpy(into + static_cast<size_t>(y) * staged.layout.Footprint.RowPitch,
               from + static_cast<size_t>(y) * from_pitch, from_pitch);
    staged.staging->Unmap(0, nullptr);
    return true;
}

// Record the upload buffer -> texture copy.
static void StagedCopy(ID3D12GraphicsCommandList *cmdl, StagedTexture &staged)
{
    D3D12_TEXTURE_COPY_LOCATION from = {}, into = {};
    from.pResource = staged.staging;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = staged.layout;
    into.pResource = staged.gpu_tex;
    into.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cmdl->CopyTextureRegion(&into, 0, 0, 0, &from, nullptr);
}

// Free a staged texture.
static void StagedRelease(StagedTexture &staged)
{
    if (staged.gpu_tex != nullptr) { staged.gpu_tex->Release(); staged.gpu_tex = nullptr; }
    if (staged.staging != nullptr) { staged.staging->Release(); staged.staging = nullptr; }
}

// The picture at full_w x full_h (the source's size) and the motion field at
// the working size w x hgt. With both the same, nothing is "stretched".
static bool PipelineBuild(Pipeline &pipe, UINT w, UINT hgt, UINT full_w, UINT full_h)
{
    pipe.work_w = w; pipe.work_h = hgt;
    pipe.picture_new = true;
    pipe.screen_w = full_w; pipe.screen_h = full_h;
    pipe.stretched = (full_w > 0 && full_h > 0 && (full_w != w || full_h != hgt));
    const UINT pic_w = pipe.stretched ? full_w : w;
    const UINT pic_h = pipe.stretched ? full_h : hgt;
    if (!StagedCreate(pipe.picture, pic_w, pic_h, DXGI_FORMAT_R8G8B8A8_UNORM, pic_w * 4) ||
        !StagedCreate(pipe.motion, w, hgt, DXGI_FORMAT_R16G16_FLOAT, w * 4,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        return false;
    // No motion to begin with - and none ever, when the generator finds it
    // itself.
    const std::vector<BYTE> none(static_cast<size_t>(w) * 4, 0);
    BYTE *into = nullptr;
    if (FAILED(pipe.motion.staging->Map(0, nullptr, reinterpret_cast<void **>(&into)))) return false;
    for (UINT y = 0; y < hgt; ++y) memcpy(into + static_cast<size_t>(y) * pipe.motion.layout.Footprint.RowPitch, none.data(), none.size());
    pipe.motion.staging->Unmap(0, nullptr);
    // The picture and the motion rest as shader resources between frames (the
    // capture's copy, the estimates and the generator all expect them so);
    // they are made in COPY_DEST.
    if (!CmdOpen()) return false;
    StagedCopy(lfCore.cmd, pipe.motion);
    D3D12_RESOURCE_BARRIER rest[] = {
        Barrier(pipe.picture.gpu_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(pipe.motion.gpu_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    lfCore.cmd->ResourceBarrier(2, rest);
    pipe.fed = true;
    const UINT64 fence = CmdSubmit();
    if (!AwaitMark(lfCore.timeline, fence, 30000)) return false;
    pipe.picture_new = false;
    Trace("[pipe] picture %ux%u, motion field %ux%u", pic_w, pic_h, w, hgt);
    return true;
}

// Free them again.
static void PipelineRelease(Pipeline &pipe)
{
    OfaStop();
    OfaClearLatch();
    if (PhasesOn()) { ++lfProf.capture_gen; lfProf.last_source_qpc = 0; lfProf.stamp = {}; }
    StagedRelease(pipe.picture);
    StagedRelease(pipe.motion);
    pipe.fed = false;

    lfCap.has_frame = false;
}
