// sm_source.inl - which window (or screen) the engine works on, and how that
// window is made to fill its screen.
//
// "START MOTION" is one button, like Lossless Scaling's: press it, click the
// game - it is taken. The window of another application that has been in
// front for half a second is the one the person clicked, and it is taken
// then; nobody waits for the count to run out. The count is only how long
// there is to click. When it runs out the window in front is taken - and,
// when at that very moment nothing can be read as being in front (a window on
// its way to the front, the taskbar still holding the focus), the last window
// that was in front during the count.
//
// A taken window fills its screen: borderless over it when the window lets
// itself be placed there and stays, so the game draws at the screen's own
// size; a window that puts its own frame or size back, or that Windows will
// not let be touched (a game running elevated), is left exactly as it was and
// its picture is scaled onto the whole screen instead (lf_window_fill.inl).
// Its style and placement are kept and put back when the engine stops.

// Windows of the shell itself: in front for a moment while a person is on the
// way to the game (the taskbar, Start, the Alt+Tab list), never a source.
static bool ShellClass(HWND hwnd)
{
    static const wchar_t *kShell[] = {
        L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Windows.UI.Core.CoreWindow",
        L"XamlExplorerHostIslandWindow", L"MultitaskingViewFrame", L"TaskSwitcherWnd", L"ForegroundStaging",
        L"NotifyIconOverflowWindow", L"TopLevelWindowForOverflowXamlIsland", L"Shell_InputSwitchTopLevelWindow",
        L"TaskListThumbnailWnd", L"#32768", L"tooltips_class32" };
    wchar_t cls[96] = {};
    if (!GetClassNameW(hwnd, cls, 96)) return false;
    for (const wchar_t *name : kShell) if (wcscmp(cls, name) == 0) return true;
    return false;
}

// Is a window the app's or the engine's own.
static bool OwnWindow(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId() || (pid != 0 && pid == smOpt.owner.load());
}

// A window a person can be looking at: shown, not minimised, not hidden by
// the compositor, not the shell's, and large enough to be a picture.
static bool CouldBeSource(HWND hwnd)
{
    if (!hwnd || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;
    if (ShellClass(hwnd)) return false;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    RECT client = {};
    if (!GetClientRect(hwnd, &client)) return false;
    return client.right >= 200 && client.bottom >= 150;
}

// The window in front, unless it is ours or the shell's. The window itself,
// whatever kind it is (round 31: a game whose window is owned by a hidden
// one, or is a "tool window" so as to keep off the taskbar, is a source too).
// Only a small window (a splash, a palette, a dialog) is passed over for its
// owner.
static HWND OtherAppInFront()
{
    const HWND hwnd = GetForegroundWindow();
    if (!hwnd || OwnWindow(hwnd)) return nullptr;
    if (CouldBeSource(hwnd)) return hwnd;
    const HWND owner = GetAncestor(hwnd, GA_ROOTOWNER);
    if (!owner || owner == hwnd || !CouldBeSource(owner) || OwnWindow(owner)) return nullptr;
    return owner;
}

static bool OursInFront()
{
    const HWND hwnd = GetForegroundWindow();
    return hwnd != nullptr && OwnWindow(hwnd);
}

// ---- Borderless over the screen, and back ------------------------------------

static const LONG_PTR kFrameStyle = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_MAXIMIZE;
static const LONG_PTR kEdgeStyle = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;

static struct BorderlessState
{
    HWND hwnd = nullptr;
    LONG_PTR style = 0, exstyle = 0;
    WINDOWPLACEMENT placement = {};
    DWORD process = 0, thread = 0;
    RECT over = {};          // the screen it was placed over
    int misses = 0;          // looks in a row that found it out of place
    ULONGLONG looked = 0;
} g_borderless;

// SetWindowLongPtr that knows whether it worked: the value before comes back,
// and zero is a failure only when the last error says so.
static bool SetLongChecked(HWND hwnd, int index, LONG_PTR value)
{
    SetLastError(0);
    const LONG_PTR before = SetWindowLongPtrW(hwnd, index, value);
    return before != 0 || GetLastError() == 0;
}

// No frame, over the rectangle. The style is read as it is now, so the same
// call puts it back after a game undid it.
static bool BorderlessPlace(HWND hwnd, const RECT &over)
{
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR exstyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (style & WS_MAXIMIZE)
    {
        ShowWindow(hwnd, SW_RESTORE);
        style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    }
    if ((style & kFrameStyle) && !SetLongChecked(hwnd, GWL_STYLE, style & ~kFrameStyle)) return false;
    if ((exstyle & kEdgeStyle) && !SetLongChecked(hwnd, GWL_EXSTYLE, exstyle & ~kEdgeStyle)) return false;
    return SetWindowPos(hwnd, nullptr, over.left, over.top, over.right - over.left, over.bottom - over.top,
                        SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE) != FALSE;
}

// Is the held window still without a frame and exactly over its screen.
static bool BorderlessInPlace()
{
    const BorderlessState &b = g_borderless;
    if (!b.hwnd || !IsWindow(b.hwnd)) return false;
    if (GetWindowLongPtrW(b.hwnd, GWL_STYLE) & (WS_CAPTION | WS_THICKFRAME)) return false;
    RECT r = {};
    return GetWindowRect(b.hwnd, &r) && EqualRect(&r, &b.over);
}

// Put the held window back as it was.
static void BorderlessRelease()
{
    BorderlessState &b = g_borderless;
    const HWND hwnd = b.hwnd;
    if (!hwnd) return;
    b.hwnd = nullptr;
    if (!IsWindow(hwnd)) return;
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
    if (pid != b.process || tid != b.thread) return;   // Windows recycled the handle; this is a different window.
    SetWindowLongPtrW(hwnd, GWL_STYLE, b.style);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, b.exstyle);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    SetWindowPlacement(hwnd, &b.placement);
    Trace("[source] window %p put back as it was", (void *)hwnd);
}

// Borderless over the screen `over` (physical pixels). False when Windows
// refuses (a game running elevated, say): the window is left as it was.
static bool BorderlessTake(HWND hwnd, const RECT &over)
{
    BorderlessRelease();
    if (!IsWindow(hwnd)) return false;
    BorderlessState &b = g_borderless;
    b.placement.length = sizeof(b.placement);
    if (!GetWindowPlacement(hwnd, &b.placement)) return false;
    b.style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    b.exstyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    b.thread = GetWindowThreadProcessId(hwnd, &b.process);
    b.hwnd = hwnd;
    b.over = over;
    b.misses = 0;
    b.looked = GetTickCount64();
    if (!BorderlessPlace(hwnd, over))
    {
        const DWORD problem = GetLastError();
        BorderlessRelease();
        Trace("[source] Windows refused to place window %p (error %lu)", (void *)hwnd, problem);
        return false;
    }
    Trace("[source] window %p borderless over %ldx%ld at (%ld,%ld)", (void *)hwnd,
          over.right - over.left, over.bottom - over.top, over.left, over.top);
    return true;
}

// Did the held window keep it? Looked at twice over a third of a second; a
// window that undid it is asked `patience` more times. False when it will not
// stay - the caller releases it.
static bool BorderlessSettle(int patience)
{
    BorderlessState &b = g_borderless;
    if (!b.hwnd) return false;
    for (int attempt = 0; attempt <= patience; ++attempt)
    {
        Sleep(150);
        const bool first = BorderlessInPlace();
        Sleep(200);
        if (first && BorderlessInPlace()) return true;
        if (attempt < patience)
        {
            Trace("[source] window %p put its own frame or size back - asked once more", (void *)b.hwnd);
            if (!BorderlessPlace(b.hwnd, b.over)) return false;
        }
    }
    return false;
}

// Once a second: a window made borderless that no longer is - the game put
// its own frame or size back later on (a change of its video settings, say).
// Asked once more; true when the window will not stay and has been released -
// the caller then takes it again, scaled.
static bool BorderlessLost()
{
    BorderlessState &b = g_borderless;
    if (!b.hwnd) return false;
    const ULONGLONG now = GetTickCount64();
    if (now - b.looked < 1000) return false;
    b.looked = now;
    if (!IsWindow(b.hwnd)) return false;
    if (BorderlessInPlace() || IsIconic(b.hwnd)) { b.misses = 0; return false; }
    if (++b.misses < 2) return false;          // seen once: it may be on its way somewhere
    b.misses = 0;
    if (BorderlessPlace(b.hwnd, b.over) && BorderlessSettle(0))
    {
        Trace("[source] window %p placed over the screen again", (void *)b.hwnd);
        return false;
    }
    const HWND hwnd = b.hwnd;
    BorderlessRelease();
    Trace("[source] window %p keeps its own size - its picture is scaled onto the screen", (void *)hwnd);
    return true;
}

// ---- Screens ---------------------------------------------------------------

static bool MonitorRect(HMONITOR monitor, RECT &rect)
{
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return false;
    rect = info.rcMonitor;
    return true;
}

// \\.\DISPLAYn, or null when no such screen is connected.
static HMONITOR MonitorByNumber(int number)
{
    if (number <= 0) return nullptr;
    wchar_t name[32] = {};
    swprintf_s(name, L"\\\\.\\DISPLAY%d", number);
    return MonitorByName(name);
}

// ---- Taking a window -------------------------------------------------------

// The count: who is in front, until someone has been for half a second or the
// time is up. `cancelled` is asked ten times a second; the app is told each
// second that goes by. Null when nobody was there to take.
static HWND PickWindow(int seconds, bool (*cancelled)())
{
    const double kClickSettles = 0.5;
    const double start = LatencyNow(), deadline = start + std::clamp(seconds, 0, 30);
    HWND seen = nullptr;
    double seen_at = 0.0;
    int told = -1;
    Trace("[source] select an application within %d seconds", seconds);
    for (;;)
    {
        if (cancelled && cancelled()) return nullptr;
        const double now = LatencyNow();
        const HWND front = OtherAppInFront();
        if (front && front != seen) { seen = front; seen_at = now; }
        if (front && now - seen_at >= kClickSettles) return front;
        if (now >= deadline)
        {
            if (front) return front;
            if (seen && IsWindow(seen) && IsWindowVisible(seen) && !IsIconic(seen) && !OursInFront()) return seen;
            return nullptr;
        }
        const int left = static_cast<int>(std::ceil(deadline - now));
        if (left != told) { told = left; Status("@pick %d", left); }
        Sleep(50);
    }
}
