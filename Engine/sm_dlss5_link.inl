// sm_dlss5_link.inl - frames from DLSS 5 MANAGER's LIVE FLOW instead of a
// capture of the game (the layout and the names: dlss5_link.h).
//
// With "Link to MFG SUPER MOTION" on in DLSS 5 MANAGER, LIVE FLOW takes the
// game, puts DLSS 5 on each of its frames and hands every finished frame over
// on the graphics card. When it offers them as START MOTION is pressed
// (option `link`, 1 by default), the engine takes the game's window LIVE FLOW
// took - no count, and the window is left as LIVE FLOW placed it - and its
// frames come from LIVE FLOW: copied out of the link's texture by the
// capture's own device after LIVE FLOW's copy into it is done (a fence the two
// programs share), then on through BridgeFrame like any captured frame, with
// the game's moment LIVE FLOW took it at. DLSS 5 works once a frame of the
// game; the frames in between are made from frames it already finished.
//
// While linked the Spout sender rests: OBS records LIVE FLOW's.
// LIVE FLOW gone (closed, the link switched off): the engine stops and says so.

static struct Dlss5Link
{
    bool on = false;
    HANDLE map = nullptr;
    LinkHeader *head = nullptr;
    HANDLE event = nullptr;
    HANDLE producer = nullptr;              // LIVE FLOW's worker, to know it is still there
    uint32_t producer_pid = 0;
    uint32_t generation = 0;                // of what is open
    ID3D11Texture2D *slot[kLinkSlots] = {};
    ID3D11Fence *ready = nullptr, *taken = nullptr;
    int64_t last = 0;                       // the newest sequence taken
    double beat_at = 0.0;
    unsigned frames = 0;
    bool lost = false;
} g_dlss5;

static bool Dlss5LinkOn() { return g_dlss5.on; }
// The source stops (WindowStop); the link's memory stays open for the next start.
static void Dlss5LinkPause() { g_dlss5.on = false; }
static bool Dlss5LinkLost() { return g_dlss5.lost; }

static int64_t Dlss5Ticks()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

static double Dlss5Seconds(int64_t ticks)
{
    static LARGE_INTEGER frequency = {};
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    return double(ticks) / double(frequency.QuadPart);
}

static void Dlss5LinkCloseTextures()
{
    for (auto &slot : g_dlss5.slot) if (slot) { slot->Release(); slot = nullptr; }
    if (g_dlss5.ready) { g_dlss5.ready->Release(); g_dlss5.ready = nullptr; }
    if (g_dlss5.taken) { g_dlss5.taken->Release(); g_dlss5.taken = nullptr; }
    g_dlss5.generation = 0;
}

static void Dlss5LinkStop()
{
    if (g_dlss5.head)
    {
        if (g_dlss5.head->consumer_pid == GetCurrentProcessId()) g_dlss5.head->consumer_pid = 0;
        UnmapViewOfFile(g_dlss5.head);
        g_dlss5.head = nullptr;
    }
    Dlss5LinkCloseTextures();
    if (g_dlss5.map) { CloseHandle(g_dlss5.map); g_dlss5.map = nullptr; }
    if (g_dlss5.event) { CloseHandle(g_dlss5.event); g_dlss5.event = nullptr; }
    if (g_dlss5.producer) { CloseHandle(g_dlss5.producer); g_dlss5.producer = nullptr; }
    if (g_dlss5.on) Trace("[dlss5] link closed after %u frames from LIVE FLOW", g_dlss5.frames);
    g_dlss5.on = false;
}

// Does LIVE FLOW offer its frames right now: the link's memory there, its
// worker alive and heard from within two seconds, a game's window named.
// Opens the memory and keeps it open when it does.
static bool Dlss5LinkOffered(HWND *game, bool *fill)
{
    if (smOpt.link.load() == 0) return false;
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kLinkMemory);
    if (map == nullptr) return false;
    auto *head = static_cast<LinkHeader *>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LinkHeader)));
    if (head == nullptr) { CloseHandle(map); return false; }
    bool ok = head->magic == kLinkMagic && head->version == kLinkVersion && head->producer_pid != 0 &&
              head->game_hwnd != 0 && Dlss5Ticks() - head->producer_beat < static_cast<int64_t>(2.0 / Dlss5Seconds(1));
    HANDLE producer = ok ? OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, head->producer_pid) : nullptr;
    if (ok && (producer == nullptr || WaitForSingleObject(producer, 0) == WAIT_OBJECT_0)) ok = false;
    const HWND hwnd = ok ? reinterpret_cast<HWND>(static_cast<uintptr_t>(head->game_hwnd)) : nullptr;
    if (ok && !IsWindow(hwnd)) ok = false;
    if (!ok)
    {
        if (producer) CloseHandle(producer);
        UnmapViewOfFile(head);
        CloseHandle(map);
        return false;
    }
    Dlss5LinkStop();
    g_dlss5.map = map;
    g_dlss5.head = head;
    g_dlss5.producer = producer;
    g_dlss5.producer_pid = head->producer_pid;
    g_dlss5.event = OpenEventW(SYNCHRONIZE, FALSE, kLinkEvent);
    *game = hwnd;
    *fill = head->fill != 0;
    Trace("[dlss5] LIVE FLOW of DLSS 5 MANAGER offers its frames (process %lu, window %p%s)",
          (unsigned long)head->producer_pid, (void *)hwnd, head->fill ? ", scaled onto its screen" : "");
    return true;
}

// The textures and fences of the generation LIVE FLOW is on now.
static bool Dlss5LinkOpen()
{
    const uint32_t generation = g_dlss5.head->generation;
    if (generation == 0) return false;            // no frame handed over yet
    if (generation == g_dlss5.generation && g_dlss5.slot[0]) return true;
    Dlss5LinkCloseTextures();
    if (!CaptureDeviceReady()) return false;
    ID3D11Device1 *d1 = nullptr;
    ID3D11Device5 *d5 = nullptr;
    if (FAILED(lfCap.d11->QueryInterface(__uuidof(ID3D11Device1), (void **)&d1)) ||
        FAILED(lfCap.d11->QueryInterface(__uuidof(ID3D11Device5), (void **)&d5)))
    {
        if (d1) d1->Release();
        Trace("[dlss5] the capture's device cannot open what LIVE FLOW shares");
        return false;
    }
    wchar_t name[160] = {};
    bool ok = true;
    for (UINT i = 0; i < kLinkSlots && ok; ++i)
    {
        swprintf_s(name, L"%ls_g%u_s%u", kLinkName, generation, i);
        ok = SUCCEEDED(d1->OpenSharedResourceByName(name, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                    __uuidof(ID3D11Texture2D), (void **)&g_dlss5.slot[i]));
    }
    ID3D11Fence **fences[2] = { &g_dlss5.ready, &g_dlss5.taken };
    const wchar_t *what[2] = { L"ready", L"taken" };
    for (int i = 0; i < 2 && ok; ++i)
    {
        swprintf_s(name, L"%ls_g%u_%ls", kLinkName, generation, what[i]);
        HANDLE handle = nullptr;
        ok = SUCCEEDED(lfCore.device->OpenSharedHandleByName(name, GENERIC_ALL, &handle)) && handle != nullptr &&
             SUCCEEDED(d5->OpenSharedFence(handle, __uuidof(ID3D11Fence), (void **)fences[i]));
        if (handle) CloseHandle(handle);
    }
    d1->Release();
    d5->Release();
    if (!ok)
    {
        Trace("[dlss5] the link's textures of generation %u could not be opened", generation);
        Dlss5LinkCloseTextures();
        return false;
    }
    g_dlss5.generation = generation;
    Trace("[dlss5] frames of %ux%u (format %u) from LIVE FLOW, generation %u", g_dlss5.head->width,
          g_dlss5.head->height, g_dlss5.head->format, generation);
    return true;
}

// The source starts: the window LIVE FLOW took, its frames from the link.
static bool Dlss5LinkStart(HWND hwnd)
{
    if (g_dlss5.head == nullptr) return false;
    if (!SwizzleReady() || !CaptureDeviceReady()) return false;
    g_dlss5.on = true;
    g_dlss5.lost = false;
    g_dlss5.last = g_dlss5.head->latest;      // only frames from now on
    g_dlss5.frames = 0;
    g_dlss5.head->consumer_pid = GetCurrentProcessId();
    g_dlss5.head->consumer_beat = Dlss5Ticks();
    lfCap.window = hwnd;
    lfCap.window_live = true;
    lfCap.window_dxgi = false;
    lfCap.monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    lfCap.display = HdrDisplayOf(lfCap.monitor);
    // The size of the frames to come: what LIVE FLOW hands over, or the
    // window's own while nothing has been yet.
    UINT w = g_dlss5.head->width, h = g_dlss5.head->height;
    if (w == 0 || h == 0)
    {
        RECT client = {};
        GetClientRect(hwnd, &client);
        w = static_cast<UINT>(client.right); h = static_cast<UINT>(client.bottom);
        for (int i = 0; i < 40 && g_dlss5.head->width == 0; ++i) Sleep(50);
        if (g_dlss5.head->width) { w = g_dlss5.head->width; h = g_dlss5.head->height; }
    }
    lfCap.w = w; lfCap.h = h;
    ShowAffinityApply();
    Trace("[dlss5] taking LIVE FLOW's frames of window %p at %ux%u", (void *)hwnd, w, h);
    return true;
}

// Is LIVE FLOW still there and still offering.
static bool Dlss5ProducerHere()
{
    const LinkHeader *head = g_dlss5.head;
    if (head == nullptr || head->magic != kLinkMagic || head->producer_pid != g_dlss5.producer_pid) return false;
    return g_dlss5.producer == nullptr || WaitForSingleObject(g_dlss5.producer, 0) != WAIT_OBJECT_0;
}

// The newest frame LIVE FLOW finished, into the pipeline; false when none is
// new (waits up to 100 ms for one, as the captures do).
static bool Dlss5LinkTake(Pipeline &pipe)
{
    lfCap.picture_changed = false;
    LinkHeader *head = g_dlss5.head;
    if (!g_dlss5.on || head == nullptr) return false;
    const double now = LatencyNow();
    if (now - g_dlss5.beat_at > 0.25)
    {
        g_dlss5.beat_at = now;
        head->consumer_beat = Dlss5Ticks();
        head->consumer_pid = GetCurrentProcessId();
        if (!Dlss5ProducerHere())
        {
            if (!g_dlss5.lost) Trace("[dlss5] LIVE FLOW stopped handing its frames over");
            g_dlss5.lost = true;
            return false;
        }
    }
    int64_t latest = head->latest;
    if (latest <= g_dlss5.last)
    {
        if (g_dlss5.event) WaitForSingleObject(g_dlss5.event, 100);
        else Sleep(4);
        latest = head->latest;
        if (latest <= g_dlss5.last)
        {
            if (latest < g_dlss5.last) g_dlss5.last = latest;   // LIVE FLOW started over
            return false;
        }
    }
    if (!Dlss5LinkOpen()) return false;
    const double take_start = PhaseClock();
    const UINT slot = static_cast<UINT>(latest % kLinkSlots);
    const LinkFrame frame = head->frame[slot];
    if (frame.sequence != latest) return false;    // written over on the way: the next one
    ID3D11DeviceContext4 *ctx4 = nullptr;
    if (FAILED(lfCap.d11_ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&ctx4))) return false;
    // LIVE FLOW's copy into the slot is done on the card before ours starts.
    ctx4->Wait(g_dlss5.ready, static_cast<UINT64>(latest));
    UINT new_w = 0, new_h = 0;
    DXGI_FORMAT new_format = DXGI_FORMAT_UNKNOWN;
    const BridgeResult st = BridgeFrame(g_dlss5.slot[slot], &new_w, &new_h, &new_format);
    // Ours is done: LIVE FLOW may write the slot again.
    ctx4->Signal(g_dlss5.taken, static_cast<UINT64>(latest));
    ctx4->Release();
    lfCap.d11_ctx->Flush();
    head->consumer_taken = latest;
    head->consumer_beat = Dlss5Ticks();
    g_dlss5.last = latest;
    PhaseCount(PHASE_TAKE, take_start);
    if (st == BridgeResult::NewFormat) return false;
    if (st == BridgeResult::NewSize)
    {
        Trace("[dlss5] LIVE FLOW's frames are %ux%u now - built again", new_w, new_h);
        DeskStop();          // the bridge is made again at the new size
        lfCap.w = new_w; lfCap.h = new_h;
        return false;
    }
    if (st != BridgeResult::Done) return false;
    lfCap.stamp = Dlss5Seconds(frame.stamp);
    lfCap.picture_changed = true;
    TimingCapture(take_start, static_cast<UINT64>(frame.stamp), "dlss5");
    if (++g_dlss5.frames == 1) Trace("[dlss5] the first frame from LIVE FLOW is here");
    return SwizzleIntoPicture(pipe);
}
