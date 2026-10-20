// lf_vrr.inl - VRR for the output window. One mechanism for every screen with a variable refresh rate -
// G-SYNC, G-SYNC Compatible, FreeSync, HDMI 2.1 VRR.
//
// Round 23, from measurements on a real PC (scratchpad vrr_probe*): the output
// window reaches the screen through a hardware overlay plane, never through
// the desktop compositor, whatever its style. So a tearing present (round 22:
// DXGI_PRESENT_ALLOW_TEARING) really could tear there whenever the screen was
// not in its variable mode - tear lines drifting up the picture at 188 FPS on
// a 200 Hz screen. And on that PC no windowed program got a variable refresh
// at all, a plain focused full-screen window included: the driver was not
// applying VRR to windows.
//
// So now:
//   - VRR presents with sync interval 1 and never with tearing. On a screen
//     in VRR mode that is the standard "G-SYNC + V-SYNC": each frame is shown
//     when it is ready, inside the range. On one that is not, it is ordinary
//     V-SYNC - never a tear line. The chain itself does not change, so the
//     switch no longer rebuilds the window.
//   - What is shown stays a little under the screen's top rate
//     (refresh - refresh^2/3600: 138 on 144 Hz, 188 on 200, 224 on 240); a
//     lower frame limit wins.
//   - It checks itself. The frame statistics say when each present reached
//     the screen: with VRR the gaps follow our frames, on a fixed refresh they
//     land on whole refresh periods. After a few seconds of frames that do not
//     fall on the refresh grid themselves it says which ("[vrr] the screen
//     follows the frames" / "[vrr] the screen stays at its fixed ..."), and
//     the app tells the person what to switch on when it is the second.

static bool VrrWanted()
{
    return smOpt.vrr.load() != 0;
}

// Is Windows' dynamic refresh rate at work on this screen, and if so the most the screen offers at its size (the
// rate that was picked; DRR runs it down to 60 when little moves). 0: it is not. device: the screen's GDI name.
// (Measured, tests\hzprobe.exe, a 200 Hz screen with DRR on: the desktop's mode said 60, and so did the display
// configuration - its signal given as 180/3; only the modes the screen offers said 200. Without DRR the signal
// is a whole number of the desktop's own rate, x/1, and the desktop's mode is what was picked.)
static int SignalRefresh(const wchar_t *device)
{
    UINT32 paths = 0, modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &paths, &modes) != ERROR_SUCCESS || paths == 0) return 0;
    std::vector<DISPLAYCONFIG_PATH_INFO> path(paths);
    std::vector<DISPLAYCONFIG_MODE_INFO> mode(modes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &paths, path.data(), &modes, mode.data(), nullptr) != ERROR_SUCCESS) return 0;
    for (UINT32 i = 0; i < paths; ++i)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path[i].sourceInfo.adapterId;
        source.header.id = path[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS || _wcsicmp(source.viewGdiDeviceName, device) != 0) continue;
        DISPLAYCONFIG_RATIONAL rate = path[i].targetInfo.refreshRate;
        const UINT32 at = path[i].targetInfo.modeInfoIdx;
        if (at != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && at < modes && mode[at].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET &&
            mode[at].targetMode.targetVideoSignalInfo.vSyncFreq.Denominator != 0)
            rate = mode[at].targetMode.targetVideoSignalInfo.vSyncFreq;
        // A rate of two whole numbers (180/3) where a plain one would do: the dynamic refresh rate's.
        const bool dynamic = rate.Denominator > 1 && rate.Numerator % rate.Denominator == 0;
        if (!dynamic) return 0;
        DEVMODEW current = {}; current.dmSize = sizeof(current);
        if (!EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, &current)) return 0;
        // Round 34: going through every mode the screen offers takes 30 ms (681 of them on the user's screen) -
        // and this was done once a second, on the thread that presents: the picture stood for a moment, over and
        // over ("a stutter every few seconds" with VRR). It is done once for a screen and a size, and kept.
        static wchar_t known_device[CCHDEVICENAME] = {};
        static DWORD known_w = 0, known_h = 0, known_most = 0;
        if (known_most != 0 && known_w == current.dmPelsWidth && known_h == current.dmPelsHeight && wcscmp(known_device, device) == 0)
            return static_cast<int>(known_most);
        DWORD most = 0;
        DEVMODEW each = {}; each.dmSize = sizeof(each);
        for (DWORD m = 0; EnumDisplaySettingsW(device, m, &each); ++m)
            if (each.dmPelsWidth == current.dmPelsWidth && each.dmPelsHeight == current.dmPelsHeight && each.dmDisplayFrequency > most)
                most = each.dmDisplayFrequency;
        wcsncpy_s(known_device, device, _TRUNCATE);
        known_w = current.dmPelsWidth; known_h = current.dmPelsHeight; known_most = most;
        return static_cast<int>(most);
    }
    return 0;
}

// The refresh of the screen the output window is on, asked at most once a
// second (the window can be dragged, or the mode changed).
//
// Round 32: the rate the signal to the screen runs at, when that is higher than what the desktop's mode says.
// With Windows' "Dynamic refresh rate" on, the desktop's mode says 60 Hz whenever little moves on it while the
// screen runs at its 200: everything paced by the refresh took 60 for the screen's rate - VRR's cap among them
// (59), and a user saw the screen's rate climb with the game to 58 and stay there.
static double OutputRefresh()
{
    static std::atomic<long long> asked{0};
    static std::atomic<int> hz{60};
    const long long now = static_cast<long long>(GetTickCount64());
    long long last = asked.load();
    // One thread asks (it is called from the presenter, the loop and the pacing at once).
    if (now - last >= 1000 && asked.compare_exchange_strong(last, now))
    {
        const HWND at = lfShow.hwnd;
        MONITORINFOEXW monitor = {}; monitor.cbSize = sizeof(monitor);
        DEVMODEW mode = {}; mode.dmSize = sizeof(mode);
        if (at != nullptr && GetMonitorInfoW(MonitorFromWindow(at, MONITOR_DEFAULTTONEAREST), &monitor) &&
            EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
        {
            const int desktop = static_cast<int>(mode.dmDisplayFrequency), signal = SignalRefresh(monitor.szDevice);
            const int rate = std::max(desktop, signal >= 24 && signal <= 1000 ? signal : 0);
            static int told_desktop = 0, told_signal = 0;
            if (signal > desktop && (desktop != told_desktop || signal != told_signal))
            {
                told_desktop = desktop; told_signal = signal;
                Trace("[screen] the desktop's mode says %d Hz (Windows' dynamic refresh rate is on), the screen goes up to %d Hz - %d Hz is the screen's rate",
                      desktop, signal, signal);
            }
            hz = rate;
        }
    }
    return static_cast<double>(hz.load());
}

// "--vrr" (round 34): is VRR on for a window on this PC - the app's VRR switch follows the answer at its start.
//     @vrr-system <1: on, 0: off, -1: cannot tell> <why>
// The screen the game is on is taken to be the main one.
//   - Driven by an NVIDIA card: what its driver is set to (read only, never changed). Our output is a window,
//     and G-SYNC reaches a window only when it is on "for windowed and full screen mode"; "full screen mode only"
//     or G-SYNC off: off.
//   - Driven by an AMD card (round 36): FreeSync on or off for that screen, as AMD's library says (AmdVrrSetting).
//   - Any card when the above cannot tell (Intel's, or no answer): Windows' dynamic refresh rate at work on that
//     screen says the screen and its driver run VRR (it is built on it): on. Otherwise nothing can be told: the
//     switch is left as the person set it.
static int NvVrrSetting(const char **why)
{
    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    if (nvapi == nullptr) { *why = "no-nvapi"; return -1; }
    typedef void *(__cdecl *QueryFn)(unsigned);
    auto query = reinterpret_cast<QueryFn>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (query == nullptr) { *why = "no-nvapi"; return -1; }
    typedef int (__cdecl *InitFn)();
    typedef int (__cdecl *SessionFn)(void **);
    typedef int (__cdecl *SessionOnlyFn)(void *);
    typedef int (__cdecl *ProfileFn)(void *, void **);
    typedef int (__cdecl *SettingFn)(void *, void *, unsigned, void *);
    auto init = reinterpret_cast<InitFn>(query(0x0150E828));
    auto create = reinterpret_cast<SessionFn>(query(0x0694D52E));
    auto destroy = reinterpret_cast<SessionOnlyFn>(query(0xDAD9CFF8));
    auto load = reinterpret_cast<SessionOnlyFn>(query(0x375DBD6B));
    auto global = reinterpret_cast<ProfileFn>(query(0x617BFF9F));
    auto get = reinterpret_cast<SettingFn>(query(0x73BF8338));
    if (!init || !create || !destroy || !load || !global || !get || init() != 0) { *why = "no-nvapi"; return -1; }
    void *session = nullptr, *profile = nullptr;
    if (create(&session) != 0 || session == nullptr) { *why = "no-nvapi"; return -1; }
    // NVDRS_SETTING (version 1): 12320 bytes; the current value as a number at 8220.
    struct Setting { unsigned char bytes[12320]; };
    auto value = [&](unsigned id, unsigned fallback) {
        static Setting s;
        memset(&s, 0, sizeof(s));
        const unsigned version = static_cast<unsigned>(sizeof(s)) | (1u << 16);
        memcpy(s.bytes, &version, 4);
        if (get(session, profile, id, &s) != 0) return fallback;     // not set anywhere: the driver's default
        unsigned v = 0;
        memcpy(&v, s.bytes + 8220, 4);
        return v;
    };
    int answer = -1;
    if (load(session) == 0 && global(session, &profile) == 0 && profile != nullptr)
    {
        const unsigned feature = value(0x1094F157, 1);      // "G-SYNC" itself (VRRFEATUREINDICATOR, default on)
        const unsigned mode = value(0x1194F158, 1);         // VRR_MODE: 0 off, 1 full screen only (default), 2 and windowed
        static char said[64];
        if (feature == 0 || mode == 0) { answer = 0; sprintf_s(said, "gsync-off(%u,%u)", feature, mode); }
        else if (mode == 1) { answer = 0; sprintf_s(said, "gsync-fullscreen-only(%u,%u)", feature, mode); }
        else if (mode == 2) { answer = 1; sprintf_s(said, "gsync-windowed(%u,%u)", feature, mode); }
        else sprintf_s(said, "gsync-unknown(%u,%u)", feature, mode);
        *why = said;
    }
    else *why = "no-nvapi";
    destroy(session);
    return answer;
}

// An AMD card (round 36): FreeSync on or off for the screen, as AMD's own library says it (atiadlxx.dll, ADL -
// read only). Untested here (no AMD card on the machine it was written on): any failure is -1, "cannot tell".
static int AmdVrrSetting(const wchar_t *device, const char **why)
{
    *why = "no-adl";
    HMODULE adl = LoadLibraryW(L"atiadlxx.dll");
    if (!adl) return -1;
    typedef void *(__stdcall *Alloc)(int);
    typedef int (*CreateFn)(Alloc, int, void **);
    typedef int (*DestroyFn)(void *);
    typedef int (*CountFn)(void *, int *);
    typedef int (*InfoFn)(void *, void *, int);
    typedef int (*DisplaysFn)(void *, int, int *, void **, int);
    typedef int (*FreeSyncFn)(void *, int, int, int *, int *, int *, int *);
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(adl, "ADL2_Main_Control_Create"));
    auto destroy = reinterpret_cast<DestroyFn>(GetProcAddress(adl, "ADL2_Main_Control_Destroy"));
    auto count = reinterpret_cast<CountFn>(GetProcAddress(adl, "ADL2_Adapter_NumberOfAdapters_Get"));
    auto info = reinterpret_cast<InfoFn>(GetProcAddress(adl, "ADL2_Adapter_AdapterInfo_Get"));
    auto displays = reinterpret_cast<DisplaysFn>(GetProcAddress(adl, "ADL2_Display_DisplayInfo_Get"));
    auto freesync = reinterpret_cast<FreeSyncFn>(GetProcAddress(adl, "ADL2_Display_FreeSyncState_Get"));
    if (!create || !destroy || !count || !info || !displays || !freesync) return -1;
    void *context = nullptr;
    static const Alloc alloc = [](int size) -> void * { return malloc(static_cast<size_t>(size)); };
    if (create(alloc, 1, &context) != 0 || !context) return -1;
    int answer = -1, adapters = 0;
    // AdapterInfo (adl_structures.h): 1572 bytes; its index at 4, its display name ("\\.\DISPLAY1") at 284.
    // ADLDisplayInfo: 552 bytes; the display's logical index at 0, its "connected / mapped" bits at 548.
    if (count(context, &adapters) == 0 && adapters > 0 && adapters < 64)
    {
        std::vector<unsigned char> all(size_t(adapters) * 1572);
        for (int i = 0; i < adapters; ++i) { const int size = 1572; memcpy(all.data() + size_t(i) * 1572, &size, 4); }
        if (info(context, all.data(), static_cast<int>(all.size())) == 0)
            for (int i = 0; i < adapters && answer == -1; ++i)
            {
                const unsigned char *a = all.data() + size_t(i) * 1572;
                int index = 0;
                memcpy(&index, a + 4, 4);
                wchar_t name[256] = {};
                MultiByteToWideChar(CP_ACP, 0, reinterpret_cast<const char *>(a + 284), 255, name, 256);
                if (_wcsicmp(name, device) != 0) continue;
                int shown = 0;
                void *list = nullptr;
                if (displays(context, index, &shown, &list, 0) != 0 || !list) continue;
                for (int d = 0; d < shown && answer == -1; ++d)
                {
                    const unsigned char *one = static_cast<const unsigned char *>(list) + size_t(d) * 552;
                    int logical = 0, value = 0;
                    memcpy(&logical, one, 4); memcpy(&value, one + 548, 4);
                    if ((value & 3) != 3) continue;         // connected and mapped
                    int current = 0, initial = 0, least = 0, most = 0;
                    if (freesync(context, index, logical, &current, &initial, &least, &most) == 0)
                    {
                        answer = (current & 1) ? 1 : 0;
                        *why = answer ? "freesync-on" : "freesync-off";
                    }
                }
                free(list);
            }
    }
    destroy(context);
    return answer;
}

static void VrrTell()
{
    const char *why = "unknown";
    int answer = -1;
    MONITORINFOEXW monitor = {}; monitor.cbSize = sizeof(monitor);
    const POINT origin = { 0, 0 };
    if (GetMonitorInfoW(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), &monitor))
    {
        // The card the main screen hangs on.
        UINT vendor = 0;
        IDXGIFactory1 *factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))) && factory)
        {
            IDXGIAdapter1 *card = nullptr;
            for (UINT i = 0; vendor == 0 && factory->EnumAdapters1(i, &card) != DXGI_ERROR_NOT_FOUND; ++i)
            {
                IDXGIOutput *output = nullptr;
                for (UINT o = 0; vendor == 0 && card->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o)
                {
                    DXGI_OUTPUT_DESC od = {};
                    if (SUCCEEDED(output->GetDesc(&od)) && _wcsicmp(od.DeviceName, monitor.szDevice) == 0)
                    {
                        DXGI_ADAPTER_DESC1 desc = {};
                        card->GetDesc1(&desc);
                        vendor = desc.VendorId;
                    }
                    output->Release();
                }
                card->Release();
            }
            factory->Release();
        }
        if (vendor == 0x10DE) answer = NvVrrSetting(&why);
        else if (vendor == 0x1002) answer = AmdVrrSetting(monitor.szDevice, &why);
        if (answer == -1 && SignalRefresh(monitor.szDevice) > 0) { answer = 1; why = "windows-dynamic-refresh"; }
    }
    Status("@vrr-system %d %s", answer, why);
}

// The highest rate that stays inside the VRR range.
static double VrrCeiling(double refresh)
{
    return std::max(30.0, refresh - refresh * refresh / 3600.0);
}

// The cap VRR puts on shown frames, 0 while VRR is off.
// Round 45: only with "Limit to refresh rate" on. A user's x6 on a 50 FPS game with VRR on came to 184 frames a
// second on a 200 Hz screen - VRR held what was shown inside its range, whatever the multiplier said, where the
// option's own word is "as the multiplier says, whatever the screen shows" (and Lossless Scaling shows 300). Frames
// above the range are presented without tearing (ShowPresentNow): the screen shows the newest at each refresh there.
static int VrrCap()
{
    if (!lfShow.vrr || !smOpt.screen_limit.load()) return 0;
    return static_cast<int>(VrrCeiling(OutputRefresh()));
}

// Sync mode ("Sync mode" in the app, the option sync): on which refreshes of
// the screen a frame may be shown.
//   0  off      the moment it is presented, in the middle of a refresh too:
//               least delay, and a tear line where one frame ends and the
//               next begins (the swap chain is made for it: ShowStart)
//   1  default  the newest frame at each refresh; none waits, one too many is
//               never shown - what the engine did before there was a choice
//   2  VSync    every frame presented is shown, for one whole refresh
//   3, 4        ... for two, for three: a screen of half, of a third its rate
// VRR is a sync mode of its own (above): while it is on, this one rests.
static int SyncEvery()
{
    const int mode = smOpt.sync.load();
    return (lfShow.vrr || mode < 2 || mode > 4) ? 0 : mode - 1;
}
// Round 45: the frames in between of the pairs just now are more than the screen's refreshes (FgPresent, lf_dlssg.inl).
static std::atomic<bool> g_present_above{false};

static bool TearingWanted()
{
    return smOpt.sync.load() == 0;
}
// Round 45: the swap chain is made so that it may tear - sync mode off, VRR, or (sync mode default, frames in between,
// "Limit to refresh rate" off) for frames above the screen's rate (ShowPresentNow).
static bool ChainMayTear()
{
    return TearingWanted() || VrrWanted() || (smOpt.sync.load() == 1 && smOpt.mfg.load() != 0 && !smOpt.screen_limit.load());
}

// The rate the screen takes frames at.
static double ShownRefresh()
{
    return OutputRefresh() / std::max(1, SyncEvery());
}

// The cap VSync puts on shown frames, 0 without it: a little under one a
// turn - at exactly one there is no slack at all, every hiccup is a frame
// that waits a whole turn, and the ones behind it with it.
static int SyncCap()
{
    return SyncEvery() > 0 ? std::max(20, static_cast<int>(ShownRefresh() * 0.97)) : 0;
}

// The cap the way of presenting puts on shown frames: VRR's range or VSync's
// turns, 0 with neither.
static int PresentCap()
{
    return lfShow.vrr ? VrrCap() : SyncCap();
}

// Does the screen follow our frames? Called after every present while VRR is
// on (one thread presents at a time; the lock only makes sure of it).
static void VrrObserve()
{
    static std::mutex lock;
    static IDXGISwapChain3 *chain = nullptr;
    static UINT last_count = 0;
    static LONGLONG last_sync = 0, last_call = 0;
    static unsigned shown = 0, shown_off = 0, sent = 0, sent_off = 0;
    static int said = -1;            // -1 nothing yet, 0 fixed, 1 follows
    std::lock_guard<std::mutex> hold(lock);
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (chain != lfShow.chain) { chain = lfShow.chain; last_count = 0; last_call = 0; shown = shown_off = sent = sent_off = 0; }
    if (chain == nullptr) return;
    const double period = 1000.0 / OutputRefresh();
    // Gaps past this are pauses (a still picture, a menu), not the pacing.
    const double longest = std::max(3.0 * period, 50.0);
    auto off_grid = [period](double gap_ms) {
        const double steps = std::round(gap_ms / period);
        return steps >= 1.0 && std::fabs(gap_ms - steps * period) > 0.2;
    };
    // Our own presents: do they fall on the refresh grid anyway (a frame rate
    // that divides the refresh tells nothing)?
    if (last_call != 0)
    {
        const double gap = double(now.QuadPart - last_call) * 1000.0 / double(freq.QuadPart);
        if (gap < longest) { ++sent; if (off_grid(gap)) ++sent_off; }
    }
    last_call = now.QuadPart;
    DXGI_FRAME_STATISTICS st = {};
    if (SUCCEEDED(chain->GetFrameStatistics(&st)) && st.PresentCount != last_count)
    {
        if (last_count != 0 && st.PresentCount == last_count + 1)
        {
            const double gap = double(st.SyncQPCTime.QuadPart - last_sync) * 1000.0 / double(freq.QuadPart);
            if (gap > 0.0 && gap < longest) { ++shown; if (off_grid(gap)) ++shown_off; }
        }
        last_count = st.PresentCount;
        last_sync = st.SyncQPCTime.QuadPart;
    }
    if (shown < 240) return;
    // Only frames that are off the grid themselves can show the difference.
    if (sent >= 120 && sent_off * 10 >= sent * 3)
    {
        const int follows = shown_off * 10 >= shown * 3 ? 1 : (shown_off * 20 <= shown ? 0 : -1);
        if (follows != -1 && follows != said)
        {
            said = follows;
            if (follows)
                Trace("[vrr] the screen follows the frames - VRR is working (%u of %u frames off the %.0f Hz grid)",
                    shown_off, shown, 1000.0 / period);
            else
                Trace("[vrr] the screen stays at its fixed %.0f Hz - VRR is not reaching it "
                      "(NVIDIA: G-SYNC for windowed and full screen mode; AMD: FreeSync in AMD Software; "
                      "and Adaptive-Sync on the monitor)", 1000.0 / period);
        }
    }
    shown = shown_off = sent = sent_off = 0;
}

// Present the output window's frame: VSync's own with VSync, 0 otherwise (the newest frame at each refresh - or,
// sync mode off, at once). source: when the shown picture's source reached the desktop (lf_latency.inl); -1 is the
// loop's own capture, 0 a frame without one.
//
// VRR (round 31). Presented with sync interval 1, as rounds 23 to 30 did, a window asks the driver for nothing:
// NVIDIA's own G-SYNC indicator said "VRR Requested: No" on a user's PC with G-SYNC on for windowed programs, and
// every frame waited for a fixed refresh. What Windows asks of a program in a window that wants the screen to
// follow its frames is a chain that may tear, presented with sync interval 0 and "allow tearing" - so now that is
// how VRR presents. What is shown stays under the screen's top rate (VrrCap): inside the range there is nothing
// to tear. (A screen with VRR switched off in the driver shows such frames the moment they come - tear lines: the
// app's VRR switch is then to be off.)
static HRESULT ShowPresentNow(double source)
{
    const bool vrr_asks = lfShow.vrr && lfShow.tearing;
    const UINT every = vrr_asks ? 0u : lfShow.vrr ? 1u : static_cast<UINT>(SyncEvery());
    UINT how = every == 0 && lfShow.tearing && (TearingWanted() || vrr_asks) ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    // Round 45: more frames than the screen's refreshes ("Limit to refresh rate" off - the multiplier's own count; a
    // user's x6 on a 50 FPS game, 300 a second on a 200 Hz screen). Presented to wait for the screen, they filled the
    // swap chain's queue and the presenter stood behind it 15 to 30 ms at a time, then sent what was late at once:
    // frames.log, 34 stutters a second where x4 had 4. A frame that comes sooner than one refresh after the one before
    // is presented the moment it is due, as with sync mode off - every one of them shown, a tear line where it begins
    // (what Lossless Scaling does). "Limit to refresh rate" keeps them to the screen's rate instead, without tearing.
    // Whether the frames are more than the screen's refreshes is the pairs' business (FgPresent: g_present_above) - all
    // of a run of pairs one way or the other: frame by frame, at exactly the screen's rate (x4 of 50 on 200 Hz) every
    // other frame tore, and the change between the two cost more stutters than either.
    if (every == 0 && lfShow.tearing && !smOpt.screen_limit.load() && FgRequested() && g_present_above.load()) how = DXGI_PRESENT_ALLOW_TEARING;
    // Two cards: the frame crosses to the screen's card first (lf_show_relay.inl).
    if (lfRelay.on && !RelayBeforePresent()) return E_FAIL;
    const HRESULT hr = lfShow.chain->Present(every, how);
    if (lfRelay.on) RelayAfterPresent();
    if (lfShow.vrr && SUCCEEDED(hr)) VrrObserve();
    if (SUCCEEDED(hr)) LatencyPresented(source < 0.0 ? lfCap.stamp : source);
    if (SUCCEEDED(hr)) CounterPresented(source, source < 0.0);
    return hr;
}
