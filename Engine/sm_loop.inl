// sm_loop.inl - the engine's life: take the source the person picked, build
// the picture's textures and the output window for it, then for every frame
// of the game take it, find its motion, and hand it to the generator - until
// the app says stop or the game's window is gone.
//
// One thread does all of it but the presenting (the generator's presenter,
// lf_dlssg.inl) and the output window's messages (lf_show.inl).

struct Session
{
    Pipeline pipe;
    HWND window = nullptr;        // the taken window; null: a whole screen
    HMONITOR monitor = nullptr;   // the screen it is on
    bool fill = false;            // the window keeps its size, its picture is scaled onto the screen
    bool dlss5 = false;           // frames from DLSS 5 MANAGER's LIVE FLOW (sm_dlss5_link.inl)
    UINT pic_w = 0, pic_h = 0;    // the source's size everything was built for
    bool built = false;
    uint32_t frame = 0;
    bool generated_before = false;
    ULONGLONG last_fresh_tick = 0;
    bool stall_pending = false;
    int show_failures = 0, source_failures = 0;
    // What the app was last told.
    int told_mfg = -2, told_base = -2, told_shown = -2;
    ULONGLONG told_at = 0;
};

// The small luma's size for a picture (320 wide at most, even).
static void LumaSizeFor(UINT pic_w, UINT pic_h, UINT &w, UINT &h)
{
    const double factor = std::min(1.0, 320.0 / pic_w);
    w = std::max(64u, static_cast<UINT>(std::lround(pic_w * factor / 2.0)) * 2u);
    h = std::max(64u, static_cast<UINT>(std::lround(pic_h * factor / 2.0)) * 2u);
}

// Open the source: the window (Desktop Duplication while it is its whole
// screen, Graphics Capture otherwise or when WGC is picked), or its screen.
static bool SourceOpen(Session &s)
{
    if (s.window != nullptr && s.dlss5)
    {
        WindowStop();
        FillSet(s.window, s.fill);
        return Dlss5LinkStart(s.window);
    }
    if (s.window != nullptr)
    {
        FillSet(s.window, s.fill);
        return WindowStart(s.window) && lfCap.window_live;
    }
    RECT rect = {};
    if (!MonitorRect(s.monitor, rect)) return false;
    g_desk_monitor = s.monitor;
    return DeskStart(static_cast<UINT>(rect.right - rect.left), static_cast<UINT>(rect.bottom - rect.top)) && lfCap.live;
}

static void SourceTell(const Session &s)
{
    const bool dxgi = lfCap.live && !lfCap.screen_via_wgc && (lfCap.window_dxgi || !lfCap.window_live);
    Status("@source %llX %u %u %s %s", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(s.window)),
           lfCap.w, lfCap.h, s.dlss5 ? "dlss5" : dxgi ? "dxgi" : "wgc",
           s.window == nullptr ? "screen" : (s.fill ? "scaled" : (g_borderless.hwnd == s.window ? "borderless" : "window")));
    LearnGame(s.window, lfCap.w, lfCap.h);
}

// Everything that depends on the source's size: the picture and the motion
// field, the output window, the small luma.
static bool SessionBuild(Session &s)
{
    s.pic_w = lfCap.w; s.pic_h = lfCap.h;
    if (s.pic_w < 64 || s.pic_h < 64 || s.pic_w > 7680 || s.pic_h > 4320)
    { Trace("[pipe] a %ux%u source makes no sense", s.pic_w, s.pic_h); return false; }
    UINT work_w = 0, work_h = 0;
    WorkSizeFor(s.pic_w, s.pic_h, work_w, work_h);
    const bool stretched = work_w != s.pic_w || work_h != s.pic_h;
    if (!PipelineBuild(s.pipe, work_w, work_h, stretched ? s.pic_w : 0, stretched ? s.pic_h : 0))
    { Trace("[pipe] the working textures did not come up"); return false; }
    RECT rect = {};
    if (MonitorRect(s.monitor, rect)) { g_show_origin.x = rect.left; g_show_origin.y = rect.top; }
    if (!ShowStart(s.pic_w, s.pic_h, 0)) return false;
    UINT luma_w = 0, luma_h = 0;
    LumaSizeFor(s.pic_w, s.pic_h, luma_w, luma_h);
    if (!LumaStart(luma_w, luma_h)) Trace("[luma] no small luma - scene cuts and repeated frames go unnoticed");
    lfOut.force_next = true;
    s.built = true;
    s.frame = 0;
    SourceTell(s);
    return true;
}

static void SessionRelease(Session &s)
{
    CmdCarryFlush();
    FgCollect();
    CmdDrain();
    if (!s.built) return;
    // The presenter is joined and the card is done with every texture before
    // any of them goes.
    CloseFgResources();
    AwaitMark(lfCore.timeline, lfCore.timeline_value, 5000);
    ShowStop();
    LumaStop();
    PipelineRelease(s.pipe);
    s.built = false;
}

// The source is another size now (the window was resized, the screen's mode
// changed, the game left its borderless size): everything is built again.
static bool SessionRebuild(Session &s, const char *why)
{
    CmdCarryFlush();
    FgCollect();
    CmdDrain();
    Trace("[pipe] building again: %s", why);
    SessionRelease(s);
    return SessionBuild(s);
}

// Once a second: the two rates and what the generator is doing.
static void SessionTell(Session &s)
{
    const ULONGLONG now = GetTickCount64();
    if (now - s.told_at < 1000) return;
    s.told_at = now;
    int base = -1, shown = -1;
    CounterRates(base, shown);
    if (base != s.told_base || shown != s.told_shown)
    {
        s.told_base = base; s.told_shown = shown;
        Status("@fps %d %d", base, shown);
    }
    const int mfg = smOpt.mfg.load() == 0 ? 0 : (g_fg.failed ? -1 : static_cast<int>(FgExtraFrames()) + 1);
    if (mfg != s.told_mfg)
    {
        s.told_mfg = mfg;
        if (mfg > 0) Status("@mfg %d", mfg);
        else if (mfg == 0) Status("@mfg off");
    }
}


// One frame. 0: go on; anything else ends the engine with that code.
static int FrameStep(Session &s)
{
    Pipeline &pipe = s.pipe;
    // A captured frame's conversion left open for the generator's work
    // (lf_gpu_core.inl, CmdCarry) is sent by itself when nothing came after it.
    struct Sent { ~Sent() { CmdCarryFlush(); } } sent;
    const bool generating = FgRequested();
    FgConfigure(static_cast<unsigned>(smOpt.multiplier.load() - 1));
    // The screenshot shortcuts hold the picture while Windows takes its shot
    // (capture_shortcuts.inl).
    if (g_snapshot_frozen && lfShow.hwnd) { Sleep(15); return 0; }

    const double t_frame = PhaseClock();
    const bool phase_on = PhasesOn();
    if (phase_on) lfProf.stamp = {};
    if (phase_on) ++lfPhase.requests;
    bool reset = false;
    UINT64 upload_done = 0;
    const ULONGLONG now_tick = GetTickCount64();
    if (s.last_fresh_tick == 0) s.last_fresh_tick = now_tick;

    // A source that closed under us (its screen changed mode, the window
    // went to another card) is opened again.
    if (!SourceLive())
    {
        if (!SourceOpen(s))
        {
            if (++s.source_failures >= 40) { Status("@error source The source cannot be captured"); return 10; }
            Sleep(50);
            return 0;
        }
        SourceTell(s);
    }
    s.source_failures = 0;

    const double t_dda = PhaseClock();
    PaceCapture();
    LowLatencyWait();
    const double stamp_before = lfCap.stamp;
    g_desk_accumulated = 0;
    const double asked_at = LatencyNow();
    const unsigned arrivals_before = g_cap_arrivals.load();
    bool got = lfCap.window_live ? WindowTake(pipe) : DeskTake(pipe);
    // (SM_TEST_FREEZE=<1|2>,<seconds>: the tests' own fault, for the watch below - from that second on every frame
    // is thrown away (1) or the capture hands over none (2), until the source is opened again. tests\freeze.ps1.)
    static int test_freeze = [] { char v[32] = {}; return GetEnvironmentVariableA("SM_TEST_FREEZE", v, sizeof(v)) > 0 ? atoi(v) : 0; }();
    static double test_freeze_at = [] { char v[32] = {}; if (GetEnvironmentVariableA("SM_TEST_FREEZE", v, sizeof(v)) == 0) return 0.0; const char *comma = strchr(v, ','); return comma ? atof(comma + 1) : 10.0; }();
    static double test_began = LatencyNow();
    if (test_freeze != 0 && LatencyNow() - test_began >= test_freeze_at)
    {
        static bool said = false;
        if (!said) { said = true; Trace("[test] the fault begins: %s", test_freeze == 1 ? "every frame is thrown away" : "the capture hands over nothing"); }
        if (test_freeze == 2) g_cap_arrivals = arrivals_before;
        got = false;
        lfCap.picture_changed = false;
    }
    const double got_at = LatencyNow();
    if (lfRun.gpu_lost) return 6;
    // Another size: nothing made for the old one is of use.
    if (SourceLive() && (lfCap.w != s.pic_w || lfCap.h != s.pic_h))
        return SessionRebuild(s, "the source has another size") ? 0 : 3;
    bool source_fresh = got && lfCap.picture_changed;
    // The picture before over again is nothing new (lf_own_motion.inl,
    // SourceSeen): it keeps the time stamp of the frame it repeats.
    const double change = source_fresh ? LumaChangeNow() : -1.0;
    bool repeated = false;
    // (Game FPS given: the same picture again is a repeat only within one frame of the game - later, the game is
    // standing still and it is its frame; a part that changed is a frame.)
    const double given = GivenFrameTime();
    const bool again = source_fresh && (given > 0.0 ? change == 0.0 && lfCap.stamp - stamp_before < 0.97 * given
                                                    : (change == 0.0 || OnlyACorner(change)));
    if (source_fresh) { SourceSeen(again); RepeatLearn(lfCap.stamp, again); }
    if (again)
    {
        // Its conversion, left open for the generator's work, is never sent.
        CmdCarryDrop();
        repeated = true;
        lfCap.stamp = stamp_before;
        lfCap.picture_changed = false;
        source_fresh = false;
        got = false;
    }
    if (source_fresh) { PaceTaken(); FgGateFresh(lfCap.stamp); }
    // Was this frame of the game there before the loop came for it? Then the
    // loop, not the game, sets the pace - and frames of the game go unseen.
    const bool counted = source_fresh && generating && lfCap.stamp > 0.0;
    const double idle = counted ? std::clamp(lfCap.stamp - asked_at, 0.0, got_at - asked_at) : 0.0;
    if (counted)
    {
        ++g_steady.taken;
        if (lfCap.stamp < asked_at - 0.0005) ++g_steady.waiting;
        g_steady.desktop += g_desk_accumulated;
    }
    // SM_DIAG=1: what the source handed over, once a second.
    {
        static const bool diag = [] { char v[4] = {}; return GetEnvironmentVariableA("SM_DIAG", v, sizeof(v)) > 0 && v[0] == '1'; }();
        static unsigned takes = 0, fresh = 0, repeats = 0;
        static double change_sum = 0.0, change_low = 1.0;
        static ULONGLONG since = 0;
        if (diag)
        {
            ++takes;
            if (source_fresh) { ++fresh; change_sum += change; if (change >= 0.0 && change < change_low) change_low = change; }
            if (repeated) ++repeats;
            if (now_tick - since >= 1000)
            {
                Trace("[diag] %u takes: %u new, %u repeats; change avg %.6f, least %.6f", takes, fresh, repeats,
                      fresh ? change_sum / fresh : 0.0, change_low);
                takes = fresh = repeats = 0; change_sum = 0.0; change_low = 1.0; since = now_tick;
            }
        }
    }
    if (phase_on && source_fresh) ++lfPhase.fresh_in;
    PhaseCount(PHASE_SOURCE, t_dda);
    if (source_fresh && SceneCutNow(change, MsmSelected() && LearnTurning())) reset = true;
    // Further than the generator follows: this pair is shown as the game drew it.
    if (source_fresh) g_fg_too_fast = GuardTooFast(reset || s.frame == 0);
    // And what the hand did meanwhile (Adaptive Motion Learning): learnt from, and the generator told.
    if (source_fresh && lfLuma.mapped && lfCap.luma_ok) LearnLooked(lfLuma.view, static_cast<int>(lfLuma.w), static_cast<int>(lfLuma.h), lfCap.stamp);
    if (source_fresh) LearnFrame(lfCap.stamp, reset || s.frame == 0, g_fg_prior.moved, RebuildPrior::kAcross * RebuildPrior::kDown);
    if (source_fresh && lfLuma.mapped && lfCap.luma_ok) LearnSeen(lfLuma.view, static_cast<int>(lfLuma.w), static_cast<int>(lfLuma.h));

    if (source_fresh)
    {
        if (s.stall_pending && now_tick - s.last_fresh_tick > 1000)
            Trace("[history] the source came back after %lu ms - the generator starts fresh",
                (unsigned long)(now_tick - s.last_fresh_tick));
        s.stall_pending = false;
        s.last_fresh_tick = now_tick;
    }
    else if (!got && now_tick - s.last_fresh_tick > 1000)
        s.stall_pending = true;

    // The watch (round 23). Users saw the output stand on one picture while the game went on behind it - after a
    // few seconds, after switching away and back, after a loading screen - until the engine was stopped and
    // started again with the shortcut. Not seen here: whatever stops the capture's frames from being used (the
    // capture itself gone quiet, its frames all taken for repeats or for not the game's), the cure is what the
    // shortcut does. So when nothing new has been used for a while with the game in front, the source is opened
    // again and what was learnt about repeats forgotten: after a second and a half when the capture still hands
    // over frames (they are being thrown away), after four when it hands over none (a game standing still sends
    // none - then it costs nothing but the opening); each time it changes nothing, twice as long before the next.
    {
        static ULONGLONG fresh_at = 0, reopened_at = 0;
        static unsigned arrivals_mark = 0, backoff = 0;
        if (fresh_at == 0) fresh_at = now_tick;
        if (source_fresh) { fresh_at = now_tick; arrivals_mark = g_cap_arrivals.load(); backoff = 0; }
        else if (SourceLive() && !g_snapshot_frozen && lfCap.has_frame)
        {
            const HWND window = lfCap.window_live ? lfCap.window : nullptr;
            bool in_front = true;
            if (window != nullptr)
            {
                DWORD source_pid = 0, front_pid = 0;
                GetWindowThreadProcessId(window, &source_pid);
                GetWindowThreadProcessId(GetForegroundWindow(), &front_pid);
                in_front = IsWindow(window) && !IsIconic(window) && (source_pid == front_pid || smOpt.multi_display.load());
            }
            const unsigned arrived = g_cap_arrivals.load() - arrivals_mark;
            const ULONGLONG wait = (arrived >= 20 ? 1500ull : 4000ull) << std::min(backoff, 5u);
            if (!in_front) fresh_at = now_tick;
            else if (now_tick - fresh_at >= wait && now_tick - reopened_at >= wait)
            {
                Trace("[watch] nothing new used for %.1f s with the game in front (%u frames of the capture came meanwhile) - the source is opened again",
                      (now_tick - fresh_at) / 1000.0, arrived);
                g_repeat_guess = RepeatGuess();
                FgGateForget();
                lfOut.force_next = true;
                reopened_at = now_tick;
                ++backoff;
                arrivals_mark = g_cap_arrivals.load();
                if (test_freeze != 0) { test_freeze = 0; Trace("[test] the fault ends with the source opened again"); }
                if (window != nullptr) WindowStart(window);
                else DeskStart(lfCap.w, lfCap.h);
                TimingResult(pipe, s.frame, false, "idle");
                return 0;
            }
        }
    }

    // Round 42: the capture has handed over nothing at all for two seconds while the output is up - the game gone
    // to exclusive full screen, its window covered or minimised, the capture itself dead. The output window is
    // taken away rather than left standing over the game with the last picture ("a black screen, it does not work
    // at all", users' logs end on "0 real + 0 generated" for half a minute). Nothing is lost by it: what it showed
    // and what is under it are the same picture; with the next frame of the game it is shown again (ShowRevealOnce).
    {
        static unsigned arrivals_seen = 0;
        static ULONGLONG arrived_at = 0;
        const unsigned arrivals = g_cap_arrivals.load();
        if (source_fresh) g_show_aside = false;
        if (arrivals != arrivals_seen || arrived_at == 0 || source_fresh) { arrivals_seen = arrivals; arrived_at = now_tick; }
        else if (now_tick - arrived_at >= 2000 && lfShow.hwnd != nullptr && lfShow.visible)
        {
            g_show_aside = true;
            ShowWindow(lfShow.hwnd, SW_HIDE);
            lfShow.visible = false;
            lfShow.revealed = false;
            Trace("[watch] the capture has handed over nothing for %.1f s - the output is taken away until the game's next frame",
                  (now_tick - arrived_at) / 1000.0);
        }
    }

    if (!got && !lfCap.has_frame)
    {
        TimingResult(pipe, s.frame, false, "idle");
        if (phase_on) ++lfPhase.idle;
        PhaseSummary();
        return 0;
    }
    if (!got)
    {
        // A picture is never worked on twice: the card goes back to the game
        // until a new frame arrives (lf_latency.inl). A repeat is passed over
        // whatever is on: shown again it would be handed over as the next
        // "new" frame at once.
        const bool skip = (generating || LowLatencyWanted() || repeated) && !lfOut.force_next;
        if (skip)
        {
            ++lfOut.still_count;
            if (!lfOut.still_logged && lfOut.still_count >= 8)
            {
                lfOut.still_logged = true;
                Trace("[idle] nothing new on screen - the generator rests until something moves");
            }
            WindowTrack();
            ShowStayOnTop();
            TimingResult(pipe, s.frame, false, "idle");
            if (phase_on) ++lfPhase.idle;
            PhaseSummary();
            return 0;
        }
    }
    else if (lfOut.still_count != 0)
    {
        if (lfOut.still_logged)
            Trace("[idle] the screen moved again after %u quiet frames", lfOut.still_count);
        lfOut.still_count = 0;
        lfOut.still_logged = false;
    }
    lfOut.force_next = false;

    // The motion is the generator's own business (sm_options.h, MotionGiven).
    // Asked for in the app it is found here, only while frames are made from
    // it: NVIDIA Optical Flow, and where that did not run the engine's GPU
    // estimate. DLSS MFG consumes it on this same GPU queue, so nothing waits
    // on the CPU here.
    if (generating && MotionGiven())
    {
        if (!s.generated_before) reset = true;      // no pair across the time it was off
        const double t_up = PhaseClock();
        // (XeSS MFG takes it as well, since round 16: the library is told the field whoever found it.)
        const bool try_nvofa = OfaWanted() && !ofa.failed && lfLuma.mapped;
        const bool nvofa_used = try_nvofa && OfaRun(pipe, reset, &upload_done);
        if (try_nvofa && !nvofa_used) reset = true;
        const bool up_ok = nvofa_used || NetMotionRun(pipe, reset, &upload_done);
        PhaseCount(PHASE_FEED, t_up);
        if (!up_ok) return lfRun.gpu_lost ? 6 : 0;
    }
    s.generated_before = generating;

    lfShow.history_restart = s.frame == 0 || reset;
    if (ShowDirect(pipe))
    {
        const double t_pres = PhaseClock();
        WindowTrack();
        ShowStayOnTop();
        const bool shown = ShowPassThrough(pipe);
        PhaseCount(PHASE_SHOW, t_pres);
        if (lfRun.gpu_lost) return 6;
        if (!shown)
        {
            // The output window is made again before giving up: a mode
            // change or a lost swap chain is passing.
            if (++s.show_failures >= 5) { Status("@error output The output window cannot be shown"); return 9; }
            Trace("[screen] the frame was not shown (%d) - new output window", s.show_failures);
            lfShow.stale = true;
            Sleep(30);
            return 0;
        }
        s.show_failures = 0;
    }
    if (counted) g_steady.busy_us += static_cast<unsigned>(std::clamp((LatencyNow() - asked_at - idle) * 1e6, 0.0, 1e6));
    PhaseCount(PHASE_WHOLE, t_frame);
    TimingResult(pipe, s.frame, source_fresh, generating ? "generated" : "shown");
    if (phase_on) ++lfPhase.processed;
    PhaseSummary();
    if (s.frame < 3 || ((s.frame + 1) % 1800) == 0)
        Trace("[pipe] heartbeat: frame %u out", s.frame + 1);
    ++s.frame;
    return 0;
}

// The engine, from the pick to the last frame.
static int EngineRun()
{
    Session s;
    Status("@state picking");
    // What is taken: a window named outright, or the one in front.
    HWND target = reinterpret_cast<HWND>(static_cast<uintptr_t>(smOpt.hwnd.load()));
    // DLSS 5 MANAGER's LIVE FLOW offers its frames: its window, no count.
    bool linked_fill = false;
    if (target == nullptr && Dlss5LinkOffered(&target, &linked_fill)) s.dlss5 = true;
    if (target == nullptr)
        target = PickWindow(smOpt.delay.load(), [] { ControlApply(); return StopAsked(); });
    if (StopAsked()) return 0;
    if (target == nullptr || !IsWindow(target))
    {
        Trace("[source] nothing to take: no game or application was in front");
        Status("@error nowindow Switch to your game during the countdown, then try again");
        return 2;
    }
    if (IsIconic(target)) { ShowWindow(target, SW_SHOWNOACTIVATE); Sleep(250); }
    s.monitor = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
    if (smOpt.source.load() == 1)
    {
        // The whole screen: the one picked, or the one that window is on.
        if (HMONITOR picked = MonitorByNumber(smOpt.monitor.load())) s.monitor = picked;
        Trace("[source] the whole screen is taken");
    }
    else
    {
        s.window = target;
        RECT over = {};
        const int mode = smOpt.window_mode.load();
        if (s.dlss5)
        {
            // LIVE FLOW placed the window already; it is left as it is.
            s.fill = linked_fill;
        }
        else if (mode == 0 && MonitorRect(s.monitor, over))
        {
            // Borderless over its screen; scaled when it will not stay.
            if (BorderlessTake(target, over))
            {
                if (!BorderlessSettle(1))
                {
                    BorderlessRelease();
                    s.fill = true;
                    Trace("[source] window %p keeps its own size - its picture is scaled onto the screen", (void *)target);
                }
            }
            else
            {
                s.fill = true;
                Trace("[source] window %p cannot be placed - its picture is scaled onto the screen", (void *)target);
            }
        }
        else s.fill = mode == 1;
        SetForegroundWindow(target);
    }
    Status("@state starting");
    // The multiplier is known before the output window is made for it.
    FgConfigure(static_cast<unsigned>(smOpt.multiplier.load() - 1));
    if (!SourceOpen(s))
    {
        Status("@error source The source cannot be captured");
        BorderlessRelease();
        return 10;
    }
    if (!SessionBuild(s))
    {
        Status("@error output The output could not be set up");
        WindowStop();
        BorderlessRelease();
        return 3;
    }
    Status("@state running");
    int code = 0;
    int mfg_before = smOpt.mfg.load();
    while (!StopAsked())
    {
        unsigned redo = ControlApply();
        cursorfx::Tick();         // Clip / speed / hide / scale cursor (sm_cursor.inl)
        // (The switch to the window's own frames where the compositor draws the output is gone: lf_own_motion.inl.)
        if (smOpt.mfg.load() != mfg_before)
        {
            // The output window's swap chain is made for the generator or
            // without it; a failure before is forgotten when it is asked again.
            mfg_before = smOpt.mfg.load();
            CloseFgResources();
            g_fg.failed = false;
            lfShow.stale = true;
            lfOut.force_next = true;
        }
        if (redo & kRedoFg) { CloseFgResources(); g_fg.failed = false; lfOut.force_next = true; }
        if (redo & kRedoSource)
        {
            Trace("[source] opening the source again for the new setting");
            if (!SourceOpen(s)) Trace("[source] the source did not open again - trying on");
            else SourceTell(s);
            lfOut.force_next = true;
        }
        // Adaptive Flow Scale picked another size for frames in between (sm_learn.inl), or a switch made its pick
        // count or not: the generator notices the size by itself; a motion field given to it is built again.
        {
            static int flow_was = FlowShare();
            const int flow_now = FlowShare();
            if (flow_now != flow_was)
            {
                flow_was = flow_now;
                if (smOpt.motion.load() != 2) redo |= kRedoPipeline;
            }
        }
        if ((redo & kRedoPipeline) && !SessionRebuild(s, "the motion field's size changed")) { code = 3; break; }
        if (s.window != nullptr)
        {
            if (!IsWindow(s.window))
            {
                Trace("[source] the source window closed");
                Status("@error closed The game's window closed");
                break;
            }
            if (s.dlss5 && Dlss5LinkLost())
            {
                Status("@error link DLSS 5 MANAGER stopped handing its frames over");
                break;
            }
            // The game put its own frame or size back: taken again, scaled.
            if (!s.dlss5 && BorderlessLost())
            {
                s.fill = true;
                if (!SourceOpen(s)) Trace("[source] the window could not be taken again");
                else SourceTell(s);
            }
        }
        code = FrameStep(s);
        if (code != 0) break;
        SessionTell(s);
        LearnTell();
    }
    LearnClose();
    TimingFlush();
    Trace("[pipe] stopping after %u frames (code %d)", s.frame, code);
    if (code == 6 || lfRun.gpu_lost) Status("@error device The graphics card stopped answering");
    SessionRelease(s);
    WindowStop();
    Dlss5LinkStop();
    BorderlessRelease();
    return code;
}
