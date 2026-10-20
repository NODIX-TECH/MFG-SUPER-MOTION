// sm_control.inl - the engine and the app. No pipe protocol and no shared
// memory: text, one line each way.
//
//   app -> engine   the start file (--config <file>, "key=value" lines) and,
//                   while it runs, lines on stdin: "key=value" for a control
//                   that moved, "stop" to end. A closed stdin is a "stop" too:
//                   the app went away, and the game's window is put back.
//   engine -> app   lines on stdout, each beginning with '@':
//                     @state starting | picking | running | stopped
//                     @pick <seconds left>
//                     @source <hwnd hex> <width> <height> <dxgi|wgc> <borderless|scaled|window|screen>
//                     @fps <real frames a second> <all frames a second>
//                     @mfg <multiplier running> | @mfg off | @mfg unavailable <why>
//                     @vrr follows | fixed
//                     @aml off | learning <percent> | learned <percent> <the game's delay, ms>
//                     @error <code> <text>
//                   and, started with --cards only (nothing else is done then):
//                     @card <number for gpu=> <1: DLSS MFG can run on it> <MB> <name>
//                     @xess <that number> <1: XeSS MFG can run on it>
//                   and, started with --vrr only:
//                     @vrr-system <1 on | 0 off | -1 cannot tell> <why>
// Everything else the engine has to say goes to its log (lf_trace.inl).

static std::mutex g_status_lock;

static void Status(const char *format, ...)
{
    char text[1024];
    va_list args;
    va_start(args, format);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args);
    va_end(args);
    std::lock_guard<std::mutex> hold(g_status_lock);
    fputs(text, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

// Lines of the log that are news for the app too.
static void TraceNews(const char *text)
{
    if (strncmp(text, "[dlssg] unavailable:", 20) == 0) Status("@mfg unavailable%s", text + 20);
    else if (strncmp(text, "[vrr] the screen follows", 24) == 0) Status("@vrr follows");
    else if (strncmp(text, "[vrr] the screen stays", 22) == 0) Status("@vrr fixed");
    else if (strncmp(text, "[dlssg] multiplier x", 20) == 0) Status("@note %s", text + 8);
}

// What a changed value asks of the loop.
enum : unsigned
{
    kRedoNothing  = 0,
    kRedoSource   = 1,   // the source is opened again (HDR, capture API)
    kRedoPipeline = 2,   // the motion field's size or its source changed
    kRedoFg       = 4,   // the generator's feature is made again
    kRedoWindow   = 8,   // the output window is rebuilt (it notices by itself)
};

static bool KeyIs(const char *key, const char *name) { return _stricmp(key, name) == 0; }

// One "key=value". Unknown keys are passed over (an older engine under a
// newer app).
static unsigned ApplyOption(const char *key, const char *value)
{
    if (KeyIs(key, "generator"))
    {
        if (!g_generator_locked) g_generator = std::clamp(atoi(value), 0, 2);
        return 0;
    }
    // The lines Adaptive Motion Learning shows while it studies a game, in the app's language: aml_say0 .. 8.
    if (_strnicmp(key, "aml_say", 7) == 0 && key[7] >= '0' && key[7] <= '9' && (key[8] == 0 || (key[8] >= '0' && key[8] <= '9' && key[9] == 0)))
    {
        LearnText(atoi(key + 7), value);
        return 0;
    }
    const long long number = _strtoi64(value, nullptr, 0);
    auto set = [&](std::atomic<int> &slot, int low, int high) {
        const int next = static_cast<int>(std::clamp<long long>(number, low, high));
        const bool changed = slot.exchange(next) != next;
        return changed;
    };
    if (KeyIs(key, "mfg"))          { if (set(smOpt.mfg, 0, 1)) { Trace("[option] frame generation %s", number ? "on" : "off"); return kRedoWindow; } return 0; }
    if (KeyIs(key, "multiplier"))   { if (set(smOpt.multiplier, 2, 6)) Trace("[option] multiplier x%d", smOpt.multiplier.load()); return 0; }
    // The generator notices its new size by itself; a motion field given to
    // it (motion 0 and 1) is sized by the same share and is built again.
    if (KeyIs(key, "flow"))
    {
        if (!set(smOpt.flow, 10, 100)) return 0;
        if (smOpt.game.load() != kGameAsSet) return 0;      // the kind of game decides; kept for "as set"
        Trace("[option] flow %d%%", smOpt.flow.load());
        return smOpt.motion.load() == 2 ? kRedoNothing : kRedoPipeline;
    }
    if (KeyIs(key, "game"))
    {
        const int before = FlowShare();
        if (!set(smOpt.game, 0, kGameAsSet)) return 0;
        Trace("[option] game type: %s - frames in between made at %d%% of the picture", kGameKinds[smOpt.game.load()].name, FlowShare());
        return FlowShare() == before || smOpt.motion.load() == 2 ? kRedoNothing : kRedoPipeline;
    }
    if (KeyIs(key, "queue"))        { if (set(smOpt.queue, 0, 2)) Trace("[option] Queue target %d", smOpt.queue.load()); return 0; }
    if (KeyIs(key, "still"))        { if (set(smOpt.still, 0, 1)) { Trace("[option] still pixels %s", number ? "kept" : "left to the generator"); return kRedoFg; } return 0; }
    if (KeyIs(key, "screen_limit")) { if (set(smOpt.screen_limit, 0, 1)) Trace("[option] frames in between %s", number ? "fill what room the screen has" : "as the multiplier says, whatever the screen shows"); return 0; }
    if (KeyIs(key, "guard"))        { if (set(smOpt.guard, 0, 1)) Trace("[option] fast motion guard %s", number ? "on" : "off"); return 0; }
    if (KeyIs(key, "msm_search"))   { if (set(smOpt.msm_search, 0, 1)) Trace("[option] MSM FG's search: %s", number ? "the lighter one" : "in full"); return 0; }
    if (KeyIs(key, "aml"))          { if (set(smOpt.aml, 0, 1)) Trace("[option] Adaptive Motion Learning %s", number ? "on" : "off"); return 0; }
    // (What it changes - the size frames in between are made at - the loop sees for itself: sm_loop.inl.)
    if (KeyIs(key, "log"))          { set(smOpt.log, 0, 1); return 0; }
    if (KeyIs(key, "frame_log"))    { set(smOpt.frame_log, 0, 1); return 0; }   // (read before the log is opened: sm_engine.cpp)      // (read before the log is opened: sm_engine.cpp)
    if (KeyIs(key, "aml_flow"))     { if (set(smOpt.aml_flow, 0, 1)) Trace("[option] Adaptive Flow Scale %s", number ? "on" : "off"); return 0; }

    // (Read before the card is used: sm_engine.cpp hands it to the driver's profile of the engine, sm_dlssg_preset.inl.)
    if (KeyIs(key, "dlssg_preset")) { set(smOpt.dlssg_preset, 0, 3); return 0; }

    if (KeyIs(key, "aml_forget"))   { LearnForget(value); return 0; }       // a game by its name: what was learnt for it is deleted in the app
    if (KeyIs(key, "multi_display")) { if (set(smOpt.multi_display, 0, 1)) Trace("[option] multi-display mode %s", number ? "on - the picture stays while another screen's window is in front" : "off"); return 0; }
    {
        // Crop input: the source is opened again for its new size.
        std::atomic<int> *const sides[] = { &smOpt.crop_l, &smOpt.crop_t, &smOpt.crop_r, &smOpt.crop_b };
        static const char *const names[] = { "crop_l", "crop_t", "crop_r", "crop_b" };
        for (int i = 0; i < 4; ++i)
            if (KeyIs(key, names[i]))
            {
                if (!set(*sides[i], 0, 4000)) return 0;
                Trace("[option] crop input: %d, %d, %d, %d pixels left out (left, top, right, bottom)",
                      smOpt.crop_l.load(), smOpt.crop_t.load(), smOpt.crop_r.load(), smOpt.crop_b.load());
                return kRedoSource;
            }
    }
    if (KeyIs(key, "cursor_clip"))  { if (set(smOpt.cursor_clip, 0, 1)) Trace("[option] clip cursor %s", number ? "on" : "off"); return 0; }
    if (KeyIs(key, "cursor_speed")) { if (set(smOpt.cursor_speed, 0, 1)) Trace("[option] adjust cursor speed %s", number ? "on" : "off"); return 0; }
    if (KeyIs(key, "cursor_hide"))  { if (set(smOpt.cursor_hide, 0, 1)) Trace("[option] hide cursor %s", number ? "on" : "off"); return 0; }
    if (KeyIs(key, "cursor_scale")) { if (set(smOpt.cursor_scale, 0, 1)) Trace("[option] scale cursor %s", number ? "on" : "off"); return 0; }
    if (KeyIs(key, "game_fps"))
    {
        const int want = (number < 20 || number > 500) ? 0 : static_cast<int>(number);
        if (smOpt.game_fps.exchange(want) != want)
            Trace(want ? "[option] game FPS given: %d - the game's pace is no longer read from the frames" : "[option] game FPS: read from the frames", want);
        return 0;
    }
    if (KeyIs(key, "frame_limit"))
    {
        const int want = (number < 20 || number > 360) ? 0 : static_cast<int>(number);
        if (smOpt.frame_limit.exchange(want) != want)
            Trace(want ? "[limit] frame limit %d FPS" : "[limit] no frame limit", want);
        return 0;
    }
    if (KeyIs(key, "vrr"))          { if (set(smOpt.vrr, 0, 1)) Trace("[option] VRR %s", number ? "on" : "off"); return 0; }
    if (KeyIs(key, "sync"))
    {
        static const char *const names[] = { "off - a frame is shown the moment it is presented, tear lines and all", "default - the newest frame at each refresh",
                                             "VSync", "VSync, every second refresh", "VSync, every third refresh" };
        const bool tore = smOpt.sync.load() == 0;
        if (!set(smOpt.sync, 0, 4)) return 0;
        Trace("[option] sync mode: %s", names[smOpt.sync.load()]);
        return tore != (smOpt.sync.load() == 0) ? kRedoWindow : kRedoNothing;   // tearing is a property of the swap chain
    }
    if (KeyIs(key, "low_latency"))  { if (set(smOpt.low_latency, 0, 1)) { Trace("[option] Low Latency %s", number ? "on" : "off"); return kRedoWindow; } return 0; }
    if (KeyIs(key, "hdr"))          { if (set(smOpt.hdr, 0, 1)) { Trace("[option] HDR %s", number ? "on" : "off"); return kRedoSource; } return 0; }
    if (KeyIs(key, "gpu_class"))    { if (set(smOpt.gpu_class, 0, 1)) Trace("[option] GPU priority %s", number ? "realtime" : "normal"); return 0; }
    if (KeyIs(key, "counter"))      { set(smOpt.counter, 0, 1); return 0; }
    if (KeyIs(key, "spout"))        { if (set(smOpt.spout, 0, 1) && number) Trace("[option] Spout output on"); return 0; }
    if (KeyIs(key, "link"))         { set(smOpt.link, 0, 1); return 0; }
    if (KeyIs(key, "upscaler"))
    {
        static const char *const names[] = { "off", "sharpening only", "integer", "nearest neighbour", "Anime4K" };
        if (set(smOpt.upscaler, 0, 4)) Trace("[option] upscaler: %s", names[smOpt.upscaler.load()]);
        return 0;
    }
    if (KeyIs(key, "sharpness"))      { set(smOpt.sharpness, 0, 100); return 0; }
    if (KeyIs(key, "over_sharpness")) { set(smOpt.over_sharpness, 0, 100); return 0; }
    if (KeyIs(key, "capture_api"))  { if (set(smOpt.capture_api, 0, 1)) { Trace("[option] capture API %s", number ? "WGC" : "DXGI"); return kRedoSource; } return 0; }
    if (KeyIs(key, "motion"))
    {
        if (!set(smOpt.motion, 0, 2)) return 0;
        const int motion = smOpt.motion.load();
        Trace("[option] motion: %s", motion == 2 ? "the generator's own" : motion == 1 ? "the engine's GPU estimate" : "NVIDIA Optical Flow");
        return kRedoPipeline;
    }
    // Read at the start only: they decide what is taken.
    if (KeyIs(key, "source"))       { set(smOpt.source, 0, 1); return 0; }
    if (KeyIs(key, "window_mode"))  { set(smOpt.window_mode, 0, 2); return 0; }
    if (KeyIs(key, "monitor"))      { set(smOpt.monitor, 0, 16); return 0; }
    if (KeyIs(key, "delay"))        { set(smOpt.delay, 0, 30); return 0; }
    if (KeyIs(key, "gpu"))          { set(smOpt.gpu, -1, 63); return 0; }
    if (KeyIs(key, "hwnd"))         { smOpt.hwnd = static_cast<unsigned long long>(number); return 0; }
    if (KeyIs(key, "owner"))        { smOpt.owner = static_cast<unsigned long>(number); return 0; }
    return 0;
}

// A line "key=value" (blanks around both allowed; '#' starts a comment).
static unsigned ApplyLine(std::string line)
{
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line.erase(hash);
    const size_t eq = line.find('=');
    if (eq == std::string::npos) return 0;
    auto trim = [](std::string s) {
        const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
    if (key.empty() || value.empty()) return 0;
    return ApplyOption(key.c_str(), value.c_str());
}

static bool ReadStartFile(const wchar_t *path)
{
    FILE *file = _wfsopen(path, L"r", _SH_DENYNO);
    if (!file) return false;
    char line[512];
    while (fgets(line, sizeof(line), file)) ApplyLine(line);
    fclose(file);
    return true;
}

// ---- Lines from the app ---------------------------------------------------

static struct ControlState
{
    std::mutex lock;
    std::vector<std::string> lines;
    std::atomic<bool> stop{false};
    HANDLE stop_event = nullptr;    // Local\SuperMotionEngineStop: set by anyone who wants the engine gone
    HANDLE instance = nullptr;
} g_control;

static DWORD WINAPI ControlThread(LPVOID)
{
    char line[512];
    while (fgets(line, sizeof(line), stdin))
    {
        std::string text(line);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
        // A byte-order mark or other stray bytes some writers put in front
        // of their first line.
        size_t first = 0;
        while (first < text.size() && !isalnum(static_cast<unsigned char>(text[first]))) ++first;
        text.erase(0, first);
        if (text.empty()) continue;
        if (_stricmp(text.c_str(), "stop") == 0) break;
        std::lock_guard<std::mutex> hold(g_control.lock);
        g_control.lines.push_back(text);
    }
    g_control.stop = true;
    return 0;
}

// One engine at a time; a second one tells the first to stop and takes over.
static bool ControlStart(bool read_stdin)
{
    g_control.stop_event = CreateEventW(nullptr, TRUE, FALSE, L"Local\\SuperMotionEngineStop");
    for (int tries = 0; tries < 40; ++tries)
    {
        g_control.instance = CreateMutexW(nullptr, TRUE, L"Local\\SuperMotionEngine");
        if (g_control.instance != nullptr && GetLastError() != ERROR_ALREADY_EXISTS) break;
        if (g_control.instance) { CloseHandle(g_control.instance); g_control.instance = nullptr; }
        if (g_control.stop_event) SetEvent(g_control.stop_event);
        Sleep(100);
    }
    if (g_control.instance == nullptr) return false;
    if (g_control.stop_event) ResetEvent(g_control.stop_event);
    if (read_stdin)
    {
        const HANDLE thread = CreateThread(nullptr, 0, ControlThread, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return true;
}

static bool StopAsked()
{
    if (g_control.stop.load()) return true;
    if (g_control.stop_event && WaitForSingleObject(g_control.stop_event, 0) == WAIT_OBJECT_0) { g_control.stop = true; return true; }
    return false;
}

// The lines that came in since the last look, applied; what they ask of the
// loop comes back.
static unsigned ControlApply()
{
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> hold(g_control.lock);
        lines.swap(g_control.lines);
    }
    unsigned redo = 0;
    for (const std::string &line : lines) redo |= ApplyLine(line);
    return redo;
}
