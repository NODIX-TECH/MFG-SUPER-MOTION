// lf_own_motion.inl - what the loop works out from the captured pictures
// itself, on the card:
//
//   1. The scene cut. Each frame's small luma (lf_luma.inl) is compared with
//      the one before: a mean difference over 0.24 of full scale is another
//      scene, and the generator drops its history across it. Exactly no
//      difference is the picture before handed over again (SourceSeen).
//
//   2. The motion. Where NVIDIA Optical Flow is not there or fails, or "GPU
//      estimate" is picked, the motion is found with the engine's own
//      estimate (lf_estimate_shader.h: coarse to fine on a colour pyramid,
//      both directions checked against each other). Measured on the football
//      truth test that estimate left the ball 2.75 px off and costs about
//      2 ms a frame on an RTX 3060.
//
// DLSS MFG is handed the field dense, as it is found: where the picture did
// not change it is put at rest by lf_fg_still.inl, not here.

// ---- 1. The scene cut --------------------------------------------------------

// How far this capture is from the one before: the mean luma difference, 0..1
// of full scale. Uses the luma the capture just wrote (lfLuma.view, w x h
// bytes). -1 when it cannot be told (no luma, the first frame, a new size).
// Exactly 0 is the picture before over again (SourceSeen).
// g_luma_spread: the share of the picture the box around everything that
// changed takes - a corner, when it was not the game that drew.
static double g_luma_spread = 1.0;
static double LumaChangeNow()
{
    static std::vector<BYTE> last;
    g_luma_spread = 1.0;
    if (!lfLuma.mapped || !lfCap.luma_ok || lfLuma.view == nullptr || lfLuma.w == 0 || lfLuma.h == 0) return -1.0;
    const size_t n = static_cast<size_t>(lfLuma.w) * lfLuma.h;
    if (last.size() != n)
    {
        last.assign(lfLuma.view, lfLuma.view + n);
        return -1.0;           // the first frame is a reset anyway
    }
    uint64_t sum = 0;
    int left = static_cast<int>(lfLuma.w), right = -1, top = static_cast<int>(lfLuma.h), bottom = -1;
    for (UINT y = 0; y < lfLuma.h; ++y)
    {
        const BYTE *now = lfLuma.view + static_cast<size_t>(y) * lfLuma.w, *before = last.data() + static_cast<size_t>(y) * lfLuma.w;
        if (memcmp(now, before, lfLuma.w) == 0) continue;
        if (static_cast<int>(y) < top) top = static_cast<int>(y);
        bottom = static_cast<int>(y);
        for (UINT x = 0; x < lfLuma.w; ++x)
        {
            const int d = int(now[x]) - int(before[x]);
            if (d == 0) continue;
            sum += static_cast<uint64_t>(d < 0 ? -d : d);
            if (static_cast<int>(x) < left) left = static_cast<int>(x);
            if (static_cast<int>(x) > right) right = static_cast<int>(x);
        }
    }
    memcpy(last.data(), lfLuma.view, n);
    g_luma_spread = right < 0 ? 0.0 : double(right - left + 1) * double(bottom - top + 1) / double(n);
    return double(sum) / (255.0 * double(n));
}

// A frame of the desktop in which only a corner changed, while whole frames
// keep coming, is not a frame of the game: a notice of another program
// sliding in over it, a statistics read-out counting, each redrawn on its own
// beat. (Desktop Duplication calls the whole screen changed for these too, so
// its own list does not tell them.) Taken for the game's, each was a "frame"
// between two real ones - a pair of two pictures the same but for the corner,
// the game's rate read as twice what it was. It is passed over like a picture
// handed over twice; the corner is seen with the game's next frame. A page
// that only ever changes in parts has no whole frames to tell its small ones
// from: once none came for an eighth of a second, a part that changed is a
// frame again.
static bool OnlyACorner(double change)
{
    static double last_whole = 0.0;
    if (change <= 0.0) return false;
    const double now = LatencyNow();
    if (g_luma_spread >= 0.08) { last_whole = now; return false; }
    return now - last_whole < 0.125;
}

// Is this capture a different scene from the one before? `turned`: the player's hand is turning the camera
// fast just now (Adaptive Motion Learning listens, sm_learn.inl) and the generator is one that is told where
// the picture went - then a picture that changed this much is that turn, not a cut, up to where hardly anything
// of it is left. (GTA V, the camera swung about for the study: a "cut" every few frames, and each a pair without
// frames in between.)
// Round 40: a cut is only where hardly anything of the picture is left (over 0.55), for every generator and every
// hand. From 0.24 on, a fast turn in a detailed game (GTA V's gardens, a mouse swung fast) was taken for a cut:
// that pair had no frames in between - the picture stood for a moment at every fast turn, with DLSS MFG, XeSS MFG
// and SMFG alike, where Lossless Scaling never stops. Between the two the turn is shown as the generator makes it.
static bool SceneCutNow(double change, bool turned)
{
    if (change <= 0.24) return false;
    turned = true;
    if (turned && change <= 0.55)
    {
        static ULONGLONG said = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - said >= 5000) { said = now; Trace("[history] the picture changed by %.2f of full scale - a fast turn or movement, not a cut: frames in between go on", change); }
        return false;
    }
    Trace("[history] scene cut (%.2f of full scale) - the generator starts fresh", change);
    return true;
}

// The capture handed over the picture before once more (round 31). Desktop
// Duplication reports a new frame of the desktop whenever the compositor draws
// it again - also when what it drew was the engine's own output on top of the
// game, which the capture leaves out: the same picture of the game, with a new
// time stamp. Where the output is put on the screen without the compositor
// that never happens, and it had not shown; where it is not (another overlay
// on top, a window that is not the whole screen) every frame shown came back
// as a "frame of the game": a 72 FPS game read as 165, frames in between made
// of two identical pictures, the generator switched off for "no room", a wave
// in the pace at 20 FPS. Such a frame is nothing new: not shown, not paced by,
// not generated from. (A game standing still hands over the same picture too,
// and is passed over the same way.) Said when repeats come in among new
// frames - a still picture is no news - once, and again after a long rest.
static void SourceSeen(bool repeated)
{
    static double since = -1e9, said = -1e9;
    static unsigned repeats = 0, fresh = 0;
    const double now = LatencyNow();
    if (now - since > 2.0) { since = now; repeats = fresh = 0; }
    if (!repeated) { ++fresh; return; }
    ++repeats;
    FgGateRepeat(now);
    if (repeats >= 16 && fresh >= 16 && now - said > 60.0)
    {
        said = now;
        Trace("[source] the desktop is handed over again each time the engine shows a frame (the compositor draws the "
              "output) - those repeats are passed over; only the game's own frames count");
        // (Until round 42 the game's window was then asked for its own frames instead - Graphics Capture, once and
        // for good. A user's CS2 log: from that switch on 10 to 27 real frames a second where the desktop had given
        // 64, coming and going with what was done in the game, and then none at all. The capture picked in the app
        // stays what it is.)
    }
}

// Repeats cost. To tell one, the frame is copied and its luma made and read:
// a piece of work on the card (lf_gpu_core.inl says what each takes from the
// game), for a picture that is thrown away. Where the compositor draws the
// output - another program's overlay on top of it is enough - there is one
// for every frame the engine shows: with x4 three or more for each frame of
// the game.
//
// So they are expected. Once six desktop frames in a row that came well
// before the game's next frame was due (under half the shortest time between
// two of its frames lately) all proved repeats, such a frame is let go
// untouched - five of six; the sixth is still looked at. One that proves new
// (the game went faster than it was taken for) ends it for two seconds. A
// frame of the game let go by mistake is not lost: the desktop's next frame
// shows it.
static struct RepeatGuess
{
    double paces[16] = {};      // the last times between two new frames
    unsigned known = 0, next = 0;
    double least = 0.0;         // the shortest of them
    double last_new = 0.0;      // the last new frame's time stamp
    int early_repeats = 0;      // early frames in a row that proved repeats
    int unchecked = 0;          // let go untouched since one was looked at
    double held_until = 0.0;    // none is let go untouched before this
} g_repeat_guess;

// A desktop frame, before anything is done with it. True: let it go untouched.
static bool RepeatExpected(double stamp)
{
    RepeatGuess &g = g_repeat_guess;
    // Game FPS given: under half a frame of it after the game's last frame, nothing learnt.
    if (const double given = GivenFrameTime(); given > 0.0)
    {
        const double since = stamp - g.last_new;
        if (g.last_new <= 0.0 || since <= 0.0 || since >= 0.5 * given) return false;
        ++g_steady.repeats;
        ++g_steady.let_go;
        return true;
    }
    if (g.early_repeats < 6 || g.least <= 0.0 || g.last_new <= 0.0) return false;
    const double since = stamp - g.last_new;
    if (since <= 0.0 || since >= 0.5 * g.least) return false;
    if (LatencyNow() < g.held_until) return false;
    if (++g.unchecked >= 6) { g.unchecked = 0; return false; }
    ++g_steady.repeats;
    ++g_steady.let_go;
    return true;
}

// The luma told: the frame with this time stamp was the picture before again, or new.
static void RepeatLearn(double stamp, bool repeated)
{
    RepeatGuess &g = g_repeat_guess;
    const double since = g.last_new > 0.0 ? stamp - g.last_new : 0.0;
    const bool early = g.least > 0.0 && since > 0.0 && since < 0.5 * g.least;
    if (repeated)
    {
        ++g_steady.repeats;
        if (early) g.early_repeats = std::min(g.early_repeats + 1, 1000);
        return;
    }
    if (early) { g.early_repeats = 0; g.held_until = LatencyNow() + 2.0; }
    if (since > 0.0 && since < 0.5)
    {
        g.paces[g.next] = since;
        g.next = (g.next + 1) % 16;
        g.known = std::min(g.known + 1, 16u);
        g.least = g.paces[0];
        for (unsigned i = 1; i < g.known; ++i) g.least = std::min(g.least, g.paces[i]);
    }
    else if (since >= 0.5) { g.known = g.next = 0; g.least = 0.0; g.early_repeats = 0; }
    g.last_new = stamp;
}

// ---- 2. The motion, found on the GPU -------------------------------

static struct NetMotionState
{
    ID3D12DescriptorHeap *heap = nullptr;   // t0 prev, t1 cur, t2/t3 no hint, t4 pyramid; u0 pyramid, u1 fields, u2/u3 result
    ID3D12Resource *prev = nullptr;         // the picture before, NPSR at rest
    ID3D12Resource *pyramid = nullptr, *fields = nullptr;     // UNORDERED_ACCESS at rest
    ID3D12Resource *mv = nullptr, *reverse = nullptr;         // NPSR at rest
    ID3D12Resource *zero = nullptr;         // no motion, for the first frame and a cut
    ID3D12Resource *bound_cur = nullptr;
    UINT pw = 0, ph = 0, mw = 0, mh = 0, ew = 0, eh = 0, levels = 0, ratio = 1;
    bool have_prev = false, failed = false, told = false, flow_valid = false;
    double flow_stamp = -1.0;
    LARGE_INTEGER last = {};
} g_nm;

static ID3D12Resource *NetMotionTexture(UINT w, UINT hgt, DXGI_FORMAT format, bool uav, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES def = {};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = hgt; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = format; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource *r = nullptr;
    if (FAILED(lfCore.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, state, nullptr,
                                                      __uuidof(ID3D12Resource), reinterpret_cast<void **>(&r))))
        return nullptr;
    return r;
}

static void NetMotionClose()
{
    for (ID3D12Resource **r : { &g_nm.prev, &g_nm.pyramid, &g_nm.fields, &g_nm.mv, &g_nm.reverse, &g_nm.zero })
        if (*r) { (*r)->Release(); *r = nullptr; }
    g_nm.bound_cur = nullptr;
    g_nm.pw = g_nm.ph = g_nm.mw = g_nm.mh = 0;
    g_nm.have_prev = false;
    g_nm.flow_valid = false;
}

// Its textures for this picture and motion size; sizes as FLOW MFG picks them
// (level 0 no wider than 800, no taller than 450).
static bool NetMotionReady(UINT pw, UINT ph, UINT mw, UINT mh)
{
    if (g_nm.failed) return false;
    if (g_nm.prev && g_nm.pw == pw && g_nm.ph == ph && g_nm.mw == mw && g_nm.mh == mh) return true;
    if (!EnsureFlowEstimate()) { g_nm.failed = true; Trace("[motion] no GPU estimate here - motion stays still"); return false; }
    NetMotionClose();
    if (g_nm.heap == nullptr)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 9;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(lfCore.device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(&g_nm.heap))))
        { g_nm.failed = true; return false; }
    }
    UINT ratio = 1;
    while ((mw + ratio - 1) / ratio > 800 || (mh + ratio - 1) / ratio > 450) ratio *= 2;
    const UINT ew = (mw + ratio - 1) / ratio, eh = (mh + ratio - 1) / ratio;
    UINT levels = 0;
    auto side = [](UINT n, UINT l) { return (n + (1u << l) - 1u) >> l; };
    while (side(ew, levels + 1) >= 16 && side(eh, levels + 1) >= 16) ++levels;
    const UINT aw = ew + (ew + 1) / 2, band = eh + 16;
    g_nm.prev = NetMotionTexture(pw, ph, DXGI_FORMAT_R8G8B8A8_UNORM, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_nm.pyramid = NetMotionTexture(aw, 2 * band, DXGI_FORMAT_R32_UINT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_nm.fields = NetMotionTexture(aw, 4 * band, DXGI_FORMAT_R32_UINT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_nm.mv = NetMotionTexture(mw, mh, DXGI_FORMAT_R16G16_FLOAT, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_nm.reverse = NetMotionTexture(mw, mh, DXGI_FORMAT_R16G16_FLOAT, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_nm.zero = NetMotionTexture(mw, mh, DXGI_FORMAT_R16G16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (!g_nm.prev || !g_nm.pyramid || !g_nm.fields || !g_nm.mv || !g_nm.reverse || !g_nm.zero)
    {
        Trace("[motion] the GPU estimate's textures failed at %ux%u", mw, mh);
        NetMotionClose();
        g_nm.failed = true;
        return false;
    }
    g_nm.pw = pw; g_nm.ph = ph; g_nm.mw = mw; g_nm.mh = mh;
    g_nm.ew = ew; g_nm.eh = eh; g_nm.levels = levels; g_nm.ratio = ratio;
    return true;
}

static void NetMotionBind(ID3D12Resource *cur)
{
    if (g_nm.bound_cur == cur) return;
    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_nm.heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    lfCore.device->CreateShaderResourceView(g_nm.prev, &sd, cpu); cpu.ptr += stride;
    lfCore.device->CreateShaderResourceView(cur, &sd, cpu); cpu.ptr += stride;
    sd.Format = DXGI_FORMAT_R16G16_FLOAT;            // no outside estimate (gFlags 0): null views
    lfCore.device->CreateShaderResourceView(nullptr, &sd, cpu); cpu.ptr += stride;
    lfCore.device->CreateShaderResourceView(nullptr, &sd, cpu); cpu.ptr += stride;
    sd.Format = DXGI_FORMAT_R32_UINT;
    lfCore.device->CreateShaderResourceView(g_nm.pyramid, &sd, cpu); cpu.ptr += stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    ud.Format = DXGI_FORMAT_R32_UINT;
    lfCore.device->CreateUnorderedAccessView(g_nm.pyramid, nullptr, &ud, cpu); cpu.ptr += stride;
    lfCore.device->CreateUnorderedAccessView(g_nm.fields, nullptr, &ud, cpu); cpu.ptr += stride;
    ud.Format = DXGI_FORMAT_R16G16_FLOAT;
    lfCore.device->CreateUnorderedAccessView(g_nm.mv, nullptr, &ud, cpu); cpu.ptr += stride;
    lfCore.device->CreateUnorderedAccessView(g_nm.reverse, nullptr, &ud, cpu);
    g_nm.bound_cur = cur;
}

// The estimate, recorded into the open list: prev and cur are shader
// resources; afterwards g_nm.mv is (current -> previous, motion texels).
static void NetMotionRecord(ID3D12GraphicsCommandList *list, double interval)
{
    ID3D12DescriptorHeap *heaps[] = { g_nm.heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_flow.est_rs);
    list->SetComputeRootDescriptorTable(1, g_nm.heap->GetGPUDescriptorHandleForHeapStart());
    const UINT ew = g_nm.ew, eh = g_nm.eh, levels = g_nm.levels;
    UINT32 k[14] = { g_nm.pw, g_nm.ph, ew, eh, 0, levels, 0u, EstimateRadius(interval),
                     0, 0, 0, g_nm.mw, g_nm.mh, g_nm.ratio };
    auto uav = [](ID3D12Resource *r) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = r;
        return b;
    };
    auto side = [](UINT n, UINT l) { return (n + (1u << l) - 1u) >> l; };
    auto pass = [&](int pso, UINT level, UINT from, UINT to, UINT step, UINT x, UINT y, ID3D12Resource *written) {
        k[4] = level; k[8] = step; k[9] = from; k[10] = to;
        list->SetComputeRoot32BitConstants(0, 14, k, 0);
        list->SetPipelineState(g_flow.est_pso[pso]);
        list->Dispatch((x + 7) / 8, (y + 7) / 8, 2);
        D3D12_RESOURCE_BARRIER b = uav(written);
        list->ResourceBarrier(1, &b);
    };
    D3D12_RESOURCE_BARRIER open[] = {
        Barrier(g_nm.mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(g_nm.reverse, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    list->ResourceBarrier(_countof(open), open);
    pass(0, 0, 0, 0, 0, ew, eh, g_nm.pyramid);
    for (UINT l = 1; l <= levels; ++l) pass(1, l, 0, 0, 0, side(ew, l), side(eh, l), g_nm.pyramid);
    D3D12_RESOURCE_BARRIER to_read = Barrier(g_nm.pyramid, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &to_read);
    UINT set = 0;
    auto spreads = [&](UINT l) {
        for (UINT step : { 4u, 2u, 1u })
        {
            if (l == 0 && step != 1) continue;
            pass(4, l, set, 1 - set, step, side(ew, l), side(eh, l), g_nm.fields);
            set = 1 - set;
        }
    };
    pass(2, levels, 0, 0, 0, side(ew, levels), side(eh, levels), g_nm.fields);
    spreads(levels);
    for (int l = int(levels) - 1; l >= 0; --l)
    {
        pass(3, UINT(l), set, 1 - set, 0, side(ew, UINT(l)), side(eh, UINT(l)), g_nm.fields);
        set = 1 - set;
        spreads(UINT(l));
    }
    pass(5, 0, set, 1 - set, 0, ew, eh, g_nm.fields);
    set = 1 - set;
    pass(6, 0, set, set, 0, ew, eh, g_nm.mv);
    D3D12_RESOURCE_BARRIER close[] = {
        Barrier(g_nm.pyramid, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(g_nm.mv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(g_nm.reverse, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    list->ResourceBarrier(_countof(close), close);
}

// The network's motion for this frame, in place of the engine's field:
// estimated between the picture before and this one, or none on the first
// frame and across a cut. Same contract as FeedMotion (sent_mark: submit
// and hand back the fence instead of waiting).
static bool NetMotionRun(Pipeline &pipe, bool reset, UINT64 *sent_mark)
{
    g_nm.flow_valid = false;
    g_nm.flow_stamp = lfCap.stamp;
    if (sent_mark) *sent_mark = 0;
    const bool flow_only = false;
    ID3D12Resource *cur = pipe.picture.gpu_tex;
    const D3D12_RESOURCE_DESC pd = cur->GetDesc();
    const UINT pw = static_cast<UINT>(pd.Width), ph = pd.Height;
    const bool ready = NetMotionReady(pw, ph, pipe.work_w, pipe.work_h);
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const double interval = g_nm.last.QuadPart ? double(now.QuadPart - g_nm.last.QuadPart) / double(freq.QuadPart) : 1.0 / 60.0;
    g_nm.last = now;
    if (!CmdOpen()) return false;
    GpuStageOpen(GS_MOVE);
    const bool estimate = ready && g_nm.have_prev && !reset && !flow_only;
    if (estimate)
    {
        NetMotionBind(cur);
        NetMotionRecord(lfCore.cmd, interval);
    }
    ID3D12Resource *from = estimate ? g_nm.mv : (ready ? g_nm.zero : nullptr);
    if (from != nullptr)
    {
        const D3D12_RESOURCE_STATES from_rest = estimate ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                                         : D3D12_RESOURCE_STATE_COPY_SOURCE;
        // The motion texture rests as a shader resource once it was fed;
        // before that it is still in its creation state, COPY_DEST.
        const bool from_moves = from_rest != D3D12_RESOURCE_STATE_COPY_SOURCE;
        D3D12_RESOURCE_BARRIER in[2];
        UINT count = 0;
        if (pipe.fed)
            in[count++] = Barrier(pipe.motion.gpu_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_COPY_DEST);
        if (from_moves) in[count++] = Barrier(from, from_rest, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (count) lfCore.cmd->ResourceBarrier(count, in);
        lfCore.cmd->CopyResource(pipe.motion.gpu_tex, from);
        D3D12_RESOURCE_BARRIER out[] = {
            Barrier(pipe.motion.gpu_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Barrier(from, D3D12_RESOURCE_STATE_COPY_SOURCE, from_rest),
        };
        lfCore.cmd->ResourceBarrier(from_moves ? 2 : 1, out);
        // NR does not need colour history while FLOW owns the estimate.
        if (!flow_only) {
        // This picture is the next frame's "before".
        D3D12_RESOURCE_BARRIER keep_in[] = {
            Barrier(cur, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Barrier(g_nm.prev, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        lfCore.cmd->ResourceBarrier(_countof(keep_in), keep_in);
        lfCore.cmd->CopyResource(g_nm.prev, cur);
        D3D12_RESOURCE_BARRIER keep_out[] = {
            Barrier(cur, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Barrier(g_nm.prev, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        lfCore.cmd->ResourceBarrier(_countof(keep_out), keep_out);
        }
        g_nm.have_prev = !flow_only;
    }
    GpuStageClose(GS_MOVE);
    const UINT64 fence = CmdSubmit();
    if (fence == 0) return false;
    if (from != nullptr) pipe.fed = true;
    if (!g_nm.told && ready)
    {
        g_nm.told = true;
        Trace("[motion] the engine finds the motion itself: GPU estimate %ux%u (level 0 %ux%u, %u levels)",
            g_nm.mw, g_nm.mh, g_nm.ew, g_nm.eh, g_nm.levels);
    }
    if (sent_mark) { g_nm.flow_valid = estimate; *sent_mark = fence; return true; }
    const bool complete = GpuStageWait(GS_MOVE, fence, 30000);
    g_nm.flow_valid = complete && estimate;
    return complete;
}

