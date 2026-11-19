// lf_desktop.inl - where the picture comes from. Three sources, one bridge:
// Desktop Duplication (DXGI) for a screen on the generator's card, Windows
// Graphics Capture (WGC) for a single window, and Graphics Capture of a whole
// screen when that screen hangs on another card or WGC is picked in the app. Every source hands a D3D11 texture to
// BridgeFrame, which moves it to D3D12 through a shared texture and a shared
// fence; SwizzleIntoPicture then puts it in RGBA order in the pipeline.

// Is any source (desktop, window or screen) running.
// Every frame a capture hands over, before anything decides it is not to be used (a repeat, not the game's,
// let go for the pace): the watch in sm_loop.inl sets it against what was used.
static std::atomic<unsigned> g_cap_arrivals{0};

// Crop input (round 43): the sides the person leaves out of a captured window w x h - none when they would leave
// under 64 pixels either way. The picture's size (lfCap.w, lfCap.h) is the window's less them, from the moment the
// window is taken (WindowStart, WindowDxgiStart); BridgeFrame copies that part; the output is put over it (WindowBounds).
static RECT CropMargins(LONG w, LONG h)
{
    const RECT m = { smOpt.crop_l.load(), smOpt.crop_t.load(), smOpt.crop_r.load(), smOpt.crop_b.load() };
    if ((m.left | m.top | m.right | m.bottom) == 0 || m.left + m.right + 64 > w || m.top + m.bottom + 64 > h) return RECT{};
    return m;
}

static bool SourceLive()
{
    return lfCap.live || lfCap.window_live;
}

// The D3D11 device sources deliver on, made on the generator's card.
static bool CaptureDeviceReady()
{
    if (lfCap.d11 != nullptr) return true;
    IDXGIFactory1 *dxgi_factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&dxgi_factory)))
    { Trace("[source] no DXGI factory"); return false; }

    const int wanted = CardInUse();
    IDXGIAdapter1 *card = nullptr;
    if (FAILED(dxgi_factory->EnumAdapters1(wanted >= 0 ? (UINT)wanted : 0, &card)))
    { Trace("[source] card %d is not there", wanted >= 0 ? wanted : 0); dxgi_factory->Release(); return false; }
    if (wanted >= 0) Trace("[source] capturing on card %d, next to the generator", wanted);
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    const HRESULT status = D3D11CreateDevice(card, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                         D3D11_SDK_VERSION, &lfCap.d11, nullptr,
                                         &lfCap.d11_ctx);
    card->Release(); dxgi_factory->Release();
    if (FAILED(status)) { Trace("[source] the D3D11 capture device did not start (0x%08X)", status); return false; }
    return true;
}

// The screen called like DXGI's device name (\\.\DISPLAYn), or null.
static HMONITOR MonitorByName(const wchar_t *name)
{
    struct Search { const wchar_t *name; HMONITOR found; } search = { name, nullptr };
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        auto *s = reinterpret_cast<Search *>(data);
        MONITORINFOEXW info = {};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info) && wcscmp(info.szDevice, s->name) == 0)
        { s->found = monitor; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

// The output of whichever card shows the wanted screen, and that card. On a
// laptop the screen usually hangs on the processor's graphics while the
// generator's card drives none at all: Desktop Duplication can only be opened
// on the card that drives the screen, so the source is taken there and its
// frames carried over to the generator's card (ScreenCross, BridgeFrame).
static HMONITOR g_capture_target_monitor = nullptr;
static HMONITOR g_desk_monitor = nullptr;

// 1.1.0 (GitHub 87, the community's Windows 10 fix - merged here instead of patching this file's bytes): before build
// 20348 Graphics Capture cannot leave out its yellow border (IsBorderRequired is not there), and its frames come on a
// 40 / 60 ms beat of its own. There a window that is not its whole screen - windowed, or scaled onto it - is taken by
// DXGI cut down to the window's client area instead. Only from build 19041 on (Windows 10 2004): before it the output
// window cannot be left out of the screen's copy (WDA_EXCLUDEFROMCAPTURE), and the engine would take its own frames.
// Windows 11 keeps Graphics Capture for such a window (round 31: it hands over the window's own frames only).
// SM_WIN10_CROP=1 / 0: as if on such a Windows / not (tests).
static DWORD WindowsBuild()
{
    static const DWORD build = [] {
        typedef LONG (WINAPI *RtlGetVersionFn)(OSVERSIONINFOW *);
        OSVERSIONINFOW v = {};
        v.dwOSVersionInfoSize = sizeof(v);
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto get = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
        return get && get(&v) == 0 ? v.dwBuildNumber : DWORD(0);
    }();
    return build;
}
static bool Win10Crop()
{
    static const bool crop = [] {
        char forced[4] = {};
        if (GetEnvironmentVariableA("SM_WIN10_CROP", forced, sizeof(forced)) > 0) return forced[0] == '1';
        const DWORD build = WindowsBuild();
        return build >= 19041 && build < 20348;
    }();
    static bool told = false;
    if (crop && !told)
    {
        told = true;
        Trace("[capture] Windows build %lu cannot leave out Graphics Capture's yellow border - DXGI is used for every "
              "source here, whatever the app's Capture API says", WindowsBuild());
    }
    return crop;
}

// Is a whole screen taken with Graphics Capture (the app's "WGC" choice)?
// Only where Windows can leave its border out: the yellow frame around the game
// is the one thing about a source nobody asks for. On Windows 10 it cannot be
// left out at all, so there DXGI is what every screen source gets, whatever the
// app's Capture API says (and a window that is not its whole screen too -
// WindowStart, Win10Crop).
static bool WgcForScreen()
{
    return smOpt.capture_api.load() == 1 && !Win10Crop();
}

// The device every captured frame is copied on: the card the screen hangs on
// (cap11, ScreenCaptureOpen). A source from Graphics Capture hands its
// frames over on the generator's own device instead (lfCap.d11).
static ID3D11Device *CopyDevice() { return lfCap.cap11 != nullptr ? lfCap.cap11 : lfCap.d11; }
static ID3D11DeviceContext *CopyContext() { return lfCap.cap11_ctx != nullptr ? lfCap.cap11_ctx : lfCap.d11_ctx; }

// Open the screen's duplication on whichever card can give it. A hybrid
// laptop's discrete card often lists the screen too and refuses to duplicate it
// (the processor's graphics is what scans it out), so every card that lists the
// screen is tried in turn and the first that hands frames over is kept. When
// that card is not the generator's, its frames cross in BridgeFrame.
static bool ScreenCaptureOpen(HMONITOR target)
{
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory)) || factory == nullptr)
    { Trace("[desk] no DXGI factory"); return false; }
    const LUID ours = lfCore.device->GetAdapterLuid();
    const bool hdr = lfCap.display.hdr_on;
    bool opened = false;
    for (UINT i = 0; !opened; ++i)
    {
        IDXGIAdapter1 *card = nullptr;
        if (factory->EnumAdapters1(i, &card) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 card_desc = {};
        card->GetDesc1(&card_desc);
        const bool same = card_desc.AdapterLuid.LowPart == ours.LowPart &&
                          card_desc.AdapterLuid.HighPart == ours.HighPart;
        for (UINT o = 0; !opened; ++o)
        {
            IDXGIOutput *output = nullptr;
            if (FAILED(card->EnumOutputs(o, &output)) || output == nullptr) break;
            DXGI_OUTPUT_DESC desc = {};
            if (FAILED(output->GetDesc(&desc)) || (target != nullptr && desc.Monitor != target))
            { output->Release(); continue; }

            ID3D11Device *device = nullptr;
            ID3D11DeviceContext *context = nullptr;
            if (same)
            {
                device = lfCap.d11; device->AddRef();
                context = lfCap.d11_ctx; context->AddRef();
            }
            else
            {
                D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
                const HRESULT made = D3D11CreateDevice(card, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1, D3D11_SDK_VERSION,
                                                       &device, nullptr, &context);
                if (FAILED(made) || device == nullptr || context == nullptr)
                {
                    Trace("[desk] %ls gave no Direct3D 11 device (0x%08X) - trying the next card",
                          card_desc.Description, (unsigned)made);
                    if (context) context->Release();
                    if (device) device->Release();
                    output->Release();
                    continue;
                }
            }

            IDXGIOutput1 *output1 = nullptr;
            IDXGIOutput5 *output5 = nullptr;
            output->QueryInterface(__uuidof(IDXGIOutput1), (void **)&output1);
            if (hdr) output->QueryInterface(__uuidof(IDXGIOutput5), (void **)&output5);
            HRESULT status = E_FAIL;
            if (hdr && output5 != nullptr)
            {
                const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM};
                status = output5->DuplicateOutput1(device, 0, _countof(formats), formats, &lfCap.duplication);
                if (FAILED(status))
                    Trace("[hdr] half-float duplication refused on %ls (0x%08X) - taking the screen in SDR",
                          card_desc.Description, (unsigned)status);
            }
            else if (hdr)
                Trace("[hdr] IDXGIOutput5 is missing on this Windows - taking the screen in SDR");
            if (output1 != nullptr && (FAILED(status) || lfCap.duplication == nullptr))
                status = output1->DuplicateOutput(device, &lfCap.duplication);

            if (SUCCEEDED(status) && lfCap.duplication != nullptr)
            {
                IDXGIOutput6 *out6 = nullptr;
                if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), (void **)&out6)) && out6 != nullptr)
                {
                    DXGI_OUTPUT_DESC1 d1 = {};
                    if (SUCCEEDED(out6->GetDesc1(&d1)))
                    {
                        const bool hdr_cs =
                            d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                            d1.ColorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020 ||
                            d1.ColorSpace == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020 ||
                            d1.ColorSpace == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020;

                        Trace("[desk] screen colour space %d at %u bits%s",
                            (int)d1.ColorSpace, d1.BitsPerColor,
                            hdr_cs ? " - this screen is in HDR" : "");
                    }
                    out6->Release();
                }
                if (output5) output5->Release();
                if (output1) output1->Release();
                output->Release();

                lfCap.cap11 = device; lfCap.cap11_ctx = context;
                lfCap.cross = !same;
                lfCap.monitor = desc.Monitor;
                lfCap.opened_hdr = hdr;
                if (!same)
                    Trace("[desk] the screen hangs on %ls, not on the generator's card: its frames are taken there and carried over",
                          card_desc.Description);
                else
                    Trace("[desk] the screen is on the generator's card");
                opened = true;
            }
            else
            {
                if (output5) output5->Release();
                if (output1) output1->Release();
                output->Release();
                Trace("[desk] %ls lists this screen but cannot duplicate it (0x%08X) - trying the next card",
                      card_desc.Description, (unsigned)status);
                lfCap.duplication = nullptr;
                context->Release();
                device->Release();
            }
        }
        card->Release();
    }
    factory->Release();
    return opened;
}

// Let a screen-card device go (Graphics Capture takes its frames on the
// generator's own instead).
static void ScreenCaptureRelease()
{
    if (lfCap.cap11_ctx) { lfCap.cap11_ctx->Release(); lfCap.cap11_ctx = nullptr; }
    if (lfCap.cap11) { lfCap.cap11->Release(); lfCap.cap11 = nullptr; }
    lfCap.cross = false;
}

static void ScreenCrossStop();

// Start taking the desktop at w x hgt: duplication of the picked screen on the
// card that drives it, in half float when that screen is in HDR. When that card
// is not the generator's, every frame crosses to it in BridgeFrame.
static bool DeskStart(UINT w, UINT hgt)
{
    WindowStop();
    DeskStop();
    lfCap.window = nullptr;
    if (w == 0 || hgt == 0) { Trace("[desk] desktop source stopped"); return true; }
    if (!SwizzleReady()) return false;
    if (!CaptureDeviceReady()) return false;

    const HMONITOR target = g_capture_target_monitor ? g_capture_target_monitor
                            : g_desk_monitor ? g_desk_monitor
                            : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    lfCap.display = HdrDisplayOf(target);
    // (WGC for a whole screen: only where Windows can leave its border out.)
    if (!g_capture_target_monitor && WgcForScreen())
    {
        Trace("[desk] WGC picked: the screen goes through Windows Graphics Capture");
        return ScreenViaWgcStart(target);
    }
    if (!ScreenCaptureOpen(target))
    {
        // No card here can duplicate the screen. Graphics Capture is the only way
        // left: a picture with Windows' border around it is better than none.
        Trace("[desk] no card here can duplicate this screen - taking it through Windows Graphics Capture%s",
              Win10Crop() ? " (on Windows 10 that keeps its yellow border)" : "");
        ScreenCaptureRelease();
        return ScreenViaWgcStart(target);
    }

    {
        DXGI_OUTDUPL_DESC dd = {};
        lfCap.duplication->GetDesc(&dd);
        static const char *kRot[] = { "unspecified", "none", "90", "180", "270" };
        const unsigned r = (unsigned)dd.Rotation;

        lfCap.upside_down = (r == DXGI_MODE_ROTATION_ROTATE180);
        const bool unhandled = (r == DXGI_MODE_ROTATION_ROTATE90 ||
                                r == DXGI_MODE_ROTATION_ROTATE270);
        Trace("[desk] screen rotation %s%s", r < 5 ? kRot[r] : "?",
            lfCap.upside_down ? " - flipped upright while copying"
            : unhandled ? " - not supported: a portrait screen swaps "
                          "width and height, the picture will be wrong"
                        : "");
    }
    lfCap.w = w; lfCap.h = hgt; lfCap.live = true;
    Trace("[desk] taking the desktop at %ux%u%s", w, hgt, lfCap.cross ? " (DXGI on the screen's card)" : "");
    return true;
}

// Stop the desktop source and free the bridge it used.
static void DeskStop()
{
    // The HDR export textures can still be used by a generation presenter.
    CloseFgResources();
    CmdDrain();
    g_convert_mark = 0;
    if (PhasesOn()) { ++lfProf.capture_gen; lfProf.last_source_qpc = 0; lfProf.stamp = {}; }
    ScreenViaWgcStop();
    lfCap.live = false;
    lfCap.has_frame = false;
    lfHdr.keep = false;
    lfHdr.float_frames = false;
    HdrRelease();
    Luma11Stop();
    ScreenCrossStop();
    if (lfCap.bridge12) { lfCap.bridge12->Release(); lfCap.bridge12 = nullptr; }
    if (lfCap.bridge_handle)  { CloseHandle(lfCap.bridge_handle); lfCap.bridge_handle = nullptr; }
    if (lfCap.bridge11) { lfCap.bridge11->Release(); lfCap.bridge11 = nullptr; }
    lfCap.bridge_cross = false;
    if (lfCap.duplication) { lfCap.duplication->Release(); lfCap.duplication = nullptr; }
    ScreenCaptureRelease();
    if (lfCap.d11_ctx) { lfCap.d11_ctx->Release(); lfCap.d11_ctx = nullptr; }
    if (lfCap.d11) { lfCap.d11->Release(); lfCap.d11 = nullptr; }
    if (lfCap.sync12) { lfCap.sync12->Release(); lfCap.sync12 = nullptr; }
    if (lfCap.sync11) { lfCap.sync11->Release(); lfCap.sync11 = nullptr; }
    if (lfCap.sync_event) { CloseHandle(lfCap.sync_event); lfCap.sync_event = nullptr; }

    if (lfCap.rgba) { lfCap.rgba->Release(); lfCap.rgba = nullptr; }
}

// Changes outside the selected client (taskbar, another window, etc.) are not
// frames of the game. What changed inside it has to cover something of it at
// all. (Whether a frame is the game's own or a corner another program redrew
// cannot be told here: Desktop Duplication calls the whole screen changed for
// both - measured; lf_own_motion.inl, OnlyACorner, tells them by the picture.)
// Without the list of what changed, or on an error reading it, the frame is
// taken.
static double g_desk_covered = -1.0;      // SM_DIAG: the share of the picture the last frame looked at changed
static bool DxgiTouchesClient(const DXGI_OUTDUPL_FRAME_INFO &info)
{
    g_desk_covered = -1.0;
    if (!info.TotalMetadataBufferSize) return true;
    RECT client = lfCap.window_crop;
    if (!lfCap.window_dxgi) client = RECT{0, 0, static_cast<LONG>(lfCap.w), static_cast<LONG>(lfCap.h)};
    const double whole = double(client.right - client.left) * double(client.bottom - client.top);
    if (whole <= 0.0) return true;
    double covered = 0.0;
    auto cover = [&](const RECT &r) {
        RECT overlap = {};
        if (IntersectRect(&overlap, &r, &client))
            covered += double(overlap.right - overlap.left) * double(overlap.bottom - overlap.top);
    };
    std::vector<RECT> dirty((info.TotalMetadataBufferSize + sizeof(RECT)-1)/sizeof(RECT));
    UINT bytes = 0;
    if (FAILED(lfCap.duplication->GetFrameDirtyRects(UINT(dirty.size()*sizeof(RECT)), dirty.data(), &bytes))) return true;
    for (UINT i=0;i<bytes/sizeof(RECT);++i) cover(dirty[i]);
    std::vector<DXGI_OUTDUPL_MOVE_RECT> moves((info.TotalMetadataBufferSize + sizeof(DXGI_OUTDUPL_MOVE_RECT)-1)/sizeof(DXGI_OUTDUPL_MOVE_RECT));
    if (FAILED(lfCap.duplication->GetFrameMoveRects(UINT(moves.size()*sizeof(DXGI_OUTDUPL_MOVE_RECT)), moves.data(), &bytes))) return true;
    for (UINT i=0;i<bytes/sizeof(DXGI_OUTDUPL_MOVE_RECT);++i) cover(moves[i].DestinationRect);
    g_desk_covered = covered / whole;
    return covered > 0.0;
}

// The newest desktop picture into the pipeline; false when nothing changed.
static bool DeskTake(Pipeline &pipe)
{
    CaptureGuard guard;
    if (g_snapshot_frozen) return false;
    if (!lfCap.live) return false;
    lfCap.picture_changed = false;
    const HWND target = lfCap.window_dxgi ? lfCap.window : nullptr;
    auto reopen = [&](UINT w, UINT h) {
        return target ? WindowStart(target) : DeskStart(w, h);
    };
    static ULONGLONG last_mode_query = 0;
    if (GetTickCount64() - last_mode_query > 1000)
    {
        lfCap.display = HdrDisplayOf(lfCap.monitor);
        last_mode_query = GetTickCount64();
        if (lfCap.opened_hdr != lfCap.display.hdr_on)
        {
            Trace("[hdr] the screen switched HDR mode - reopening the source");
            reopen(lfCap.w, lfCap.h);
            return false;
        }
    }
    if (lfCap.screen_via_wgc) return ScreenViaWgcTake(pipe);
    IDXGIResource *res = nullptr;
    DXGI_OUTDUPL_FRAME_INFO fi = {};
    const double take_start = PhaseClock();
    HRESULT status = lfCap.duplication->AcquireNextFrame(100, &fi, &res);
    // SM_DIAG=1: what Desktop Duplication hands over, once a second.
    static const bool dda_diag = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_DIAG", v, sizeof(v)) > 0 && v[0] == '1'; }();
    static unsigned d_frames = 0, d_timeouts = 0, d_pointer = 0, d_untouched = 0, d_letgo = 0, d_accumulated = 0, d_nometa = 0;
    static ULONGLONG d_since = 0;
    if (dda_diag)
    {
        if (status == DXGI_ERROR_WAIT_TIMEOUT) ++d_timeouts;
        else if (SUCCEEDED(status))
        {
            ++d_frames;
            d_accumulated += fi.AccumulatedFrames;
            if (fi.LastPresentTime.QuadPart == 0) ++d_pointer;
            if (fi.TotalMetadataBufferSize == 0) ++d_nometa;
        }
        const ULONGLONG tick = GetTickCount64();
        if (tick - d_since >= 1000)
        {
            Trace("[diag] duplication: %u frames (%u accumulated), %u pointer only, %u without metadata, %u not the game's, "
                  "%u let go, %u waits ran out", d_frames, d_accumulated, d_pointer, d_nometa, d_untouched, d_letgo, d_timeouts);
            d_frames = d_timeouts = d_pointer = d_untouched = d_letgo = d_accumulated = d_nometa = 0;
            d_since = tick;
        }
    }
    // DLSS MFG under a game faster than the screen can show multiplied takes
    // every second (third) frame, by the frames' own time stamps
    // (lf_fg_pace.inl): one that is not taken is let go here and the next one
    // waited for.
    static bool let_go = false;
    for (int tries = 0; SUCCEEDED(status) && FgRequested() && fi.LastPresentTime.QuadPart != 0 && tries < 8; ++tries)
    {
        if (!DxgiTouchesClient(fi)) break;
        LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
        // With VRR what is shown stays inside the screen's range (lf_vrr.inl):
        // its cap is the room there is, not the refresh rate and a little over.
        // (VSync's the same: lf_vrr.inl, PresentCap.)
        const int vrr_cap = PresentCap();
        const double room = vrr_cap > 0 ? vrr_cap / 1.04 : OutputRefresh();
        if (FgGateTakes(double(fi.LastPresentTime.QuadPart) / frequency.QuadPart, FgExtraFrames(), room,
                        ShownCap() > 0))
        { let_go = false; break; }
        if (res) { res->Release(); res = nullptr; }
        lfCap.duplication->ReleaseFrame();
        let_go = true;
        ++d_letgo;
        status = lfCap.duplication->AcquireNextFrame(50, &fi, &res);
    }
    PhaseCount(PHASE_TAKE, take_start);
    if (SUCCEEDED(status) && fi.LastPresentTime.QuadPart != 0) ++g_cap_arrivals;
    if (status == DXGI_ERROR_WAIT_TIMEOUT)
    {
        // The game stopped right after a frame that was let go: the picture
        // on the screen would stay one frame behind. The duplication is
        // opened again - its first frame is the desktop as it is now.
        if (let_go)
        {
            let_go = false;
            FgGateForget();
            reopen(lfCap.w, lfCap.h);
        }
        return false;
    }
    if (FAILED(status))
    {
        Trace("[desk] frame wait failed (0x%08X) - reopening", status);
        reopen(lfCap.w, lfCap.h);
        return false;
    }

    lfCap.picture_changed = fi.LastPresentTime.QuadPart != 0;
    g_desk_accumulated = fi.AccumulatedFrames;
    const bool the_games = !lfCap.picture_changed || DxgiTouchesClient(fi);
    if (dda_diag && lfCap.picture_changed)
    {
        // How much of the picture the frames changed: all of it, a part, nearly none, no list.
        static unsigned whole = 0, part = 0, corner = 0, unknown = 0;
        static ULONGLONG told = 0;
        ++(g_desk_covered < 0.0 ? unknown : g_desk_covered >= 0.9 ? whole : g_desk_covered >= 0.05 ? part : corner);
        if (GetTickCount64() - told >= 1000)
        {
            Trace("[diag] frames by how much of the picture changed: %u all of it, %u a part, %u under a twentieth, %u unknown",
                  whole, part, corner, unknown);
            whole = part = corner = unknown = 0; told = GetTickCount64();
        }
    }
    if ((lfCap.window_dxgi && !lfCap.picture_changed) || !the_games) {
        if (lfCap.picture_changed) ++d_untouched;
        if (res) res->Release();
        lfCap.duplication->ReleaseFrame();
        lfCap.picture_changed = false;
        return false;
    }
    if (lfCap.picture_changed) {
        LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
        const double stamp = double(fi.LastPresentTime.QuadPart) / frequency.QuadPart;
        // The engine's own frame coming back, as the ones before it did
        // (lf_own_motion.inl): let go without a copy.
        if (FgRequested() && RepeatExpected(stamp))
        {
            if (res) res->Release();
            lfCap.duplication->ReleaseFrame();
            lfCap.picture_changed = false;
            return false;
        }
        lfCap.stamp = stamp;
    }
    TimingCapture(take_start, static_cast<UINT64>(fi.LastPresentTime.QuadPart), "dda");
    ID3D11Texture2D *frame = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&frame)))
    {
        lfCap.duplication->ReleaseFrame();
        res->Release();
        return false;
    }
    UINT new_w = 0, new_h = 0;
    DXGI_FORMAT new_format = DXGI_FORMAT_UNKNOWN;
    const BridgeResult st = BridgeFrame(frame, &new_w, &new_h, &new_format);
    frame->Release();
    res->Release();

    lfCap.duplication->ReleaseFrame();

    if (st == BridgeResult::NewFormat) return false;
    if (st == BridgeResult::NewSize)
    {
        Trace("[desk] the desktop is now %ux%u, format %u - reopening",
            new_w, new_h, (unsigned)new_format);
        reopen(new_w, new_h);
        return false;
    }
    if (st != BridgeResult::Done) return false;
    return SwizzleIntoPicture(pipe);
}

// DXGI duplicates a monitor; restrict the GPU copy to the selected client's
// rectangle. Rotated, off-screen and cross-adapter windows retain WGC.
static bool WindowClientOnMonitor(HWND hwnd, RECT &crop, HMONITOR &monitor)
{
    RECT client = {}; POINT origin = {};
    MONITORINFO mi = {sizeof(mi)};
    monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin) ||
        !GetMonitorInfoW(monitor, &mi) || client.right <= 0 || client.bottom <= 0) return false;
    crop = {origin.x - mi.rcMonitor.left, origin.y - mi.rcMonitor.top,
            origin.x - mi.rcMonitor.left + client.right, origin.y - mi.rcMonitor.top + client.bottom};
    return crop.left >= 0 && crop.top >= 0 &&
        crop.right <= mi.rcMonitor.right - mi.rcMonitor.left &&
        crop.bottom <= mi.rcMonitor.bottom - mi.rcMonitor.top;
}

// Does the cut-out take in the whole of its screen.
static bool CropIsWholeScreen(const RECT &crop, HMONITOR monitor)
{
    MONITORINFO mi = {sizeof(mi)};
    return GetMonitorInfoW(monitor, &mi) && crop.left == 0 && crop.top == 0 &&
        crop.right == mi.rcMonitor.right - mi.rcMonitor.left && crop.bottom == mi.rcMonitor.bottom - mi.rcMonitor.top;
}

static bool WindowDxgiStart(HWND hwnd)
{
    RECT crop = {}; HMONITOR monitor = nullptr;
    if (!WindowClientOnMonitor(hwnd, crop, monitor)) return false;
    // Only a window that is its whole screen (round 31). A cut-out of the
    // desktop counts whatever changes over the window's place as a frame of
    // the game - the engine's own output among it, when the compositor draws
    // it there: a 15 FPS game was read as 50, and at 20 FPS the extra
    // "frames" were a wave in DLSS MFG's pace (4.6 ms, up to 20, against 0.3
    // for a window over the whole screen). Any other window is taken by
    // Graphics Capture, which hands over that window's own frames only.
    if (!CropIsWholeScreen(crop, monitor) && !Win10Crop()) return false;
    g_capture_target_monitor = monitor;
    const RECT margins = CropMargins(crop.right - crop.left, crop.bottom - crop.top);
    const bool opened = DeskStart(crop.right - crop.left - margins.left - margins.right, crop.bottom - crop.top - margins.top - margins.bottom);
    g_capture_target_monitor = nullptr;
    DXGI_OUTDUPL_DESC desc = {};
    if (lfCap.duplication) lfCap.duplication->GetDesc(&desc);
    if (!opened || !lfCap.duplication || (desc.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
                                        desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)) {
        DeskStop(); return false;
    }
    lfCap.window = hwnd;
    lfCap.window_live = lfCap.window_dxgi = true;
    lfCap.window_crop = crop;
    ShowAffinityApply();
    Trace("[window] DXGI client capture %p at %ux%u (crop %ld,%ld)",
          (void *)hwnd, lfCap.w, lfCap.h, crop.left, crop.top);
    return true;
}

// Prefer DXGI for MFG's focused window; WGC remains the compatibility fallback.
static bool WindowStart(HWND hwnd)
{
    WindowStop();
    if (hwnd == nullptr) { Trace("[window] window source stopped"); return true; }
    if (!IsWindow(hwnd)) { Trace("[window] %p is no window", (void *)hwnd); return false; }
    // (1.1.0: on Windows 10 - Win10Crop - by DXGI whatever was picked, a window scaled onto its screen too.)
    if (((smOpt.capture_api.load() != 1 && !FillWanted()) || Win10Crop()) && WindowDxgiStart(hwnd)) return true;
    Trace("[window] taken by Graphics Capture (the window's own frames)");
    if (!wgc_ns::GraphicsCaptureSession::IsSupported())
    { Trace("[window] this Windows has no Graphics Capture"); return false; }
    if (!SwizzleReady()) return false;
    if (!CaptureDeviceReady()) return false;

    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    catch (winrt::hresult_error const &) {}

    WgcLink *s = new WgcLink();
    try
    {
        IDXGIDevice *dxgi = nullptr;
        if (FAILED(lfCap.d11->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi)))
        { Trace("[window] IDXGIDevice is missing"); delete s; return false; }
        winrt::com_ptr<::IInspectable> insp;
        const HRESULT status = CreateDirect3D11DeviceFromDXGIDevice(dxgi, insp.put());
        dxgi->Release();
        if (FAILED(status))
        { Trace("[window] could not hand the device to WinRT (0x%08X)", status); delete s; return false; }
        s->device = insp.as<wgdx_ns::Direct3D11::IDirect3DDevice>();
        auto capture_interop = winrt::get_activation_factory<wgc_ns::GraphicsCaptureItem>()
                           .as<::IGraphicsCaptureItemInterop>();
        const HRESULT item_status = capture_interop->CreateForWindow(
            hwnd, winrt::guid_of<wgc_ns::GraphicsCaptureItem>(),
            winrt::put_abi(s->item));
        if (FAILED(item_status) || s->item == nullptr)
        { Trace("[window] the window cannot be captured (0x%08X)", item_status); delete s; return false; }
        const auto size = s->item.Size();
        if (size.Width <= 0 || size.Height <= 0)
        { Trace("[window] the window has no size - minimised?"); delete s; return false; }
        lfCap.monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        lfCap.display = HdrDisplayOf(lfCap.monitor);

        s->hdr = lfCap.display.hdr_on;
        s->pool = wgc_ns::Direct3D11CaptureFramePool::CreateFreeThreaded(
            s->device, s->hdr ? wgdx_ns::DirectXPixelFormat::R16G16B16A16Float
                                    : wgdx_ns::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, size);
        s->mailbox = std::make_shared<FrameDrop>();
        s->mailbox->latest_only = FgRequested() || LowLatencyWanted();
        if (s->mailbox->arrived) {
            s->arrival_token = s->pool.FrameArrived([box = s->mailbox](auto const &pool, auto const &) {
                try {
                    if (box->latest_only) {
                        std::lock_guard<std::mutex> lock(box->mutex);
                        for (int n = 0; n < 8; ++n) {
                            auto frame = pool.TryGetNextFrame();
                            if (!frame) break;
                            if (box->latest) box->latest.Close();
                            box->latest = frame;
                        }
                    }
                    SetEvent(box->arrived.get());
                } catch (winrt::hresult_error const &) {   }
            });
        }
        s->session = s->pool.CreateCaptureSession(s->item);
        try { s->session.IsCursorCaptureEnabled(false); }
        catch (winrt::hresult_error const &) { Trace("[window] the pointer stays in the picture"); }

        try { s->session.IsBorderRequired(false); }
        catch (winrt::hresult_error const &) { Trace("[window] Windows keeps its capture border"); }
        // Every frame the window shows, as soon as it shows it (round 31).
        // Left to itself Graphics Capture hands frames over on a slow beat of
        // its own: a game drawing evenly every 50 ms arrived 40, 60, 40, 60 ms
        // apart, and DLSS MFG's frames in between were paced to that.
        try { s->session.MinUpdateInterval(std::chrono::milliseconds(1)); }
        catch (winrt::hresult_error const &) { Trace("[window] this Windows sets the capture's pace itself"); }
        s->session.StartCapture();
        lfCap.window_link = s;
        lfCap.window = hwnd;
        lfCap.window_live = true;

        ShowAffinityApply();
        {
            const RECT margins = CropMargins(size.Width, size.Height);
            lfCap.w = (UINT)(size.Width - margins.left - margins.right);
            lfCap.h = (UINT)(size.Height - margins.top - margins.bottom);
            if (margins.left | margins.top | margins.right | margins.bottom)
                Trace("[window] crop input: %ld, %ld, %ld, %ld pixels left out - the picture is %ux%u", margins.left, margins.top, margins.right, margins.bottom, lfCap.w, lfCap.h);
        }
        Trace("[window] taking window %p at %dx%d", (void *)hwnd, size.Width, size.Height);
        return true;
    }
    catch (winrt::hresult_error const &e)
    {
        Trace("[window] opening threw 0x%08X", (unsigned)e.code());
        delete s;
        return false;
    }
}

// Stop the window source.
static void WindowStop()
{
    Dlss5LinkPause();
    lfCap.window_live = false;
    lfCap.window_dxgi = false;
    lfShow.follow_rect = RECT{};

    ShowAffinityApply();
    if (!lfShow.visible && lfShow.hwnd != nullptr && lfShow.revealed)
    {
        ShowWindow(lfShow.hwnd, SW_SHOWNOACTIVATE);
        lfShow.visible = true;
    }
    if (lfCap.window_link != nullptr)
    {
        try
        {
            if (lfCap.window_link->pool != nullptr && lfCap.window_link->arrival_token.value)
                lfCap.window_link->pool.FrameArrived(lfCap.window_link->arrival_token);
            if (lfCap.window_link->session != nullptr) lfCap.window_link->session.Close();
            if (lfCap.window_link->pool != nullptr) lfCap.window_link->pool.Close();
        }
        catch (winrt::hresult_error const &) {   }
        delete lfCap.window_link;
        lfCap.window_link = nullptr;
        Trace("[window] window source closed");
    }
    DeskStop();
}

// The newest picture of the window, always the newest of the pool.
static bool WindowTake(Pipeline &pipe)
{
    if (Dlss5LinkOn()) return Dlss5LinkTake(pipe);
    if (lfCap.window_live && lfCap.window_dxgi) {
        lfCap.picture_changed = false;
        WindowTrack();
        if (!IsWindow(lfCap.window) || IsIconic(lfCap.window)) { Sleep(8); return false; }
        const HWND front = GetForegroundWindow();
        DWORD source_pid = 0, front_pid = 0;
        GetWindowThreadProcessId(lfCap.window, &source_pid);
        GetWindowThreadProcessId(front, &front_pid);
        static HWND focus_source = nullptr;
        static bool focus_paused = false;
        // (Multi-display mode, round 43: another program in front goes on being taken - it is on another screen.)
        const bool paused = front_pid != source_pid && !smOpt.multi_display.load();
        if (focus_source != lfCap.window || focus_paused != paused)
        {
            if (paused || focus_paused)
                Trace(paused ? "[window] DXGI capture paused: source is not foreground (source PID %lu, foreground PID %lu)"
                             : "[window] DXGI capture resumed: source is foreground (source PID %lu, foreground PID %lu)",
                      source_pid, front_pid);
            focus_source = lfCap.window;
            focus_paused = paused;
        }
        if (paused) { Sleep(8); return false; }
        RECT crop = {}; HMONITOR monitor = nullptr;
        // No longer the whole screen (the game left its borderless size): it
        // is taken again, by Graphics Capture this time (WindowDxgiStart).
        // (1.1.0, Win10Crop: a window cut out of its screen is taken again only when it changed size or left the screen.)
        const bool on_screen = WindowClientOnMonitor(lfCap.window, crop, monitor);
        const bool resized = crop.right - crop.left != lfCap.window_crop.right - lfCap.window_crop.left ||
                             crop.bottom - crop.top != lfCap.window_crop.bottom - lfCap.window_crop.top;
        if (!on_screen || monitor != lfCap.monitor || resized || (!CropIsWholeScreen(crop, monitor) && !Win10Crop())) {
            const HWND hwnd = lfCap.window; WindowStart(hwnd); return false;
        }
        lfCap.window_crop = crop;
        return DeskTake(pipe);
    }
    if (!lfCap.window_live || lfCap.window_link == nullptr) return false;
    lfCap.picture_changed = false;
    const HMONITOR monitor = MonitorFromWindow(lfCap.window, MONITOR_DEFAULTTONEAREST);
    static ULONGLONG last_mode_query = 0;
    if (monitor != lfCap.monitor || GetTickCount64() - last_mode_query > 1000)
    {
        lfCap.monitor = monitor;
        lfCap.display = HdrDisplayOf(monitor);
        last_mode_query = GetTickCount64();
        if (lfCap.window_link->hdr != lfCap.display.hdr_on)
        {
            const HWND hwnd = lfCap.window;
            Trace("[hdr] the window's screen switched HDR mode - reopening");
            WindowStart(hwnd);
            return false;
        }
    }
    try
    {
        const double take_start = PhaseClock();
        auto box = lfCap.window_link->mailbox;
        box->latest_only = FgRequested() || LowLatencyWanted();
        auto newest = [&]() {
            std::lock_guard<std::mutex> lock(box->mutex);
            auto frame = box->latest;
            box->latest = nullptr;

            for (int n = 0; n < 8; ++n) {
                auto next = lfCap.window_link->pool.TryGetNextFrame();
                if (!next) break;
                if (frame) frame.Close();
                frame = next;
            }
            return frame;
        };
        auto frame = newest();

        // Low Latency skips a frame with nothing new: wait a little for one
        // instead of answering at once (the loop would ask again straight
        // away, spinning thousands of times a second). Everything else waits
        // for one too (round 31), as long as the desktop source does
        // (AcquireNextFrame): with nothing new the answer came at once and
        // DLSS MFG's loop span between frames, taking the card from the game.
        if (frame == nullptr && box->arrived) {
            WaitForSingleObject(box->arrived.get(), LowLatencyWanted() ? 8 : 100);
            frame = newest();
        }

        PhaseCount(PHASE_TAKE, take_start);

        if (frame == nullptr) return false;
        lfCap.stamp = frame.SystemRelativeTime().count() * 1e-7;
        TimingCapture(take_start, 0, "wgc");
        lfCap.picture_changed = true;
        ++g_cap_arrivals;
        auto surface_access = frame.Surface().as<IDxgiAccessBridge>();
        ID3D11Texture2D *tex = nullptr;
        if (FAILED(surface_access->GetInterface(__uuidof(ID3D11Texture2D), (void **)&tex)) ||
            tex == nullptr)
        { frame.Close(); return false; }
        UINT new_w = 0, new_h = 0;
        DXGI_FORMAT new_format = DXGI_FORMAT_UNKNOWN;
        const BridgeResult st = BridgeFrame(tex, &new_w, &new_h, &new_format);
        tex->Release();
        frame.Close();
        if (st == BridgeResult::NewSize)
        {
            static UINT last_w = 0, last_h = 0;
            static ULONGLONG first_seen = 0;
            const ULONGLONG now = GetTickCount64();
            if (new_w != last_w || new_h != last_h)
            {
                last_w = new_w; last_h = new_h; first_seen = now;
                return false;
            }
            if (now - first_seen < 250)
                return false;
            Trace("[window] the window came to rest at %ux%u, format %u - reopening",
                new_w, new_h, (unsigned)new_format);
            last_w = last_h = 0; first_seen = 0;
            WindowStart(lfCap.window);
            return false;
        }
        if (st != BridgeResult::Done) return false;
        return SwizzleIntoPicture(pipe);
    }
    catch (winrt::hresult_error const &e)
    {
        Trace("[window] a frame threw 0x%08X - source closed", (unsigned)e.code());
        WindowStop();
        return false;
    }
}

// Take a whole screen through Graphics Capture: used for a screen the
// generator's card does not drive, where duplication cannot reach, or with
// WGC picked in the app.
// Windows moves the frames between the cards.
static bool ScreenViaWgcStart(HMONITOR monitor)
{
    if (!wgc_ns::GraphicsCaptureSession::IsSupported())
    { Trace("[desk] no Windows Graphics Capture here - that screen cannot be taken"); return false; }
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    catch (winrt::hresult_error const &) {}
    WgcLink *s = new WgcLink();
    try
    {
        IDXGIDevice *dxgi = nullptr;
        if (FAILED(lfCap.d11->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi)))
        { Trace("[desk] IDXGIDevice is missing for the screen source"); delete s; return false; }
        winrt::com_ptr<::IInspectable> insp;
        const HRESULT status = CreateDirect3D11DeviceFromDXGIDevice(dxgi, insp.put());
        dxgi->Release();
        if (FAILED(status)) { Trace("[desk] could not hand the device to WinRT (0x%08X)", status); delete s; return false; }
        s->device = insp.as<wgdx_ns::Direct3D11::IDirect3DDevice>();
        auto capture_interop = winrt::get_activation_factory<wgc_ns::GraphicsCaptureItem>()
                           .as<::IGraphicsCaptureItemInterop>();
        const HRESULT item_status = capture_interop->CreateForMonitor(
            monitor, winrt::guid_of<wgc_ns::GraphicsCaptureItem>(), winrt::put_abi(s->item));
        if (FAILED(item_status) || s->item == nullptr)
        { Trace("[desk] the screen cannot be captured (0x%08X)", item_status); delete s; return false; }
        const auto size = s->item.Size();
        if (size.Width <= 0 || size.Height <= 0)
        { Trace("[desk] the screen reports no size"); delete s; return false; }
        lfCap.monitor = monitor;
        lfCap.display = HdrDisplayOf(monitor);

        lfCap.opened_hdr = lfCap.display.hdr_on;
        s->hdr = lfCap.display.hdr_on;
        s->pool = wgc_ns::Direct3D11CaptureFramePool::CreateFreeThreaded(
            s->device, s->hdr ? wgdx_ns::DirectXPixelFormat::R16G16B16A16Float
                              : wgdx_ns::DirectXPixelFormat::B8G8R8A8UIntNormalized, 3, size);
        s->mailbox = std::make_shared<FrameDrop>();
        s->mailbox->latest_only = true;
        if (s->mailbox->arrived)
        {
            s->arrival_token = s->pool.FrameArrived([box = s->mailbox](auto const &pool, auto const &) {
                try
                {
                    std::lock_guard<std::mutex> lock(box->mutex);
                    for (int n = 0; n < 8; ++n)
                    {
                        auto frame = pool.TryGetNextFrame();
                        if (!frame) break;
                        if (box->latest) box->latest.Close();
                        box->latest = frame;
                    }
                    SetEvent(box->arrived.get());
                }
                catch (winrt::hresult_error const &) {   }
            });
        }
        s->session = s->pool.CreateCaptureSession(s->item);

        try { s->session.IsCursorCaptureEnabled(false); }
        catch (winrt::hresult_error const &) { Trace("[desk] the pointer stays in the screen picture"); }
        try { s->session.IsBorderRequired(false); }
        catch (winrt::hresult_error const &) { Trace("[desk] Windows keeps its capture border"); }
        s->session.StartCapture();
        lfCap.screen_link = s;
        lfCap.screen_via_wgc = true;
        lfCap.upside_down = false;
        lfCap.w = (UINT)size.Width;
        lfCap.h = (UINT)size.Height;
        lfCap.live = true;
        Trace("[desk] taking the desktop at %ux%u (through Windows Graphics Capture)", lfCap.w, lfCap.h);
        return true;
    }
    catch (winrt::hresult_error const &e)
    {
        Trace("[desk] the screen source threw 0x%08X", (unsigned)e.code());
        delete s;
        return false;
    }
}

// Stop the screen source.
static void ScreenViaWgcStop()
{
    lfCap.screen_via_wgc = false;
    if (lfCap.screen_link == nullptr) return;
    try
    {
        if (lfCap.screen_link->pool != nullptr && lfCap.screen_link->arrival_token.value)
            lfCap.screen_link->pool.FrameArrived(lfCap.screen_link->arrival_token);
        if (lfCap.screen_link->session != nullptr) lfCap.screen_link->session.Close();
        if (lfCap.screen_link->pool != nullptr) lfCap.screen_link->pool.Close();
    }
    catch (winrt::hresult_error const &) {   }
    delete lfCap.screen_link;
    lfCap.screen_link = nullptr;
    Trace("[desk] screen source (Graphics Capture) closed");
}

// The newest screen picture; waits up to 100 ms like duplication does.
static bool ScreenViaWgcTake(Pipeline &pipe)
{
    if (lfCap.screen_link == nullptr) return false;
    try
    {
        const double take_start = PhaseClock();
        auto box = lfCap.screen_link->mailbox;
        auto newest = [&]() {
            std::lock_guard<std::mutex> lock(box->mutex);
            auto frame = box->latest;
            box->latest = nullptr;
            for (int n = 0; n < 8; ++n)
            {
                auto next = lfCap.screen_link->pool.TryGetNextFrame();
                if (!next) break;
                if (frame) frame.Close();
                frame = next;
            }
            return frame;
        };
        auto frame = newest();

        if (frame == nullptr && box->arrived)
        {
            WaitForSingleObject(box->arrived.get(), 100);
            frame = newest();
        }
        PhaseCount(PHASE_TAKE, take_start);
        if (frame == nullptr) return false;
        lfCap.stamp = frame.SystemRelativeTime().count() * 1e-7;
        TimingCapture(take_start, 0, "wgc-monitor");
        lfCap.picture_changed = true;
        auto surface_access = frame.Surface().as<IDxgiAccessBridge>();
        ID3D11Texture2D *tex = nullptr;
        if (FAILED(surface_access->GetInterface(__uuidof(ID3D11Texture2D), (void **)&tex)) || tex == nullptr)
        { frame.Close(); return false; }
        UINT new_w = 0, new_h = 0;
        DXGI_FORMAT new_format = DXGI_FORMAT_UNKNOWN;
        const BridgeResult st = BridgeFrame(tex, &new_w, &new_h, &new_format);
        tex->Release();
        frame.Close();
        if (st == BridgeResult::NewFormat) return false;
        if (st == BridgeResult::NewSize)
        {
            Trace("[desk] the screen is now %ux%u, format %u - reopening",
                new_w, new_h, (unsigned)new_format);
            DeskStart(new_w, new_h);
            return false;
        }
        if (st != BridgeResult::Done) return false;
        return SwizzleIntoPicture(pipe);
    }
    catch (winrt::hresult_error const &e)
    {
        Trace("[desk] a screen frame threw 0x%08X - reopening", (unsigned)e.code());
        DeskStart(lfCap.w, lfCap.h);
        return false;
    }
}

// The small luma (lf_luma.inl), made on the capture's own device in the piece
// of work that copies the frame, and read from there. Made on the generator's
// device it was a piece of work by itself, and so was the conversion before
// it: the loop needs the luma before it knows whether the frame is a new one
// of the game's at all, and how far the picture moved (sm_guard.inl) - and
// only then is the generator asked. This way a captured frame is two pieces:
// the copy with its luma here, then everything the generator's device does.
//
// For what DLSS MFG takes by itself: an 8-bit picture the right way up, the
// generator finding the motion (no optical flow luma to make).
static struct Luma11
{
    ID3D11Device *device = nullptr;          // the device all of this was made on
    ID3D11Texture2D *bridge = nullptr;       // the shared texture the view is of
    ID3D11ComputeShader *shader = nullptr;
    ID3D11Buffer *extent = nullptr;
    ID3D11Texture2D *target = nullptr, *staging = nullptr;
    ID3D11UnorderedAccessView *target_view = nullptr;
    ID3D11ShaderResourceView *picture_view = nullptr;
    UINT w = 0, h = 0, pic_w = 0, pic_h = 0;
    bool failed = false, told = false;
} g_luma11;

// This frame's luma came with its copy: its conversion needs no reading back.
static bool g_capture_joined = false;

static void Luma11Stop()
{
    Luma11 &l = g_luma11;
    if (l.picture_view) { l.picture_view->Release(); l.picture_view = nullptr; }
    if (l.target_view) { l.target_view->Release(); l.target_view = nullptr; }
    if (l.staging) { l.staging->Release(); l.staging = nullptr; }
    if (l.target) { l.target->Release(); l.target = nullptr; }
    if (l.extent) { l.extent->Release(); l.extent = nullptr; }
    if (l.shader) { l.shader->Release(); l.shader = nullptr; }
    if (l.bridge) { l.bridge->Release(); l.bridge = nullptr; }
    if (l.device) { l.device->Release(); l.device = nullptr; }
    l.w = l.h = l.pic_w = l.pic_h = 0;
}

static bool Luma11Wanted(DXGI_FORMAT format)
{
    static const bool off = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_SPLIT_CAPTURE", v, sizeof(v)) > 0 && v[0] == '1'; }();
    return !off && !g_luma11.failed && FgRequested() && !MotionGiven() && (format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM) &&
           !(lfCap.live && lfCap.upside_down) && lfLuma.mapped && lfLuma.view != nullptr && lfLuma.w != 0;
}

static bool Luma11Ready(ID3D11Device *device, UINT pic_w, UINT pic_h)
{
    Luma11 &l = g_luma11;
    if (l.device == device && l.bridge == lfCap.bridge11 && l.w == lfLuma.w && l.h == lfLuma.h && l.pic_w == pic_w &&
        l.pic_h == pic_h && l.shader != nullptr)
        return true;
    Luma11Stop();
    auto fail = [&](const char *what) {
        Trace("[luma] the capture's device cannot make the luma (%s) - made on the generator's, a piece of work more", what);
        Luma11Stop();
        l.failed = true;
        return false;
    };
    ID3DBlob *blob = nullptr, *err = nullptr;
    if (FAILED(CachedCompile(kLumaHlsl, sizeof(kLumaHlsl) - 1, "lf-luma.hlsl", nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, &blob, &err)))
        return fail("its shader did not compile");
    const HRESULT made = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &l.shader);
    blob->Release();
    if (FAILED(made)) return fail("no compute shader");
    const UINT sizes[4] = { pic_w, pic_h, lfLuma.w, lfLuma.h };
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(sizes); bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA init = { sizes, 0, 0 };
    if (FAILED(device->CreateBuffer(&bd, &init, &l.extent))) return fail("no constant buffer");
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = lfLuma.w; td.Height = lfLuma.h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &l.target))) return fail("no target");
    if (FAILED(device->CreateUnorderedAccessView(l.target, nullptr, &l.target_view))) return fail("no view of the target");
    td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &l.staging))) return fail("no texture to read from");
    if (FAILED(device->CreateShaderResourceView(lfCap.bridge11, nullptr, &l.picture_view))) return fail("no view of the picture");
    l.device = device; l.device->AddRef();
    l.bridge = lfCap.bridge11; l.bridge->AddRef();
    l.w = lfLuma.w; l.h = lfLuma.h; l.pic_w = pic_w; l.pic_h = pic_h;
    if (!l.told)
    {
        l.told = true;
        Trace("[luma] made with the capture's copy, on its device: a captured frame goes to the card in two pieces of work");
    }
    return true;
}

// After the frame's copy into the shared texture, before the flush.
static void Luma11Record(ID3D11DeviceContext *c)
{
    Luma11 &l = g_luma11;
    c->CSSetShader(l.shader, nullptr, 0);
    c->CSSetConstantBuffers(0, 1, &l.extent);
    c->CSSetShaderResources(0, 1, &l.picture_view);
    c->CSSetUnorderedAccessViews(0, 1, &l.target_view, nullptr);
    c->Dispatch((l.w + 7) / 8, (l.h + 7) / 8, 1);
    ID3D11ShaderResourceView *no_picture = nullptr;
    ID3D11UnorderedAccessView *no_target = nullptr;
    c->CSSetShaderResources(0, 1, &no_picture);
    c->CSSetUnorderedAccessViews(0, 1, &no_target, nullptr);
    c->CopyResource(l.staging, l.target);
}

// After the flush: waits for the card and takes the luma.
static bool Luma11Read(ID3D11DeviceContext *context)
{
    Luma11 &l = g_luma11;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(l.staging, 0, D3D11_MAP_READ, 0, &mapped)) || mapped.pData == nullptr) return false;
    for (UINT y = 0; y < l.h; ++y)
        memcpy(lfLuma.view + static_cast<size_t>(y) * l.w, static_cast<const BYTE *>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, l.w);
    context->Unmap(l.staging, 0);
    return true;
}

// ---- A picture from the screen's own card -----------------------------------
// On a laptop the screen usually hangs on the processor's graphics, while the
// generator's card drives no screen at all - and a picture made on one card
// cannot be opened by a device on another (Direct3D refuses the shared handle).
// So a frame goes the way the output does when the screen hangs on another card
// (lf_show_relay.inl): through a buffer both cards see, in the computer's
// memory, with a fence each way. The screen's card copies the frame into one of
// three such buffers; the generator's card copies it out into the bridge
// picture the channel-order pass reads. Each card waits on the other's fence,
// never on the processor, and a frame in hand is never waited for.
struct ScreenCross
{
    bool on = false;
    ID3D12Device *dev = nullptr;                 // on the screen's card
    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *alloc[3] = {};
    ID3D12GraphicsCommandList *list = nullptr;   // there: the picture into a shared buffer
    ID3D12CommandAllocator *alloc_gen[3] = {};   // on the generator's card: a buffer into the bridge picture
    ID3D12GraphicsCommandList *list_gen[3] = {};
    ID3D12Fence *input12 = nullptr;              // the copy on the screen card's Direct3D 11 device is done
    ID3D11Fence *input11 = nullptr;
    ID3D12Fence *made = nullptr, *made_gen = nullptr;   // a buffer is filled ...
    ID3D12Fence *read = nullptr, *read_cap = nullptr;   // ... and may be written again
    ID3D12Heap *heap_gen = nullptr, *heap_cap = nullptr;
    ID3D12Resource *pass_gen[3] = {}, *pass_cap[3] = {};
    ID3D12Resource *share12 = nullptr;           // the frame's copy, opened on the screen's card
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot = {};
    UINT64 pass_bytes = 0;
    UINT w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT64 input_value = 0, made_value = 0, read_value = 0;
    UINT64 read_of[3] = {};
    UINT at = 0;
    HANDLE event = nullptr;
};
static ScreenCross g_screen_cross;

static void ScreenCrossStop()
{
    ScreenCross &c = g_screen_cross;
    // Both cards done with the buffers before any of them is let go.
    if (c.made && c.made_value) AwaitMark(c.made, c.made_value, 2000);
    if (c.read && c.read_value) AwaitMark(c.read, c.read_value, 2000);
    for (int i = 0; i < 3; ++i)
    {
        if (c.pass_gen[i]) { c.pass_gen[i]->Release(); c.pass_gen[i] = nullptr; }
        if (c.pass_cap[i]) { c.pass_cap[i]->Release(); c.pass_cap[i] = nullptr; }
        if (c.list_gen[i]) { c.list_gen[i]->Release(); c.list_gen[i] = nullptr; }
        if (c.alloc_gen[i]) { c.alloc_gen[i]->Release(); c.alloc_gen[i] = nullptr; }
        if (c.alloc[i]) { c.alloc[i]->Release(); c.alloc[i] = nullptr; }
        c.read_of[i] = 0;
    }
    if (c.list) { c.list->Release(); c.list = nullptr; }
    if (c.share12) { c.share12->Release(); c.share12 = nullptr; }
    if (c.heap_cap) { c.heap_cap->Release(); c.heap_cap = nullptr; }
    if (c.heap_gen) { c.heap_gen->Release(); c.heap_gen = nullptr; }
    if (c.input12) { c.input12->Release(); c.input12 = nullptr; }
    if (c.input11) { c.input11->Release(); c.input11 = nullptr; }
    if (c.made_gen) { c.made_gen->Release(); c.made_gen = nullptr; }
    if (c.made) { c.made->Release(); c.made = nullptr; }
    if (c.read_cap) { c.read_cap->Release(); c.read_cap = nullptr; }
    if (c.read) { c.read->Release(); c.read = nullptr; }
    if (c.queue) { c.queue->Release(); c.queue = nullptr; }
    if (c.dev) { c.dev->Release(); c.dev = nullptr; }
    if (c.event) { CloseHandle(c.event); c.event = nullptr; }
    c.on = false;
    c.w = c.h = 0; c.format = DXGI_FORMAT_UNKNOWN;
    c.input_value = c.made_value = c.read_value = 0;
    c.at = 0;
}

// Everything the two cards need to pass a picture between them, for a picture
// of w x h in format (the bridge picture, made on the generator's own card).
static bool ScreenCrossSet(UINT w, UINT h, DXGI_FORMAT format)
{
    ScreenCrossStop();
    ScreenCross &c = g_screen_cross;
    c.w = w; c.h = h; c.format = format;
    c.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    bool ok = c.event != nullptr;

    // A device on the screen's card, only to reach the memory both cards see.
    IDXGIDevice *dxgi = nullptr;
    IDXGIAdapter *adapter = nullptr;
    if (ok && (FAILED(lfCap.cap11->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi)) || dxgi == nullptr ||
               FAILED(dxgi->GetAdapter(&adapter)) || adapter == nullptr))
    { Trace("[capture] the screen's card cannot name its adapter"); ok = false; }
    if (dxgi) dxgi->Release();
    if (ok)
    {
        const HRESULT made = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void **)&c.dev);
        if (FAILED(made) || c.dev == nullptr)
        {
            Trace("[capture] the screen's card gave no Direct3D 12 device (0x%08X) - its frames cannot cross", (unsigned)made);
            ok = false;
        }
    }
    if (adapter) adapter->Release();

    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    if (ok) ok = SUCCEEDED(c.dev->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&c.queue)));
    for (int i = 0; ok && i < 3; ++i)
        ok = SUCCEEDED(c.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c.alloc[i]))) &&
             SUCCEEDED(lfCore.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c.alloc_gen[i]))) &&
             SUCCEEDED(lfCore.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.alloc_gen[i], nullptr, IID_PPV_ARGS(&c.list_gen[i])));
    if (ok) ok = SUCCEEDED(c.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c.alloc[0], nullptr, IID_PPV_ARGS(&c.list)));
    for (int i = 0; ok && i < 3; ++i) if (FAILED(c.list_gen[i]->Close())) ok = false;
    if (ok && FAILED(c.list->Close())) ok = false;

    // The frame's copy, opened where the screen's card can see it.
    if (ok) ok = SUCCEEDED(c.dev->OpenSharedHandle(lfCap.bridge_handle, IID_PPV_ARGS(&c.share12)));

    // The copyable layout of the picture, and the memory both cards see.
    if (ok)
    {
        D3D12_RESOURCE_DESC picture = lfCap.bridge12->GetDesc();
        UINT rows = 0; UINT64 row_bytes = 0, bytes = 0;
        lfCore.device->GetCopyableFootprints(&picture, 0, 1, 0, &c.foot, &rows, &row_bytes, &bytes);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT there = {};
        c.dev->GetCopyableFootprints(&picture, 0, 1, 0, &there, nullptr, nullptr, nullptr);
        if (bytes == 0 || there.Footprint.RowPitch != c.foot.Footprint.RowPitch)
        {
            Trace("[capture] the two cards lay a picture out differently (%u against %u a row) - frames cannot cross",
                  (unsigned)there.Footprint.RowPitch, (unsigned)c.foot.Footprint.RowPitch);
            ok = false;
        }
        else c.pass_bytes = (bytes + 65535) & ~UINT64(65535);
    }
    D3D12_HEAP_DESC heap = {};
    heap.SizeInBytes = c.pass_bytes * 3;
    heap.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER;
    HANDLE shared = nullptr;
    if (ok) ok = SUCCEEDED(lfCore.device->CreateHeap(&heap, IID_PPV_ARGS(&c.heap_gen))) &&
                 SUCCEEDED(lfCore.device->CreateSharedHandle(c.heap_gen, nullptr, GENERIC_ALL, nullptr, &shared)) &&
                 shared != nullptr &&
                 SUCCEEDED(c.dev->OpenSharedHandle(shared, IID_PPV_ARGS(&c.heap_cap)));
    if (shared) CloseHandle(shared);
    D3D12_RESOURCE_DESC pass = {};
    pass.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    pass.Width = c.pass_bytes; pass.Height = 1; pass.DepthOrArraySize = 1; pass.MipLevels = 1;
    pass.SampleDesc.Count = 1; pass.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    pass.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    for (int i = 0; ok && i < 3; ++i)
        ok = SUCCEEDED(lfCore.device->CreatePlacedResource(c.heap_gen, i * c.pass_bytes, &pass, D3D12_RESOURCE_STATE_COMMON,
                                                           nullptr, IID_PPV_ARGS(&c.pass_gen[i]))) &&
             SUCCEEDED(c.dev->CreatePlacedResource(c.heap_cap, i * c.pass_bytes, &pass, D3D12_RESOURCE_STATE_COMMON,
                                                   nullptr, IID_PPV_ARGS(&c.pass_cap[i])));

    // A fence each way: made on one card, opened on the other.
    auto fence_pair = [](ID3D12Device *from, ID3D12Device *to, ID3D12Fence **here, ID3D12Fence **there) {
        if (FAILED(from->CreateFence(0, D3D12_FENCE_FLAG_SHARED | D3D12_FENCE_FLAG_SHARED_CROSS_ADAPTER, IID_PPV_ARGS(here))))
            return false;
        HANDLE handle = nullptr;
        if (FAILED(from->CreateSharedHandle(*here, nullptr, GENERIC_ALL, nullptr, &handle)) || handle == nullptr) return false;
        const bool opened = SUCCEEDED(to->OpenSharedHandle(handle, IID_PPV_ARGS(there)));
        CloseHandle(handle);
        return opened;
    };
    ok = ok && fence_pair(c.dev, lfCore.device, &c.made, &c.made_gen) &&
         fence_pair(lfCore.device, c.dev, &c.read, &c.read_cap);
    // The copy on the screen card's Direct3D 11 device: the same card, so the
    // fence only needs to be shared, not shared across adapters.
    if (ok) ok = SUCCEEDED(c.dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&c.input12)));
    HANDLE input_handle = nullptr;
    if (ok) ok = SUCCEEDED(c.dev->CreateSharedHandle(c.input12, nullptr, GENERIC_ALL, nullptr, &input_handle)) &&
                 input_handle != nullptr;
    if (ok)
    {
        ID3D11Device5 *d5 = nullptr;
        ok = SUCCEEDED(lfCap.cap11->QueryInterface(IID_PPV_ARGS(&d5))) && d5 != nullptr &&
             SUCCEEDED(d5->OpenSharedFence(input_handle, IID_PPV_ARGS(&c.input11)));
        if (d5) d5->Release();
    }
    if (input_handle) CloseHandle(input_handle);

    if (ok)
    {
        c.on = true;
        Trace("[capture] the screen's frames cross to the generator's card through %llu KB buffers",
              (unsigned long long)(c.pass_bytes / 1024));
    }
    else
    {
        Trace("[capture] the two cards cannot pass a picture between them - the frames cannot cross");
        ScreenCrossStop();
    }
    return ok;
}

// One frame from the screen's card into the bridge picture on the generator's.
// Called after the frame was copied into the bridge texture on the screen's
// card and that copy was signalled (input fence).
static bool ScreenCrossSend()
{
    ScreenCross &c = g_screen_cross;
    if (!c.on || c.list == nullptr || c.list_gen[0] == nullptr) return false;

    const UINT slot = c.at;
    // The buffer this frame goes into may still be read by the generator's card.
    if (c.read_of[slot] != 0 && FAILED(c.queue->Wait(c.read_cap, c.read_of[slot]))) return false;
    if (FAILED(c.alloc[slot]->Reset())) return false;
    if (FAILED(c.list->Reset(c.alloc[slot], nullptr))) return false;
    D3D12_RESOURCE_BARRIER to_source = Barrier(c.share12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    c.list->ResourceBarrier(1, &to_source);
    D3D12_TEXTURE_COPY_LOCATION from = {}, into = {};
    from.pResource = c.share12; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex = 0;
    into.pResource = c.pass_cap[slot]; into.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; into.PlacedFootprint = c.foot;
    c.list->CopyTextureRegion(&into, 0, 0, 0, &from, nullptr);
    D3D12_RESOURCE_BARRIER back = Barrier(c.share12, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    c.list->ResourceBarrier(1, &back);
    if (FAILED(c.list->Close())) return false;
    ID3D12CommandList *lists[] = { c.list };
    c.queue->ExecuteCommandLists(1, lists);
    ++c.made_value;
    if (FAILED(c.queue->Signal(c.made, c.made_value))) return false;

    // The generator's card: waits for the buffer, copies it into the picture the
    // channel-order pass reads (which is recorded after this, on the same queue).
    if (FAILED(lfCore.direct_queue->Wait(c.made_gen, c.made_value))) return false;
    if (c.read_of[slot] != 0 && c.read->GetCompletedValue() < c.read_of[slot] &&
        !AwaitMark(c.read, c.read_of[slot], 1000)) return false;
    if (FAILED(c.alloc_gen[slot]->Reset())) return false;
    if (FAILED(c.list_gen[slot]->Reset(c.alloc_gen[slot], nullptr))) return false;
    D3D12_RESOURCE_BARRIER to_dest = Barrier(lfCap.bridge12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    c.list_gen[slot]->ResourceBarrier(1, &to_dest);
    D3D12_TEXTURE_COPY_LOCATION from_gen = {}, into_gen = {};
    from_gen.pResource = c.pass_gen[slot]; from_gen.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from_gen.PlacedFootprint = c.foot;
    into_gen.pResource = lfCap.bridge12; into_gen.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; into_gen.SubresourceIndex = 0;
    c.list_gen[slot]->CopyTextureRegion(&into_gen, 0, 0, 0, &from_gen, nullptr);
    D3D12_RESOURCE_BARRIER back_gen = Barrier(lfCap.bridge12, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    c.list_gen[slot]->ResourceBarrier(1, &back_gen);
    if (FAILED(c.list_gen[slot]->Close())) return false;
    ID3D12CommandList *gen_lists[] = { c.list_gen[slot] };
    lfCore.direct_queue->ExecuteCommandLists(1, gen_lists);
    ++c.read_value;
    if (FAILED(lfCore.direct_queue->Signal(c.read, c.read_value))) return false;
    c.read_of[slot] = c.read_value;
    // (The output window's queue waits on this one: the picture is in the bridge.)
    if (FAILED(lfCore.direct_queue->Signal(lfCap.sync12, lfCap.sync_value))) return false;
    ++lfCap.sync_value;
    ++g_pieces[2];
    c.at = (c.at + 1) % 3;
    return true;
}

// Copy a D3D11 source texture into the shared texture and signal the fence
// D3D12 waits on. A new size or format rebuilds the bridge. A frame made on
// the screen's own card (another card than the generator's) crosses through
// ScreenCrossSend instead of the shared handle.
static BridgeResult BridgeFrame(ID3D11Texture2D *frame, UINT *out_w, UINT *out_h,
                                      DXGI_FORMAT *out_format)
{
    D3D11_TEXTURE2D_DESC fd = {};
    frame->GetDesc(&fd);
    D3D11_BOX source_box = {0, 0, 0, fd.Width, fd.Height, 1};
    if (lfCap.window_dxgi) {
        const RECT &r = lfCap.window_crop;
        if (r.left < 0 || r.top < 0 || r.right <= r.left || r.bottom <= r.top ||
            r.right > LONG(fd.Width) || r.bottom > LONG(fd.Height)) return BridgeResult::Error;
        source_box = {UINT(r.left), UINT(r.top), 0, UINT(r.right), UINT(r.bottom), 1};
        fd.Width = r.right - r.left; fd.Height = r.bottom - r.top;
    }
    // Crop input (round 43): the sides the person left out of the captured window - only of a window (the output is
    // put over the same part of it: WindowBounds), and never down to less than 64 pixels a side.
    if (lfCap.window_live && !Dlss5LinkOn())
    {
        const RECT m = CropMargins(LONG(fd.Width), LONG(fd.Height));
        if (m.left | m.top | m.right | m.bottom)
        {
            source_box.left += m.left; source_box.top += m.top; source_box.right -= m.right; source_box.bottom -= m.bottom;
            fd.Width -= m.left + m.right; fd.Height -= m.top + m.bottom;
        }
    }
    if (out_w != nullptr) *out_w = fd.Width;
    if (out_h != nullptr) *out_h = fd.Height;
    if (out_format != nullptr) *out_format = fd.Format;

    if (fd.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        fd.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&      // LIVE FLOW's frames (sm_dlss5_link.inl)
        fd.Format != DXGI_FORMAT_R10G10B10A2_UNORM &&
        fd.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
    { Trace("[hdr] cannot use source format %u", fd.Format); return BridgeResult::Error; }

    // Which card the frame came on. The generator's own device (Graphics
    // Capture, LIVE FLOW) is copied on it; the screen's card's device is the
    // other way, and its frames cross.
    ID3D11Device *source_device = nullptr;
    frame->GetDevice(&source_device);
    if (source_device == nullptr)
    { Trace("[source] the frame names no device"); return BridgeResult::Error; }
    const bool from_screen_card = lfCap.cap11 != nullptr && source_device == lfCap.cap11;
    source_device->Release();
    ID3D11Device *copy_dev = from_screen_card ? lfCap.cap11 : lfCap.d11;
    ID3D11DeviceContext *copy_ctx = from_screen_card ? lfCap.cap11_ctx : lfCap.d11_ctx;
    if (copy_dev == nullptr || copy_ctx == nullptr) { Trace("[source] the frame's device is gone"); return BridgeResult::Error; }
    const bool cross = from_screen_card && lfCap.cross;

    // A bridge made the other way (one card against two) is made again.
    if (lfCap.bridge11 != nullptr && lfCap.bridge_cross != cross)
    {
        Trace("[source] the source's card changed - the bridge is made again");
        if (lfCap.bridge12) { lfCap.bridge12->Release(); lfCap.bridge12 = nullptr; }
        if (lfCap.bridge_handle) { CloseHandle(lfCap.bridge_handle); lfCap.bridge_handle = nullptr; }
        if (lfCap.bridge11) { lfCap.bridge11->Release(); lfCap.bridge11 = nullptr; }
        if (lfCap.sync12) { lfCap.sync12->Release(); lfCap.sync12 = nullptr; }
        if (lfCap.sync11) { lfCap.sync11->Release(); lfCap.sync11 = nullptr; }
        ScreenCrossStop();
        lfCap.bridge_cross = false;
    }

    if (lfCap.bridge11 == nullptr)
    {
        lfHdr.float_frames = fd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;

        lfHdr.keep = lfHdr.float_frames && HdrSwitchOn() && lfCap.display.hdr_on;
        Trace("[source] source format %u (%s)", (unsigned)fd.Format,
            fd.Format == DXGI_FORMAT_B8G8R8A8_UNORM ? "8-bit BGRA" :
            fd.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? "8-bit RGBA" :
            fd.Format == DXGI_FORMAT_R10G10B10A2_UNORM ? "10-bit RGB10A2 screen" :
            "half float");
        Trace("[hdr] source %s",
            lfHdr.float_frames ? (lfHdr.keep ? "half-float scRGB"
                                             : "half float, tone-mapped to SDR")
                            : "SDR");
        D3D11_TEXTURE2D_DESC view_desc = {};
        view_desc.Width = fd.Width; view_desc.Height = fd.Height; view_desc.MipLevels = 1; view_desc.ArraySize = 1;
        view_desc.Format = fd.Format; view_desc.SampleDesc.Count = 1; view_desc.Usage = D3D11_USAGE_DEFAULT;
        view_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        view_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        if (FAILED(copy_dev->CreateTexture2D(&view_desc, nullptr, &lfCap.bridge11)))
        { Trace("[source] no shared texture"); return BridgeResult::Error; }
        IDXGIResource1 *r1 = nullptr;
        lfCap.bridge11->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1);
        if (!r1 || FAILED(r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &lfCap.bridge_handle)))
        { Trace("[source] no NT handle for the shared texture"); if (r1) r1->Release(); goto fail_capture; }
        r1->Release();

        lfCap.sync_value = 1;
        if (cross)
        {
            // A picture of the generator's own card, filled from the shared buffers.
            D3D12_HEAP_PROPERTIES vram_heap = { D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC tex_desc = {};
            tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            tex_desc.Width = fd.Width; tex_desc.Height = fd.Height; tex_desc.DepthOrArraySize = 1; tex_desc.MipLevels = 1;
            tex_desc.Format = fd.Format; tex_desc.SampleDesc.Count = 1;
            tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            tex_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            if (FAILED(lfCore.device->CreateCommittedResource(&vram_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                                      D3D12_RESOURCE_STATE_COMMON, nullptr, __uuidof(ID3D12Resource),
                                                      reinterpret_cast<void **>(&lfCap.bridge12))))
            { Trace("[source] no picture of the screen's size on the generator's card"); goto fail_capture; }
            // (The output window's queue waits on this: the frame is in the bridge.)
            if (FAILED(lfCore.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                          reinterpret_cast<void **>(&lfCap.sync12))))
            { Trace("[source] no fence for the bridge picture"); goto fail_capture; }
            if (!ScreenCrossSet(fd.Width, fd.Height, fd.Format)) goto fail_capture;
        }
        else
        {
            if (FAILED(lfCore.device->OpenSharedHandle(lfCap.bridge_handle, __uuidof(ID3D12Resource),
                                               (void **)&lfCap.bridge12)))
            { Trace("[source] D3D12 could not open the shared texture"); goto fail_capture; }

            if (FAILED(lfCore.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence),
                                          reinterpret_cast<void **>(&lfCap.sync12))))
            { Trace("[source] no shared fence"); goto fail_capture; }
            HANDLE nt_f = nullptr;
            lfCore.device->CreateSharedHandle(lfCap.sync12, nullptr, GENERIC_ALL, nullptr, &nt_f);
            ID3D11Device5 *d5 = nullptr;
            if (copy_dev->QueryInterface(__uuidof(ID3D11Device5), (void **)&d5) == S_OK)
            {
                d5->OpenSharedFence(nt_f, __uuidof(ID3D11Fence), (void **)&lfCap.sync11);
                d5->Release();
            }
            if (nt_f) CloseHandle(nt_f);
            if (!lfCap.sync11)
            { Trace("[source] the D3D11 side has no fence - source unusable"); goto fail_capture; }
        }
        D3D12_HEAP_PROPERTIES vram_heap = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC tex_desc = {};
        tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tex_desc.Width = fd.Width; tex_desc.Height = fd.Height; tex_desc.DepthOrArraySize = 1; tex_desc.MipLevels = 1;
        tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; tex_desc.SampleDesc.Count = 1;
        tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(lfCore.device->CreateCommittedResource(&vram_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                  __uuidof(ID3D12Resource),
                                                  reinterpret_cast<void **>(&lfCap.rgba))))
        { Trace("[source] no RGBA target"); goto fail_capture; }
        lfCap.bridge_cross = cross;
        Trace("[source] shared picture %ux%u ready%s", (UINT)fd.Width, (UINT)fd.Height,
              cross ? " (through the memory both cards see)" : "");
    }

    {
        D3D11_TEXTURE2D_DESC view_desc = {};
        lfCap.bridge11->GetDesc(&view_desc);
        if (view_desc.Width != fd.Width || view_desc.Height != fd.Height)
            return BridgeResult::NewSize;
        if (view_desc.Format != fd.Format)
        {
            Trace("[source] source format %u -> %u - new bridge",
                (unsigned)view_desc.Format, (unsigned)fd.Format);
            CloseFgResources();
            HdrRelease();
            ScreenCrossStop();
            if (lfCap.bridge12) { lfCap.bridge12->Release(); lfCap.bridge12 = nullptr; }
            if (lfCap.bridge_handle)  { CloseHandle(lfCap.bridge_handle); lfCap.bridge_handle = nullptr; }
            if (lfCap.sync12) { lfCap.sync12->Release(); lfCap.sync12 = nullptr; }
            if (lfCap.sync11) { lfCap.sync11->Release(); lfCap.sync11 = nullptr; }
            if (lfCap.rgba) { lfCap.rgba->Release(); lfCap.rgba = nullptr; }
            if (lfCap.bridge11) { lfCap.bridge11->Release(); lfCap.bridge11 = nullptr; }
            lfCap.bridge_cross = false;
            return BridgeResult::NewFormat;
        }
        if (g_convert_mark) { AwaitMark(lfCore.timeline, g_convert_mark, 2000); g_convert_mark = 0; }
        if (cross && g_screen_cross.made_value != 0)
        {
            // The screen's card may still be copying the frame before into the
            // picture this one goes into: written again only once that is done.
            if (!AwaitMark(g_screen_cross.made, g_screen_cross.made_value, 2000)) return BridgeResult::Error;
        }
        copy_ctx->CopySubresourceRegion(lfCap.bridge11, 0, 0, 0, 0, frame, 0, &source_box);
    }
    g_capture_joined = false;
    const bool luma_here = Luma11Wanted(fd.Format) && Luma11Ready(copy_dev, fd.Width, fd.Height);
    if (luma_here) Luma11Record(copy_ctx);
    ID3D11DeviceContext4 *ctx4 = nullptr;
    if (FAILED(copy_ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&ctx4)))
        return BridgeResult::Error;
    if (cross)
    {
        const HRESULT signaled = ctx4->Signal(g_screen_cross.input11, ++g_screen_cross.input_value);
        ctx4->Release();
        if (FAILED(signaled)) return BridgeResult::Error;
        if (FAILED(g_screen_cross.queue->Wait(g_screen_cross.input12, g_screen_cross.input_value)))
            return BridgeResult::Error;
        copy_ctx->Flush();
        if (!ScreenCrossSend()) { Trace("[source] the frame could not cross to the generator's card"); return BridgeResult::Error; }
        if (luma_here)
        {
            if (Luma11Read(copy_ctx)) g_capture_joined = true;
            else { Trace("[luma] the capture's device did not hand the luma over - made on the generator's from here on"); g_luma11.failed = true; }
        }
        return BridgeResult::Done;
    }
    const HRESULT signaled = ctx4->Signal(lfCap.sync11, lfCap.sync_value);
    ctx4->Release();
    if (FAILED(signaled)) return BridgeResult::Error;
    copy_ctx->Flush();
    ++g_pieces[2];

    const UINT64 wanted = lfCap.sync_value;
    // The conversion is submitted next on this queue. GPU-to-GPU ordering
    // replaces a CPU sleep/wakeup between the shared copy and conversion.
    if (FAILED(lfCore.direct_queue->Wait(lfCap.sync12, wanted))) return BridgeResult::Error;
    ++lfCap.sync_value;
    if (luma_here)
    {
        if (Luma11Read(copy_ctx)) g_capture_joined = true;
        else { Trace("[luma] the capture's device did not hand the luma over - made on the generator's from here on"); g_luma11.failed = true; }
    }
    return BridgeResult::Done;
fail_capture:

    ScreenCrossStop();
    if (lfCap.bridge12) { lfCap.bridge12->Release(); lfCap.bridge12 = nullptr; }
    if (lfCap.bridge_handle)  { CloseHandle(lfCap.bridge_handle); lfCap.bridge_handle = nullptr; }
    if (lfCap.sync12) { lfCap.sync12->Release(); lfCap.sync12 = nullptr; }
    if (lfCap.sync11) { lfCap.sync11->Release(); lfCap.sync11 = nullptr; }
    if (lfCap.rgba) { lfCap.rgba->Release(); lfCap.rgba = nullptr; }
    if (lfCap.bridge11) { lfCap.bridge11->Release(); lfCap.bridge11 = nullptr; }
    lfCap.bridge_cross = false;
    return BridgeResult::Error;
}

// The small pass that puts BGRA sources in RGBA order.
static bool SwizzleReady()
{
    if (lfCap.swz_pso) return true;
    ID3DBlob *blob = nullptr, *err = nullptr;
    HRESULT status = CachedCompile(kHdrInHlsl, sizeof(kHdrInHlsl) - 1, "lf-order.hlsl",
                            nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, &blob, &err);
    if (FAILED(status)) { Trace("[desk] channel-order shader did not compile: %s", err ? (char *)err->GetBufferPointer() : "?"); return false; }
    D3D12_ROOT_PARAMETER prm_desc[3] = {};
    D3D12_DESCRIPTOR_RANGE r0 = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0 };
    D3D12_DESCRIPTOR_RANGE r1 = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0 };
    prm_desc[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    prm_desc[0].DescriptorTable.NumDescriptorRanges = 1; prm_desc[0].DescriptorTable.pDescriptorRanges = &r0;
    prm_desc[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    prm_desc[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    prm_desc[1].DescriptorTable.NumDescriptorRanges = 1; prm_desc[1].DescriptorTable.pDescriptorRanges = &r1;
    prm_desc[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    prm_desc[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    prm_desc[2].Constants.Num32BitValues = 3;
    prm_desc[2].Constants.ShaderRegister = 0;
    root_desc.NumParameters = 3; root_desc.pParameters = prm_desc;
    ID3DBlob *sig = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    { Trace("[desk] channel-order root signature did not serialize"); return false; }
    if (FAILED(lfCore.device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                          __uuidof(ID3D12RootSignature),
                                          reinterpret_cast<void **>(&lfCap.swz_root))))
    { Trace("[desk] channel-order root signature refused"); return false; }
    sig->Release();
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = lfCap.swz_root;
    pd.CS.pShaderBytecode = blob->GetBufferPointer(); pd.CS.BytecodeLength = blob->GetBufferSize();
    if (FAILED(lfCore.device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState),
                                                 reinterpret_cast<void **>(&lfCap.swz_pso))))
    { Trace("[desk] channel-order pipeline refused"); return false; }
    blob->Release();
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 2; heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(lfCore.device->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap),
                                           reinterpret_cast<void **>(&lfCap.swz_heap))))
    { Trace("[desk] channel-order descriptor heap refused"); return false; }
    Trace("[desk] channel-order pipeline ready");
    return true;
}

// Point the channel-order pass at its source.
static void SwizzleBind(ID3D12Resource *from)
{
    const UINT step_bytes = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_at = lfCap.swz_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC view_desc = {};
    view_desc.Format = from->GetDesc().Format; view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view_desc.Texture2D.MipLevels = 1;
    lfCore.device->CreateShaderResourceView(from, &view_desc, cpu_at);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    cpu_at.ptr += step_bytes;
    lfCore.device->CreateUnorderedAccessView(lfCap.rgba, nullptr, &uav_desc, cpu_at);
}

// Run the channel-order pass from the bridge into the pipeline's picture.
static bool SwizzleIntoPicture(Pipeline &pipe)
{
    static ULONGLONG last_display_query = 0;
    if (GetTickCount64() - last_display_query > 1000 || last_display_query == 0)
    {
        lfCap.display = HdrDisplayOf(lfCap.monitor);
        last_display_query = GetTickCount64();
    }
    lfHdr.paper_white = lfCap.display.sdr_white;
    if (!CmdOpen()) return false;
    GpuStageOpen(GS_CONVERT);
    D3D12_RESOURCE_BARRIER to_srv = Barrier(lfCap.bridge12, D3D12_RESOURCE_STATE_COMMON,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    lfCore.cmd->ResourceBarrier(1, &to_srv);
    SwizzleBind(lfCap.bridge12);
    ID3D12DescriptorHeap *heap_list[] = { lfCap.swz_heap };
    lfCore.cmd->SetDescriptorHeaps(1, heap_list);
    lfCore.cmd->SetComputeRootSignature(lfCap.swz_root);
    lfCore.cmd->SetPipelineState(lfCap.swz_pso);
    D3D12_GPU_DESCRIPTOR_HANDLE g0 = lfCap.swz_heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE g1 = g0;
    g1.ptr += lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    lfCore.cmd->SetComputeRootDescriptorTable(0, g0);
    lfCore.cmd->SetComputeRootDescriptorTable(1, g1);

    struct { UINT is_float; float white; UINT rotate180; } hdr = {
        lfHdr.float_frames ? (lfHdr.keep ? 1u : 2u) : 0u, lfHdr.paper_white,
        (lfCap.live && lfCap.upside_down) ? 1u : 0u };
    lfCore.cmd->SetComputeRoot32BitConstants(2, 3, &hdr, 0);
    lfCore.cmd->Dispatch((lfCap.w + 7) / 8, (lfCap.h + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER pre_color = Barrier(pipe.picture.gpu_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                  D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_BARRIER to_copy = Barrier(lfCap.rgba, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_RESOURCE_BARRIER pre_c[2] = { to_copy, pre_color };
    lfCore.cmd->ResourceBarrier(2, pre_c);
    D3D12_TEXTURE_COPY_LOCATION from = {}, into = {};
    from.pResource = lfCap.rgba; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex = 0;
    into.pResource = pipe.picture.gpu_tex; into.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; into.SubresourceIndex = 0;

    {
        const D3D12_RESOURCE_DESC view_desc = lfCap.rgba->GetDesc();
        const D3D12_RESOURCE_DESC dd = pipe.picture.gpu_tex->GetDesc();
        const UINT pic_w = (UINT)((view_desc.Width < dd.Width) ? view_desc.Width : dd.Width);
        const UINT pic_h = (UINT)((view_desc.Height < dd.Height) ? view_desc.Height : dd.Height);
        if (pic_w != view_desc.Width || pic_h != view_desc.Height)
            Trace("[source] sizes differ, %llux%llu against %llux%llu - cropped",
                (unsigned long long)view_desc.Width, (unsigned long long)view_desc.Height,
                (unsigned long long)dd.Width, (unsigned long long)dd.Height);
        if (pic_w == 0 || pic_h == 0)
        { Trace("[source] nothing to copy - frame dropped"); return false; }
        // (Round 88l: SMSR 1.0 - the frame taken made twice as large into the picture, once a real frame: lf_smsr.inl.)
        ID3D12Resource *enlarged = nullptr;
        if (g_smsr_capture && dd.Width == 2 * view_desc.Width && dd.Height == 2 * view_desc.Height)
        {
            const RECT all = { 0, 0, LONG(view_desc.Width), LONG(view_desc.Height) };
            enlarged = SmsrRun(lfCore.cmd, lfCap.rgba, view_desc.Format, all);
        }
        if (enlarged) lfCore.cmd->CopyResource(pipe.picture.gpu_tex, enlarged);
        else
        {
            D3D12_BOX box = { 0, 0, 0, pic_w, pic_h, 1 };
            lfCore.cmd->CopyTextureRegion(&into, 0, 0, 0, &from, &box);
        }
    }
    D3D12_RESOURCE_BARRIER to_uav = Barrier(lfCap.rgba, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_BARRIER to_nps = Barrier(pipe.picture.gpu_tex, D3D12_RESOURCE_STATE_COPY_DEST,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    D3D12_RESOURCE_BARRIER to_common = Barrier(lfCap.bridge12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                  D3D12_RESOURCE_STATE_COMMON);
    D3D12_RESOURCE_BARRIER post_c[3] = { to_uav, to_nps, to_common };
    lfCore.cmd->ResourceBarrier(3, post_c);
    GpuStageClose(GS_CONVERT);
    // The luma came with the copy (Luma11): nothing of the conversion is read
    // back, so it is not waited for. Sent at once it follows the copy on the
    // card (or the pair before's work, when that is still running) and the
    // loop goes on to judge the frame - the generator's list comes after it
    // (lf_dlssg.inl, FgPipelined). Not pipelined, it waits in the open list
    // for the generator's work and the two are sent as one piece.
    if (g_capture_joined)
    {
        if (FgPipelined())
        {
            g_convert_mark = CmdSubmit();
            if (g_convert_mark == 0) return false;
        }
        else CmdCarry();
        lfCap.luma_ok = true;
        lfCap.has_frame = true;
        lfCap.reopen_tried = false;
        return true;
    }
    const UINT64 fence = CmdSubmit();
    if (FgRequested() && lfLuma.mapped)
    {
        // Luma has its own descriptors: conversion and luma may be queued
        // together without rewriting descriptors still in use. Its final
        // readback fence covers BOTH jobs, avoiding a GPU time-slice wait
        // between two dependent pieces of the same captured picture.
        const bool luma_ok = fence && LumaFromPicture();
        if (!luma_ok && !GpuStageWait(GS_CONVERT, fence, 10000)) return false;
        lfCap.luma_ok = luma_ok;
        if (lfRun.gpu_lost) return false;
        lfCap.has_frame = true;
        lfCap.reopen_tried = false;
        return true;
    }
    if (!GpuStageWait(GS_CONVERT, fence, 10000)) { Trace("[source] waited too long for the channel-order pass"); return false; }

    if (!LumaFromPicture()) {   }
    lfCap.luma_ok = lfLuma.mapped;
    if (lfRun.gpu_lost) return false;
    lfCap.has_frame = true;
    lfCap.reopen_tried = false;
    return true;
}
