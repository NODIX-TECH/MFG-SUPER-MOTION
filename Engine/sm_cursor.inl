// sm_cursor.inl - the pointer (round 43), as Lossless Scaling has it: Clip cursor, Adjust cursor speed, Hide cursor,
// Scale cursor. Each only while the game is in front and the picture is up; all of it put back the moment that ends,
// when the engine stops, and when it is stopped by a fault (CrashFilter, ConsoleStop).
//
//   Clip cursor     the pointer kept inside the picture (ClipCursor) - Windows lets go of it whenever another window
//                   comes in front, so it is asked for again while the game is.
//   Adjust speed    scaled onto the screen: the pointer's speed times the scale, so that it crosses the picture as it
//                   crossed the game's window (Windows' own speed, 1..20, for this session only - never saved).
//   Hide cursor     no pointer while it is over the picture: the system's pointers swapped for an empty one.
//   Scale cursor    scaled onto the screen: the system's pointers swapped for copies as large as the picture's scale.
//
// The two that change what Windows keeps for everyone (the pointers, the speed) leave a note in the engine's folder
// (cursor.state) until they are put back: should the engine be ended from outside, the app puts them back itself
// (MotionEngine.fs, restoreCursor).

namespace cursorfx
{
    static constexpr DWORD kIds[] = { 32512, 32513, 32514, 32515, 32516, 32642, 32643, 32644, 32645, 32646, 32648, 32649, 32650 };
    static bool clipped = false;
    static RECT clip_rect = {};
    static int replaced = 0;             // 0 the system's pointers, 1 empty ones, 2 scaled ones
    static double replaced_scale = 0.0;
    static int speed_before = 0;         // the speed before ours; 0 none set
    static int speed_set = 0;
    static ULONGLONG last_tick = 0;

    static std::wstring NotePath()
    {
        wchar_t path[MAX_PATH] = {};
        EngineDataPath(path);
        return std::wstring(path) + L"\\cursor.state";
    }

    // What is to be put back should the engine be ended: written while it is changed, gone when it is put back.
    static void Note()
    {
        const std::wstring path = NotePath();
        if (replaced == 0 && speed_set == 0) { DeleteFileW(path.c_str()); return; }
        FILE *file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"w") != 0 || !file) return;
        fprintf(file, "cursors=%d\nspeed=%d\n", replaced != 0 ? 1 : 0, speed_set != 0 ? speed_before : 0);
        fclose(file);
    }

    static void PutPointersBack()
    {
        if (replaced == 0) return;
        SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
        replaced = 0;
        replaced_scale = 0.0;
        Note();
    }

    static void PutSpeedBack()
    {
        if (speed_set == 0) return;
        SystemParametersInfoW(SPI_SETMOUSESPEED, 0, reinterpret_cast<void *>(static_cast<INT_PTR>(speed_before)), 0);
        speed_set = 0;
        Note();
    }

    static void LetGo()
    {
        if (!clipped) return;
        RECT now = {};
        // (Only our own clip is let go: a game that holds the pointer itself keeps it.)
        if (GetClipCursor(&now) && EqualRect(&now, &clip_rect)) ClipCursor(nullptr);
        clipped = false;
    }

    static void HidePointers()
    {
        if (replaced == 1) return;
        PutPointersBack();
        BYTE and_mask[32 * 4 * 32], xor_mask[32 * 4 * 32];
        memset(and_mask, 0xFF, sizeof(and_mask));
        memset(xor_mask, 0x00, sizeof(xor_mask));
        for (const DWORD id : kIds)
            if (HCURSOR empty = CreateCursor(GetModuleHandleW(nullptr), 0, 0, 32, 32, and_mask, xor_mask)) SetSystemCursor(empty, id);
        replaced = 1;
        Note();
        Trace("[cursor] hidden over the picture");
    }

    static void ScalePointers(double scale)
    {
        if (replaced == 2 && std::fabs(replaced_scale - scale) < 0.05) return;
        PutPointersBack();
        const int base = GetSystemMetrics(SM_CXCURSOR) > 0 ? GetSystemMetrics(SM_CXCURSOR) : 32;
        const int size = std::clamp(static_cast<int>(std::lround(base * scale)), base, 256);
        for (const DWORD id : kIds)
        {
            HCURSOR system = LoadCursorW(nullptr, MAKEINTRESOURCEW(id));
            if (!system) continue;
            if (HCURSOR large = static_cast<HCURSOR>(CopyImage(system, IMAGE_CURSOR, size, size, 0))) SetSystemCursor(large, id);
        }
        replaced = 2;
        replaced_scale = scale;
        Note();
        Trace("[cursor] drawn at %d pixels (x%.2f, the picture's scale)", size, scale);
    }

    // All of it put back: the engine stops, or is stopped by a fault.
    static void Restore()
    {
        LetGo();
        PutPointersBack();
        PutSpeedBack();
    }

    // Once a loop turn; does something at most every 50 ms.
    static void Tick()
    {
        const ULONGLONG now = GetTickCount64();
        if (now - last_tick < 50) return;
        last_tick = now;
        const bool any = smOpt.cursor_clip.load() || smOpt.cursor_speed.load() || smOpt.cursor_hide.load() || smOpt.cursor_scale.load();
        if (!any && !clipped && replaced == 0 && speed_set == 0) return;
        // The game in front, the picture up.
        bool active = lfCap.window_live && lfCap.window != nullptr && IsWindow(lfCap.window) && !IsIconic(lfCap.window) &&
                      lfShow.hwnd != nullptr && lfShow.visible;
        if (active)
        {
            DWORD game = 0, front = 0;
            GetWindowThreadProcessId(lfCap.window, &game);
            GetWindowThreadProcessId(GetForegroundWindow(), &front);
            active = game != 0 && game == front;
        }
        RECT picture = {};
        if (active && !GetWindowRect(lfShow.hwnd, &picture)) active = false;
        // Scaled onto the screen: how much larger the picture is than the game's window.
        double scale = 1.0;
        if (lfShow.upscaling)
        {
            std::lock_guard<std::mutex> hold(g_up.lock);
            const LONG sw = g_up.src.right - g_up.src.left, dw = g_up.dst.right - g_up.dst.left;
            if (g_up.placed && sw > 0 && dw > 0) scale = double(dw) / double(sw);
        }
        // Clip cursor.
        if (active && smOpt.cursor_clip.load())
        {
            RECT now_clip = {};
            if (!clipped || !GetClipCursor(&now_clip) || !EqualRect(&now_clip, &picture))
            {
                if (ClipCursor(&picture)) { if (!clipped) Trace("[cursor] kept inside the picture"); clipped = true; clip_rect = picture; }
            }
        }
        else LetGo();
        // Adjust cursor speed.
        if (active && smOpt.cursor_speed.load() && scale > 1.05)
        {
            if (speed_set == 0)
            {
                int before = 10;
                if (!SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &before, 0)) before = 10;
                speed_before = std::clamp(before, 1, 20);
            }
            const int want = std::clamp(static_cast<int>(std::lround(speed_before * scale)), 1, 20);
            if (want != speed_set &&
                SystemParametersInfoW(SPI_SETMOUSESPEED, 0, reinterpret_cast<void *>(static_cast<INT_PTR>(want)), 0))
            {
                if (speed_set == 0) Trace("[cursor] speed %d -> %d (x%.2f, the picture's scale)", speed_before, want, scale);
                speed_set = want;
                Note();
            }
        }
        else PutSpeedBack();
        // Hide cursor / Scale cursor: the pointer over the picture.
        POINT at = {};
        const bool over = active && GetCursorPos(&at) && PtInRect(&picture, at);
        if (over && smOpt.cursor_hide.load()) HidePointers();
        else if (over && smOpt.cursor_scale.load() && scale > 1.05) ScalePointers(scale);
        else PutPointersBack();
    }
}
