// lf_counter.inl - the FPS counter, drawn by the engine itself.
//
// A counter that is a window of its own on top of the output makes the
// compositor blend the two for every frame shown instead of handing the
// output's frames straight to the screen - more delay and uneven frames
// exactly when a generator runs, which is when a person turns the counter on.
//
// Here it is drawn into the frame just before its Present, by the thread that
// presents - no window, no other process. It is two figures and nothing else,
//     95/380
// the game's own frames a second, and all frames shown a second: white with a
// thin dark edge, straight on the picture. They are counted where the frames
// are shown:
//     shown  every Present of the output
//     base   every real frame's first Present (a picture shown again, and a
//            frame in between, are not the game's)
// over the last second, in quarter-second steps. The text is drawn with GDI
// only when a figure changes - how much of each pixel the figures cover, and
// their edge - and a small pass blends it onto the back buffer in that
// buffer's own format (8-bit, HDR10 or scRGB).
//
// Whether it is shown is the "FPS counter" switch in the app; the app reads
// the same two figures once a second for its own status line (CounterRates).
//
// Round 79, "Advanced" (counter=2): below the figures, the frame time of what
// reached the SCREEN - each frame's moment there as the swap chain says it
// (its frame statistics: the Present it showed last and when), not when it was
// handed over - as a line over the last 160 frames, with its middle, its
// spread and its longest, and the game's own frame time. The user's word: the
// figures stay steady while the eye sees the frames come unevenly - a line of
// the Presents would have shown nothing (PresentMon: 0.1% of the Presents a
// quarter off the middle, 25% of the frames on the screen; FIX PACING.md).
// Redrawn twenty times a second at most.

static struct CounterState
{
    std::mutex lock;
    // What the manager asked for.
    bool on = false, hold = false;
    int dx = 12, dy = 12;
    // Counting.
    unsigned shown = 0, reals = 0;          // in the running quarter second
    unsigned shown_q[4] = {}, reals_q[4] = {};
    double span_q[4] = {};
    unsigned q = 0;
    double since = 0.0, last_source = 0.0;
    int base_fps = -1, shown_fps = -1;      // what is drawn
    // The drawing.
    HDC dc = nullptr;
    HBITMAP dib = nullptr;
    uint32_t *pixels = nullptr;
    HFONT font = nullptr;
    int scale_for = 0;
    ID3D12Resource *upload[2] = {};         // the figures as GDI left them: edge and fill, a byte each
    BYTE *mapped[2] = {};
    unsigned front = 0;
    ID3D12Resource *glyphs = nullptr;       // the same on the card
    ID3D12RootSignature *root = nullptr;
    ID3D12PipelineState *blend = nullptr;   // made for `format`
    ID3D12DescriptorHeap *views = nullptr, *target = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT w = 0, h = 0, pitch = 0;           // the drawn box; pitch: a row of it in the back buffer's format (CounterProof)
    bool drawn = false, failed = false;
    int drawn_base = -2, drawn_shown = -2, drawn_mode = -2;
    // Round 79, Advanced: the last frames' times on the screen (ms), the swap chain's last word on what it showed.
    bool advanced = false;
    static constexpr unsigned kGaps = 160;
    float gaps[kGaps] = {};
    unsigned gap_n = 0, gap_at = 0;
    UINT stat_count = 0, stat_refresh = 0;
    double stat_at = 0.0, last_present = 0.0, held = 0.0;
    bool stat_ok = false, stat_exact = false;
    static constexpr unsigned kIds = 16;
    UINT ids[kIds] = {};
    bool id_repeat[kIds] = {};
    unsigned id_at = 0;
    ULONGLONG drawn_at = 0;
    HFONT small_font = nullptr;
    uint64_t drawn_key = 0;
    std::vector<uint32_t> rgba;
    HFONT console_fps_font = nullptr;
    HFONT console_badge_font = nullptr;
    HFONT console_meta_font = nullptr;
    int console_font_scale = 0;
} g_counter;

static constexpr UINT kCounterMaxW = 640, kCounterMaxH = 320;


// The switch in the app (the corner offset is the counter's own): 0 off, 1 the figures, 2 with the frame time line.
static void CounterLook()
{
    CounterState &c = g_counter;
    const int mode = smOpt.counter.load();
    const bool on = mode != 0, advanced = mode == 2;
    if (on != c.on || advanced != c.advanced)
        Trace("[counter] the FPS counter is %s (drawn by the engine, in the frame)", !on ? "off" : advanced ? "on, with the frame time line" : "on");
    c.on = on;
    c.advanced = advanced;
    c.hold = false;
}

// Round 79: after a Present, the frame time on the screen - the swap chain's own word on the frame it showed last
// and when; the Presents' own times only where it says nothing. Under c.lock.
static void CounterScreenTime(double now, bool repeat)
{
    CounterState &c = g_counter;
    // This Present's own number, and whether it shows a picture again (round 80): the statistics come one or two
    // Presents behind, and what each tells of is looked up by its number - taken from the Present being made, a
    // "once more" fell on the wrong frame and the line showed spikes of 60 ms where PresentMon saw 5.00 every time.
    UINT id = 0;
    if (lfShow.chain && SUCCEEDED(lfShow.chain->GetLastPresentCount(&id)))
    {
        c.ids[c.id_at] = id;
        c.id_repeat[c.id_at] = repeat;
        c.id_at = (c.id_at + 1) % CounterState::kIds;
    }
    auto repeated = [&](UINT which) {
        for (unsigned i = 0; i < CounterState::kIds; ++i) if (c.ids[i] == which && which != 0) return c.id_repeat[i];
        return false;
    };
    DXGI_FRAME_STATISTICS st = {};
    if (!lfShow.chain || FAILED(lfShow.chain->GetFrameStatistics(&st)) || st.SyncQPCTime.QuadPart == 0) return;
    if (st.PresentCount == c.stat_count) return;
    static LARGE_INTEGER frequency = {};
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    // SyncQPCTime is the refresh the statistics were taken at, not the one the frame came on (read right after a
    // Present that mixed 5 and 10 ms where PresentMon saw 5.00 every time): the frame's own refresh is
    // PresentRefreshCount. On a fixed refresh that count is the time; with VRR the time is only known where the
    // statistics were taken at the frame's own refresh.
    const bool exact = st.SyncRefreshCount == st.PresentRefreshCount;
    const double at = double(st.SyncQPCTime.QuadPart) / double(frequency.QuadPart);
    // The frame before this one was on the screen from its refresh to this one's.
    double gap = -1.0;
    if (c.stat_ok && st.PresentCount == c.stat_count + 1)
    {
        const double refresh = OutputRefresh();
        if (!lfShow.vrr && refresh >= 24.0 && st.PresentRefreshCount > c.stat_refresh)
            gap = (st.PresentRefreshCount - c.stat_refresh) * 1000.0 / refresh;
        else if (lfShow.vrr && exact && c.stat_exact && at > c.stat_at) gap = (at - c.stat_at) * 1000.0;
    }
    c.stat_count = st.PresentCount;
    c.stat_refresh = st.PresentRefreshCount;
    c.stat_at = at;
    c.stat_exact = exact;
    c.stat_ok = true;
    if (gap <= 0.0 || gap > 250.0) { c.held = 0.0; return; }
    // A frame is on the screen until a new picture comes: its time goes on while the one shown now is the same again.
    c.held += gap;
    if (repeated(st.PresentCount)) return;
    c.gaps[c.gap_at] = static_cast<float>(c.held);
    c.gap_at = (c.gap_at + 1) % CounterState::kGaps;
    if (c.gap_n < CounterState::kGaps) ++c.gap_n;
    c.held = 0.0;
}

// After every Present of the output (lf_vrr.inl). real: a real frame's first.
// Round 56, Adaptive MFG: a pair shown by the beat counts as a frame of the game whether or not its real frame
// falls on a beat (the counter read 15 to 33 for a game at 59 - most real frames lie between beats).
static void CounterGameFrame(double source)
{
    CounterState &c = g_counter;
    std::lock_guard<std::mutex> hold(c.lock);
    if (source > 0.0 && source != c.last_source) { c.last_source = source; ++c.reals; }
}

static void CounterPresented(double source, bool own_capture)
{
    CounterState &c = g_counter;
    std::lock_guard<std::mutex> hold(c.lock);
    const double now = LatencyNow();
    const double stamp = own_capture ? lfCap.stamp : source;
    const bool real = own_capture ? (stamp <= 0.0 || stamp != c.last_source) : (stamp > 0.0 && stamp != c.last_source);
    if (real) { c.last_source = stamp; ++c.reals; }
    ++c.shown;
    if (c.on && c.advanced) CounterScreenTime(now, false);
    if (c.since == 0.0) c.since = now;
    if (now - c.since < 0.25) return;
    c.shown_q[c.q] = c.shown;
    c.reals_q[c.q] = c.reals;
    c.span_q[c.q] = now - c.since;
    c.q = (c.q + 1) % 4;
    c.shown = c.reals = 0;
    c.since = now;
    double span = 0.0;
    unsigned shown = 0, reals = 0;
    for (unsigned i = 0; i < 4; ++i) { span += c.span_q[i]; shown += c.shown_q[i]; reals += c.reals_q[i]; }
    // A gap (a still picture, a menu) is not a rate.
    if (span <= 0.0 || span > 3.0) { c.base_fps = c.shown_fps = -1; return; }
    c.base_fps = static_cast<int>(reals / span + 0.5);
    c.shown_fps = static_cast<int>(shown / span + 0.5);
}

// The two figures for the app's status line: real frames and all frames shown
// a second, -1 while there is no rate (a still picture).
static void CounterRates(int &base, int &shown)
{
    std::lock_guard<std::mutex> hold(g_counter.lock);
    base = g_counter.base_fps;
    shown = g_counter.shown_fps;
}

static constexpr UINT kCounterGlyphPitch = (kCounterMaxW * 4u + 255u) & ~255u;

static void CounterRelease()
{
    CounterState &c = g_counter;
    for (unsigned i = 0; i < 2; ++i)
    {
        if (c.upload[i]) { c.upload[i]->Unmap(0, nullptr); c.upload[i]->Release(); c.upload[i] = nullptr; }
        c.mapped[i] = nullptr;
    }
    if (c.glyphs) { c.glyphs->Release(); c.glyphs = nullptr; }
    if (c.blend) { c.blend->Release(); c.blend = nullptr; }
    if (c.views) { c.views->Release(); c.views = nullptr; }
    if (c.target) { c.target->Release(); c.target = nullptr; }
    c.format = DXGI_FORMAT_UNKNOWN;
    c.drawn = false;
    c.drawn_key = 0;
}

static UINT CounterBytes(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
}

// What GDI drew (the edge in red, the figures in green over it) into the
// upload buffer: premultiplied R8G8B8A8_UNORM matching the monochrome shader output.
static void CounterConvert(BYTE *to)
{
    CounterState &c = g_counter;
    for (UINT y = 0; y < c.h; ++y)
    {
        const uint32_t *from = c.pixels + static_cast<size_t>(y) * kCounterMaxW;
        BYTE *row = to + static_cast<size_t>(y) * kCounterGlyphPitch;
        for (UINT x = 0; x < c.w; ++x)
        {
            const unsigned ex = (from[x] >> 16) & 0xFFu;
            const unsigned gy = (from[x] >> 8) & 0xFFu;
            const BYTE lum = static_cast<BYTE>(gy);
            const BYTE alpha = static_cast<BYTE>(std::min<unsigned>(255u, (ex * 217u + 127u) / 255u + gy));
            row[x * 4] = lum;
            row[x * 4 + 1] = lum;
            row[x * 4 + 2] = lum;
            row[x * 4 + 3] = alpha;
        }
    }
}

static void CounterCopyRgba(BYTE *to)
{
    CounterState &c = g_counter;
    const size_t row_bytes = static_cast<size_t>(c.w) * 4u;
    for (UINT y = 0; y < c.h; ++y)
        memcpy(to + static_cast<size_t>(y) * kCounterGlyphPitch,
               c.rgba.data() + static_cast<size_t>(y) * kCounterMaxW, row_bytes);
}

static bool CounterEnsureDib()
{
    CounterState &c = g_counter;
    if (!c.dc)
    {
        c.dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info = {};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = kCounterMaxW;
        info.bmiHeader.biHeight = -static_cast<LONG>(kCounterMaxH);
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        void *bits = nullptr;
        c.dib = c.dc ? CreateDIBSection(c.dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
        c.pixels = static_cast<uint32_t *>(bits);
        if (!c.dib || !c.pixels) return false;
        SelectObject(c.dc, c.dib);
        SetBkMode(c.dc, TRANSPARENT);
    }
    return c.pixels != nullptr;
}

// The text, when a figure changed: "95/380", or the one figure there is
// without frames in between.
static bool CounterDraw(int base, int shown, bool both, UINT screen_h)
{
    CounterState &c = g_counter;
    if (!CounterEnsureDib()) return false;
    // 24 pixels tall on 1440 lines; other screens in proportion. (Round 85, GitHub 71: times the size asked in the app.)
    const int scale = std::clamp(static_cast<int>(screen_h * 100u / 1440u) * smOpt.counter_size.load() / 100, 35, 220);
    if (scale != c.scale_for)
    {
        if (c.font) DeleteObject(c.font);
        c.font = CreateFontW(-(24 * scale / 100), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
        c.scale_for = scale;
    }
    wchar_t text[40];
    if (base < 0) wcscpy_s(text, L"-");
    else if (both && shown >= 0) swprintf_s(text, L"%d/%d", base, shown);
    else swprintf_s(text, L"%d", base);
    const int length = static_cast<int>(wcslen(text));
    SelectObject(c.dc, c.font);
    SIZE size = {};
    GetTextExtentPoint32W(c.dc, text, length, &size);
    const int edge = std::max(1, 2 * scale / 100);
    const int w = std::min<int>((size.cx + 2 * edge + 3) & ~1, kCounterMaxW);
    const int h = std::min<int>((size.cy + 2 * edge + 1) & ~1, kCounterMaxH);
    RECT all = {0, 0, w, h};
    FillRect(c.dc, &all, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    // The edge: the figures once at every place around their own.
    SetTextColor(c.dc, RGB(255, 0, 0));
    for (int dy = -edge; dy <= edge; ++dy)
        for (int dx = -edge; dx <= edge; ++dx)
            if ((dx || dy) && dx * dx + dy * dy <= edge * edge + 1) TextOutW(c.dc, edge + dx, edge + dy, text, length);
    SetTextColor(c.dc, RGB(0, 255, 0));
    TextOutW(c.dc, edge, edge, text, length);
    GdiFlush();
    c.w = static_cast<UINT>(w);
    c.h = static_cast<UINT>(h);
    return true;
}

// Round 79, Advanced: the figures, a line of numbers and the frame time line on a dark box. GDI's red is how dark
// (the box at four fifths, the edges of the text in full), its green how white (text and the line in full,
// the line's middle dim). Under c.lock, after CounterDraw made the DC.
static bool CounterDrawAdvanced(int base, int shown, bool both, UINT screen_h)
{
    CounterState &c = g_counter;
    if (!CounterDraw(base, shown, both, screen_h)) return false;   // the DC, the big font, the figures' own box
    const int scale = c.scale_for;
    static int small_for = 0;
    if (!c.small_font || small_for != scale)
    {
        if (c.small_font) DeleteObject(c.small_font);
        c.small_font = CreateFontW(-(15 * scale / 100), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
        small_for = scale;
    }
    // The last frames in order, and what they say.
    float v[CounterState::kGaps];
    const unsigned n = c.gap_n;
    for (unsigned i = 0; i < n; ++i) v[i] = c.gaps[(c.gap_at + CounterState::kGaps - n + i) % CounterState::kGaps];
    double sum = 0.0, most = 0.0;
    for (unsigned i = 0; i < n; ++i) { sum += v[i]; most = std::max(most, double(v[i])); }
    const double mean = n ? sum / n : 0.0;
    double var = 0.0;
    for (unsigned i = 0; i < n; ++i) var += (v[i] - mean) * (v[i] - mean);
    const double sd = n ? std::sqrt(var / n) : 0.0;
    const int pad = std::max(4, 6 * scale / 100);
    const int w = std::min<int>(kCounterMaxW, (330 * scale / 100) & ~1);
    const int graph_h = std::min<int>(70 * scale / 100, kCounterMaxH / 2);
    // The figures (already drawn at the top left by CounterDraw), then the line of numbers.
    const int figures_h = static_cast<int>(c.h);
    wchar_t line[96];
    if (n >= 8)
        swprintf_s(line, L"%.2f ms  \x00B1%.2f  max %.1f   game %.1f ms", mean, sd, most, base > 0 ? 1000.0 / base : 0.0);
    else wcscpy_s(line, L"frame time: -");
    SelectObject(c.dc, c.small_font);
    SIZE size = {};
    GetTextExtentPoint32W(c.dc, line, static_cast<int>(wcslen(line)), &size);
    const int text_y = figures_h;
    const int graph_y = text_y + size.cy + pad / 2;
    const int h = std::min<int>((graph_y + graph_h + pad + 1) & ~1, kCounterMaxH);
    // The box, under the figures drawn already: only where nothing of theirs is.
    for (int y = 0; y < h; ++y)
    {
        uint32_t *row = c.pixels + static_cast<size_t>(y) * kCounterMaxW;
        for (int x = 0; x < w; ++x)
        {
            if (y < figures_h && x < static_cast<int>(c.w) && (row[x] & 0x00FFFF00u)) continue;
            row[x] = 0x00D00000u;
        }
    }
    // The numbers, with an edge.
    SetTextColor(c.dc, RGB(255, 0, 0));
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
            if (dx || dy) TextOutW(c.dc, pad + dx, text_y + dy, line, static_cast<int>(wcslen(line)));
    SetTextColor(c.dc, RGB(255, 255, 0));
    TextOutW(c.dc, pad, text_y, line, static_cast<int>(wcslen(line)));
    // The line: its top twice the middle, so an even line lies across the middle; the middle dim.
    if (n >= 2)
    {
        const double top = std::max(2.0 * mean, most * 1.05);
        const int gx = pad, gw = w - 2 * pad, gy = graph_y, gh = graph_h;
        HPEN dim = CreatePen(PS_SOLID, 1, RGB(208, 70, 0));
        HPEN bright = CreatePen(PS_SOLID, std::max(1, scale / 100), RGB(255, 255, 0));
        HGDIOBJ old = SelectObject(c.dc, dim);
        const int mid = gy + gh - static_cast<int>(gh * mean / top);
        MoveToEx(c.dc, gx, mid, nullptr); LineTo(c.dc, gx + gw, mid);
        SelectObject(c.dc, bright);
        for (unsigned i = 0; i < n; ++i)
        {
            const int x = gx + static_cast<int>((gw - 1) * double(i + CounterState::kGaps - n) / double(CounterState::kGaps - 1));
            const int y = gy + gh - static_cast<int>(gh * std::min(1.0, v[i] / top));
            if (i == 0) MoveToEx(c.dc, x, y, nullptr); else LineTo(c.dc, x, y);
        }
        SelectObject(c.dc, old);
        DeleteObject(dim);
        DeleteObject(bright);
    }
    GdiFlush();
    c.w = static_cast<UINT>(w);
    c.h = static_cast<UINT>(h);
    return true;
}

static inline uint32_t CounterPackRgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) |
           (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24);
}

static inline void CounterBlendOver(uint32_t &dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    if (a == 0) return;
    const unsigned sr = (static_cast<unsigned>(r) * a + 127u) / 255u;
    const unsigned sg = (static_cast<unsigned>(g) * a + 127u) / 255u;
    const unsigned sb = (static_cast<unsigned>(b) * a + 127u) / 255u;
    if (a == 255) { dst = CounterPackRgba(static_cast<uint8_t>(sr), static_cast<uint8_t>(sg), static_cast<uint8_t>(sb), 255); return; }
    const unsigned inv = 255u - a;
    const unsigned dr = dst & 0xFFu;
    const unsigned dg = (dst >> 8) & 0xFFu;
    const unsigned db = (dst >> 16) & 0xFFu;
    const unsigned da = (dst >> 24) & 0xFFu;
    const uint8_t or_ = static_cast<uint8_t>(std::min<unsigned>(255u, sr + (dr * inv + 127u) / 255u));
    const uint8_t og  = static_cast<uint8_t>(std::min<unsigned>(255u, sg + (dg * inv + 127u) / 255u));
    const uint8_t ob  = static_cast<uint8_t>(std::min<unsigned>(255u, sb + (db * inv + 127u) / 255u));
    const uint8_t oa  = static_cast<uint8_t>(std::min<unsigned>(255u, a  + (da * inv + 127u) / 255u));
    dst = CounterPackRgba(or_, og, ob, oa);
}

static inline float CounterRoundedSdf(float px, float py, float x, float y, float rw, float rh, float rad)
{
    const float hx = 0.5f * rw, hy = 0.5f * rh;
    const float qx = std::fabs(px - (x + hx)) - (hx - rad);
    const float qy = std::fabs(py - (y + hy)) - (hy - rad);
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - rad;
}

static void CounterFillRoundedRect(std::vector<uint32_t> &buf, int w, int h,
                                   int rx, int ry, int rw, int rh, float rad,
                                   uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    const int x0 = std::max(0, rx - 1), y0 = std::max(0, ry - 1);
    const int x1 = std::min(w, rx + rw + 1), y1 = std::min(h, ry + rh + 1);
    const float rr = std::min(rad, 0.5f * static_cast<float>(std::min(rw, rh)));
    for (int py = y0; py < y1; ++py)
    {
        uint32_t *row = buf.data() + static_cast<size_t>(py) * kCounterMaxW;
        for (int px = x0; px < x1; ++px)
        {
            const float d = CounterRoundedSdf(px + 0.5f, py + 0.5f, float(rx), float(ry), float(rw), float(rh), rr);
            const float cov = std::clamp(0.5f - d, 0.0f, 1.0f);
            if (cov > 0.004f) CounterBlendOver(row[px], r, g, b, static_cast<uint8_t>(a * cov + 0.5f));
        }
    }
}

static void CounterStrokeRoundedRect(std::vector<uint32_t> &buf, int w, int h,
                                     int rx, int ry, int rw, int rh, float rad, float stroke,
                                     uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    const int x0 = std::max(0, rx - 1), y0 = std::max(0, ry - 1);
    const int x1 = std::min(w, rx + rw + 1), y1 = std::min(h, ry + rh + 1);
    const float rr = std::min(rad, 0.5f * static_cast<float>(std::min(rw, rh)));
    const float half = 0.5f * stroke;
    for (int py = y0; py < y1; ++py)
    {
        uint32_t *row = buf.data() + static_cast<size_t>(py) * kCounterMaxW;
        for (int px = x0; px < x1; ++px)
        {
            const float d = CounterRoundedSdf(px + 0.5f, py + 0.5f, float(rx), float(ry), float(rw), float(rh), rr);
            const float cov = std::clamp(0.5f + half - std::fabs(d + half), 0.0f, 1.0f);
            if (cov > 0.004f) CounterBlendOver(row[px], r, g, b, static_cast<uint8_t>(a * cov + 0.5f));
        }
    }
}

static void CounterDrawTextColor(int x, int y, HFONT font, const wchar_t *text, int len,
                                 uint8_t r, uint8_t g, uint8_t b, uint8_t a, int max_w, int max_h)
{
    if (!text || len <= 0) return;
    CounterState &c = g_counter;
    SelectObject(c.dc, font);
    SIZE sz = {};
    GetTextExtentPoint32W(c.dc, text, len, &sz);
    const int tw = std::min<int>(sz.cx + 4, static_cast<int>(kCounterMaxW));
    const int th = std::min<int>(sz.cy + 2, static_cast<int>(kCounterMaxH));
    if (tw <= 0 || th <= 0) return;
    RECT clear_r = {0, 0, tw, th};
    FillRect(c.dc, &clear_r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    SetTextColor(c.dc, RGB(255, 255, 255));
    TextOutW(c.dc, 0, 0, text, len);
    GdiFlush();
    for (int py = 0; py < th; ++py)
    {
        const int dy = y + py;
        if (dy < 0 || dy >= max_h) continue;
        const uint32_t *src = c.pixels + static_cast<size_t>(py) * kCounterMaxW;
        uint32_t *dst = c.rgba.data() + static_cast<size_t>(dy) * kCounterMaxW;
        for (int px = 0; px < tw; ++px)
        {
            const int dx = x + px;
            if (dx < 0 || dx >= max_w) continue;
            const unsigned cov = (src[px] >> 8) & 0xFFu;
            if (cov == 0) continue;
            const uint8_t eff_a = static_cast<uint8_t>((static_cast<unsigned>(a) * cov + 127u) / 255u);
            CounterBlendOver(dst[dx], r, g, b, eff_a);
        }
    }
}

static void CounterDrawSegment(std::vector<uint32_t> &buf, int w, int h,
                               float x0, float y0, float x1, float y1, float thick,
                               uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    const float dx = x1 - x0, dy = y1 - y0;
    const float len2 = dx * dx + dy * dy;
    const float half = 0.5f * thick;
    const int bx0 = std::max(0, static_cast<int>(std::floor(std::min(x0, x1) - half - 1.5f)));
    const int bx1 = std::min(w, static_cast<int>(std::ceil(std::max(x0, x1) + half + 1.5f)));
    const int by0 = std::max(0, static_cast<int>(std::floor(std::min(y0, y1) - half - 1.5f)));
    const int by1 = std::min(h, static_cast<int>(std::ceil(std::max(y0, y1) + half + 1.5f)));
    for (int py = by0; py < by1; ++py)
    {
        uint32_t *row = buf.data() + static_cast<size_t>(py) * kCounterMaxW;
        const float fy = py + 0.5f;
        for (int px = bx0; px < bx1; ++px)
        {
            const float fx = px + 0.5f;
            const float t = len2 > 1e-6f ? std::clamp(((fx - x0) * dx + (fy - y0) * dy) / len2, 0.0f, 1.0f) : 0.0f;
            const float px_d = fx - (x0 + t * dx), py_d = fy - (y0 + t * dy);
            const float dist = std::sqrt(px_d * px_d + py_d * py_d) - half;
            const float cov = std::clamp(0.5f - dist, 0.0f, 1.0f);
            if (cov > 0.004f) CounterBlendOver(row[px], r, g, b, static_cast<uint8_t>(a * cov + 0.5f));
        }
    }
}

static void CounterFormatMfgBadge(wchar_t *out, size_t n, bool fg_on, int gen, int msm_ai, int mult, int ratio, int adaptive)
{
    if (!out || n == 0) return;
    if (!fg_on) { swprintf_s(out, n, L"FG OFF"); return; }
    const wchar_t *prefix = (gen == 0) ? L"DLSS MFG" : (gen == 1) ? L"XeSS MFG" : (msm_ai != 0 ? L"SMAI" : L"SMFG");
    if (adaptive > 1) swprintf_s(out, n, L"%s \x00B7 Adaptive %d", prefix, adaptive);
    else if (adaptive == 1) swprintf_s(out, n, L"%s \x00B7 Adaptive", prefix);
    else if (ratio > 0) swprintf_s(out, n, L"%s \x00B7 x%.1f", prefix, ratio / 10.0);
    else swprintf_s(out, n, L"%s \x00B7 x%d", prefix, std::clamp(mult, 2, 6));
}

static const wchar_t *CounterFormatUpscalerBadge(int upscaler)
{
    switch (upscaler)
    {
        case 1: return L"Sharp";
        case 2: return L"Integer";
        case 3: return L"Nearest";
        case 4: return L"Anime4K";
        case 5: return L"SMSR 1.0";
        case 6: return L"FSR 1.0";
        case 7: return L"DLSS 5";
        default: return L"Native";
    }
}

// Console Link modern HUD card (drawn only when stream_dedup != 0): shows base -> shown FPS,
// MFG generator + multiplier/adaptive target, active upscaler, manual lock, and optional frametime graph.
static bool CounterDrawConsole(int base, int shown, bool both, bool advanced, UINT screen_h)
{
    CounterState &c = g_counter;
    if (!CounterEnsureDib()) return false;
    if (c.rgba.size() < static_cast<size_t>(kCounterMaxW) * kCounterMaxH)
        c.rgba.assign(static_cast<size_t>(kCounterMaxW) * kCounterMaxH, 0u);

    const int scale = std::clamp(static_cast<int>(screen_h * 100u / 1440u) * smOpt.counter_size.load() / 100, 50, 170);
    if (scale != c.console_font_scale || !c.console_fps_font)
    {
        if (c.console_fps_font) DeleteObject(c.console_fps_font);
        if (c.console_badge_font) DeleteObject(c.console_badge_font);
        if (c.console_meta_font) DeleteObject(c.console_meta_font);
        c.console_fps_font = CreateFontW(-(25 * scale / 100), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
        c.console_badge_font = CreateFontW(-(13 * scale / 100), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
        c.console_meta_font = CreateFontW(-(12 * scale / 100), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
        c.console_font_scale = scale;
    }

    const int pad = std::max(7, 11 * scale / 100);
    const int row_gap = std::max(4, 7 * scale / 100);
    const int pill_px = std::max(6, 9 * scale / 100);
    const int pill_py = std::max(2, 4 * scale / 100);
    const int pill_gap = std::max(5, 7 * scale / 100);
    const int accent_w = std::max(2, 3 * scale / 100);
    const int accent_gap = std::max(5, 7 * scale / 100);

    wchar_t base_s[16] = {}, arrow_s[8] = L" \x2192 ", shown_s[16] = {}, fps_s[8] = L" FPS", lock_s[24] = {};
    if (base < 0) wcscpy_s(base_s, L"--");
    else swprintf_s(base_s, L"%d", base);
    if (both && shown >= 0 && base >= 0) swprintf_s(shown_s, L"%d", shown);

    const int lock_fps = smOpt.game_fps.load();
    const bool has_lock = (lock_fps >= 20 && lock_fps <= 500);
    if (has_lock) swprintf_s(lock_s, L"MAX %d", lock_fps);

    wchar_t mfg_s[64] = {};
    CounterFormatMfgBadge(mfg_s, _countof(mfg_s), both, g_generator, smOpt.msm_ai.load(),
                          smOpt.multiplier.load(), smOpt.ratio.load(), smOpt.adaptive.load());
    const int up_kind = smOpt.upscaler.load();
    const wchar_t *up_s = CounterFormatUpscalerBadge(up_kind);

    auto measure = [&](HFONT f, const wchar_t *txt) -> SIZE {
        SIZE sz = {};
        if (!txt || !*txt) return sz;
        SelectObject(c.dc, f);
        GetTextExtentPoint32W(c.dc, txt, static_cast<int>(wcslen(txt)), &sz);
        return sz;
    };

    const SIZE sz_base = measure(c.console_fps_font, base_s);
    const SIZE sz_arrow = (both && shown >= 0 && base >= 0) ? measure(c.console_fps_font, arrow_s) : SIZE{};
    const SIZE sz_shown = (both && shown >= 0 && base >= 0) ? measure(c.console_fps_font, shown_s) : SIZE{};
    const SIZE sz_fps = measure(c.console_meta_font, fps_s);
    const SIZE sz_lock = has_lock ? measure(c.console_meta_font, lock_s) : SIZE{};
    const SIZE sz_mfg = measure(c.console_badge_font, mfg_s);
    const SIZE sz_up = measure(c.console_badge_font, up_s);

    const int lock_pill_w = has_lock ? (sz_lock.cx + 2 * pill_px) : 0;
    const int lock_pill_h = has_lock ? (sz_lock.cy + 2 * pill_py) : 0;
    const int top_w = sz_base.cx + sz_arrow.cx + sz_shown.cx + sz_fps.cx + (has_lock ? pill_gap * 2 + lock_pill_w : 0);
    const int mfg_pill_w = sz_mfg.cx + 2 * pill_px;
    const int up_pill_w = sz_up.cx + 2 * pill_px;
    const int pill_h = std::max(sz_mfg.cy, sz_up.cy) + 2 * pill_py;
    const int badges_w = mfg_pill_w + pill_gap + up_pill_w;

    float v[CounterState::kGaps] = {};
    unsigned n = 0;
    double mean = 0.0, sd = 0.0, most = 0.0;
    wchar_t meta_s[96] = {};
    SIZE sz_meta = {};
    const int graph_h = advanced ? std::max(34, 54 * scale / 100) : 0;
    if (advanced)
    {
        n = c.gap_n;
        for (unsigned i = 0; i < n; ++i) v[i] = c.gaps[(c.gap_at + CounterState::kGaps - n + i) % CounterState::kGaps];
        double sum = 0.0;
        for (unsigned i = 0; i < n; ++i) { sum += v[i]; most = std::max(most, double(v[i])); }
        mean = n ? sum / n : 0.0;
        double var = 0.0;
        for (unsigned i = 0; i < n; ++i) var += (v[i] - mean) * (v[i] - mean);
        sd = n ? std::sqrt(var / n) : 0.0;
        if (n >= 8)
            swprintf_s(meta_s, L"%.2f ms  \x00B7  \x00B1%.2f  \x00B7  max %.1f  \x00B7  game %.1f ms",
                       mean, sd, most, base > 0 ? 1000.0 / base : 0.0);
        else
            wcscpy_s(meta_s, L"frametime: sampling\x2026");
        sz_meta = measure(c.console_meta_font, meta_s);
    }

    const int min_w = (advanced ? 344 : 254) * scale / 100;
    const int need_w = pad * 2 + accent_w + accent_gap + std::max({top_w, badges_w, static_cast<int>(sz_meta.cx)});
    const int w = std::min<int>(kCounterMaxW, (std::max(min_w, need_w) + 1) & ~1);
    const int top_h = sz_base.cy;
    int h = pad + top_h + row_gap + pill_h + pad;
    const int sep_y = pad + top_h + row_gap + pill_h + row_gap;
    const int meta_y = sep_y + 1 + row_gap;
    const int graph_y = meta_y + sz_meta.cy + std::max(3, row_gap / 2);
    if (advanced) h = graph_y + graph_h + pad;
    h = std::min<int>(kCounterMaxH, (h + 1) & ~1);

    for (int py = 0; py < h; ++py)
        memset(c.rgba.data() + static_cast<size_t>(py) * kCounterMaxW, 0, static_cast<size_t>(w) * sizeof(uint32_t));

    // Card background & crisp border
    const float rad = std::max(6.0f, 10.0f * scale / 100.0f);
    CounterFillRoundedRect(c.rgba, w, h, 0, 0, w, h, rad, 12, 16, 24, 214);
    CounterStrokeRoundedRect(c.rgba, w, h, 0, 0, w, h, rad, 1.0f, 255, 255, 255, 38);

    // Left vertical status stripe
    const int stripe_y = pad;
    const int stripe_h = h - 2 * pad;
    if (both) CounterFillRoundedRect(c.rgba, w, h, pad - 2, stripe_y, accent_w, stripe_h, 1.5f, 30, 155, 250, 235);
    else CounterFillRoundedRect(c.rgba, w, h, pad - 2, stripe_y, accent_w, stripe_h, 1.5f, 124, 135, 155, 200);

    const int cx = pad + accent_w + accent_gap;
    int tx = cx;
    const int ty = pad;
    if (both && shown >= 0 && base >= 0)
    {
        CounterDrawTextColor(tx, ty, c.console_fps_font, base_s, static_cast<int>(wcslen(base_s)), 190, 202, 222, 255, w, h);
        tx += sz_base.cx;
        CounterDrawTextColor(tx, ty, c.console_fps_font, arrow_s, static_cast<int>(wcslen(arrow_s)), 56, 189, 248, 255, w, h);
        tx += sz_arrow.cx;
        CounterDrawTextColor(tx, ty, c.console_fps_font, shown_s, static_cast<int>(wcslen(shown_s)), 92, 225, 230, 255, w, h);
        tx += sz_shown.cx;
    }
    else
    {
        CounterDrawTextColor(tx, ty, c.console_fps_font, base_s, static_cast<int>(wcslen(base_s)), 241, 245, 249, 255, w, h);
        tx += sz_base.cx;
    }
    const int fps_y = ty + std::max<int>(0, static_cast<int>(sz_base.cy - sz_fps.cy) - std::max(2, 4 * scale / 100));
    CounterDrawTextColor(tx, fps_y, c.console_meta_font, fps_s, static_cast<int>(wcslen(fps_s)), 126, 139, 160, 255, w, h);

    if (has_lock)
    {
        const int lx = w - pad - lock_pill_w;
        const int ly = ty + std::max(0, (top_h - lock_pill_h) / 2);
        const float pr = std::max(4.0f, 6.0f * scale / 100.0f);
        CounterFillRoundedRect(c.rgba, w, h, lx, ly, lock_pill_w, lock_pill_h, pr, 118, 185, 0, 52);
        CounterStrokeRoundedRect(c.rgba, w, h, lx, ly, lock_pill_w, lock_pill_h, pr, 1.0f, 163, 230, 53, 120);
        CounterDrawTextColor(lx + pill_px, ly + pill_py, c.console_meta_font, lock_s, static_cast<int>(wcslen(lock_s)), 217, 249, 157, 255, w, h);
    }

    // Badge pills: MFG generator+multiplier, and Active Upscaler
    const int by = ty + top_h + row_gap;
    const float pill_rad = std::max(4.0f, 6.0f * scale / 100.0f);
    if (both)
    {
        CounterFillRoundedRect(c.rgba, w, h, cx, by, mfg_pill_w, pill_h, pill_rad, 30, 155, 250, 48);
        CounterStrokeRoundedRect(c.rgba, w, h, cx, by, mfg_pill_w, pill_h, pill_rad, 1.0f, 56, 189, 248, 115);
        CounterDrawTextColor(cx + pill_px, by + pill_py, c.console_badge_font, mfg_s, static_cast<int>(wcslen(mfg_s)), 186, 230, 253, 255, w, h);
    }
    else
    {
        CounterFillRoundedRect(c.rgba, w, h, cx, by, mfg_pill_w, pill_h, pill_rad, 148, 163, 184, 28);
        CounterStrokeRoundedRect(c.rgba, w, h, cx, by, mfg_pill_w, pill_h, pill_rad, 1.0f, 148, 163, 184, 70);
        CounterDrawTextColor(cx + pill_px, by + pill_py, c.console_badge_font, mfg_s, static_cast<int>(wcslen(mfg_s)), 180, 192, 208, 255, w, h);
    }

    const int ux = cx + mfg_pill_w + pill_gap;
    if (up_kind == 7) // DLSS 5 neural accent
    {
        CounterFillRoundedRect(c.rgba, w, h, ux, by, up_pill_w, pill_h, pill_rad, 139, 92, 246, 56);
        CounterStrokeRoundedRect(c.rgba, w, h, ux, by, up_pill_w, pill_h, pill_rad, 1.0f, 196, 181, 253, 125);
        CounterDrawTextColor(ux + pill_px, by + pill_py, c.console_badge_font, up_s, static_cast<int>(wcslen(up_s)), 237, 233, 254, 255, w, h);
    }
    else if (up_kind != 0) // Active spatial/FSR/SMSR/Anime4K upscaler
    {
        CounterFillRoundedRect(c.rgba, w, h, ux, by, up_pill_w, pill_h, pill_rad, 16, 185, 129, 44);
        CounterStrokeRoundedRect(c.rgba, w, h, ux, by, up_pill_w, pill_h, pill_rad, 1.0f, 52, 211, 153, 110);
        CounterDrawTextColor(ux + pill_px, by + pill_py, c.console_badge_font, up_s, static_cast<int>(wcslen(up_s)), 167, 243, 208, 255, w, h);
    }
    else // Native
    {
        CounterFillRoundedRect(c.rgba, w, h, ux, by, up_pill_w, pill_h, pill_rad, 148, 163, 184, 26);
        CounterStrokeRoundedRect(c.rgba, w, h, ux, by, up_pill_w, pill_h, pill_rad, 1.0f, 148, 163, 184, 65);
        CounterDrawTextColor(ux + pill_px, by + pill_py, c.console_badge_font, up_s, static_cast<int>(wcslen(up_s)), 165, 178, 196, 255, w, h);
    }

    if (advanced)
    {
        CounterFillRoundedRect(c.rgba, w, h, cx, sep_y, w - cx - pad, 1, 0.0f, 255, 255, 255, 24);
        CounterDrawTextColor(cx, meta_y, c.console_meta_font, meta_s, static_cast<int>(wcslen(meta_s)), 203, 213, 225, 255, w, h);
        const int gw = w - cx - pad;
        const int gbox_h = std::max(16, h - pad - graph_y);
        const float grad = std::max(4.0f, 6.0f * scale / 100.0f);
        CounterFillRoundedRect(c.rgba, w, h, cx, graph_y, gw, gbox_h, grad, 7, 10, 16, 170);
        CounterStrokeRoundedRect(c.rgba, w, h, cx, graph_y, gw, gbox_h, grad, 1.0f, 255, 255, 255, 22);
        if (n >= 2)
        {
            const int inner_pad = 3;
            const int ix = cx + inner_pad, iw = std::max(4, gw - 2 * inner_pad);
            const int iy = graph_y + inner_pad, ih = std::max(6, gbox_h - 2 * inner_pad);
            const double top = std::max({2.0 * mean, most * 1.08, 1.0});
            const int mid_y = iy + ih - static_cast<int>(ih * std::min(1.0, mean / top));
            CounterDrawSegment(c.rgba, w, h, float(ix), float(mid_y), float(ix + iw - 1), float(mid_y), 1.0f, 148, 163, 184, 70);
            const float stroke_w = std::max(1.35f, 1.5f * scale / 100.0f);
            float prev_x = 0.0f, prev_y = 0.0f;
            for (unsigned i = 0; i < n; ++i)
            {
                const float x = ix + static_cast<float>((iw - 1) * double(i + CounterState::kGaps - n) / double(CounterState::kGaps - 1));
                const float y = iy + ih - static_cast<float>(ih * std::min(1.0, v[i] / top));
                if (i > 0)
                {
                    const bool spike = (v[i] > mean * 1.45 && v[i] - mean > 4.0);
                    CounterDrawSegment(c.rgba, w, h, prev_x, prev_y, x, y, stroke_w,
                                       spike ? 251 : 56, spike ? 191 : 189, spike ? 36 : 248, 245);
                }
                prev_x = x; prev_y = y;
            }
        }
    }

    c.w = static_cast<UINT>(w);
    c.h = static_cast<UINT>(h);
    return true;
}

static const char kCounterHlsl[] = R"hlsl(
Texture2D<float4> glyphs : register(t0);
cbuffer C : register(b0) { int2 origin; uint encoding; float white; };

float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    return float4(id == 1 ? 3.0 : -1.0, id == 2 ? -3.0 : 1.0, 0.0, 1.0);
}

// Premultiplied RGBA in the back buffer's own encoding:
// 0 8-bit, 1 HDR10 (white at 203 nits), 2 scRGB (`white` times 80 nits).
float4 PSMain(float4 at : SV_Position) : SV_Target
{
    float4 g = glyphs.Load(int3(int2(at.xy) - origin, 0));
    float level = 1.0;
    if (encoding == 1)
    {
        float n = pow(203.0 / 10000.0, 0.1593017578125);
        level = pow((0.8359375 + 18.8515625 * n) / (1.0 + 18.6875 * n), 78.84375);
    }
    else if (encoding == 2) level = white;
    return float4(level * g.rgb, g.a);
}
)hlsl";

// The blend pass for a back buffer format, the figures' texture and its two
// upload buffers. Under g_counter.lock.
static bool CounterReady(DXGI_FORMAT format)
{
    CounterState &c = g_counter;
    if (c.failed) return false;
    if (c.format == format && c.blend && c.glyphs) return true;
    CounterRelease();
    auto fail = [&](const char *what) { c.failed = true; CounterRelease(); Trace("[counter] off: %s", what); return false; };
    if (!c.root)
    {
        D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0};
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 4;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = {1, &range};
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC desc = {2, params};
        winrt::com_ptr<ID3DBlob> signature, errors;
        if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, signature.put(), errors.put())) ||
            FAILED(lfCore.device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                                      IID_PPV_ARGS(&c.root)))) return fail("its root signature");
    }
    winrt::com_ptr<ID3DBlob> vs, ps, errors;
    if (FAILED(CachedCompile(kCounterHlsl, sizeof(kCounterHlsl) - 1, "sm-counter.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, vs.put(), errors.put())) ||
        FAILED(CachedCompile(kCounterHlsl, sizeof(kCounterHlsl) - 1, "sm-counter.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, ps.put(), errors.put())))
        return fail("its shaders");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = c.root;
    pso.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso.BlendState.RenderTarget[0].BlendEnable = TRUE;
    pso.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    pso.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    pso.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso.BlendState.RenderTarget[0].RenderTargetWriteMask =
        D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = format;
    pso.SampleDesc.Count = 1;
    if (FAILED(lfCore.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&c.blend)))) return fail("its pipeline");
    D3D12_DESCRIPTOR_HEAP_DESC views = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
    D3D12_DESCRIPTOR_HEAP_DESC target = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE};
    if (FAILED(lfCore.device->CreateDescriptorHeap(&views, IID_PPV_ARGS(&c.views))) ||
        FAILED(lfCore.device->CreateDescriptorHeap(&target, IID_PPV_ARGS(&c.target)))) return fail("its descriptor heaps");
    D3D12_HEAP_PROPERTIES card = {D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC texture = {};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = kCounterMaxW; texture.Height = kCounterMaxH; texture.DepthOrArraySize = 1; texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM; texture.SampleDesc.Count = 1;
    if (FAILED(lfCore.device->CreateCommittedResource(&card, D3D12_HEAP_FLAG_NONE, &texture,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&c.glyphs)))) return fail("its texture");
    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM; view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; view.Texture2D.MipLevels = 1;
    lfCore.device->CreateShaderResourceView(c.glyphs, &view, c.views->GetCPUDescriptorHandleForHeapStart());
    D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_UPLOAD};
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = UINT64(kCounterGlyphPitch) * kCounterMaxH;
    buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    for (unsigned i = 0; i < 2; ++i)
    {
        D3D12_RANGE none = {0, 0};
        void *at = nullptr;
        if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&c.upload[i]))) ||
            FAILED(c.upload[i]->Map(0, &none, &at))) return fail("its upload buffers");
        c.mapped[i] = static_cast<BYTE *>(at);
    }
    c.format = format;
    c.pitch = (kCounterMaxW * CounterBytes(format) + 255u) & ~255u;
    c.drawn = false;
    c.drawn_base = -2;
    return true;
}

// LF_COUNTER_PROOF=<file>: once, some seconds in, the counter's box is read
// back from the back buffer it was copied onto and written out raw (width,
// height, format, row pitch as four 32-bit numbers, then the rows) - the
// output is hidden from screen capture, so tests/screen_counter_native.py has
// no other way to see what reached the frame. Under g_counter.lock.
static void CounterProof(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer, DXGI_FORMAT format, UINT x, UINT y)
{
    static int stage = -1;   // -1 unread, 0 off, 1 waiting, 2 copied, 3 done
    static char path[MAX_PATH] = {};
    static ID3D12Resource *readback = nullptr;
    static ULONGLONG began = 0, copied = 0;
    static UINT w = 0, h = 0, pitch = 0;
    CounterState &c = g_counter;
    if (stage == -1)
    {
        const DWORD len = GetEnvironmentVariableA("LF_COUNTER_PROOF", path, sizeof(path));
        stage = (len > 0 && len < sizeof(path)) ? 1 : 0;
        began = GetTickCount64();
    }
    if (stage == 0 || stage == 3) return;
    if (stage == 1)
    {
        static const ULONGLONG after = [] {
            char text[16] = {};
            const DWORD len = GetEnvironmentVariableA("LF_COUNTER_PROOF_AFTER", text, sizeof(text));
            return static_cast<ULONGLONG>((len > 0 && len < sizeof(text)) ? std::max(200, atoi(text)) : 6000);
        }();
        if (GetTickCount64() - began < after || c.base_fps < 0) return;
        w = c.w; h = c.h; pitch = c.pitch;
        D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC buffer = {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = UINT64(pitch) * h;
        buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) { stage = 0; return; }
        auto to_source = Barrier(back_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &to_source);
        D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
        from.pResource = back_buffer;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = readback;
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Footprint.Format = format;
        to.PlacedFootprint.Footprint.Width = w;
        to.PlacedFootprint.Footprint.Height = h;
        to.PlacedFootprint.Footprint.Depth = 1;
        to.PlacedFootprint.Footprint.RowPitch = pitch;
        const D3D12_BOX box = {x, y, 0, x + w, y + h, 1};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
        auto back = Barrier(back_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &back);
        copied = GetTickCount64();
        stage = 2;
        return;
    }
    // Long after the frame it was copied from has been shown.
    if (GetTickCount64() - copied < 1000) return;
    stage = 3;
    void *at = nullptr;
    D3D12_RANGE all = {0, SIZE_T(pitch) * h};
    if (FAILED(readback->Map(0, &all, &at))) return;
    FILE *file = nullptr;
    if (fopen_s(&file, path, "wb") == 0 && file)
    {
        const uint32_t head[4] = {w, h, static_cast<uint32_t>(format), pitch};
        fwrite(head, sizeof(head), 1, file);
        fwrite(at, 1, SIZE_T(pitch) * h, file);
        fclose(file);
        Trace("[counter] proof written: %ux%u, format %u, base %d, shown %d", w, h, (unsigned)format, c.drawn_base, c.drawn_shown);
    }
    D3D12_RANGE none = {0, 0};
    readback->Unmap(0, &none);
    readback->Release();
    readback = nullptr;
}

// The counter onto a back buffer in COPY_DEST, by the thread that presents;
// the buffer is in COPY_DEST again afterwards.
static void CounterStamp(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer)
{
    CounterState &c = g_counter;
    std::lock_guard<std::mutex> hold(c.lock);
    CounterLook();
    if (!c.on || c.hold || !list || !back_buffer || !lfCore.device) return;
    const D3D12_RESOURCE_DESC desc = back_buffer->GetDesc();
    if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        desc.Format != DXGI_FORMAT_R10G10B10A2_UNORM && desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT) return;
    if (!CounterReady(desc.Format)) return;
    // Both figures while frames in between are made; the one there is otherwise.
    const bool both = FgRequested();
    const bool console_hud = smOpt.stream_dedup.load() != 0;
    const int base = c.base_fps, shown = both ? c.shown_fps : -1;
    // (Round 79: Advanced - its line moves with every frame: redrawn twenty times a second.)
    const ULONGLONG tick = GetTickCount64();
    const int mode = int(both) + (c.advanced ? 2 : 0) + (console_hud ? 4 : 0);
    auto hash_mix = [](uint64_t h, int v) -> uint64_t {
        h ^= static_cast<uint64_t>(static_cast<uint32_t>(v));
        return h * 1099511628211ull;
    };
    uint64_t hud_key = 14695981039346656037ull;
    hud_key = hash_mix(hud_key, base);
    hud_key = hash_mix(hud_key, shown);
    hud_key = hash_mix(hud_key, mode);
    hud_key = hash_mix(hud_key, smOpt.counter_size.load());
    if (console_hud)
    {
        hud_key = hash_mix(hud_key, g_generator);
        hud_key = hash_mix(hud_key, smOpt.msm_ai.load());
        hud_key = hash_mix(hud_key, smOpt.multiplier.load());
        hud_key = hash_mix(hud_key, smOpt.ratio.load());
        hud_key = hash_mix(hud_key, smOpt.adaptive.load());
        hud_key = hash_mix(hud_key, smOpt.upscaler.load());
        hud_key = hash_mix(hud_key, smOpt.game_fps.load());
    }
    if (!c.drawn || hud_key != c.drawn_key || (c.advanced && tick - c.drawn_at >= 50))
    {
        const bool ok = console_hud
            ? CounterDrawConsole(base, shown, both, c.advanced, desc.Height)
            : (c.advanced ? CounterDrawAdvanced(base, shown, both, desc.Height) : CounterDraw(base, shown, both, desc.Height));
        if (!ok) return;
        c.drawn_at = tick;
        // The other buffer: the one in front may still be read by a list on its way.
        const unsigned next = c.front ^ 1u;
        if (console_hud) CounterCopyRgba(c.mapped[next]);
        else CounterConvert(c.mapped[next]);
        c.front = next;
        c.drawn = true;
        c.drawn_base = base; c.drawn_shown = shown; c.drawn_mode = mode; c.drawn_key = hud_key;
        auto to_write = Barrier(c.glyphs, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &to_write);
        D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
        from.pResource = c.upload[c.front];
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        from.PlacedFootprint.Footprint.Width = c.w;
        from.PlacedFootprint.Footprint.Height = c.h;
        from.PlacedFootprint.Footprint.Depth = 1;
        from.PlacedFootprint.Footprint.RowPitch = kCounterGlyphPitch;
        to.pResource = c.glyphs;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        auto to_read = Barrier(c.glyphs, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &to_read);
    }
    if (c.w == 0 || c.h == 0 || desc.Width < c.w || desc.Height < c.h) return;
    const UINT x = static_cast<UINT>(std::clamp<LONGLONG>(c.dx, 0, static_cast<LONGLONG>(desc.Width) - c.w));
    const UINT y = static_cast<UINT>(std::clamp<LONGLONG>(c.dy, 0, static_cast<LONGLONG>(desc.Height) - c.h));
    auto to_target = Barrier(back_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ResourceBarrier(1, &to_target);
    const D3D12_CPU_DESCRIPTOR_HANDLE target = c.target->GetCPUDescriptorHandleForHeapStart();
    lfCore.device->CreateRenderTargetView(back_buffer, nullptr, target);
    list->OMSetRenderTargets(1, &target, FALSE, nullptr);
    const D3D12_VIEWPORT viewport = {float(x), float(y), float(c.w), float(c.h), 0.0f, 1.0f};
    const D3D12_RECT box = {LONG(x), LONG(y), LONG(x + c.w), LONG(y + c.h)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &box);
    ID3D12DescriptorHeap *heaps[] = {c.views};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(c.root);
    list->SetPipelineState(c.blend);
    // scRGB: 1.0 is 80 nits. HDR kept: white at 203 nits; the linear SDR
    // output: white at the screen's SDR white, like the picture under it.
    struct { int x, y; UINT encoding; float white; } numbers = {
        int(x), int(y),
        desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ? 1u : desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 2u : 0u,
        lfHdr.keep ? 2.5375f : lfHdr.paper_white};
    list->SetGraphicsRoot32BitConstants(0, 4, &numbers, 0);
    list->SetGraphicsRootDescriptorTable(1, c.views->GetGPUDescriptorHandleForHeapStart());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    auto to_copy = Barrier(back_buffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &to_copy);
    CounterProof(list, back_buffer, desc.Format, x, y);
}

// LF_SHOW_PROOF=<file>: what reached the screen, for tests. Whenever
// "<file>.go" exists (looked for twice a second) the next real frame's whole
// back buffer is read back - before the counter goes on it - and written to
// <file> raw (width, height, format and row pitch as four 32-bit numbers, then
// the rows); "<file>.go" is removed when it is there. The output is hidden
// from screen capture, and the engine's own screenshot is the network's
// result, not what the presenter put on the screen (tests/screen_color_passes.py).
// By the thread that presents; back_buffer in COPY_DEST.
static void ShowProof(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer, bool real)
{
    static std::mutex lock;
    static int stage = -1;   // -1 unread, 0 off, 1 waiting for the go, 2 copied
    static char path[MAX_PATH] = {}, go[MAX_PATH + 4] = {};
    static ID3D12Resource *readback = nullptr;
    static ULONGLONG looked = 0, copied = 0;
    static UINT w = 0, h = 0, pitch = 0;
    static DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    if (stage == 0) return;
    std::lock_guard<std::mutex> hold(lock);
    if (stage == -1)
    {
        const DWORD len = GetEnvironmentVariableA("LF_SHOW_PROOF", path, sizeof(path));
        stage = (len > 0 && len < sizeof(path)) ? 1 : 0;
        if (stage) { strcpy_s(go, path); strcat_s(go, ".go"); }
        return;
    }
    const ULONGLONG tick = GetTickCount64();
    if (stage == 1)
    {
        if (!list || !back_buffer) return;
        // "<file>.go" holding a "g" asks for a frame in between instead.
        static bool want_made = false, asked = false;
        if (!asked)
        {
            if (tick - looked < 500) return;
            looked = tick;
            if (GetFileAttributesA(go) == INVALID_FILE_ATTRIBUTES) return;
            char kind = 0;
            FILE *which = nullptr;
            if (fopen_s(&which, go, "rb") == 0 && which) { fread(&kind, 1, 1, which); fclose(which); }
            want_made = kind == 'g';
            asked = true;
        }
        if (real == want_made) return;
        asked = false;
        const D3D12_RESOURCE_DESC desc = back_buffer->GetDesc();
        const UINT bytes = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8u : 4u;
        const UINT want_pitch = (static_cast<UINT>(desc.Width) * bytes + 255u) & ~255u;
        if (!readback || w != desc.Width || h != desc.Height || pitch != want_pitch)
        {
            if (readback) { readback->Release(); readback = nullptr; }
            D3D12_HEAP_PROPERTIES heap = {D3D12_HEAP_TYPE_READBACK};
            D3D12_RESOURCE_DESC buffer = {};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = UINT64(want_pitch) * desc.Height;
            buffer.Height = 1; buffer.DepthOrArraySize = 1; buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1; buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) { readback = nullptr; return; }
        }
        w = static_cast<UINT>(desc.Width); h = desc.Height; pitch = want_pitch; format = desc.Format;
        auto to_source = Barrier(back_buffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &to_source);
        D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
        from.pResource = back_buffer;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource = readback;
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Footprint.Format = format;
        to.PlacedFootprint.Footprint.Width = w;
        to.PlacedFootprint.Footprint.Height = h;
        to.PlacedFootprint.Footprint.Depth = 1;
        to.PlacedFootprint.Footprint.RowPitch = pitch;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        auto back = Barrier(back_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &back);
        copied = tick;
        stage = 2;
        return;
    }
    // Long after the frame it was copied from has been shown.
    if (tick - copied < 700) return;
    stage = 1;
    void *at = nullptr;
    D3D12_RANGE all = {0, SIZE_T(pitch) * h};
    if (FAILED(readback->Map(0, &all, &at))) return;
    FILE *file = nullptr;
    if (fopen_s(&file, path, "wb") == 0 && file)
    {
        const uint32_t head[4] = {w, h, static_cast<uint32_t>(format), pitch};
        fwrite(head, sizeof(head), 1, file);
        fwrite(at, 1, SIZE_T(pitch) * h, file);
        fclose(file);
    }
    D3D12_RANGE none = {0, 0};
    readback->Unmap(0, &none);
    DeleteFileA(go);
}
