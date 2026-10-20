// lf_show.inl - the output window: a topmost, click-through popup over the
// captured screen with its own swap chain, hidden from screen capture so the
// worker never captures itself. In window mode it follows the captured
// window and hides while that window is minimised or covered.

// Where the output window goes: the picked screen's corner (the loop sets it
// before the window opens; a window source is followed from there on).
static POINT g_show_origin = { 0, 0 };
static void ShowOriginRead()
{
    lfShow.x = g_show_origin.x;
    lfShow.y = g_show_origin.y;
}

// Hide the output window from screen capture unless it has to be seen.
static void ShowAffinityApply()
{
    if (lfShow.hwnd == nullptr) return;
    const bool hide = lfCap.window_dxgi || !(lfCap.window_live || lfShow.capturable);
    const DWORD wanted = hide ? WDA_EXCLUDEFROMCAPTURE : 0u;
    if (!SetWindowDisplayAffinity(lfShow.hwnd, wanted))
        Trace("[screen] display affinity %lu refused (%lu)",
            wanted, GetLastError());
    else
        Trace("[screen] %s screen capture", hide ? "hidden from" : "VISIBLE to");
}

// The output window never takes clicks or focus.
static LRESULT CALLBACK ShowWndProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m)
    {
    case WM_APP + 42: BeginSnapshot(15000); return 0;
    case WM_APP + 43: EndSnapshot(); return 0;
    case WM_TIMER: if (wp == kSnapshotTimer) { EndSnapshot(); return 0; } break;

    case WM_NCHITTEST:     return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND:    return 1;

    case WM_CLOSE:         DestroyWindow(w); return 0;
    case WM_DESTROY:       PostQuitMessage(0); return 0;
    default: break;
    }
    return DefWindowProcW(w, m, wp, lp);
}

// The output window's own thread and message loop.
static DWORD WINAPI ShowThread(LPVOID)
{
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = ShowWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SuperMotionOutput";
    RegisterClassExW(&wc);

    lfShow.hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        wc.lpszClassName, L"MFG SUPER MOTION Output", WS_POPUP,
        lfShow.x, lfShow.y, static_cast<int>(lfShow.bw), static_cast<int>(lfShow.bh),
        nullptr, nullptr, wc.hInstance, nullptr);
    if (lfShow.hwnd == nullptr)
    {
        Trace("[screen] the output window did not open (%lu)", GetLastError());
        InterlockedExchange(&lfShow.state, -1);
        return 0;
    }

    ShowAffinityApply();

    SetWindowPos(lfShow.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_HIDEWINDOW);
    InterlockedExchange(&lfShow.state, 1);

    g_snapshot_hook = SetWindowsHookExW(WH_KEYBOARD_LL, SnapshotKeys, GetModuleHandleW(nullptr), 0);
    if (!g_snapshot_hook) Trace("[snapshot] the shortcut hook did not install (%lu)", GetLastError());
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_snapshot_hook) UnhookWindowsHookEx(g_snapshot_hook);
    g_snapshot_hook = nullptr;
    InterlockedExchange(&g_snapshot_frozen, 0);
    return 0;
}

// Open the output window and its swap chain at width x height.
static bool ShowStart(UINT width, UINT height, uint32_t flags)
{
    ShowStop();
    if (width < 64 || height < 64 || width > 7680 || height > 4320)
    {
        Trace("[screen] a %ux%u output window makes no sense - refused", width, height);
        return false;
    }
    lfShow.w = width;
    lfShow.h = height;
    ShowOriginRead();
    // A window scaled onto its screen (lf_window_fill.inl): the output window
    // and its swap chain take the whole screen that window is on, and every
    // frame is scaled onto it (ShowBlit).
    lfShow.bw = width;
    lfShow.bh = height;
    lfShow.upscaling = false;
    {
        std::lock_guard<std::mutex> hold(g_up.lock);
        g_up.placed = false;
    }
    g_up.announced = false;   // each window says how its first frame was scaled
    if (UpscaleWanted())
    {
        RECT monitor = {};
        if (UpscaleMonitor(monitor))
        {
            lfShow.bw = static_cast<UINT>(monitor.right - monitor.left);
            lfShow.bh = static_cast<UINT>(monitor.bottom - monitor.top);
            lfShow.x = monitor.left;
            lfShow.y = monitor.top;
            lfShow.upscaling = true;
            std::lock_guard<std::mutex> hold(g_up.lock);
            g_up.monitor = monitor;
        }
    }
    lfShow.vrr = VrrWanted();
    // Low Latency (lf_latency.inl): the window queues one frame at most.
    lfShow.low_latency = LowLatencyWanted();
    // (A chain that may tear is also what asks the screen for VRR: lf_vrr.inl, ShowPresentNow.)
    lfShow.tear_asked = ChainMayTear();
    lfShow.tearing = false;
    lfShow.capturable = (flags & kScreenCapturable) != 0;
    {
        // SM_SHOW_CAPTURABLE=1 (tests): the window can be photographed, to see what it shows.
        char v[4] = {};
        if (GetEnvironmentVariableA("SM_SHOW_CAPTURABLE", v, sizeof(v)) > 0 && v[0] == '1') lfShow.capturable = true;
    }
    lfShow.flags = flags;
    lfShow.stale = false;
    lfShow.state = 0;
    lfShow.thread = CreateThread(nullptr, 0, ShowThread, nullptr, 0, &lfShow.thread_id);
    if (lfShow.thread == nullptr) { Trace("[screen] the window thread did not start"); return false; }
    for (int i = 0; i < 500 && lfShow.state == 0; ++i) Sleep(4);
    if (lfShow.state != 1) { Trace("[screen] the output window never appeared"); ShowStop(); return false; }

    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    auto create_factory = dxgi ? reinterpret_cast<DxgiFactoryFn>(
                                     GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    if (create_factory == nullptr) { Trace("[screen] no DXGI factory call"); ShowStop(); return false; }
    IDXGIFactory2 *dxgi_factory = nullptr;
    HRESULT status = create_factory(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&dxgi_factory));
    if (FAILED(status)) { Trace("[screen] no DXGI factory (0x%08X)", status); ShowStop(); return false; }

    DXGI_SWAP_CHAIN_DESC1 view_desc = {};
    view_desc.Width       = lfShow.bw;
    view_desc.Height      = lfShow.bh;
    view_desc.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    view_desc.SampleDesc.Count = 1;
    view_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

    // DLSS MFG's producer and presenter overlap. With only two surfaces the
    // compositor can hold the one the GPU needs next, serializing the copy
    // and model work. Three surfaces do not add a queued present: DLSS still
    // owns the waitable and sets maximum frame latency to one.
    // Five for frames in between (round 11). With three - one on the screen,
    // one waiting for it, one to draw into - the screen took two frames a
    // refresh and no more: 400 a second at 200 Hz, whatever was asked. A
    // light game at x6 (83 pairs a second, 498 frames) had its drawings wait
    // for a surface, frames came late and were passed over, the count a pair
    // was cut to three and tried at four again every few seconds, and now and
    // then the presenter stood for a second or two behind it all ("very bad
    // lag"). Measured with five: 83 pairs with five each, none late, a frame
    // 1.8 ms from its Present to the screen. Six give nothing more.
    view_desc.BufferCount = FgRequested() ? 5u : lfShow.low_latency ? 2u : 3u;
    {
        // SM_BUFFERS=<n> (to measure): another number of them.
        char v[8] = {};
        if (FgRequested() && GetEnvironmentVariableA("SM_BUFFERS", v, sizeof(v)) > 0 && atoi(v) >= 2 && atoi(v) <= 8) view_desc.BufferCount = atoi(v);
    }
    view_desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    view_desc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
    if (FgRequested() || lfShow.low_latency) view_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    // Sync mode off (lf_vrr.inl): a chain that may show a frame in the middle
    // of a refresh. Only then - any other mode's chain is what it always was.
    if (lfShow.tear_asked)
    {
        IDXGIFactory5 *newer = nullptr;
        BOOL may = FALSE;
        if (SUCCEEDED(dxgi_factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void **>(&newer))) && newer != nullptr)
        {
            if (FAILED(newer->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &may, sizeof(may)))) may = FALSE;
            newer->Release();
        }
        lfShow.tearing = may != FALSE;
        if (lfShow.tearing) view_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    IDXGISwapChain1 *sc1 = nullptr;

    // Two cards (lf_show_relay.inl): the swap chain on the queue of the card that drives the screen.
    IUnknown *present_queue = lfCore.show_queue;
    if (RelayStart(lfShow.hwnd, view_desc.Format, view_desc.Width, view_desc.Height, view_desc.BufferCount)) present_queue = lfRelay.queue;
    status = dxgi_factory->CreateSwapChainForHwnd(present_queue, lfShow.hwnd, &view_desc, nullptr, nullptr, &sc1);
    if ((FAILED(status) || sc1 == nullptr) && lfRelay.on)
    {
        Trace("[relay] the screen's card made no swap chain (0x%08X) - Windows copies the frames across", (unsigned)status);
        RelayStop();
        status = dxgi_factory->CreateSwapChainForHwnd(lfCore.show_queue, lfShow.hwnd, &view_desc, nullptr, nullptr, &sc1);
    }
    if (FAILED(status) || sc1 == nullptr)
    {
        Trace("[screen] no swap chain for the window (0x%08X)", status);
        dxgi_factory->Release(); ShowStop(); return false;
    }
    dxgi_factory->MakeWindowAssociation(lfShow.hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    dxgi_factory->Release();

    IDXGISwapChain2 *sc2 = nullptr;
    status = sc1->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void **>(&sc2));
    if (SUCCEEDED(status) && sc2 != nullptr)
        sc2->Release();
    status = sc1->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void **>(&lfShow.chain));
    sc1->Release();
    if (FAILED(status) || lfShow.chain == nullptr)
    {
        Trace("[screen] IDXGISwapChain3 missing (0x%08X)", status);
        ShowStop(); return false;
    }
    // The window owns this handle across the generator's feature and format
    // recreations. The producer only consumes it in ordinary Low Latency output.
    if (lfShow.low_latency || FgRequested())
    {
        lfShow.chain->SetMaximumFrameLatency(1);
        lfShow.quick_waitable = lfShow.chain->GetFrameLatencyWaitableObject();
    }

    const LONG ex = GetWindowLongW(lfShow.hwnd, GWL_EXSTYLE);
    SetWindowLongW(lfShow.hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED | WS_EX_TRANSPARENT);
    if (!SetLayeredWindowAttributes(lfShow.hwnd, 0, 255, LWA_ALPHA))
        Trace("[screen] layered window setup refused (%lu)", GetLastError());
    SetWindowPos(lfShow.hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    Trace("[screen] output window %ux%u up (flip discard, clicks pass through%s)",
        lfShow.bw, lfShow.bh, lfShow.capturable ? ", visible to capture" : ", hidden from capture");
    if (lfShow.upscaling)
    {
        Trace("[upscale] a window of its own size: every frame scaled onto the %ux%u screen the window is on",
              lfShow.bw, lfShow.bh);
        UpscaleFollowStart();
    }
    if (lfShow.low_latency)
        Trace("[latency] Low Latency: one frame queued at most, the capture waits for a free buffer");
    if (lfShow.tear_asked && TearingWanted() && !lfShow.vrr)
        Trace(lfShow.tearing ? "[sync] off: a frame is shown the moment it is presented (tear lines), unless VRR is on"
                             : "[sync] off was asked, and this Windows shows no frame in the middle of a refresh: the newest at each refresh");
    else if (!lfShow.vrr && SyncEvery() > 0)
        Trace("[sync] VSync: a frame every %d refresh%s, shown frames capped at %d on this %.0f Hz screen",
              SyncEvery(), SyncEvery() > 1 ? "es" : "", PresentCap(), OutputRefresh());
    if (lfShow.vrr)
    {
        char cap[96];
        if (VrrCap() > 0) sprintf_s(cap, "shown frames capped at %d on this %.0f Hz screen", VrrCap(), OutputRefresh());
        else sprintf_s(cap, "frames as the multiplier says (Limit to refresh rate off) on this %.0f Hz screen", OutputRefresh());
        Trace(lfShow.tearing ? "[vrr] on: each frame is presented the moment it is due and the screen asked to follow it (VRR); %s"
                             : "[vrr] on, but this Windows cannot present a frame between refreshes: frames wait for the screen; %s", cap);
    }

    {
        const HMONITOR shown_on = MonitorFromWindow(lfShow.hwnd, MONITOR_DEFAULTTONEAREST);
        bool here = false;
        IDXGIFactory1 *dxgi_factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&dxgi_factory)))
        {
            IDXGIAdapter1 *card = nullptr;
            const int index = CardInUse();
            if (SUCCEEDED(dxgi_factory->EnumAdapters1(index >= 0 ? (UINT)index : 0, &card)))
            {
                for (UINT i = 0; !here; ++i)
                {
                    IDXGIOutput *output = nullptr;
                    if (FAILED(card->EnumOutputs(i, &output)) || output == nullptr) break;
                    DXGI_OUTPUT_DESC desc = {};
                    here = SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == shown_on;
                    output->Release();
                }
                card->Release();
            }
            dxgi_factory->Release();
        }
        MONITORINFOEXW info = {};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(shown_on, &info);
        Trace(here ? "[screen] shown on %ls, driven by the generator's card"
                 : lfRelay.on ? "[screen] shown on %ls, driven by ANOTHER card than the generator's - its frames presented by that card (lf_show_relay.inl)"
                 : "[screen] shown on %ls, driven by ANOTHER card than the generator's - "
                   "Windows copies every frame across", info.szDevice);
    }
    return true;
}

// Close them.
static void ShowStop()
{
    CloseFgResources();
    if (lfShow.quick_waitable) { CloseHandle(lfShow.quick_waitable); lfShow.quick_waitable = nullptr; }
    if (lfShow.chain != nullptr) { lfShow.chain->Release(); lfShow.chain = nullptr; }
    RelayStop();

    if (lfShow.hwnd != nullptr) PostMessageW(lfShow.hwnd, WM_CLOSE, 0, 0);
    if (lfShow.thread != nullptr)
    {
        if (WaitForSingleObject(lfShow.thread, 2000) != WAIT_OBJECT_0)
        {
            Trace("[screen] the window thread stayed after WM_CLOSE");
            if (lfShow.thread_id != 0) PostThreadMessageW(lfShow.thread_id, WM_QUIT, 0, 0);
            WaitForSingleObject(lfShow.thread, 1000);
        }
        CloseHandle(lfShow.thread);
        lfShow.thread = nullptr;
    }
    lfShow.hwnd = nullptr;
    lfShow.thread_id = 0;
    lfShow.w = lfShow.h = 0;
    lfShow.bw = lfShow.bh = 0;
    lfShow.upscaling = false;
    lfShow.vrr = false;
    lfShow.low_latency = false;
    lfShow.tear_asked = lfShow.tearing = false;
    // A new window starts at the screen's corner; the next WindowTrack must
    // place it again even though the captured window did not move.
    lfShow.follow_rect = RECT{};
    lfShow.state = 0;

    lfShow.visible = false;
    lfShow.revealed = false;

    lfShow.color_space_set = false;
    lfShow.color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
}

// Can the result go straight to the output window.
static bool ShowDirect(const Pipeline &pipe)
{
    if (lfShow.chain == nullptr) return false;
    // A window scaled onto its screen: the picture is smaller than the output
    // window on purpose; every present scales it (ShowBlit).
    if (lfShow.upscaling) return true;
    const UINT ow = pipe.stretched ? pipe.screen_w : pipe.work_w;
    const UINT oh = pipe.stretched ? pipe.screen_h : pipe.work_h;
    if (ow != lfShow.w || oh != lfShow.h)
    {
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            Trace("[screen] picture %ux%u and window %ux%u differ - nothing is shown", ow, oh, lfShow.w, lfShow.h);
        }

        lfShow.visible = false;
        return false;
    }
    return true;
}

// Put the output window back on top when something pushed over it.
static void ShowStayOnTop()
{
    if (lfCap.window_live) return;
    if (lfShow.hwnd == nullptr) return;
    static ULONGLONG last_check = 0, burst_start = 0;
    static int burst = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - burst_start > 20000) { burst_start = now; burst = 0; }
    if (now - last_check < (burst >= 5 ? 10000u : 2000u)) return;
    last_check = now;
    HWND top = GetTopWindow(0);
    if (top == nullptr || top == lfShow.hwnd) return;
    wchar_t cls[64];

    if (GetClassNameW(top, cls, 64) > 0
        && wcscmp(cls, L"SuperMotionOutput") == 0)
        return;
    RECT r = {};
    if (GetWindowRect(top, &r) && r.right == r.left && r.bottom == r.top)
        return;

    if (!IsWindowVisible(top)) return;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(top, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return;
    const LONG_PTR ex = GetWindowLongPtrW(top, GWL_EXSTYLE);
    if ((ex & WS_EX_TRANSPARENT) != 0 && (ex & WS_EX_LAYERED) != 0) return;
    RECT mine = {}, both = {};
    if (!GetWindowRect(lfShow.hwnd, &mine) || !IntersectRect(&both, &r, &mine)) return;
    const LONGLONG mine_area = static_cast<LONGLONG>(mine.right - mine.left) * (mine.bottom - mine.top);
    const LONGLONG over = static_cast<LONGLONG>(both.right - both.left) * (both.bottom - both.top);
    if (mine_area <= 0 || over * 2 < mine_area) return;
    SetWindowPos(lfShow.hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ++burst;
}

// Round 42: the output was taken away because the capture handed over nothing (sm_loop.inl); only a new frame of
// the game brings it back - not a frame in between of the pair before, still on its way to the screen.
static std::atomic<bool> g_show_aside{false};

// The window stays hidden until it has a first picture.
static void ShowRevealOnce()
{
    if (lfShow.hwnd == nullptr || lfShow.revealed || g_show_aside.load()) return;
    lfShow.revealed = true;
    if (lfCap.window_live && lfCap.window != nullptr)
    {
        RECT r = {};
        if (WindowBounds(r) && !WindowInView(r)) return;
    }
    ShowWindow(lfShow.hwnd, SW_SHOWNOACTIVATE);
    lfShow.visible = true;
    Trace("[screen] window shown with its first picture");
}

// What a Present answered: a mode change rebuilds the window.
static bool ShowResult(HRESULT status, const char *where)
{
    if (status == DXGI_STATUS_MODE_CHANGED || status == DXGI_STATUS_MODE_CHANGE_IN_PROGRESS)
    {
        Trace("[screen] %s: the screen mode changed under the swap chain "
            "(0x%08X) - new window", where, (unsigned)status);
        lfShow.stale = true;
        return true;
    }
    if (FAILED(status))
    {
        Trace("[screen] %s failed 0x%08X", where, (unsigned)status);
        return false;
    }
    return true;
}

// Rebuild the window when an earlier Present asked for it.
static bool ShowRebuildIfStale()
{
    // VRR only changes how frames are presented (lf_vrr.inl): no rebuild.
    if (lfShow.chain != nullptr && VrrWanted() != lfShow.vrr)
    {
        lfShow.vrr = VrrWanted();
        if (lfShow.vrr)
            Trace(VrrCap() > 0 ? "[vrr] on: shown frames capped at %d on this %.0f Hz screen" : "[vrr] on (frames as the multiplier says: %d is no cap) on this %.0f Hz screen", VrrCap(), OutputRefresh());
        else
            Trace("[vrr] off: the newest frame is shown at each refresh");
    }
    // Scaling a window source onto its screen, and Low Latency, are properties
    // of the window and its chain: their size, place and buffers.
    if (lfShow.chain != nullptr && lfShow.w != 0 && !lfShow.stale)
    {
        if (UpscaleWanted() != lfShow.upscaling)
        {
            Trace("[upscale] scaling onto the screen %s - the output window is rebuilt for it",
                UpscaleWanted() ? "starts" : "ends");
            lfShow.stale = true;
        }
        else if (LowLatencyWanted() != lfShow.low_latency)
        {
            Trace("[latency] Low Latency %s - the output window is rebuilt for it",
                LowLatencyWanted() ? "on" : "off");
            lfShow.stale = true;
        }
        else if (ChainMayTear() != lfShow.tear_asked)
        {
            Trace("[sync] %s - the output window is rebuilt for it", VrrWanted() ? "VRR on" : TearingWanted() ? "sync mode off" : "VRR off");
            lfShow.stale = true;
        }
        else if (lfShow.upscaling && lfCap.window_live)
        {
            // The captured window went to another screen.
            static ULONGLONG checked = 0;
            const ULONGLONG now = GetTickCount64();
            RECT monitor = {}, current = {};
            if (now - checked >= 1000 && UpscaleMonitor(monitor))
            {
                checked = now;
                {
                    std::lock_guard<std::mutex> hold(g_up.lock);
                    current = g_up.monitor;
                }
                if (!EqualRect(&monitor, &current))
                {
                    Trace("[upscale] the window is on another screen now - the output window follows it");
                    lfShow.stale = true;
                }
            }
        }
    }
    if (!lfShow.stale || lfShow.w == 0) return true;
    const UINT w = lfShow.w, h = lfShow.h;
    const uint32_t flags = lfShow.flags;
    ShowStop();
    if (!ShowStart(w, h, flags))
    { Trace("[screen] no new window after the mode change"); return false; }
    lfOut.force_next = true;
    return true;
}

// Show the source picture untouched.
static bool ShowPassThrough(Pipeline &pipe)
{
    FgStopIfFailed();
    SpoutFollow();
    if (!ShowRebuildIfStale()) return false;
    if (lfHdr.keep) return ShowHdr(pipe);
    if (HdrSwitchOn() && !ShowFormatReady(false)) return false;
    if (FgRequested() && FgPresent(pipe, pipe.picture.gpu_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) return true;

    g_up.in_real = false;       // no frame generation: "Upscaling" sharpens on the way to the screen (ShowBlit)
    ID3D12Resource *back_buffer = nullptr;
    if (FAILED(ShowBackBuffer(__uuidof(ID3D12Resource), reinterpret_cast<void **>(&back_buffer))) || back_buffer == nullptr)
    { Trace("[screen] no back buffer for the untouched picture"); return false; }

    bool ok = false;
    if (CmdOpen())
    {
        GpuStageOpen(GS_SHOW);
        D3D12_RESOURCE_BARRIER barriers_in[] = {
            Barrier(back_buffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
            Barrier(pipe.picture.gpu_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COPY_SOURCE),
        };
        lfCore.cmd->ResourceBarrier(_countof(barriers_in), barriers_in);
        ShowBlit(lfCore.cmd, back_buffer, pipe.picture.gpu_tex);
        ShowProof(lfCore.cmd, back_buffer, true);
        CounterStamp(lfCore.cmd, back_buffer);
        BannerStamp(lfCore.cmd, back_buffer);
        D3D12_RESOURCE_BARRIER barriers_out[] = {
            Barrier(back_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT),
            Barrier(pipe.picture.gpu_tex, D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        lfCore.cmd->ResourceBarrier(_countof(barriers_out), barriers_out);
        GpuStageClose(GS_SHOW);
        if (CmdSubmitShow(2000))
        {
            if (PhasesOn()) lfProf.stamp.present_call = PhaseClock();
            ok = ShowResult(ShowPresentNow(), "present");
            if (!ok) lfProf.stamp.present_call = 0.0;
        }
        else
            Trace("[screen] waited too long for the untouched picture");
    }
    back_buffer->Release();
    if (ok) ShowRevealOnce();
    return ok;
}

// The captured window's rectangle on the desktop.
static bool WindowBoundsWhole(RECT &r)
{
    if (lfCap.window_dxgi) {
        POINT origin = {};
        if (!GetClientRect(lfCap.window, &r) || !ClientToScreen(lfCap.window, &origin)) return false;
        OffsetRect(&r, origin.x, origin.y);
        return true;
    }
    return SUCCEEDED(DwmGetWindowAttribute(lfCap.window, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) ||
           GetWindowRect(lfCap.window, &r);
}

// The part of the captured window the picture is of: less the sides Crop input leaves out (lf_desktop.inl,
// BridgeFrame, on the same terms).
static bool WindowBounds(RECT &r)
{
    if (!WindowBoundsWhole(r)) return false;
    if (Dlss5LinkOn()) return true;
    const RECT m = CropMargins(r.right - r.left, r.bottom - r.top);
    r.left += m.left; r.top += m.top; r.right -= m.right; r.bottom -= m.bottom;
    return true;
}

// How much of the captured window other windows cover, 0..1.
static float WindowCoverage(const RECT &target)
{
    const LONGLONG area = static_cast<LONGLONG>(target.right - target.left) *
                          static_cast<LONGLONG>(target.bottom - target.top);
    if (area <= 0) return 0.0f;
    LONGLONG covered = 0;
    int guard = 0;
    for (HWND w = GetWindow(lfCap.window, GW_HWNDPREV); w != nullptr && guard < 512;
         w = GetWindow(w, GW_HWNDPREV), ++guard)
    {
        if (w == lfShow.hwnd || !IsWindowVisible(w) || IsIconic(w)) continue;
        BOOL cloaked = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
            continue;
        const LONG_PTR ex = GetWindowLongPtrW(w, GWL_EXSTYLE);
        if ((ex & WS_EX_TRANSPARENT) != 0 && (ex & WS_EX_LAYERED) != 0) continue;
        RECT r = {};
        if (FAILED(DwmGetWindowAttribute(w, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) &&
            !GetWindowRect(w, &r))
            continue;
        RECT i = {};
        if (!IntersectRect(&i, &r, &target)) continue;
        covered += static_cast<LONGLONG>(i.right - i.left) * static_cast<LONGLONG>(i.bottom - i.top);
        if (covered >= area) break;
    }
    return static_cast<float>(static_cast<double>(covered) / static_cast<double>(area));
}

// Is the captured window visible enough to draw over.
static bool WindowInView(const RECT &target)
{
    // (Multi-display mode, round 43: another program in front - on another screen - leaves the picture up.)
    if (lfCap.window_dxgi && !smOpt.multi_display.load()) {
        DWORD source = 0, front = 0;
        GetWindowThreadProcessId(lfCap.window, &source);
        GetWindowThreadProcessId(GetForegroundWindow(), &front);
        if (source != front) return false;
    }
    return !IsIconic(lfCap.window) && WindowCoverage(target) < 0.25f;
}

// Should the output window show right now (settles over a few checks).
static bool WindowWantsOverlay(const RECT &target)
{
    static bool settled = true;
    static bool pending = true;
    static ULONGLONG pending_since = 0;
    if (IsIconic(lfCap.window)) { settled = pending = false; return false; }
    if (lfCap.window_dxgi && !WindowInView(target)) { settled = pending = false; return false; }
    const bool now_ok = WindowInView(target);
    const ULONGLONG now = GetTickCount64();
    if (now_ok != pending) { pending = now_ok; pending_since = now; }
    if (pending != settled && now - pending_since >= 200) settled = pending;
    return settled;
}

// Keep the output window on the captured window as it moves.
static void WindowTrack()
{
    if (!lfCap.window_live || lfShow.hwnd == nullptr || lfCap.window == nullptr) return;
    if (!IsWindow(lfCap.window)) return;
    RECT r = {};
    if (!WindowBounds(r)) return;
    if (lfShow.upscaling ? !UpscaleWantsOverlay() : !WindowWantsOverlay(r))
    {
        if (lfShow.visible)
        {
            ShowWindow(lfShow.hwnd, SW_HIDE);
            lfShow.visible = false;
            Trace(IsIconic(lfCap.window) ? "[window] the window is minimised - output hidden"
                  : lfShow.upscaling ? "[window] the game is not in front - the scaled picture is hidden"
                                     : "[window] something covers the window - output hidden");
        }
        return;
    }
    if (!lfShow.visible && lfShow.revealed)
    {
        ShowWindow(lfShow.hwnd, SW_SHOWNOACTIVATE);
        lfShow.visible = true;
        Trace("[window] the window is back - output shown");
    }
    // A window scaled onto its screen: the output window stays on the whole
    // screen (the game's window moves under it with the pointer); only where
    // the picture lands is worked out again.
    if (lfShow.upscaling)
    {
        RECT monitor = {};
        {
            std::lock_guard<std::mutex> hold(g_up.lock);
            monitor = g_up.monitor;
        }
        if (!EqualRect(&monitor, &lfShow.follow_rect))
        {
            lfShow.follow_rect = monitor;
            const bool picture_on_top = GetTopWindow(nullptr) == lfShow.hwnd;
            SetWindowPos(lfShow.hwnd, picture_on_top ? nullptr : HWND_TOPMOST,
                         monitor.left, monitor.top, monitor.right - monitor.left, monitor.bottom - monitor.top,
                         SWP_NOACTIVATE | (picture_on_top ? SWP_NOZORDER : 0));
        }
        UpscaleTrack(r);
        return;
    }
    if (r.left != lfShow.follow_rect.left || r.top != lfShow.follow_rect.top ||
        r.right != lfShow.follow_rect.right || r.bottom != lfShow.follow_rect.bottom)
    {
        lfShow.follow_rect = r;

        const UINT bw = lfShow.w, bh = lfShow.h;
        const bool buffer_matches =
            bw == (UINT)(r.right - r.left) && bh == (UINT)(r.bottom - r.top);
        int left = r.left, top = r.top;
        UINT w = r.right - r.left, hgt = r.bottom - r.top;
        if (!buffer_matches)
        {
            w = bw;
            hgt = bh;
            if (left + (int)w > r.right) left = r.right - (int)w;
            if (top + (int)hgt > r.bottom) top = r.bottom - (int)hgt;
            if (left < r.left) left = r.left;
            if (top < r.top) top = r.top;
        }

        const bool picture_on_top = GetTopWindow(nullptr) == lfShow.hwnd;
        SetWindowPos(lfShow.hwnd,
                     picture_on_top ? nullptr : HWND_TOPMOST,
                     left, top, w, hgt,
                     SWP_NOACTIVATE | (picture_on_top ? SWP_NOZORDER : 0));
    }
}
