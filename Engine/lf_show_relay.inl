// lf_show_relay.inl - two cards (round 42): the screen is driven by another card than the generator's.
//
// Until now the output window's swap chain was made on the generator's own queue whatever card drove the screen,
// and Windows copied every frame across to that card itself, through the compositor: a frame could not be shown the
// moment it came (no tearing), the screen was never asked to follow the frames (no VRR, "VRR Requested: No"), and
// Low Latency's queue of one was the compositor's, not ours. A user's own case: an RTX 4090 for the game and an RTX
// 5070, its screen on it, for the frames in between; or the screen on the processor's own graphics.
//
// Now, when the screen is on another card, the swap chain is made on that card - a device and a queue of its own
// there - so tearing, VRR and Low Latency are that card's, as with one card. The generator's card goes on drawing
// exactly as before, into pictures of its own the size and format of the swap chain's ("stand-ins": what
// ShowBackBuffer hands out in place of the swap chain's buffers). At each Present the picture crosses once: copied on
// the generator's card into a buffer both cards can see (a heap shared across adapters, in the computer's memory),
// and on the screen's card from it into the swap chain's own buffer - each card waiting for the other on the card
// itself (two fences shared across adapters), never on the processor. Three such buffers in turn, so the generator
// can copy the next frame while the screen's card still reads the one before.
//
// With one card nothing of this is made and nothing changes. If a part of it cannot be made (an old driver, a card
// that does not share), the window is made as before. SM_RELAY=0 turns it off, to compare.

struct RelayState
{
    bool on = false;
    ID3D12Device *device = nullptr;             // on the screen's card
    ID3D12CommandQueue *queue = nullptr;        // the swap chain's queue there
    UINT count = 0, at = 0;                     // the stand-ins, and the one the next frame is drawn into
    ID3D12Resource *stand[8] = {};
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT w = 0, h = 0;
    static constexpr UINT kPasses = 3;
    ID3D12Heap *heap_gen = nullptr, *heap_show = nullptr;
    ID3D12Resource *pass_gen[kPasses] = {}, *pass_show[kPasses] = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot = {};
    UINT64 pass_bytes = 0;
    // made: the generator's card has copied a frame into a buffer (made there, opened on the screen's card);
    // read: the screen's card has copied it out (made there, opened on the generator's).
    ID3D12Fence *made_gen = nullptr, *made_show = nullptr, *read_show = nullptr, *read_gen = nullptr;
    UINT64 made = 0, read = 0, read_of[kPasses] = {};
    static constexpr UINT kRing = 4;
    ID3D12CommandAllocator *gen_alloc[kRing] = {}, *show_alloc[kRing] = {};
    UINT64 gen_mark[kRing] = {}, show_mark[kRing] = {};
    ID3D12GraphicsCommandList *gen_list = nullptr, *show_list = nullptr;
    UINT ring = 0;
    UINT64 frame = 0;
    HANDLE event = nullptr;
    bool told = false;
};
static RelayState lfRelay;

template <typename T> static void RelayDrop(T *&p) { if (p) { p->Release(); p = nullptr; } }

static bool RelayAwait(ID3D12Fence *fence, UINT64 value, DWORD ms)
{
    if (!fence || fence->GetCompletedValue() >= value) return true;
    if (FAILED(fence->SetEventOnCompletion(value, lfRelay.event))) return false;
    return WaitForSingleObject(lfRelay.event, ms) == WAIT_OBJECT_0 || fence->GetCompletedValue() >= value;
}

// Both cards done with everything of the relay's.
static void RelayIdle()
{
    if (!lfRelay.made_gen) return;
    if (lfCore.show_queue && SUCCEEDED(lfCore.show_queue->Signal(lfRelay.made_gen, ++lfRelay.made)))
        RelayAwait(lfRelay.made_gen, lfRelay.made, 2000);
    if (lfRelay.queue && lfRelay.read_show && SUCCEEDED(lfRelay.queue->Signal(lfRelay.read_show, ++lfRelay.read)))
        RelayAwait(lfRelay.read_show, lfRelay.read, 2000);
}

static void RelayDropBuffers()
{
    for (auto &s : lfRelay.stand) RelayDrop(s);
    for (UINT k = 0; k < RelayState::kPasses; ++k) { RelayDrop(lfRelay.pass_gen[k]); RelayDrop(lfRelay.pass_show[k]); lfRelay.read_of[k] = 0; }
    RelayDrop(lfRelay.heap_gen); RelayDrop(lfRelay.heap_show);
    lfRelay.count = 0; lfRelay.at = 0;
}

static void RelayStop()
{
    if (lfRelay.device) RelayIdle();
    RelayDropBuffers();
    RelayDrop(lfRelay.gen_list); RelayDrop(lfRelay.show_list);
    for (UINT i = 0; i < RelayState::kRing; ++i) { RelayDrop(lfRelay.gen_alloc[i]); RelayDrop(lfRelay.show_alloc[i]); lfRelay.gen_mark[i] = lfRelay.show_mark[i] = 0; }
    RelayDrop(lfRelay.made_gen); RelayDrop(lfRelay.made_show); RelayDrop(lfRelay.read_show); RelayDrop(lfRelay.read_gen);
    RelayDrop(lfRelay.queue); RelayDrop(lfRelay.device);
    if (lfRelay.event) { CloseHandle(lfRelay.event); lfRelay.event = nullptr; }
    lfRelay.on = false;
    lfRelay.made = lfRelay.read = 0;
    lfRelay.ring = 0;
    lfRelay.frame = 0;
    lfRelay.told = false;
}

// The stand-ins on the generator's card, and the buffers both cards see.
static bool RelayMakeBuffers(DXGI_FORMAT format, UINT w, UINT h, UINT count)
{
    RelayDropBuffers();
    count = std::min<UINT>(count, 8);
    D3D12_HEAP_PROPERTIES local = {}; local.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC picture = {};
    picture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    picture.Width = w; picture.Height = h; picture.DepthOrArraySize = 1; picture.MipLevels = 1;
    picture.Format = format; picture.SampleDesc.Count = 1;
    picture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    for (UINT i = 0; i < count; ++i)
        if (FAILED(lfCore.device->CreateCommittedResource(&local, D3D12_HEAP_FLAG_NONE, &picture, D3D12_RESOURCE_STATE_PRESENT, nullptr,
                                                          IID_PPV_ARGS(&lfRelay.stand[i]))))
        { Trace("[relay] no picture of %ux%u on the generator's card", w, h); RelayDropBuffers(); return false; }
    UINT rows = 0; UINT64 row_bytes = 0, bytes = 0;
    lfCore.device->GetCopyableFootprints(&picture, 0, 1, 0, &lfRelay.foot, &rows, &row_bytes, &bytes);
    // (The same layout on the screen's card: rows of 256-byte steps, as every card has them.)
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT there = {};
    lfRelay.device->GetCopyableFootprints(&picture, 0, 1, 0, &there, nullptr, nullptr, nullptr);
    if (there.Footprint.RowPitch != lfRelay.foot.Footprint.RowPitch) { Trace("[relay] the two cards lay a picture out differently"); RelayDropBuffers(); return false; }
    lfRelay.pass_bytes = (bytes + 65535) & ~UINT64(65535);
    D3D12_HEAP_DESC heap = {};
    heap.SizeInBytes = lfRelay.pass_bytes * RelayState::kPasses;
    heap.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
    HANDLE shared = nullptr;
    if (FAILED(lfCore.device->CreateHeap(&heap, IID_PPV_ARGS(&lfRelay.heap_gen))) ||
        FAILED(lfCore.device->CreateSharedHandle(lfRelay.heap_gen, nullptr, GENERIC_ALL, nullptr, &shared)))
    { Trace("[relay] no memory both cards can see"); RelayDropBuffers(); return false; }
    const HRESULT opened = lfRelay.device->OpenSharedHandle(shared, IID_PPV_ARGS(&lfRelay.heap_show));
    CloseHandle(shared);
    if (FAILED(opened)) { Trace("[relay] the screen's card cannot open the shared memory (0x%08X)", (unsigned)opened); RelayDropBuffers(); return false; }
    D3D12_RESOURCE_DESC pass = {};
    pass.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    pass.Width = lfRelay.pass_bytes; pass.Height = 1; pass.DepthOrArraySize = 1; pass.MipLevels = 1;
    pass.SampleDesc.Count = 1; pass.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    pass.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    for (UINT k = 0; k < RelayState::kPasses; ++k)
        if (FAILED(lfCore.device->CreatePlacedResource(lfRelay.heap_gen, k * lfRelay.pass_bytes, &pass, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                       IID_PPV_ARGS(&lfRelay.pass_gen[k]))) ||
            FAILED(lfRelay.device->CreatePlacedResource(lfRelay.heap_show, k * lfRelay.pass_bytes, &pass, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                        IID_PPV_ARGS(&lfRelay.pass_show[k]))))
        { Trace("[relay] no shared buffer"); RelayDropBuffers(); return false; }
    lfRelay.count = count; lfRelay.at = 0;
    lfRelay.format = format; lfRelay.w = w; lfRelay.h = h;
    return true;
}

// The card that drives the screen the window is on, when it is not the generator's: a device and a queue there,
// what the two cards share, and the stand-ins. False: one card, or no relay can be made (the window is made as before).
static bool RelayStart(HWND window, DXGI_FORMAT format, UINT w, UINT h, UINT count)
{
    RelayStop();
    char v[8] = {};
    if (GetEnvironmentVariableA("SM_RELAY", v, sizeof(v)) > 0 && v[0] == '0') return false;
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)))) return false;
    IDXGIAdapter1 *screen_card = nullptr;
    for (UINT i = 0; !screen_card; ++i)
    {
        IDXGIAdapter1 *card = nullptr;
        if (factory->EnumAdapters1(i, &card) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT o = 0; !screen_card; ++o)
        {
            IDXGIOutput *output = nullptr;
            if (FAILED(card->EnumOutputs(o, &output)) || !output) break;
            DXGI_OUTPUT_DESC desc = {};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) { screen_card = card; card->AddRef(); }
            output->Release();
        }
        card->Release();
    }
    factory->Release();
    if (!screen_card) return false;
    DXGI_ADAPTER_DESC1 screen = {};
    screen_card->GetDesc1(&screen);
    const LUID ours = lfCore.device->GetAdapterLuid();
    if (screen.AdapterLuid.LowPart == ours.LowPart && screen.AdapterLuid.HighPart == ours.HighPart) { screen_card->Release(); return false; }
    const HRESULT made = D3D12CreateDevice(screen_card, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&lfRelay.device));
    screen_card->Release();
    if (FAILED(made)) { Trace("[relay] the screen's card (%ls) gave no device (0x%08X) - Windows copies the frames across", screen.Description, (unsigned)made); RelayStop(); return false; }
    D3D12_COMMAND_QUEUE_DESC queue = {};
    queue.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    lfRelay.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ok = lfRelay.event && SUCCEEDED(lfRelay.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&lfRelay.queue)));
    for (UINT i = 0; ok && i < RelayState::kRing; ++i)
        ok = SUCCEEDED(lfCore.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&lfRelay.gen_alloc[i]))) &&
             SUCCEEDED(lfRelay.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&lfRelay.show_alloc[i])));
    ok = ok && SUCCEEDED(lfCore.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, lfRelay.gen_alloc[0], nullptr, IID_PPV_ARGS(&lfRelay.gen_list))) &&
         SUCCEEDED(lfRelay.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, lfRelay.show_alloc[0], nullptr, IID_PPV_ARGS(&lfRelay.show_list)));
    if (ok) { lfRelay.gen_list->Close(); lfRelay.show_list->Close(); }
    // The two fences, each made on one card and opened on the other.
    auto share = [](ID3D12Device *from, ID3D12Device *to, ID3D12Fence *&here, ID3D12Fence *&there) {
        HANDLE handle = nullptr;
        if (FAILED(from->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(&here))) ||
            FAILED(from->CreateSharedHandle(here, nullptr, GENERIC_ALL, nullptr, &handle))) return false;
        const bool opened = SUCCEEDED(to->OpenSharedHandle(handle, IID_PPV_ARGS(&there)));
        CloseHandle(handle);
        return opened;
    };
    ok = ok && share(lfCore.device, lfRelay.device, lfRelay.made_gen, lfRelay.made_show) &&
         share(lfRelay.device, lfCore.device, lfRelay.read_show, lfRelay.read_gen);
    if (!ok) { Trace("[relay] the screen's card (%ls) and the generator's cannot wait for each other - Windows copies the frames across", screen.Description); RelayStop(); return false; }
    if (!RelayMakeBuffers(format, w, h, count)) { RelayStop(); return false; }
    lfRelay.on = true;
    Trace("[relay] the screen is on %ls: the window's frames are presented by that card itself (VRR, tearing and Low Latency "
          "are its own); each frame crosses once from the generator's card, %.1f MB", screen.Description, lfRelay.pass_bytes / 1048576.0);
    return true;
}

// What the frames are drawn into: the swap chain's own buffer, or with two cards the stand-in of this frame.
static HRESULT ShowBackBuffer(REFIID riid, void **out)
{
    if (lfRelay.on)
    {
        ID3D12Resource *stand = lfRelay.count ? lfRelay.stand[lfRelay.at % lfRelay.count] : nullptr;
        return stand ? stand->QueryInterface(riid, out) : E_FAIL;
    }
    return lfShow.chain->GetBuffer(lfShow.chain->GetCurrentBackBufferIndex(), riid, out);
}

// The swap chain in another format (HDR on or off): the stand-ins follow it.
static HRESULT ShowResizeBuffers(DXGI_FORMAT format, UINT flags)
{
    if (!lfRelay.on) return lfShow.chain->ResizeBuffers(0, 0, 0, format, flags);
    RelayIdle();
    RelayDropBuffers();
    const HRESULT hr = lfShow.chain->ResizeBuffers(0, 0, 0, format, flags);
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    lfShow.chain->GetDesc1(&desc);
    if (!RelayMakeBuffers(desc.Format, desc.Width, desc.Height, desc.BufferCount)) return E_FAIL;
    return hr;
}

// Just before the Present: the frame drawn into this stand-in crosses to the swap chain's buffer on the screen's card.
static bool RelayBeforePresent()
{
    const UINT slot = static_cast<UINT>(lfRelay.frame % RelayState::kPasses);
    ID3D12Resource *stand = lfRelay.count ? lfRelay.stand[lfRelay.at % lfRelay.count] : nullptr;
    if (!stand) return false;
    const UINT r = lfRelay.ring;
    lfRelay.ring = (lfRelay.ring + 1) % RelayState::kRing;
    // The allocators of this turn: free once their last work has run.
    if (!RelayAwait(lfRelay.made_gen, lfRelay.gen_mark[r], 2000) || !RelayAwait(lfRelay.read_show, lfRelay.show_mark[r], 2000))
    { Trace("[relay] a card stayed busy with a frame of before"); return false; }
    // 1. On the generator's card: the stand-in into the shared buffer - once the screen's card has read what was in it.
    if (FAILED(lfRelay.gen_alloc[r]->Reset()) || FAILED(lfRelay.gen_list->Reset(lfRelay.gen_alloc[r], nullptr))) return false;
    D3D12_RESOURCE_BARRIER turn = {};
    turn.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    turn.Transition.pResource = stand; turn.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    turn.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; turn.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    lfRelay.gen_list->ResourceBarrier(1, &turn);
    D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
    from.pResource = stand; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = lfRelay.pass_gen[slot]; to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = lfRelay.foot;
    lfRelay.gen_list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    std::swap(turn.Transition.StateBefore, turn.Transition.StateAfter);
    lfRelay.gen_list->ResourceBarrier(1, &turn);
    if (FAILED(lfRelay.gen_list->Close())) return false;
    if (lfRelay.read_of[slot]) lfCore.show_queue->Wait(lfRelay.read_gen, lfRelay.read_of[slot]);
    ID3D12CommandList *gen[] = { lfRelay.gen_list };
    lfCore.show_queue->ExecuteCommandLists(1, gen);
    if (FAILED(lfCore.show_queue->Signal(lfRelay.made_gen, ++lfRelay.made))) return false;
    lfRelay.gen_mark[r] = lfRelay.made;
    // 2. On the screen's card: from the shared buffer into the swap chain's buffer, once the copy above is done.
    ID3D12Resource *back = nullptr;
    if (FAILED(lfShow.chain->GetBuffer(lfShow.chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back))) || !back) return false;
    bool ok = SUCCEEDED(lfRelay.show_alloc[r]->Reset()) && SUCCEEDED(lfRelay.show_list->Reset(lfRelay.show_alloc[r], nullptr));
    if (ok)
    {
        turn.Transition.pResource = back;
        turn.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; turn.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        lfRelay.show_list->ResourceBarrier(1, &turn);
        D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
        src.pResource = lfRelay.pass_show[slot]; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = lfRelay.foot;
        dst.pResource = back; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        lfRelay.show_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(turn.Transition.StateBefore, turn.Transition.StateAfter);
        lfRelay.show_list->ResourceBarrier(1, &turn);
        ok = SUCCEEDED(lfRelay.show_list->Close());
    }
    back->Release();
    if (!ok) return false;
    lfRelay.queue->Wait(lfRelay.made_show, lfRelay.made);
    ID3D12CommandList *show[] = { lfRelay.show_list };
    lfRelay.queue->ExecuteCommandLists(1, show);
    if (FAILED(lfRelay.queue->Signal(lfRelay.read_show, ++lfRelay.read))) return false;
    lfRelay.show_mark[r] = lfRelay.read;
    lfRelay.read_of[slot] = lfRelay.read;
    ++lfRelay.frame;
    if (!lfRelay.told) { lfRelay.told = true; Trace("[relay] first frame crossed to the screen's card"); }
    return true;
}

// After the Present: the next frame is drawn into the next stand-in.
static void RelayAfterPresent()
{
    if (lfRelay.count) lfRelay.at = (lfRelay.at + 1) % lfRelay.count;
}
