// lf_gpu_core.inl - the D3D12 device and the one command list everything is
// recorded into: a ring of three allocators guarded by a timeline fence, and
// the card choice (the one picked in the app, else the first NVIDIA card).

// How many cards of a kind are really in the machine: the display devices on
// the PCI bus with that maker and model. DXGI alone does not say - a virtual
// display (Parsec, a remote desktop's) shows the card that draws it a second
// time, under a number of its own, and two cards of one model look the same.
static unsigned CardsInMachine(unsigned vendor, unsigned device)
{
    static const GUID display_class = { 0x4d36e968, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };
    const HDEVINFO set = SetupDiGetClassDevsW(&display_class, L"PCI", nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return 0;
    unsigned count = 0;
    SP_DEVINFO_DATA one = {};
    one.cbSize = sizeof(one);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &one); ++i)
    {
        wchar_t ids[512] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &one, SPDRP_HARDWAREID, nullptr, reinterpret_cast<PBYTE>(ids), sizeof(ids) - 4, nullptr)) continue;
        unsigned ven = 0, dev = 0;
        if (swscanf_s(ids, L"PCI\\VEN_%4x&DEV_%4x", &ven, &dev) == 2 && ven == vendor && dev == device) ++count;
    }
    SetupDiDestroyDeviceInfoList(set);
    return count;
}

// "--cards": the graphics cards the app may offer ("Preferred GPU"), one line
// each, and nothing else done -
//     @card <number> <1: DLSS MFG can run on it, 0: not> <MB of its own memory> <name>
// The number is the one "gpu=<number>" takes. XeSS MFG may be tried on any of
// them (Intel's library says at the start whether it runs); DLSS MFG needs
// an NVIDIA RTX card. A card DXGI shows more often than the machine has it
// is listed that often only.
static void CardsTell()
{
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) || !factory) return;
    std::map<unsigned long long, unsigned> listed;
    for (UINT i = 0; ; ++i)
    {
        IDXGIAdapter1 *card = nullptr;
        if (factory->EnumAdapters1(i, &card) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc = {};
        card->GetDesc1(&desc);
        const unsigned long long kind = (static_cast<unsigned long long>(desc.VendorId) << 32) | desc.DeviceId;
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || ++listed[kind] > std::max(1u, CardsInMachine(desc.VendorId, desc.DeviceId)))
        {
            card->Release();
            continue;
        }
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
        const bool dlss = desc.VendorId == 0x10DE && wcsstr(desc.Description, L"RTX") != nullptr;
        Status("@card %u %d %llu %s", i, dlss ? 1 : 0, static_cast<unsigned long long>(desc.DedicatedVideoMemory >> 20), name);
        // And XeSS MFG: asked of Intel's library itself (it refuses older graphics of Intel's own, for one).
        Status("@xess %u %d", i, XeRunsOn(card) ? 1 : 0);
        card->Release();
    }
    factory->Release();
}

// The device, its queue, the allocator ring and the fence, on the card the
// generator can use.
static bool GpuCoreStart()
{
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto create_device  = d3d12 ? reinterpret_cast<D3d12CreateFn>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    auto create_factory = dxgi ? reinterpret_cast<DxgiFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    if (create_device == nullptr || create_factory == nullptr) { Trace("[card] dxgi.dll / d3d12.dll lack the exports we need"); return false; }

    IDXGIFactory2 *dxgi_factory = nullptr;
    HRESULT status = create_factory(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&dxgi_factory));
    if (FAILED(status)) { Trace("[card] no DXGI factory (0x%08X)", status); return false; }

    const int wanted = CardWanted();
    IDXGIAdapter1 *nvidia = nullptr;
    IDXGIAdapter1 *chosen = nullptr;
    int nvidia_idx = -1;
    int chosen_idx = -1;
    SIZE_T fallback_memory = 0;
    for (UINT i = 0; ; ++i)
    {
        IDXGIAdapter1 *option = nullptr;
        if (dxgi_factory->EnumAdapters1(i, &option) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc = {};
        option->GetDesc1(&desc);

        Trace("[card] #%u %ls, vendor 0x%04X, %llu MB, luid %08X:%08X",
            i, desc.Description, desc.VendorId,
            (unsigned long long)(desc.DedicatedVideoMemory >> 20),
            (unsigned)desc.AdapterLuid.HighPart, (unsigned)desc.AdapterLuid.LowPart);
        const bool usable = (!NgxSelected() || desc.VendorId == 0x10DE) &&
                            !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
        if (wanted >= 0 && static_cast<int>(i) == wanted && usable)
        { chosen = option; chosen_idx = static_cast<int>(i); continue; }
        // XeSS may see an unsupported integrated Intel adapter before a
        // compatible discrete AMD/NVIDIA card. Prefer dedicated VRAM when
        // no adapter was explicitly selected; DLSS keeps its previous order.
        if (usable && (nvidia == nullptr || (!NgxSelected() && desc.DedicatedVideoMemory > fallback_memory)))
        {
            if (nvidia) nvidia->Release();
            nvidia = option; nvidia_idx = static_cast<int>(i);
            fallback_memory = desc.DedicatedVideoMemory;
            continue;
        }
        option->Release();
    }
    if (wanted >= 0 && chosen == nullptr)
    {
        Trace("[card] card %d is unavailable for the selected generator - taking the first compatible adapter",
            wanted);
        // The app is told: what it shows as picked is not what runs.
        if (nvidia != nullptr) Status("@note %s cannot run on the picked card - another one is used", GeneratorName());
    }
    if (chosen != nullptr)
    {
        if (nvidia != nullptr) nvidia->Release();
        nvidia = chosen;
        nvidia_idx = chosen_idx;
        Trace("[card] card %d picked in the app", wanted);
    }
    if (nvidia == nullptr) { dxgi_factory->Release(); Trace("[card] no compatible adapter in this machine"); return false; }

    lfCard.index = nvidia_idx;
    Trace("[card] card %d does both the generator and the source", nvidia_idx);
    if (FAILED(nvidia->QueryInterface(__uuidof(IDXGIAdapter3),
                                      reinterpret_cast<void **>(&lfCard.adapter))))
        lfCard.adapter = nullptr;

    status = create_device(nvidia, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                       reinterpret_cast<void **>(&lfCore.device));
    nvidia->Release();
    if (FAILED(status)) { Trace("[card] the D3D12 device did not start (0x%08X)", status); return false; }

    {
        ID3D12DeviceRemovedExtendedDataSettings1 *dred1 = nullptr;
        const HRESULT hr1 = lfCore.device->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedDataSettings1),
                                                  reinterpret_cast<void **>(&dred1));
        if (SUCCEEDED(hr1))
        {
            dred1->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred1->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred1->Release();
            Trace("[card] crash breadcrumbs on (DRED settings1)");
        }
        else
        {
            ID3D12DeviceRemovedExtendedDataSettings *dred = nullptr;
            const HRESULT hr0 = lfCore.device->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedDataSettings),
                                                      reinterpret_cast<void **>(&dred));
            if (SUCCEEDED(hr0))
            {
                dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->Release();
                Trace("[card] crash breadcrumbs on (DRED settings)");
            }
            else
            {
                Trace("[card] no crash breadcrumbs (settings1 0x%08X, settings 0x%08X)",
                    (unsigned)hr1, (unsigned)hr0);
            }
        }
    }

    dxgi_factory->Release();
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    lfCore.device->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&lfCore.direct_queue));
    if (lfCore.direct_queue == nullptr) { Trace("[card] no command queue"); return false; }
    // The output window has a queue of its own, ahead of the other. On one
    // queue everything runs in the order it was sent: a frame's small copy
    // into the back buffer, and the Present itself, waited behind the
    // generator's work on the next pair (3 to 6 ms on an RTX 3060) - "drawing
    // takes 7 ms" per frame under a game, the presenter fell behind and let
    // frames go.
    D3D12_COMMAND_QUEUE_DESC show_desc = {};
    show_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    lfCore.device->CreateCommandQueue(&show_desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&lfCore.show_queue));
    if (lfCore.show_queue == nullptr)
    {
        Trace("[card] no queue of its own for the output window - it shares the generator's");
        lfCore.show_queue = lfCore.direct_queue;
        lfCore.show_queue->AddRef();
    }
    Trace("[card] D3D12 device up");

    for (int i = 0; i < GpuCore::kRing; ++i)
        lfCore.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                      reinterpret_cast<void **>(&lfCore.allocators[i]));
    if (lfCore.allocators[0] != nullptr)
        lfCore.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, lfCore.allocators[0], nullptr,
                                 __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&lfCore.cmd));
    if (lfCore.cmd != nullptr) lfCore.cmd->Close();
    lfCore.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&lfCore.timeline));
    lfCore.timeline_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (lfCore.cmd == nullptr || lfCore.timeline == nullptr) { Trace("[card] no command list or fence"); return false; }

    return true;
}

// The card picked in the app, or -1 for the first NVIDIA one.
static int CardWanted()
{
    const int value = smOpt.gpu.load();
    return value >= 0 && value <= 63 ? value : -1;
}

// The card the device was really made on (the one picked before that).
static int CardInUse()
{
    return lfCard.index >= 0 ? lfCard.index : CardWanted();
}

// Open the command list on the next allocator of the ring, waiting for the
// card to be done with it first.
// One frame's work goes to the card in as few pieces as can be. Each piece
// sent by itself takes the card from the game and hands it back: measured
// under a game that fills the card, a quarter to a third of a millisecond of
// the game's time for each, whatever little it holds - the conversion of a
// captured frame (0.17 ms of work) cost the game half a millisecond sent
// alone. So a piece that has more coming is left open (CmdCarry): the next
// CmdOpen goes on in the same list, and whoever sends last sends it all.
// Nothing left open outlives its frame (CmdCarryFlush).
static bool g_cmd_carried = false;

static void CmdCarry()
{
    g_cmd_carried = true;
    lfProf.stage_now = GS_NONE;
}

static void CmdCarryFlush()
{
    if (!g_cmd_carried) return;
    g_cmd_carried = false;
    const UINT64 mark = CmdSubmit();
    if (mark != 0) AwaitMark(lfCore.timeline, mark, 2000);
}

// Everything sent so far has run: before what it uses is let go.
static void CmdDrain()
{
    if (lfCore.timeline && lfCore.timeline_value != 0 && !lfRun.gpu_lost) AwaitMark(lfCore.timeline, lfCore.timeline_value, 5000);
}

// The frame it was recorded for is nothing new (the picture before handed
// over again): never sent. The next CmdOpen takes the allocator again.
static void CmdCarryDrop()
{
    if (!g_cmd_carried) return;
    g_cmd_carried = false;
    lfCore.cmd->Close();
    lfProf.stage_now = GS_NONE;
}

static bool CmdOpen()
{
    if (lfRun.gpu_lost || lfCore.cmd == nullptr) return false;
    if (g_cmd_carried) { g_cmd_carried = false; return true; }
    const int slot = lfCore.ring_slot;
    const UINT64 done_mark = lfCore.allocator_mark[slot];
    if (done_mark != 0 && lfCore.timeline->GetCompletedValue() < done_mark)
    {
        const bool phase = PhasesOn();
        const double t_wait = phase ? PhaseClock() : 0.0;
        if (!AwaitMark(lfCore.timeline, done_mark, 2000))
        { Trace("[card] allocator %d is still busy on the card", slot); return false; }
        if (phase)
        {
            const double spent = PhaseClock() - t_wait;
            ++lfProf.allocator_waits;
            lfProf.allocator_wait_sum += spent;
            if (spent > lfProf.allocator_wait_peak) lfProf.allocator_wait_peak = spent;
        }
    }
    if (PhasesOn()) TimingCollect();
    if (FAILED(lfCore.allocators[slot]->Reset()))
    {
        ReportDeviceLoss("allocator reuse");
        return false;
    }
    return SUCCEEDED(lfCore.cmd->Reset(lfCore.allocators[slot], nullptr));
}

// Close and send the list; returns the fence value that marks its end
// (0 when the card refused it).
static UINT64 CmdSubmit()
{
    const bool phase = PhasesOn();
    const double t_submit = phase ? PhaseClock() : 0.0;
    const HRESULT closed = lfCore.cmd->Close();
    if (FAILED(closed))
    {
        Trace("[card] the command list did not close (0x%08X) - nothing sent", closed);
        lfRun.gpu_lost = true;
        lfProf.stage_now = GS_NONE;
        DropCommands();
        return 0;
    }
    ID3D12CommandList *lists[] = { lfCore.cmd };
    lfCore.direct_queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++lfCore.timeline_value;

    const HRESULT sig = lfCore.direct_queue->Signal(lfCore.timeline, v);
    if (FAILED(sig))
    {
        Trace("[card] the queue refused a signal (0x%08X)", sig);
        lfRun.gpu_lost = true;
        lfProf.stage_now = GS_NONE;
        return 0;
    }
    lfCore.allocator_mark[lfCore.ring_slot] = v;
    if (lfProf.stage_now != GS_NONE && lfProf.slot_stage[lfCore.ring_slot] != GS_NONE)
        lfProf.slot_mark[lfCore.ring_slot] = v;
    lfCore.ring_slot = (lfCore.ring_slot + 1) % GpuCore::kRing;
    if (phase)
    {
        lfProf.stamp.fence = v;
        if (lfProf.stage_now >= 0 && lfProf.stage_now < GS_COUNT)
        {
            const double spent = PhaseClock() - t_submit;
            lfProf.submit_sum[lfProf.stage_now] += spent;
            if (spent > lfProf.submit_peak[lfProf.stage_now]) lfProf.submit_peak[lfProf.stage_now] = spent;
            ++lfProf.submit_n[lfProf.stage_now];
        }
        const UINT64 reached = lfCore.timeline->GetCompletedValue();
        const UINT64 outstanding = reached == UINT64_MAX || reached >= v ? 0 : v - reached;
        if (outstanding > lfProf.in_flight_peak)
            lfProf.in_flight_peak = static_cast<unsigned>(outstanding);
    }
    lfProf.stage_now = GS_NONE;
    return v;
}

// Wait until a fence reaches a value, at most ms milliseconds. A removed
// device answers UINT64_MAX, which is a failure, not a success.
//
// Every thread that waits has an event of its own. There was one for all of
// them (round 19: "it runs for a minute or more, then stops by itself"): the
// loop's thread and the presenter's wait for the card at the same time, each
// for its own work, and the one signal of an event that resets itself went to
// whichever woke first. The loop took the presenter's, found its own work not
// done and waited on; the presenter's was gone. As long as the loop came back
// to wait again soon, the next signal let the presenter go a frame late. When
// the picture stood still - a menu, a loading screen, a page not scrolled -
// and the loop sent the card nothing, none came: two seconds on the presenter
// gave the card up ("GPU work timed out") and ended the engine, with the card
// in perfect order.
static bool AwaitMark(ID3D12Fence *f, UINT64 v, DWORD timeout_ms)
{
    if (v == 0) return false;

    if (f->GetCompletedValue() == UINT64_MAX) return false;
    if (f->GetCompletedValue() >= v) return true;

    static thread_local HANDLE own_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const HANDLE event = own_event ? own_event : lfCore.timeline_event;
    ResetEvent(event);
    if (f->GetCompletedValue() >= v) return true;
    if (FAILED(f->SetEventOnCompletion(v, event))) return false;
    const ULONGLONG until = GetTickCount64() + timeout_ms;
    for (;;)
    {
        const ULONGLONG now_ms = GetTickCount64();
        const DWORD left = now_ms >= until ? 0 : (DWORD)(until - now_ms);
        if (WaitForSingleObject(event, left) != WAIT_OBJECT_0) return false;
        const UINT64 now = f->GetCompletedValue();
        if (now == UINT64_MAX) return false;
        if (now >= v) return true;
    }
}

// Close the list and send it on the output window's own queue, after
// everything sent before on the other one, and wait for it: a back buffer may
// only be written on the queue its swap chain was made on (written on the
// other, the device was removed at the first frame shown without the
// generator). The allocator is free again when this returns.
static bool CmdSubmitShow(DWORD timeout_ms)
{
    if (lfCore.show_queue == lfCore.direct_queue)
    {
        const UINT64 mark = CmdSubmit();
        if (mark != 0) SpoutAfter(lfCore.direct_queue);
        return mark != 0 && AwaitMark(lfCore.timeline, mark, timeout_ms);
    }
    static ID3D12Fence *fence = nullptr;
    static UINT64 value = 0;
    if (fence == nullptr &&
        FAILED(lfCore.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&fence))))
        return false;
    if (FAILED(lfCore.cmd->Close()))
    {
        Trace("[card] the command list did not close - nothing sent");
        lfRun.gpu_lost = true;
        lfProf.stage_now = GS_NONE;
        DropCommands();
        return false;
    }
    if (lfCore.timeline_value != 0) lfCore.show_queue->Wait(lfCore.timeline, lfCore.timeline_value);
    // A captured frame's conversion may ride in this list (CmdCarry): not
    // before the capture's own device has copied it.
    if (lfCap.sync12 != nullptr && lfCap.sync_value > 1) lfCore.show_queue->Wait(lfCap.sync12, lfCap.sync_value - 1);
    ID3D12CommandList *lists[] = { lfCore.cmd };
    lfCore.show_queue->ExecuteCommandLists(1, lists);
    const bool signalled = SUCCEEDED(lfCore.show_queue->Signal(fence, ++value));
    SpoutAfter(lfCore.show_queue);
    lfProf.stage_now = GS_NONE;
    const int slot = lfCore.ring_slot;
    lfCore.ring_slot = (lfCore.ring_slot + 1) % GpuCore::kRing;
    const bool done = signalled && AwaitMark(fence, value, timeout_ms);
    // Not done in time: the allocator must not be reused until everything is.
    lfCore.allocator_mark[slot] = done ? 0 : lfCore.timeline_value;
    return done;
}

// Close the list even if a runtime left it in a state that throws.
static void CmdCloseSafe()
{
    __try { lfCore.cmd->Close(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Throw away a list a runtime crashed in: wait for what was already sent,
// then start a fresh list.
static void DropCommands()
{
    if (lfCore.cmd == nullptr) return;
    UINT64 done_mark = 0;
    for (int i = 0; i < GpuCore::kRing; ++i)
        if (lfCore.allocator_mark[i] > done_mark) done_mark = lfCore.allocator_mark[i];
    if (done_mark && !AwaitMark(lfCore.timeline, done_mark, 60000))
    { lfRun.gpu_lost = true; Trace("[card] old commands still running - the list cannot be replaced"); return; }
    CmdCloseSafe();
    lfCore.cmd->Release();
    lfCore.cmd = nullptr;
    HRESULT status = lfCore.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, lfCore.allocators[lfCore.ring_slot], nullptr,
                                        __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&lfCore.cmd));
    if (SUCCEEDED(status)) status = lfCore.cmd->Close();
    if (FAILED(status))
    {
        Trace("[card] no new command list (0x%08X)", status);
        if (lfCore.cmd) lfCore.cmd->Release();
        lfCore.cmd = nullptr;
        lfRun.gpu_lost = true;
    }
}

// A 2D texture in video memory.
static ID3D12Resource *NewTexture(UINT w, UINT h_, DXGI_FORMAT pixel_format, bool uav_view)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC res_desc = {};
    res_desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    res_desc.Width            = w;
    res_desc.Height           = h_;
    res_desc.DepthOrArraySize = 1;
    res_desc.MipLevels        = 1;
    res_desc.Format           = pixel_format;
    res_desc.SampleDesc.Count = 1;
    res_desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    res_desc.Flags            = uav_view ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource *t = nullptr;
    lfCore.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &res_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                   __uuidof(ID3D12Resource), reinterpret_cast<void **>(&t));
    return t;
}

// A state transition for one resource.
static D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}
