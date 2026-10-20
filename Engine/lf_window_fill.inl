// lf_window_fill.inl - a game played in a window smaller than its screen is
// shown over the whole screen.
//
// "START MOTION" makes the game's window borderless over its screen; a game
// that puts its own size or frame back, one Windows will not let be touched
// (running elevated), or "Scaled" picked in the app, keeps its window as it
// is: the output window then covers the whole screen that window is on and
// the picture is scaled up to fill it, aspect kept, black bars where the
// shapes differ. The window's title bar and borders are left out.
//
// Quality - what a screen capture allows. Plain D3D12 compute:
//   1. Edge-directed Lanczos-2. For each screen pixel the 4x4 texels around
//      it; the edge direction, and whether it is a clean edge or a thin line
//      or noise, come from the luma gradients of the inner four. The kernel
//      is stretched along a clean edge (stair-steps become straight lines)
//      and keeps its full sharpness across it. Deringing: the result never
//      leaves the range of the four texels around it, so the Lanczos lobes
//      cannot draw halos.
//   2. Contrast-limited sharpening at the screen's own size (the RCAS shape):
//      each pixel pushed from its four neighbours only as far as they leave
//      headroom, and half as far where it stands out alone (noise). Grows
//      with the scale (none at 1:1, full from 1.5x); 8-bit output only - the
//      HDR formats get step 1 alone.
// It runs on every frame shown, real or made in between, as the last step
// before a Present (ShowBlit). HDR kept as HDR needs the capture's own size,
// so the window stays the size it is there.
//
// "Upscaling" in the app (round 14) puts step 2 into the hands of whoever
// plays, and onto a picture that is not scaled at all too:
//   - Upscaler "Off": as above - by the scale, nothing at 1:1.
//   - any other choice: "Sharpness" 0..100% is how far step 2 goes (100: all
//     the picture's own contrast leaves room for), whatever the scale;
//     "Sharpness only" is these two steps and nothing else - what FSR 1 is.
//   - "Over sharpness" 0..100% on top: every pixel pushed further from the
//     eight around it with no such limit - edges get a bright and a dark
//     rim, which is what is asked for by whoever turns it up.
// Where it is done. Sharpening every frame shown, on its way to the screen,
// cost a game that fills the card 62 -> 55 frames a second at x6 (measured:
// 0.3 ms a frame for the pass and its copy, and a frame in between could no
// longer be drawn straight onto the back buffer - 0.17 ms more). So where
// frames in between are rebuilt from the real ones (sm_rebuild.inl) and
// nothing is scaled, the real frame is sharpened once, as it is taken
// (SharpenInto, from lf_dlssg.inl), and every frame in between is built
// from sharp ones: one pass a pair instead of six. Everywhere else - no frame
// generation, a window scaled onto its screen, frames in between shown as the
// generator made them - it is the last step before the Present, as above.
//
// Three more kinds of "Upscaler" (round 16), each for a picture the two steps
// above are wrong for:
//   - "Nearest Neighbor": every screen pixel is the one texel it lies in. A
//     picture made of pixels on purpose - an old game, pixel art - keeps them
//     as squares; nothing is smoothed and nothing sharpened.
//   - "Integer": the same, at the largest whole number of times the picture
//     fits its screen, in the middle, black around it: every texel is a
//     square of the same size.
//   - "Anime4K": for drawn pictures - flat colours and lines. After step 1,
//     the two steps of Anime4K as it first was (0.9, bloc97): the lighter
//     side is pushed into a dark line, which thins it (AnimePushMain); then
//     every pixel on a soft edge takes the colour of the side where the
//     picture is calm, which makes the edge a step (AnimeGradMain).
//     "Sharpness" is how far both go. On a picture that is not scaled it is
//     those two steps alone.
//
// The mouse. The game's window stays under the picture, so a click would land
// on it at the wrong place. The window is therefore moved with the pointer, a
// few hundred times a second, so that wherever the pointer is on the scaled
// picture the game pixel it points at lies exactly under it (client origin =
// pointer - (pointer - picture corner) / scale). The game only ever sees its
// window being moved, and the pointer stays the system's own. Whenever the
// scaled picture is not shown (the game in the background, minimised) the
// window goes back to where it was; so it does when the engine stops.

static struct UpscaleState
{
    // Where the picture goes: written by the loop (UpscaleTrack), read by the
    // presenters and the pointer thread.
    std::mutex lock;
    RECT monitor = {};        // the screen scaled onto, desktop coordinates
    RECT src = {};            // the game's client area, in captured pixels
    LONG cap_w = 0, cap_h = 0;   // the captured size src is measured in
    RECT dst = {};            // where it lands in the output window
    bool placed = false;
    // The pointer thread, and the window it moved from where.
    HANDLE follow = nullptr;
    std::atomic<bool> stop{false};
    HWND moved = nullptr;
    RECT home = {};
    // The GPU side: one caller at a time (only one thread presents at once,
    // the lock only makes sure of it).
    std::mutex gpu;
    ID3D12RootSignature *rs = nullptr;
    ID3D12PipelineState *scale_pso = nullptr;
    ID3D12PipelineState *sharp_pso = nullptr;
    ID3D12PipelineState *push_pso = nullptr, *grad_pso = nullptr;     // Anime4K's two steps
    ID3D12DescriptorHeap *heap = nullptr;   // kUpscaleSets x (SRV src, UAV a, SRV a, UAV b)
    UINT ring = 0;
    ID3D12Resource *a = nullptr;            // scaled, UNORDERED_ACCESS at rest
    ID3D12Resource *b = nullptr;            // sharpened, UNORDERED_ACCESS at rest
    UINT tw = 0, th = 0;
    DXGI_FORMAT tf = DXGI_FORMAT_UNKNOWN;
    // The real frame sharpened as it is taken (SharpenInto): the loop's own, UNORDERED_ACCESS at rest.
    ID3D12Resource *real = nullptr;
    UINT rw = 0, rh = 0;
    DXGI_FORMAT rf = DXGI_FORMAT_UNKNOWN;
    std::atomic<bool> in_real{false};       // the real frames the presenter is showing came sharpened already
    bool failed = false;
    bool announced = false;
} g_up;

// Every user of the ring waits for its commands before it records again (the
// loop, the presenter, the read-backs), so sixteen sets never overlap.
static constexpr UINT kUpscaleSets = 16;

// A window that keeps its own size (round 31). The loop asks for it with the
// window (FillSet). Such
// a window is taken by Graphics Capture, never as a cut-out of the desktop:
// the cut-out counts everything that changes over the window's place as a
// frame of the game, and at 20 FPS those extra "frames" were a wave in DLSS
// MFG's pace (measured: 0.3 ms borderless, 4.6 ms and up to 20 as a cut-out).
//
// HDR kept as HDR needs the capture's own size: there the window stays as
// it is. That is the HDR compatibility switch AND a
// screen that is in HDR - the switch alone, on an SDR screen, keeps nothing.
static bool g_fill = false;
static bool g_fill_hdr_kept = false;

static void FillSet(HWND hwnd, bool on)
{
    g_fill_hdr_kept = on && HdrSwitchOn() && HdrDisplayOf(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)).hdr_on;
    if (on != g_fill)
        Trace(!on ? "[upscale] the window fills its screen by itself - nothing to scale"
              : g_fill_hdr_kept ? "[upscale] the window keeps its own size, and HDR is kept as HDR on this screen - it stays the size it is"
                                : "[upscale] the window keeps its own size - its picture is scaled onto the whole screen");
    g_fill = on;
}

static bool FillWanted()
{
    return g_fill && !g_fill_hdr_kept && !g_up.failed;
}

// A window source: the output window takes the whole screen the window is on.
static bool UpscaleWanted()
{
    return FillWanted() && lfCap.window_live && lfCap.window != nullptr;
}

// The screen the captured window is on.
static bool UpscaleMonitor(RECT &monitor)
{
    if (lfCap.window == nullptr) return false;
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(MonitorFromWindow(lfCap.window, MONITOR_DEFAULTTONEAREST), &info)) return false;
    if (info.rcMonitor.right - info.rcMonitor.left < 64 || info.rcMonitor.bottom - info.rcMonitor.top < 64) return false;
    monitor = info.rcMonitor;
    return true;
}

// Where the picture goes this frame. frame is the captured window's
// rectangle (WindowBounds), which is where the picture's corner is.
static void UpscaleTrack(const RECT &frame)
{
    const HWND game = lfCap.window;
    RECT src = { 0, 0, static_cast<LONG>(lfShow.w), static_cast<LONG>(lfShow.h) };
    POINT corner = { 0, 0 };
    RECT client = {};
    if (ClientToScreen(game, &corner) && GetClientRect(game, &client) && client.right > 0 && client.bottom > 0)
    {
        const RECT inside = { corner.x - frame.left, corner.y - frame.top,
                              corner.x - frame.left + client.right, corner.y - frame.top + client.bottom };
        const RECT picture = src;
        RECT both = {};
        if (IntersectRect(&both, &inside, &picture) && both.right - both.left >= 64 && both.bottom - both.top >= 64)
            src = both;
    }
    const LONG sw = src.right - src.left, sh = src.bottom - src.top;
    const LONG bw = static_cast<LONG>(lfShow.bw), bh = static_cast<LONG>(lfShow.bh);
    if (sw <= 0 || sh <= 0 || bw <= 0 || bh <= 0) return;
    // ("Integer": the largest whole number of times it fits.)
    double scale = std::min(double(bw) / sw, double(bh) / sh);
    if (smOpt.upscaler.load() == 2 && scale >= 1.0) scale = std::floor(scale);
    const LONG dw = std::min(bw, static_cast<LONG>(std::lround(sw * scale)));
    const LONG dh = std::min(bh, static_cast<LONG>(std::lround(sh * scale)));
    const RECT dst = { (bw - dw) / 2, (bh - dh) / 2, (bw - dw) / 2 + dw, (bh - dh) / 2 + dh };
    std::lock_guard<std::mutex> hold(g_up.lock);
    if (!g_up.placed || !EqualRect(&src, &g_up.src) || !EqualRect(&dst, &g_up.dst))
        Trace("[upscale] %ldx%ld of the window -> %ldx%ld on the %ldx%ld screen (x%.2f)",
            sw, sh, dw, dh, bw, bh, scale);
    g_up.src = src;
    g_up.cap_w = static_cast<LONG>(lfShow.w);
    g_up.cap_h = static_cast<LONG>(lfShow.h);
    g_up.dst = dst;
    g_up.placed = true;
}

// While scaling, the picture shows only while the game is the window in
// front: it covers the whole screen, and anything else the person turns to
// would sit under it. Settles over 200 ms like WindowWantsOverlay.
static bool UpscaleWantsOverlay()
{
    static bool settled = true, pending = true;
    static ULONGLONG since = 0;
    const HWND game = lfCap.window;
    if (IsIconic(game)) { settled = pending = false; return false; }
    bool now_ok = false;
    const HWND front = GetForegroundWindow();
    if (front == game) now_ok = true;
    else if (front != nullptr)
    {
        DWORD front_pid = 0, game_pid = 0;
        GetWindowThreadProcessId(front, &front_pid);
        GetWindowThreadProcessId(game, &game_pid);
        now_ok = front_pid != 0 && (front_pid == game_pid || front_pid == GetCurrentProcessId());
        // Multi-display mode (round 43): a window in front on another screen than the game's leaves the picture up.
        if (!now_ok && smOpt.multi_display.load())
            now_ok = MonitorFromWindow(front, MONITOR_DEFAULTTONULL) != MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST);
    }
    const ULONGLONG now = GetTickCount64();
    if (now_ok != pending) { pending = now_ok; since = now; }
    if (pending != settled && now - since >= 200) settled = pending;
    return settled;
}

// Put a window the pointer thread moved back where it was.
static void UpscaleGoHome()
{
    const HWND moved = g_up.moved;
    if (moved == nullptr) return;
    g_up.moved = nullptr;
    if (!IsWindow(moved)) return;
    SetWindowPos(moved, nullptr, g_up.home.left, g_up.home.top, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
    Trace("[upscale] the window is back at %ld,%ld", g_up.home.left, g_up.home.top);
}

// One look at the pointer: move the game so the pixel it points at on the
// scaled picture is the game's pixel under it.
static void UpscaleFollowTick()
{
    const HWND game = lfCap.window;
    const bool active = lfShow.upscaling && lfShow.visible && lfCap.window_live && game != nullptr && IsWindow(game);
    if (g_up.moved != nullptr && (!active || g_up.moved != game)) UpscaleGoHome();
    if (!active || IsIconic(game) || IsZoomed(game)) return;
    POINT at = {};
    if (!GetCursorPos(&at)) return;
    RECT monitor, src, dst;
    {
        std::lock_guard<std::mutex> hold(g_up.lock);
        if (!g_up.placed) return;
        monitor = g_up.monitor;
        src = g_up.src;
        dst = g_up.dst;
    }
    if (!PtInRect(&monitor, at)) return;
    const LONG sw = src.right - src.left, sh = src.bottom - src.top;
    const LONG dw = dst.right - dst.left, dh = dst.bottom - dst.top;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    // The game pixel under the pointer, relative to the client corner; held
    // on the picture's edge while the pointer is over a black bar, so a click
    // there still lands on the game and never on whatever lies behind.
    const double gx = std::clamp((at.x - monitor.left - dst.left + 0.5) * double(sw) / dw, 0.0, sw - 1.0);
    const double gy = std::clamp((at.y - monitor.top - dst.top + 0.5) * double(sh) / dh, 0.0, sh - 1.0);
    const POINT want = { at.x - static_cast<LONG>(gx), at.y - static_cast<LONG>(gy) };
    POINT now = { 0, 0 };
    if (!ClientToScreen(game, &now)) return;
    if (want.x == now.x && want.y == now.y) return;
    RECT window = {};
    if (!GetWindowRect(game, &window)) return;
    if (g_up.moved == nullptr)
    {
        g_up.home = window;
        g_up.moved = game;
        Trace("[upscale] the window follows the pointer (its place: %ld,%ld)", window.left, window.top);
    }
    SetWindowPos(game, nullptr, window.left + (want.x - now.x), window.top + (want.y - now.y), 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
}

static DWORD WINAPI UpscaleFollowThread(LPVOID)
{
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
    while (!g_up.stop)
    {
        LARGE_INTEGER due;
        due.QuadPart = -20000;   // 2 ms
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 20);
        else Sleep(2);
        UpscaleFollowTick();
    }
    if (timer) CloseHandle(timer);
    return 0;
}

// The worker is ending: stop following and put the window back.
static void UpscaleAtExit()
{
    g_up.stop = true;
    if (g_up.follow != nullptr)
    {
        WaitForSingleObject(g_up.follow, 500);
        CloseHandle(g_up.follow);
        g_up.follow = nullptr;
    }
    UpscaleGoHome();
}

// Start the pointer thread, once; it idles while nothing is scaled.
static void UpscaleFollowStart()
{
    if (g_up.follow != nullptr) return;
    g_up.stop = false;
    g_up.follow = CreateThread(nullptr, 0, UpscaleFollowThread, nullptr, 0, nullptr);
    if (g_up.follow == nullptr) { Trace("[upscale] the pointer thread did not start"); return; }
    atexit(UpscaleAtExit);
}

static const char kUpscaleHlsl[] = R"hlsl(
Texture2D<float4>   gSrc : register(t0);
RWTexture2D<float4> gDst : register(u0);
SamplerState        gLin : register(s0);
cbuffer UC : register(b0)
{
    int gSrcX; int gSrcY; int gSrcW; int gSrcH;   // the part of the picture scaled
    int gDstX; int gDstY; int gDstW; int gDstH;   // where it lands in the output
    uint gOutW; uint gOutH; float gSharp; float gOver;
    uint gKind;                                    // 2, 3: the nearest texel as it is
};
static const float3 kLuma = float3(0.299f, 0.587f, 0.114f);
static const float kPi = 3.14159265f;

bool Inside(int2 o)
{
    return o.x >= gDstX && o.y >= gDstY && o.x < gDstX + gDstW && o.y < gDstY + gDstH;
}

float3 Tap(int2 q)
{
    return gSrc[clamp(q, int2(gSrcX, gSrcY), int2(gSrcX + gSrcW - 1, gSrcY + gSrcH - 1))].rgb;
}

// Lanczos-2 of a distance given squared: sinc(x) sinc(x/2), zero from 2 out.
float Lanczos2(float r2)
{
    if (r2 >= 4.0f) return 0.0f;
    if (r2 < 1e-6f) return 1.0f;
    float a = kPi * sqrt(r2);
    return 2.0f * sin(a) * sin(0.5f * a) / (a * a);
}

[numthreads(8, 8, 1)]
void ScaleMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gOutW || id.y >= gOutH) return;
    int2 o = int2(id.xy);
    if (!Inside(o)) { gDst[o] = float4(0.0f, 0.0f, 0.0f, 1.0f); return; }
    float2 ratio = float2(gSrcW, gSrcH) / float2(gDstW, gDstH);
    float2 pos = float2(gSrcX, gSrcY) + (float2(o - int2(gDstX, gDstY)) + 0.5f) * ratio;
    if (gKind == 2u || gKind == 3u) { gDst[o] = float4(Tap(int2(floor(pos))), 1.0f); return; }
    if (ratio.x > 1.0f || ratio.y > 1.0f)
    {
        // A window larger than the screen: four bilinear taps over the
        // footprint instead.
        uint pw, ph;
        gSrc.GetDimensions(pw, ph);
        float2 inv = 1.0f / float2(pw, ph);
        float2 q = 0.25f * ratio;
        float3 s = gSrc.SampleLevel(gLin, (pos + float2(-q.x, -q.y)) * inv, 0).rgb +
                   gSrc.SampleLevel(gLin, (pos + float2( q.x, -q.y)) * inv, 0).rgb +
                   gSrc.SampleLevel(gLin, (pos + float2(-q.x,  q.y)) * inv, 0).rgb +
                   gSrc.SampleLevel(gLin, (pos + float2( q.x,  q.y)) * inv, 0).rgb;
        gDst[o] = float4(s * 0.25f, 1.0f);
        return;
    }
    pos -= 0.5f;                      // texel centres on whole numbers
    int2 base = int2(floor(pos));
    float2 f = pos - float2(base);
    float3 c[4][4];
    float l[4][4];
    [unroll] for (int y = 0; y < 4; ++y)
    [unroll] for (int x = 0; x < 4; ++x)
    {
        c[y][x] = Tap(base + int2(x - 1, y - 1));
        l[y][x] = dot(c[y][x], kLuma);
    }

    // The edge: the luma gradient of the four inner texels, each weighted by
    // how near the position is. A gradient whose two half-steps agree is an
    // edge; one whose half-steps cancel is a thin line or noise.
    float2 dir = 0.0f;
    float clean = 0.0f;
    [unroll] for (int j = 1; j <= 2; ++j)
    [unroll] for (int i = 1; i <= 2; ++i)
    {
        float w = (i == 1 ? 1.0f - f.x : f.x) * (j == 1 ? 1.0f - f.y : f.y);
        float gx = l[j][i + 1] - l[j][i - 1];
        float gy = l[j + 1][i] - l[j - 1][i];
        dir += float2(gx, gy) * w;
        float sx = abs(l[j][i + 1] - l[j][i]) + abs(l[j][i] - l[j][i - 1]);
        float sy = abs(l[j + 1][i] - l[j][i]) + abs(l[j][i] - l[j - 1][i]);
        float ax = abs(gx), ay = abs(gy);
        float both = ax + ay;
        if (both > 1e-4f)
            clean += w * (ax * ax / max(sx, 1e-4f) + ay * ay / max(sy, 1e-4f)) / both;
    }
    float len = length(dir);
    float2 d = len > 1e-4f ? dir / len : float2(1.0f, 0.0f);
    // A faint gradient is not an edge worth following.
    float edge = saturate(clean) * saturate(len * 16.0f);
    edge *= edge;
    // Along a clean edge the kernel is up to twice as long; across it, it is
    // Lanczos as it is.
    float squeeze = lerp(1.0f, 0.5f, edge);
    float3 acc = 0.0f;
    float wsum = 0.0f;
    [unroll] for (int v = 0; v < 4; ++v)
    [unroll] for (int u = 0; u < 4; ++u)
    {
        float2 off = float2(u - 1, v - 1) - f;
        float across = dot(off, d);
        float along = dot(off, float2(-d.y, d.x)) * squeeze;
        float w = Lanczos2(across * across + along * along);
        acc += c[v][u] * w;
        wsum += w;
    }
    float3 result = acc / max(wsum, 1e-4f);
    // Deringing: never outside the four texels around.
    float3 mn = min(min(c[1][1], c[1][2]), min(c[2][1], c[2][2]));
    float3 mx = max(max(c[1][1], c[1][2]), max(c[2][1], c[2][2]));
    gDst[o] = float4(clamp(result, mn, mx), 1.0f);
}

[numthreads(8, 8, 1)]
void SharpMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gOutW || id.y >= gOutH) return;
    int2 o = int2(id.xy);
    float3 e = gSrc[o].rgb;
    if (!Inside(o)) { gDst[o] = float4(e, 1.0f); return; }
    int2 lo = int2(gDstX, gDstY);
    int2 hi = int2(gDstX + gDstW - 1, gDstY + gDstH - 1);
    float3 b = gSrc[clamp(o + int2(0, -1), lo, hi)].rgb;
    float3 d = gSrc[clamp(o + int2(-1, 0), lo, hi)].rgb;
    float3 f = gSrc[clamp(o + int2(1, 0), lo, hi)].rgb;
    float3 h = gSrc[clamp(o + int2(0, 1), lo, hi)].rgb;
    float3 mn4 = min(min(b, d), min(f, h));
    float3 mx4 = max(max(b, d), max(f, h));
    // How far the four may push before the result leaves 0..1.
    float3 hitMin = mn4 / (4.0f * mx4 + 1e-4f);
    float3 hitMax = (1.0f - mx4) / (4.0f * mn4 - 4.0f - 1e-4f);
    float3 lobe3 = max(-hitMin, hitMax);
    float lobe = max(-0.1875f, min(max(lobe3.r, max(lobe3.g, lobe3.b)), 0.0f)) * gSharp;
    // A pixel that stands out alone is noise: half the push.
    float lb = dot(b, kLuma), ld = dot(d, kLuma), lf = dot(f, kLuma), lh = dot(h, kLuma), le = dot(e, kLuma);
    float range = max(max(max(lb, ld), max(lf, lh)), le) - min(min(min(lb, ld), min(lf, lh)), le);
    float nz = saturate(abs(0.25f * (lb + ld + lf + lh) - le) / max(range, 1e-4f));
    lobe *= 1.0f - 0.5f * nz;
    float3 r = (lobe * (b + d + f + h) + e) / (4.0f * lobe + 1.0f);
    if (gOver > 0.0f)
    {
        // Over sharpness: further from the eight around, by brightness (no coloured rims), without the limit.
        float corners = dot(gSrc[clamp(o + int2(-1, -1), lo, hi)].rgb + gSrc[clamp(o + int2(1, -1), lo, hi)].rgb +
                            gSrc[clamp(o + int2(-1, 1), lo, hi)].rgb + gSrc[clamp(o + int2(1, 1), lo, hi)].rgb, kLuma);
        float around = (2.0f * (lb + ld + lf + lh) + corners) / 12.0f;
        r += gOver * 2.5f * (le - around);
    }
    gDst[o] = float4(saturate(r), 1.0f);
}

// ---- Anime4K (0.9). A texel with its brightness beside it.
float4 Lit(int2 q, int2 lo, int2 hi)
{
    const float3 c = gSrc[clamp(q, lo, hi)].rgb;
    return float4(c, dot(c, kLuma));
}

// The three on the lighter side, mixed into the pixel - kept if that is lighter than what it has so far.
float4 Lighter(float4 mc, float4 so_far, float4 a, float4 b, float4 c, float strength)
{
    const float4 mixed = lerp(mc, (a + b + c) / 3.0f, strength);
    return mixed.a > so_far.a ? mixed : so_far;
}

// One line through the pixel: three texels on one side of it, three on the other. Where one side is lighter
// than the pixel and than all of the other side, the pixel is on the dark side of an edge there.
float4 PushLine(float4 mc, float4 so_far, float4 d0, float4 d1, float4 d2, float4 l0, float4 l1, float4 l2, float strength)
{
    if (min(l0.a, min(l1.a, l2.a)) > max(mc.a, max(d0.a, max(d1.a, d2.a)))) return Lighter(mc, so_far, l0, l1, l2, strength);
    if (min(d0.a, min(d1.a, d2.a)) > max(mc.a, max(l0.a, max(l1.a, l2.a)))) return Lighter(mc, so_far, d0, d1, d2, strength);
    return so_far;
}

// The lighter side pushed into what is dark: a dark line gets thinner. The pixel's brightness goes with it
// (in the fourth place) for the pass after.
[numthreads(8, 8, 1)]
void AnimePushMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gOutW || id.y >= gOutH) return;
    const int2 o = int2(id.xy);
    const int2 lo = int2(gDstX, gDstY), hi = int2(gDstX + gDstW - 1, gDstY + gDstH - 1);
    const float4 mc = Lit(o, lo, hi);
    if (!Inside(o)) { gDst[o] = mc; return; }
    const float4 tl = Lit(o + int2(-1, -1), lo, hi), tc = Lit(o + int2(0, -1), lo, hi), tr = Lit(o + int2(1, -1), lo, hi);
    const float4 ml = Lit(o + int2(-1, 0), lo, hi), mr = Lit(o + int2(1, 0), lo, hi);
    const float4 bl = Lit(o + int2(-1, 1), lo, hi), bc = Lit(o + int2(0, 1), lo, hi), br = Lit(o + int2(1, 1), lo, hi);
    const float strength = gSharp * 0.34f;
    float4 light = mc;
    light = PushLine(mc, light, bl, bc, br, tl, tc, tr, strength);
    light = PushLine(mc, light, ml, bc, bl, mr, tc, tr, strength);
    light = PushLine(mc, light, tl, ml, bl, tr, mr, br, strength);
    light = PushLine(mc, light, ml, tc, tl, mr, bc, br, strength);
    gDst[o] = float4(light.rgb, dot(light.rgb, kLuma));
}

// How steeply the brightness changes at a texel (Sobel), turned round: 1 where the picture is calm.
float Calm(int2 q, int2 lo, int2 hi)
{
    const float tl = gSrc[clamp(q + int2(-1, -1), lo, hi)].a, tc = gSrc[clamp(q + int2(0, -1), lo, hi)].a, tr = gSrc[clamp(q + int2(1, -1), lo, hi)].a;
    const float ml = gSrc[clamp(q + int2(-1, 0), lo, hi)].a, mr = gSrc[clamp(q + int2(1, 0), lo, hi)].a;
    const float bl = gSrc[clamp(q + int2(-1, 1), lo, hi)].a, bc = gSrc[clamp(q + int2(0, 1), lo, hi)].a, br = gSrc[clamp(q + int2(1, 1), lo, hi)].a;
    const float gx = (tr + 2.0f * mr + br) - (tl + 2.0f * ml + bl), gy = (bl + 2.0f * bc + br) - (tl + 2.0f * tc + tr);
    return 1.0f - saturate(sqrt(gx * gx + gy * gy));
}

float4 Calmer(int2 q, int2 lo, int2 hi) { return float4(gSrc[clamp(q, lo, hi)].rgb, Calm(q, lo, hi)); }

// A pixel on a soft edge takes the colour of the side where the picture is calm: the edge becomes a step.
[numthreads(8, 8, 1)]
void AnimeGradMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gOutW || id.y >= gOutH) return;
    const int2 o = int2(id.xy);
    const int2 lo = int2(gDstX, gDstY), hi = int2(gDstX + gDstW - 1, gDstY + gDstH - 1);
    const float4 mc = Calmer(o, lo, hi);
    float3 result = mc.rgb;
    if (Inside(o))
    {
        const float4 tl = Calmer(o + int2(-1, -1), lo, hi), tc = Calmer(o + int2(0, -1), lo, hi), tr = Calmer(o + int2(1, -1), lo, hi);
        const float4 ml = Calmer(o + int2(-1, 0), lo, hi), mr = Calmer(o + int2(1, 0), lo, hi);
        const float4 bl = Calmer(o + int2(-1, 1), lo, hi), bc = Calmer(o + int2(0, 1), lo, hi), br = Calmer(o + int2(1, 1), lo, hi);
        const float strength = gSharp * 0.7f;
        // The first line that has a calm side: the pixel goes over to it.
        float4 side[8][3] = { { tl, tc, tr }, { br, bc, bl }, { mr, tc, tr }, { bl, ml, bc }, { mr, br, tr }, { ml, tl, bl }, { mr, br, bc }, { tc, ml, tl } };
        float4 against[8][3] = { { br, bc, bl }, { tl, tc, tr }, { mc, ml, bc }, { mc, mr, tc }, { ml, tl, bl }, { mr, br, tr }, { mc, ml, tc }, { mc, mr, bc } };
        for (int i = 0; i < 8; ++i)
        {
            const float least = min(side[i][0].a, min(side[i][1].a, side[i][2].a));
            const float most = max(against[i][0].a, max(against[i][1].a, against[i][2].a));
            if (least > mc.a && least > most)
            {
                result = lerp(mc.rgb, (side[i][0].rgb + side[i][1].rgb + side[i][2].rgb) / 3.0f, strength);
                break;
            }
        }
    }
    gDst[o] = float4(result, 1.0f);
}
)hlsl";

// The two passes, compiled and made. Nothing is marked failed while they are
// being made: the loop asks FillWanted from another thread meanwhile.
static bool BuildUpscalePipeline()
{
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? static_cast<CompileFn>(&CachedCompile) : nullptr;
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    auto serialize = d3d12 ? reinterpret_cast<SerializeRootFn>(
                                 GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
    if (compile == nullptr || serialize == nullptr) { Trace("[upscale] compiler unavailable"); return false; }
    ID3DBlob *codes[4] = {};
    const char *entries[4] = { "ScaleMain", "SharpMain", "AnimePushMain", "AnimeGradMain" };
    for (int i = 0; i < 4; ++i)
    {
        ID3DBlob *errors = nullptr;
        const HRESULT hr = compile(kUpscaleHlsl, sizeof(kUpscaleHlsl) - 1, "upscale.hlsl", nullptr, nullptr,
                                   entries[i], "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &codes[i], &errors);
        if (FAILED(hr) || codes[i] == nullptr)
        {
            Trace("[upscale] %s failed 0x%08X: %s", entries[i], hr,
                errors ? static_cast<const char *>(errors->GetBufferPointer()) : "(no log)");
            if (errors) errors->Release();
            for (auto *code : codes) if (code) code->Release();
            return false;
        }
        if (errors) errors->Release();
    }
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 13;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = _countof(params);
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &samp;
    ID3DBlob *rs_blob = nullptr, *errors = nullptr;
    HRESULT hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors);
    if (errors) errors->Release();
    if (FAILED(hr) || rs_blob == nullptr)
    {
        Trace("[upscale] root signature failed 0x%08X", hr);
        for (auto *code : codes) code->Release();
        return false;
    }
    hr = lfCore.device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                            __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(&g_up.rs));
    rs_blob->Release();
    ID3D12PipelineState **psos[4] = { &g_up.scale_pso, &g_up.sharp_pso, &g_up.push_pso, &g_up.grad_pso };
    for (int i = 0; i < 4 && SUCCEEDED(hr); ++i)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = g_up.rs;
        pd.CS.pShaderBytecode = codes[i]->GetBufferPointer();
        pd.CS.BytecodeLength = codes[i]->GetBufferSize();
        hr = lfCore.device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(psos[i]));
    }
    for (auto *code : codes) code->Release();
    if (FAILED(hr)) { Trace("[upscale] pipeline failed 0x%08X", hr); return false; }
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kUpscaleSets * 6;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(lfCore.device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(&g_up.heap))))
    { Trace("[upscale] no descriptor heap"); return false; }
    Trace("[upscale] pipeline ready (edge-directed Lanczos-2, deringing, contrast-limited sharpening)");
    return true;
}

static bool EnsureUpscalePipeline()
{
    if (g_up.scale_pso != nullptr && g_up.sharp_pso != nullptr && g_up.push_pso != nullptr && g_up.grad_pso != nullptr && g_up.heap != nullptr) return true;
    if (g_up.failed) return false;
    if (!BuildUpscalePipeline()) { g_up.failed = true; return false; }
    return true;
}

// The two working textures at the back buffer's size and format.
static bool EnsureUpscaleTargets(UINT w, UINT hgt, DXGI_FORMAT format)
{
    if (g_up.a != nullptr && g_up.b != nullptr && g_up.tw == w && g_up.th == hgt && g_up.tf == format) return true;
    if (g_up.a) { g_up.a->Release(); g_up.a = nullptr; }
    if (g_up.b) { g_up.b->Release(); g_up.b = nullptr; }
    g_up.tw = g_up.th = 0;
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = { format };
    if (FAILED(lfCore.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
        (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0)
    {
        Trace("[upscale] this card cannot write format %u from a shader", (unsigned)format);
        return false;
    }
    D3D12_HEAP_PROPERTIES def = {};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = hgt; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = format; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource **targets[2] = { &g_up.a, &g_up.b };
    for (auto **target : targets)
        if (FAILED(lfCore.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
                reinterpret_cast<void **>(target))))
        {
            Trace("[upscale] %ux%u working textures failed", w, hgt);
            if (g_up.a) { g_up.a->Release(); g_up.a = nullptr; }
            if (g_up.b) { g_up.b->Release(); g_up.b = nullptr; }
            return false;
        }
    g_up.tw = w;
    g_up.th = hgt;
    g_up.tf = format;
    return true;
}

// Where the whole of a sw x sh picture lands on a bw x bh output: fitted,
// aspect kept. Shapes within 1% of each other fill it - the internal size is
// the display's scaled and rounded, and its rounding is no reason for a bar.
static void UpscaleFit(LONG sw, LONG sh, LONG bw, LONG bh, RECT &s, RECT &d)
{
    s = { 0, 0, sw, sh };
    double scale = std::min(double(bw) / sw, double(bh) / sh);
    const bool whole = smOpt.upscaler.load() == 2 && scale >= 1.0;      // "Integer"
    if (whole) scale = std::floor(scale);
    LONG dw = std::min(bw, static_cast<LONG>(std::lround(sw * scale)));
    LONG dh = std::min(bh, static_cast<LONG>(std::lround(sh * scale)));
    if (!whole && std::abs(double(dw) / bw - 1.0) < 0.01 && std::abs(double(dh) / bh - 1.0) < 0.01) { dw = bw; dh = bh; }
    d = { (bw - dw) / 2, (bh - dh) / 2, (bw - dw) / 2 + dw, (bh - dh) / 2 + dh };
}

static bool SharpenFormat(DXGI_FORMAT format)
{
    return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM;
}

// The kind of "Upscaler" picked in the app: 0 off, 1 sharpening only, 2 integer, 3 nearest neighbour, 4 Anime4K.
static int UpscaleKind()
{
    return smOpt.upscaler.load();
}

// "Upscaling" in the app asks for sharpening (or Anime4K's steps, which stand in its place): its sliders
// decide, at any scale.
static bool SharpenAsked()
{
    const int kind = UpscaleKind();
    return (kind == 1 ? smOpt.sharpness.load() > 0 || smOpt.over_sharpness.load() > 0 : kind == 4 && smOpt.sharpness.load() > 0) && !g_up.failed;
}

// How hard the output is sharpened. Left alone ("Off"): none at 1:1, full
// from 1.5x. Asked for: as the slider says. 8-bit only (the HDR formats get
// the scaling alone).
static float UpscaleSharpness(const RECT &s, const RECT &d, DXGI_FORMAT format)
{
    const LONG srw = s.right - s.left, srh = s.bottom - s.top, dsw = d.right - d.left, dsh = d.bottom - d.top;
    if (srw <= 0 || srh <= 0 || !SharpenFormat(format)) return 0.0f;
    const int kind = UpscaleKind();
    if (kind == 2 || kind == 3) return 0.0f;        // texels as they are
    if (kind != 0) return smOpt.sharpness.load() / 100.0f;
    const float scale = static_cast<float>(std::min(double(dsw) / srw, double(dsh) / srh));
    return 0.4f * std::clamp((scale - 1.0f) / 0.5f, 0.0f, 1.0f);
}

// Is it still to be done on a frame's way to the screen (ShowBlit)? Not where
// the real frames were sharpened as they were taken.
static bool SharpenInBlit()
{
    return SharpenAsked() && !g_up.in_real.load();
}

// And how far past that ("Over sharpness").
static float UpscaleOver(DXGI_FORMAT format)
{
    return UpscaleKind() == 1 && SharpenFormat(format) ? smOpt.over_sharpness.load() / 100.0f : 0.0f;
}

// The two passes. src (COPY_SOURCE, left so) is scaled into a, then sharpened
// into b; a and b are w x hgt in format, UNORDERED_ACCESS at rest and left so.
// s is the part of src scaled, d where it lands (black around it). Returns the
// finished one. The caller holds g_up.gpu.
// scale false: src is the output's own size already and only sharpened (into b).
// kind 4 (Anime4K, and a is there): its two steps in place of the sharpening - the first into b, the second
// back into a.
static ID3D12Resource *UpscalePasses(ID3D12GraphicsCommandList *list, ID3D12Resource *src, DXGI_FORMAT src_format,
                                     ID3D12Resource *a, ID3D12Resource *b, DXGI_FORMAT format, UINT w, UINT hgt,
                                     const RECT &s, const RECT &d, float sharp, float over = 0.0f, bool scale = true, int kind = 0)
{
    const LONG srw = s.right - s.left, srh = s.bottom - s.top, dsw = d.right - d.left, dsh = d.bottom - d.top;
    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT set = g_up.ring++ % kUpscaleSets;
    const bool anime = kind == 4 && a != nullptr && sharp > 0.0f;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_up.heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(set) * 6 * stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    srv.Format = src_format;
    lfCore.device->CreateShaderResourceView(src, &srv, cpu);
    cpu.ptr += stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    lfCore.device->CreateUnorderedAccessView(scale ? a : b, nullptr, &uav, cpu);
    cpu.ptr += stride;
    srv.Format = format;
    lfCore.device->CreateShaderResourceView(a, &srv, cpu);
    cpu.ptr += stride;
    lfCore.device->CreateUnorderedAccessView(b, nullptr, &uav, cpu);
    cpu.ptr += stride;
    lfCore.device->CreateShaderResourceView(b, &srv, cpu);
    cpu.ptr += stride;
    lfCore.device->CreateUnorderedAccessView(a, nullptr, &uav, cpu);

    D3D12_RESOURCE_BARRIER in = Barrier(src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &in);
    ID3D12DescriptorHeap *heaps[] = { g_up.heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_up.rs);
    UINT32 c[13] = { UINT32(s.left), UINT32(s.top), UINT32(srw), UINT32(srh),
                     UINT32(d.left), UINT32(d.top), UINT32(dsw), UINT32(dsh),
                     w, hgt, 0, 0, UINT32(kind) };
    memcpy(&c[10], &sharp, sizeof(float));
    memcpy(&c[11], &over, sizeof(float));
    list->SetComputeRoot32BitConstants(0, 13, c, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_up.heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(set) * 6 * stride;
    const D3D12_GPU_DESCRIPTOR_HANDLE first = gpu;
    list->SetComputeRootDescriptorTable(1, gpu);
    // Anime4K's second step: what the first wrote into b, into a.
    auto second = [&] {
        D3D12_RESOURCE_BARRIER to_read = Barrier(b, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &to_read);
        D3D12_GPU_DESCRIPTOR_HANDLE at = first;
        at.ptr += 4 * static_cast<UINT64>(stride);
        list->SetComputeRootDescriptorTable(1, at);
        list->SetPipelineState(g_up.grad_pso);
        list->Dispatch((w + 7) / 8, (hgt + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER back = Barrier(b, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &back);
    };
    if (!scale && anime)
    {
        // The picture as it is: the first step from src into b, the second into a.
        list->SetPipelineState(g_up.push_pso);
        list->Dispatch((w + 7) / 8, (hgt + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER as_it_was = Barrier(src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &as_it_was);
        second();
        return a;
    }
    if (!scale)
    {
        // The picture as it is, sharpened: one pass, src into b.
        list->SetPipelineState(g_up.sharp_pso);
        list->Dispatch((w + 7) / 8, (hgt + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER as_it_was = Barrier(src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &as_it_was);
        return b;
    }
    list->SetPipelineState(g_up.scale_pso);
    list->Dispatch((w + 7) / 8, (hgt + 7) / 8, 1);
    ID3D12Resource *done = a;
    if (sharp > 0.0f || over > 0.0f)
    {
        D3D12_RESOURCE_BARRIER to_read = Barrier(a, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        list->ResourceBarrier(1, &to_read);
        gpu.ptr += 2 * static_cast<UINT64>(stride);
        list->SetComputeRootDescriptorTable(1, gpu);
        list->SetPipelineState(anime ? g_up.push_pso : g_up.sharp_pso);
        list->Dispatch((w + 7) / 8, (hgt + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER back = Barrier(a, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->ResourceBarrier(1, &back);
        done = b;
        if (anime) { second(); done = a; }
    }
    D3D12_RESOURCE_BARRIER out = Barrier(src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &out);
    return done;
}

// Records the scaling of src (COPY_SOURCE) into bb (COPY_DEST); both are
// left as they came.
static bool UpscaleRecord(ID3D12GraphicsCommandList *list, ID3D12Resource *bb, ID3D12Resource *src,
                          const D3D12_RESOURCE_DESC &sd, const D3D12_RESOURCE_DESC &bd)
{
    std::lock_guard<std::mutex> hold(g_up.gpu);
    if (!EnsureUpscalePipeline() || !EnsureUpscaleTargets(static_cast<UINT>(bd.Width), bd.Height, bd.Format))
        return false;
    const LONG sw = static_cast<LONG>(sd.Width), sh = static_cast<LONG>(sd.Height);
    const LONG bw = static_cast<LONG>(bd.Width), bh = static_cast<LONG>(bd.Height);
    RECT s = {}, d = {};
    UpscaleFit(sw, sh, bw, bh, s, d);
    // A window scaled onto its screen: the game's client area where the loop
    // placed it. That rectangle is in captured pixels. One from another size
    // (the window was just resized) leaves the whole picture fitted until the
    // loop places it again.
    if (lfShow.upscaling)
    {
        std::lock_guard<std::mutex> where(g_up.lock);
        if (g_up.placed && g_up.cap_w > 0 && g_up.cap_h > 0)
        {
            const double fx = double(sw) / g_up.cap_w, fy = double(sh) / g_up.cap_h;
            const RECT ps = { static_cast<LONG>(std::lround(g_up.src.left * fx)), static_cast<LONG>(std::lround(g_up.src.top * fy)),
                              static_cast<LONG>(std::lround(g_up.src.right * fx)), static_cast<LONG>(std::lround(g_up.src.bottom * fy)) };
            const RECT pic = { 0, 0, sw, sh }, out = { 0, 0, bw, bh };
            RECT s2 = {}, d2 = {};
            if (ps.right - ps.left > 0 && ps.bottom - ps.top > 0 &&
                IntersectRect(&s2, &ps, &pic) && EqualRect(&s2, &ps) &&
                IntersectRect(&d2, &g_up.dst, &out) && EqualRect(&d2, &g_up.dst))
            {
                s = ps;
                d = g_up.dst;
            }
        }
    }
    const LONG srw = s.right - s.left, srh = s.bottom - s.top, dsw = d.right - d.left, dsh = d.bottom - d.top;
    if (srw <= 0 || srh <= 0 || dsw <= 0 || dsh <= 0) return false;
    const float sharp = UpscaleSharpness(s, d, bd.Format), over = UpscaleOver(bd.Format);
    // (A picture the output's own size, all of it: nothing to scale.)
    const bool scale = srw != bw || srh != bh || dsw != bw || dsh != bh;
    if (!scale && sharp <= 0.0f && over <= 0.0f) return false;
    ID3D12Resource *done = UpscalePasses(list, src, sd.Format, g_up.a, g_up.b, bd.Format,
                                         static_cast<UINT>(bw), static_cast<UINT>(bh), s, d, sharp, over, scale, UpscaleKind());
    D3D12_RESOURCE_BARRIER to_copy = Barrier(done, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &to_copy);
    list->CopyResource(bb, done);
    D3D12_RESOURCE_BARRIER rest = Barrier(done, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(1, &rest);
    if (!g_up.announced)
    {
        g_up.announced = true;
        if (scale) Trace("[upscale] first scaled frame: %ldx%ld -> %ldx%ld%s", srw, srh, dsw, dsh, sharp > 0.0f || over > 0.0f ? ", sharpened" : "");
        else Trace("[upscale] first sharpened frame: %ldx%ld as it is", srw, srh);
    }
    return true;
}

// A real frame sharpened as it is taken: src (COPY_SOURCE) into `into`
// (COPY_DEST, the same size and kind), both left as they came. False: not
// done - the caller copies it as it is.
static bool SharpenInto(ID3D12GraphicsCommandList *list, ID3D12Resource *into, ID3D12Resource *src)
{
    const D3D12_RESOURCE_DESC sd = src->GetDesc(), dd = into->GetDesc();
    if (sd.Width != dd.Width || sd.Height != dd.Height || !SharpenFormat(sd.Format) || !SharpenFormat(dd.Format)) return false;
    std::lock_guard<std::mutex> hold(g_up.gpu);
    if (!EnsureUpscalePipeline()) return false;
    const UINT w = static_cast<UINT>(dd.Width), hgt = dd.Height;
    if (g_up.real == nullptr || g_up.rw != w || g_up.rh != hgt || g_up.rf != dd.Format)
    {
        // (Another size: the loop has waited for everything it sent before it builds for one - sm_loop.inl.)
        if (g_up.real) { g_up.real->Release(); g_up.real = nullptr; }
        D3D12_HEAP_PROPERTIES def = {};
        def.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = hgt; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = dd.Format; td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(lfCore.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                          __uuidof(ID3D12Resource), reinterpret_cast<void **>(&g_up.real))))
        { g_up.real = nullptr; return false; }
        g_up.rw = w; g_up.rh = hgt; g_up.rf = dd.Format;
    }
    const RECT whole = { 0, 0, static_cast<LONG>(w), static_cast<LONG>(hgt) };
    const float sharp = UpscaleSharpness(whole, whole, dd.Format), over = UpscaleOver(dd.Format);
    if (sharp <= 0.0f && over <= 0.0f) return false;
    // (Anime4K takes two pictures to work in: it is done on a frame's way to the screen, ShowBlit.)
    if (UpscaleKind() == 4) return false;
    ID3D12Resource *done = UpscalePasses(list, src, sd.Format, nullptr, g_up.real, dd.Format, w, hgt, whole, whole, sharp, over, false);
    D3D12_RESOURCE_BARRIER to_copy = Barrier(done, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &to_copy);
    list->CopyResource(into, done);
    D3D12_RESOURCE_BARRIER rest = Barrier(done, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(1, &rest);
    return true;
}

// The last step before every Present of the output window: src (in
// COPY_SOURCE) into the back buffer (in COPY_DEST). Scaled whenever the two
// sizes differ - a window's picture onto its whole screen - and copied as it
// is otherwise.
// Both are left in the states they came in.
static void ShowBlitAsItIs(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer, ID3D12Resource *src)
{
    const D3D12_RESOURCE_DESC sd = src->GetDesc(), bd = back_buffer->GetDesc();
    const bool same = sd.Width == bd.Width && sd.Height == bd.Height;
    const bool sharpen = SharpenInBlit() && SharpenFormat(bd.Format) && SharpenFormat(sd.Format);
    if ((lfShow.upscaling || !same || sharpen) && UpscaleRecord(list, back_buffer, src, sd, bd)) return;
    if (same) { list->CopyResource(back_buffer, src); return; }
    // The passes are not there: copy what overlaps rather than nothing.
    D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
    to.pResource = back_buffer;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.pResource = src;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box = { 0, 0, 0, static_cast<UINT>(std::min(sd.Width, bd.Width)),
                            std::min(sd.Height, bd.Height), 1 };
    list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
}

// And, "Spout output" on in the app, the frame is shared with other programs as it goes to the screen (sm_spout.inl).
static void ShowBlit(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer, ID3D12Resource *src)
{
    ShowBlitAsItIs(list, back_buffer, src);
    SpoutCopy(list, back_buffer);
}

