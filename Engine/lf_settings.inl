// lf_settings.inl - the frame limit and the capture pacing it asks for, and
// the smallest size the motion field is worked out at.

// The frame limit: most frames a second shown, 0 for none.
static int ShownCap()
{
    const int limit = smOpt.frame_limit.load();
    return (limit < 20 || limit > 360) ? 0 : limit;
}

// The frame limit's capture side: wait until the next picture may be taken.
// Taking the picture only when its turn comes keeps it fresh - nothing waits
// in a queue - which is the latency idea behind Reflex and Anti-Lag 2, and
// leaves the card's spare time to the game.
// When the last new picture was taken (QPC ticks) - the loop says so.
static LONGLONG g_pace_taken = 0;
static void PaceTaken()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_pace_taken = now.QuadPart;
}

static void PaceCapture()
{
    static LARGE_INTEGER frequency = {}, next = {};
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    // DLSS MFG limits presentation separately. Dividing capture by x6 made a
    // 45-FPS cap capture at 7.5 FPS, starving motion estimation of real frames.
    const unsigned shown = 1u;
    int limit = ShownCap();
    // VRR keeps shown frames inside the screen's range, VSync at one a turn (lf_vrr.inl).
    const int vrr = shown == 1 ? PresentCap() : 0;
    if (vrr > 0 && (limit <= 0 || vrr < limit)) limit = vrr;
    // No limit set and the generator on: every frame of the game is taken as
    // it comes, and the generator fills what room the screen has left
    // (lf_dlssg.inl, FgPresent). Round 27 took real frames no faster than the
    // screen's rate divided by the multiplier, so every pair was shown whole -
    // x4 on 200 Hz took 48 frames a second of a game drawing 130.
    unsigned per_real = shown;
    bool from_last = false;
    if (limit <= 0) { next = now; return; }
    const LONGLONG step = static_cast<LONGLONG>(frequency.QuadPart * double(per_real) / double(limit));
    // The screen's own limit spaces the pictures taken, counted from the last
    // new one - not a fixed grid: against a grid a game a little slower than
    // it (60 FPS under a 64.7 grid at x3 on 200 Hz) lost every slot that came
    // up empty and was taken at 52.
    if (from_last)
    {
        next.QuadPart = g_pace_taken + step;
        if (now.QuadPart >= next.QuadPart || next.QuadPart - now.QuadPart > step) return;
    }
    else if (next.QuadPart == 0 || now.QuadPart - next.QuadPart > step) next = now;
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
    while (now.QuadPart < next.QuadPart)
    {
        const double left = double(next.QuadPart - now.QuadPart) / double(frequency.QuadPart);
        if (timer && left > .0006)
        {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>((left - .0004) * 1e7);
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 50);
            else Sleep(0);
        }
        else SwitchToThread();
        QueryPerformanceCounter(&now);
    }
    if (!from_last) next.QuadPart += step;
}

// Never let the working size fall under 256x144 (or the picture, if smaller).
static void ClampWorkSize(UINT src_w, UINT src_h, UINT &w, UINT &height)
{
    const UINT mw=std::min(256u,src_w), mh=std::min(144u,src_h);
    if (w>=mw && height>=mh) return;
    const double ratio=std::min(1.,std::max({double(mw)/src_w,double(mh)/src_h,double(w)/src_w,double(height)/src_h}));
    w=std::min(src_w,UINT(ceil(src_w*ratio/2))*2);
    height=std::min(src_h,UINT(ceil(src_h*ratio/2))*2);
}

// The motion field's size for a picture: the Flow slider's share of it, even,
// never under ClampWorkSize's floor and never over 3840x2160. Left to the
// generator there is nothing in it: the floor is enough.
static void WorkSizeFor(UINT pic_w, UINT pic_h, UINT &w, UINT &height)
{
    if (!MotionGiven()) { w = height = 0; ClampWorkSize(pic_w, pic_h, w, height); return; }
    const double share = std::clamp(FlowShare() / 100.0, 0.10, 1.0);
    if (share >= 1.0) { w = pic_w; height = pic_h; }
    else
    {
        w = std::max(64u, static_cast<UINT>(pic_w * share) / 2 * 2);
        height = std::max(64u, static_cast<UINT>(pic_h * share) / 2 * 2);
    }
    w = std::min(w, pic_w); height = std::min(height, pic_h);
    ClampWorkSize(pic_w, pic_h, w, height);
    if (w > 3840 || height > 2160)
    {
        const double k = std::min(3840.0 / w, 2160.0 / height);
        w = std::max(64u, static_cast<UINT>(w * k) / 2 * 2);
        height = std::max(64u, static_cast<UINT>(height * k) / 2 * 2);
    }
    w = std::min(w, pic_w); height = std::min(height, pic_h);
}
